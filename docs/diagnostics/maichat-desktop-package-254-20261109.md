# MaiChat 2.2.54 desktop candidate packages

2026-11-09. This is a local candidate delivery requested by the user. It creates
no public GitHub Release or tag and replaces no installed application.

## Source and functional scope

- Frozen source: `adfff832723595355a47db88430478cc60637d9e`.
- MaiChat tree: `861cbcb3c8e9bd7113bcd380b817054780b4e44b`.
- Core tree: `69e18eb1e4b587b30ad8b467dd3172bea0443b8e`.
- Function change: `ce90da98`, the Mac player title restore action no longer
  queues maximization after native fullscreen. The version commit changes only
  the desktop version source and packaging plan. Windows player behavior is
  unchanged, and this package has no font correction.

Existing product suites are reused for unchanged functional input. Both hosts
normally rebuild the actual new-version App, and verify actual new packages.

## Windows x64

Normal configure/build and deployment succeed, including the three-binary,
114-import build gate. Actual App version 2.2.54 is recorded by every smoke child.
App SHA256: `5f30a24d0a322041319e04aebbb849b6e224eb9eba7fa704a3b096d29c591e38`.

| Artifact | Bytes | SHA256 |
| --- | ---: | --- |
| MaiChat-Setup-win64-v2.2.54-adfff832.exe | 200952507 | `3a1b39e329ac5a4d18d003855de5d30de84250d26212fe391963de4663142cb7` |
| MaiChat-win64-v2.2.54-20261109-adfff832.zip | 241337577 | `45b1378865ed36ae12c8009219925c029e3e883d4b4feee38565e6b94c76567d` |

ZIP CRC and all 81 application file hashes match portable staging. The actual
NSIS installer is extracted with 7-Zip; all 81 application files match its
independent staging, with six additional NSIS support files. The installer is
never run. The three full dependency audits cover 37/37/42 PE files with no
missing or retired Qt modules. Five models and the official Portrait/ORT gate
pass in each copy. D3D/compiler runtime resources match the normal build.

Six ordinary/debug-plugin smoke children use system-only PATH from portable
staging, actual ZIP extraction and actual installer payload. All exit 0, record
version 2.2.54 and the same executable hash, and load Qt/plugins from their own
copy. PIDs: 35168/34284, 40732/33124, 21540/11980. Only these owned smoke processes
run; no account login, installation or existing application termination occurs.
Both the App and installer remain `NotSigned`.

An initial plain-shell package omitted three auxiliary runtime/support files
because the VS environment was missing. Its78-file outputs and logs remain in
`attempt-no-vs-env`. The final candidates were recreated with the normal VS
environment and all 81 files; the earlier direct-import gate is not treated as
proof of a complete final package.

Private evidence: `.cache/release-254-20261109/`, including build-complete.json,
package-audit-verification.json, candidate-delivery-verification.json, six smoke
receipts, manifests/checksums and original logs. The original Windows title
chrome regression remains3/0 for the exact same Player blobs.

## Mac arm64

The Mac peer normally built the same frozen source and generated fresh DMG/ZIP.
Root reviewed its raw receipts/logs, SHA manifest and all four native screenshots;
Mac execution is performed by the peer, not by this Windows host.

| Artifact | Bytes | SHA256 |
| --- | ---: | --- |
| MaiChat-macos-arm64-v2.2.54-20261109-adfff832.dmg | 213780883 | `3307bd64aeadf5e76a86a78134e25b1bf50e9b6b64176bedb13cee3fdc38f724` |
| MaiChat-macos-arm64-v2.2.54-20261109-adfff832.zip | 214363157 | `e3499f820c13f8d57f9d1308a41df5388f182a57c9a233a67b73979bfcbe9f5b` |

Source App SHA256:
`5fa5d1e76bcd3be7ba869988f7425734a855dba501327abe6b320d116a4a75b6`.
Signed App SHA256:
`475c91e976752166cd0a5f4b42f9e8053e249d25d691392c7676ca2b13c49b7a`.

