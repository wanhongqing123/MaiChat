# MaiChat 2.2.53 macOS release verification

2026-11-09. The user authorized a new Mac/Windows package and GitHub Release after
the Qt 6 verification. The Windows coordinator owns the common version change,
tag, draft and publication. This host builds and verifies the Mac assets and
uploads only those assets to the coordinator's draft.

Reproduction and expected delivery: start from the same frozen full source commit
with desktop version 2.2.53; normally configure/build the existing Release target;
run the canonical packaging script to create a new DMG and portable ZIP. Check
the exact bundle version, executable hashes, signatures, runtime dependencies and
models in staging, a read-only mounted DMG and an actual ZIP extraction. Run
ordinary and QT_DEBUG_PLUGINS Cocoa smoke children with a bare system PATH and
private HOME, denying external network, source SDK/build/Homebrew libraries and
real user-data access. Both modes must exit zero and show Qt/Cocoa loaded from
that copy. Upload the verified Mac assets, sanitized manifest and checksums to
the same draft. Read back remote asset names, sizes, SHA256 and final publication.

Baseline: `8b717c48e137c4548811d9addbe36e5dc9c4cc9f`; the existing feature-source
evidence is Mac 18/18 affected registered suites and Windows 17/17. A version-only
change reuses that functional evidence while verifying the new actual packages.
The previous 2.2.52 release and private verification packages remain separate.

Local independent diagnostics were backed up under
`/private/tmp/maichat-release-253-20261109/before-release/` before synchronization.
No installed application is replaced or stopped. No user database, live account,
camera or paid provider is used for package verification.

Actual platform boundary: macOS 27.2 arm64. This release does not claim runtime
verification on macOS 14.4. The existing packaging flow uses an ad-hoc signature,
without a Developer ID or notarization claim. Final source SHA, asset hashes and
execution results follow below.

## Frozen source and actual new packages

Both hosts use `36db08d1efb67a20b5511b56ae66b359378c5f78`. MaiChat tree:
`b3e340463d83a6e0ee68a79941d1d55128dbe7db`; Core:
`69e18eb1e4b587b30ad8b467dd3172bea0443b8e`. Only the desktop version and root
release plan changed after the baseline; there are no new functional changes.
The local fast-forward preserved the independent diagnostics. Normal configure,
normal maichat build and the canonical full packaging script all exited zero.
The actual Info.plist short/build versions are both 2.2.53. All ten Qt package
and Tools directories still select install-widgets-native-images, Qt 6.12.0.

Source executable SHA256:
`62b5954a186d31b33ac9d282f8f35b08e0d7de87a6f55ecb6616da79d0f09841`.
Signed executable SHA256:
`2441cb61c93865d5c0ad882e2f6cefee775d8560fa0ac92d05e186a97f5406f7`.

- DMG: `MaiChat-macos-arm64-v2.2.53-20261109-36db08d1.dmg`, 213780637 bytes,
  SHA256 `0dc5f3a435989e6db6ba60fbcc2c7606bbe02482288f55134a3614806aec03e8`.
- ZIP: `MaiChat-macos-arm64-v2.2.53-20261109-36db08d1.zip`, 214363053 bytes,
  SHA256 `85e1f837a84256d71ea6430957fd8b392e808044a0ecb7ec1f15a01a111d7502`.

Staging, the actual read-only DMG and the actual ZIP extraction have identical
338 regular-file hashes and identical symlinks. All three strict signature and
26 Mach-O dependency/architecture audits exit zero. The vendor's separate
non-target CoreAudio driver resource remains explicitly listed by the audit.
All five ONNX file hashes (four Portrait models plus RVM) match the prior accepted
inputs; ONNX Runtime is present. DMG integrity, Finder metadata readback, ZIP CRC
and extraction pass. Only Qt Core/Gui/Widgets/Svg/Concurrent frameworks are in
the bundle, with the retained UI plugin closure and no retired image plugins.

Ordinary/debug Cocoa sandbox children all exit zero: staging PIDs 96528/96692,
read-only DMG PIDs 97128/97283, extracted ZIP PIDs 97667/97821. All six original
DYLD logs show the five Qt frameworks and Cocoa loaded from that exact copy,
with bare PATH and the stated source/library/network/user-data denies.
The app-only comparison from the prior feature delivery is not substituted for
these new actual package checks.

Private reproducible commands, original logs and complete receipt are in
`/private/tmp/maichat-release-253-20261109/`. The original proof ZIP is 365062
bytes, SHA256 `fc243d801cf00975c66915114549fee51858ac1e1ed17edd015fd56b39d7e3be`,
and was sent to the Windows coordinator. Public assets include a sanitized
release-manifest-macos-arm64.json and SHA256SUMS-macos-arm64.txt; raw host logs
are not release assets. The shared draft is ID 438802912, tag v2.2.53, targeting
the frozen source. Final remote upload/publication receipts are kept separately.

## Publication

Published by the Windows coordinator at 2026-11-09 06:54:29 UTC:
https://github.com/wanhongqing123/MaiChat/releases/tag/v2.2.53

An independent GET confirms draft=false and all eight assets uploaded with exact
digests against the local Mac inputs and the actually downloaded Windows
checksums/manifest. The source target remains the frozen 36db08d1 commit.
Mac asset IDs: DMG 726495188, ZIP 726521708, manifest 726480156, checksums 726480157.
The publication inventory is all-eight-assets-receipt.json in the private task
directory. No installed application was replaced.

The first concurrent gh upload reported an existing checksum name after the two
small files had uploaded correctly. Both were verified and retained. A separate
gh DMG upload then failed with unexpected EOF; its HTTP/1 retry succeeded. The
gh HTTP/1 ZIP request ended with a closed network connection. A final single
curl HTTP/1 ZIP POST completed in 66.571 seconds with exit zero and the exact
server size/digest. No package bytes, source files or correct remote assets were
changed during transport recovery. The IM backup requests had no remote URL at
the time of inspection and were not treated as successful transfer evidence.
