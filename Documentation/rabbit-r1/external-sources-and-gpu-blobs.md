# Where to obtain the excluded sources and GPU blobs

This partial release does not bundle the Rabbit panel/WLAN implementations or proprietary GPU binaries. The links below identify Rabbit's public originals. Apply our [optional compatibility patches](../../patches/rabbit-r1/README.md) to prepare the Linux integration locally; the vendor originals remain separate downloads.

## Panel source

Rabbit publishes its kernel sources in [rabbit-hmi-oss/android_kernel_rabbit_mt6765](https://github.com/rabbit-hmi-oss/android_kernel_rabbit_mt6765).

The panel reference used during development is [ili9883_boe_mipi_hd.c at the pinned commit](https://github.com/rabbit-hmi-oss/android_kernel_rabbit_mt6765/blob/8167c8c1087f057d2ef302fc93b47554291687ec/drivers/misc/mediatek/lcm/ili9883_boe_mipi_hd/ili9883_boe_mipi_hd.c):

- Commit: `8167c8c1087f057d2ef302fc93b47554291687ec`.
- Path: `drivers/misc/mediatek/lcm/ili9883_boe_mipi_hd/ili9883_boe_mipi_hd.c`.
- SHA256: `9fd1eb026a320b1a63fd78cc3739663e0f2fb28db3ec090f0d46a2b0a8d69fc9`.

This is the stock reference, not the omitted Linux 7.1 Rabbit panel port. Copying it into this tree is not a complete integration procedure. The licensing-context question remains deferred as documented in [redistribution review](redistribution-review.md).

## Wi-Fi source

Rabbit publishes its module sources in [rabbit-hmi-oss/android_kernel_modules_rabbit_mt6765](https://github.com/rabbit-hmi-oss/android_kernel_modules_rabbit_mt6765).

Use commit `0afdf059a0dfa17becc4bbe47363280a42224c2f`:

- [gen4m WLAN driver](https://github.com/rabbit-hmi-oss/android_kernel_modules_rabbit_mt6765/tree/0afdf059a0dfa17becc4bbe47363280a42224c2f/connectivity/wlan/core/gen4m).
- [Wi-Fi adapter](https://github.com/rabbit-hmi-oss/android_kernel_modules_rabbit_mt6765/tree/0afdf059a0dfa17becc4bbe47363280a42224c2f/connectivity/wlan/adaptor).

The four previously reviewed inputs are `include/wsys_cmd_handler_fw.h`, `common/debug.c`, `mgmt/reg_rule.c`, and `mgmt/tkip_mic.c`, relative to `connectivity/wlan/core/gen4m/`. The public stock source still needs kernel-version compatibility work; this partial tree supplies no WLAN build recipe. Wi-Fi licensing review remains deferred.

## GPU firmware and userspace libraries

Get the stock image from **Rabbit's official [firmware releases](https://github.com/rabbit-hmi-oss/firmware/releases)**. The development reference was [rabbitOS v0.8.293](https://github.com/rabbit-hmi-oss/firmware/releases/tag/v0.8.293), with the asset [rabbit_OS_v0.8.293.zip](https://github.com/rabbit-hmi-oss/firmware/releases/download/v0.8.293/rabbit_OS_v0.8.293.zip).

Extract the archive offline, then unpack `super.img` to obtain `vendor_a.img`. For this reference image the sequence is Android sparse-image conversion with `simg2img`, dynamic-partition extraction with `lpunpack`, then read-only filesystem extraction using `debugfs` or 7-Zip. No flashing or device access is needed to obtain the files.

| File in the stock vendor filesystem | Purpose |
| --- | --- |
| `/firmware/rgx.fw` | PowerVR GPU firmware; appears as `/vendor/firmware/rgx.fw` on Android |
| `/firmware/rgx.sh` | Companion shader bundle; despite its suffix, this is binary data, **not a shell script** |
| `/lib64/libsrv_um.so` | PowerVR userspace services library |
| `/lib64/egl/libGLESv2_mtk.so` | Stock GLES library |
| `/lib64/egl/` and `/lib64/` | Related EGL/GLES libraries and dependencies, including `libIMGegl.so`, `libglslcompiler.so`, `libusc.so`, and MediaTek support libraries |
| `/lib64/hw/gralloc.rogue.so` | Stock graphics allocator |

The extracted firmware reference is 114,688 bytes, SHA256 `26450212605f42cecb11b254c7c1a27d628db361ba735436b4a9c79bd22101e3`. The stock userspace DDK identifies itself as `1.13@5776728`, matching the vendor kernel-driver family used here. Keep the firmware and dependent libraries from the same reference image; arbitrary newer firmware is not verified compatible.

These are Android/Bionic binaries, not drop-in glibc/Mesa libraries. They need a compatible loader, dependencies, allocator and kernel ABI. The stock firmware is not compatible merely by renaming it for the upstream open PowerVR driver. Kernel source alone does not provide a complete accelerated desktop stack.

The blobs remain outside this repository. This document points to their official distribution and records the tested reference; it does not grant new redistribution rights for them.

### Python extraction tool

After unpacking the raw partition images, [extract-gpu.py](../../patches/rabbit-r1/extract-gpu.py) can extract the pinned GPU firmware/libraries from `vendor_a.img` and optional Android runtime dependencies from `system_a.img`. See [usage and safeguards](../../patches/rabbit-r1/README.md#optional-gpu-blob-extraction-in-python). The script contains no proprietary blob payload: it reads your local official image, verifies file hashes, and writes a new private directory outside Git. It neither downloads firmware nor installs or executes extracted files.
