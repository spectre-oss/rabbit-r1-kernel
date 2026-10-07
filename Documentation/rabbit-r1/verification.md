# Verification workflow

> Partial source snapshot: panel and WLAN are omitted. Prior full-build evidence below is historical. See [current publication scope](partial-release.md).

## Local source checks

Run from the repository root:

```sh
python3 -m unittest discover -s scripts/rabbit-r1/tests -v
python3 scripts/rabbit-r1/audit-source.py
```

For the private prepublication review, pass `--private-terms-file /outside/repository/private-terms.txt`. Put one exact identifier per line. Never commit that file or its contents. The scanner reports finding categories without exposing matched values, and suppresses filenames containing a supplied private term. It is a bounded check, not proof that no sensitive information exists.

The source-checks CI workflow runs the tests, shell syntax checks and public-pattern scan. It uses a commit-pinned checkout action with read-only repository permissions and no retained credentials. It does not download private panel inputs, compile release artifacts, upload binaries or deploy to hardware. Local checks have run; a hosted run can only be verified after publication.

## Read-only device correspondence

Run `scripts/rabbit-r1/device-inventory.py` on the intended device with Python 3, through the maintainer's existing authenticated SSH connection. Capture stdout to a private file outside this repository. The script reads kernel configuration, module files and loaded-module build notes. It performs no module loads or device-control operations.

Compare that inventory against a private index of local module artifacts:

```sh
python3 scripts/rabbit-r1/match-modules.py \
  /private/device-inventory.json /private/local-module-artifacts.json \
  > /private/module-comparison.json
```

An artifact index is a JSON array containing `path` and full-file `sha256` for each local `.ko` file. The matcher requires full hashes for installed files and matching build-note hashes for loaded modules. Missing notes, unknown files, ambiguous matches and inventory errors fail closed. A matched build note is supporting evidence; source correspondence still requires recovering the exact source inputs and rebuilding the module.

Do not publish a raw inventory automatically. Its paths and version strings are private audit material until reviewed. Empty inventories never count as a pass.

The recorded publication gate is `python3 scripts/rabbit-r1/release-gate.py`. It blocks release while required evidence is incomplete. Passing that record check is not a substitute for inspecting the evidence.

The clean main-kernel build also reproduces the panel module's allocated contents, relocations, globals and layout except its GNU build ID. Its generated panel table remains a private dependency with unresolved redistribution rights; neither that table nor the resulting module/image is cleared for publication.

The DRM module matches after exactly two `./include/` versus `include/` diagnostic-path spellings are normalized. They account for all four bytes of `.rodata` size difference. Associated relocation offsets, global symbols and the remaining section layout were verified; executable code is unchanged. This is source correspondence, not a new display acceptance test.

## Main image correspondence

The installed build-49 compressed image has SHA256 `46560253fe0264cb27e5a162ff1ef73c02a2e9ed37c45ebf82da9ca6bb644f12`; its decompressed image is 26,804,232 bytes with SHA256 `6931f8914450221fbd08312238573f0fb8b2624e5969d06d90b80580f6b404a2`. A retained full ELF belongs to build 45, and the development ELF belongs to the uninstalled build 50. Object comparisons against those references do not by themselves prove build-49 image correspondence. An isolated private reconstruction has now been compared directly with that build-49 image. Historical build identity is private audit material and must not enter release sources or artifacts.

The 26,804,232-byte reconstructed image differs in only two 20-byte GNU build-ID note descriptors, whose ELF note headers were checked explicitly. Every other byte is identical, including executable code, data and embedded initramfs. See `kernel-correspondence.json` for offsets, scope and private metadata adjustments. An added SPDX line changed an initcall symbol’s line-number suffix; a private line directive reproduced the historical suffix without removing the public notice. This proves main-kernel source correspondence, not binary reproducibility of the sanitized release build, legal clearance or new hardware acceptance.

The source scanner rejects the four unresolved WLAN inputs by their exact intended paths and full-file hashes, including renamed copies. This supplements the publication gate; it cannot identify arbitrary edited or newly encountered restricted material. The excluded source contents are not included in tests.

A private WLAN build from the 98-object fixed recipe matches the installed reference: all allocated code/non-string data, normalized string contents, every relocated referent, global symbols and section layout agree. Differences are the GNU build ID and 112 bytes of source-root spelling. The build requires four excluded unresolved-license files. Consequently all 60 observed loaded modules have rebuild correspondence evidence, but the complete public source/build and final publication checks remain open. Vendor compiler warnings remain; this is not a warning-free build or new hardware test.

The supported WLAN build interface has also been validated from a new output directory against the installed reference with the same complete section/relocation comparison. Its private-input validation has synthetic tests for tampering, missing files, symlinks, relative paths and release-tree paths; unrelated files are excluded. This completes recipe verification for the observed loaded and selected on-demand artifacts, not final licensing or publication readiness.

`correspondence-coverage.json` records the exact inventory size and digest of the private per-artifact evidence index: 60 loaded modules and five on-demand artifacts, covering 64 distinct module names. Every entry references a named, nonempty comparison record; the alternative GPS WMT remains separate. This is an audit-navigation aid, not an automatic release pass.

The current-source audit checked the selected external build manifests against the release tree. The WLAN group was rebuilt after its shared build recipes changed and again matched its installed reference. The modem recipe change was only an added license comment, with all make instructions identical. Private camera and encoder inputs were hash-checked and regenerated; their headers matched the verified builds byte for byte. Future build records distinguish `input_sha256` from the generated target’s `sha256`. Subsequent camera calibration and bundled encoder-mode builds reproduced the verified modules byte for byte; their scoped provenance checks now pass. Panel/WLAN disposition and final license, privacy and complete clean-release build checks remain open. See `release-status.json` for current check values.

## Deterministic initramfs ownership

The full build wrapper overrides initramfs UID/GID mapping to root without changing `kernel49.config`. An incremental Image.gz/configured-module rebuild passed with no observed compiler warnings or errors. Parsing the previous and new archives confirmed that only the init entry’s UID/GID fields changed; executable bytes, mode, timestamp and all other entry fields were retained. Independent direct archive generation produced identical archive bytes. This removes build-account ownership from that archive, but does not establish cross-host reproducibility of the entire kernel or replace the pending fresh complete-release build. The historical source-to-device reconstruction remains separate evidence.

## Fresh normalized kernel build

The updated full wrapper completed from a new output directory with the saved configuration, all 11 configured modules and no observed compiler warnings/errors. Its initramfs matches the earlier normalized archive byte for byte. Comparing the two decompressed 26,804,232-byte kernel images found exactly 40 differing bytes, confined to two 20-byte GNU build-ID descriptors; both note headers were verified. Every other image byte matches. This is same-toolchain output-directory comparison, not cross-host byte reproducibility or another live-device test.

All 21 external build groups subsequently passed from new output directories against that clean kernel. Of 103 output artifacts (including duplicated dependencies), 102 match prior verified build artifacts byte for byte. The remaining WMT artifact has equal allocated contents except build ID, allocated relocations, global symbols and layout; differences are confined to debug/symbol metadata and build ID. `clean-build-review.json` records the scope and limitations. This passes the clean-build check while panel/WLAN disposition, final license and privacy reviews remain separate.
