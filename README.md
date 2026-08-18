# clevo-acpi-dkms

Enables keyboard RGB backlight control, and (on boards whose EC supports it)
battery charge threshold, performance/fan mode, and continuous fan-curve
control, on generic Clevo/Tongfang-based laptop barebones that System76's
`system76` DKMS package doesn't recognize yet — because it gates the driver
to their own branded models by DMI vendor string, not because the hardware
itself is unsupported.

For actually *controlling* these features once the driver is enabled (GUI +
CLI, permissions, boot persistence), see
[Clevo Control Panel](https://github.com/arbitrary-string/clevo-control-panel)
(formerly `keyboardcolors`, renamed when battery threshold support was
added). This repo only concerns itself with getting the kernel driver to
bind and exposing the right sysfs attributes.

**This repo is a full merge of
[`pop-os/system76-dkms`](https://github.com/pop-os/system76-dkms)'s
history**, not just a single patched file — `git clone` this repo and run
`./install.sh` and you have everything needed to build and install,
including on a board this project doesn't recognize (it'll just skip the
generic-barebone whitelist and behave exactly like upstream). No System76
apt repo/PPA to add or remove, no risk of pulling in unrelated package/kernel
upgrades just to satisfy one dependency.

## Background

Many of these laptops (System76, and barebones from other resellers built on
the same Clevo/Tongfang/Uniwill ODM hardware) expose keyboard RGB control
through an ACPI method (`ECMD`, on the embedded controller) plus a `_DSM`
method on ACPI device `CLV0001`. System76 publishes an open-source (GPL-2.0)
driver for exactly this interface: `clevo-acpi.c`, part of the
`system76-dkms` package this repo is built from.

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
works unmodified; a `performance_mode` attribute (quiet/balanced/
performance/max-fan); and continuous per-fan duty control with a
kernel-side watchdog dead-man's-switch (see "Safety notes" below). All of
this only lives in `src/clevo-acpi.c` — everything else merged in from
upstream (`system76.c`, keyboard/AP-LED handling for real System76
hardware, etc.) is untouched. `git log -- src/clevo-acpi.c` shows exactly
what changed and when, against the real upstream history.

## Is your board supported?

Check first:

```
ls /sys/bus/acpi/devices/ | grep CLV0001   # the ACPI device must exist
cat /sys/class/dmi/id/board_name           # note this exact string
```

If `CLV0001` exists, your EC almost certainly speaks the same protocol —
see the DMI whitelist in `src/clevo-acpi.c` for boards already added. If
yours isn't listed, see "Adding your board" below before assuming it
won't work.

## Requirements

- Debian/Ubuntu (or similar), with `dkms`, `git`, and a kernel headers/
  build-tools set matching your running kernel (`linux-headers-$(uname -r)`
  on Debian/Ubuntu) already installed. No System76 apt repo/PPA needed —
  this repo builds and installs the whole driver package itself.
- `sudo`

## Usage

```
git clone https://github.com/arbitrary-string/clevo-acpi-dkms.git
cd clevo-acpi-dkms
./install.sh
```

`install.sh` checks your board against the same DMI whitelist compiled
into `src/clevo-acpi.c` (both matching mechanisms — System76's own
vendor+version match and the generic board-name match) before doing
anything else. If your board isn't recognized, it explains why and
points you at "Adding your board" below instead of building and
installing a driver that would just silently refuse to bind.

Safe to re-run. Exports this repo's *last commit* (via `git archive HEAD`,
so uncommitted edits aren't picked up) into `/usr/src/clevo-acpi-<version>/`
(a name that can never collide with a real `system76-dkms` apt install, if
you happen to have one), registers and builds it with DKMS under its own
package name, and reloads the module. Re-running after committing local
changes picks them up (it removes any prior DKMS registration under the
same name/version first, since DKMS keys off name+version, not file
contents).

**Upgrading from an older checkout of this repo** (before it merged
`system76-dkms`'s full source): if you have the `system76-dkms` apt
package installed, its DKMS registration shares the exact module names
(`system76`, `clevo-acpi`) this repo now registers independently under
its own package name. Remove the old one first to avoid the two fighting
over the same installed files:

```
sudo dkms remove -m system76 -v <version-from-dkms-status> --all
sudo apt remove system76-dkms
```

Then run `./install.sh` as above, and reload once more
(`sudo rmmod clevo_acpi && sudo modprobe clevo_acpi`) to make sure the
currently-loaded module matches what actually got installed, not
whatever `dkms remove` may have restored from an archived backup in the
process.

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
- Not affiliated with System76 or TUXEDO Computers. This repo is a full
  merge of System76's GPL-licensed `system76-dkms` source
  (`pop-os/system76-dkms`), with changes scoped to `src/clevo-acpi.c` only
  — `git log -- src/clevo-acpi.c` shows exactly what changed against the
  real upstream history.
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

This repo is a full merge of GPL-2.0-or-later licensed code from
[`pop-os/system76-dkms`](https://github.com/pop-os/system76-dkms); this
project's own additions (the `src/clevo-acpi.c` changes, `install.sh`,
this README) are likewise GPL-2.0-or-later — see [LICENSE](LICENSE).
