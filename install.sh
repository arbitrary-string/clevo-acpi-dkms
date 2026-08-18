#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds and installs this repo's driver directly via DKMS -- no separate
# system76-dkms apt/PPA package needed. This repo is a full merge of
# pop-os/system76-dkms with clevo-acpi.c's DMI whitelist extended for
# generic/rebranded Clevo/Tongfang barebones alongside System76's own
# branded models (see README for the list and how to add yours).
#
# This package only enables the kernel driver (the clevo-acpi LED device).
# For a GUI/CLI to actually control it, with permissions/persistence set up,
# install the `clevo-control-panel` app separately.
#
# Safe to re-run.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KVER="$(uname -r)"
PKG_NAME="clevo-acpi"
PKG_VERSION="$(sed -n 's/^PACKAGE_VERSION="\(.*\)"$/\1/p' "$SCRIPT_DIR/dkms.conf")"
DKMS_SRC_DIR="/usr/src/${PKG_NAME}-${PKG_VERSION}"

# Mirrors system76_dmi_table[] in src/clevo-acpi.c exactly -- two
# independent matching mechanisms, not one flat list, so this has to
# check both DMI fields the way the driver itself does:
#   - SYSTEM76_DMI(version): sys_vendor == "System76" AND
#     product_version == <version>, for System76's own branded models.
#   - BOARD_DMI(board): board_name == <board> directly, for generic/
#     rebranded Clevo/Tongfang barebones.
# Keep this list in sync with src/clevo-acpi.c whenever a board is added.
SYSTEM76_MODELS=("addp6" "lemp14" "lemp14-b" "oryp14")
GENERIC_BOARDS=("L55xJNP_N_Mx")

BOARD_NAME="$(cat /sys/class/dmi/id/board_name 2>/dev/null || echo unknown)"
SYS_VENDOR="$(cat /sys/class/dmi/id/sys_vendor 2>/dev/null || echo unknown)"
PRODUCT_VERSION="$(cat /sys/class/dmi/id/product_version 2>/dev/null || echo unknown)"

SUPPORTED=0
if [ "$SYS_VENDOR" = "System76" ]; then
	for m in "${SYSTEM76_MODELS[@]}"; do
		[ "$m" = "$PRODUCT_VERSION" ] && SUPPORTED=1
	done
fi
for b in "${GENERIC_BOARDS[@]}"; do
	[ "$b" = "$BOARD_NAME" ] && SUPPORTED=1
done

if [ "$SUPPORTED" != 1 ]; then
	cat >&2 <<EOF
This board isn't in the known-supported list, so the compiled driver
would refuse to bind (clevo_acpi: model does not utilize this driver)
and this install would do nothing useful. Detected:
  board_name:      $BOARD_NAME
  sys_vendor:       $SYS_VENDOR
  product_version:  $PRODUCT_VERSION

If your board's EC actually speaks the same protocol (check: does
/sys/bus/acpi/devices/ list CLV0001?), see "Adding your board" in
README.md to find your own EC's offsets/values and open a PR -- don't
guess or copy another board's values.
EOF
	exit 1
fi

echo "Installing source to $DKMS_SRC_DIR..."
sudo rm -rf "$DKMS_SRC_DIR"
sudo mkdir -p "$DKMS_SRC_DIR"
git -C "$SCRIPT_DIR" archive HEAD | sudo tar -x -C "$DKMS_SRC_DIR"

# Remove any prior registration of this exact name/version first: dkms
# keys off name+version, not file contents, so re-running this script
# after local source edits would otherwise silently reuse a stale build.
sudo dkms remove -m "$PKG_NAME" -v "$PKG_VERSION" --all 2>/dev/null || true

echo "Registering and building via DKMS for kernel $KVER..."
sudo dkms add -m "$PKG_NAME" -v "$PKG_VERSION"
sudo dkms build -m "$PKG_NAME" -v "$PKG_VERSION" -k "$KVER"
sudo dkms install -m "$PKG_NAME" -v "$PKG_VERSION" -k "$KVER" --force

echo "Reloading clevo_acpi module..."
sudo rmmod clevo_acpi 2>/dev/null || true
sudo modprobe clevo_acpi

# Workaround: on this hardware, the automatic cold-boot module load
# consistently fails clevo_acpi's DMI check (observed across multiple
# reboots), even though the exact same module reloaded manually right
# after boot binds correctly every time. A oneshot reload once the system
# is fully up works around it reliably.
sudo install -m 0644 "$SCRIPT_DIR/clevo-acpi-reload.service" \
	/etc/systemd/system/clevo-acpi-reload.service
sudo systemctl daemon-reload
sudo systemctl enable clevo-acpi-reload.service

echo
echo "Driver enabled. Check: ls -la /sys/devices/platform/CLV0001:00/"
echo "To actually control it (permissions, GUI/CLI, boot persistence), install"
echo "the clevo-control-panel app: https://github.com/arbitrary-string/clevo-control-panel"
