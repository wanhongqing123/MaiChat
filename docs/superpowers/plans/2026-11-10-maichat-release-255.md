# MaiChat 2.2.55 Candidate Packaging Plan

> **For agentic workers:** Use superpowers:subagent-driven-development. Work on main. Root owns shared version, integration and source freeze; the Windows packaging worker and Mac peer operate separate artifact directories.

**Goal:** Deliver verified Windows EXE/ZIP and macOS DMG/ZIP candidates from one source commit after the context-compaction gates pass.

**Architecture:** Reuse the normal native Qt caches and existing platform packagers. Keep the deployed 8cde service source, installed applications and earlier candidate assets intact. Root freezes the C++/fixture changes and desktop version together; both packagers verify that commit before building.

**Tech Stack:** C++17, Qt6, MSVC/NSIS, macOS native tools, existing SHA/PE/Mach-O and isolated smoke checks, IM 16 MiB serial delivery.

## Task 1: Freeze tested source and release metadata

Files: `MaiChat/desktop/CMakeLists.txt`, the 31-file Core/host freeze, `MaiAgent/tests/e2e/console_e2e.py`, and context diagnostics.

- [x] Verify v7d's 31 raw hashes and exact previous/new Git blobs. Final Core aggregate is `da292832a59f11057f9dc63323907848df810c07e460ff1f826c40c3e537a6b6`.
- [x] Build Windows Core/console targets, run 12 registered suites, and independently review the empty-history guard and image/tool pairing.
- [x] Resolve the console e2e's observed output-wait failures using the original business assertions; preserve original logs and prove the cause.
- [x] Set the common desktop version line to `project(MaiChatDesktop VERSION 2.2.55 LANGUAGES C CXX)`.
- [x] Add precise candidate notes and the final Windows/Mac source/runtime boundaries to diagnostics.
- [ ] Stage only the explicit freeze and metadata paths, commit/push main, and compare local SHA with `git ls-remote origin refs/heads/main`.

## Task 2: Windows candidate artifacts

Tools: `MaiChat/desktop/scripts/package-windows.ps1`, `make-installer-windows.ps1`, and the adapted private driver `.cache/release-2.2.55-20261110/run-windows-candidate.ps1`.

- [ ] Build `maichat` in `MaiChat/desktop/.cache/build-qt6-ui-20261109` under `vcvars64.bat`; require version 2.2.55, successful windeployqt and PE/plugin closure before executing the binary.
- [ ] Run the driver's `Package` phase with the exact 40-character frozen `-FullCommit` and `-ExpectedVersion 2.2.55`; run `Audit` with the same parameters.
- [ ] Verify fresh portable and installer stages, ZIP CRC/extraction, actual NSIS extracted payload, model hashes and per-file equality. Keep each stage inside the new private release directory.
- [ ] Run `Smoke` with normal/debug bare PATH from stage, actual ZIP extraction and actual NSIS payload: six real executions, all exit 0. Do not execute the installer against Program Files.
- [ ] Run `Manifest`; retain the EXE/ZIP plus manifest and SHA256SUMS in the driver's `assets/` directory. Check the manifest source/version against the frozen commit and actual binary resources.

## Task 3: Mac candidate artifacts

The Mac peer receives Root's exact version/source SHA and preserves its independent docs/overlays before fast-forwarding.

- [ ] Normally configure/build the frozen source; verify 2.2.55 in the actual bundle and executable.
- [ ] Produce fresh DMG/ZIP, then verify source/stage/readonly-DMG/actual-ZIP file identity, symlinks, dependency/model closure, signatures and archive integrity.
- [ ] Run normal/debug isolated package smoke from all three payloads. Record the actual macOS/architecture and ad-hoc/not-notarized boundary.
- [ ] Return package manifests, sizes, hashes and real gate receipts to Root.

## Task 4: Deliver and record

- [ ] Copy the verified complete assets to a clearly named local candidate directory.
- [ ] Deliver original packages through 16 MiB chunks. Verify merged original SHA first; send serially and wait for `sent-to-im` plus a real remote message ID before the next chunk.
- [ ] Deliver merge instructions/manifest/checksums and retain the actual IM receipts. Earlier 100 MB rejection and 80 MiB timeout records remain preserved.
- [ ] Record both platforms' final source, artifact hashes, delivery counts and remaining acceptance boundaries. This authorization is candidate delivery; no GitHub tag/public Release or installed-app replacement is included.
