# Hardware and module scope

> Partial source snapshot: panel and WLAN are omitted. Prior full-build evidence below is historical. See [current publication scope](partial-release.md).

The October 7 read-only inventory identifies the running build-49 kernel, 60 loaded modules and five selected on-demand artifacts (64 distinct module names, including a separate alternate WMT artifact). This is an artifact inventory, not a new hardware acceptance test. Exact hashes and evidence levels are recorded in `live-reference.json` and `module-reference.json`.

| Area | Kernel-side support identified | Source release status |
| --- | --- | --- |
| Display | MediaTek DRM, panel, backlight and board-node helpers | Backlight, board-node, DRM and panel sources rebuilt and matched; panel input remains private with unresolved redistribution rights |
| GPU | `pvrsrvkm`, `r1_ion` | Matched PowerVR and ION source recipes staged; original notices retained; firmware/userspace excluded |
| Storage | `r1_msdc`, `r1_emmc` | Source recipes rebuilt with original Clang toolchain; active boot ramdisk references match |
| CPU/power | DVFS, idle, hotplug and power helpers | Four selected source recipes rebuilt and matched; deep sleep is not claimed complete |
| Battery/ADC | Battery driver and MT6359 AUXADC | Battery and AUXADC recipes staged and rebuilt |
| Audio | `soundcore`, `snd`, `snd_timer`, `snd_pcm`, `r1_audio` | Source and build recipe staged; rebuild correspondence verified with documented metadata normalization |
| Input/haptics/IMU | Keypad, power-key helper, haptics, ICM42607; in-tree wheel driver | Source recipes staged and rebuilt |
| USB-C | Type-C bus, TCPC, session and host-speed helpers | Source recipes staged and rebuilt; known TCPC IRQ-storm issue remains |
| Wi-Fi/Bluetooth | WMT, BTIF, Wi-Fi modules, SCO and RFCOMM | SCO, historical RFCOMM, WMT, BTIF and Wi-Fi adapter rebuilt and matched; on-demand Bluetooth transport rebuilt and matched; WLAN privately rebuilt and matched; four required source inputs remain excluded for licensing review; firmware excluded |
| GPS | On-demand `gps_drv` and alternate WMT fallback | GPS and its alternate WMT fallback rebuilt and matched to installed saved artifacts |
| Modem/network | ECCCI, CCCI utility, AUXADC helper, XFRM | XFRM and three modem modules rebuilt and matched; source notices retained; firmware/IMS userspace excluded |
| Video/JPEG | Media stack, JPEG, shared codec owner, encoder/decoder helpers | Codec owner, JPEG/shared media, decoder, encoder and video-node sources rebuilt and matched; functional encoder mode data is included with reviewed provenance; separate rate-control firmware is excluded |
| Camera/motor | On-demand `r1_camera_live` and `i2c_algo_bit`; loaded `r1_ms35774` motor driver | Sources staged and rebuilt against installed references; public receiver template and calibration generator reproduce the verified modules; target calibration remains an external private input |

The installed camera service selects the early-power camera implementation. It was inactive during inspection and was not started. Camera position journals, per-device calibration, firmware, vendor userspace libraries and device backups do not belong in the public kernel source tree.

The receiver-table research inputs were traced to boot-provided device information. The public template separates 12 calibration-dependent operations from 76 fixed operations. Raw reference-device values remain excluded; owners supply target calibration through the documented build interface. See `camera-calibration-provenance.json`. This does not establish cross-device calibration compatibility or identify the values as personal identifiers.

The recorded loaded/on-demand scope has source recipes, clean-build and correspondence evidence. Nondeferred license review is complete; panel/WLAN disposition and the final publication snapshot remain deferred/pending. A listed module or successful compilation alone does not establish that the hardware works correctly.

A later read-only service audit identified the configured Bluetooth transport and launcher-triggered GPS test outside the loaded-module snapshot. GPS can load an alternate WMT implementation when the shared stack is absent; it differs in executable code and must be reviewed separately. These are recorded in `on-demand-reference.json`. The optional MT6370 charger path is disabled by the installed configuration; older disabled CPU services are not selected as current driver recipes.
