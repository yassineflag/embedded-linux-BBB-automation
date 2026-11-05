// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yassine El H. @yassineflag

/*
 * oled_test.c: userspace test app for SSD1315 OLED + MPU6050 + 4 buttons
 *
 * Usage: oled-test [mode]
 *   0  checkerboard (default)
 *   1  all pixels on
 *   2  clear (ioctl SSD1315_IOCTL_CLEAR)
 *   3  stream MPU6050 accel/gyro to stdout, X accel bar on the OLED
 *   4  print button presses, highlight the pressed button on the OLED
 *
 * Devices:
 *   /dev/ssd1315             ssd1315_oled.ko (out of tree)
 *   /sys/bus/iio/devices/iio:deviceN  inv_mpu6050_i2c (in tree, IIO)
 *   /dev/input/eventN                 gpio-keys (in tree, evdev)
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <linux/input.h>

#include "ssd1315_ioctl.h"

#define OLED_DEVICE		"/dev/ssd1315"
#define OLED_PAGES		(SSD1315_HEIGHT / 8)

#define IIO_DEVICES_PATH	"/sys/bus/iio/devices"
#define MPU_NAME		"mpu6050"
#define INPUT_DEVICES_PATH	"/dev/input"
#define NUM_BUTTONS		4

#define BITS_PER_LONG		(sizeof(long) * CHAR_BIT)
#define NLONGS(x)		(((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define TEST_BIT(b, a)		(((a)[(b) / BITS_PER_LONG] >> ((b) % BITS_PER_LONG)) & 1UL)

static volatile sig_atomic_t running = 1;

static void on_signal(int sig)
{
	(void)sig;
	running = 0;
}

/* No SA_RESTART: blocking read() must return EINTR on Ctrl+C */
static void install_signals(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
}

static int oled_show(int fd, const uint8_t *fb)
{
	ssize_t n = write(fd, fb, SSD1315_FB_SIZE);

	if (n == SSD1315_FB_SIZE)
		return 0;
	if (n < 0)
		perror("[oled-test] write " OLED_DEVICE);
	else
		fprintf(stderr, "[oled-test] short write: %zd bytes\n", n);
	return -1;
}

/* ------------------------------------------------------------------------
 * Patterns. Layout: fb[page * 128 + col], bit 0 is the top row of the page
 * ------------------------------------------------------------------------ */
static void pattern_checkerboard(uint8_t *fb)
{
	for (int page = 0; page < OLED_PAGES; page++)
		for (int col = 0; col < SSD1315_WIDTH; col++) {
			uint8_t v = (col & 1) ? 0x55 : 0xaa;

			fb[page * SSD1315_WIDTH + col] = (page & 1) ? (uint8_t)~v : v;
		}
}

/* Horizontal bar over pages 2..4, -1 g .. +1 g mapped to 0..128 columns */
static void pattern_accel_bar(uint8_t *fb, long accel_x, long one_g)
{
	long width;

	memset(fb, 0, SSD1315_FB_SIZE);
	if (accel_x < -one_g)
		accel_x = -one_g;
	if (accel_x > one_g)
		accel_x = one_g;
	width = (accel_x + one_g) * SSD1315_WIDTH / (2 * one_g);

	for (int page = 2; page <= 4; page++)
		memset(fb + page * SSD1315_WIDTH, 0xff, (size_t)width);
}

static void pattern_button(uint8_t *fb, int idx)
{
	const int seg = SSD1315_WIDTH / NUM_BUTTONS;

	memset(fb, 0, SSD1315_FB_SIZE);
	for (int page = 0; page < OLED_PAGES; page++)
		memset(fb + page * SSD1315_WIDTH + idx * seg, 0xff, seg);
}

/* ------------------------------------------------------------------------
 * MPU6050 over IIO sysfs
 * ------------------------------------------------------------------------ */
static char iio_path[PATH_MAX];

static int find_mpu6050(void)
{
	struct dirent *ent;
	char path[PATH_MAX], name[64];
	DIR *d = opendir(IIO_DEVICES_PATH);

	if (!d) {
		perror("[oled-test] opendir " IIO_DEVICES_PATH);
		return -1;
	}

	while ((ent = readdir(d))) {
		FILE *f;
		int match;

		if (strncmp(ent->d_name, "iio:device", 10))
			continue;
		if (snprintf(path, sizeof(path), "%s/%s/name", IIO_DEVICES_PATH,
			     ent->d_name) >= (int)sizeof(path))
			continue;
		f = fopen(path, "r");
		if (!f)
			continue;
		match = fgets(name, sizeof(name), f) && strstr(name, MPU_NAME);
		fclose(f);
		if (match) {
			closedir(d);
			if (snprintf(iio_path, sizeof(iio_path), "%s/%s",
				     IIO_DEVICES_PATH, ent->d_name) >= (int)sizeof(iio_path))
				return -1;
			printf("[oled-test] MPU6050 at %s\n", iio_path);
			return 0;
		}
	}

	closedir(d);
	fprintf(stderr, "[oled-test] MPU6050 IIO device not found. Is inv_mpu6050_i2c loaded?\n");
	return -1;
}

static int read_iio(const char *attr, const char *fmt, void *out)
{
	char path[PATH_MAX];
	FILE *f;
	int ok;

	if (snprintf(path, sizeof(path), "%s/%s", iio_path, attr) >= (int)sizeof(path))
		return -1;
	f = fopen(path, "r");
	if (!f)
		return -1;
	ok = fscanf(f, fmt, out) == 1;
	fclose(f);
	return ok ? 0 : -1;
}

