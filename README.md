# clevo-acpi-dkms

Enables keyboard RGB backlight control on generic Clevo/Tongfang-based
laptop barebones that System76's `system76` DKMS package doesn't recognize
yet — because it gates the driver to their own branded models by DMI vendor
string, not because the hardware itself is unsupported.

For actually *controlling* the backlight once it's enabled (GUI + CLI,
permissions, boot persistence), see
[keyboardcolors](https://github.com/arbitrary-string/keyboardcolors). This
repo only concerns itself with getting the kernel driver to bind.

## Background

Many of these laptops (System76, and barebones from other resellers built on
the same Clevo/Tongfang/Uniwill ODM hardware) expose keyboard RGB control
through an ACPI method (`ECMD`, on the embedded controller) plus a `_DSM`
method on ACPI device `CLV0001`. System76 publishes an open-source (GPL-2.0)
driver for exactly this interface, as part of their
[`system76`](https://github.com/pop-os/system76) package's `clevo-acpi.c`.

That driver works great — but it only binds on a hardcoded list of System76
product codenames (matched via `DMI_SYS_VENDOR == "System76"` plus a specific
`DMI_PRODUCT_VERSION`). A generic/unbranded barebone reports `sys_vendor:
Notebook` and a raw board name, so the driver correctly identifies the ACPI
interface but refuses to attach: `clevo_acpi: model does not utilize this
driver`. This project patches that whitelist to also match by
`DMI_BOARD_NAME`, and adds per-zone (left/center/right/numpad/lightbar)
custom-color sysfs attributes the upstream driver doesn't expose (it only
cycles through 7 fixed colors via the keyboard's own Fn shortcut).

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

- Debian/Ubuntu (or similar) with the `system76` DKMS package already
  installed (`apt install system76-dkms` or via System76's own driver PPA/
  repo — this project patches their source, it doesn't replace it)
- `dkms`, `patch`, `sudo`

## Usage

```
git clone https://github.com/arbitrary-string/clevo-acpi-dkms.git
cd clevo-acpi-dkms
./install.sh
```

Safe to re-run — skips the patch step if already applied, and always
re-patches after a `system76` package upgrade overwrites the source under
`/usr/src`. Rebuilds/reinstalls the DKMS module and reloads it.

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

- This sends the same EC commands System76's own driver already sends on
  supported hardware (command `0xCA`, documented zone bytes) — it does not
  add any new/unverified command paths, only widens which boards can reach
  the existing ones.
- Not affiliated with System76 or TUXEDO Computers. It's a small patch to
  System76's GPL-licensed source, distributed as a patch (not a fork) so it
  stays trivial to diff against upstream and re-apply after their updates.
- Out of scope on purpose: fan curves, power profiles, and other EC
  functionality this driver family also exposes. Keyboard backlight only —
  smaller, easier-to-trust surface area for hardware we haven't tested.

## License

The patch modifies GPL-2.0-or-later licensed code
(`system76/clevo-acpi.c`); this repo's own files (patch, install script,
this README) are likewise GPL-2.0-or-later — see [LICENSE](LICENSE).
