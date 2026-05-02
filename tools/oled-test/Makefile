# Makefile for oled-test userspace app (cross-compile for ARM)
CROSS_COMPILE ?= arm-linux-gnueabihf-
CC            := $(CROSS_COMPILE)gcc
CFLAGS        := -Wall -Wextra -O2 -static
TARGET        := oled-test

.PHONY: all clean

all: $(TARGET)

$(TARGET): oled_test.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)
