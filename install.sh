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
