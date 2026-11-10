# MaiChat 2.2.56 GitHub Release Plan

> **For agentic workers:** Use superpowers:subagent-driven-development for the independent packaging tasks. Develop on main; do not create branches. Root owns the version line, frozen commit, Windows package coordination, tag, draft and publication. The Mac peer owns Mac packaging/uploads. Never send ZIP files or chunks through IM.

**Goal:** Publish complete Windows x64 and Apple Silicon Mac packages for the latest merged functionality as a new GitHub release, preserving v2.2.53 and the older 2.2.55 candidate files.

**Architecture:** Change only the shared Desktop CMake version line to 2.2.56 over feature commit b8782ef6 and device documentation c94b4055. Both platforms build the same frozen commit using their existing normal SDK caches and packaging scripts. Publish only after actual archive/payload/runtime gates and server asset digests match the platform manifests.

**Tech Stack:** CMake/Ninja/MSVC/Qt, NSIS/7-Zip/PowerShell, macOS package tooling, gh CLI/Release API.

## Task 1: Freeze the common source

- [x] Read actual GitHub state: latest public release is v2.2.53; v2.2.54/v2.2.55/v2.2.56 tags do not exist. Keep the published release and all its assets unchanged.
- [x] Set `project(MaiChatDesktop VERSION 2.2.56 LANGUAGES C CXX)` in `MaiChat/desktop/CMakeLists.txt`; do not change iOS/Android versions, feature sources, SDKs or flags.
- [ ] Commit/push the version line and this plan; record full SHA and MaiChat/Core trees. Send the exact freeze to both packaging owners. Create the new tag/draft against this commit without replacing any existing ref or asset.

## Task 2: Build and verify Windows complete assets

- [ ] Reconfigure/build the normal Windows Release cache at the frozen source and wait for windeployqt/PE gates to finish. Preserve all old stage/package/install directories.
- [ ] Run `MaiChat/desktop/scripts/package-windows.ps1` and `make-installer-windows.ps1` into a new 2.2.56 output root. Verify each resolved output target is within that new root before any script can remove a staging directory.
- [ ] Test actual ZIP CRC/extraction and actual NSIS compressed payload extraction without executing the installer. Compare every application file to its corresponding stage; audit PE dependencies and all five bundled models.
- [ ] From portable stage, actual ZIP extraction and actual installer extraction, run ordinary/debug smoke with system-only PATH and private data directories. Bind each actual process/version/hash and loaded Qt modules to that copy.
- [ ] Produce four complete Windows assets: setup EXE, portable ZIP, `release-manifest-win64.json`, `SHA256SUMS-win64.txt`. Record exact sizes/SHA256 and gate boundaries.

## Task 3: Build and verify Mac complete assets

- [ ] Mac safely synchronizes the exact common source while preserving its four independent old documents; normal configure/build with the existing pinned Qt SDK and flags.
- [ ] Build new DMG and ZIP with `package-macos.sh`; verify stage, read-only DMG and actual ZIP extraction file/symlink equality, signatures, target Mach-O dependencies and all five models.
- [ ] Run six ordinary/debug bare-PATH smoke processes from those actual bundles with private HOME/data isolation; retain real process/module traces. Report the actual macOS runtime and signing/notarization boundaries.
- [ ] Produce four complete Mac assets: DMG, ZIP, `release-manifest-macos-arm64.json`, `SHA256SUMS-macos-arm64.txt`. Upload only these Mac filenames to the common draft; do not edit tag/body/publication or Windows assets.

## Task 4: Verify GitHub assets and publish

- [ ] Root prepares dated, categorized user-facing release notes for session-specific models/effort, GLM native retrieval, long-history checkpoints and the verified desktop tool improvements since v2.2.53. Keep physical iOS menu/connection acceptance, paid model behavior and desktop installation boundaries explicit.
- [ ] Upload Windows assets and independently check all eight server filenames/sizes/SHA256/state against the exact local/peer manifests. Download the four small metadata assets and compare actual bytes. On transfer errors, inspect server state first; never clobber a good asset.
- [ ] Publish the new draft as the normal latest release after all package and server gates pass. Read back draft/prerelease/latest flags, full target SHA and peeled remote tag, all eight assets, and unchanged v2.2.53 body/asset identities.
- [ ] Save final diagnostics and receipts under `docs/diagnostics/` and the ignored release cache. Send the public GitHub release link; do not send archive attachments to IM.

## Independent iOS device follow-up

The normal user App 2.2.4(204) is already installed from b8782ef6. The final read-only menu check is a separate Mac-owned device task and must not trigger rebuilding/reinstalling the user App. Its pending device connectivity/unlock condition does not change the Desktop release source or justify claiming that menu acceptance passed.
