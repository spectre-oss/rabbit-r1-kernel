# Build validation

> Partial source snapshot: panel and WLAN are omitted. Prior full-build evidence below is historical. See [current publication scope](partial-release.md).

The saved target configuration was built with AArch64 GCC 16.1.0 and GNU binutils 2.47. Use an isolated output directory and preserve the original development checkout.

## Compile-only check

```
JOBS=6 scripts/rabbit-r1/compile-check.sh /absolute/path/to/new-output
```

This deliberately sets an empty built-in initramfs to separate compilation from complete reproduction. It also normalizes the build user, host and timestamp to avoid embedding workstation identity. Therefore its kernel image is **not a byte-identical reproduction and must not be flashed as the matched release**.

The finalized full-build wrapper passed from a new output directory on October 7, 2026: Image.gz and all 11 configured in-tree modules built without compiler warnings or errors. Its configuration exactly matches the saved build-49 configuration. Scoped external-module and live-device correspondence checks have since passed; final complete-release checks remain pending.

The original embedded init has now been reconstructed and its binary hash verified with the pinned compiler/libc inputs. Run `scripts/rabbit-r1/prepare-init.sh OUTPUT` to generate it in a build output directory.

The panel module requires a separately supplied vendor table source whose redistribution rights are unresolved. Run `python3 scripts/rabbit-r1/prepare-panel.py VENDOR_SOURCE OUTPUT` only with an input you are authorized to use. The generated header stays under `OUTPUT/include/generated/` and must not be committed or distributed.

The check validates compilation of the selected kernel source and configuration. It does not validate boot, device-tree packaging, external modules, firmware, hardware behavior, or release completeness.

The original DTB packaging and runtime overlays must be reproduced separately. A freshly generated board DTB is not an established replacement for the installed bootloader-compatible DTB. No flashing script is supplied during release preparation.

## Full source-build recipe

```
R1_PANEL_SOURCE=/path/to/authorized/vendor/panel-source.c \
  JOBS=6 scripts/rabbit-r1/build.sh /absolute/path/to/new-output
```

This generates the byte-verified embedded init and private panel header, then builds the kernel and configured in-tree modules. `init-toolchain-inputs.json` records the static-link input hashes used for the verified init reproduction. Rebuilding with other libc/compiler versions is rejected when the embedded init hash differs.

The build uses public identity metadata (`spectre@builder`) while retaining the recorded kernel release and build timestamp needed by existing service guards. This intentional metadata change means public-source builds are not claimed byte-identical to a device binary embedding the original workstation identity. Main-image source correspondence was verified separately against the recorded device image; see `kernel-correspondence.json`.

External modules (Wi-Fi, Bluetooth, GPU, modem, camera and other device helpers) require their separately audited source inputs and recipes. Their inclusion is not complete. The build output contains the private panel dependency and is not a distributable release image.

## Correspondence limits observed locally

The saved configuration disables `CONFIG_MODVERSIONS`. Matching `Module.symvers` therefore confirms the exported-symbol inventory, not CRC-based ABI compatibility; it does not replace rebuilding and checking external modules.

The original development VDSO and the fresh build have identical allocated code/data sections except the GNU build-ID note. Their Linux notes also match. Debug/build paths can affect build IDs even when loaded code agrees. The public identity metadata, include-path spelling and initramfs metadata remain additional reasons not to claim byte identity with the saved kernel. The full-build wrapper explicitly maps initramfs UID/GID to root through make-command overrides, leaving the saved configuration unchanged. This removes build-account ownership from the archive. Cross-host reproducibility of the complete build remains pending.

## External modules: implemented groups

After preparing the target kernel output directory, build each group into a separate new directory:

```sh
python3 scripts/rabbit-r1/build-external.py audio \
  /absolute/kernel-output /absolute/new-audio-output --jobs 8
python3 scripts/rabbit-r1/build-external.py core-helpers \
  /absolute/kernel-output /absolute/new-core-output --jobs 4
```

`audio` builds the five mutually matched ALSA/R1 modules. `core-helpers` builds the MT6359 AUXADC and shared codec-owner modules from source already present in the kernel tree. The recipe checks the target configuration/release, records copied input hashes, and applies audio configuration flags only to this external build. It never installs or loads modules.

Compiler source/debug paths are normalized to `/usr/src/linux`, `/usr/src/linux-build` and `/usr/src/rabbit-r1/<group>` to avoid embedding the maintainer's workspace paths. The original ALSA artifacts include original source-root strings; correspondence checks account for those exact path substitutions and their relocation-offset changes, as well as build IDs. They do not ignore arbitrary code/data differences. Cross-host/toolchain reproducibility remains a separate requirement.

