#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Applies the L550JNP DMI-whitelist + per-zone RGB patch to the System76
# `system76` DKMS package's clevo-acpi.c, then rebuilds/reinstalls/reloads
# the module for the running kernel.
#
# This package only enables the kernel driver (the clevo-acpi LED device).
# For a GUI/CLI to actually control it, with permissions/persistence set up,
# install the `keyboardcolors` app separately.
#
# Safe to re-run: skips the patch step if already applied (e.g. after a
# `dkms build` triggered by a kernel upgrade already picked it up), and
# always re-applies after a `system76` package upgrade replaces the source
# under /usr/src.
set -euo pipefail

PATCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/patches"
PATCH_FILE="$PATCH_DIR/clevo-acpi-l550jnp.patch"
MARKER="L55xJNP_N_Mx"
KVER="$(uname -r)"

SRC_DIR=$(find /usr/src -maxdepth 1 -type d -name 'system76-[0-9]*' | sort -V | tail -1)
if [ -z "$SRC_DIR" ]; then
	echo "error: no /usr/src/system76-* DKMS source tree found (is the 'system76' package installed?)" >&2
	exit 1
fi

PKG_VERSION="$(basename "$SRC_DIR" | sed 's/^system76-//')"
echo "Using DKMS source: $SRC_DIR (system76 $PKG_VERSION)"

if sudo grep -q "$MARKER" "$SRC_DIR/clevo-acpi.c"; then
	echo "Patch already applied, skipping patch step."
else
	echo "Applying patch..."
	TMP="$(mktemp -d)"
	sudo cp "$SRC_DIR/clevo-acpi.c" "$TMP/clevo-acpi.c"
	sudo chown "$(id -u):$(id -g)" "$TMP/clevo-acpi.c"
	patch -p1 -d "$TMP" < "$PATCH_FILE"
	sudo cp "$TMP/clevo-acpi.c" "$SRC_DIR/clevo-acpi.c"
	rm -rf "$TMP"
fi

echo "Rebuilding via DKMS for kernel $KVER..."
sudo dkms build -m system76 -v "$PKG_VERSION" -k "$KVER" --force
sudo dkms install -m system76 -v "$PKG_VERSION" -k "$KVER" --force

echo "Reloading clevo_acpi module..."
sudo rmmod clevo_acpi 2>/dev/null || true
sudo modprobe clevo_acpi

echo
echo "Driver enabled. Check: ls -la /sys/devices/platform/CLV0001:00/"
echo "To actually control it (permissions, GUI/CLI, boot persistence), install"
echo "the keyboardcolors app: https://github.com/arbitrary-string/keyboardcolors"
