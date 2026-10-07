# Release audit

> Partial source snapshot: panel and WLAN are omitted. Prior full-build evidence below is historical. See [current publication scope](partial-release.md).

Status: **in progress — do not publish a release or flash this checkout**.

## Target evidence

The most recent available installation records identify `7.1.0-rabbit-r1+`, build 49. Two independently retained local copies of the installation kernel agree:

```
SHA256(Image.gz) = 46560253fe0264cb27e5a162ff1ef73c02a2e9ed37c45ebf82da9ca6bb644f12
```

Read-only device inspection on October 7 confirms the running release/build, exact configuration hash, and active-slot boot kernel hash match the retained build-49 evidence. The source working tree's latest build is build 50, whose USB PHY lifetime experiment was documented as uninstalled. That experiment is excluded here; it is not silently folded into the target.

The saved build-49 configuration is retained as `kernel49.config`. The working configuration matches that saved configuration. Neither fact alone proves the complete source matches the installed executable.

## Publication requirements

- [x] Obtain current kernel, config, and installed module hashes through read-only device inspection.
- [x] Verify main-kernel source correspondence against the installed image.
- [x] Verify external-module correspondence within the recorded loaded/on-demand and startup-loader scope.
- [x] Reconstruct the embedded init source and verify its build matches the original executable byte for byte.
- [x] Build the kernel and all 21 supported external groups from clean output directories with pinned external inputs; verify correspondence as scoped in `clean-build-review.json`.
- [x] Exclude the restrictively marked panel data from the publication tree; verify all 40 steps can be generated identically from an explicitly supplied private input.
- [ ] Review the private panel dependency documentation and publication exclusion gate.
- [x] Complete nondeferred file-level notice/provenance review; see `nondeferred-license-review.json`. Panel/WLAN scope remains deferred.
- [x] Review the source payload, secret-scan findings and local preparation-commit metadata; see `privacy-review.json`. Later edits require renewed review.
- [x] Prepare the hardware support matrix in `hardware.md`, tied to the recorded loaded/on-demand artifacts; public publication is not performed.
- [ ] Final staged-tree review before any public push.

## Decisions already made

- Do not import private development Git history, build outputs, local paths, device dumps, credentials, calibration data, or private logs.
- Preserve upstream author attribution, license notices, and public contributor metadata. These are not personal device data to redact.
- Do not copy the old prebuilt embedded `init` into the public source tree. `tools/rabbit-r1/minit.c` reconstructs its source; a static GCC 16.1.0 `-O2 -s` build matches the original SHA256 `d4f101fa165966f533a823b7f70c4c26f0dff0ab1dd3c17b4e1be92bb07d24be`. The binary is generated only in the build output.
- Exclude the upstream precompiled `tools/testing/selftests/tc-testing/action-ebpf` fixture from this source-only staging tree; review its source-based replacement before advertising that selftest.
- A checked-in SGX selftest signing key is a known upstream test fixture, not an owner credential. Confirm it against the pinned upstream before allowing the scan finding.
- All 21 supported external groups have packaged recipes and clean-build evidence. Experimental probes are not automatically included merely because they exist locally.

## Hardware limitations to retain in release notes

Existing development reports identify unresolved USB-C interrupt storms and incomplete deep-suspend/modem coexistence. Camera positioning requires a checked physical reference after a new boot. These statements must be reconciled with the final live inventory and should not be presented as newly verified by this source audit.

No device writes, service restarts, module loads, camera operations, motor movement, modem commands, reboots, or flashing are part of this audit.

## Local validation completed

