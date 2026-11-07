#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026 Yassine El H. @yassineflag
# scripts/build-oled-test.sh: cross build tools/oled-test and install it into the rootfs
set -euo pipefail
STEP=OLED-TEST
# shellcheck source=env.sh
. "$(dirname "$0")/env.sh"

[ -x "${ROOTFS_OUT}/bin/busybox" ] || die "rootfs missing, run 'make rootfs' first"

make -C "${TOP}/tools/oled-test" CROSS_COMPILE="${CROSS_COMPILE}"
install -D -m 755 "${TOP}/tools/oled-test/oled-test" "${ROOTFS_OUT}/usr/bin/oled-test"
log "Done: oled-test -> ${ROOTFS_OUT}/usr/bin/oled-test"
