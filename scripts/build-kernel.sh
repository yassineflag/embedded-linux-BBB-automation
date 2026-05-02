#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026 Yassine El H. @yassineflag
# scripts/build-kernel.sh: Clone and cross-compile Linux kernel for BeagleBone Black
set -euo pipefail

TOP="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${TOP}/src/linux"
OUTPUT="${TOP}/output/boot"
MODULES_OUT="${TOP}/output/modules"
CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabihf-}"
KERNEL_TAG="${KERNEL_TAG:-v6.6.21}"

mkdir -p "${OUTPUT}" "${MODULES_OUT}"

# Clone if not already present
if [ ! -d "${SRC}" ]; then
    echo "[KERNEL] Cloning linux-stable ${KERNEL_TAG}..."
    git clone --depth=1 --branch "${KERNEL_TAG}" \
        https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git "${SRC}"
else
    echo "[KERNEL] Source already present: ${SRC}"
fi

echo "[KERNEL] Applying omap2plus_defconfig..."
make -C "${SRC}" ARCH=arm CROSS_COMPILE="${CROSS_COMPILE}" omap2plus_defconfig

echo "[KERNEL] Merging BBB OLED config fragment..."
"${SRC}/scripts/kconfig/merge_config.sh" -m \
    "${SRC}/.config" \
    "${TOP}/kernel/configs/bbb-oled.config"
make -C "${SRC}" ARCH=arm CROSS_COMPILE="${CROSS_COMPILE}" olddefconfig

echo "[KERNEL] Building zImage + DTBs + modules (jobs=$(nproc))..."
make -C "${SRC}" ARCH=arm CROSS_COMPILE="${CROSS_COMPILE}" \
    -j"$(nproc)" zImage dtbs modules

echo "[KERNEL] Copying artifacts to ${OUTPUT}..."
cp "${SRC}/arch/arm/boot/zImage" "${OUTPUT}/"
cp "${SRC}/arch/arm/boot/dts/ti/omap/am335x-boneblack.dtb" "${OUTPUT}/"

echo "[KERNEL] Installing modules to ${MODULES_OUT}..."
make -C "${SRC}" ARCH=arm CROSS_COMPILE="${CROSS_COMPILE}" \
    INSTALL_MOD_PATH="${MODULES_OUT}" modules_install

echo "[KERNEL] Done — zImage + am335x-boneblack.dtb → ${OUTPUT}"
