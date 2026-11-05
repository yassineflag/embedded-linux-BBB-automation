# BeagleBone Black: Embedded Linux + SSD1315 OLED + MPU6050 + Buttons (I2C2)

A personal project that builds each embedded Linux component individually and automatically, no Yocto, no Buildroot. Each piece (bootloader, kernel, rootfs, drivers) is fetched and compiled by a dedicated script, and the SD card image is assembled **without root**. On the driver side, it includes a custom out of tree I2C kernel module for the SSD1315 OLED display, and integrates two in kernel drivers via Device Tree: the IIO based MPU6050 IMU driver and the generic `gpio-keys` driver for four push buttons.

---

## Project Structure

```
.
├── Makefile                          # Thin wrapper: one target per script
├── boot/
│   └── extlinux/extlinux.conf        # U-Boot distro boot entry (kernel, DTB, bootargs)
├── kernel/
│   ├── configs/bbb-oled.config       # Kernel config fragment (I2C, IIO, gpio-keys, rootfs)
│   └── dts/am335x-boneblack-oled.dts # Board DTS: SSD1315 + MPU6050 + buttons + pinmux
├── drivers/
│   └── ssd1315/
│       ├── ssd1315_oled.c            # SSD1315 I2C kernel driver (out of tree)
│       ├── ssd1315_ioctl.h           # Userspace ABI shared with oled-test
│       └── Makefile                  # Kbuild out of tree Makefile
├── tools/
│   └── oled-test/
│       ├── oled_test.c               # Userspace test app (5 modes)
│       └── Makefile
├── rootfs/
│   └── etc/init.d/S70oled            # Loads the drivers on boot
├── scripts/
│   ├── env.sh                        # Versions, paths, toolchain (single source of truth)
│   ├── build-uboot.sh
│   ├── build-kernel.sh
│   ├── build-rootfs.sh
│   ├── build-modules.sh
│   ├── build-oled-test.sh
│   ├── make-image.sh
│   └── BUILD_ROOTFS_EXPLAINED.md
└── .github/workflows/build.yml       # CI: lint + full build + image artifact
```

---

## Kernel Drivers

### SSD1315 OLED: out of tree I2C module (`drivers/ssd1315/ssd1315_oled.c`)

A custom kernel module written for the SSD1315 controller. It exposes `/dev/ssd1315` as a misc device: write exactly 1024 bytes (128x64 / 8) to update the full framebuffer, or use ioctl to clear, turn on or turn off the display.

The SSD1315 initialization differs from the more common SSD1306 on two constants:

| Parameter | SSD1315 (this driver) | SSD1306 |
|-----------|----------------------|---------|
| Charge pump voltage | `0x95` = 9 V | `0x14` = 7.5 V |
| Clock divider | `0xF0` (max oscillator freq) | `0x80` |

Other tunable values in the driver: contrast (`0xCF`), pre charge period (`0xF1`), VCOMH level (`0x20`). The Device Tree node `oled@3c` sets the I2C address: change `reg = <0x3c>` to `<0x3d>` if SA0 is pulled high.

Implementation notes:

* The whole init sequence is one I2C command stream, and GDDRAM is cleared **before** `DISPLAY_ON`, so there is no flash of random pixels at probe.
* A frame goes out as a single 1025 byte transfer from a buffer preallocated in the device struct: no allocation per frame.
* `write()` copies from userspace under the device mutex, so concurrent writers and `SSD1315_IOCTL_CLEAR` can not interleave inside a frame.
* The device struct is reference counted (`kref`): unbinding the driver or unplugging the I2C client while a process holds `/dev/ssd1315` open is safe, and that process gets `-ENODEV` instead of a use after free.

### MPU6050 IMU: in tree IIO driver (`inv_mpu6050_i2c`)

No custom driver needed, the upstream `inv_mpu6050_i2c` module handles it. The DTS node `imu@68` sets the I2C address: `reg = <0x68>` when AD0 is tied to GND, `<0x69>` when AD0 is pulled high.

The INT pin is optional. If it is wired to a BBB GPIO, add `interrupt-parent` and `interrupts` to the node (an example is in the DTS) to get data ready interrupts and IIO buffered capture.

### GPIO Buttons: in tree `gpio-keys` driver

Entirely configured in the Device Tree, no C code needed. Each button entry sets:
* `gpios`: GPIO bank, line and polarity (`GPIO_ACTIVE_LOW`)
* `linux,code`: the evdev key code reported to userspace (`BTN_0` .. `BTN_3` = 256 .. 259)
* `debounce-interval`: debounce in milliseconds (20 ms here)

