// SPDX-License-Identifier: GPL-2.0-or-later

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/bitops.h>
#include <linux/dmi.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kstrtox.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/sysfs.h>
#include <linux/version.h>
#include <linux/workqueue.h>

// Clevo DCHU DSM UUID: "93f224e4-fbdc-4bbf-add6-db71bdc0afad"
static const guid_t dchu_dsm_guid =
	GUID_INIT(0x93f224e4, 0xfbdc, 0x4bbf,
		  0xad, 0xd6, 0xdb, 0x71, 0xbd, 0xc0, 0xaf, 0xad);

static const enum led_brightness kb_led_levels[] = {
	48,
	72,
	96,
	144,
	192,
	255,
};

static const u32 kb_led_colors[] = {
	0xFFFFFF, // WHITE
	0x0000FF, // BLUE
	0xFF0000, // RED
	0xFF00FF, // MAGENTA
	0x00FF00, // GREEN
	0x00FFFF, // CYAN
	0xFFFF00, // YELLOW
};

enum clevo_kbd_zone {
	CLEVO_ZONE_LEFT,
	CLEVO_ZONE_CENTER,
	CLEVO_ZONE_RIGHT,
	CLEVO_ZONE_NUMPAD,
	CLEVO_ZONE_LIGHTBAR,
	CLEVO_ZONE_COUNT,
};

static const u8 clevo_kbd_zone_ids[CLEVO_ZONE_COUNT] = {
	[CLEVO_ZONE_LEFT]     = 0x03,
	[CLEVO_ZONE_CENTER]   = 0x04,
	[CLEVO_ZONE_RIGHT]    = 0x05,
	[CLEVO_ZONE_NUMPAD]   = 0x0B,
	[CLEVO_ZONE_LIGHTBAR] = 0x07,
};

// Declared here (ahead of struct clevo_data below, which needs
// CLEVO_FAN_COUNT) even though the rest of the fan control feature lives
// further down in its own section, alongside the performance-mode block.
enum clevo_fan_index {
	CLEVO_FAN0,
	CLEVO_FAN1,
	CLEVO_FAN2,
	CLEVO_FAN_COUNT,
};

struct clevo_data {
	struct platform_device *pdev;
	struct input_dev *input;
	struct led_classdev kb_led;
	u8 kb_brightness;
	u8 kb_toggle_brightness;
	u32 kb_color_index;
	u32 kb_zone_color[CLEVO_ZONE_COUNT];
	u8 kbd_type;
	u8 perf_mode;

	// Continuous fan duty control -- see the "fan control" section below
	// for the DCHU commands and the watchdog dead-man's-switch design.
	// fan_lock is scoped to this subsystem only, not a general driver
	// lock (kb_zone_color/perf_mode above remain unlocked, as before).
	struct mutex fan_lock;
	u8 fan_duty_raw[CLEVO_FAN_COUNT];
	u8 fan_present;
	bool fan_manual_active;
	unsigned long fan_watchdog_timeout_jiffies;
	struct delayed_work fan_watchdog_work;
};

static const struct key_entry clevo_keymap[] = {
	// White-only KBD
	// Brightness keys reported as KE_IGNORE, not KE_KEY: the EC command
	// already runs in-kernel (kbled_hotkey_*), so nothing needs to react
	// to the corresponding evdev key. On GNOME (gsd-power/upower), letting
	// KEY_KBDILLUM* reach userspace causes it to independently recompute
	// and rewrite brightness a few ms later based on its own (buggy)
	// logic, clobbering the correct in-kernel change back to 0.
	{ KE_IGNORE, 0x20 },			// Brightness down
	{ KE_IGNORE, 0x21 },			// Brightness up
	{ KE_IGNORE, 0x3f },			// Brightness toggle

	// RGB KBD
	{ KE_IGNORE, 0x81 },			// Brightness down
	{ KE_IGNORE, 0x82 },			// Brightness up
	{ KE_IGNORE, 0x83 },			// Color cycle
	{ KE_IGNORE, 0x9f },			// Brightness toggle

	{ KE_KEY, 0x5d, { KEY_F21 } },		// Touchpad disable
	{ KE_IGNORE, 0x70 },			// Fan max off (Fn+1)
	{ KE_IGNORE, 0x7b },			// Fn+Backspace
	{ KE_IGNORE, 0x8f },			// Fan max on (Fn+1)
	{ KE_IGNORE, 0x95 },			// Fn+Esc
	{ KE_IGNORE, 0xf6 },			// Camera disable
	{ KE_IGNORE, 0xf7 },			// Camera enable
	{ KE_IGNORE, 0xfa },			// Speaker volume change
	{ KE_IGNORE, 0xfb },			// Speaker mute toggle
	{ KE_IGNORE, 0xfc, { KEY_F21 } },	// Touchpad disable
	{ KE_IGNORE, 0xfd, { KEY_F21 } },	// Touchpad enable
	{ KE_END }
};

static acpi_status clevo_ec_locate(acpi_handle handle, u32 level,
				   void *context, void **retval)
{
	*(acpi_handle *)retval = handle;
	return AE_CTRL_TERMINATE;
}

static acpi_status clevo_ec_cmd(u8 *input, size_t input_length,
				u8 *output, size_t output_length)
{
	struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
	struct acpi_object_list in;
	union acpi_object obj;
	acpi_handle handle;
	acpi_status status;

	if (!input || input_length != 8)
		return AE_BAD_PARAMETER;
	if (output && output_length != 6)
		return AE_BAD_PARAMETER;

	obj.type = ACPI_TYPE_BUFFER;
	obj.buffer.length = input_length;
	obj.buffer.pointer = input;

	in.count = 1;
	in.pointer = &obj;

	// TODO: Get the handle once and save
	status = acpi_get_devices("PNP0C09", clevo_ec_locate, NULL, &handle);
	if (ACPI_FAILURE(status) || !handle) {
		pr_err("failed to get EC handle: %s\n", acpi_format_exception(status));
		return status;
	}

	status = acpi_evaluate_object(handle, "ECMD", &in, &out);
	if (ACPI_FAILURE(status)) {
		pr_err("failed to call ECMD: %s\n", acpi_format_exception(status));
		return status;
	}

	// ECMD always returns a 6-byte buffer.
	union acpi_object *ret_obj __free(kfree) = out.pointer;

	if (output) {
		if (!ret_obj)
			return AE_ERROR;

		if (ret_obj->type != ACPI_TYPE_BUFFER)
			return AE_ERROR;

		memcpy(output, ret_obj->buffer.pointer, output_length);
	}

	return AE_OK;
}

