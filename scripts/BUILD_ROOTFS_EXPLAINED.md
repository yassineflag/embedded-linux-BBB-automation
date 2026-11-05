# build-rootfs.sh: Deep Dive

A walkthrough of `scripts/build-rootfs.sh`, which assembles a minimal **BusyBox** root filesystem for the BeagleBone Black, and of how the rest of the pipeline finishes it.

---

## What is a RootFS?

The root filesystem is everything the kernel mounts at `/` after boot: userspace binaries, libraries, configuration files and init scripts. On an embedded system it is kept as small as possible. **BusyBox** gives us a shell plus the core Unix utilities in a single ~1 MB static binary.

---

## Step by Step

### 1. Shared environment

```bash
set -euo pipefail
STEP=ROOTFS
. "$(dirname "$0")/env.sh"
```

| Flag | Effect |
|------|--------|
| `-e` | Exit immediately if any command fails |
| `-u` | Treat unset variables as errors |
| `-o pipefail` | A pipe fails if **any** command in it fails |

`scripts/env.sh` is sourced by every script. It defines `TOP`, the `src/` and `output/` paths, `CROSS_COMPILE`, `JOBS` and the pinned versions (`BUSYBOX_VERSION`, `BUSYBOX_SHA256`, ...). Every value can be overridden from the environment or from `make`, for example `make BUSYBOX_VERSION=... rootfs`.

---

### 2. Fetch BusyBox, checksum verified

```bash
wget -q -O "${TARBALL}" "https://busybox.net/downloads/busybox-${BUSYBOX_VERSION}.tar.bz2"
echo "${BUSYBOX_SHA256}  ${TARBALL}" | sha256sum -c --quiet -
```

The tarball is kept in `src/` and verified against a pinned SHA256 before extraction, so a corrupted or tampered download fails the build instead of producing a broken image.

---

### 3. Configure: defconfig, static, no `tc`

```bash
make defconfig
sed -i -e 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' \
       -e 's/^CONFIG_TC=y/# CONFIG_TC is not set/' .config
make oldconfig < /dev/null
```

Configuration only happens when `src/busybox/.config` does not exist, so local tweaks made with `make -C src/busybox menuconfig` survive rebuilds.

**Why static?** A dynamically linked BusyBox needs glibc and `ld-linux-armhf.so.3` in `/lib`. Static linking puts everything into the single `busybox` binary. glibc prints warnings about `getpwnam` and friends in static binaries: they are harmless for this rootfs.

**Why no `tc`?** The `tc` applet in BusyBox 1.36.1 uses the CBQ qdisc definitions that were removed from the kernel UAPI headers in Linux 6.8. Cross toolchains on Ubuntu 24.04 and newer ship those headers, so `tc` fails to compile.

---

### 4. Build and install

```bash
make -j"${JOBS}"
rm -rf "${ROOTFS_OUT}"
make CONFIG_PREFIX="${ROOTFS_OUT}" install
```

The rootfs directory is **deleted and recreated on every run**: stale files from an earlier build can never leak into the image. BusyBox's install target creates `bin/`, `sbin/`, `usr/bin/`, `usr/sbin/` where every applet (`ls`, `sh`, `mount`, `modprobe` ...) is a symlink to `bin/busybox`. BusyBox looks at `argv[0]` to pick the applet.

---

### 5. Directory skeleton

```bash
mkdir -p "${ROOTFS_OUT}"/{dev,proc,sys,tmp,run,mnt,root,etc/init.d,usr/bin,usr/sbin,lib/modules}
chmod 1777 "${ROOTFS_OUT}/tmp"
```

| Directory | Purpose |
|-----------|---------|
| `/proc`, `/sys` | Mount points for procfs and sysfs |
| `/dev` | Mount point for devtmpfs (mounted by the kernel) |
| `/tmp`, `/run` | tmpfs mount points (RAM) |
| `/root` | Home of the root user |
| `/etc/init.d` | `rcS`, `rcK` and the `S??*` service scripts |
| `/lib/modules` | Kernel modules and the `modules.dep` database |

---

### 6. `/etc/inittab`

```
::sysinit:/etc/init.d/rcS
ttyS0::askfirst:-/bin/sh
::ctrlaltdel:/sbin/reboot
::shutdown:/etc/init.d/rcK
::shutdown:/bin/umount -a -r
```

BusyBox `init` (PID 1) reads this file. Format: `tty:runlevels:action:process` (runlevels are ignored by BusyBox).

| Line | Meaning |
|------|---------|
| `::sysinit:/etc/init.d/rcS` | Run once at boot, before anything else |
| `ttyS0::askfirst:-/bin/sh` | "Press Enter" then a login shell on the debug UART |
| `::shutdown:/etc/init.d/rcK` | Stop services (`S??* stop`) in reverse order |
| `::shutdown:/bin/umount -a -r` | Unmount, or remount read only, before power off |

