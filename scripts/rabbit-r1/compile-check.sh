#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Compile-only validation. No device connection, installation or flash operation.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT=${1:?Usage: compile-check.sh NEW_ABSOLUTE_OUTPUT_DIRECTORY}
case "$OUT" in /*) ;; *) echo 'Output must be an absolute path' >&2; exit 2 ;; esac
OUT=$(realpath -m -- "$OUT")
case "$OUT/" in "$ROOT/"*) echo 'Keep build outputs outside the source tree' >&2; exit 2 ;; esac
[ ! -e "$OUT" ] && [ ! -L "$OUT" ] || { echo 'Refusing an existing output directory' >&2; exit 2; }
mkdir -p "$OUT"
cp "$ROOT/Documentation/rabbit-r1/kernel49.config" "$OUT/.config"
"$ROOT/scripts/config" --file "$OUT/.config" --set-str INITRAMFS_SOURCE ''
"$ROOT/scripts/config" --file "$OUT/.config" --disable DRM_PANEL_RABBIT_R1
export KBUILD_BUILD_USER=spectre KBUILD_BUILD_HOST=builder
export KBUILD_BUILD_TIMESTAMP='2026-09-28 09:50:35 UTC' KBUILD_BUILD_VERSION=49
make -C "$ROOT" O="$OUT" ARCH=arm64 CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}" LOCALVERSION=+ olddefconfig
make -C "$ROOT" O="$OUT" ARCH=arm64 CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}" LOCALVERSION=+ -j"${JOBS:-4}" Image.gz
printf '%s\n' 'Compile-only image: NOT an installed-kernel reproduction; do not flash.'