static void clevo_ec_kbd_zone_color_set(enum clevo_kbd_zone zone, u32 color)
{
	u8 buf[8] = {};

	buf[0] = 5; // Payload size
	buf[2] = 0xCA; // Command
	buf[3] = clevo_kbd_zone_ids[zone];
	buf[4] = color & 0xFF; // Blue
	buf[5] = (color >> 16) & 0xFF; // Red
	buf[6] = (color >> 8) & 0xFF; // Green

	(void)clevo_ec_cmd(buf, ARRAY_SIZE(buf), NULL, 0);
}

static void clevo_kbd_zones_set(struct clevo_data *priv, u32 color)
{
	for (int i = 0; i < CLEVO_ZONE_COUNT; i++) {
		priv->kb_zone_color[i] = color;
		clevo_ec_kbd_zone_color_set(i, color);
	}
}

static void clevo_ec_kbd_brightness_set(enum led_brightness value)
{
	u8 buf[8] = {};

	buf[0] = 5; // Payload size
	buf[2] = 0xCA; // Command
	buf[3] = 0x06; // Brightness
	buf[4] = value; // KBD
	buf[6] = value; // Lightbar

	(void)clevo_ec_cmd(buf, ARRAY_SIZE(buf), NULL, 0);
}

// TODO: Return an enum.
// Clevo only reported 2 possible values (1, 6), and serw14 returns 0x17.
static u8 clevo_dchu_kbd_type(acpi_handle handle)
{
	union acpi_object arg4;
	union acpi_object *obj;
	u8 kbd_type = 0;
	u8 buf[256] = {};

	arg4.type = ACPI_TYPE_BUFFER;
	arg4.buffer.length = sizeof(buf);
	arg4.buffer.pointer = buf;

	obj = acpi_evaluate_dsm_typed(handle, &dchu_dsm_guid, 0, 0x0d, &arg4,
				      ACPI_TYPE_BUFFER);
	if (obj) {
		kbd_type = obj->buffer.pointer[0x0f];
		ACPI_FREE(obj);
	}

	return kbd_type;
}

// Clevo DCHU SCMD expects a package with one integer element.
static bool clevo_dchu_cmd(acpi_handle handle, u8 method_id, u32 data)
{
	union acpi_object arg4;
	union acpi_object req;
	union acpi_object *obj;

	req.type = ACPI_TYPE_INTEGER;
	req.integer.value = data;

	arg4.type = ACPI_TYPE_PACKAGE;
	arg4.package.count = 1;
	arg4.package.elements = &req;

	obj = acpi_evaluate_dsm_typed(handle, &dchu_dsm_guid, 0, method_id, &arg4,
				      ACPI_TYPE_INTEGER);

	if (obj) {
		// SCMD returns the method ID on success
		bool ret = obj->integer.value == method_id;

		ACPI_FREE(obj);
		return ret;
	}

	return false;
}

// Same DSM call as clevo_dchu_cmd(), but for "get"-style method IDs that
// return real data rather than echoing the method ID back on success.
static bool clevo_dchu_cmd_get(acpi_handle handle, u8 method_id, u32 arg,
			       u32 *result)
{
	union acpi_object arg4;
	union acpi_object req;
	union acpi_object *obj;

	req.type = ACPI_TYPE_INTEGER;
	req.integer.value = arg;

	arg4.type = ACPI_TYPE_PACKAGE;
	arg4.package.count = 1;
	arg4.package.elements = &req;

	obj = acpi_evaluate_dsm_typed(handle, &dchu_dsm_guid, 0, method_id, &arg4,
				      ACPI_TYPE_INTEGER);
	if (!obj)
		return false;

	*result = (u32)obj->integer.value;
	ACPI_FREE(obj);

	return true;
}

// Sets HKDR=1 so NEVT will Notify device
static bool clevo_enable_notify_events(acpi_handle handle)
{
	return clevo_dchu_cmd(handle, 0x46, 0);
}

// "Flexicharger" battery charge threshold feature (BIOS setting + vendor
// Windows Control Center software: stop charging at a configurable upper
// threshold, resume at a configurable lower one, to reduce Li-ion battery
// wear from staying near 100% or fully depleted for extended periods).
//
// Same DCHU DSM interface as the keyboard, different method IDs (0x77 to
// read, 0x76 to write) -- confirmed against this board's own DSDT
// (GCMD/SCMD dispatch) to match the "legacy flexicharger" bit layout
// already reverse-engineered and shipped by TUXEDO Computers' tuxedo-drivers
// for other Clevo/Tongfang boards: a single packed 32-bit value,
// end<<16 | start<<8 | status. See ~/laptopissues/battery-threshold/NOTES.md
// for the full writeup this is based on.
#define CLEVO_FLEXICHARGER_GET 0x77
#define CLEVO_FLEXICHARGER_SET 0x76

static bool clevo_flexicharger_read(acpi_handle handle, u8 *start, u8 *end,
				    u8 *status)
{
	u32 data;

	if (!clevo_dchu_cmd_get(handle, CLEVO_FLEXICHARGER_GET, 0, &data))
		return false;

	if (status)
		*status = data & 0x01;
	if (start)
		*start = (data >> 0x08) & 0xFF;
	if (end)
		*end = (data >> 0x10) & 0xFF;

	return true;
}

// Only one of start/end/status needs to be non-NULL; the others are left
// at their current value. Mirrors tuxedo-drivers' clevo_legacy_flexicharger_write():
// thresholds and status are two separate SCMD calls, thresholds first.
static bool clevo_flexicharger_write(acpi_handle handle, const u8 *start,
				     const u8 *end, const u8 *status)
{
	u8 prev_start, prev_end, prev_status;
	u8 set_start, set_end, set_status;
	u32 write_thresholds, write_status;

	if (!clevo_flexicharger_read(handle, &prev_start, &prev_end, &prev_status))
		return false;

	set_start = start ? *start : prev_start;
	set_end = end ? *end : prev_end;
	set_status = status ? *status : prev_status;

	write_thresholds = (0x06 << 0x18) | set_start | (set_end << 0x08);
	write_status = (0x05 << 0x18) | (set_status & 0x01);

	if (!clevo_dchu_cmd(handle, CLEVO_FLEXICHARGER_SET, write_thresholds))
		return false;

	return clevo_dchu_cmd(handle, CLEVO_FLEXICHARGER_SET, write_status);
}

