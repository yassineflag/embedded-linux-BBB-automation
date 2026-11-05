/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (C) 2026 Yassine El H. @yassineflag
 *
 * Userspace ABI of the ssd1315_oled driver (/dev/ssd1315).
 * Shared by the kernel module and by tools/oled-test.
 *
 * write(): exactly SSD1315_FB_SIZE bytes, SSD1306 page layout
 *          (8 pages of 128 columns, one byte = 8 vertical pixels, LSB on top).
 */
#ifndef _UAPI_SSD1315_IOCTL_H
#define _UAPI_SSD1315_IOCTL_H

#include <linux/ioctl.h>

#define SSD1315_WIDTH		128
#define SSD1315_HEIGHT		64
#define SSD1315_FB_SIZE		(SSD1315_WIDTH * SSD1315_HEIGHT / 8)

#define SSD1315_IOCTL_MAGIC	'S'
#define SSD1315_IOCTL_CLEAR	_IO(SSD1315_IOCTL_MAGIC, 0)
#define SSD1315_IOCTL_ON	_IO(SSD1315_IOCTL_MAGIC, 1)
#define SSD1315_IOCTL_OFF	_IO(SSD1315_IOCTL_MAGIC, 2)

#endif /* _UAPI_SSD1315_IOCTL_H */
