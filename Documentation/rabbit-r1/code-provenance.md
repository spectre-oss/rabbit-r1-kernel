# Code provenance and source inventory

> Partial source snapshot: panel and WLAN are omitted. Prior full-build evidence below is historical. See [current publication scope](partial-release.md).

This document records where the proposed Rabbit R1 kernel release comes from. It distinguishes upstream code, publicly released vendor sources, local adaptations, blob-derived hardware research, and private device inputs. It does not attribute confidential-source access to the project owner.

## Project license

Original Spectre kernel contributions use **GPL-2.0-only**; upstream and vendor notices remain intact. The [licensing policy](licensing-policy.md) distinguishes this choice from the remaining third-party notice review. Unresolved notices are not a finding of prohibited redistribution.

## Owner and research context

The owner reports using public material, device blobs and AI assistance, with no access to nonpublic vendor code. AI assistance alone does not establish independent authorship or third-party origin. Local Git history begins with a combined snapshot, so the person or agent that originally acquired each reference is not established. Component origins below are based on source identity, build scripts and retained research records.

## Linux base and kernel patches

The base is [linux-mt6765](https://github.com/evilMyQueen/linux-mt6765) at commit `22b4320e48480edd891d0a7d872a578b5d2f7f98`. Existing upstream copyright/license notices remain in place. [source-delta.json](source-delta.json) lists all 49 recorded changed paths and their dispositions; it is the per-path index for kernel changes. [kernel-correspondence.json](kernel-correspondence.json) explains correspondence with the recorded installed kernel, including metadata normalization. The reconstructed embedded init program and release tooling are local project work; they are not a bundled stock userspace image.

## Rabbit-published vendor sources

**Rabbit itself publicly published the two source repositories used as references:**

- [Rabbit kernel source](https://github.com/rabbit-hmi-oss/android_kernel_rabbit_mt6765/tree/8167c8c1087f057d2ef302fc93b47554291687ec), pinned commit `8167c8c1087f057d2ef302fc93b47554291687ec`.
- [Rabbit kernel modules](https://github.com/rabbit-hmi-oss/android_kernel_modules_rabbit_mt6765/tree/0afdf059a0dfa17becc4bbe47363280a42224c2f), pinned commit `0afdf059a0dfa17becc4bbe47363280a42224c2f`.

These are public GitHub sources, not evidence of private MediaTek access. The panel reference is byte-identical to Rabbit's public `drivers/misc/mediatek/lcm/ili9883_boe_mipi_hd/ili9883_boe_mipi_hd.c` (SHA256 `9fd1eb026a320b1a63fd78cc3739663e0f2fb28db3ec090f0d46a2b0a8d69fc9`). That repository's top-level COPYING states GPL-2.0 with the syscall note; the file also contains a restrictive notice. The four flagged WLAN files likewise have public-source provenance. Public availability is established; conflicting or missing file-level terms remain recorded in [redistribution-review.md](redistribution-review.md), rather than silently erased or treated as a finding of unlawful access.

## Component origins

The following is a component-level map. The linked manifests preserve finer-grained paths, hashes, references and notices. A source hash proves identity, not who authored a file or a blanket licensing conclusion.

| Component | Origin and treatment |
| --- | --- |
| audio | Local R1 audio integration, public MediaTek codec/AFE references, and upstream Linux ALSA. |
| core-helpers | Kernel-tree AUXADC code and local shared video-codec ownership helper. |
| camera | Local native camera implementation; public sensor/power references and host-only reverse engineering of shipped HAL behavior. Calibration is supplied separately by the target owner. |
| board | R1 board helpers selected from saved working sources: battery, input, haptics, IMU, camera motor and USB-C. Existing notices are retained; the manifest identifies exact source snapshots rather than attributing every line to one author. |
| cpu | R1 CPU/power integration from the saved working source set; exact files are listed below. This inventory does not assert independent authorship of every hardware definition. |
| display | Local overlay/backlight integration plus public vendor display references. Linux DRM and panel changes are also recorded in the kernel delta. |
| bluetooth-voice | R1 SCO integration with public MediaTek BTCVSD reference attribution. |
| storage | Upstream Linux MediaTek MMC driver with R1-specific adaptations. |
| xfrm | Upstream Linux XFRM user interface, compiled for the recorded reference configuration. |
| rfcomm | Upstream Linux Bluetooth RFCOMM with the recorded historical configuration. |
| jpeg-helpers | Local device-node, power, ownership and integration helpers for the kernel JPEG stack; exact retained sources below. |
| media | Linux media/V4L2/videobuf2, MediaTek JPEG, IOMMU and SMI sources from the pinned kernel tree with recorded configuration/patches. |
| ion | Adaptation of Linux system-heap code; Google, Linaro and Texas Instruments notices retained. |
| powervr | Public vendor PowerVR kernel sources with retained MIT/GPL notices; locally authored fixed build recipe. GPU firmware and userspace libraries are excluded. |
| connectivity | Public Rabbit vendor BTIF/WMT/adapter source with local Linux compatibility adaptations and authored build recipes. |
| modem | Public vendor kernel CCCI sources and local integration; modem firmware, IMS userspace and SIM data excluded. |
| video-decoder | Local stateless V4L2 implementation based on recovered hardware behavior from stock userspace; vendor library not bundled. |
| video | Local native encoder integration and recovered functional mode data (register settings, allocation sizes, SPS/PPS); separate rate-control firmware excluded. |
| bluetooth-transport | Public vendor Bluetooth transport sources with local integration and authored build recipe. |
| gps | Public vendor GPS/WMT sources plus local antenna/integration changes. |
| wlan | Public Rabbit gen4m vendor source port with local compatibility patches. Four exact inputs remain separately supplied while notice conflicts are reviewed. |

## Exact external-source index

[external-sources.json](external-sources.json) is the common inventory. Large imported source closures have separate manifests:

- [powervr-sources.json](powervr-sources.json)
- [connectivity-sources.json](connectivity-sources.json)
- [modem-sources.json](modem-sources.json)
- [bluetooth-transport-sources.json](bluetooth-transport-sources.json)
- [gps-sources.json](gps-sources.json)
- [wlan-sources.json](wlan-sources.json)

The following explicit paths are taken from the common inventory; directory-based Linux groups and separate manifests above cover additional compiled files.

### audio

- `external/rabbit-r1/audio/r1_audio.c`
- `external/rabbit-r1/audio/r1-audio-analog.h`

### core-helpers

- `drivers/iio/adc/mt6359-auxadc.c`
- `drivers/media/platform/mediatek/jpeg/r1_vcodec_owner.c`
- `drivers/media/platform/mediatek/jpeg/r1_vcodec_owner.h`

### camera

- `external/rabbit-r1/camera/r1_camera_probe.c`
- `external/rabbit-r1/camera/r1_camera_capture.h`
- `external/rabbit-r1/camera/r1_camera_pipeline.h`
- `external/rabbit-r1/camera/r1_camera_stream.h`
- `external/rabbit-r1/camera/sensor-tables.h`
- `scripts/rabbit-r1/camera-receiver-template.json`
- `scripts/rabbit-r1/prepare-camera-calibration.py`

### board

- `external/rabbit-r1/board/r1_battery.c`
- `external/rabbit-r1/board/r1_haptics.c`
- `external/rabbit-r1/board/r1_icm42607.c`
- `external/rabbit-r1/board/r1_ms35774.c`
- `external/rabbit-r1/board/r1_input_power.c`
- `external/rabbit-r1/board/r1_keypad.c`
- `external/rabbit-r1/board/tcpc.c`
- `external/rabbit-r1/board/bus.c`
- `external/rabbit-r1/board/i2c-algo-bit.c`
- `external/rabbit-r1/board/session-v2.c`
- `external/rabbit-r1/board/host-speed.c`

### cpu

- `external/rabbit-r1/cpu/r1_cpu_dvfs.c`
- `external/rabbit-r1/cpu/r1_cpu_hotplug.c`
- `external/rabbit-r1/cpu/r1_cpu_idle_smp.c`
- `external/rabbit-r1/cpu/r1_cpu_power.c`
- `external/rabbit-r1/cpu/r1_cpu_power.h`
- `external/rabbit-r1/cpu/r1_cpu_voltage.h`

### display

- `external/rabbit-r1/display/r1_backlight.c`
- `external/rabbit-r1/display/r1_display_nodes.c`

### bluetooth-voice

- `external/rabbit-r1/bluetooth-voice/r1_sco.c`

### storage

- `external/rabbit-r1/storage/r1_emmc.c`

### xfrm

- `net/xfrm/xfrm_user.c`

### rfcomm

- `net/bluetooth/rfcomm/core.c`
- `net/bluetooth/rfcomm/sock.c`
- `net/bluetooth/rfcomm/tty.c`

### jpeg-helpers

- `external/rabbit-r1/jpeg-helpers/r1_jpeg_iommu_nodes.c`
- `external/rabbit-r1/jpeg-helpers/r1_jpeg_nodes.c`
- `external/rabbit-r1/jpeg-helpers/r1_jpeg_power_clocks.c`
- `external/rabbit-r1/jpeg-helpers/r1_jpeg_power_links.c`
- `external/rabbit-r1/jpeg-helpers/r1_jpeg_power_nodes.c`
- `external/rabbit-r1/jpeg-helpers/r1_jpeg_selector_nodes.c`
- `external/rabbit-r1/jpeg-helpers/vcodec-power.dts`

### media

- `drivers/iommu/mtk_iommu.c`
- `drivers/memory/mtk-smi.c`
- `drivers/media/mc`
- `drivers/media/v4l2-core`
- `drivers/media/common/videobuf2`
- `drivers/media/platform/mediatek/jpeg`

### ion

- `external/rabbit-r1/ion/r1_ion.c`
- `external/rabbit-r1/ion/r1_system_heap.c`

### video-decoder

- `external/rabbit-r1/video/r1_h264_limits.h`
- `external/rabbit-r1/video/r1_h264_lists.h`
- `external/rabbit-r1/video/r1_h264_regs.h`
- `external/rabbit-r1/video/r1_h264_reorder_check.h`
- `external/rabbit-r1/video/r1_h264_weight_span.h`
- `external/rabbit-r1/video/r1_vdec_hw.inc`
- `external/rabbit-r1/video/r1_vdec_pcm.inc`
- `external/rabbit-r1/video/r1_vdec_probe.c`
- `external/rabbit-r1/video/r1_vdec_v4l2.inc`
- `external/rabbit-r1/video/r1_venc_nodes.c`

### video

- `external/rabbit-r1/video/r1_venc.c`
- `external/rabbit-r1/video/r1_venc_regs.h`
- `external/rabbit-r1/video/r1_venc_v4l2.inc`
- `external/rabbit-r1/video/encoder-modes.json`

## Recovered data and private inputs

- Camera receiver settings were recovered through host-only HAL emulation. Twelve of 88 writes depend on target calibration. The public template omits reference calibration; the private input reproduces the verified modules exactly. See [camera-calibration-provenance.json](camera-calibration-provenance.json).
- Encoder modes contain hardware configuration and fully parsed SPS/PPS data, not rate-control executable code. See [encoder-mode-provenance.json](encoder-mode-provenance.json).
- The panel command table currently requires the separately supplied public vendor reference. Do not mistake its exclusion from this proposed payload for a claim that it was secretly obtained.
- The WLAN build currently requires four separately supplied public-source files: `include/wsys_cmd_handler_fw.h`, `common/debug.c`, `mgmt/reg_rule.c`, and `mgmt/tkip_mic.c`, relative to vendor `connectivity/wlan/core/gen4m`. Exact findings are in the redistribution review.

## Excluded artifacts

Stock firmware, vendor userspace libraries, boot/device dumps, private calibration, NVRAM, SIM information, credentials, personal photographs and message history are not release source. Build outputs containing device calibration stay private. Runtime firmware requirements are documented separately from the kernel source license.

## Verification and remaining work

[build.md](build.md) documents build inputs and commands; [external-correspondence-review.json](external-correspondence-review.json) scopes module comparisons. Preserve existing author notices; do not relabel imported code as original merely because AI participated. [release-status.json](release-status.json) is the current release gate. The project is not yet a published or fully cleared release.

## WLAN compiled-input review

The public gen4m entry point declares `MODULE_LICENSE("Dual BSD/GPL")`. This is recorded alongside, rather than substituted for, the individual source notices. In the verified build, `common/debug.c`, `mgmt/reg_rule.c` and `mgmt/tkip_mic.c` contribute code/data symbols; the local regulatory database is enabled. `include/wsys_cmd_handler_fw.h` is referenced by the recorded compiler dependencies. The separately supplied inputs are therefore not merely unused files in a vendor archive. Removing them or disabling those features would not preserve the verified build.

This establishes build dependency and public release context. It does not identify which person or AI session originally acquired each file, nor assert that the owner accessed nonpublic source.
