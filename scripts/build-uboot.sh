#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026 Yassine El H. @yassineflag
# scripts/build-uboot.sh: fetch and cross build U-Boot (MLO + u-boot.img)
set -euo pipefail
STEP=U-BOOT
# shellcheck source=env.sh
. "$(dirname "$0")/env.sh"

need_cmd git "${CROSS_COMPILE}gcc" bison flex
mkdir -p "${SRC_DIR}" "${BOOT_OUT}"
git_fetch "${UBOOT_GIT}" "${UBOOT_TAG}" "${UBOOT_SRC}"

umake() { make -C "${UBOOT_SRC}" ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" "$@"; }

[ -f "${UBOOT_SRC}/.config" ] || umake am335x_evm_defconfig

log "Building (jobs=${JOBS})"
umake -j"${JOBS}"

cp "${UBOOT_SRC}/MLO" "${UBOOT_SRC}/u-boot.img" "${BOOT_OUT}/"
log "Done: MLO, u-boot.img -> ${BOOT_OUT}"
