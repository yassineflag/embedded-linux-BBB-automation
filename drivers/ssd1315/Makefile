# SPDX-License-Identifier: GPL-2.0
# Out-of-tree build of ssd1315_oled.ko
#
#   make                     build against ../../src/linux
#   make KDIR=/path/to/linux build against another configured tree
#   make install INSTALL_MOD_PATH=<rootfs>

ifneq ($(KERNELRELEASE),)
obj-m := ssd1315_oled.o
else
KDIR          ?= $(CURDIR)/../../src/linux
ARCH          ?= arm
CROSS_COMPILE ?= arm-linux-gnueabihf-
KMAKE = $(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)

.PHONY: all install clean
all:
	$(KMAKE) modules

install: all
	$(KMAKE) INSTALL_MOD_PATH=$(INSTALL_MOD_PATH) modules_install

clean:
	$(KMAKE) clean
endif