static enum led_brightness clevo_kbled_get(struct led_classdev *led_cdev)
{
	struct clevo_data *priv;

	priv = container_of(led_cdev, struct clevo_data, kb_led);

	return priv->kb_brightness;
}

static int clevo_kbled_set(struct led_classdev *led_cdev,
			   enum led_brightness brightness)
{
	struct clevo_data *priv;
	struct acpi_device *adev;

	pr_debug("%s %d\n", __func__, (int)brightness);

	priv = container_of(led_cdev, struct clevo_data, kb_led);
	adev = ACPI_COMPANION(&priv->pdev->dev);

	priv->kb_brightness = brightness;
	if (brightness > 0)
		priv->kb_toggle_brightness = brightness;

	if (priv->kbd_type == 1)
		clevo_dchu_cmd(adev->handle, 0x27, priv->kb_brightness);
	else
		clevo_ec_kbd_brightness_set(priv->kb_brightness);

	return 0;
}

static void kbled_hotkey_toggle(struct clevo_data *priv)
{
	if (priv->kb_brightness > 0) {
		priv->kb_toggle_brightness = priv->kb_brightness;
		priv->kb_brightness = 0;
	} else {
		priv->kb_brightness = priv->kb_toggle_brightness;
	}

	clevo_kbled_set(&priv->kb_led, priv->kb_brightness);
	led_classdev_notify_brightness_hw_changed(&priv->kb_led, priv->kb_brightness);
}

static void kbled_hotkey_white_dec(struct clevo_data *priv)
{
	if (priv->kb_brightness > 0)
		priv->kb_brightness--;

	clevo_kbled_set(&priv->kb_led, priv->kb_brightness);
	led_classdev_notify_brightness_hw_changed(&priv->kb_led, priv->kb_brightness);
}

static void kbled_hotkey_rgb_dec(struct clevo_data *priv)
{
	if (priv->kb_brightness > 0) {
		for (int i = ARRAY_SIZE(kb_led_levels); i > 0; i--) {
			if (kb_led_levels[i - 1] < priv->kb_brightness) {
				priv->kb_brightness = kb_led_levels[i - 1];
				clevo_kbled_set(&priv->kb_led, priv->kb_brightness);
				break;
			}
		}
	} else {
		clevo_kbled_set(&priv->kb_led, priv->kb_toggle_brightness);
	}

	led_classdev_notify_brightness_hw_changed(&priv->kb_led, priv->kb_brightness);
}

static void kbled_hotkey_white_inc(struct clevo_data *priv)
{
	if (priv->kb_brightness < 5)
		priv->kb_brightness++;

	clevo_kbled_set(&priv->kb_led, priv->kb_brightness);
	led_classdev_notify_brightness_hw_changed(&priv->kb_led, priv->kb_brightness);
}

static void kbled_hotkey_rgb_inc(struct clevo_data *priv)
{
	if (priv->kb_brightness > 0) {
		for (int i = 0; i < ARRAY_SIZE(kb_led_levels); i++) {
			if (kb_led_levels[i] > priv->kb_brightness) {
				priv->kb_brightness = kb_led_levels[i];
				clevo_kbled_set(&priv->kb_led, priv->kb_brightness);
				break;
			}
		}
	} else {
		clevo_kbled_set(&priv->kb_led, priv->kb_toggle_brightness);
	}

	led_classdev_notify_brightness_hw_changed(&priv->kb_led, priv->kb_brightness);
}

static void kbled_hotkey_rgb_color(struct clevo_data *priv)
{
	priv->kb_color_index += 1;
	if (priv->kb_color_index >= ARRAY_SIZE(kb_led_colors))
		priv->kb_color_index = 0;

	clevo_kbd_zones_set(priv, kb_led_colors[priv->kb_color_index]);

	led_classdev_notify_brightness_hw_changed(&priv->kb_led, priv->kb_brightness);
}

static int clevo_kbled_init(struct device *dev)
{
	struct clevo_data *priv = dev_get_drvdata(dev);
	struct acpi_device *adev = ACPI_COMPANION(dev);
	struct led_init_data init_data = {
		.devicename = "clevo-acpi",
		.default_label = ":" LED_FUNCTION_KBD_BACKLIGHT,
		.devname_mandatory = true,
	};
	int err;

	priv->kbd_type = clevo_dchu_kbd_type(adev->handle);

	if (priv->kbd_type == 1) {
		pr_debug("white-only KBLED\n");
		priv->kb_toggle_brightness = 2;
		priv->kb_led.max_brightness = 5;
	} else {
		pr_debug("RGB KBLED\n");
		priv->kb_toggle_brightness = 72;
		priv->kb_led.max_brightness = 255;
	}

	priv->kb_color_index = 0;
	priv->kb_brightness = priv->kb_toggle_brightness;

	priv->kb_led.brightness = priv->kb_brightness;
	priv->kb_led.brightness_set_blocking = clevo_kbled_set;
	priv->kb_led.brightness_get = clevo_kbled_get;
	// XXX: Other flags?
	priv->kb_led.flags = LED_BRIGHT_HW_CHANGED |
			     LED_REJECT_NAME_CONFLICT;

	err = devm_led_classdev_register_ext(dev, &priv->kb_led, &init_data);
	if (err)
		return err;

	clevo_dchu_cmd(adev->handle, 0x67, 0xE007F001);
	clevo_kbled_set(&priv->kb_led, priv->kb_brightness);
	if (priv->kbd_type != 1)
		clevo_kbd_zones_set(priv, kb_led_colors[priv->kb_color_index]);

	return 0;
}

static int clevo_input_init(struct device *dev)
{
	struct clevo_data *priv = dev_get_drvdata(dev);
	struct input_dev *input;
	int err;

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;

	input->name = "Clevo ACPI hotkeys";
	input->phys = "clevo-acpi/input0";
	input->id.bustype = BUS_HOST;

	err = sparse_keymap_setup(input, clevo_keymap, NULL);
	if (err) {
		pr_err("failed to set up input device keymap\n");
		return err;
	}

	err = input_register_device(input); // devres managed
	if (err) {
		pr_err("failed to register input device\n");
		return err;
	}

	priv->input = input;

	return 0;
}

