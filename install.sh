#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Installs the L550JNP-enabled clevo-acpi.c (DMI-whitelist + per-zone RGB)
# into the System76 `system76` DKMS package's source tree, then rebuilds/
# reinstalls/reloads the module for the running kernel.
#
# This package only enables the kernel driver (the clevo-acpi LED device).
# For a GUI/CLI to actually control it, with permissions/persistence set up,
# install the `keyboardcolors` app separately.
#
# files/clevo-acpi.c is deployed as a complete file, not a patch: the
# `system76` package's PPA snapshot at any given time may or may not already
# contain clevo-acpi.c at all (it was added upstream partway through this
# package's history, and PPA snapshots have been observed to roll backward
# to versions that predate it) -- rather than depend on a patch applying
# cleanly against a base file that might not exist, this always deploys our
# known-good complete file directly. patches/clevo-acpi-l550jnp.patch is kept
# for reference (it shows exactly what changed relative to upstream) but
# install.sh no longer depends on it applying.
#
# Safe to re-run: overwriting with identical content is a no-op, and it
# always re-deploys after a `system76` package upgrade replaces the source
# under /usr/src.
set -euo pipefail

FILES_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/files"
KVER="$(uname -r)"

SRC_DIR=$(find /usr/src -maxdepth 1 -type d -name 'system76-[0-9]*' | sort -V | tail -1)
if [ -z "$SRC_DIR" ]; then
	echo "error: no /usr/src/system76-* DKMS source tree found (is the 'system76' package installed?)" >&2
	exit 1
fi

PKG_VERSION="$(basename "$SRC_DIR" | sed 's/^system76-//')"
echo "Using DKMS source: $SRC_DIR (system76 $PKG_VERSION)"

echo "Deploying clevo-acpi.c..."
sudo cp "$FILES_DIR/clevo-acpi.c" "$SRC_DIR/clevo-acpi.c"

if ! sudo grep -q "clevo-acpi" "$SRC_DIR/Kbuild"; then
	echo "obj-m += clevo-acpi.o" | sudo tee -a "$SRC_DIR/Kbuild" >/dev/null
fi

if ! sudo grep -q "clevo-acpi" "$SRC_DIR/dkms.conf"; then
	printf 'BUILT_MODULE_NAME[1]="clevo-acpi"\nDEST_MODULE_LOCATION[1]="/updates/dkms"\n' \
		| sudo tee -a "$SRC_DIR/dkms.conf" >/dev/null
fi

echo "Rebuilding via DKMS for kernel $KVER..."
sudo dkms build -m system76 -v "$PKG_VERSION" -k "$KVER" --force
sudo dkms install -m system76 -v "$PKG_VERSION" -k "$KVER" --force

echo "Reloading clevo_acpi module..."
sudo rmmod clevo_acpi 2>/dev/null || true
sudo modprobe clevo_acpi

# Workaround: on this hardware, the automatic cold-boot module load
# consistently fails clevo_acpi's DMI check (observed across multiple
# reboots), even though the exact same module reloaded manually right
# after boot binds correctly every time. A oneshot reload once the system
# is fully up works around it reliably.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
sudo install -m 0644 "$SCRIPT_DIR/clevo-acpi-reload.service" \
	/etc/systemd/system/clevo-acpi-reload.service
sudo systemctl daemon-reload
sudo systemctl enable clevo-acpi-reload.service

echo
echo "Driver enabled. Check: ls -la /sys/devices/platform/CLV0001:00/"
echo "To actually control it (permissions, GUI/CLI, boot persistence), install"
echo "the keyboardcolors app: https://github.com/arbitrary-string/keyboardcolors"