- All 41 top-level upstream Git objects match the public base commit.
- Configuration extracted from the retained build-49 kernel matches `kernel49.config` exactly (SHA256 `d67dd7d67bb53460f9f4fe6595c4636b4c67019663b770785228b899d15941f3`).
- Reconstructed embedded init matches the historical executable byte for byte.
- Generated private panel sequence matches all 40 original steps; five parser tests pass.
- Clean compile-only Image.gz build passes; subsequent full configured kernel and all 11 in-tree modules build with the reconstructed init/private panel input. No compile warnings were found in those logs.
- Rebuilt `vmlinux.symvers` exactly matches the development build's current symbol export table. The older saved `Module.symvers` was stale; it is not treated as authoritative ABI evidence.
- ARM64 module metadata parsing verified against all 11 rebuilt modules.
- Preliminary payload scan found no private-term, token, private-key, ELF-binary or database findings after allowing the exact upstream SGX test fixture. This is not a comprehensive certification.

The normalized public build is not byte-identical to the installed image. A subsequent isolated private reconstruction matches all 26,804,232 bytes except two verified 20-byte GNU build-ID fields. Historical identity, archive/version timestamps and the initcall line-number effect of an added license notice were accounted for explicitly. Main-kernel source correspondence now passes; see `kernel-correspondence.json`. Panel/WLAN input disposition and final release checks remain open; external-module correspondence has since passed within its recorded scope.

The read-only `device-inventory.py` helper keeps raw evidence private. Subsequent correspondence and loader reviews are recorded in `external-correspondence-review.json` and `correspondence-coverage.json`.

## Fresh build and verification tooling follow-up

The finalized build wrapper completed from an empty output directory with exit status zero, producing Image.gz and all 11 configured in-tree modules without observed compiler warnings or errors. The saved build-49 configuration is unchanged; the embedded init retains its verified reference hash. This passes the kernel recipe check only; the complete release build still needs its external modules.

Twelve parser/matcher tests pass, as does a synthetic inventory comparison using all 11 rebuilt ELF modules. Source-output boundary checks cover symlink aliases. A scan of 93,934 source files found no configured private terms or token/key/binary findings. Source-check CI is prepared but not yet run on GitHub. None of these checks constitutes live device acceptance or complete license clearance.

## Live module evidence

`module-reference.json` records all 60 loaded modules observed on October 7 without device-specific identifiers. Of these, 54 match local full-file artifacts and loaded build notes; two storage modules also match their active boot ramdisk files exactly. The DRM duplicate files have identical allocated contents, relocations, global symbols and layout. Three further modules were rebuilt from available source with matching allocated contents (except build IDs), relocations, global symbols and layout.

This resolves the observed loaded-module artifact inventory, not complete source publication or hardware acceptance. Source packaging/rebuilds, dependency notices, and installed but unloaded/on-demand drivers remain required. No service, kernel module, camera, motor or boot state was changed. A broad read-only inventory process was stopped and replaced with a targeted scan.

### Expanded on-demand scope

The loaded-module snapshot is not the full required set. A read-only review of 39 configured service units found a Bluetooth transport module and launcher-triggered GPS module, plus a GPS fallback WMT variant with different executable code. Their file hashes match saved local artifacts. Separate Bluetooth and GPS build groups now reproduce their code/data and relocation referents with documented build-ID/source-path normalization. This includes the alternate GPS WMT implementation; final licensing and dynamic-loader coverage review remain pending. No driver or test service was started. The installed charger-disable flag excludes optional MT6370 charger modules from the selected startup path. The selected variable-based loader paths were subsequently reviewed; see `module-loader-review.json` and `external-correspondence-review.json`.

### Licensing and packaging review

Original Spectre kernel contributions use **GPL-2.0-only**, with existing notices preserved; see [licensing policy](licensing-policy.md). The [redistribution review](redistribution-review.md) records the panel and four WLAN input questions as unresolved context, not proven redistribution prohibitions. Camera provenance and encoder mode-data review now pass. Engineering work can continue while final publication review remains open.

### Variable-based startup loads reviewed

Five installed startup scripts were read without execution to resolve audio/JPEG/USB module loops and the selected support/bootstrap paths. Every resolved name belongs to the verified loaded-module set; no new driver was found. Script hashes and resolved names are recorded in `module-loader-review.json`. This closes those specific traversal gaps while preserving the final review requirement for other dynamic paths.
