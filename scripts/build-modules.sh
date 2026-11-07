#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026 Yassine El H. @yassineflag
# scripts/build-modules.sh: build ssd1315_oled.ko and install it into the rootfs
set -euo pipefail
STEP=MODULES
# shellcheck source=env.sh
. "$(dirname "$0")/env.sh"

[ -f "${KERNEL_SRC}/Module.symvers" ] || die "kernel not built, run 'make kernel' first"
[ -x "${ROOTFS_OUT}/bin/busybox" ] || die "rootfs missing, run 'make rootfs' first"
need_cmd depmod

DRV="${TOP}/drivers/ssd1315"

log "Building ssd1315_oled.ko"
make -C "${DRV}" KDIR="${KERNEL_SRC}"

# Kbuild installs to lib/modules/<release>/updates and runs depmod there
log "Installing into ${ROOTFS_OUT}"
make -C "${DRV}" KDIR="${KERNEL_SRC}" install \
    INSTALL_MOD_PATH="${ROOTFS_OUT}" INSTALL_MOD_STRIP=1

KVER=$(cat "${KERNEL_SRC}/include/config/kernel.release")
find "${ROOTFS_OUT}/lib/modules/${KVER}" -name ssd1315_oled.ko | grep -q . || \
    die "ssd1315_oled.ko not found under ${ROOTFS_OUT}/lib/modules/${KVER}"
log "Done: ssd1315_oled.ko -> ${ROOTFS_OUT}/lib/modules/${KVER}"