The `button-pins` pinctrl group switches P8.7 to P8.10 from their reset function (GPMC) to GPIO mode 7 with the internal pull up enabled. Without it the GPIOs never see the buttons.

Button events are read from `/dev/input/eventX` with the standard Linux input API or with `evtest`. To remap or add buttons, only the DTS needs to change.

### Why a board DTS and not an overlay

`kernel/dts/am335x-boneblack-oled.dts` includes the upstream `am335x-boneblack.dts` and adds the nodes. It is copied into the kernel tree and built as `am335x-boneblack-oled.dtb`. An overlay would need the base DTB compiled with symbols (`dtc -@`, which upstream does not do for this board) and an `fdt apply` step in U-Boot. Since this project builds its own kernel, a board DTS is simpler and is checked by `dtc` at build time.

---

## Hardware Connections

### OLED + IMU: I2C2 Bus (P9.19 = SCL, P9.20 = SDA)

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
| MPU6050 | AD0 | P9.1 | GND = I2C addr 0x68 |

I2C2 also carries the cape EEPROMs at 0x54 to 0x57. The DTS raises the bus from 100 kHz to 400 kHz, which every device on it supports.

### GPIO Buttons: P8 Header (other leg to GND, P8.1 or P8.2)

| Button | BBB Pin | Pad (mode 7) | GPIO | Linux Key Code |
|:------:|:-------:|:------------:|:----:|:--------------:|
| BTN_1 | P8.7 | gpmc_advn_ale | GPIO2_2 | BTN_0 (256) |
| BTN_2 | P8.8 | gpmc_oen_ren | GPIO2_3 | BTN_1 (257) |
| BTN_3 | P8.9 | gpmc_be0n_cle | GPIO2_5 | BTN_2 (258) |
| BTN_4 | P8.10 | gpmc_wen | GPIO2_4 | BTN_3 (259) |

---

## Prerequisites

Tested on Ubuntu 24.04.

```bash
sudo apt-get install -y \
    gcc-arm-linux-gnueabihf binutils-arm-linux-gnueabihf \
    make git wget bzip2 bc bison flex libssl-dev \
    device-tree-compiler kmod \
    fdisk dosfstools mtools e2fsprogs fakeroot
```

---

## Build

```bash
# Build everything, ends with output/sdcard.img
make all

# Or step by step:
make toolchain   # check the cross compiler
make uboot       # output/boot/MLO, u-boot.img
make kernel      # output/boot/zImage, am335x-boneblack-oled.dtb, output/modules/
make rootfs      # output/rootfs/ (rebuilt from scratch)
make modules     # ssd1315_oled.ko into output/rootfs/lib/modules/
make oled-test   # output/rootfs/usr/bin/oled-test
make image       # output/sdcard.img
```

Versions (pinned in `scripts/env.sh`): **U-Boot v2024.01**, **Linux v6.6.21 LTS**, **BusyBox 1.36.1**. Override any of them on the command line, for example `make KERNEL_TAG=v6.6.60 all`. A source tree already in `src/` at a different tag is refused: run `make distclean` or remove that directory.

`build-kernel.sh` verifies that every `CONFIG_*` line of the fragment ended up in the final `.config`. Kconfig silently drops options whose dependencies are not met, and this turns that into a build error.

No step needs `sudo`: the image is assembled from standalone FAT and ext4 filesystem files (`mtools`, `mkfs.ext4 -d` under `fakeroot`).

---

## Boot Flow

1. The AM335x ROM finds the **active FAT partition** on the SD card and loads `MLO` (U-Boot SPL) into internal SRAM.
2. SPL initializes DDR3 and loads `u-boot.img` from the same FAT partition.
3. U-Boot runs `distro_bootcmd`, finds `/extlinux/extlinux.conf` on the bootable partition, and loads `zImage` and `am335x-boneblack-oled.dtb`.
4. The kernel mounts `root=PARTUUID=bbb0a1ed-02` (the ext4 partition) and starts BusyBox `init`.

> U-Boot v2024.01 `am335x_evm` no longer imports `uEnv.txt` (its bootcmd is `findfdt; init_console; finduuid; distro_bootcmd`), which is why the boot configuration is an `extlinux.conf`. Edit `append` there to change the kernel command line. `make-image.sh` reads the PARTUUID from that line and writes the matching MBR disk id, so it only needs changing in one place.

---

## SD Card Layout

`make image` produces `output/sdcard.img` (512 MiB, MBR, disk id `0xbbb0a1ed`):

