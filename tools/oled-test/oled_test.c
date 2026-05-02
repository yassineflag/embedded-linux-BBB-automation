/*
 * oled_test.c — Userspace test app for SSD1315 OLED + MPU6050 + 4 buttons
 *
 * Usage: oled-test [mode]
 *   mode 0  — show checkerboard on OLED (default)
 *   mode 1  — all pixels ON
 *   mode 2  — clear display
 *   mode 3  — stream MPU6050 accel/gyro data to stdout + show bar on OLED
 *   mode 4  — wait for button presses and print which button was pressed
 *
 * Devices used:
 *   /dev/ssd1315          — SSD1315 OLED (our kernel module)
 *   /sys/bus/iio/devices/ — MPU6050 via in-tree IIO driver
 *   /dev/input/eventX     — GPIO buttons via gpio-keys driver
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <linux/input.h>

/* -------------------------------------------------------------------------
 * OLED constants
 * ------------------------------------------------------------------------- */
#define OLED_DEVICE         "/dev/ssd1315"
#define DISPLAY_WIDTH       128
#define DISPLAY_PAGES       8
#define FRAMEBUF_SIZE       (DISPLAY_WIDTH * DISPLAY_PAGES)  /* 1024 bytes */

#define SSD1315_IOCTL_CLEAR  _IO('S', 0)
#define SSD1315_IOCTL_ON     _IO('S', 1)
#define SSD1315_IOCTL_OFF    _IO('S', 2)

/* -------------------------------------------------------------------------
 * MPU6050 IIO sysfs paths
 * The in-tree inv_mpu6050_i2c driver exposes data under:
 *   /sys/bus/iio/devices/iio:device<N>/
 * We find the right device by checking for the "invensense" name.
 * ------------------------------------------------------------------------- */
#define IIO_DEVICES_PATH    "/sys/bus/iio/devices"
#define MPU_NAME            "mpu6050"
#define IIO_SCALE_DEFAULT   "0.000598"   /* default accel scale m/s² per LSB */

static char iio_path[256] = "";   /* filled by find_mpu6050() */

/**
 * find_mpu6050() - Scan IIO devices to find the MPU6050 sysfs path.
 * Returns 0 on success, -1 if not found.
 */
static int find_mpu6050(void)
{
    DIR *d;
    struct dirent *ent;
    char namepath[512], name[64];
    FILE *f;

    d = opendir(IIO_DEVICES_PATH);
    if (!d) {
        perror("[oled-test] opendir " IIO_DEVICES_PATH);
        return -1;
    }

    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "iio:device", 10) != 0)
            continue;

        snprintf(namepath, sizeof(namepath), "%s/%s/name",
                 IIO_DEVICES_PATH, ent->d_name);
        f = fopen(namepath, "r");
        if (!f) continue;

        if (fgets(name, sizeof(name), f) && strstr(name, MPU_NAME)) {
            snprintf(iio_path, sizeof(iio_path), "%s/%s",
                     IIO_DEVICES_PATH, ent->d_name);
            fclose(f);
            closedir(d);
            printf("[oled-test] Found MPU6050 at: %s\n", iio_path);
            return 0;
        }
        fclose(f);
    }

    closedir(d);
    fprintf(stderr, "[oled-test] MPU6050 IIO device not found.\n"
                    "  Hint: Is inv_mpu6050_i2c module loaded?\n");
    return -1;
}

/**
 * read_iio_value() - Read a single integer raw value from an IIO sysfs file.
 */