The shell is bound to **`ttyS0`, not `/dev/console`**. `/dev/console` can not be a controlling terminal, so a shell on it has no job control and **Ctrl+C does not send SIGINT** to foreground programs. `oled-test 3` and `oled-test 4` rely on Ctrl+C to stop. On kernel 6.6 the AM335x UART is driven by `8250_omap`, so the device is `ttyS0` (the old `ttyO0` name only works through a compatibility fixup).

---

### 7. `/etc/init.d/rcS`

```sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mountpoint -q /dev || mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/pts
mount -a
hostname -F /etc/hostname
echo /sbin/mdev > /proc/sys/kernel/hotplug
mdev -s
for s in /etc/init.d/S??*; do [ -x "$s" ] && "$s" start; done
```

1. **procfs and sysfs** first: `ps`, `modprobe`, `mdev` and IIO all depend on them.
2. **devtmpfs** is normally already on `/dev`: the kernel config fragment sets `CONFIG_DEVTMPFS_MOUNT=y`, so the kernel mounts it right after the root filesystem. The `mountpoint` check keeps the script correct on kernels without that option.
3. **`mount -a`** mounts what `/etc/fstab` lists: `/tmp`, `/run` and `/dev/pts`.
4. **`hostname -F`**: the kernel does not read `/etc/hostname` by itself, userspace has to.
5. **mdev**: registered as hotplug helper for devices that appear later, and `mdev -s` does one coldplug pass for the ones already there. devtmpfs already creates the nodes, mdev adds permissions and symlinks if `/etc/mdev.conf` asks for them.
6. **`S??*` scripts** run in glob order, which is alphabetical: `S70oled` loads `ssd1315_oled` and `inv_mpu6050_i2c`. Pick the number to control ordering.

`rcK` is the mirror image, run at shutdown: every `S??*` script with `stop`, highest number first.

---

### 8. `/etc/fstab`

```
tmpfs   /tmp      tmpfs   mode=1777  0 0
tmpfs   /run      tmpfs   mode=0755  0 0
devpts  /dev/pts  devpts  defaults   0 0
```

Only what `mount -a` should mount. proc and sysfs are mounted explicitly by `rcS`, and devtmpfs by the kernel: listing them here would make `mount -a` try to mount them a second time.

---

### 9. Users, profile, hostname

```bash
echo "root:x:0:0:root:/root:/bin/sh" > etc/passwd
echo "root:x:0:"                     > etc/group
echo "bbb-oled"                      > etc/hostname
```

`/etc/passwd` and `/etc/group` let `ls -l`, `id` and the shell prompt resolve uid 0 to `root`. `/etc/profile` sets `PATH` and a `user@host:dir#` prompt for the login shell.

---

### 10. Service script

```bash
install -m 755 "${TOP}/rootfs/etc/init.d/S70oled" "${ROOTFS_OUT}/etc/init.d/S70oled"
```

`rcS` only runs scripts that are executable (`[ -x "$s" ]`), hence `install -m 755`.

---

### 11. In tree kernel modules

```bash
cp -a "${MODULES_OUT}/lib/modules/." "${ROOTFS_OUT}/lib/modules/"
rm -f "${ROOTFS_OUT}"/lib/modules/*/build "${ROOTFS_OUT}"/lib/modules/*/source
```

`build-kernel.sh` runs `modules_install INSTALL_MOD_PATH=output/modules INSTALL_MOD_STRIP=1`, which already produces `modules.dep`, `modules.alias` and friends. They are copied as is. The `build` and `source` symlinks point into the host's kernel tree and mean nothing on the target, so they are removed.

---

## After build-rootfs.sh

| Step | Script | What it adds |
|------|--------|--------------|
| `make modules` | `build-modules.sh` | `ssd1315_oled.ko` via kbuild `modules_install` into `lib/modules/<release>/updates/`, then `depmod` so `modprobe ssd1315_oled` works |
| `make oled-test` | `build-oled-test.sh` | `/usr/bin/oled-test` (static) |
| `make image` | `make-image.sh` | Packs the tree into ext4 with `mkfs.ext4 -d` inside **fakeroot**, so every file is owned by `root:root` on the target although the build ran as a normal user |

## Output

```
output/rootfs/
├── bin/ sbin/ usr/bin/ usr/sbin/   <- BusyBox applets, oled-test
├── lib/modules/<release>/
│   ├── kernel/...                  <- in tree modules (inv_mpu6050_i2c.ko ...)
│   ├── updates/ssd1315_oled.ko     <- out of tree driver
│   └── modules.dep, modules.alias ...
├── etc/
│   ├── inittab  fstab  profile  passwd  group  hostname
│   └── init.d/
│       ├── rcS       <- boot
│       ├── rcK       <- shutdown
│       └── S70oled   <- loads the OLED and IMU drivers
├── dev/ proc/ sys/ tmp/ run/ mnt/ root/
```
