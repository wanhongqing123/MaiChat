# MaiChat Desktop 3.0.1 release plan

The user explicitly authorized working with mac-maichat to build the latest
packages and publish a GitHub Release. That instruction resumes peer cooperation.
Root owns the shared version, source freeze, Windows package, tag/draft/body,
upload windows and publication. The Mac peer owns clean Mac builds and supplies
four verified assets. Installed applications, services, drivers and TCC settings
are unchanged. Mobile patches stay outside this Desktop publication.

The user then explicitly selected version **3.0.1** for this larger update. The earlier 2.2.66 preparation never created a tag, draft or assets.

Live baseline verified on 2026-11-17: latest public release `v2.2.65`, eight
assets, source `c3ea0bbbbb63f09b67329f57b31fd5e6b92c554c`. No `v3.0.1` tag
exists. Remote main `95b65d6e` is an ancestor of Root `c06a9d5c`. Preserve every
existing release, asset and tag.

## Source integration before version freeze

- [x] Read actual remote state, packaging scripts and prior release procedures.
- [x] Identify completed Desktop WIP: Starter model-efficiency copy, PDF/MD file
  thumbnails and their complete Markdown/PDF/platform/UI dependency graph.
- [x] Verify/apply Mac's latest six navigation files once against exact before
  blobs; all postimages match `5d4dedbf...` packet.
- [ ] Close reproduced loading-first-Paint geometry and UTF-8 truncated-EOF
  regressions; close the completed-future full-page retention lifetime defect.
- [ ] Run fresh normal renderer, Markdown, file-delivery UI, Starter and affected
  MainWindow selectors. Inspect owned captures. Do not substitute old 2.2.65
  peer runtime evidence or ready-only UI tests for this integrated source.
- [ ] Commit only reviewed Desktop integration directly on main. Preserve raw
  `.gitignore`, MobileAgent adapter/tests and all unrelated platform WIP.
- [ ] Set the single Desktop CMake version to `3.0.1`, commit and push the
  final common source. Record full commit/component trees and five model hashes.

## Windows package

- [ ] Configure/build the normal Release cache at the exact frozen source,
  forcing restored/changed inputs to recompile. Complete App post-build deploy
  and PE gates before launching an App executable.
- [ ] Create a fresh version/date/SHA output root; verify resolved containment,
  disjointness and every existing ancestor's non-reparse status before package
  scripts can delete/rebuild their exact staging directory.
- [ ] Produce portable ZIP and NSIS Setup without executing the installer.
- [ ] Check actual ZIP CRC/extraction and NSIS payload extraction, compare all
  application files with each corresponding stage, audit the complete dependency
  closure, models, resources and license files.
- [ ] Run ordinary/debug bare-PATH isolated-profile smoke from stage, real ZIP
  and real installer payload. Record actual PID/path/version/EXE/plugin hashes.
- [ ] Produce `release-manifest-win64.json` and `SHA256SUMS-win64.txt` for the
  two final archives and manifest. Windows signature status must be factual.

## Mac package

- [x] Peer confirmed free normal slot, 38 GiB space and a separate main-project
  clone on main. Original working tree and installed App remain untouched.
- [x] Peer verified an existing pinned DMG-tool environment; no new dependency
  installation is required. Only Apple Development identity is available, so
  the public desktop artifacts will report ad-hoc signing and no notarization.
- [ ] Send exact version/freeze to peer; clean clone must resolve git SHA and
  CMAKE_HOME_DIRECTORY to that source, not its old c3 parent. New main-project
  build objects; reuse only verified existing SDK/vendor/model inputs.
- [ ] Receive Mac arm64 DMG, ZIP, platform manifest and checksum file, with actual
  read-only DMG/ZIP bundle, closure, signature and isolated bare-PATH smoke gates.
  Source/tree/model identities must match Windows/common freeze.

## GitHub publication

- [ ] Root creates a new annotated source tag and same-source draft with accurate
  user-facing notes. No strict Sunshine equivalence or excluded-device claims.
- [ ] Upload Windows and Mac asset windows serially; inspect server state after
  any interrupted transfer before retrying. Do not overwrite a correct asset.
- [ ] Verify all eight names/sizes/digests/states against local and peer manifests;
  download the four metadata assets and compare exact bytes.
- [ ] Publish as a normal latest release only after all package/server gates.
- [ ] Independently read public page/API/latest, peeled tag, assets and unchanged
  old release identities. Return the public release/download link to the user.

Evidence root: `.cache/maichat-release-20261117/`. GitHub plugin lacks Release
asset upload/publication APIs; use the existing authenticated `gh` workflow for
that explicitly authorized action. No archives or chunks are sent over IM.
