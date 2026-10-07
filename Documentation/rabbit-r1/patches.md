# Rabbit R1 changes

> Partial source snapshot: panel and WLAN are omitted. Prior full-build evidence below is historical. See [current publication scope](partial-release.md).

This is a source correspondence release in preparation. Preserve the recorded build-49 behavior before proposing functional cleanup. The verified community base and the complete path inventory are in `provenance.md` and `source-delta.json`.

| Area | Changes carried by the staged source |
| --- | --- |
| Board description | Rabbit R1 DTS and defconfig; PMIC keys label; DSI PHY register range, display RDMA clock and JPEG node. The board DTS is not yet verified against the packaged device DTB. |
| CPU and suspend | Early CPU-hotplug lock; opt-in MT6765 SPM suspend path and firmware checks. Runtime CPU support also depends on external modules. Deep sleep is not claimed working. |
| Android Binder | Untag user pointers at transaction and ioctl boundaries. |
| Display | MMSYS clock-device binding and routes; RDMA vblank source; clear bootloader OVL interrupts; bounded DSI IRQ wait and corrected setup/error handling; panel driver with separately supplied private initialization table; legacy bootloader framebuffer driver. |
| Input | Board-selected PMIC power-key polling and OCH1970 wheel driver. |
| Memory and DMA | MT6765 IOMMU address-width correction and runtime-PM ordering; SMI larb configuration; eMMC 36-bit DMA capability. |
| JPEG and shared codecs | MT6765 clocks, quality codes and binding; shared codec ownership; clock-error propagation; watchdog/IRQ completion ownership; encoded-length validation. External video drivers are separate. |
| Power | VCODEC bus protection and domain supply; MT6357 VSRAM selector masks; boot watchdog disarm and reset sequence. |
| USB | Recorded DMA mask/interrupt and diagnostic changes. The later, uninstalled PHY-lifetime experiment is excluded. |
| Recovery and init | R1 reboot/FASTBOOT interface; reconstructed source for the original embedded init. |

## Intentional limitations of the compatibility target

The early CPU-hotplug lock is unconditional in this tree. The watchdog probe changes and several bring-up diagnostics also remain as recorded. Removing these during cosmetic cleanup could change the behavior being reproduced. This tree is a Rabbit R1 development target, not a claim that its changes are suitable for unrelated MediaTek boards or upstream acceptance.

The embedded fallback init launches recovery gettys. Review its authentication and boot assumptions before exposing a system built from it; it is preserved here for correspondence with the recorded executable.

The kernel alone does not provide the owner's complete hardware stack. Wi-Fi, Bluetooth, GPU, modem, camera, USB role handling, CPU control and other helpers require separately matched external modules. Firmware, vendor userspace, OS services and private device data are not part of this source tree.

A passing build is not device acceptance. Publication remains blocked until the live kernel, installed and loaded modules, source inputs, licensing and privacy checks are complete.