Stage, readonly mounted DMG and actual ZIP extraction have identical 338 regular
files and 37 symlinks. Strict signatures, 26 target Mach-O audits, five models,
ORT, DMG layout/integrity and ZIP CRC pass. Six ordinary/debug children exit 0;
their five Qt frameworks and Cocoa plugin load from their own bundle under bare
PATH with network/source SDK/build/Homebrew/real AppData denied. PIDs:
52989/53203, 53717/53878, 54258/54402.

The latest native Player fixture, including DidExit/capture, now passes 6/0/0
in 27.796 seconds after unlock. DidExit count reaches 1 and the restored 640x480
window stays stable. Four before/restored screenshots show the owned test video
and controls. Original native geometry cases report 0px mismatch and 0 extra
animations. This supersedes the previous locked-desktop pending entry.

Four real MainWindow messageVideoButton entry cases cover normal/maximized/
fullscreen owner with 1920x1080 SDR, plus normal owner with HLG. No player resize
or fullscreen request is injected. All open ordinary 1285x723 windows within
the existing 85 percent bound. The user's initial automatic-fullsize report was
not reproduced; its cause remains unconfirmed.

Mac execution is verified on macOS 27.2 arm64 only. Packages are ad-hoc signed,
not notarized, and do not establish macOS 14.4 runtime compatibility. The raw
proof ZIP is 4046596 bytes, SHA256
`0c1740a1e671149d46a3652436049cbf8ca6f28e205947e2b3ecf018d95f19f3`.
It is archived under the Windows evidence directory's `mac-proof/`.

## Delivery and open issue

Both hosts submit their candidate packages and metadata to the user through IM.
Windows small metadata requests 1192/1193 are confirmed `sent-to-im` with an IM
message id. Original EXE/ZIP requests 1194/1195 failed with code 2402: file exceeds
the actual 100 MB IM limit. The same failure is independently observed on Mac.
CLI exit 0 only meant queue acceptance; its advertised 20 GB limit does not describe
the effective SDK/server limit. These original large files were not delivered.

Delivery therefore uses 80 MiB binary parts, preserving the already verified
original package bytes. Per-part hashes and platform merge helpers accompany
the parts; final reconstruction must match the original full-package SHA256.
Windows actually reconstructs both complete packages, independently with only
the corresponding three parts present, and verifies byte equality and complete
SHA256 against the originals. Existing equal outputs are skipped unchanged;
conflicting destinations are refused without overwriting. A small tools ZIP
contains the helpers and Chinese instructions.

However, actual SDK state confirms all six Windows 80 MiB part requests
1208–1213 also fail with IM send timeouts and no remote message id. Only tools
request 1207 is `sent-to-im`. Mac reports the same outcome for its six parts.
Those failed parts were not delivered and are not retried.

The final transport uses distinct `chunk16` names and 16 MiB parts. A single Mac
trial succeeds in about 7.96 seconds. Both complete sets then send serially,
waiting for an actual `sent-to-im` state and IM message id before enqueuing the
next file. Windows confirms 28/28 files: tools ZIP 1221, installer 12 parts 1222–1233,
portable 15 parts 1234–1248, zero failures or retries, 100.424 seconds. Mac confirms
29/29 files: 13 DMG parts, 13 ZIP parts and three helpers; its original small
manifest/checksums 1727/1728 are also confirmed. Recipient downloads are not
observed, so the claim is delivery to IM rather than download completion.

Both Windows `chunk16` package reconstructions are tested independently with
only their own part sets, byte for byte and SHA256 equal to the original complete
artifacts. Existing equal outputs are skipped and conflicting outputs preserved.
The small tools ZIP contains the matching manifest, merge helper, two CMD
entry points, checksums and Chinese instructions. Use the new tools with the
new parts; do not mix them with the superseded 80 MiB attempt. Mac reports the
equivalent reconstruction checks in its final delivery receipt.

Root also asked whether this already verified version may be publicly released
on GitHub; this version's earlier authorization only specified making packages.
That separate choice remains pending. No new public Release or tag is created.

The Windows black-font report is confirmed on installed 2.2.53 with Noto Sans SC
and Large text. Controlled cold and runtime-switch MainWindow/timer captures
still do not reproduce its hairline glyphs. It is an open issue, not part of the
Player fix. See [font investigation](maichat-windows-noto-sans-20261109.md).
