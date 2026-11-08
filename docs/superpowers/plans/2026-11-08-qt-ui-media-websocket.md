# Qt UI, Native Media and WebSocket Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development. Work directly on main. Root owns shared build files and all Windows builds/runs; Mac owns only its assigned native adapter. Preserve the previously uncommitted Qt6 overlay.

**Goal:** Remove Qt Multimedia, Qt MultimediaWidgets, Qt WebSockets and Qt Network while preserving browser CDP, HTTP/download/model/session networking, proxy behavior, inline audio, video metadata and camera preview/photo behavior. Keep storage on the existing SQLite engine rather than a Qt SQL driver as the UI boundary is completed.

**Architecture:** Qt supplies widgets and UI dispatch. Browser CDP uses the vendored WebSocket++/Asio stack through a plain C++ request interface. Media metadata uses ordinary FFmpeg APIs, audio uses the existing FFplay session with explicit ownership, and camera acquisition uses Media Foundation/AVFoundation through one Qt-free interface. Keep existing SessionSync WSS/account retry policy and TRTC media dependencies intact.

**Tech Stack:** Qt 6.12.0 source modules qtbase/qtsvg/qtimageformats, existing FFmpeg/FFplay/Graphics, WebSocket++ 0.8.2/standalone Asio, Windows Media Foundation, macOS AVFoundation.

## Reproducible behavior

1. Open the Creation Center browser tool against a local isolated browser page; observe, act and capture work without Qt WebSockets. A disconnect never replays a click or text insertion.
2. Video cards display duration without starting a player. Inline audio plays/pauses/seeks/replays, pauses when switching sessions, and coordinates with music/video playback. An old card cannot control a newer owner's player.
3. Both the attachment camera button and the Agent camera tool open the same preview. The shutter enables only after a valid native frame. A user-confirmed photo returns native JPEG bytes saved as one new file; cancel or close releases capture and removes incomplete output.
4. Configuration, linker inputs, deployed packages and dependency audits contain no Qt Multimedia/MultimediaWidgets/WebSockets. Qt ShaderTools is removed from the SDK module lock because this Widgets build no longer needs its former Multimedia consumer.
5. Installed applications, account data, camera/microphone permissions and published release assets are preserved. Native tests use owned fixtures; physical camera acceptance is recorded separately.

## Baseline and ownership

- Base main: `b95e1ff006628bbf1301d7ec42749f6417442f36`.
- The Mac Qt6 overlay is preserved and imported with its exact 46-file manifest: patch SHA256 `3ef0014976d66f06fed02d76c50f3c84d1b103e32b981d11d2c1fa09dfd27b0d`. Existing new Root probe/audio contract files are backed up before importing it.
- Root: CMake, SDK module lock/bootstrap, packaging, camera public header and Qt camera dialog, legacy video fallback removal, integration, plans/diagnostics and build execution.
- CDP worker: `MaiChat/SessionSync/MaiCdpTransport.{h,cpp}`, its ordinary tests and `DesktopBrowserTools` adaptation.
- Audio worker: ordinary `MaiMediaProbe`, `DesktopAudioPlayback`, inline/audio/music bindings and their tests; only the audio handoff in the existing video dialog.
- Windows camera worker: `MaiChat/Media/MaiCameraCaptureWindows.cpp` and private/offline Windows camera tests after the common header is frozen.
- Mac peer: `MaiChat/Media/MaiCameraCaptureMac.mm` and private/offline Mac tests under the same frozen header. No shared CMake edits.

## Task 1: Preserve and verify the Qt6 overlay

- [x] Verify the supplied patch/archive hashes and apply-check against the actual base.
- [x] Back up the five existing new probe/audio files, apply the Qt6 overlay, and compare all 46 canonical file/gitlink entries.
- [x] Preserve existing Mac Qt checkouts/SDKs. Remove only the three unwanted module gitlinks/lock entries from the new formal dependency graph, without deleting peer source directories.
- [x] Acquire the three pinned source modules before building; the source build itself remains offline.

## Task 2: Ordinary CDP transport

Files: `MaiChat/SessionSync/MaiCdpTransport.h`, `.cpp`, `tests/MaiCdpTransportTests.cpp`; adapter `MaiChat/desktop/src/MaiChatTools/DesktopBrowserTools.cpp`.

```cpp
MaiResult<std::string> MaiCdpTransport::call(
    const std::string& socketUrl, const std::string& method,
    const std::string& parametersJson, std::function<bool()> isCanceled);
```

- [x] Freeze a header using only standard C++ types/MaiError, 6000 ms connect and response deadlines, and a 32 MiB assembled-message bound covering a 20 MiB PNG's Base64 representation.
- [x] Add an explicitly failing stub plus local loopback fixture tests. Root builds/runs the stub and retains a behavioral RED, rather than counting a missing symbol as a test failure.
- [x] Implement WebSocket++ plain WS numeric-loopback transport. Match request IDs, ignore notifications/unrelated IDs, return CDP errors, and send each command once. Cancel/stop wake waits; stop joins before returning and leaves no late user callback.
- [x] Run the same local tests for pairing, fragmentation, message limits, close/error, deadlines, cancellation and no replay; then adapt the existing browser manager and run its isolated local-page integration.
- [x] Keep the existing SessionSync WSS transport's authentication, TLS, routing and retry contracts unchanged.

## Task 3: Ordinary probe and owned audio playback

Files: `MaiChat/Media/MaiMediaProbe.{h,cpp}`, `Media/tests/MaiMediaProbeTests.cpp`; `desktop/src/ui/DesktopAudioPlayback.{h,cpp}`, `DesktopMusicPlayerDialog`, `AgentChatPanel`, audio handoff in `MaiFfplayVideoDialog`, and corresponding tests.

