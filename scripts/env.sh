# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026 Yassine El H. @yassineflag
# shellcheck shell=bash
#
# scripts/env.sh: single source of truth for versions, paths and toolchain.
# Sourced by every build script. Any variable can be overridden from the
# environment or from make, for example: make KERNEL_TAG=v6.6.60 kernel

TOP="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

ARCH=arm
CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabihf-}"
JOBS="${JOBS:-$(nproc)}"

UBOOT_TAG="${UBOOT_TAG:-v2024.01}"
KERNEL_TAG="${KERNEL_TAG:-v6.6.21}"
BUSYBOX_VERSION="${BUSYBOX_VERSION:-1.36.1}"
BUSYBOX_SHA256="${BUSYBOX_SHA256:-b8cc24c9574d809e7279c3be349795c5d5ceb6fdf19ca709f80cde50e47de314}"

UBOOT_GIT="${UBOOT_GIT:-https://github.com/u-boot/u-boot.git}"
KERNEL_GIT="${KERNEL_GIT:-https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git}"

SRC_DIR="${TOP}/src"
UBOOT_SRC="${SRC_DIR}/u-boot"
KERNEL_SRC="${SRC_DIR}/linux"
BUSYBOX_SRC="${SRC_DIR}/busybox"

OUTPUT="${TOP}/output"
BOOT_OUT="${OUTPUT}/boot"
ROOTFS_OUT="${OUTPUT}/rootfs"
MODULES_OUT="${OUTPUT}/modules"
IMAGE="${OUTPUT}/sdcard.img"

# Board device tree built from kernel/dts/ on top of am335x-boneblack.dts
BOARD_DTS=am335x-boneblack-oled
BOARD_DTB="${BOARD_DTS}.dtb"

export ARCH CROSS_COMPILE

log() { printf '[%s] %s\n' "${STEP:-BUILD}" "$*"; }
die() { printf '[%s] ERROR: %s\n' "${STEP:-BUILD}" "$*" >&2; exit 1; }

need_cmd() {
    local c
    for c in "$@"; do
        command -v "$c" >/dev/null 2>&1 || die "'$c' not found, see Prerequisites in README.md"
    done
}

# git_fetch <url> <tag> <dir>: shallow clone once, then refuse a tag mismatch
git_fetch() {
    local url=$1 tag=$2 dir=$3 have
    if [ ! -d "${dir}/.git" ]; then
        log "Cloning ${url} at ${tag}"
        git clone --depth=1 --branch "${tag}" "${url}" "${dir}"
        return
    fi
    have=$(git -C "${dir}" describe --tags --exact-match 2>/dev/null || echo unknown)
    [ "${have}" = "${tag}" ] || \
        die "${dir} is at ${have}, expected ${tag}. Remove it to re fetch."
    log "Source present: ${dir} (${tag})"
}
