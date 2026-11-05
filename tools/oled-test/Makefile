# SPDX-License-Identifier: GPL-2.0
# oled-test: userspace test app, cross compiled and statically linked
CROSS_COMPILE ?= arm-linux-gnueabihf-
CC            := $(CROSS_COMPILE)gcc
CFLAGS        ?= -O2
CFLAGS        += -Wall -Wextra -Werror -std=gnu11
CPPFLAGS      += -I$(CURDIR)/../../drivers/ssd1315
LDFLAGS       += -static
TARGET        := oled-test

.PHONY: all clean
all: $(TARGET)

$(TARGET): oled_test.c ../../drivers/ssd1315/ssd1315_ioctl.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)