static int run_imu(int oled_fd, uint8_t *fb)
{
	static const char *const attrs[6] = {
		"in_accel_x_raw", "in_accel_y_raw", "in_accel_z_raw",
		"in_anglvel_x_raw", "in_anglvel_y_raw", "in_anglvel_z_raw",
	};
	const struct timespec period = { 0, 100 * 1000 * 1000 };	/* 10 Hz */
	double accel_scale = 0;
	long one_g = 16384;	/* raw LSB per g at the default +-2 g range */
	long v[6];

	if (find_mpu6050())
		return -1;

	/* Follow the configured range: scale is m/s^2 per LSB */
	if (!read_iio("in_accel_scale", "%lf", &accel_scale) && accel_scale > 0)
		one_g = (long)(9.80665 / accel_scale + 0.5);

	printf("[oled-test] Streaming MPU6050 raw values, 1 g = %ld LSB (Ctrl+C to stop)\n", one_g);
	printf("%9s %9s %9s | %9s %9s %9s\n", "acc_x", "acc_y", "acc_z", "gyr_x", "gyr_y", "gyr_z");

	while (running) {
		for (int i = 0; i < 6; i++) {
			if (read_iio(attrs[i], "%ld", &v[i])) {
				fprintf(stderr, "\n[oled-test] failed to read %s\n", attrs[i]);
				return -1;
			}
		}
		printf("%9ld %9ld %9ld | %9ld %9ld %9ld\r", v[0], v[1], v[2], v[3], v[4], v[5]);
		fflush(stdout);

		pattern_accel_bar(fb, v[0], one_g);
		if (oled_show(oled_fd, fb))
			return -1;
		nanosleep(&period, NULL);
	}
	printf("\n");
	return 0;
}

/* ------------------------------------------------------------------------
 * Buttons: pick the evdev node that actually reports BTN_0..BTN_3
 * ------------------------------------------------------------------------ */
static int find_button_device(void)
{
	struct dirent *ent;
	DIR *d = opendir(INPUT_DEVICES_PATH);

	if (!d) {
		perror("[oled-test] opendir " INPUT_DEVICES_PATH);
		return -1;
	}

	while ((ent = readdir(d))) {
		unsigned long keys[NLONGS(KEY_CNT)] = { 0 };
		char path[PATH_MAX], name[128] = "?";
		int fd, ok = 1;

		if (strncmp(ent->d_name, "event", 5))
			continue;
		if (snprintf(path, sizeof(path), "%s/%s", INPUT_DEVICES_PATH,
			     ent->d_name) >= (int)sizeof(path))
			continue;
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;

		if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0)
			ok = 0;
		for (int b = BTN_0; ok && b < BTN_0 + NUM_BUTTONS; b++)
			ok = TEST_BIT(b, keys);

		if (ok) {
			ioctl(fd, EVIOCGNAME(sizeof(name)), name);
			printf("[oled-test] Buttons on %s (%s)\n", path, name);
			closedir(d);
			return fd;
		}
		close(fd);
	}

	closedir(d);
	fprintf(stderr, "[oled-test] no input device reports BTN_0..BTN_3. Check the gpio-keys node in the DTB\n");
	return -1;
}

static int run_buttons(int oled_fd, uint8_t *fb)
{
	struct input_event ev;
	int fd = find_button_device();

	if (fd < 0)
		return -1;

	printf("[oled-test] Waiting for button presses (Ctrl+C to stop)\n");
	while (running) {
		ssize_t n = read(fd, &ev, sizeof(ev));

		if (n < 0) {
			if (errno != EINTR)
				perror("[oled-test] read");
			break;
		}
		if (n != sizeof(ev) || ev.type != EV_KEY || ev.value != 1)
			continue;
		if (ev.code < BTN_0 || ev.code >= BTN_0 + NUM_BUTTONS)
			continue;

		printf("[oled-test] Pressed BTN_%d (code %u)\n", ev.code - BTN_0 + 1, ev.code);
		pattern_button(fb, ev.code - BTN_0);
		if (oled_show(oled_fd, fb))
			break;
	}
	close(fd);
	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [mode]\n"
		"  0  checkerboard (default)\n"
		"  1  all pixels on\n"
		"  2  clear\n"
		"  3  MPU6050 stream + accel bar\n"
		"  4  button monitor\n", prog);
}

int main(int argc, char *argv[])
{
	static uint8_t fb[SSD1315_FB_SIZE];
	long mode = 0;
	int fd, ret = 0;

	if (argc > 2) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (argc == 2) {
		char *end;

		mode = strtol(argv[1], &end, 10);
		if (*argv[1] == '\0' || *end != '\0' || mode < 0 || mode > 4) {
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	install_signals();

	fd = open(OLED_DEVICE, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		perror("[oled-test] open " OLED_DEVICE);
		fprintf(stderr, "  Is ssd1315_oled loaded? Try: modprobe ssd1315_oled\n");
		return EXIT_FAILURE;
	}

	switch (mode) {
	case 0:
		pattern_checkerboard(fb);
		ret = oled_show(fd, fb);
		break;
	case 1:
		memset(fb, 0xff, sizeof(fb));
		ret = oled_show(fd, fb);
		break;
	case 2:
		ret = ioctl(fd, SSD1315_IOCTL_CLEAR);
		if (ret)
			perror("[oled-test] ioctl CLEAR");
		break;
	case 3:
		ret = run_imu(fd, fb);
		break;
	case 4:
		ret = run_buttons(fd, fb);
		break;
	}

	close(fd);
	return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}