```cpp
MaiMediaProbeResult maiMediaProbeFile(const char* utf8Path);
// error_code == 0 only on success; duration_us/stream flags/dimensions remain
// zero on failure. Local read-only avformat probing never starts FFplay.
```

- [x] Root runs the failed probe stub against a generated three-second WAV, the owned MKV fixture, Unicode paths and invalid/truncated files.
- [x] Implement ordinary avformat metadata and verify GREEN; replace QMediaPlayer duration-only instances.
- [x] Freeze the audio UI bridge states Stopped/Starting/Playing/Paused/Ended/Error. One process owner/generation coordinates inline cards, music and video; source selection alone does not start audio.
- [x] Test actual FFplay pause/seek/EOF replay with silent generated audio, owner A→B isolation, video handoff, switch-session pause and destruction. Build and run tests before replacing production bindings.
- [x] Replace QMediaPlayer card instances with the bridge, preserving current controls and accessible state.

## Task 4: Native camera, shared UI contract

Files: `MaiChat/Media/MaiCameraCapture.h`, Windows/Mac implementations, native offline tests, and `DesktopCameraDialog.{h,cpp}`.

- [x] Freeze a Qt-free owner/pimpl interface with owning BGRA frames, serialized state/frame/photo callbacks, async `start()` and `capturePhoto(requestId)`, and explicit `stop()`/join. Start acceptance is not readiness; the first valid frame establishes Running.
- [x] Keep native photo output distinct from preview frames. Mac uses AVCapturePhotoOutput JPEG; Windows uses IMFCapturePhotoSink JPEG sample callbacks. Windows explicitly selects video-device-only mode so a null audio argument cannot silently open a microphone.
- [x] All native setup/start/stop and output delivery are serialized. Stop invalidates the generation before draining callbacks; canceled/late results cannot restart capture or publish a photo. A pending photo produces one matching result/error unless stop invalidates it.
- [x] Add offline tests for BGRA stride/bounds, null frames, readiness, terminal-state latching, delayed permission/photo callbacks, stop and owner destruction. Record physical camera access separately; do not open a user's camera just to obtain a green gate.
- [x] Qt displays bounded latest-frame updates and shutter state. The same dialog and native backend serve manual and Agent entry points; user cancel cleans only that operation's new output.

## Task 5: Dependency and packaging closure

Files: `.gitmodules`, `desktop/qt/sources.lock.json`, `desktop/scripts/build-qt.py`, `desktop/CMakeLists.txt`, SDK runtime test, README and both platform package scripts.

- [x] Remove Qt Multimedia/MultimediaWidgets/WebSockets components, links, plugin preferences and required packaged artifacts after replacing all consumers.
- [x] Remove the old Qt video fallback and its backend-specific test. Normal FFplay video behavior and forwarding/save controls remain in the existing implementation.
- [x] Add source/bootstrap/runtime regression coverage for a minimal Widgets SDK. Build explicit Network targets before running tests that were previously EXCLUDE_FROM_ALL.
- [x] Reproduce and resolve the imported Qt6 canonical-path and font-refresh failures with their original cases; retain environment skips and unrelated failures with exact evidence.
- [x] Build the normal product and all affected header consumers. Inspect actual UI captures, run ordinary/native-owned/desktop gates, and verify runtime dependencies.
- [x] Export a private package only after exact-source freeze; verify complete staging and actual ZIP extraction with model/runtime closure and bare-system-PATH smoke. The code commit precedes the ZIP filename; a documentation-only receipt records the verified archive afterward. No installation/publication is implied.

## Task 6: Curl, native system proxy and SQLite boundaries

- [x] Replace the remaining browser HTTP request with the existing curl backend, preserving loopback/NoProxy/size/deadline/cancel semantics. Most other production HTTP already uses curl; their remaining Qt Network dependency is system-proxy discovery.
- [x] Add `MaiChat/Network/MaiSystemProxy.h` with Direct/Http/Socks5/Unsupported/Unavailable results and native Windows/Mac implementations. Preserve process proxy/no_proxy, native manual/PAC settings, bypass and unsupported-proxy failure; never silently erase a configured proxy or log credentials.
- [x] Adapt DesktopServiceRoute, account/model/session clients, attachment downloads, avatar downloads and their public proxy arguments. Perform potentially blocking proxy resolution in worker code.
- [x] Move local HTTP/TLS/WSS test servers from Qt Network to the existing httplib/WebSocket++/OpenSSL or owned ordinary socket fixtures. Keep certificates, wrong-host/CA cases, request bodies, cancel and late-callback assertions intact.
- [x] Replace LocalMessageDatabase's Qt SQL statements with the already vendored SQLite API while retaining its on-disk schema, parameter binding, transaction boundaries, Unicode and pagination behavior. Regression tests use owned temporary/legacy fixture databases only.
- [x] Remove Qt Network/Sql imports and packaged driver/TLS plugins after the ordinary replacements and original regressions pass. Qt Core/Gui/Widgets/Svg and UI scheduling remain framework dependencies.

## Related questions already audited

The existing view_image 20 MiB/1280 px/2 MiB caps are application policy, and the product callback currently always makes a JPEG. Complete Live Photo pairing is absent from cloud upload/download. Those findings are preserved as separate issues; no cloud schema or model-image policy rewrite is included merely to remove the two Qt modules.

## Integration record

See `docs/diagnostics/maichat-qt-ui-boundary-20261109.md` for real RED/GREEN, native
versus fake boundaries, the pause clock and logger lifecycle fixes, SDK limitations,
and exact delivery evidence. Physical camera/keyboard/mouse and live supplier
acceptance remain separate.
