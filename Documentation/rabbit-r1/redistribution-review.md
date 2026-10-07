# Redistribution review

> Partial source snapshot: panel and WLAN are omitted. Prior full-build evidence below is historical. See [current publication scope](partial-release.md).

Original Spectre kernel contributions use **GPL-2.0-only**. Existing upstream and vendor notices remain intact; see [licensing policy](licensing-policy.md). This document records evidence and packaging decisions, not a legal clearance or a finding that redistribution is prohibited.

## Public Rabbit sources

Rabbit publicly published both reference repositories:

- [Kernel source, commit 8167c8c1087f057d2ef302fc93b47554291687ec](https://github.com/rabbit-hmi-oss/android_kernel_rabbit_mt6765/tree/8167c8c1087f057d2ef302fc93b47554291687ec).
- [Module source, commit 0afdf059a0dfa17becc4bbe47363280a42224c2f](https://github.com/rabbit-hmi-oss/android_kernel_modules_rabbit_mt6765/tree/0afdf059a0dfa17becc4bbe47363280a42224c2f).

The owner reports using public material, shipped blobs and AI assistance, without nonpublic vendor-source access. Verified public content identities support public-source provenance. File notices are not evidence of confidential access. [Code provenance](code-provenance.md) records component origins and exact inventories.

## Panel: conflicting license context

The reference `drivers/misc/mediatek/lcm/ili9883_boe_mipi_hd/ili9883_boe_mipi_hd.c` exactly matches Rabbit’s pinned public source, SHA256 `9fd1eb026a320b1a63fd78cc3739663e0f2fb28db3ec090f0d46a2b0a8d69fc9`. The repository's top-level COPYING declares GPL-2.0 with the Linux syscall note; the file also carries a restrictive MediaTek notice. Both facts matter. This audit has not resolved their relationship and does not conclude the public GPL release is invalid.

The current staging package uses an external, hash-pinned input through `prepare-panel.py`, preserving the original notice. All 40 generated steps match the reference. A self-contained release needs a documented disposition of the conflicting context. Reviewing the existing public grant, finding additional applicable terms or developing an independently justified replacement are possible paths; fresh vendor permission is not presumed mandatory.

## WLAN: four file-specific questions

Paths below are under `connectivity/wlan/core/gen4m/` in Rabbit's pinned module repository. Public source identity has been verified.

| Path | Recorded licensing context |
| --- | --- |
| `include/wsys_cmd_handler_fw.h` | Restrictive MediaTek notice; public Git blob `0c0b46ef14bedf2fb8893d2793456469ca2ef024`. |
| `common/debug.c` | No file-level notice found; public Git blob `890ae3280ea8874482ade4551653efce9bd525e1`. |
| `mgmt/reg_rule.c` | No file-level notice found; public Git blob `d7aa5bccd424a9dddaddbc39864ffacff2a75dfc`. |
| `mgmt/tkip_mic.c` | MediaTek GPL/BSD grant followed by an older restrictive Inprocomm notice. Their relationship remains unassessed; the grant is not declared invalid. |

The module declares `MODULE_LICENSE("Dual BSD/GPL")` in `os/linux/gl_init.c`. No root README/COPYING/LICENSE/NOTICE or WLAN license file was found in the bounded tree review. A separate conninfra GPL notice was found. These observations need contextual interpretation; missing per-file notices alone do not establish proprietary status.

The three C inputs contribute compiled code/data, and the header occurs in 95 dependency records. They cannot simply be omitted while claiming the same driver. The current recipe accepts four hash-pinned external inputs. `wlan-sources.json` records 218 public vendor files, two compatibility headers and an authored build recipe. Original notices are retained. No vendor message has been sent.

## Resolved input reviews and exclusions

| Component | Current disposition |
| --- | --- |
| Camera receiver | Provenance review passes. Host-only HAL emulation recovered register operations. The public template separates 12 calibration-dependent operations from 76 fixed operations; device calibration remains external. Generated reference modules match the verified builds. See `camera-calibration-provenance.json`. |
| Encoder modes | Functional mode data is included in `external/rabbit-r1/video/encoder-modes.json`. Schema review and independent SPS/PPS parsing/reconstruction account for the payload. Generated modules match the verified builds. See `encoder-mode-provenance.json`. |
| Encoder rate-control firmware | Separate from mode data and excluded. |
| Other firmware and vendor userspace | Excluded; no redistribution terms asserted here. |
| Vendor build boilerplate | Selected PowerVR, connectivity, WMT and Bluetooth recipes were replaced with authored recipes from recorded object lists/settings; correspondence evidence is retained. |

Recovered functional settings are assessed separately from copied program expression. The research basis is recorded in [U.S. Copyright Office Circular 61](https://www.copyright.gov/circs/circ61.pdf) and [EU Directive 2009/24/EC](https://eur-lex.europa.eu/legal-content/EN/ALL/?uri=CELEX%3A32009L0024), particularly Articles 1(2), 5(3) and 6. These do not provide blanket clearance of every extraction or artifact.

## Work remaining

Panel and WLAN disposition remains recorded as unresolved in [release-status.json](release-status.json). Engineering and documentation can continue. The nondeferred file-level notice/provenance review is complete; see [review evidence](nondeferred-license-review.json). Clean-release build and preparation-commit privacy checks have passed within their recorded scope. Later edits require refreshed payload review. Panel and WLAN are deferred by the owner; the full license gate remains pending for that scope only.

The publication gate remains a conservative readiness check, not a legal judgment. No notices should be removed or licensing-check results marked passed solely because the project's original contributions use GPL.
