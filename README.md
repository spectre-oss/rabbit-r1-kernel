# Spectre Kernel for Rabbit R1

Linux device support for Rabbit R1, maintained by [Spectre OSS](https://github.com/spectre-oss).

**Partial source release: Complete vendor panel/Wi-Fi sources are excluded; optional compatibility patches are available. This snapshot is not a complete device kernel or a flashable image.**

See [included/excluded scope and build limitations](Documentation/rabbit-r1/partial-release.md). Earlier full-build results document the private preparation tree, not this filtered snapshot.

The target is the Linux 7.1.0 Rabbit R1 development kernel, together with the kernel-side support required by its external hardware modules. It is a downstream community kernel, not an upstream Linux release or a complete operating system.

## Documentation

- [Optional panel/Wi-Fi compatibility patches and preparation instructions](patches/rabbit-r1/README.md)

- [Rabbit panel/Wi-Fi source links and official GPU blob downloads](Documentation/rabbit-r1/external-sources-and-gpu-blobs.md)

- [Release audit and outstanding requirements](Documentation/rabbit-r1/release-audit.md)
- [Source provenance and licensing](Documentation/rabbit-r1/provenance.md)
- [Kernel changes and compatibility limits](Documentation/rabbit-r1/patches.md)
- [Hardware and external-module scope](Documentation/rabbit-r1/hardware.md)
- [Build validation](Documentation/rabbit-r1/build.md)
- [Source checks and device correspondence](Documentation/rabbit-r1/verification.md)

The Linux license and syscall exception are described in [COPYING](COPYING). Existing file-level licenses and copyright notices remain applicable.

Independent community project; not affiliated with Rabbit Inc.

Code origins and per-component source inventory: [Code provenance](Documentation/rabbit-r1/code-provenance.md).

## License

Original Spectre kernel contributions use **GPL-2.0-only**. Existing upstream licenses, copyright notices and the Linux syscall exception remain applicable. See the [licensing policy](Documentation/rabbit-r1/licensing-policy.md) and [redistribution review](Documentation/rabbit-r1/redistribution-review.md).