static void clevo_acpi_notify(acpi_handle handle, u32 event, void *context)
{
	struct clevo_data *priv = dev_get_drvdata(context);

	pr_debug("event: %#x\n", event);

	switch (event) {
	case 0x20:
		kbled_hotkey_white_dec(priv);
		break;
	case 0x21:
		kbled_hotkey_white_inc(priv);
		break;

	case 0x81:
		kbled_hotkey_rgb_dec(priv);
		break;
	case 0x82:
		kbled_hotkey_rgb_inc(priv);
		break;
	case 0x83:
		if (priv->kbd_type != 1)
			kbled_hotkey_rgb_color(priv);
		break;

	case 0x3f:
	case 0x9f:
		kbled_hotkey_toggle(priv);
		break;
	}

	if (!sparse_keymap_report_event(priv->input, event, 1, true))
		pr_warn("unknown key event: %#x\n", event);
}

static int clevo_acpi_suspend(struct device *dev)
{
	struct clevo_data *priv = dev_get_drvdata(dev);

	dev_dbg(dev, "suspend\n");

	// Don't let the fan watchdog timer (based on pre-suspend jiffies)
	// fire mid-suspend or right at resume -- clevo_acpi_resume() below
	// re-arms it with a fresh timeout if a manual fan override was still
	// active. This deliberately does NOT release fan control to auto: a
	// custom fan curve should survive a lid close, not be silently
	// canceled by it.
	cancel_delayed_work_sync(&priv->fan_watchdog_work);

	// Explicitly turn the backlight off via the EC rather than relying on
	// a sysfs brightness write reaching us beforehand (e.g. from a desktop
	// environment) -- same reasoning as the FIXME in clevo_acpi_resume().
	for (int i = 0; i < CLEVO_ZONE_COUNT; i++)
		clevo_ec_kbd_zone_color_set(i, 0);

	return 0;
}

static int clevo_acpi_resume(struct device *dev)
{
	struct clevo_data *priv = dev_get_drvdata(dev);
	struct acpi_device *adev = ACPI_COMPANION(dev);

	dev_dbg(dev, "resume\n");

	clevo_enable_notify_events(adev->handle);

	// FIXME: This fixes turning KBLED back on for some reason.
	// Even on White-only KBLED.
	for (int i = 0; i < CLEVO_ZONE_COUNT; i++)
		clevo_ec_kbd_zone_color_set(i, priv->kb_zone_color[i]);

	// Some desktop environments (confirmed: GNOME's gsd-power) explicitly
	// zero the brightness before suspend as a power-saving measure, but
	// never restore it on resume. Restore it ourselves from the last known
	// nonzero brightness rather than trusting whatever priv->kb_brightness
	// currently holds, since that's exactly the value such a write clobbers.
	clevo_kbled_set(&priv->kb_led, priv->kb_toggle_brightness);

	// If a manual fan override was in effect before suspend, give the
	// userspace daemon (resumed asynchronously by systemd, possibly with
	// some lag) a full fresh window to notice it's back and resume
	// petting, rather than trusting stale pre-suspend timing.
	mutex_lock(&priv->fan_lock);
	if (priv->fan_manual_active)
		mod_delayed_work(system_wq, &priv->fan_watchdog_work,
				 priv->fan_watchdog_timeout_jiffies);
	mutex_unlock(&priv->fan_lock);

	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 17, 0)
static DEFINE_SIMPLE_DEV_PM_OPS(clevo_acpi_pm, clevo_acpi_suspend, clevo_acpi_resume);
#else
static SIMPLE_DEV_PM_OPS(clevo_acpi_pm, clevo_acpi_suspend, clevo_acpi_resume);
#endif

static ssize_t clevo_zone_color_show(struct clevo_data *priv,
				     enum clevo_kbd_zone zone, char *buf)
{
	return sysfs_emit(buf, "%06x\n", priv->kb_zone_color[zone]);
}

static ssize_t clevo_zone_color_store(struct clevo_data *priv,
				      enum clevo_kbd_zone zone,
				      const char *buf, size_t count)
{
	u32 color;
	int err;

	err = kstrtou32(buf, 16, &color);
	if (err)
		return err;
	if (color > 0xFFFFFF)
		return -EINVAL;

	priv->kb_zone_color[zone] = color;
	clevo_ec_kbd_zone_color_set(zone, color);

	return count;
}

#define CLEVO_ZONE_ATTR(_name, _zone)					\
static ssize_t _name##_show(struct device *dev,			\
			     struct device_attribute *attr, char *buf)	\
{									\
	return clevo_zone_color_show(dev_get_drvdata(dev), _zone, buf); \
}									\
static ssize_t _name##_store(struct device *dev,			\
			      struct device_attribute *attr,		\
			      const char *buf, size_t count)		\
{									\
	return clevo_zone_color_store(dev_get_drvdata(dev), _zone,	\
				      buf, count);			\
}									\
static DEVICE_ATTR_RW(_name)

CLEVO_ZONE_ATTR(color_left, CLEVO_ZONE_LEFT);
CLEVO_ZONE_ATTR(color_center, CLEVO_ZONE_CENTER);
CLEVO_ZONE_ATTR(color_right, CLEVO_ZONE_RIGHT);
CLEVO_ZONE_ATTR(color_numpad, CLEVO_ZONE_NUMPAD);
CLEVO_ZONE_ATTR(color_lightbar, CLEVO_ZONE_LIGHTBAR);

static struct attribute *clevo_kbd_zone_attrs[] = {
	&dev_attr_color_left.attr,
	&dev_attr_color_center.attr,
	&dev_attr_color_right.attr,
	&dev_attr_color_numpad.attr,
	&dev_attr_color_lightbar.attr,
	NULL,
};
static const struct attribute_group clevo_kbd_zone_group = {
	.attrs = clevo_kbd_zone_attrs,
};

// Attribute names match tuxedo-drivers' convention (which in turn matches
// the standard Linux power_supply charge_control_*_threshold interface),
// so tools that already know that interface (e.g. TLP) work unmodified.
static ssize_t charge_control_start_threshold_show(struct device *dev,
						    struct device_attribute *attr,
						    char *buf)
{
	struct acpi_device *adev = ACPI_COMPANION(dev);
	u8 start;

	if (!clevo_flexicharger_read(adev->handle, &start, NULL, NULL))
		return -EIO;

	return sysfs_emit(buf, "%u\n", start);
}