static int read_iio_value(const char *filename, long *out)
{
    char path[512];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", iio_path, filename);
    f = fopen(path, "r");
    if (!f) return -1;

    if (fscanf(f, "%ld", out) != 1) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

/* -------------------------------------------------------------------------
 * OLED pattern generators
 * ------------------------------------------------------------------------- */
static void pattern_checkerboard(uint8_t *fb)
{
    int page, col;
    for (page = 0; page < DISPLAY_PAGES; page++)
        for (col = 0; col < DISPLAY_WIDTH; col++) {
            uint8_t v = (col % 2) ? 0x55 : 0xAA;
            fb[page * DISPLAY_WIDTH + col] = (page % 2) ? (v ^ 0xFF) : v;
        }
}

static void pattern_all_on(uint8_t *fb)  { memset(fb, 0xFF, FRAMEBUF_SIZE); }
static void pattern_all_off(uint8_t *fb) { memset(fb, 0x00, FRAMEBUF_SIZE); }

/**
 * pattern_accel_bar() - Draw a horizontal bar proportional to X acceleration.
 * Maps accel_x_raw (−32768..32767) to a bar width across DISPLAY_WIDTH.
 * The bar fills page 3 (vertical middle of the 64px display).
 */
static void pattern_accel_bar(uint8_t *fb, long accel_x)
{
    int bar_width;
    int col, page;

    memset(fb, 0x00, FRAMEBUF_SIZE);

    /* Clamp to ±16384 (±1g at default ±2g range), map to 0–128 */
    if (accel_x < -16384) accel_x = -16384;
    if (accel_x >  16384) accel_x =  16384;
    bar_width = (int)((accel_x + 16384) * DISPLAY_WIDTH / 32768);

    /* Draw bar on pages 2, 3, 4 (centre of display) */
    for (page = 2; page <= 4; page++)
        for (col = 0; col < bar_width && col < DISPLAY_WIDTH; col++)
            fb[page * DISPLAY_WIDTH + col] = 0xFF;
}

/* -------------------------------------------------------------------------
 * Find GPIO buttons input device
 * ------------------------------------------------------------------------- */
#define INPUT_DEVICES_PATH  "/dev/input"
#define BTN_0_CODE          256
#define BTN_3_CODE          259

/**
 * find_button_device() - Find the /dev/input/eventX for our gpio-keys.
 * Returns an open fd or -1.
 */
static int find_button_device(void)
{
    DIR *d;
    struct dirent *ent;
    char path[256], name[128];
    int fd;

    d = opendir(INPUT_DEVICES_PATH);
    if (!d) return -1;

    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) != 0) continue;

        snprintf(path, sizeof(path), "%s/%s", INPUT_DEVICES_PATH, ent->d_name);
        fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 &&
            strstr(name, "gpio") != NULL) {
            printf("[oled-test] Found button device: %s (%s)\n", path, name);
            closedir(d);
            return fd;
        }
        close(fd);
    }

    closedir(d);
    fprintf(stderr, "[oled-test] GPIO button input device not found.\n");
    return -1;
}

/* -------------------------------------------------------------------------
 * Signal handler for clean exit in streaming modes
 * ------------------------------------------------------------------------- */
static volatile int running = 1;
static void sig_handler(int s) { (void)s; running = 0; }

