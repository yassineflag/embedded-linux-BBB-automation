#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026 Yassine El H. @yassineflag
# scripts/build-kernel.sh: fetch, configure and cross build Linux for the BBB
set -euo pipefail
STEP=KERNEL
# shellcheck source=env.sh
. "$(dirname "$0")/env.sh"

FRAGMENT="${TOP}/kernel/configs/bbb-oled.config"
DTS_DIR="${KERNEL_SRC}/arch/arm/boot/dts/ti/omap"
STAMP="${KERNEL_SRC}/.bbb-config.stamp"

need_cmd git "${CROSS_COMPILE}gcc" bc bison flex
mkdir -p "${SRC_DIR}"
git_fetch "${KERNEL_GIT}" "${KERNEL_TAG}" "${KERNEL_SRC}"

# LOCALVERSION set but empty: no "+" suffix for a tree that is not a clean tag
kmake() { make -C "${KERNEL_SRC}" ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" LOCALVERSION= "$@"; }

# Configure only when the fragment changed, so incremental builds stay fast
want=$(sha256sum < "${FRAGMENT}" | cut -d" " -f1)
if [ ! -f "${KERNEL_SRC}/.config" ] || [ "$(cat "${STAMP}" 2>/dev/null)" != "${want}" ]; then
    log "omap2plus_defconfig + $(basename "${FRAGMENT}")"
    kmake omap2plus_defconfig
    # merge_config.sh writes ./.config, so it must run inside the kernel tree
    (cd "${KERNEL_SRC}" && ./scripts/kconfig/merge_config.sh -m .config "${FRAGMENT}")
    kmake olddefconfig

    # Kconfig silently drops symbols with unmet dependencies: fail instead
    missing=0
    while IFS= read -r line; do
        grep -qxF "${line}" "${KERNEL_SRC}/.config" || { echo "  not applied: ${line}" >&2; missing=1; }
    done < <(grep -E '^CONFIG_[A-Z0-9_]+=' "${FRAGMENT}")
    while IFS= read -r sym; do
        if grep -q "^${sym}=" "${KERNEL_SRC}/.config"; then
            echo "  still enabled: ${sym}" >&2
            missing=1
        fi
    done < <(sed -n 's/^# \(CONFIG_[A-Z0-9_]*\) is not set$/\1/p' "${FRAGMENT}")
    [ "${missing}" -eq 0 ] || die "config fragment not fully applied"
    echo "${want}" > "${STAMP}"
else
    log "Config up to date"
fi

log "Installing ${BOARD_DTS}.dts into the kernel tree"
cp "${TOP}/kernel/dts/${BOARD_DTS}.dts" "${DTS_DIR}/"
grep -q "${BOARD_DTB}" "${DTS_DIR}/Makefile" || \
    echo "dtb-\$(CONFIG_SOC_AM33XX) += ${BOARD_DTB}" >> "${DTS_DIR}/Makefile"

log "Building zImage, DTBs and modules (jobs=${JOBS})"
kmake -j"${JOBS}" zImage dtbs modules

mkdir -p "${BOOT_OUT}"
cp "${KERNEL_SRC}/arch/arm/boot/zImage" "${BOOT_OUT}/"
cp "${DTS_DIR}/${BOARD_DTB}" "${DTS_DIR}/am335x-boneblack.dtb" "${BOOT_OUT}/"

log "Installing in tree modules to ${MODULES_OUT}"
rm -rf "${MODULES_OUT}"
kmake INSTALL_MOD_PATH="${MODULES_OUT}" INSTALL_MOD_STRIP=1 modules_install

log "Done: zImage, ${BOARD_DTB} -> ${BOOT_OUT}"
