# BeagleBone Black: Embedded Linux + SSD1315 OLED + MPU6050 + Buttons (I2C2)

A personal project that builds each embedded Linux component individually and automatically, no Yocto, no Buildroot. Each piece (bootloader, kernel, rootfs, drivers) is fetched and compiled by a dedicated script. On the driver side, it includes a custom out-of-tree I2C kernel module for the SSD1315 OLED display, and integrates two in-kernel drivers via Device Tree: the IIO-based MPU6050 IMU driver and the generic `gpio-keys` driver for four push-buttons.

---

## Project Structure

```
.
├── Makefile                                    # Top-level build orchestrator
├── boot/
│   └── uEnv.txt                                # U-Boot environment
├── kernel/
│   ├── configs/bbb-oled.config                 # Kernel config fragment (I2C, IIO, gpio-keys)
│   └── dts/bbb-ssd1315-mpu6050.dts            # Device Tree overlay (SSD1315 + MPU6050 + buttons)
├── drivers/
│   └── ssd1315/
│       ├── ssd1315_oled.c                      # SSD1315 I2C kernel driver (out-of-tree)
│       └── Makefile                            # Out-of-tree module Makefile
├── tools/
│   └── oled-test/
│       ├── oled_test.c                         # Userspace test app (5 modes)
│       └── Makefile
├── rootfs/
│   └── etc/init.d/S70oled                     # Auto-load drivers on boot
└── scripts/
    ├── build-uboot.sh
    ├── build-kernel.sh
    ├── build-modules.sh
    ├── build-rootfs.sh
    ├── build-oled-test.sh
    ├── make-image.sh
    └── BUILD_ROOTFS_EXPLAINED.md
```

---

## Kernel Drivers

### SSD1315 OLED: out-of-tree I2C module (`drivers/ssd1315/ssd1315_oled.c`)

A custom kernel module written specifically for the SSD1315 controller. It exposes `/dev/ssd1315` as a misc device: write 1024 bytes (128x64 / 8) to update the full framebuffer, or use ioctl to clear/turn on/off the display.

The SSD1315 initialization differs from the more common SSD1306 on two constants:

| Parameter | SSD1315 (this driver) | SSD1306 |
|-----------|----------------------|---------|
| Charge pump voltage | `0x95` -> 9 V | `0x14` -> 7.5 V |
| Clock divider | `0xF0` (max oscillator freq) | `0x80` |

Other tunable values in the driver: contrast (`0xCF`), pre-charge period (`0xF1`), VCOMH level (`0x20`). The Device Tree node (`oled@3c`) sets the I2C address; change `reg = <0x3c>` to `<0x3d>` if SA0 is pulled high.

### MPU6050 IMU: in-tree IIO driver (`inv_mpu6050_i2c`)

No custom driver needed, the upstream `inv_mpu6050_i2c` module handles it. The DTS node (`imu@68`) sets the I2C address: `reg = <0x68>` when AD0 is tied to GND, `<0x69>` when AD0 is pulled high (useful if both I2C addresses on the bus need to be distinct).

The INT pin is optional. If wired to a BBB GPIO, add `interrupt-parent` and `interrupts` to the DTS node to enable interrupt-driven data-ready notifications instead of polling.

### GPIO Buttons: in-tree `gpio-keys` driver

Entirely configured in the Device Tree, no C code needed. Each button entry in the DTS sets:
- `gpios`: which GPIO bank/line, and polarity (`1` = active-low)
- `linux,code`: the evdev key code reported to userspace (BTN_0..BTN_3 = 256..259)
- `debounce-interval`: hardware debounce in milliseconds (20 ms here)

Button events are read from `/dev/input/eventX` with the standard Linux input API or with `evtest`. To remap or add buttons, only the DTS needs to change; the kernel driver is generic.

---

## Hardware Connections

### OLED + IMU: I2C2 Bus (P9.19=SCL, P9.20=SDA)

| Device | SSD1315 Pin / MPU6050 Pin | BBB Pin | Signal |
|:------:|:-------------------------:|:-------:|:------:|
| SSD1315 | VCC | P9.3 | 3.3V |
| SSD1315 | GND | P9.1 | GND |
| SSD1315 | **SCL** | **P9.19** | I2C2_SCL |
| SSD1315 | **SDA** | **P9.20** | I2C2_SDA |
| MPU6050 | VCC | P9.3 | 3.3V |
| MPU6050 | GND | P9.1 | GND |
| MPU6050 | **SCL** | **P9.19** | I2C2_SCL |
| MPU6050 | **SDA** | **P9.20** | I2C2_SDA |
| MPU6050 | AD0 | P9.1 | GND -> I2C addr 0x68 |

### GPIO Buttons: P8 Header (connect other pin to GND)