The selected hardware groups and their runtime dependencies are recorded in `hardware.md`. A source build does not package a complete device installation; panel/WLAN inputs and firmware dependencies remain separately documented.

The five audio module files were also built in two separate clean output directories using the same local toolchain; all five SHA256 hashes matched byte for byte. This verifies output-directory reproducibility for that group, not cross-host reproducibility of the whole kernel release. Configured private-term scans found no matches in the seven normalized audio/core module artifacts.

## Board helpers and private camera input

```sh
python3 scripts/rabbit-r1/build-external.py board \
  /absolute/kernel-output /absolute/new-board-output --jobs 8
python3 scripts/rabbit-r1/build-external.py camera \
  /absolute/kernel-output /absolute/new-camera-output --jobs 4 \
  --receiver-calibration /absolute/private/calibration.json \
  --receiver-sha256 "$PRIVATE_RECEIVER_SHA256"
```

`board` builds ten verified battery, haptics, IMU, motor, input and USB helpers. Motor compile-time limits remain exactly those in the recorded module; they are applied only to its compilation unit. These recipes do not start any hardware.

`camera` builds the installed early-power camera driver and its I2C algorithm module. The public receiver template contains 88 operations, with 12 parameterized by external target calibration. Provenance is recorded in `camera-calibration-provenance.json`; see the calibration format below. The legacy `--receiver-table` interface remains available for a hash-checked numeric table and rejects executable C expressions or out-of-window addresses. Both interfaces write generated headers only to an external build directory.

The HI846 sensor table retains the identified GPLv2 vendor source's MediaTek copyright/license notice. Calibration-bearing artifacts remain private rather than being presented as generic device binaries. No proprietary camera library or reference-device calibration is bundled.

The `cpu` group builds the four selected DVFS, WFI, hotplug and power-completion modules:

```sh
python3 scripts/rabbit-r1/build-external.py cpu \
  /absolute/kernel-output /absolute/new-cpu-output --jobs 8
```

All four rebuilt modules match the recorded installed allocated contents, relocations, global symbols and layout except GNU build IDs. No CPU policy or device state is changed by this build. Other CPU experiments are excluded.

Additional verified helper groups:

```sh
python3 scripts/rabbit-r1/build-external.py display /absolute/kernel-output /absolute/new-display-output
python3 scripts/rabbit-r1/build-external.py bluetooth-voice /absolute/kernel-output /absolute/new-sco-output
python3 scripts/rabbit-r1/build-external.py storage /absolute/kernel-output /absolute/new-storage-output
python3 scripts/rabbit-r1/build-external.py xfrm /absolute/kernel-output /absolute/new-xfrm-output
```

Display requires `dtc`, `fdtget`, `fdtput` and the cross GCC preprocessor. Its 13 runtime overlays are generated from readable source; no prebuilt overlay is committed. Both display modules match the reference allocated contents/relocations/globals/layout except build IDs.

Storage requires Clang 22.1.8 and LLVM tools, matching the reference modules. The expected compiler-mismatch warning identifies GCC 16.1.0 for the main kernel and Clang for these two modules. The recipe retains the exact MSDC driver-name/OCR adaptations and checks the existing 36-bit DMA correction. Both rebuilt modules match reference allocated contents/relocations/globals/layout except build IDs.

SCO and XFRM retain the same code/data after exact source-root normalization. SCO's `.rodata` decreases by 112 bytes and XFRM's by 56 bytes; source-root string lengths account for these differences. All associated relocation and global-symbol offsets were compared with the same exact transformation. Build IDs differ. This does not claim byte-identical artifacts or new hardware validation.

The `rfcomm` group uses Clang 22.1.8 and the recorded September 12 module configuration:

```sh
python3 scripts/rabbit-r1/build-external.py rfcomm /absolute/kernel-output /absolute/new-rfcomm-output
```

The installed module predates the main kernel's `SECURITY_NETWORK`, `NETWORK_SECMARK` and `SECURITY_SELINUX` enablement. A group-local header reproduces their original disabled state for this module only. The kernel configuration is unchanged. This preserves the installed behavior; it is not a security improvement. Building directly with the newer settings changes code and introduces a security hook, so that build is not an exact reconstruction.

The packaged historical build matches code, non-string data and global symbols. Normalizing the four source-root strings shortens the merged string pool by 168 bytes and changes its ordering. The full string multiset and every relocated string/data referent match; the remaining section layout is identical. GNU build IDs differ. These comparisons establish source correspondence, not ABI safety across different kernel configurations or fresh Bluetooth hardware acceptance.

JPEG and media groups:

