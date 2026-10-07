#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Generate the historical embedded init; never execute it on the host or device.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT=${1:?Usage: prepare-init.sh KERNEL_OUTPUT_DIRECTORY}
OUT=$(realpath -m -- "$OUT")
TARGET=$(realpath -m -- "$OUT/usr/r1-initramfs/init")
case "$TARGET/" in "$ROOT/"*) echo 'Generated init must stay outside the source tree' >&2; exit 2 ;; esac
mkdir -p "$OUT/usr/r1-initramfs"
"${CROSS_COMPILE:-aarch64-linux-gnu-}gcc" -static -O2 -s \
    -o "$OUT/usr/r1-initramfs/init" "$ROOT/tools/rabbit-r1/minit.c"
expected=d4f101fa165966f533a823b7f70c4c26f0dff0ab1dd3c17b4e1be92bb07d24be
actual=$(sha256sum "$OUT/usr/r1-initramfs/init" | cut -d' ' -f1)
[ "$actual" = "$expected" ] || {
    echo 'Embedded init differs from reference; check compiler, libc and binutils versions.' >&2
    exit 1
}
# The reference config preserves source mtimes inside the archive.
touch -d '2026-08-27 13:32:43 UTC' "$OUT/usr/r1-initramfs/init"
echo 'Embedded init matches the recorded original executable.'