| Button | BBB Pin | GPIO | Linux Key Code |
|:------:|:-------:|:----:|:--------------:|
| BTN_1 | P8.7 | GPIO2_2 | BTN_0 (256) |
| BTN_2 | P8.8 | GPIO2_3 | BTN_1 (257) |
| BTN_3 | P8.9 | GPIO2_5 | BTN_2 (258) |
| BTN_4 | P8.10 | GPIO2_4 | BTN_3 (259) |

---

## Prerequisites

```bash
sudo apt-get install -y \
    gcc-arm-linux-gnueabihf binutils-arm-linux-gnueabihf \
    bc bison flex libssl-dev libncurses-dev \
    device-tree-compiler \
    parted dosfstools e2fsprogs \
    wget git make
```

---

## Build

```bash
# Build everything
make all

# Or step by step:
make toolchain   # verify cross-compiler
make uboot       # -> output/boot/MLO, u-boot.img
make kernel      # -> output/boot/zImage, am335x-boneblack.dtb
make modules     # -> drivers/ssd1315/ssd1315_oled.ko
make rootfs      # -> output/rootfs/
make oled-test   # -> output/rootfs/usr/bin/oled-test
make image       # -> output/sdcard.img
```

Versions: **U-Boot v2024.01**, **Linux v6.6.21 LTS**, **BusyBox 1.36.1**

---

## SD Card Layout

`make image` produces `output/sdcard.img` with two partitions:

```
+------------------------------------------------+
| Partition 1 (FAT32, 64 MB)  boot               |
|   MLO              (AM335x ROM 1st stage)       |
|   u-boot.img                                    |
|   uEnv.txt         (boot environment)           |
|   zImage           (Linux kernel)               |
|   am335x-boneblack.dtb  (Device Tree)           |
+------------------------------------------------+
| Partition 2 (ext4, remainder)  rootfs           |
|   /bin, /sbin, /lib, /etc  (BusyBox)            |
|   /lib/modules/.../ssd1315_oled.ko              |
|   /usr/bin/oled-test                            |
|   /etc/init.d/S70oled  (auto modprobe on boot)  |
+------------------------------------------------+
```

---

## Flash to microSD

```bash
# Replace /dev/sdX with your microSD card (check with lsblk first!)
sudo dd if=output/sdcard.img of=/dev/sdX bs=4M status=progress && sync
```

---

## Boot & Test on BBB

Insert SD card, power on BBB while holding **BOOT button** (S2).

Connect UART debug cable (115200 8N1):
```bash
picocom -b 115200 /dev/ttyUSB0
```

`S70oled` auto-loads `ssd1315_oled` and `inv_mpu6050_i2c` before you get a shell prompt.

```sh
# Verify I2C devices visible (should show 3c=SSD1315, 68=MPU6050)
i2cdetect -y 2

# Confirm drivers loaded
dmesg | grep -i ssd1315
# Expected: "SSD1315 initialised (128x64, horizontal addr, 9V pump)"
# Expected: "/dev/ssd1315 registered"

dmesg | grep -i mpu6050
# Expected: "inv_mpu6050: IMU at addr 0x68"

# Test OLED
oled-test 0          # checkerboard
oled-test 1          # all ON
oled-test 2          # clear

# Stream MPU6050 accel/gyro + live bar on OLED
oled-test 3          # Ctrl+C to stop

# Monitor button presses
oled-test 4          # press BTN_1..BTN_4, Ctrl+C to stop
```

> **Note:** If drivers didn't auto-load: `modprobe ssd1315_oled && modprobe inv_mpu6050_i2c`

---

## Driver API: /dev/ssd1315

```c
// Send 1024-byte framebuffer (128 cols x 8 pages, 1 bit per pixel)
int fd = open("/dev/ssd1315", O_WRONLY);
write(fd, framebuf, 1024);

// ioctl commands
#define SSD1315_IOCTL_CLEAR  _IO('S', 0)   // clear display
#define SSD1315_IOCTL_ON     _IO('S', 1)   // display on
#define SSD1315_IOCTL_OFF    _IO('S', 2)   // display off
ioctl(fd, SSD1315_IOCTL_CLEAR);
```

## MPU6050: IIO sysfs

```sh
# After modprobe inv_mpu6050_i2c:
cat /sys/bus/iio/devices/iio:device0/in_accel_x_raw
cat /sys/bus/iio/devices/iio:device0/in_anglvel_x_raw
```

---

## Porting to Another Board

1. Change `CROSS_COMPILE`, kernel `defconfig`, and DTB filename in `Makefile`
2. Update `&i2cX` bus and GPIO numbers in `kernel/dts/bbb-ssd1315-mpu6050.dts`
3. Update `CONFIG_I2C_<ADAPTER>` in `kernel/configs/bbb-oled.config`

---

## Going Deeper

[scripts/BUILD_ROOTFS_EXPLAINED.md](scripts/BUILD_ROOTFS_EXPLAINED.md): step-by-step breakdown of how `build-rootfs.sh` assembles the BusyBox root filesystem, covering static linking, `inittab`, `rcS`, `fstab`, `depmod`, and more.