static ssize_t charge_control_start_threshold_store(struct device *dev,
						     struct device_attribute *attr,
						     const char *buf, size_t count)
{
	struct acpi_device *adev = ACPI_COMPANION(dev);
	u8 value;
	int err;

	err = kstrtou8(buf, 10, &value);
	if (err)
		return err;
	if (value < 1 || value > 100)
		return -EINVAL;

	if (!clevo_flexicharger_write(adev->handle, &value, NULL, NULL))
		return -EIO;

	return count;
}
static DEVICE_ATTR_RW(charge_control_start_threshold);

static ssize_t charge_control_end_threshold_show(struct device *dev,
						  struct device_attribute *attr,
						  char *buf)
{
	struct acpi_device *adev = ACPI_COMPANION(dev);
	u8 end;

	if (!clevo_flexicharger_read(adev->handle, NULL, &end, NULL))
		return -EIO;

	return sysfs_emit(buf, "%u\n", end);
}

static ssize_t charge_control_end_threshold_store(struct device *dev,
						   struct device_attribute *attr,
						   const char *buf, size_t count)
{
	struct acpi_device *adev = ACPI_COMPANION(dev);
	u8 value;
	int err;

	err = kstrtou8(buf, 10, &value);
	if (err)
		return err;
	if (value < 1 || value > 100)
		return -EINVAL;

	if (!clevo_flexicharger_write(adev->handle, NULL, &value, NULL))
		return -EIO;

	return count;
}
static DEVICE_ATTR_RW(charge_control_end_threshold);

static struct attribute *clevo_battery_attrs[] = {
	&dev_attr_charge_control_start_threshold.attr,
	&dev_attr_charge_control_end_threshold.attr,
	NULL,
};
static const struct attribute_group clevo_battery_group = {
	.attrs = clevo_battery_attrs,
};

// "Performance mode": DCHU function 0x79, sub-command 1 (top byte of the
// packed ARGS value), takes a 0-8 enum (bottom bytes) that sends a distinct
// single-bit-flag EC command (0xD7/0x01) and then notifies Intel's IETM/
// TCPU thermal ACPI objects -- this is what Windows' Control Center calls
// for its Performance/Balanced/Quiet mode switch (and, for value 1, a
// fan-boost override). Not vendor-documented anywhere we could find; the
// mapping below was determined empirically on 2026-08-08 by observing real
// fan RPM response to sustained CPU load for each value, with the laptop
// fully cooled down between each isolated test -- see
// ~/laptopissues/performance-mode/NOTES.md for the raw data. Values 2, 6,
// and 8 showed some distinct effect during testing but weren't cleanly
// classified against a real profile name, so they're intentionally not
// exposed here; add them later if their behavior gets pinned down.
//
// No read-back exists for this (no corresponding GCMD case was found), so
// unlike the flexicharger attributes above, the "current mode" shown here
// is only what this driver last set, cached in priv->perf_mode -- not a
// live hardware read. It defaults to "balanced" on module load, matching
// the EC's own apparent power-on default observed during testing.
#define CLEVO_PERF_MODE_SET 0x79

enum clevo_perf_mode {
	CLEVO_PERF_BALANCED	= 0,
	CLEVO_PERF_MAX_FAN	= 1,
	CLEVO_PERF_QUIET	= 5,
	CLEVO_PERF_PERFORMANCE	= 7,
};

static const char *clevo_perf_mode_to_name(u8 mode)
{
	switch (mode) {
	case CLEVO_PERF_BALANCED:
		return "balanced";
	case CLEVO_PERF_PERFORMANCE:
		return "performance";
	case CLEVO_PERF_QUIET:
		return "quiet";
	case CLEVO_PERF_MAX_FAN:
		return "max-fan";
	default:
		return "unknown";
	}
}

static bool clevo_perf_mode_from_name(const char *name, u8 *mode)
{
	if (sysfs_streq(name, "balanced"))
		*mode = CLEVO_PERF_BALANCED;
	else if (sysfs_streq(name, "performance"))
		*mode = CLEVO_PERF_PERFORMANCE;
	else if (sysfs_streq(name, "quiet"))
		*mode = CLEVO_PERF_QUIET;
	else if (sysfs_streq(name, "max-fan"))
		*mode = CLEVO_PERF_MAX_FAN;
	else
		return false;

	return true;
}

static ssize_t performance_mode_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct clevo_data *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", clevo_perf_mode_to_name(priv->perf_mode));
}

static ssize_t performance_mode_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct clevo_data *priv = dev_get_drvdata(dev);
	struct acpi_device *adev = ACPI_COMPANION(dev);
	u8 mode;

	if (!clevo_perf_mode_from_name(buf, &mode))
		return -EINVAL;

	if (!clevo_dchu_cmd(adev->handle, CLEVO_PERF_MODE_SET,
			    (1U << 24) | mode))
		return -EIO;

	priv->perf_mode = mode;

	return count;
}
static DEVICE_ATTR_RW(performance_mode);

static struct attribute *clevo_perf_attrs[] = {
	&dev_attr_performance_mode.attr,
	NULL,
};
static const struct attribute_group clevo_perf_group = {
	.attrs = clevo_perf_attrs,
};

// Continuous per-fan duty control, distinct from the discrete profiles
// above. Same DCHU DSM interface, different method IDs -- confirmed
// against TUXEDO Computers' open-source tuxedo-drivers (clevo_interfaces.h)
// to be a second, independently-implemented command family on this
// board's own firmware, not just present on TUXEDO's hardware. Live-tested
// 2026-08-09: GET_FANINFO* correctly decoded duty/temperature and
// correctly detected this board only has 2 real fan slots (fan index 2
// reads back as absent); SET_FANSPEED_VALUE produced an exact,
// proportional RPM change with temperatures unaffected; SET_FANSPEED_AUTO
// (0x0F bitmask) returned both fans to normal firmware-auto behavior
// within seconds. See ~/odm-laptop-research/NOTES.md for the full
// writeup this is based on.
#define CLEVO_CMD_GET_FANINFO1 0x63
#define CLEVO_CMD_GET_FANINFO2 0x64
#define CLEVO_CMD_GET_FANINFO3 0x6e
#define CLEVO_CMD_SET_FANSPEED_VALUE 0x68
#define CLEVO_CMD_SET_FANSPEED_AUTO 0x69
#define CLEVO_FAN_AUTO_RELEASE_MASK 0x0F

