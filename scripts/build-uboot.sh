#!/usr/bin/env bash
# scripts/build-uboot.sh — Clone and cross-compile U-Boot for BeagleBone Black
set -euo pipefail

TOP="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${TOP}/src/u-boot"
OUTPUT="${TOP}/output/boot"
CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabihf-}"
UBOOT_TAG="${UBOOT_TAG:-v2024.01}"

mkdir -p "${OUTPUT}"

# Clone if not already present
if [ ! -d "${SRC}" ]; then
    echo "[U-BOOT] Cloning U-Boot ${UBOOT_TAG}..."
    git clone --depth=1 --branch "${UBOOT_TAG}" \
        https://github.com/u-boot/u-boot.git "${SRC}"
else
    echo "[U-BOOT] Source already present: ${SRC}"
fi

echo "[U-BOOT] Configuring am335x_evm_defconfig..."
make -C "${SRC}" ARCH=arm CROSS_COMPILE="${CROSS_COMPILE}" am335x_evm_defconfig

echo "[U-BOOT] Building (jobs=$(nproc))..."
make -C "${SRC}" ARCH=arm CROSS_COMPILE="${CROSS_COMPILE}" -j"$(nproc)"

echo "[U-BOOT] Copying artifacts to ${OUTPUT}..."
cp "${SRC}/MLO"        "${OUTPUT}/"
cp "${SRC}/u-boot.img" "${OUTPUT}/"
cp "${TOP}/boot/uEnv.txt" "${OUTPUT}/"

echo "[U-BOOT] Done — MLO + u-boot.img → ${OUTPUT}"
