# clevo-acpi-dkms

Enables keyboard RGB backlight control, and (on boards whose EC supports it)
battery charge threshold control, on generic Clevo/Tongfang-based laptop
barebones that System76's `system76` DKMS package doesn't recognize yet —
because it gates the driver to their own branded models by DMI vendor
string, not because the hardware itself is unsupported.

For actually *controlling* these features once the driver is enabled (GUI +
CLI, permissions, boot persistence), see
[Clevo Control Panel](https://github.com/arbitrary-string/clevo-control-panel)
(formerly `keyboardcolors`, renamed when battery threshold support was
added). This repo only concerns itself with getting the kernel driver to
bind and exposing the right sysfs attributes.

## Background

Many of these laptops (System76, and barebones from other resellers built on
the same Clevo/Tongfang/Uniwill ODM hardware) expose keyboard RGB control
through an ACPI method (`ECMD`, on the embedded controller) plus a `_DSM`
method on ACPI device `CLV0001`. System76 publishes an open-source (GPL-2.0)
driver for exactly this interface: `clevo-acpi.c` in
[`pop-os/system76-dkms`](https://github.com/pop-os/system76-dkms) (the
`system76-dkms` package). Note this file may not be present in whatever
version of that package your distro/PPA currently serves — it was added
partway through the project's history, and PPA snapshots have been observed
to lag behind or roll back past it. `files/clevo-acpi.c` in this repo is our
own copy, taken directly from that upstream source, so this doesn't depend
on your local `system76-dkms` source tree already having it.

That driver works great — but it only binds on a hardcoded list of System76
product codenames (matched via `DMI_SYS_VENDOR == "System76"` plus a specific
`DMI_PRODUCT_VERSION`). A generic/unbranded barebone reports `sys_vendor:
Notebook` and a raw board name, so the driver correctly identifies the ACPI
interface but refuses to attach: `clevo_acpi: model does not utilize this
driver`. This project adds a `DMI_BOARD_NAME` match to that whitelist, and
adds per-zone (left/center/right/numpad/lightbar) custom-color sysfs
attributes the upstream driver doesn't expose (it only cycles through 7
fixed colors via the keyboard's own Fn shortcut). It also adds standard
`charge_control_start_threshold`/`charge_control_end_threshold` sysfs
attributes for boards whose EC exposes a "flexicharger"-style battery
charge threshold feature (same `\_SB.DCHU` ACPI device and `_DSM`
mechanism as the keyboard, different function indices) — matches the
convention TUXEDO's `tuxedo-drivers` project uses for other Clevo/Tongfang
boards, so existing tooling that knows that standard interface (e.g. TLP)
works unmodified. See `patches/clevo-acpi-l550jnp.patch` for exactly what
changed, as a diff against the unmodified upstream file.

## Is your board supported?

Check first:

```
ls /sys/bus/acpi/devices/ | grep CLV0001   # the ACPI device must exist
cat /sys/class/dmi/id/board_name           # note this exact string
```

If `CLV0001` exists, your EC almost certainly speaks the same protocol —
see `patches/` for boards already added. If yours isn't listed, see
"Adding your board" below before assuming it won't work.

## Requirements

- Debian/Ubuntu (or similar) with the `system76-dkms` package already
  installed (`apt install system76-dkms`, or via System76's own driver PPA/
  repo — this project deploys our own copy of one file into their source
  tree, it doesn't replace the package)
- `dkms`, `sudo`

## Usage

```
git clone https://github.com/arbitrary-string/clevo-acpi-dkms.git
cd clevo-acpi-dkms
./install.sh
```

Safe to re-run — deploys `files/clevo-acpi.c` into the local `system76-dkms`
source tree (overwriting any existing copy, including after a `system76`
package upgrade replaces the source under `/usr/src`), wires it into
`Kbuild`/`dkms.conf` if not already present, then rebuilds/reinstalls the
DKMS module and reloads it.

## Adding your board

Board-specific quirks are why this stays a curated list rather than matching
any device that merely exposes ACPI ID `CLV0001` — different hardware
generations sharing the same generic `ECMD` dispatcher aren't guaranteed to
agree on what a given command byte *means* (upstream's own source has
comments noting exactly this kind of generational drift). Widening the match
casually risks sending a command that means something else entirely on a
board we've never tested.

To propose adding yours, open a PR with:

1. `cat /sys/class/dmi/id/board_name` and `board_vendor`
2. Confirmation `/sys/bus/acpi/devices/CLV0001:00` exists
3. After applying the patch locally with your board added: confirm the
   keyboard actually lights up correctly (right zones, sane colors) and that
   brightness/Fn-key hotkeys still behave — not just that the module loads
   without error

## Safety notes

- Keyboard backlight control sends the same EC commands System76's own
  driver already sends on supported hardware (command `0xCA`, documented
  zone bytes) — it does not add any new/unverified command paths there,
  only widens which boards can reach the existing ones.
- Battery charge threshold control does add a new command path (function
  indices `0x76`/`0x77` on the same `DCHU` `_DSM` device), not present in
  upstream System76 code — verified against this board's own DSDT and
  cross-checked against TUXEDO's independent reverse-engineering of the
  same mechanism on other Clevo/Tongfang boards before being tested live.
- Not affiliated with System76 or TUXEDO Computers. It's a small modification
  to one file of System76's GPL-licensed `system76-dkms` source
  (`pop-os/system76-dkms`) — see `patches/` for a diff against the
  unmodified upstream file.
- The battery charge threshold write path (function index `0x76`) uses the
  same `_DSM`/single-Integer calling convention already validated for the
  keyboard, and was tested read-only first, then a same-value no-op write,
  before any real threshold change — see the writeup this is based on at
  `~/laptopissues/battery-threshold/NOTES.md` (not included in this repo).
- Fan control (function indices `0x63`/`0x64`/`0x6e` to read per-fan duty
  and temperature, `0x68` to set a continuous manual duty, `0x69` to
  release back to firmware auto control) was added after being validated
  live on this exact board: read-back correctly decoded duty/temperature
  and correctly identified this board as having only 2 real fan slots; a
  manual duty write produced the expected proportional RPM change with
  temperatures unaffected (confirming a direct fan-speed effect, not a
  side effect of a thermal/power-state change); and release back to
  firmware auto control was confirmed complete within seconds — see
  `~/odm-laptop-research/NOTES.md` for the full writeup. A kernel-internal
  dead-man's-switch (`fan_watchdog_timeout_ms`/`fan_watchdog_ping`/
  `fan_release`) forces release-to-auto if userspace stops actively
  renewing a manual override — confirmed live, including the case of an
  uncatchably killed (`SIGKILL`) controlling process, so a crashed or
  killed control daemon can never leave the fan stuck at a stale speed.
- Still out of scope on purpose: a second, separate command (`0x79`
  sub-command `0x19`, a quiet/power_saving/performance/entertainment
  scheme distinct from the sub-command `1` the existing
  `performance_mode` attribute uses) was found to have a real, distinct
  effect during the same investigation, but hasn't been characterized
  well enough yet to trust, and conceptually overlaps with
  `performance_mode` — not exposed as a driver attribute.

## License

`files/clevo-acpi.c` is a modified copy of GPL-2.0-or-later licensed code
from [`pop-os/system76-dkms`](https://github.com/pop-os/system76-dkms)
(`src/clevo-acpi.c`); this repo's own files (that modified copy, the
reference patch, install script, this README) are likewise
GPL-2.0-or-later — see [LICENSE](LICENSE).