```
+---------------------------------------------------+  0
| MBR                                               |
+---------------------------------------------------+  4 MiB
| Partition 1  FAT32 "BOOT", bootable, 64 MiB       |
|   MLO                  (U-Boot SPL)               |
|   u-boot.img                                      |
|   zImage                                          |
|   am335x-boneblack-oled.dtb  (+ stock DTB)        |
|   extlinux/extlinux.conf                          |
+---------------------------------------------------+  68 MiB
| Partition 2  ext4 "rootfs", rest of the image     |
|   /bin /sbin /usr      BusyBox, oled-test         |
|   /lib/modules/<release>/updates/ssd1315_oled.ko  |
|   /etc/init.d/S70oled  (loads the drivers)        |
+---------------------------------------------------+  512 MiB
```

Use `make IMG_SIZE_MIB=1024 image` for a bigger rootfs partition.

---

## Flash to microSD

```bash
# Replace /dev/sdX with your microSD card (check with lsblk first!)
sudo dd if=output/sdcard.img of=/dev/sdX bs=4M conv=fsync status=progress
```

---

## Boot & Test on BBB

Insert the SD card and power on the BBB while holding the **BOOT button** (S2). Otherwise the ROM boots from eMMC first.

Connect a 3.3 V UART cable to the J1 debug header (115200 8N1):
```bash
picocom -b 115200 /dev/ttyUSB0
```

`S70oled` loads `ssd1315_oled` and `inv_mpu6050_i2c` before the shell prompt. Press Enter to get a shell on `ttyS0`.

```sh
# I2C devices on bus 2: 3c = SSD1315, 68 = MPU6050, 54..57 = cape EEPROMs.
# Bound addresses show as UU.
i2cdetect -y -r 2

# Driver status
/etc/init.d/S70oled status
dmesg | grep -i ssd1315
# Expected: "SSD1315 128x64 ready at 0x3c, /dev/ssd1315"

# Test OLED
oled-test 0          # checkerboard
oled-test 1          # all on
oled-test 2          # clear

# Stream MPU6050 accel/gyro + live X accel bar on the OLED
oled-test 3          # Ctrl+C to stop

# Monitor button presses
oled-test 4          # press BTN_1..BTN_4, Ctrl+C to stop
```

> **Note:** if the drivers did not load: `modprobe ssd1315_oled && modprobe inv_mpu6050_i2c`, then check `dmesg`.

---

## Driver API: /dev/ssd1315

Include `drivers/ssd1315/ssd1315_ioctl.h`.

```c
#include "ssd1315_ioctl.h"

// Full frame: 8 pages of 128 columns, 1 byte = 8 vertical pixels, LSB on top
int fd = open("/dev/ssd1315", O_WRONLY);
write(fd, framebuf, SSD1315_FB_SIZE);     // anything else than 1024 bytes: EINVAL

ioctl(fd, SSD1315_IOCTL_CLEAR);           // clear the display
ioctl(fd, SSD1315_IOCTL_OFF);             // panel off (GDDRAM kept)
ioctl(fd, SSD1315_IOCTL_ON);              // panel on
```

## MPU6050: IIO sysfs

```sh
D=$(grep -l mpu6050 /sys/bus/iio/devices/iio:device*/name | xargs dirname)
cat $D/in_accel_x_raw $D/in_accel_scale      # m/s^2 = raw * scale
cat $D/in_anglvel_x_raw $D/in_anglvel_scale  # rad/s = raw * scale
```

---

## Continuous Integration

`.github/workflows/build.yml` runs on every push and pull request:

1. **Lint**: `shellcheck` on the scripts, `checkpatch.pl --strict` on the driver.
2. **Build**: `make all` on Ubuntu 24.04, with the downloaded sources cached between runs.
3. **Artifacts**: `sdcard.img` and the boot files are uploaded for every run.

---

## Porting to Another Board

1. Change the U-Boot defconfig in `scripts/build-uboot.sh` and the kernel defconfig and DTS directory in `scripts/build-kernel.sh`.
2. Change `#include "am335x-boneblack.dts"`, the `&i2cX` bus, the pinmux and the GPIO numbers in `kernel/dts/am335x-boneblack-oled.dts`.
3. Update `CONFIG_I2C_<ADAPTER>` in `kernel/configs/bbb-oled.config`.

---

## Going Deeper

[scripts/BUILD_ROOTFS_EXPLAINED.md](scripts/BUILD_ROOTFS_EXPLAINED.md): step by step breakdown of how the BusyBox root filesystem is assembled, covering static linking, `inittab`, `rcS`, `fstab`, modules and how `make-image.sh` packs it.
