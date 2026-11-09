# MaiChat 2.2.53 Windows / Mac release

Status: published and verified as GitHub latest at `2026-11-09T06:54:29Z`.
Release ID: `438802912`. [MaiChat 2.2.53](https://github.com/wanhongqing123/MaiChat/releases/tag/v2.2.53).
All eight uploaded asset names, sizes, SHA256 digests and states match the
verified local packages; four metadata files were actually downloaded and compared.
The previous v2.2.52 body and all eight existing asset identities are unchanged.

## Authorization, source and scope

The user authorized new packages and GitHub Release publication after Qt 6
verification. Root owns shared version metadata, Windows packages and publication;
the Mac peer owns its four Mac assets. Existing installed applications and the
previous release remain untouched.

- Version/tag: `2.2.53` / `v2.2.53`.
- Frozen source: `36db08d1efb67a20b5511b56ae66b359378c5f78`.
- MaiChat tree: `b3e340463d83a6e0ee68a79941d1d55128dbe7db`.
- Core tree: `69e18eb1e4b587b30ad8b467dd3172bea0443b8e`.
- The code delta from `8b717c48` is only the shared desktop CMake version line;
  the other new file is the release plan. Existing Windows 17/17 and Mac 18/18
  affected suites, including native raster 48/0/0, are reused for functionality.
  New version packages are separately rebuilt and verified below.

## Windows actual artifacts

Normal configure/build and post-build runtime gates succeed. Six owned smoke
children prove runtime version 2.2.53 and the same actual executable SHA in the
portable stage, actual ZIP extraction and actual NSIS extraction. Each copy runs
ordinary/debug mode with a system-only PATH and loads Qt/QWindows from itself.

- Actual App SHA256: `242b202264fdb203cf059236d1b727795149522a2af2aae9a939d1a2ffa366c9`.
- Portable ZIP: CRC passes; all 81 stage/extracted file hashes match.
- NSIS: compilation succeeds; 7-Zip extraction without installer execution proves
  all 81 installer-stage application files match the actual compressed payload.
  Its six extra uninstaller/plugin support files are separately recorded.
- PE audits: 37 binaries in portable/ZIP copies; 42 including NSIS support in the
  extracted installer; no missing or retired Qt imports.
- Five actual model files match source packaging inputs and the Mac artifacts.
- Installer numeric version is 2.2.53.0; App startup, not an absent PE VERSIONINFO
  resource, proves the main executable's version. Both are NotSigned.
- No existing process is terminated and the installer is never run.

## Mac actual artifacts

The Mac peer normally configures/builds from the same frozen commit. Root checks
its machine-readable receipt and the 41-entry original proof archive, including
all six actual DYLD module traces against their original logs.

- Host: macOS 27.2 arm64. Source App SHA256:
  `62b5954a186d31b33ac9d282f8f35b08e0d7de87a6f55ecb6616da79d0f09841`.
- Signed stage App SHA256:
  `2441cb61c93865d5c0ad882e2f6cefee775d8560fa0ac92d05e186a97f5406f7`.
- Stage, readonly mounted DMG and actual extracted ZIP: 338 regular files and
  37 symlinks all agree; each strict signature and 26 target Mach-O gate passes.
  The SDK's TRTC driver resource is separately listed outside that target gate.
- Five models and ORT verified; six ordinary/debug sandbox smokes exit 0 and
  observe all five Qt frameworks plus Cocoa from the corresponding bundle.
- DMG integrity/Finder layout and ZIP CRC/extraction pass. Packages are ad-hoc
  signed and not notarized; release requirement is Apple Silicon/macOS27.2+.
  No macOS14.4 runtime acceptance is claimed.

## Verified asset identities

The following local values are compared to each GitHub uploaded asset's size,
SHA256 digest and state before publication. Per-platform manifests/checksums are
also actually downloaded from GitHub and compared to the locally verified data.

| Asset | Bytes | SHA256 |
| --- | ---: | --- |
| MaiChat-Setup-win64-v2.2.53-36db08d1.exe | 200972427 | `fb246f21b856d8d763b6ce6e8f4c966085c06c6cae15359295ad333ed073c107` |
| MaiChat-win64-v2.2.53-20261109-36db08d1.zip | 241337642 | `c976899d9dd0e1656caef26025b0743d5442d7fd0d7938e6d9f4922dd330bde9` |
| release-manifest-win64.json | 7900 | `689f196faab6b71241ef3de96b9bad5d30c2826717784e67d55dc3d4ac448d59` |
| SHA256SUMS-win64.txt | 314 | `cc46c141d5fd9a1ee54a2108bb70c72b38beab7ec3ae929b03fdbd29fc39d333` |
| MaiChat-macos-arm64-v2.2.53-20261109-36db08d1.dmg | 213780637 | `0dc5f3a435989e6db6ba60fbcc2c7606bbe02482288f55134a3614806aec03e8` |
| MaiChat-macos-arm64-v2.2.53-20261109-36db08d1.zip | 214363053 | `85e1f837a84256d71ea6430957fd8b392e808044a0ecb7ec1f15a01a111d7502` |
| release-manifest-macos-arm64.json | 5880 | `5fc947989ec2da1b45e7e0cf76078e293fc84c8c9ab2605090a37ec1c5573068` |
| SHA256SUMS-macos-arm64.txt | 332 | `f2093a12cd3922a49709d7efdc361ef9e68504f7396e46c635514eda8401ba2f` |

## Boundaries and evidence

No installed App is replaced, no user data migrated, no live vendor/model,
camera or desktop input acceptance performed, and no SessionSync deployed.
Only Windows/macOS desktop assets are published. Detailed prior image/Qt feature
validation is in `maichat-ffmpeg-user-bitmaps-20261109.md` and its delivery JSON.
Local release receipts, raw logs and upload identities are retained under
`.cache/release-253-20261109/`; public manifests accompany the binaries.

## Transfer recovery and publication

Mac gh uploads initially returned EOF/closed-connection errors; package validation
was unaffected. The coordinator checked server state before each retry, retained
correct small assets, and maintained a single uploader for each missing filename.
The final curl HTTP/1 ZIP upload completed and its exact server SHA was verified
before publication. No existing asset was clobbered or deleted. Queued IM backup
transfers were not counted as completed downloads.

Public latest, draft/prerelease flags, full source target, peeled remote tag and
all eight asset digests were read back after publication. The source tag stays
on `36db08d1`; the subsequent root documentation commit does not change product trees.