```sh
python3 scripts/rabbit-r1/build-external.py jpeg-helpers /absolute/kernel-output /absolute/new-jpeg-helpers-output
python3 scripts/rabbit-r1/build-external.py media /absolute/kernel-output /absolute/new-media-output
```

The six JPEG helpers use a runtime power overlay generated from `vcodec-power.dts` with `dtc`. No precompiled overlay is committed. All six match the recorded allocated code/data, relocations, globals and layout except build IDs.

The media group uses the 16 media options from the recorded module configuration while leaving the main kernel configuration unchanged. It builds the media controller, V4L2, videobuf2, JPEG, SMI and IOMMU sources already in this tree. It also builds the shared codec-owner and V4L2 timing helper selected by their existing Makefiles; these do not add new devices to the recorded loaded-module set.

Diagnostic paths retain the reference relative spellings. The rebuilt JPEG core uses the reference's generic `/tmp/r1-jpeg-kernel` diagnostic prefix; this is a compiler mapping, not a required build directory. Owner filesystem paths are not retained.

All 12 newly covered reference media modules match allocated code/data, relocations, global symbols and layout except GNU build IDs and, where present in the reference, exactly the `intree=Y` module metadata field. External compilation omits that marker, which can affect kernel taint reporting. These are source-correspondence builds, not drop-in deployment artifacts or new hardware acceptance tests.

GPU kernel-side groups:

```sh
python3 scripts/rabbit-r1/build-external.py ion /absolute/kernel-output /absolute/new-ion-output
python3 scripts/rabbit-r1/build-external.py powervr /absolute/kernel-output /absolute/new-powervr-output
```

ION uses the adapted Linux system-heap allocator with its original notices. PowerVR uses the exact compiled source closure from the matched 1.13@5776728 Linux 7.1 port and two GPL MediaTek headers. File hashes, vendor references and modified-file identities are listed in `powervr-sources.json`. Original MIT/GPL notices and license texts are retained. Firmware and vendor userspace libraries are excluded; these modules alone do not provide a complete graphics runtime.

Both modules match reference allocated contents, relocations, global symbols and layout after exact source-root normalization; GNU build IDs differ. The obsolete C90 declaration warning is disabled in the PowerVR Makefile without changing driver code. The remaining warnings are five enum conversions, six unused functions, one unused variable, one system-wide workqueue flush warning, and a missing module-description modpost warning. These are recorded technical debt; this is not a warning-free or newly hardware-validated driver.

Connectivity transport groups:

```sh
python3 scripts/rabbit-r1/build-external.py connectivity /absolute/kernel-output /absolute/new-connectivity-output
```

This builds BTIF, WMT and the Wi-Fi character-device adapter using Clang 22.1.8 and the recorded MT6765/MT6631 options. It does not build WLAN, stage firmware or initialize radios. Source closure, original hashes and local port changes are listed in `connectivity-sources.json`.

BTIF matches allocated contents, relocations, globals and layout except its build ID. WMT and the adapter additionally have source-root string changes: 168 bytes and 56 bytes respectively. The normalized string multisets, every relocated string/data referent, non-string data and adjusted global-symbol positions match exactly. Compiler warnings are recorded by category in `external-sources.json`; this legacy port is not warning-free.

The WLAN compiled closure includes `wsys_cmd_handler_fw.h` with an explicit restrictive MediaTek notice, and `common/debug.c` `mgmt/reg_rule.c` and `mgmt/tkip_mic.c` without clear file-level grants. No repository-level license file was found in the retained modules tree. These inputs and the WLAN driver are excluded pending provenance review. A GPL declaration elsewhere in the module does not resolve those individual inputs automatically.

Modem kernel modules:

```sh
python3 scripts/rabbit-r1/build-external.py modem /absolute/kernel-output /absolute/new-modem-output
```

This builds the recorded ECCCI, CCCI utility and AUXADC modules with Clang 22.1.8. It preserves the existing bootloader handoff checks, port adaptations and power behavior. It does not load modules, initialize a modem or provide firmware/IMS userspace. The source closure and vendor hashes are in `modem-sources.json`.

AUXADC matches reference allocated contents, relocations, globals and layout except build ID. ECCCI and CCCI utility also have source-root string shortening of 224 and 112 bytes respectively. Normalized strings, every relocated string/data referent, non-string contents and adjusted symbol/layout positions match. Warnings remain documented in `external-sources.json`. No modem functionality was tested during release preparation.

Video decoder and node helper:

```sh
python3 scripts/rabbit-r1/build-external.py video-decoder /absolute/kernel-output /absolute/new-video-decoder-output
```

