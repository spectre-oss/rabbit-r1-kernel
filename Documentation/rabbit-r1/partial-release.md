# Partial source release

Published at the owner’s request with Rabbit R1 panel and WLAN deferred.

## Included

The verified upstream Linux base, non-panel Rabbit kernel adaptations, and reviewed non-WLAN external driver sources. Shared WMT/BTIF connectivity code remains because Bluetooth/GPS use it; it is not a working WLAN implementation. Generic upstream Linux panel and wireless drivers remain part of the upstream base.

## Excluded

The active source tree omits the Rabbit-specific panel implementation/generator, its generated command table, the entire external gen4m WLAN tree, and the separate Wi-Fi adapter source directory. Our panel integration and Wi-Fi compatibility changes are available separately as [optional patches](../../patches/rabbit-r1/README.md); the original table and complete vendor sources are not bundled. Proprietary firmware/userspace, private calibration, device data and compiled artifacts remain excluded. Panel/WLAN paths and hashes mentioned in audit documents are evidence references, not bundled source.

The publication began with a fresh root commit and no preparation/development history. Later optional patches contain reviewed integration changes; they do not add the original panel table or the four deferred WLAN inputs.

For the public stock source repositories and official GPU firmware archive, see [where to obtain excluded sources and GPU blobs](external-sources-and-gpu-blobs.md).

## Build limitations

This is a source snapshot, not a complete corresponding-source offer for the installed device image. Do not flash it. `build.sh` deliberately refuses a full-device build. `compile-check.sh` disables the omitted Rabbit panel and embedded init; it produces only an experimental compile-check image, with no panel functionality or device acceptance claim. External recipes retain the recorded full-kernel configuration guard; a reader needs a compatible prepared kernel output. WLAN is not a supported recipe here. Connectivity recipes omit the Wi-Fi adapter; these filtered recipes have not been rebuilt, and earlier full-build results must not be applied to them.

The prior full preparation tree built all 21 groups and passed scoped correspondence checks. Those JSON records are historical evidence. The final partial tree is validated for source inventory, absence of omitted components, retained notices, tests and privacy; it is not claimed to reproduce the installed kernel.

## Review status

Nondeferred file-level review identified and restored one MediaTek copyright notice. Existing upstream and vendor notices remain intact. Panel and WLAN disposition stays deferred. The full-release gate remains closed; this separately scoped partial source publication does not clear it.
