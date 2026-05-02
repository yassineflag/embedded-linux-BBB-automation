#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026 Yassine El H. @yassineflag
# scripts/build-modules.sh: Build and install ssd1315_oled out-of-tree kernel module
set -euo pipefail

TOP="$(cd "$(dirname "$0")/.." && pwd)"
KERNEL_SRC="${TOP}/src/linux"
DRIVER_SRC="${TOP}/drivers/ssd1315"
ROOTFS_OUT="${TOP}/output/rootfs"
CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabihf-}"

if [ ! -d "${KERNEL_SRC}" ]; then
    echo "[MODULES] ERROR: kernel source not found at ${KERNEL_SRC}"
    echo "  Run scripts/build-kernel.sh first."
    exit 1
fi

echo "[MODULES] Building ssd1315_oled.ko..."
make -C "${KERNEL_SRC}" ARCH=arm CROSS_COMPILE="${CROSS_COMPILE}" \
    M="${DRIVER_SRC}" modules

echo "[MODULES] Installing ssd1315_oled.ko into rootfs..."
# Get kernel version from the built tree
KVER=$(cat "${KERNEL_SRC}/include/config/kernel.release" 2>/dev/null \
       || make -C "${KERNEL_SRC}" -s kernelrelease ARCH=arm)

MODULES_DIR="${ROOTFS_OUT}/lib/modules/${KVER}/extra"
mkdir -p "${MODULES_DIR}"
cp "${DRIVER_SRC}/ssd1315_oled.ko" "${MODULES_DIR}/"

# Rebuild module dependency map
if command -v depmod >/dev/null 2>&1; then
    depmod -b "${ROOTFS_OUT}" "${KVER}" 2>/dev/null || true
fi

echo "[MODULES] Done — ssd1315_oled.ko → ${MODULES_DIR}"
