# build-rootfs.sh: Deep Dive

A detailed walkthrough of every step in `scripts/build-rootfs.sh`, which assembles a minimal Linux root filesystem using **BusyBox** for the BeagleBone Black.

---

## What is a RootFS?

The root filesystem is everything the kernel mounts at `/` after boot. It contains all the userspace binaries, libraries, configuration files, and device nodes. On an embedded system this is typically kept as small as possible. We use **BusyBox** to get an entire shell + core Unix utilities in a single ~1 MB static binary.

---

## Step-by-Step Breakdown

### 1. Safety flags and path setup

```bash
set -euo pipefail
TOP="$(cd "$(dirname "$0")/.." && pwd)"
```

| Flag | Effect |
|------|--------|
| `-e` | Exit immediately if any command fails |
| `-u` | Treat unset variables as errors |
| `-o pipefail` | A pipe fails if **any** command in it fails (not just the last) |

`TOP` resolves to the project root regardless of where you call the script from, making all paths relative to the repo.

---

### 2. BusyBox configuration

```bash
make -C "${SRC}" ARCH=arm CROSS_COMPILE="${CROSS_COMPILE}" defconfig
```

Runs BusyBox's built-in `defconfig` target, which generates a `.config` file with a sensible set of enabled applets (shell, coreutils, networking tools, init, etc.) pre-selected for the target architecture.

`ARCH=arm` and `CROSS_COMPILE=arm-linux-gnueabihf-` tell BusyBox's Kconfig system we're building for 32-bit ARM with the hard-float ABI.

---

### 3. Enable static linking

```bash
sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' "${SRC}/.config"
make -C "${SRC}" ARCH=arm CROSS_COMPILE="${CROSS_COMPILE}" oldconfig -s
```

**Why static?**  
A dynamically linked BusyBox requires `glibc` (or `musl`) shared libraries to be present in the rootfs under `/lib`. Building statically links everything into the single `busybox` binary, no shared libs needed, no `ld.so`, simpler rootfs.

The `sed` patches the `.config` directly (the quickest way to flip a single option), then `oldconfig` re-validates the configuration to resolve any dependencies that may have changed.

---

### 4. Cross-compile BusyBox

```bash
make -C "${SRC}" ARCH=arm CROSS_COMPILE="${CROSS_COMPILE}" -j"$(nproc)"
```

Compiles every enabled applet and links them all into a single `busybox` binary. `-j$(nproc)` parallelises the build across all available CPU cores. The output is `src/busybox/busybox`, an ARM ELF binary.

---

### 5. Install BusyBox into the rootfs

```bash
make -C "${SRC}" ARCH=arm CROSS_COMPILE="${CROSS_COMPILE}" \
    CONFIG_PREFIX="${ROOTFS}" install
```

BusyBox's install target creates the following layout under `output/rootfs/`:

```
output/rootfs/
├── bin/
│   ├── busybox       <- the real binary
│   ├── sh -> busybox <- symlink
│   ├── ls -> busybox <- symlink
│   └── ...           <- hundreds of symlinks
├── sbin/
│   └── ...
└── usr/
    └── ...
```

Every applet (ls, cat, sh, mount, modprobe ...) is just a **symlink** pointing back to the same `busybox` binary. BusyBox reads `argv[0]` at runtime to know which applet to run.

---

### 6. Create the essential directory skeleton

```bash
mkdir -p "${ROOTFS}"/{proc,sys,dev,tmp,run,mnt,boot,etc/init.d,usr/bin,usr/sbin,lib/modules}
```

These are **mount points and directories required by Linux** at runtime:

| Directory | Purpose |
|-----------|---------|
| `/proc` | Virtual FS, kernel exposes process info, CPU info, etc. |
| `/sys` | Virtual FS, kernel exposes device/driver model (sysfs) |
| `/dev` | Device nodes (tty, null, zero, i2c, ...) |
| `/tmp` | Temporary files (mounted as tmpfs, lives in RAM) |
| `/run` | Runtime data (PIDs, sockets) |
| `/mnt` | Convenient manual mount point |
| `/boot` | Placeholder (kernel + DTB live in FAT partition) |
| `/etc/init.d` | SysV init scripts |
| `/usr/bin` | Userspace binaries (oled-test will go here) |
| `/lib/modules` | Kernel modules (`ssd1315_oled.ko` and `inv_mpu6050_i2c.ko` go here) |

Without these directories, `mount`, `modprobe`, and most init scripts fail immediately.

---

### 7. Create `/etc/inittab`

```bash
cat > "${ROOTFS}/etc/inittab" << 'EOF'
::sysinit:/etc/init.d/rcS
::askfirst:-/bin/sh
::ctrlaltdel:/sbin/reboot
::shutdown:/sbin/swapoff -a
::shutdown:/bin/umount -a -r
EOF
```

`inittab` is the configuration file for BusyBox `init`, the first userspace process (PID 1) started by the kernel. Each line is: `id:runlevels:action:process`

