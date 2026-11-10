# MaiChat 2.2.56 Windows / Mac release

Status: **published and independently verified as GitHub Latest** at `2026-11-10T12:36:52Z`.
Release ID: `440314433`. [MaiChat 2.2.56](https://github.com/wanhongqing123/MaiChat/releases/tag/v2.2.56).

## Authorization and frozen source

The user requested complete GitHub package delivery after the session-model changes. Root owns the shared version, source freeze, Windows coordination, tag/draft and publication; the Mac peer owns Mac packaging and its four uploads. No ZIP or chunks were sent to IM. Existing installed desktop applications, v2.2.53 and all old 2.2.55 candidates were preserved.

- Version/tag: `2.2.56` / `v2.2.56`.
- Frozen source: `b9243c3ddb99f4f51da9657ad6bf4d4849760b7a`.
- MaiChat tree: `99c5b5944078337b6109b9078dadf27b790afb82`.
- Core tree: `b1698e10ee4494a925a7e0e69fdf50de4ec893ff`.
- Annotated tag object: `76bc5aacaaabc80dc22c0e844e7b96087a52872f`; remote peeled tag independently matches the full frozen source.
- The delta from `c94b4055` is only the Desktop CMake version line and the release plan. Feature source is unchanged from the accepted b8782ef6 cohort: Core 13/13, Android Java 61/61, iOS Simulator 3/3 plus the platform-specific Desktop/Native gates. Those functional results are not relabelled as a new full-suite run by packaging.

## User-facing changes

The dated release notes cover four cloud models, per-session model/effort persistence, GLM native retrieval, incremental long-history checkpoints and exact original-history lookup. They also include the computer display/input tools and temporary remote-image references since public v2.2.53, plus the scoped media/Chat-proxy/window fixes. Paid-model quality, visual-model autonomous desktop tasks and unperformed physical-device paths are not claimed as accepted.

## Windows actual packages

- Normal Release configure/build: exit 0, 8.819s / 13.051s, using the existing Qt 6.12 native-images SDK/cache. windeployqt and the build's PE gate finish before any packaged smoke.
- Main App SHA256: `086e4e4584817db713f7050f131b9c32e2a5ddc675471a4fdbe7baf960a4e236`.
- Actual ZIP CRC/extraction: all 81 files match the portable stage. Actual NSIS extraction: all 81 application files match the installer stage; six support/uninstaller files are separately listed. The installer is never executed.
- Portable and installer stages intentionally contain different generated use-instructions. Root's first cross-stage comparison caught that difference; the completed audit compares each archive to its corresponding stage and confirms the same App executable. No product or package change was needed.
- PE audits contain 37/37/42 binaries for portable/ZIP/NSIS copies, no missing or retired Qt imports. Five model hashes match their source inputs and the Mac manifest.
- Six system-only PATH/private-data ordinary/debug smokes exit 0, runtime version 2.2.56, PIDs `41200/45520/3200/41660/47932/47512`; five Qt/QWindows modules load from each corresponding payload. Root rehashed both complete assets, corresponding stage/extracted files, 39 indexed evidence files, six actual runtime logs and their module bindings.
- The App and installer are NotSigned. The App version is proven by startup logs; the installer has numeric version 2.2.56.0. Existing installations and user processes were not replaced or terminated.

Before upload, an inherited public manifest label `local-candidate-only` was corrected to `validated-for-github-release`; the packaging worker's no-GitHub statement was scoped to that worker. The original metadata, handoff and raw logs were preserved. Only metadata/sums changed; no package rebuild or repeated runtime test occurred.

## Mac actual packages

- Normal configure/build exit 0, 8.741s / 51.252s. Source App SHA256 `3285b70b7cc1ccdde2f818292bfc94d693dfa4ecc8b94686fa910f4750fcfa27`; signed stage App SHA256 `50e8b5bd0db02b4f247879f4e3ef227227631fe11fcae07e70436a33b2deaeba`.
- Mac executor verifies staging, read-only DMG and actual ZIP extraction: 338 files and 37 symlinks per copy, all matching; strict signatures, 26 target Mach-O dependencies, five models and six ordinary/debug bare-PATH sandbox smokes pass.
- Root receives the public manifest, safe/full receipts, command records and all six original smoke logs. Each log's length/SHA matches; actual dyld PID, main-image path, version 2.2.56/Qt 6.12.0 and QtCore/Gui/Widgets/Svg/Concurrent plus Cocoa loads belong to the corresponding bundle. PIDs: `86287/86506/86914/87019/87513/87616`.
- The supplied receipts are not a complete per-file inventory. The 338/37 payload-equality result is Mac executor evidence; Root independently inspects the receipts and original runtime logs, but does not claim to have rehashed every Mac payload file locally. The SDK audio-driver resource remains separately scoped outside the target Mach-O gate.
- Platform: Apple Silicon, macOS 27.2+; actual runtime macOS 27.2 arm64 only. Ad-hoc signed, not notarized; no macOS 14.4 runtime compatibility claim. OpenCV's real build label is `b9243c3d-dirty`, caused only by four preserved independent root documents; product/index are clean. The label was not rewritten.

## Uploaded complete asset identities

| Asset | Bytes | SHA256 |
| --- | ---: | --- |
| MaiChat-Setup-win64-v2.2.56-b9243c3d.exe | 201072882 | `fb1cb2fcf5499197d6478f19ffba6712aa5a70cac826206992f740d129d17966` |
| MaiChat-win64-v2.2.56-20261110-b9243c3d.zip | 241514910 | `abf809a8701203aacce2280a4a959450516b126ae55f2cda6071393e29d5056b` |
| release-manifest-win64.json | 6108 | `5387620a0afd532e81f50fcd7e8d563d184aa93c25ae2d47bc0be6f851f76e29` |
| SHA256SUMS-win64.txt | 314 | `12209e7ed375dcc71838b0370c0969ef26dbd17725487141f7788399bd4e4274` |
| MaiChat-macos-arm64-v2.2.56-20261110-b9243c3d.dmg | 213976841 | `4f3d19dcd2c8a8ab5f0fa3557ced5976ecb650baaa39f4a2c2626cf36c402664` |
| MaiChat-macos-arm64-v2.2.56-20261110-b9243c3d.zip | 214548982 | `021d34e3df145c1adf468c95fc033334d87c764b588fa3d72a94e10bea74a347` |
| release-manifest-macos-arm64.json | 4358 | `67a74f22ff95ed75cdf56e2ac909e8e3f1a61e84146b477af368f3132319618f` |
| SHA256SUMS-macos-arm64.txt | 332 | `d69e9d3a563b541e4d3bc7c4570c8d7c29a8cefceef504bdff6dccf524634a9c` |

## Transfer recovery and public verification

Windows uploads finish with all four server digests matching. Mac large-file transfers initially encounter bad-record-MAC/empty-response/EOF errors; all processes end before the next same-name POST. Correct assets are preserved. Process-only real-DNS/HTTP1/rate-limited transfer succeeds for DMG; ZIP succeeds with fixed TLS1.2/real-DNS/2 MiB rate limit, HTTP201/exit0, 108.182s. No system proxy/route, source or verified package is changed. No correct asset is clobbered.

Before publication Root checks all eight server names/sizes/digests/states, downloads all four metadata assets and verifies actual bytes/hashes, compares five model identities across platforms, reads the remote peeled tag, and checks v2.2.53 body/assets/tag unchanged. Publication uses the already authorized normal Latest release. Fresh API readback confirms draft=false, prerelease=false, Latest ID=440314433, exact target and all eight unchanged asset identities. The anonymous public page is also available; the Mac peer independently confirms its Latest marker, body and four metadata downloads.

Local evidence is retained under `.cache/release-2.2.56-20261110/`; the same-name JSON records final identities and verification scope. These are eight manually uploaded assets (four complete packages plus four metadata files); GitHub also supplies its automatic source archives.

## Separate iOS device follow-up

iOS normal App 2.2.4(204) was already signed, covered over 203 and initially launched from b8782ef6. The user subsequently reports unlocking, but the device tunnel/transport is unavailable and the private verifier reports CoreDevice4016; this is not a normal-App failure. Read-only menu acceptance remains pending restored device connectivity, with no repeated normal-App installation. This Desktop release neither blocks on that check nor claims it passed. See [device204 record](maichat-ios-device-update-204-20261110.md).