This builds the existing media dependencies, the stateless V4L2 H.264 decoder and the encoder device-node adapter. It does not include the encoder itself, its generated mode table or firmware. Diagnostic paths reproduce the recorded generic `/tmp/r1-jpeg-kernel` prefix through compiler mapping. Both newly covered modules match recorded allocated contents, relocations, global symbols and layout except build IDs. No video playback, camera capture or hardware operation was performed for this verification.

The decoder's optional diagnostic firmware requests are retained as source behavior, but no diagnostic bitstreams or device captures are bundled. Existing fixed test-vector code and all GPL notices are preserved. Hardware register conversions are locally authored reverse-engineered code; the broader provenance review remains open.

Full video source reconstruction with private encoder data:

```sh
python3 scripts/rabbit-r1/build-external.py video /absolute/kernel-output /absolute/new-video-output
```

The `video` group adds the encoder to the decoder/media build. It defaults to the bundled, hash-pinned functional mode JSON. The generator accepts bounded numeric geometry, buffer sizes, register pairs and header bytes; it writes a new header outside the source tree without executing supplied C. Optional input overrides require both `--encoder-modes` and `--encoder-sha256`.

`encoder-mode-provenance.json` records the host-emulation recovery method and field review. The bundled data reproduces all four recorded modes and the verified modules byte for byte. Runtime `r1-venc-rc.bin` remains a separate, unbundled firmware dependency; a successful build is not a complete runtime installation or new hardware acceptance test.

### Bluetooth transport

```sh
python3 scripts/rabbit-r1/build-external.py bluetooth-transport /absolute/kernel-output /absolute/new-bluetooth-output
```

Builds the shared connectivity modules and `bt_drv_connac1x` with Clang 22.1.8 and the recorded connac1x options. The transport matches the installed reference after build-ID and source-path normalization, including relocation referents. See `bluetooth-transport-sources.json`. Firmware and device-specific Bluetooth addresses are excluded; this command does not install modules or start Bluetooth.

### GPS and alternate WMT

```sh
python3 scripts/rabbit-r1/build-external.py gps /absolute/kernel-output /absolute/new-gps-output
```

Uses Clang 22.1.8 and the recorded GPS configuration. The separate WMT overlay preserves GPS antenna GPIO control and thread cleanup fixes from the installed fallback; it does not replace the connectivity group’s WMT. Both GPS and fallback WMT match their installed saved references after build-ID/source-path normalization, including every relocated referent. See `gps-sources.json`. No firmware, positioning data or runtime service files are bundled. No service is started. Existing vendor prototype and related compiler warnings remain.

### WLAN with excluded private inputs

```sh
python3 scripts/rabbit-r1/build-external.py wlan /absolute/kernel-output /absolute/new-wlan-output \
  --wlan-private-dir /authorized/private/wlan-inputs
```

This fixed Clang recipe uses the 98 objects and compiler settings recovered from the installed reference. The input directory must contain exactly matching versions of `common/debug.c`, `include/wsys_cmd_handler_fw.h` `mgmt/reg_rule.c` and `mgmt/tkip_mic.c`; unrelated files are not read or copied. Full-file hashes are pinned, symlink inputs and release-tree inputs are rejected, and the selected bytes are written only to the external build directory with mode 0600. The input manifest records relative labels rather than private source paths. These files are excluded because their redistribution terms remain unresolved; the hash check does not grant rights. Keep the resulting module private pending the [redistribution review](redistribution-review.md). No radio or service is started.

The PowerVR group now uses an authored fixed 132-object MT6765 recipe. It reproduces the verified GPU module; the broad vendor Makefile is no longer included. Existing compiler warnings remain documented and are not treated as successful hardware validation.

### Camera calibration input

Camera builds can use `--receiver-calibration /absolute/private/calibration.json` instead of `--receiver-table`, together with `--receiver-sha256` for that input. The JSON has one key, `port2_words`, containing two nonzero unsigned 32-bit integers from the target device's boot devinfo offsets 0x1d0 and 0x1d4 (little-endian). Never commit this file or copy another device's calibration. The fixed template contains no reference calibration values. Generated headers are private build outputs, created with mode 0600.

The reference calibration reproduces the previously verified header byte-for-byte. Zero/missing inputs are rejected; other-device camera operation is not validated. This build-time interface does not read hardware or alter the running device. Redistribution review remains separate.

### Encoder mode defaults

The `video` build now defaults to the audited, hash-pinned `external/rabbit-r1/video/encoder-modes.json`. Earlier instructions requiring a private copy are superseded. Optional overrides still require both `--encoder-modes` and `--encoder-sha256`. The separately loaded rate-control firmware remains excluded and is required for runtime encoding.