/* -------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    int mode = 0;
    int oled_fd;
    uint8_t fb[FRAMEBUF_SIZE];

    if (argc > 1) mode = atoi(argv[1]);

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    /* Open OLED device */
    oled_fd = open(OLED_DEVICE, O_WRONLY);
    if (oled_fd < 0) {
        perror("[oled-test] open " OLED_DEVICE);
        fprintf(stderr, "  Hint: Is ssd1315_oled module loaded?\n");
        return EXIT_FAILURE;
    }

    switch (mode) {

    /* --- Mode 0: Checkerboard --- */
    case 0:
        pattern_checkerboard(fb);
        printf("[oled-test] Pattern: checkerboard\n");
        if (write(oled_fd, fb, FRAMEBUF_SIZE) != FRAMEBUF_SIZE)
            perror("[oled-test] write");
        break;

    /* --- Mode 1: All ON --- */
    case 1:
        pattern_all_on(fb);
        if (write(oled_fd, fb, FRAMEBUF_SIZE) != FRAMEBUF_SIZE)
            perror("[oled-test] write");
        printf("[oled-test] All pixels ON\n");
        break;

    /* --- Mode 2: Clear --- */
    case 2:
        pattern_all_off(fb);
        if (write(oled_fd, fb, FRAMEBUF_SIZE) != FRAMEBUF_SIZE)
            perror("[oled-test] write");
        printf("[oled-test] Display cleared\n");
        break;

    /* --- Mode 3: MPU6050 streaming + OLED accel bar --- */
    case 3: {
        long ax, ay, az, gx, gy, gz;

        if (find_mpu6050() < 0) { close(oled_fd); return EXIT_FAILURE; }

        printf("[oled-test] Streaming MPU6050 (Ctrl+C to stop)...\n");
        printf("%-12s %-12s %-12s | %-12s %-12s %-12s\n",
               "Accel X", "Accel Y", "Accel Z", "Gyro X", "Gyro Y", "Gyro Z");

        while (running) {
            if (read_iio_value("in_accel_x_raw",  &ax) ||
                read_iio_value("in_accel_y_raw",  &ay) ||
                read_iio_value("in_accel_z_raw",  &az) ||
                read_iio_value("in_anglvel_x_raw", &gx) ||
                read_iio_value("in_anglvel_y_raw", &gy) ||
                read_iio_value("in_anglvel_z_raw", &gz)) {
                fprintf(stderr, "[oled-test] Failed to read IIO values\n");
                break;
            }

            printf("%-12ld %-12ld %-12ld | %-12ld %-12ld %-12ld\r",
                   ax, ay, az, gx, gy, gz);
            fflush(stdout);

            /* Draw X-acceleration bar on OLED */
            pattern_accel_bar(fb, ax);
            write(oled_fd, fb, FRAMEBUF_SIZE);

            usleep(100000);  /* 10 Hz refresh */
        }
        printf("\n[oled-test] Stopped.\n");
        break;
    }

    /* --- Mode 4: Button event monitor --- */
    case 4: {
        struct input_event ev;
        int btn_fd;
        const char *btn_names[] = { "BTN_1", "BTN_2", "BTN_3", "BTN_4" };

        btn_fd = find_button_device();
        if (btn_fd < 0) { close(oled_fd); return EXIT_FAILURE; }

        /* Make button fd blocking for clean event reads */
        int flags = fcntl(btn_fd, F_GETFL, 0);
        fcntl(btn_fd, F_SETFL, flags & ~O_NONBLOCK);

        printf("[oled-test] Waiting for button presses (Ctrl+C to stop)...\n");

        while (running) {
            ssize_t n = read(btn_fd, &ev, sizeof(ev));
            if (n < 0) { if (errno == EINTR) break; perror("read"); break; }
            if (n != sizeof(ev)) continue;

            /* Only process key press events (type=1, value=1) */
            if (ev.type == EV_KEY && ev.value == 1 &&
                ev.code >= BTN_0_CODE && ev.code <= BTN_3_CODE) {
                int idx = ev.code - BTN_0_CODE;
                printf("[oled-test] Pressed: %s (code %d)\n",
                       btn_names[idx], ev.code);

                /* Show button number as filled columns on OLED */
                memset(fb, 0x00, FRAMEBUF_SIZE);
                int seg_width = DISPLAY_WIDTH / 4;
                int start_col = idx * seg_width;
                for (int p = 0; p < DISPLAY_PAGES; p++)
                    for (int c = start_col; c < start_col + seg_width; c++)
                        fb[p * DISPLAY_WIDTH + c] = 0xFF;
                write(oled_fd, fb, FRAMEBUF_SIZE);
            }
        }
        close(btn_fd);
        printf("\n[oled-test] Stopped.\n");
        break;
    }

    default:
        fprintf(stderr, "Unknown mode %d.\n"
                "  0=checkerboard  1=all_on  2=clear\n"
                "  3=mpu6050_stream  4=button_monitor\n", mode);
        close(oled_fd);
        return EXIT_FAILURE;
    }

    close(oled_fd);
    return EXIT_SUCCESS;
}
