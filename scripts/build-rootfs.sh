#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026 Yassine El H. @yassineflag
# scripts/build-rootfs.sh: build a minimal static BusyBox root filesystem
#
# The rootfs is rebuilt from scratch on every run so stale files never leak
# into the image. File ownership is fixed later, inside fakeroot, by
# scripts/make-image.sh: nothing here needs root.
set -euo pipefail
STEP=ROOTFS
# shellcheck source=env.sh
. "$(dirname "$0")/env.sh"

need_cmd wget sha256sum tar "${CROSS_COMPILE}gcc"

# ---------------------------------------------------------------------------
# 1. Fetch BusyBox (checksum verified)
# ---------------------------------------------------------------------------
TARBALL="${SRC_DIR}/busybox-${BUSYBOX_VERSION}.tar.bz2"
mkdir -p "${SRC_DIR}"
if [ ! -d "${BUSYBOX_SRC}" ]; then
    if [ ! -f "${TARBALL}" ]; then
        log "Downloading BusyBox ${BUSYBOX_VERSION}"
        # .part + mv: a failed download never leaves a truncated tarball behind
        wget -q -O "${TARBALL}.part" \
            "https://busybox.net/downloads/busybox-${BUSYBOX_VERSION}.tar.bz2" || \
            { rm -f "${TARBALL}.part"; die "download failed"; }
        mv "${TARBALL}.part" "${TARBALL}"
    fi
    echo "${BUSYBOX_SHA256}  ${TARBALL}" | sha256sum -c --quiet - || \
        { rm -f "${TARBALL}"; die "BusyBox checksum mismatch"; }
    tar -xf "${TARBALL}" -C "${SRC_DIR}"
    mv "${SRC_DIR}/busybox-${BUSYBOX_VERSION}" "${BUSYBOX_SRC}"
fi

bmake() { make -C "${BUSYBOX_SRC}" ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" "$@"; }

# ---------------------------------------------------------------------------
# 2. Configure and build BusyBox
# ---------------------------------------------------------------------------
if [ ! -f "${BUSYBOX_SRC}/.config" ]; then
    log "Configuring BusyBox ${BUSYBOX_VERSION} (defconfig, static)"
    bmake defconfig
    # Static: no shared libraries needed on the target
    # TC: the tc applet does not build against kernel headers >= 6.8
    #     (CBQ qdisc removed), which Ubuntu 24.04 cross toolchains ship
    sed -i \
        -e 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' \
        -e 's/^CONFIG_TC=y/# CONFIG_TC is not set/' \
        "${BUSYBOX_SRC}/.config"
    bmake oldconfig < /dev/null
fi

log "Building BusyBox (jobs=${JOBS})"
bmake -j"${JOBS}"

# ---------------------------------------------------------------------------
# 3. Assemble the tree
# ---------------------------------------------------------------------------
log "Assembling ${ROOTFS_OUT}"
rm -rf "${ROOTFS_OUT}"
bmake CONFIG_PREFIX="${ROOTFS_OUT}" install > /dev/null

mkdir -p "${ROOTFS_OUT}"/{dev,proc,sys,tmp,run,mnt,root,etc/init.d,usr/bin,usr/sbin,lib/modules}
chmod 1777 "${ROOTFS_OUT}/tmp"

# BusyBox init. The shell runs on ttyS0, not on /dev/console, so it gets a
# controlling tty: job control and Ctrl+C work (needed by oled-test 3 and 4)
cat > "${ROOTFS_OUT}/etc/inittab" <<'EOF'
::sysinit:/etc/init.d/rcS
ttyS0::askfirst:-/bin/sh
::ctrlaltdel:/sbin/reboot
::shutdown:/etc/init.d/rcK
::shutdown:/bin/umount -a -r
EOF

# Boot: mount pseudo filesystems, start mdev, run S??* in order
cat > "${ROOTFS_OUT}/etc/init.d/rcS" <<'EOF'
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
# devtmpfs is already mounted on /dev by the kernel (CONFIG_DEVTMPFS_MOUNT)
mountpoint -q /dev || mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/pts
mount -a
hostname -F /etc/hostname
echo /sbin/mdev > /proc/sys/kernel/hotplug
mdev -s

for s in /etc/init.d/S??*; do
    [ -x "$s" ] && "$s" start
done
EOF

# Shutdown: stop services in reverse order
cat > "${ROOTFS_OUT}/etc/init.d/rcK" <<'EOF'
#!/bin/sh
for s in $(ls -r /etc/init.d/S??* 2>/dev/null); do
    [ -x "$s" ] && "$s" stop
done
EOF
chmod 755 "${ROOTFS_OUT}/etc/init.d/rcS" "${ROOTFS_OUT}/etc/init.d/rcK"

# proc, sysfs and devtmpfs are mounted by rcS / the kernel, not by mount -a
cat > "${ROOTFS_OUT}/etc/fstab" <<'EOF'
tmpfs   /tmp      tmpfs   mode=1777  0 0
tmpfs   /run      tmpfs   mode=0755  0 0
devpts  /dev/pts  devpts  defaults   0 0
EOF

cat > "${ROOTFS_OUT}/etc/profile" <<'EOF'
export PATH=/sbin:/usr/sbin:/bin:/usr/bin
export PS1='\u@\h:\w\$ '
EOF

echo "root:x:0:0:root:/root:/bin/sh" > "${ROOTFS_OUT}/etc/passwd"
echo "root:x:0:" > "${ROOTFS_OUT}/etc/group"
echo "bbb-oled" > "${ROOTFS_OUT}/etc/hostname"

install -m 755 "${TOP}/rootfs/etc/init.d/S70oled" "${ROOTFS_OUT}/etc/init.d/S70oled"

# ---------------------------------------------------------------------------
# 4. In tree kernel modules (out of tree ones come from build-modules.sh)
# ---------------------------------------------------------------------------
if [ -d "${MODULES_OUT}/lib/modules" ]; then
    log "Installing kernel modules"
    cp -a "${MODULES_OUT}/lib/modules/." "${ROOTFS_OUT}/lib/modules/"
    # modules_install leaves build/ and source/ symlinks to the host tree
    rm -f "${ROOTFS_OUT}"/lib/modules/*/build "${ROOTFS_OUT}"/lib/modules/*/source
else
    log "WARNING: ${MODULES_OUT} missing, run 'make kernel' for in tree modules"
fi

log "Done: ${ROOTFS_OUT}"
