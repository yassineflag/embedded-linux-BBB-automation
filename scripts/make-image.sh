#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026 Yassine El H. @yassineflag
# scripts/make-image.sh: assemble a bootable SD card image, without root
#
#   sector 0          MBR, disk id taken from boot/extlinux/extlinux.conf
#   4 MiB .. 68 MiB   p1 FAT32 "BOOT", bootable: MLO, u-boot.img, zImage,
#                     DTBs, extlinux/extlinux.conf
#   68 MiB .. end     p2 ext4 "rootfs"
#
# Each filesystem is built as a standalone file (mtools for FAT, mkfs.ext4 -d
# for ext4) and then written at its offset: no sudo, no loop devices, no
# mounts, so it also runs in CI and containers. mkfs.ext4 runs inside
# fakeroot so every file in the rootfs is owned by root on the target.
#
# The AM335x ROM loads MLO from the FAT partition flagged active. MLO is not
# written raw: FAT mode is what SPL then uses to find u-boot.img.
set -euo pipefail
STEP=IMAGE
# shellcheck source=env.sh
. "$(dirname "$0")/env.sh"

IMG_SIZE_MIB="${IMG_SIZE_MIB:-512}"
BOOT_START_MIB=4
BOOT_SIZE_MIB=64
ROOT_START_MIB=$((BOOT_START_MIB + BOOT_SIZE_MIB))
ROOT_SIZE_MIB=$((IMG_SIZE_MIB - ROOT_START_MIB))
EXTLINUX="${TOP}/boot/extlinux/extlinux.conf"

need_cmd sfdisk mkfs.vfat mcopy mmd mkfs.ext4 fakeroot

for f in MLO u-boot.img zImage "${BOARD_DTB}"; do
    [ -f "${BOOT_OUT}/${f}" ] || die "missing ${BOOT_OUT}/${f}, run 'make uboot kernel'"
done
[ -x "${ROOTFS_OUT}/bin/busybox" ] || die "rootfs missing, run 'make rootfs'"
[ "${ROOT_SIZE_MIB}" -gt 64 ] || die "IMG_SIZE_MIB=${IMG_SIZE_MIB} is too small"

# root=PARTUUID=<disk id>-02 in extlinux.conf is the single source of truth
DISK_ID=$(sed -n 's/.*root=PARTUUID=\([0-9a-fA-F]\{8\}\)-02.*/\1/p' "${EXTLINUX}")
[ -n "${DISK_ID}" ] || die "no root=PARTUUID=<8 hex digits>-02 in ${EXTLINUX}"

WORK=$(mktemp -d "${OUTPUT}/.image.XXXXXX")
trap 'rm -rf "${WORK}"' EXIT

# ---------------------------------------------------------------------------
# Boot partition (FAT32)
# ---------------------------------------------------------------------------
log "Building FAT32 boot partition (${BOOT_SIZE_MIB} MiB)"
BOOT_FS="${WORK}/boot.vfat"
mkfs.vfat -F 32 -n BOOT -C "${BOOT_FS}" $((BOOT_SIZE_MIB * 1024)) > /dev/null
export MTOOLS_SKIP_CHECK=1
mcopy -i "${BOOT_FS}" "${BOOT_OUT}/MLO" "${BOOT_OUT}/u-boot.img" "${BOOT_OUT}/zImage" "${BOOT_OUT}"/*.dtb ::
mmd   -i "${BOOT_FS}" ::extlinux
mcopy -i "${BOOT_FS}" "${EXTLINUX}" ::extlinux/extlinux.conf

# ---------------------------------------------------------------------------
# Root partition (ext4), owned by root:root
# ---------------------------------------------------------------------------
log "Building ext4 rootfs partition (${ROOT_SIZE_MIB} MiB)"
ROOT_FS="${WORK}/rootfs.ext4"
# shellcheck disable=SC2016 # expanded by the inner sh, not here
fakeroot -- sh -c '
    set -e
    cp -a "$1" "$2/tree"
    chown -R 0:0 "$2/tree"
    mkfs.ext4 -q -F -L rootfs -d "$2/tree" "$3" "$4M"
' sh "${ROOTFS_OUT}" "${WORK}" "${ROOT_FS}" "${ROOT_SIZE_MIB}"

# ---------------------------------------------------------------------------
# Disk image: MBR + both partitions
# ---------------------------------------------------------------------------
log "Writing ${IMAGE} (${IMG_SIZE_MIB} MiB, disk id 0x${DISK_ID})"
rm -f "${IMAGE}"
truncate -s "${IMG_SIZE_MIB}M" "${IMAGE}"
sfdisk --quiet "${IMAGE}" <<EOF
label: dos
label-id: 0x${DISK_ID}
unit: sectors

start=$((BOOT_START_MIB * 2048)), size=$((BOOT_SIZE_MIB * 2048)), type=c, bootable
start=$((ROOT_START_MIB * 2048)), size=$((ROOT_SIZE_MIB * 2048)), type=83
EOF
dd if="${BOOT_FS}" of="${IMAGE}" bs=1M seek="${BOOT_START_MIB}" conv=notrunc,sparse status=none
dd if="${ROOT_FS}" of="${IMAGE}" bs=1M seek="${ROOT_START_MIB}" conv=notrunc,sparse status=none

log "SD card image ready: ${IMAGE}"
log "Flash it (check the device with lsblk first):"
log "  sudo dd if=${IMAGE} of=/dev/sdX bs=4M conv=fsync status=progress"
