# Desktop 3.1.0 Windows publication

The user explicitly requested GitHub Release publication on 2026-11-20.
This release provides Windows x64 artifacts; no new Mac package is claimed.
The installed Windows application and user data were not changed.

- Source/version commit: `b20688bab97eee2880450aba05d73db53b37d5ed`.
- Functional integration: `940c459ce4139bc9bd84b95423d0f56197a06ae8`.
- Annotated tag: `v3.1.0`, peeled remote commit verified as the source above.
- Public release: https://github.com/wanhongqing123/MaiChat/releases/tag/v3.1.0
- Anonymous GitHub API independently confirms it is the published latest release.
- Previous `v3.0.1` release identity, target, publication time and all eight asset
  IDs/names/sizes/digests were read before and after and remained equal.

## Normal Windows artifacts

The normal Release cache was rebuilt after changing the authoritative Desktop
CMake version to 3.1.0. App deployment and PE/plugin checks passed. Cached Qt
runtime hashes match the previous public SDK; the existing Qt source changes
were preserved rather than rebuilt or included in the main commit.

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `MaiChat-win64-v3.1.0-20261120-b20688ba.zip` | 261132838 | `c81846caa90aeacc6829913f3c0c28ae81e14e9670bf5416be029f1a009f8b3e` |
| `MaiChat-Setup-win64-v3.1.0-b20688ba.exe` | 220024231 | `44c995ee96b1178b9e0c74368df94b01d98848411a8939bc7cb43f20ba8b7453` |

Application exe: 189051392 bytes, SHA-256
`2b85cf154d8c8500dad239fda9e5ae31fb13648af084465b27d1a44ca5fbff54`.
Both application and installer Authenticode status are `NotSigned`.
`release-manifest-win64.json` and `SHA256SUMS-win64.txt` accompany the two binaries.
All four server asset sizes, digests and uploaded states matched local files;
the two metadata files were downloaded and compared byte-for-byte before publish.

## Validation and limits

Normal scripts produced ZIP and NSIS Setup. The installer was extracted with
7-Zip and never executed. Stage, actual ZIP and installer payload match their
85-file manifests; only the expected installer readme differs. Complete PE
audits found 37 application binaries and no missing dependencies; installer
support adds five expected binaries.

All three directories ran `--smoke` with bare Windows PATH and fresh owned
profiles. Actual PID/path, logged version 3.1.0, packaged Qt module locations
and exe hashes were verified. Profile isolation used the existing known-folder
proof bound to the exact same packaged QtCore, plus each child's actual log path.

Functional OCR/receiver tests and Windows 150% owned-window mapping results are
in `2026-11-20-desktop-coordinate-ocr-triage.md`. Version-only release preparation
does not turn the unavailable foreground input test into a pass. Mac window-image
alignment remains explicitly unverified; the tool recommends bound display
observations or native AX geometry. No paid model validation, credential/service
change, installer execution, installed-process replacement or release overwrite
occurred. Local evidence is under `.cache/maichat-release-20261120-310/`.
