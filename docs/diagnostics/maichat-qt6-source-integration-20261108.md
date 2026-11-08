# Qt 6 source integration

2026-11-08, macOS arm64. Baseline: main
`b95e1ff006628bbf1301d7ec42749f6417442f36`. Preserve the earlier uncommitted
disk-cleanup diagnostic; it is independent of this migration.

Request: replace the desktop Qt 5.15 SDK dependency with Qt source under
`MaiChat/desktop/qt`. Reproduction: configure a fresh desktop build without a
Homebrew/installed Qt SDK. Previously `find_package(Qt5)` failed or selected the
machine's SDK; expected behavior is to build the pinned Qt 6 source modules and
link/deploy that same runtime.

The Qt 6.12.0 tag is pinned by commit for qtbase and the required companion
modules. Each source module is a shallow Git submodule. Qt build/install outputs
use a fixed ignored directory, with examples/tests/PCH disabled to control disk
usage. No Qt source is downloaded during normal CMake configure. Upstream
declares CMake 3.25+, macOS 14.4+, and this product profile uses MSVC 2022 on Windows.

The user subsequently narrowed Qt to the UI. Production capture now uses native
AVFoundation/Media Foundation, audio and probing use the existing FFplay/FFmpeg,
CDP uses vendored WebSocket++/Asio, HTTP uses curl with native system proxy
discovery, and the message database uses the existing vendored SQLite API.
Qt Network, WebSockets, Sql, Multimedia and MultimediaWidgets are absent from
the tested App dependencies. Old source checkouts and SDK files remain on disk.

## Mac source and normal build, 2026-11-09

The shared common-2 temporary source tree is
`c5e361ce885637596d3e00c96ea745c95e7bb627`; this is not a commit. The two Mac
follow-ups are packaging script `3867985f3b398bcb83fda29635c7717d4fc22c35`
and camera Session test `a3021f9cc64061d3b52c1e012c2c6a67cf0250da`.
The later AppLog fix is `3ddbfdf187ecf91aad0460a43b54544eb9812f47`, with
lifecycle test `014e3371aa73a2b4bb8d084eb549a7c255ada5a8`.
At the time of these runs, the peer HEAD remained the baseline above; no
independent commit or push was made. Final source freeze and CMake integration
are recorded by the coordinator in the UI boundary report and delivery receipt.

The normal Release build is
`/private/tmp/maichat-desktop-portrait-20261105/full-config`, using the pinned
source SDK, remote desktop enabled and the real product libraries. Normal App
and default targets rebuilt successfully, followed by the six explicit Network
test targets. All 20 objects depending on the changed native
`LocalMessageDatabase.h` were newer than that header; old QtSql layouts were not
mixed into consumers.

The first selected offscreen CTest run was 89 pass / 1 fail out of 90. The failure
compared `/var/...` with the same directory's `/private/var/...` spelling in
`DesktopCameraSessionTest`. Both parent paths now use `canonicalPath`; the same
normal target passed 15 / 0 / 0 after rebuilding. Absolute path, exact native
JPEG bytes, claim and stop/cleanup assertions remain. The separately registered
isolated real-browser test also passed without changing its assertions. Two
additional, restrictive outer-sandbox browser runs returned `ERR_ABORTED`; the
original registered environment passed with the same source. These logs remain
separate from the normal result.

Important normal-target results: native Mac camera 122 checks, Mac native proxy
92 checks, storage 30, audio 7, camera Session 15, camera Dialog 6, Controller 22,
Panel 73, UiZoom 8 and attachment TLS 11, all passing. Original live opt-ins and
platform skips remain. Real camera, OS input and user-database acceptance were
not run.

## Plugin deployment and actual App shutdown

Fresh Mac `macdeployqt` did not copy TUIO by default. A private input retaining
`generic/libqtuiotouchplugin.dylib` did pull in QtNetwork and failed the retired
module gate. Packaging now reconstructs only staged UI plugin seeds and passes
each to `macdeployqt -no-plugins -executable=...`, which rewrites their load
commands. Both Qt configuration filenames restrict plugin lookup to the bundle.
The source SDK is preserved. An SDK-read-denied Cocoa fixture passed 30 image,
pixmap, style and icon checks; its 19 Mach-O images passed closure checks.

The full private App has 33 Mach-O images, 14 UI plugins, no retired Qt imports,
no missing bundled dependencies and valid deep strict ad-hoc signatures. The
non-arm64 TRTC auxiliary driver remains an explicitly reported vendor resource,
not an App load dependency. The signed private App starts and exits with bare
system PATH, Cocoa, network denied, production AppData denied, and source
SDK/build/Homebrew reads denied.

An additional `QT_DEBUG_PLUGINS=1` smoke exposed a real shutdown SIGBUS.
`QFactoryLoader` had been constructed before AppLog; its static destructor still
called the installed handler after the function-static log file and other state
had been destroyed. The crash frames identify `AppLog::handler + 504` and
`openLocked + 308`. AppLog now constructs a private restoration sentinel last,
after its initial messages establish the logging state. Its destructor restores
the previous Qt handler before that state is released. Locking, flush and
rotation code is unchanged; no log file is leaked.

The new unchanged child-process test was 2 pass / 2 real native-exit failures
against the original source, then 4 / 0 / 0 after the fix. It covers writable and
failed log-open paths, restored late-warning delivery, immediate flush,
idempotent install and lock release, using only random Qt test-mode namespaces.
The normal App was rebuilt and repackaged; the original Cocoa debug-plugin
smoke then exited 0 in 3.032 seconds and logged successful plugin unloading.

Final source App SHA256:
`ae3c138c350907e833463586f9039d6a0d1f1302dd919ab4512532b763d46b62`.
Signed private App executable SHA256:
`7abcc27efee122636acf723f4ae9b6f4a7480d856c7ac4329ef7c9241fa5db76`.
App path:
`/private/tmp/maichat-qt6-common-mac-20261109/package-output/MaiChat-macos-arm64/MaiChat.app`.
Raw runs, selection, manifests and original failures are under
`/private/tmp/maichat-qt6-common-mac-20261109` and
`/private/tmp/maichat-applog-lifecycle-20261109`.

## Verified boundaries

The actual host is macOS 27.2.0 arm64. Nominal 14.4 deployment flags do not prove
14.4 compatibility: retained FFmpeg/Homebrew dependencies require newer systems.
No 14.4 acceptance, real hardware capture or authenticated supplier workflow is
claimed. The installed App process, PID 96951, was left running unchanged; no
installation, release or publication was performed.

A valid 38-byte lossless WebP remains a known pinned-Qt direct-decoder failure:
the handler requires a 40-byte initial peek. Original source-SDK and deployed
QImage/QPixmap failures and the fixture are retained. The normal textured WebP
fixture passes. `MessageImageLoader` is compiled with its existing FFmpeg decode
path; the direct Qt limitation must not be generalized to that path without a
separate test. Neither Qt SDK nor image business code was changed for this issue.

Coordinator report: [Desktop Qt UI boundary](maichat-qt-ui-boundary-20261109.md).
