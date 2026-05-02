#!/usr/bin/env bash
# scripts/make-image.sh — Assemble bootable SD card image
#
# Creates sdcard.img with:
#   Partition 1: FAT32 64MB (boot: MLO, u-boot.img, uEnv.txt, zImage, DTB)
#   Partition 2: ext4 remainder (rootfs)
set -euo pipefail

TOP="$(cd "$(dirname "$0")/.." && pwd)"
OUTPUT="${TOP}/output"
IMAGE="${OUTPUT}/sdcard.img"
BOOT_OUT="${OUTPUT}/boot"
ROOTFS_OUT="${OUTPUT}/rootfs"

# Image size in MiB (adjust as needed)
IMG_SIZE=512

echo "[IMAGE] Creating ${IMG_SIZE}MiB image: ${IMAGE}"
dd if=/dev/zero of="${IMAGE}" bs=1M count="${IMG_SIZE}" status=progress

echo "[IMAGE] Partitioning..."
parted -s "${IMAGE}" \
    mklabel msdos \
    mkpart primary fat32   4MiB   68MiB \
    mkpart primary ext4   68MiB  100% \
    set 1 boot on

# Mount image partitions via loop device
LOOPDEV=$(sudo losetup --find --partscan --show "${IMAGE}")
echo "[IMAGE] Loop device: ${LOOPDEV}"

# Format
echo "[IMAGE] Formatting partitions..."
sudo mkfs.vfat -F 32 -n "BOOT" "${LOOPDEV}p1"
sudo mkfs.ext4 -L "rootfs"     "${LOOPDEV}p2"

BOOT_MNT=$(mktemp -d)
ROOT_MNT=$(mktemp -d)

sudo mount "${LOOPDEV}p1" "${BOOT_MNT}"
sudo mount "${LOOPDEV}p2" "${ROOT_MNT}"

echo "[IMAGE] Copying boot files..."
# U-Boot requires MLO to be written raw to sector 256 (per AM335x TRM)
# Also copy to FAT for reference
sudo cp "${BOOT_OUT}/MLO"      "${BOOT_MNT}/"
sudo cp "${BOOT_OUT}/u-boot.img" "${BOOT_MNT}/"
sudo cp "${BOOT_OUT}/uEnv.txt"   "${BOOT_MNT}/"
sudo cp "${BOOT_OUT}/zImage"     "${BOOT_MNT}/"
sudo cp "${BOOT_OUT}/am335x-boneblack.dtb" "${BOOT_MNT}/"

# Also write MLO to the raw location (sector 256, 512-byte sectors)
sudo dd if="${BOOT_OUT}/MLO" of="${LOOPDEV}" seek=256 bs=512 conv=notrunc 2>/dev/null

echo "[IMAGE] Copying rootfs..."
sudo cp -a "${ROOTFS_OUT}/." "${ROOT_MNT}/"

echo "[IMAGE] Unmounting..."
sudo umount "${BOOT_MNT}" "${ROOT_MNT}"
sudo losetup -d "${LOOPDEV}"
rmdir "${BOOT_MNT}" "${ROOT_MNT}"

echo ""
echo "[IMAGE] SD card image ready: ${IMAGE}"
echo "  Flash to microSD (replace /dev/sdX with your card):"
echo "    sudo dd if=${IMAGE} of=/dev/sdX bs=4M status=progress && sync"
