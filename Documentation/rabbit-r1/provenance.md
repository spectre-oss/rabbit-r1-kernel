# Source provenance and licensing

> Partial source snapshot: panel and WLAN are omitted. Prior full-build evidence below is historical. See [current publication scope](partial-release.md).

See the [project licensing policy](licensing-policy.md): original Spectre kernel contributions use **GPL-2.0-only**, with third-party notices preserved.

## Community kernel base

The development snapshot records this base:

- Original repository: https://github.com/TheKnightSky/linux-mt6765
- Resolved repository: https://github.com/evilMyQueen/linux-mt6765
- Commit: `22b4320e48480edd891d0a7d872a578b5d2f7f98`
- Tree: `dff6f9a832834a9544dc7b6e085fc0b695ffe241`
- Branch recorded at import: `mt6765-devel`

The commit was resolved through GitHub's API during release preparation. All 41 top-level tracked Git objects of the on-disk reference snapshot were independently hashed and matched the public commit tree, including all source subtrees. Local-only extra files are not part of that verification. Do not describe reconstructed local history as original upstream ancestry.

`source-delta.json` inventories modified and added paths relative to that local reference snapshot. It is evidence for review, not a license clearance certificate.

## Vendor references

Public source repositories used during bring-up include:

- https://github.com/rabbit-hmi-oss/android_kernel_rabbit_mt6765
- https://github.com/rabbit-hmi-oss/android_kernel_modules_rabbit_mt6765

The panel initialization table and the external HI846 sensor tables require file-specific attribution review. The inspected HI846 four-lane source carries MediaTek's 2018 copyright and GPL version 2 notice. Preserve that notice when publishing derived tables; the generated SPDX marker alone does not preserve attribution.

Wi-Fi, Bluetooth, GPU and modem modules need their own pinned source inventories. A module declaring `MODULE_LICENSE("GPL")` does not substitute for reviewing all files used to build it. Binary firmware and proprietary userspace libraries are separate dependencies and are not included here.

## Release rules

Keep `COPYING`, `LICENSES/`, upstream copyright notices, and original contributor attribution. Do not apply a blanket new license to third-party code. Document every external source revision and imported file, and resolve conflicting or missing notices before publication.

Private development commit metadata and owner-specific identifiers are not imported. New release commits should use the maintainer's public GitHub identity. Normal upstream attribution remains intact.

References: https://www.kernel.org/doc/html/latest/process/license-rules.html

## Confirmed licensing review issue: panel table

`drivers/gpu/drm/panel/panel-rabbit-r1-init.h` identifies source SHA256 `9fd1eb026a320b1a63fd78cc3739663e0f2fb28db3ec090f0d46a2b0a8d69fc9`. This exactly matches the vendor `drivers/misc/mediatek/lcm/ili9883_boe_mipi_hd/ili9883_boe_mipi_hd.c`. That vendor file carries an explicit restrictive MediaTek notice and no file-level GPL grant was found. The generated header's GPL marker does not resolve this conflict.

The table has been removed from the publication checkout. `prepare-panel.py` accepts an explicitly supplied, hash-checked vendor source and generates a private build header outside the source tree, retaining its original notices. All 40 generated steps match the reference commands, bytes, and delays. The utility neither downloads nor executes the input and grants no rights to use or redistribute it. The current staging package excludes that generated header and images containing it pending disposition. Rabbit’s pinned public kernel repository also declares GPL-2.0 with the Linux syscall note in its top-level COPYING. The issue is conflicting licensing context, not proof of prohibited redistribution. A self-contained release needs a documented disposition of this context; a new grant or replacement is not assumed to be the only possible resolution.

The Wi-Fi scanner's restrictive-language hits require contextual review: inspected `wlan_lib.c` explicitly offers GPLv2 or BSD licensing. Keyword hits alone are not grounds to call a driver proprietary. The modem manifest's 969 source inputs all contain recognizable GPL notices in this preliminary scan; completeness and full license compatibility still require review.

## Wheel driver reference

The OCH1970 driver identifies the vendor wheel algorithm as a reference. The inspected vendor `drivers/input/touchscreen/och1970.c`, SHA256 `956969380bcf5c3b5143c67130e8eb41ad1f59e1dcdce5074e7d3eb05e7ab64b`, carries `GPL-2.0+` and Red Hat / Hans de Goede attribution. That notice is retained in the staged driver. The local file selects GPL version 2 only, which is within the reference file’s version-2-or-later grant. This records the observed notice; it is not independent proof of the vendor’s chain of title.

## Vendor module provenance pin

The public `rabbit-hmi-oss/android_kernel_modules_rabbit_mt6765` r1 commit was resolved as `0afdf059a0dfa17becc4bbe47363280a42224c2f`, tree `89284f2694e7c72946cb43de903ba64657881ca9`. The complete recursive tree was returned without truncation. A total of 114 vendor inputs referenced across the selected connectivity, modem, PowerVR, Bluetooth and GPS manifests were compared against their public Git blob hashes and recorded original SHA256 values. Only manifest entries identifying this module repository are covered; other vendor repositories and local additions require their own evidence. Applicable entries now record the commit and blob.

The unresolved WLAN files also match that public tree. Their licensing remains unresolved; see [redistribution review](redistribution-review.md) for exact files and required evidence.