// If nothing pets the watchdog within this many ms of the last manual
// duty write, clevo_fan_watchdog_work_fn() releases fan control back to
// firmware auto control on its own, independent of userspace -- a
// dead-man's-switch so a crashed/killed/hung control daemon can never
// leave the fan stuck at a stale speed.
#define CLEVO_FAN_WATCHDOG_MIN_MS 5000
#define CLEVO_FAN_WATCHDOG_MAX_MS 60000
#define CLEVO_FAN_WATCHDOG_DEFAULT_MS 15000

static const u8 clevo_fan_info_cmd[CLEVO_FAN_COUNT] = {
	CLEVO_CMD_GET_FANINFO1,
	CLEVO_CMD_GET_FANINFO2,
	CLEVO_CMD_GET_FANINFO3,
};

// Byte layout of a GET_FANINFO* result: bits 0-7 = live duty (0-255),
// bits 8-15 = a less reliable temperature reading, bits 16-23 = a more
// reliable signed temperature reading ("temp2"). A fan slot that doesn't
// physically exist reads back with temp2 <= 1.
static u8 clevo_fan_decode_duty(u32 data)
{
	return data & 0xFF;
}

static s8 clevo_fan_decode_temp(u32 data)
{
	return (s8)((data >> 16) & 0xFF);
}

static s8 clevo_fan_decode_temp_alt(u32 data)
{
	return (s8)((data >> 8) & 0xFF);
}

static bool clevo_fan_present_data(u32 data)
{
	return clevo_fan_decode_temp(data) > 1;
}

static u8 clevo_fan_percent_to_raw(u8 percent)
{
	return DIV_ROUND_CLOSEST((unsigned int)percent * 0xFF, 100);
}

static u8 clevo_fan_raw_to_percent(u8 raw)
{
	return DIV_ROUND_CLOSEST((unsigned int)raw * 100, 0xFF);
}

static bool clevo_fan_read(acpi_handle handle, enum clevo_fan_index idx,
			   u32 *data)
{
	return clevo_dchu_cmd_get(handle, clevo_fan_info_cmd[idx], 0, data);
}

// Called once at probe to find out how many fans this board actually
// has -- the DMI table below spans several boards, with no guarantee
// they all share this board's 2-fan layout.
static void clevo_fan_detect_present(struct clevo_data *priv,
				     acpi_handle handle)
{
	int i;

	priv->fan_present = 0;

	for (i = 0; i < CLEVO_FAN_COUNT; i++) {
		u32 data;

		if (clevo_fan_read(handle, i, &data) && clevo_fan_present_data(data))
			priv->fan_present |= BIT(i);
	}
}

// Must be called with fan_lock held. SET_FANSPEED_VALUE packs all three
// fan slots into a single 32-bit argument -- writing just one fan's byte
// without resending the other two's current value would zero them, so
// the full packed word is always rebuilt from the cache and sent as one
// call (also closes the TOCTOU window a separate read-then-write would
// have against a concurrent writer or the watchdog).
static bool clevo_fan_apply_locked(struct clevo_data *priv, acpi_handle handle)
{
	u32 packed = priv->fan_duty_raw[CLEVO_FAN0] |
		     (priv->fan_duty_raw[CLEVO_FAN1] << 8) |
		     (priv->fan_duty_raw[CLEVO_FAN2] << 16);

	return clevo_dchu_cmd(handle, CLEVO_CMD_SET_FANSPEED_VALUE, packed);
}

// Arms/extends the dead-man's-switch. Must be called with fan_lock held.
// Lazily started on first use, so a machine that never enables manual
// fan control never pays any periodic timer cost.
static void clevo_fan_watchdog_pet_locked(struct clevo_data *priv)
{
	priv->fan_manual_active = true;
	mod_delayed_work(system_wq, &priv->fan_watchdog_work,
			 priv->fan_watchdog_timeout_jiffies);
}

// The watchdog timeout callback. Uses delayed_work rather than a plain
// timer_list, and a mutex rather than a spinlock, because it needs to
// call clevo_dchu_cmd() -- which sleeps (ACPICA control-method
// evaluation) -- something a timer_list softirq callback cannot do.
static void clevo_fan_watchdog_work_fn(struct work_struct *work)
{
	struct clevo_data *priv = container_of(to_delayed_work(work),
					       struct clevo_data,
					       fan_watchdog_work);
	struct acpi_device *adev = ACPI_COMPANION(&priv->pdev->dev);

	mutex_lock(&priv->fan_lock);
	if (!priv->fan_manual_active) {
		mutex_unlock(&priv->fan_lock);
		return;
	}
	priv->fan_manual_active = false;
	mutex_unlock(&priv->fan_lock);

	clevo_dchu_cmd(adev->handle, CLEVO_CMD_SET_FANSPEED_AUTO,
		      CLEVO_FAN_AUTO_RELEASE_MASK);
	dev_warn(&priv->pdev->dev,
		"fan watchdog: no heartbeat for %ums, released to firmware auto control\n",
		jiffies_to_msecs(priv->fan_watchdog_timeout_jiffies));
}

// Full graceful release: used by the explicit "fan_release" sysfs store,
// and by the remove()/module-exit path. Cancels the watchdog *outside*
// the lock -- the work function above wants the same lock, so canceling
// while holding it risks deadlock -- then always issues the actual EC
// release call regardless of race outcome, since that call is idempotent
// and harmless even if the watchdog already fired on its own.
static void clevo_fan_release(struct clevo_data *priv, acpi_handle handle)
{
	mutex_lock(&priv->fan_lock);
	priv->fan_manual_active = false;
	mutex_unlock(&priv->fan_lock);

	cancel_delayed_work_sync(&priv->fan_watchdog_work);

	clevo_dchu_cmd(handle, CLEVO_CMD_SET_FANSPEED_AUTO,
		      CLEVO_FAN_AUTO_RELEASE_MASK);
}

static ssize_t clevo_fan_duty_show(acpi_handle handle,
				   enum clevo_fan_index idx, char *buf)
{
	u32 data;

	if (!clevo_fan_read(handle, idx, &data))
		return -EIO;

	return sysfs_emit(buf, "%u\n", clevo_fan_raw_to_percent(clevo_fan_decode_duty(data)));
}

static ssize_t clevo_fan_temp_show(acpi_handle handle,
				   enum clevo_fan_index idx, char *buf)
{
	u32 data;

