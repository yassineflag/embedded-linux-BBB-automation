#!/usr/bin/env bash
# scripts/build-oled-test.sh — Cross-compile userspace oled-test app and install to rootfs
set -euo pipefail

TOP="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${TOP}/tools/oled-test"
ROOTFS_OUT="${TOP}/output/rootfs"
CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabihf-}"

echo "[OLED-TEST] Cross-compiling oled-test..."
make -C "${SRC}" CROSS_COMPILE="${CROSS_COMPILE}"

echo "[OLED-TEST] Installing to ${ROOTFS_OUT}/usr/bin/..."
mkdir -p "${ROOTFS_OUT}/usr/bin"
install -m 755 "${SRC}/oled-test" "${ROOTFS_OUT}/usr/bin/oled-test"

echo "[OLED-TEST] Done — oled-test → ${ROOTFS_OUT}/usr/bin/oled-test"
