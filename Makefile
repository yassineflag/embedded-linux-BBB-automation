# =============================================================================
# BeagleBone Black — Embedded Linux Build System
# Cross-toolchain | U-Boot | Kernel | RootFS | SSD1315 Driver | MPU6050 | SD Image
# =============================================================================

CROSS_COMPILE   ?= arm-linux-gnueabihf-
ARCH            := arm
JOBS            := $(shell nproc)

# Versions
UBOOT_TAG       := v2024.01
KERNEL_TAG      := v6.6.21
BUSYBOX_VERSION := 1.36.1

# Directories
TOP             := $(CURDIR)
SRC             := $(TOP)/src
OUTPUT          := $(TOP)/output
BOOT_OUT        := $(OUTPUT)/boot
ROOTFS_OUT      := $(OUTPUT)/rootfs
MODULES_OUT     := $(OUTPUT)/modules

UBOOT_SRC       := $(SRC)/u-boot
KERNEL_SRC      := $(SRC)/linux
BUSYBOX_SRC     := $(SRC)/busybox
DRIVER_SRC      := $(TOP)/drivers/ssd1315
OLED_TEST_SRC   := $(TOP)/tools/oled-test

export CROSS_COMPILE ARCH

.PHONY: all toolchain uboot kernel modules rootfs oled-test image clean help

all: toolchain uboot kernel modules rootfs oled-test image
	@echo ""
	@echo "================================================================"
	@echo "  Build complete! SD card image: $(OUTPUT)/sdcard.img"
	@echo "  Flash with: sudo dd if=$(OUTPUT)/sdcard.img of=/dev/sdX bs=4M status=progress"
	@echo "================================================================"

# -----------------------------------------------------------------------------
# 0. Toolchain check
# -----------------------------------------------------------------------------
toolchain:
	@echo "[TOOLCHAIN] Checking cross-compiler: $(CROSS_COMPILE)gcc"
	@which $(CROSS_COMPILE)gcc > /dev/null 2>&1 || \
	  (echo "  Installing arm-linux-gnueabihf toolchain..." && \
	   sudo apt-get install -y gcc-arm-linux-gnueabihf binutils-arm-linux-gnueabihf)
	@echo "[TOOLCHAIN] OK: $$($(CROSS_COMPILE)gcc --version | head -1)"

# -----------------------------------------------------------------------------
# 1. U-Boot
# -----------------------------------------------------------------------------
uboot: $(UBOOT_SRC)
	@echo "[U-BOOT] Configuring am335x_evm_defconfig..."
	$(MAKE) -C $(UBOOT_SRC) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) am335x_evm_defconfig
	@echo "[U-BOOT] Building..."
	$(MAKE) -C $(UBOOT_SRC) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -j$(JOBS)
	@mkdir -p $(BOOT_OUT)
	cp $(UBOOT_SRC)/MLO $(UBOOT_SRC)/u-boot.img $(BOOT_OUT)/
	cp $(TOP)/boot/uEnv.txt $(BOOT_OUT)/
	@echo "[U-BOOT] Done — MLO + u-boot.img → $(BOOT_OUT)"

$(UBOOT_SRC):
	@echo "[U-BOOT] Cloning U-Boot $(UBOOT_TAG)..."
	git clone --depth=1 --branch $(UBOOT_TAG) \
	    https://github.com/u-boot/u-boot.git $(UBOOT_SRC)

# -----------------------------------------------------------------------------
# 2. Linux Kernel
# -----------------------------------------------------------------------------
kernel: $(KERNEL_SRC)
	@echo "[KERNEL] Applying omap2plus_defconfig..."
	$(MAKE) -C $(KERNEL_SRC) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) omap2plus_defconfig
	@echo "[KERNEL] Merging BBB OLED config fragment..."
	$(KERNEL_SRC)/scripts/kconfig/merge_config.sh -m \
	    $(KERNEL_SRC)/.config \
	    $(TOP)/kernel/configs/bbb-oled.config
	$(MAKE) -C $(KERNEL_SRC) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) olddefconfig
	@echo "[KERNEL] Building zImage + DTBs..."
	$(MAKE) -C $(KERNEL_SRC) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) \
	    -j$(JOBS) zImage dtbs modules
	@mkdir -p $(BOOT_OUT) $(MODULES_OUT)
	cp $(KERNEL_SRC)/arch/arm/boot/zImage $(BOOT_OUT)/
	cp $(KERNEL_SRC)/arch/arm/boot/dts/ti/omap/am335x-boneblack.dtb $(BOOT_OUT)/
	$(MAKE) -C $(KERNEL_SRC) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) \
	    INSTALL_MOD_PATH=$(MODULES_OUT) modules_install
	@echo "[KERNEL] Done — zImage + DTB → $(BOOT_OUT)"

