#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026 Yassine El H. @yassineflag
# scripts/build-rootfs.sh: Build a minimal BusyBox rootfs
set -euo pipefail

TOP="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${TOP}/src/busybox"
ROOTFS="${TOP}/output/rootfs"
CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabihf-}"
MODULES_OUT="${TOP}/output/modules"

echo "[ROOTFS] Configuring BusyBox..."
make -C "${SRC}" ARCH=arm CROSS_COMPILE="${CROSS_COMPILE}" defconfig

# Enable static build so we don't need shared libs on target
sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' "${SRC}/.config"
make -C "${SRC}" ARCH=arm CROSS_COMPILE="${CROSS_COMPILE}" oldconfig -s

echo "[ROOTFS] Building BusyBox (static)..."
make -C "${SRC}" ARCH=arm CROSS_COMPILE="${CROSS_COMPILE}" -j"$(nproc)"

echo "[ROOTFS] Installing BusyBox to ${ROOTFS}..."
make -C "${SRC}" ARCH=arm CROSS_COMPILE="${CROSS_COMPILE}" \
    CONFIG_PREFIX="${ROOTFS}" install

# Create essential directory structure
mkdir -p "${ROOTFS}"/{proc,sys,dev,tmp,run,mnt,boot,etc/init.d,usr/bin,usr/sbin,lib/modules}

# Minimal /etc/inittab
cat > "${ROOTFS}/etc/inittab" <<'EOF'
::sysinit:/etc/init.d/rcS
::askfirst:-/bin/sh
::ctrlaltdel:/sbin/reboot
::shutdown:/sbin/swapoff -a
::shutdown:/bin/umount -a -r
EOF

# /etc/init.d/rcS — run all S* scripts
cat > "${ROOTFS}/etc/init.d/rcS" <<'EOF'
#!/bin/sh
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev 2>/dev/null || mdev -s
echo /sbin/mdev > /proc/sys/kernel/hotplug

# Run all S* scripts
for script in /etc/init.d/S??*; do
    [ -x "$script" ] && "$script" start
done
EOF
chmod +x "${ROOTFS}/etc/init.d/rcS"

# /etc/fstab
cat > "${ROOTFS}/etc/fstab" <<'EOF'
proc      /proc  proc      defaults  0 0
sysfs     /sys   sysfs     defaults  0 0
devtmpfs  /dev   devtmpfs  defaults  0 0
tmpfs     /tmp   tmpfs     defaults  0 0
EOF

# hostname
echo "bbb-oled" > "${ROOTFS}/etc/hostname"

# Copy our SSD1315 init script
cp "${TOP}/rootfs/etc/init.d/S70oled" "${ROOTFS}/etc/init.d/S70oled"
chmod +x "${ROOTFS}/etc/init.d/S70oled"

# Install kernel modules into rootfs
if [ -d "${MODULES_OUT}" ]; then
    echo "[ROOTFS] Installing kernel modules..."
    cp -a "${MODULES_OUT}/lib/modules" "${ROOTFS}/lib/"
    # Run depmod using target kernel version
    KVER=$(ls "${MODULES_OUT}/lib/modules/" | head -1)
    if command -v depmod >/dev/null 2>&1; then
        depmod -b "${ROOTFS}" "${KVER}" 2>/dev/null || true
    fi
fi

echo "[ROOTFS] Done — ${ROOTFS}"