	if (!clevo_fan_read(handle, idx, &data))
		return -EIO;

	return sysfs_emit(buf, "%d\n", clevo_fan_decode_temp(data));
}

static ssize_t clevo_fan_temp_alt_show(acpi_handle handle,
				       enum clevo_fan_index idx, char *buf)
{
	u32 data;

	if (!clevo_fan_read(handle, idx, &data))
		return -EIO;

	return sysfs_emit(buf, "%d\n", clevo_fan_decode_temp_alt(data));
}

static ssize_t clevo_fan_manual_duty_show(struct clevo_data *priv,
					  enum clevo_fan_index idx, char *buf)
{
	u8 raw;

	mutex_lock(&priv->fan_lock);
	raw = priv->fan_duty_raw[idx];
	mutex_unlock(&priv->fan_lock);

	return sysfs_emit(buf, "%u\n", clevo_fan_raw_to_percent(raw));
}

static ssize_t clevo_fan_manual_duty_store(struct clevo_data *priv,
					   acpi_handle handle,
					   enum clevo_fan_index idx,
					   const char *buf, size_t count)
{
	u8 percent;
	int err;
	bool ok;

	err = kstrtou8(buf, 10, &percent);
	if (err)
		return err;
	if (percent > 100)
		return -EINVAL;

	mutex_lock(&priv->fan_lock);
	priv->fan_duty_raw[idx] = clevo_fan_percent_to_raw(percent);
	ok = clevo_fan_apply_locked(priv, handle);
	if (ok)
		clevo_fan_watchdog_pet_locked(priv);
	mutex_unlock(&priv->fan_lock);

	if (!ok)
		return -EIO;

	return count;
}

#define CLEVO_FAN_INFO_ATTR(_name, _idx, _field)				\
static ssize_t _name##_show(struct device *dev,				\
			     struct device_attribute *attr, char *buf)		\
{										\
	struct acpi_device *adev = ACPI_COMPANION(dev);			\
	return clevo_fan_##_field##_show(adev->handle, _idx, buf);		\
}										\
static DEVICE_ATTR_RO(_name)

CLEVO_FAN_INFO_ATTR(fan1_duty, CLEVO_FAN0, duty);
CLEVO_FAN_INFO_ATTR(fan2_duty, CLEVO_FAN1, duty);
CLEVO_FAN_INFO_ATTR(fan3_duty, CLEVO_FAN2, duty);
CLEVO_FAN_INFO_ATTR(fan1_temp, CLEVO_FAN0, temp);
CLEVO_FAN_INFO_ATTR(fan2_temp, CLEVO_FAN1, temp);
CLEVO_FAN_INFO_ATTR(fan3_temp, CLEVO_FAN2, temp);
CLEVO_FAN_INFO_ATTR(fan1_temp_alt, CLEVO_FAN0, temp_alt);
CLEVO_FAN_INFO_ATTR(fan2_temp_alt, CLEVO_FAN1, temp_alt);
CLEVO_FAN_INFO_ATTR(fan3_temp_alt, CLEVO_FAN2, temp_alt);

#define CLEVO_FAN_MANUAL_DUTY_ATTR(_name, _idx)				\
static ssize_t _name##_show(struct device *dev,				\
			     struct device_attribute *attr, char *buf)		\
{										\
	return clevo_fan_manual_duty_show(dev_get_drvdata(dev), _idx, buf);	\
}										\
static ssize_t _name##_store(struct device *dev,				\
			      struct device_attribute *attr,			\
			      const char *buf, size_t count)			\
{										\
	struct clevo_data *priv = dev_get_drvdata(dev);			\
	struct acpi_device *adev = ACPI_COMPANION(dev);			\
	return clevo_fan_manual_duty_store(priv, adev->handle, _idx, buf, count); \
}										\
static DEVICE_ATTR_RW(_name)

CLEVO_FAN_MANUAL_DUTY_ATTR(fan1_manual_duty, CLEVO_FAN0);
CLEVO_FAN_MANUAL_DUTY_ATTR(fan2_manual_duty, CLEVO_FAN1);
CLEVO_FAN_MANUAL_DUTY_ATTR(fan3_manual_duty, CLEVO_FAN2);

static ssize_t fan_manual_active_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct clevo_data *priv = dev_get_drvdata(dev);
	bool active;

	mutex_lock(&priv->fan_lock);
	active = priv->fan_manual_active;
	mutex_unlock(&priv->fan_lock);

	return sysfs_emit(buf, "%u\n", active ? 1 : 0);
}
static DEVICE_ATTR_RO(fan_manual_active);

static ssize_t fan_watchdog_timeout_ms_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct clevo_data *priv = dev_get_drvdata(dev);
	unsigned int ms;

	mutex_lock(&priv->fan_lock);
	ms = jiffies_to_msecs(priv->fan_watchdog_timeout_jiffies);
	mutex_unlock(&priv->fan_lock);

	return sysfs_emit(buf, "%u\n", ms);
}

static ssize_t fan_watchdog_timeout_ms_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t count)
{
	struct clevo_data *priv = dev_get_drvdata(dev);
	unsigned int ms;
	int err;

	err = kstrtouint(buf, 10, &ms);
	if (err)
		return err;
	if (ms < CLEVO_FAN_WATCHDOG_MIN_MS || ms > CLEVO_FAN_WATCHDOG_MAX_MS)
		return -EINVAL;

	mutex_lock(&priv->fan_lock);
	priv->fan_watchdog_timeout_jiffies = msecs_to_jiffies(ms);
	mutex_unlock(&priv->fan_lock);

	return count;
}
static DEVICE_ATTR_RW(fan_watchdog_timeout_ms);

static ssize_t fan_watchdog_ping_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct clevo_data *priv = dev_get_drvdata(dev);

	mutex_lock(&priv->fan_lock);
	if (priv->fan_manual_active)
		clevo_fan_watchdog_pet_locked(priv);
	mutex_unlock(&priv->fan_lock);

	return count;
}
static DEVICE_ATTR_WO(fan_watchdog_ping);

static ssize_t fan_release_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct clevo_data *priv = dev_get_drvdata(dev);
	struct acpi_device *adev = ACPI_COMPANION(dev);

	clevo_fan_release(priv, adev->handle);

	return count;
}
static DEVICE_ATTR_WO(fan_release);