$(KERNEL_SRC):
	@echo "[KERNEL] Cloning linux-stable $(KERNEL_TAG)..."
	git clone --depth=1 --branch $(KERNEL_TAG) \
	    https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git $(KERNEL_SRC)

# -----------------------------------------------------------------------------
# 3. SSD1315 Out-of-Tree Kernel Module
# -----------------------------------------------------------------------------
modules: kernel
	@echo "[MODULES] Building ssd1315_oled.ko..."
	$(MAKE) -C $(KERNEL_SRC) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) \
	    M=$(DRIVER_SRC) modules
	@echo "[MODULES] Done — $(DRIVER_SRC)/ssd1315_oled.ko"

# -----------------------------------------------------------------------------
# 4. BusyBox RootFS
# -----------------------------------------------------------------------------
rootfs: $(BUSYBOX_SRC)
	@bash $(TOP)/scripts/build-rootfs.sh
	@echo "[ROOTFS] Done — $(ROOTFS_OUT)"

$(BUSYBOX_SRC):
	@echo "[BUSYBOX] Downloading BusyBox $(BUSYBOX_VERSION)..."
	@mkdir -p $(SRC)
	wget -q -O /tmp/busybox.tar.bz2 \
	    https://busybox.net/downloads/busybox-$(BUSYBOX_VERSION).tar.bz2
	tar -xf /tmp/busybox.tar.bz2 -C $(SRC)
	mv $(SRC)/busybox-$(BUSYBOX_VERSION) $(BUSYBOX_SRC)

# -----------------------------------------------------------------------------
# 5. Userspace OLED test app
# -----------------------------------------------------------------------------
oled-test: rootfs
	@echo "[OLED-TEST] Cross-compiling oled-test..."
	$(MAKE) -C $(OLED_TEST_SRC) CROSS_COMPILE=$(CROSS_COMPILE)
	install -m 755 $(OLED_TEST_SRC)/oled-test $(ROOTFS_OUT)/usr/bin/oled-test
	@echo "[OLED-TEST] Done"

# -----------------------------------------------------------------------------
# 6. SD Card Image
# -----------------------------------------------------------------------------
image: uboot kernel modules rootfs oled-test
	@bash $(TOP)/scripts/make-image.sh
	@echo "[IMAGE] Done — $(OUTPUT)/sdcard.img"

# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------
clean:
	@echo "[CLEAN] Removing output/..."
	rm -rf $(OUTPUT)
	$(MAKE) -C $(DRIVER_SRC) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) \
	    M=$(DRIVER_SRC) clean 2>/dev/null || true
	$(MAKE) -C $(OLED_TEST_SRC) clean 2>/dev/null || true

help:
	@echo "BeagleBone Black Embedded Linux Build"
	@echo ""
	@echo "Targets:"
	@echo "  all        — Build everything (default)"
	@echo "  toolchain  — Check/install cross-compiler"
	@echo "  uboot      — Build U-Boot (MLO + u-boot.img)"
	@echo "  kernel     — Build Linux kernel (zImage + DTB)"
	@echo "  modules    — Build SSD1315 out-of-tree module"
	@echo "  rootfs     — Build BusyBox rootfs"
	@echo "  oled-test  — Build userspace OLED test app"
	@echo "  image      — Create SD card image"
	@echo "  clean      — Remove build artifacts"
	@echo ""
	@echo "Variables:"
	@echo "  CROSS_COMPILE=$(CROSS_COMPILE)"
	@echo "  JOBS=$(JOBS)"
