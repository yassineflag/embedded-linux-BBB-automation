# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026 Yassine El H. @yassineflag
#
# BeagleBone Black embedded Linux build: U-Boot, kernel, BusyBox rootfs,
# SSD1315 driver, oled-test, SD card image.
#
# Every target is a thin wrapper around scripts/*.sh, which hold the actual
# logic. Versions and paths live in scripts/env.sh. Override them on the
# command line, for example:
#   make KERNEL_TAG=v6.6.60 JOBS=8 all

SCRIPTS := $(CURDIR)/scripts
CROSS_COMPILE ?= arm-linux-gnueabihf-

# Forwarded to the scripts. Unset values fall back to scripts/env.sh defaults
export CROSS_COMPILE JOBS UBOOT_TAG KERNEL_TAG BUSYBOX_VERSION BUSYBOX_SHA256 IMG_SIZE_MIB

# The scripts already build with -j$(JOBS). Running two of them at once only
# interleaves the logs, and modules/oled-test both write into output/rootfs
.NOTPARALLEL:

.PHONY: all toolchain uboot kernel rootfs modules oled-test image clean distclean help

all: image
	@echo
	@echo "  Build complete: output/sdcard.img"
	@echo "  Flash: sudo dd if=output/sdcard.img of=/dev/sdX bs=4M conv=fsync status=progress"

toolchain:
	@command -v $(CROSS_COMPILE)gcc > /dev/null || { \
	  echo "[TOOLCHAIN] $(CROSS_COMPILE)gcc not found."; \
	  echo "  sudo apt-get install gcc-arm-linux-gnueabihf binutils-arm-linux-gnueabihf"; \
	  exit 1; }
	@echo "[TOOLCHAIN] $$($(CROSS_COMPILE)gcc --version | head -1)"

uboot: toolchain
	@bash $(SCRIPTS)/build-uboot.sh

kernel: toolchain
	@bash $(SCRIPTS)/build-kernel.sh

# rootfs is rebuilt from scratch and pulls in the in tree modules
rootfs: kernel
	@bash $(SCRIPTS)/build-rootfs.sh

# out of tree ssd1315_oled.ko, installed into the rootfs
modules: rootfs
	@bash $(SCRIPTS)/build-modules.sh

oled-test: rootfs
	@bash $(SCRIPTS)/build-oled-test.sh

image: uboot modules oled-test
	@bash $(SCRIPTS)/make-image.sh

clean:
	rm -rf output
	-[ -f src/linux/Makefile ] && $(MAKE) -C drivers/ssd1315 KDIR=$(CURDIR)/src/linux clean
	$(MAKE) -C tools/oled-test clean

# Also drops downloaded sources (U-Boot, kernel, BusyBox)
distclean: clean
	rm -rf src

help:
	@echo "Targets:"
	@echo "  all        everything, ends with output/sdcard.img (default)"
	@echo "  toolchain  check the cross compiler"
	@echo "  uboot      MLO + u-boot.img             -> output/boot/"
	@echo "  kernel     zImage + DTBs + modules      -> output/boot/, output/modules/"
	@echo "  rootfs     BusyBox rootfs from scratch  -> output/rootfs/"
	@echo "  modules    ssd1315_oled.ko              -> output/rootfs/lib/modules/"
	@echo "  oled-test  test app                     -> output/rootfs/usr/bin/"
	@echo "  image      SD card image, no root       -> output/sdcard.img"
	@echo "  clean      remove output/ and local build artifacts"
	@echo "  distclean  clean + remove src/"
	@echo
	@echo "Variables: CROSS_COMPILE JOBS UBOOT_TAG KERNEL_TAG BUSYBOX_VERSION IMG_SIZE_MIB"
