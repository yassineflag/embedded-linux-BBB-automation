# Out-of-tree Makefile for ssd1315_oled kernel module
obj-m := ssd1315_oled.o

KDIR ?= ../../../src/linux

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(shell pwd) ARCH=arm \
	    CROSS_COMPILE=arm-linux-gnueabihf- modules

clean:
	$(MAKE) -C $(KDIR) M=$(shell pwd) clean
