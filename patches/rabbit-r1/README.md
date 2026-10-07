# Optional Rabbit R1 compatibility patches

These patches let you combine this partial source release with sources downloaded separately from Rabbit. They contain our integration work and the context needed for the compatibility edits. They do **not** bundle the original panel command table, the complete vendor Wi-Fi driver, GPU blobs, or private device calibration.

## What each patch targets

| Patch | Base | Changes |
| --- | --- | --- |
| `0001-rabbit-wlan-linux71.patch` | Rabbit module repository, commit `0afdf059a0dfa17becc4bbe47363280a42224c2f` | 20 existing WLAN/adapter files: Linux API compatibility and R1 integration. Apply at the module repository root with `git apply`. |
| `0002-spectre-optional-integration.patch` | Spectre partial source commit `ffd6fc4ecb6797bb8c29245e0de19f28c35fd576` | Adds our panel driver/table generator, local Wi-Fi compatibility headers and authored build recipes; restores the optional build entry points. This patch targets Spectre's tree, **not** Rabbit's Android kernel. |

The panel port is a separate Linux DRM driver, not an in-place patch to Rabbit's Android LCM file. Its generator reads the original table from your downloaded Rabbit source when you build. The four deferred WLAN files are not modified by either patch and remain external inputs. Existing file notices are retained. Original project changes use GPL-2.0-only; vendor context retains its existing terms. Publishing these patches does not settle the deferred licensing questions for complete vendor inputs.

## Obtain the exact sources

Download or clone:

- [Rabbit kernel source](https://github.com/rabbit-hmi-oss/android_kernel_rabbit_mt6765), commit `8167c8c1087f057d2ef302fc93b47554291687ec`.
- [Rabbit module source](https://github.com/rabbit-hmi-oss/android_kernel_modules_rabbit_mt6765), commit `0afdf059a0dfa17becc4bbe47363280a42224c2f`.
- This Spectre repository, with full history (the preparation tool reads its pinned base commit).

For GPU binaries and exact stock file locations, see [the official-source guide](../../Documentation/rabbit-r1/external-sources-and-gpu-blobs.md).

## Prepare an isolated source tree

From this repository, run:

```sh
python3 patches/rabbit-r1/prepare.py \
  --rabbit-kernel /absolute/path/android_kernel_rabbit_mt6765 \
  --rabbit-modules /absolute/path/android_kernel_modules_rabbit_mt6765 \
  --output /absolute/path/r1-optional-source
```

The output must not exist and must be outside all three input repositories. The tool operates offline. It verifies the two patch hashes, the panel reference hash and all 220 selected vendor-file hashes; applies both patches without fuzzy matching; then checks all prepared vendor/integration hashes against `manifest.json`. It changes none of the input repositories and does not install, load, flash or publish anything. It needs Python 3, Git and tar. Keep the prepared tree and build outputs outside the publication checkout.

A failed preparation can leave an incomplete output directory; inspect/remove that directory yourself or choose a new output path before retrying. Supplying different upstream versions is intentionally rejected.

## Build

Use the toolchains and prerequisites in [build.md](../../Documentation/rabbit-r1/build.md). The commands below apply to the **prepared optional tree**, not the unmodified partial release.

```sh
R1_PANEL_SOURCE=/absolute/path/android_kernel_rabbit_mt6765/drivers/misc/mediatek/lcm/ili9883_boe_mipi_hd/ili9883_boe_mipi_hd.c \
  /absolute/path/r1-optional-source/scripts/rabbit-r1/build.sh \
  /absolute/path/r1-kernel-output

python3 /absolute/path/r1-optional-source/scripts/rabbit-r1/build-external.py \
  wlan /absolute/path/r1-kernel-output /absolute/path/r1-wlan-output \
  --wlan-private-dir /absolute/path/android_kernel_modules_rabbit_mt6765/connectivity/wlan/core/gen4m \
  --jobs 4
```

Both output directories must be new. The WLAN command builds BTIF, WMT, the Wi-Fi adapter and WLAN together. Its four separately supplied inputs are hash checked. The generated panel table stays in the kernel build output. Firmware, NVRAM/calibration, runtime setup and device deployment are separate; these patches alone do not provision a working device. Do not upload generated inputs or binary artifacts as part of this source patch set.

## Validation

See [validation.json](validation.json) for the recorded application, source-identity and build checks. Hardware behavior is supported only by the prior scoped reference evidence; this publication performs no new device or radio test.

## Optional GPU blob extraction in Python

The GPU kernel source is already included. To obtain the separate stock firmware/libraries locally, use `extract-gpu.py` after extracting Rabbit's official v0.8.293 archive into raw `vendor_a.img` and, optionally, `system_a.img` partitions:

```sh
python3 patches/rabbit-r1/extract-gpu.py \
  --vendor-image /absolute/path/vendor_a.img \
  --system-image /absolute/path/system_a.img \
  --output /absolute/private/non-git/path/r1-gpu-stock
```

Omit `--system-image` for vendor files only. The output must be new, its parent must exist, and it must be outside Git checkouts. `debugfs` from e2fsprogs is required. No downloads or package installation happen automatically. See the [stock image extraction guide](../../Documentation/rabbit-r1/external-sources-and-gpu-blobs.md) for obtaining the partition images; this script does not accept the ZIP or sparse `super.img` directly.

The tool uses read-only filesystem extraction, checks each file against `gpu-blobs.json`, writes private non-executable files, and removes its newly created partial output on failure. It never mounts images, executes extracted libraries, installs files, or touches a device. It extracts only the listed GPU files and their stock Android runtime dependencies, not personal partitions or calibration. Keep the output out of Git.

Validation extracted and verified all 80 vendor/system entries from the reference stock images. Two stock library hashes differ from the earlier development runtime manifest; this manifest intentionally pins the unmodified stock versions. The output preserves partition-relative layout under `vendor/` and `system/`, including the system image's inner `system/` directory. It is a collection of original files, **not an installed or configured GPU runtime**; loader integration and any development-specific adaptations remain separate.
