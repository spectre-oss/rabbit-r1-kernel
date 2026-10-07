# Project licensing policy

> Partial source snapshot: panel and WLAN are omitted. Prior full-build evidence below is historical. See [current publication scope](partial-release.md).

Spectre's original Rabbit R1 kernel contributions are released under **GPL-2.0-only**, as selected by the project owner. This applies to original code the project is entitled to license; it does not replace third-party licenses or claim ownership of upstream work.

Keep the kernel's existing `COPYING`, `LICENSES/`, copyright notices, SPDX identifiers and applicable Linux syscall exception. Preserve existing dual-license choices and attribution on imported or adapted code. Do not relabel an entire imported file as exclusively Spectre-authored.

## Source and research context

Rabbit publicly published both reference repositories, as pinned in [code provenance](code-provenance.md). The owner reports using public material, shipped blobs and AI assistance, with no access to nonpublic vendor source. The review found no basis to assert confidential-source access.

Independently implemented hardware support and recovered functional settings are documented separately from copied source, firmware and device calibration. AI assistance does not itself determine a component's origin or license.

## Remaining review

The panel and four WLAN inputs have documented conflicting or incomplete licensing context. These are unresolved interpretation questions, **not findings that redistribution is prohibited**. Choosing GPL for original contributions does not independently settle third-party notices. See [redistribution review](redistribution-review.md) for the exact evidence.

Engineering, provenance documentation and reproducibility work can continue while those questions remain recorded. The current staging tree keeps those inputs external; this is a packaging decision pending review, not a claim that public-source use was improper. Clean-build and preparation-commit privacy checks have passed; later edits need refreshed payload review. The full license review retains the deferred scope recorded in [release status](release-status.json).

Proprietary firmware/userspace binaries, credentials, device dumps and private calibration are not bundled. No statement here licenses those excluded artifacts.

## Completed nondeferred review

The file-level [review inventory](nondeferred-license-review.json) covers 772 source/build/tool files outside panel/WLAN, including present kernel-delta sources and local release code. It records primary grants and current hashes. For 647 imported source files it also verifies the pinned original hash and retention of original notice blocks. The remaining unchanged Linux base inherits its verified upstream context; this review is not a new audit of every upstream file.

The review found and restored the MediaTek 2019 copyright block in the adapted `mtk_mfgsys.c`. The block was appended after the existing source, preserving all previous bytes and line positions. Its inventory hash was updated. Embedded zlib terms in the modem header remain intact alongside the MediaTek GPL identifier; the primary-grant column does not supersede additional notices. Functional camera/encoder data retain their separate provenance dispositions.

GPL variants and preserved dual grants were assessed using the [Linux kernel licensing rules](https://www.kernel.org/doc/html/latest/process/license-rules.html) and the actual retained source notices. No remaining notice/provenance issue was identified within this scope. The full license gate stays pending for the owner's deferred panel/WLAN scope; no blanket clearance of excluded firmware, patents or unrecorded origins is asserted.