static struct attribute *clevo_fan_attrs[] = {
	&dev_attr_fan1_duty.attr,
	&dev_attr_fan2_duty.attr,
	&dev_attr_fan3_duty.attr,
	&dev_attr_fan1_temp.attr,
	&dev_attr_fan2_temp.attr,
	&dev_attr_fan3_temp.attr,
	&dev_attr_fan1_temp_alt.attr,
	&dev_attr_fan2_temp_alt.attr,
	&dev_attr_fan3_temp_alt.attr,
	&dev_attr_fan1_manual_duty.attr,
	&dev_attr_fan2_manual_duty.attr,
	&dev_attr_fan3_manual_duty.attr,
	&dev_attr_fan_manual_active.attr,
	&dev_attr_fan_watchdog_timeout_ms.attr,
	&dev_attr_fan_watchdog_ping.attr,
	&dev_attr_fan_release.attr,
	NULL,
};

// Hides the per-fan attributes for slots this specific board doesn't
// have -- the DMI table below spans several boards, not all necessarily
// sharing this board's 2-fan layout. The always-present control
// attributes (fan_manual_active, watchdog, release) are never hidden;
// only the first 12 positions (3 fans x {duty, temp, temp_alt,
// manual_duty}) are gated per-fan.
static umode_t clevo_fan_attr_is_visible(struct kobject *kobj,
					 struct attribute *attr, int n)
{
	struct clevo_data *priv = dev_get_drvdata(kobj_to_dev(kobj));
	static const enum clevo_fan_index per_fan_attr_index[] = {
		CLEVO_FAN0, CLEVO_FAN1, CLEVO_FAN2, // duty
		CLEVO_FAN0, CLEVO_FAN1, CLEVO_FAN2, // temp
		CLEVO_FAN0, CLEVO_FAN1, CLEVO_FAN2, // temp_alt
		CLEVO_FAN0, CLEVO_FAN1, CLEVO_FAN2, // manual_duty
	};

	if (n < ARRAY_SIZE(per_fan_attr_index) &&
	    !(priv->fan_present & BIT(per_fan_attr_index[n])))
		return 0;

	return attr->mode;
}

static const struct attribute_group clevo_fan_group = {
	.attrs = clevo_fan_attrs,
	.is_visible = clevo_fan_attr_is_visible,
};

static const struct attribute_group *clevo_acpi_groups[] = {
	&clevo_kbd_zone_group,
	&clevo_battery_group,
	&clevo_perf_group,
	&clevo_fan_group,
	NULL,
};

// TODO: Model-specific quirks
#define SYSTEM76_DMI(version) { \
		.matches = { \
			DMI_MATCH(DMI_SYS_VENDOR, "System76"), \
			DMI_MATCH(DMI_PRODUCT_VERSION, version), \
		}, \
	}

#define BOARD_DMI(board) { \
		.matches = { \
			DMI_MATCH(DMI_BOARD_NAME, board), \
		}, \
	}

// XXX: Limit functionality to latest models while drivers are being reworked.
static const struct dmi_system_id system76_dmi_table[] = {
	SYSTEM76_DMI("addp6"),
	SYSTEM76_DMI("lemp14"),
	SYSTEM76_DMI("lemp14-b"),
	SYSTEM76_DMI("oryp14"),
	// For testing; model has lightbar
	//SYSTEM76_DMI("serw14"),
	// Clevo L550JNP barebone (Panther Lake), pre-OEM-branding DMI strings
	BOARD_DMI("L55xJNP_N_Mx"),
	{ }

};
MODULE_DEVICE_TABLE(dmi, system76_dmi_table);

static int clevo_acpi_probe(struct platform_device *pdev)
{
	struct clevo_data *priv;
	struct acpi_device *adev;
	int err;

	dev_dbg(&pdev->dev, "probe\n");

	adev = ACPI_COMPANION(&pdev->dev);
	if (!adev)
		return -ENODEV;

	if (!dmi_check_system(system76_dmi_table)) {
		pr_info("model does not utilize this driver\n");
		return -ENODEV;
	}

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);
	priv->pdev = pdev;

	mutex_init(&priv->fan_lock);
	INIT_DELAYED_WORK(&priv->fan_watchdog_work, clevo_fan_watchdog_work_fn);
	priv->fan_watchdog_timeout_jiffies = msecs_to_jiffies(CLEVO_FAN_WATCHDOG_DEFAULT_MS);
	clevo_fan_detect_present(priv, adev->handle);

	err = clevo_input_init(&pdev->dev);
	if (err)
		return err;

	err = clevo_kbled_init(&pdev->dev);
	if (err)
		return err;

	// TODO: Use `devm_acpi_install_notify_handler` when available.
	err = acpi_dev_install_notify_handler(adev, ACPI_ALL_NOTIFY,
					      clevo_acpi_notify, &pdev->dev);
	if (err)
		return err;

	clevo_enable_notify_events(adev->handle);

	return 0;
}

static void clevo_acpi_remove(struct platform_device *pdev)
{
	struct clevo_data *priv = dev_get_drvdata(&pdev->dev);

	dev_dbg(&pdev->dev, "remove\n");

	// Independent safety net alongside fan_release()'s own callers: an
	// rmmod/DKMS-reinstall cycle (e.g. via install.sh) can never leave a
	// stale manual fan duty pinned, even if userspace already exited
	// cleanly for some unrelated reason.
	clevo_fan_release(priv, ACPI_COMPANION(&pdev->dev)->handle);

	acpi_dev_remove_notify_handler(ACPI_COMPANION(&pdev->dev),
				       ACPI_ALL_NOTIFY, clevo_acpi_notify);
}

static const struct acpi_device_id clevo_acpi_ids[] = {
	{ "CLV0001", 0 },
	{ },
};
MODULE_DEVICE_TABLE(acpi, clevo_acpi_ids);

static struct platform_driver clevo_acpi_driver = {
	.probe = clevo_acpi_probe,
	.remove = clevo_acpi_remove,
	.driver = {
		.name = "clevo-acpi",
		.acpi_match_table = clevo_acpi_ids,
		.dev_groups = clevo_acpi_groups,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 17, 0)
		.pm = pm_sleep_ptr(&clevo_acpi_pm),
#else
		.pm = pm_ptr(&s76_pm),
#endif
	},
};
module_platform_driver(clevo_acpi_driver);

MODULE_DESCRIPTION("Clevo ACPI driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.2.0");