| Line | Meaning |
|------|---------|
| `::sysinit:/etc/init.d/rcS` | Run `rcS` once at startup before anything else |
| `::askfirst:-/bin/sh` | Give a login shell on the console (the `-` prefix means login shell) |
| `::ctrlaltdel:/sbin/reboot` | Ctrl+Alt+Del triggers a clean reboot |
| `::shutdown:...` | On shutdown, unmount filesystems cleanly to avoid corruption |

---

### 8. Create `/etc/init.d/rcS`

```bash
cat > "${ROOTFS}/etc/init.d/rcS" << 'EOF'
#!/bin/sh
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev 2>/dev/null || mdev -s
echo /sbin/mdev > /proc/sys/kernel/hotplug

for script in /etc/init.d/S??*; do
    [ -x "$script" ] && "$script" start
done
EOF
```

`rcS` is the **system initialisation script** called by `init` at boot. It runs in order:

1. **`mount -t proc none /proc`**: the kernel needs `/proc` to expose process/system information. Without this, commands like `ps`, `free`, `cat /proc/cpuinfo` are broken.

2. **`mount -t sysfs none /sys`**: mounts the sysfs virtual filesystem, required for `udev`/`mdev` to enumerate devices and for any driver interaction via sysfs attributes.

3. **`mount -t devtmpfs none /dev || mdev -s`**: populates `/dev` with device nodes.
   - `devtmpfs`: the kernel itself creates device nodes automatically as drivers register (preferred).
   - `mdev -s` (fallback): BusyBox's lightweight `udev` replacement; scans `/sys` and creates nodes manually if `devtmpfs` isn't available.

4. **`echo /sbin/mdev > /proc/sys/kernel/hotplug`**: registers `mdev` as the hotplug handler so that devices plugged in after boot (USB, I2C devices loaded by modules) also get their `/dev` nodes created.

5. **`for script in /etc/init.d/S??*`**: runs all init scripts matching `S` + two digits + anything, in **alphabetical order**. This is the standard SysV convention:
   - `S70oled` loads `ssd1315_oled` and `inv_mpu6050_i2c` kernel modules
   - Higher numbers run later; you control startup order by choosing the number.

---

### 9. Create `/etc/fstab`

```bash
cat > "${ROOTFS}/etc/fstab" << 'EOF'
proc      /proc  proc      defaults  0 0
sysfs     /sys   sysfs     defaults  0 0
devtmpfs  /dev   devtmpfs  defaults  0 0
tmpfs     /tmp   tmpfs     defaults  0 0
EOF
```

`fstab` tells `mount -a` (and some init systems) which filesystems to mount. Even though `rcS` mounts proc/sys/dev manually, having them in `fstab` ensures tools like `df` and `findmnt` show them correctly. `/tmp` as `tmpfs` means temp files live in RAM (fast, and disappear on reboot).

---

### 10. Set hostname

```bash
echo "bbb-oled" > "${ROOTFS}/etc/hostname"
```

The kernel reads this at boot and sets the system hostname. Shows up in the shell prompt and in `hostname` command output.

---

### 11. Install the OLED init script

```bash
cp "${TOP}/rootfs/etc/init.d/S70oled" "${ROOTFS}/etc/init.d/S70oled"
chmod +x "${ROOTFS}/etc/init.d/S70oled"
```

Copies our custom `S70oled` script into the rootfs so it gets picked up by `rcS`'s `for` loop on boot. The `chmod +x` is critical: `rcS` checks `[ -x "$script" ]` before running it.

---

### 12. Install kernel modules

```bash
if [ -d "${MODULES_OUT}" ]; then
    cp -a "${MODULES_OUT}/lib/modules" "${ROOTFS}/lib/"
    KVER=$(ls "${MODULES_OUT}/lib/modules/" | head -1)
    depmod -b "${ROOTFS}" "${KVER}" 2>/dev/null || true
fi
```

- **`cp -a`**: copies the entire module tree preserving symlinks and permissions. `modules_install` (run by `build-kernel.sh`) creates:
  ```
  output/modules/lib/modules/6.6.21-.../{kernel/,extra/,modules.dep,...}
  ```

- **`depmod -b "${ROOTFS}" "${KVER}"`**: generates `modules.dep` and `modules.alias` inside the rootfs. These files are what `modprobe` reads to resolve module dependencies. Without running `depmod`, `modprobe ssd1315_oled` would fail even if the `.ko` file is present, because `modprobe` (unlike `insmod`) relies on the dependency database.

> **Note:** The `|| true` prevents the script from failing if `depmod` isn't installed on the host; `build-modules.sh` also runs `depmod` after installing the `.ko`, so this is a safety net.

---

## Output

After the script completes, `output/rootfs/` is a fully populated directory tree ready to be packed into an ext4 partition:

```
output/rootfs/
├── bin/          <- BusyBox applets (sh, ls, cat, mount, modprobe...)
├── sbin/
├── usr/bin/      <- oled-test (installed by build-oled-test.sh)
├── lib/modules/  <- ssd1315_oled.ko + inv_mpu6050_i2c.ko + modules.dep
├── etc/
│   ├── inittab
│   ├── fstab
│   ├── hostname
│   └── init.d/
│       ├── rcS       <- system init
│       └── S70oled   <- loads OLED driver
├── proc/  sys/  dev/  tmp/  run/  mnt/
```
