# Desktop Qt UI boundary: native media, curl and SQLite

## Request and reproduction

Work on `main` from `b95e1ff006628bbf1301d7ec42749f6417442f36`, preserving the
previous Mac Qt 6 migration overlay before applying it. The requested boundary is
Qt for widgets and UI delivery; HTTP uses existing curl, CDP/WebSockets use
WebSocket++, media uses existing FFmpeg/FFplay and native camera APIs, and storage
uses the existing SQLite amalgamation. No service deployment, installation or
public release is part of this task.

Reproduce with a normal source-SDK desktop build. Inspect both linker imports and
the deployed files, then exercise a local browser page, duration-only media
probing, silent audio controls and ownership, temporary-database operations and
owned camera fixtures. A successful tool result or compilation alone is not a
physical-camera acceptance result.

## Implementation

- Pin and build only `qtbase`, `qtsvg`, and `qtimageformats` at the exact commits in
  `MaiChat/desktop/qt/sources.lock.json`. Preserve old peer SDK/checkouts. Qt source
  is not patched; normal configure does not download sources.
- Remove Qt Multimedia/MultimediaWidgets, WebSockets, Network and Sql component
  discovery, links and deployment requirements. Retire the alternative Qt video
  player. Normal FFplay video controls remain the product path.
- Add ordinary `MaiCdpTransport`, `MaiMediaProbe`, `MaiCameraCapture` and
  `MaiSystemProxy` APIs. Manual UI and model tools share the existing desktop
  adapters. Camera output is native JPEG, distinct from owning BGRA preview frames.
- Keep one process-wide audio bridge owner, with old-owner and late-start guards.
  Native camera stop invalidates generations and drains callbacks; GUI delivery
  coalesces preview frames. A photograph is exclusively saved in a worker and
  survives stop only after the UI explicitly claims it.
- Preserve HTTP/TLS/redirect/cancel behavior and native system proxy discovery.
  Explicit proxy/PAC errors remain typed failures. An absent WPAD configuration
  is distinguished from a broken explicit PAC. SessionSync proxy credentials are
  decoded separately from the HTTP CONNECT URI. Potentially blocking lookup runs
  in workers, not on the UI thread.
- Replace the Qt SQL implementation with native prepared statements and checked
  transactions. Preserve the on-disk schema, NULL/empty/NUL-containing strings,
  int64 values, pagination, deduplication and status behavior. Tests use temporary
  databases; no installed/user database was read or migrated.

## Failures found and their evidence

1. **Localized MSVC dependency discovery:** a double-decoded `/showIncludes`
   prefix gave Ninja zero header dependencies. The source builder now probes the
   compiler's actual prefix and imports it after compiler metadata. The rebuilt
   FFplay entry object records 68 dependencies; Qt Core records its headers too.
2. **Native audio pause clock:** the original Windows five-slot component test
   failed because a paused position fell from approximately 156825 us to zero.
   The current-source native rebuild reproduced it. Freeze/rebase the audio clock
   at pause/resume and prevent a paused silence callback from overwriting it,
   under a short private mutex. The unchanged 60 ms assertion and all original
   controls pass on Windows and Mac; Qt does not fabricate a frozen clock.
3. **Windows camera lifetime/identity:** real synthetic failures showed samples
   retained across two close paths and an old photo callback occupying a new
   request slot. Drain samples on the owning MTA before native cleanup, and
   recheck generation/request/serial at mailbox insertion. Metadata-readable but
   pixel-invalid JPEG bytes exposed a separate failure; bounded WIC band decoding
   now validates the full image while returning the original JPEG bytes.
4. **Mac camera delegate ownership:** the original native submission branch with
   a weak-holding native substitute produced two lifetime failures. Retain each
   delegate by internal token until its final callback; its reverse owner remains
   weak. Old final callbacks only release their own token. The final 122-check
   suite and ASan/UBSan pass without hardware or permission requests.
5. **Qt 6 test assumptions:** canonicalize existing file/directory identity across
   `/var` and `/private/var`. A displayed font fixture waits for normal font events
   and independently resolves the complete requested fallback list. Removing the
   actual production font refresh still fails. Production UiZoom is unchanged.
6. **Windows deployment closure:** the executable directly imports only Qt Core,
   Gui, Widgets and Svg, but default `windeployqt` copied the optional generic TUIO
   plugin, which imports Qt Network. Skip generic/networkinformation/TLS plugins;
   retain a gate against retired modules. Mac fresh deployment does not share this
   default behavior; its separate stale-plugin fixture reproduces the same closure
   problem. Only generated staging plugins are rebuilt from explicit UI seeds.
7. **Shutdown diagnostics:** enabling `QT_DEBUG_PLUGINS=1` exposed an AppLog static
   lifetime crash after a normal smoke exit. Mac's stack reaches `openLocked` from
   the message handler during QFactoryLoader destruction. Windows independently
   reproduces access violation `0xC0000005` in the same controlled smoke setup.
   A last-constructed restoration sentinel now restores the prior Qt handler before
   the log state is destroyed. The unchanged two child cases reproduce two native
   failures on the original source and pass 4/0/0 on both platforms after the fix.
   Windows and Mac normal/debug-plugin package smoke both exit 0 afterward.

## Windows validation

Platform: Windows 11 x64, MSVC 2022 14.44, pinned Qt 6.12.0 built from source.
Normal product build: `MaiChat/desktop/.cache/build-qt6-ui-20261109`.
Root evidence: `.cache/qt-ui-boundary-20261108`.

The 25 distinct normal-build suites exercised are:

| Area | Suites / observed result |
| --- | --- |
| Ordinary transports | CDP, native/shared proxy, SessionSync proxy authentication, isolated local-browser page, ordinary media probe: PASS |
| Camera | Native synthetic suite, GUI bridge 15/0/0, owned fake Dialog 6/0/0: PASS |
| Audio | Five behavior slots plus setup/cleanup 7/0/0; music window silent-file play/pause/close: PASS |
| Product integration | AgentController 21/0/0, AgentPanel 73/0/0, SessionSync TLS 5/0/0, attachment TLS 11/0/0: PASS |
| Storage | Native temporary/legacy database suite 30/0/0: PASS |
| Logging | Child-process lifecycle suite 4/0/0: PASS |
| UI | UiZoom 8/0/0, four Markdown suites, SDK UI plugin runtime: PASS |
| MainWindow | Original full group under actual qwindows: 158 PASS, 0 FAIL, 1 live-avatar SKIP |
| Login | Original local HTTP cases: 31 PASS, 0 FAIL, 5 live-account/production opt-in SKIPs |

The final complete qwindows run passed all 25 suites in 167.53 seconds, using
the final normal build after preserving vendor-specific MSVC exception options.
Its original log is `product-final-25-tests.txt` in the evidence directory. The
normal build completed without the earlier global `/EHsc` override of OpenCV's
own `/EHa` setting. Core source is unchanged.

A final source review removed the unused Windows `QT_MEDIA_BACKEND=windows`
startup setting and its obsolete Qt Multimedia comment. Only `main.cpp` changes
after the 25-suite run; its normal App target is rebuilt and the resulting package
is tested again. The unchanged test binaries are not presented as a new test run.

The first 17-suite offscreen run retained two failing Windows native-caption/style
assertions. Their HWND style and native hit-test requirements were then tested by
the unchanged whole MainWindow group under qwindows, which passed. No threshold or
skip was changed to hide the offscreen failures. Browser and media probe were also
run through their actual registered CTest names. The final camera test-only alias
change was rebuilt and re-executed on Windows.

Four actual qwindows widget captures were inspected for opening, ready, saving and
error camera states. They prominently identify the private fake backend. An actual
music-widget capture uses a generated silent WAV. These are component UI evidence,
not real camera input or installed-application acceptance.

The final Windows staging has 42 PE files with no missing/retired Qt imports,
four verified Portrait models and ONNX Runtime, and normal/debug-plugin system-PATH smoke exit
0. An owned-process module probe confirms Core/Gui/Widgets/Svg and qwindows load
from staging. The pre-fix candidate crash and its different binary hash are retained.
After removing the unused startup setting, the final build/staging executable is
SHA256 `11296b712215a45a940f9263790a4cf03fbcf42ce39424b7bb7cebeb81827921`.
Final executable/ZIP hashes, full source identity, actual ZIP checks and the
post-fix smoke results belong to the delivery receipt.

## Mac validation and boundaries

Peer normal App and default targets compile with RemoteDesktop ON, FakeClient OFF,
OpenCV/Portrait/TRTC retained; six excluded-from-all Network targets are explicitly
built. The initial 90-suite non-capture run has 89 PASS and the camera test's parent
directory alias failure; the same corrected target then passes 15/0/0. With the separately registered browser
case and the final formally registered logger target, the peer records 92 unique
registered suites passing. The identical logger fixture passes 4/0/0. Explicit
reconfiguration changes none of the 1816 existing Mac compile commands and adds
only the three commands for that target; the source App hash stays unchanged.
The final alignment archive is 31295 bytes, SHA256
`2018727be02fdb9d6ed9483386603c1a14d45ba68742b766f51d5daef66104f2`.
Native
camera 122, native proxy 92, audio 7, storage 30, AgentPanel 73, AgentController 22,
UiZoom 8, and attachment TLS 11 checks pass. MainWindow retains three Windows-only
skips and the live-avatar skip; Login retains five live opt-in skips. Excluded
desktop capture/input/provider SDK/Keychain groups are not counted as passed or
skipped. The owned-browser sandbox restriction failure and its original-environment
PASS are both retained.

The independent UI deployment fixture verifies bundle-only Cocoa/JPEG/WebP/SVG/
icon/style loading, pixel readback, rewritten dependencies/rpaths, strict signing
and no build-SDK/Homebrew/retired-Qt dependency. A private normal App staging passes
33 Mach-O dependency checks and ordinary sandboxed bare-PATH smoke; the same previously failing debug-plugin exit
also passes after the logger fix described above. Installed PID 96951 is not replaced.

Actual Mac execution is macOS 27.2 arm64. Older cached media/vendor objects and some
Homebrew dependencies have a higher minOS than the declared 14.4 target. This work
does not establish macOS 14.4 runtime compatibility and is not a public release.

## Image and acceptance boundaries

- The pinned Qt WebP plugin rejects a valid owned 38-byte 16x12 WebP on both
  Windows and Mac. Retain that decoder failure; do not call WebP unconditionally
  compatible. Normal textured WebP, SVG and JPEG deployment fixtures pass.
- A Windows private fixture compiles the production MessageImageLoader with its
  normal FFmpeg definition and confirms that same 38-byte image is delivered with
  all 192 pixels correct. This does not claim that direct `QPixmap(path)` preview
  constructors pass, nor substitute for a Mac product-path test.
- Physical camera preview/shutter/permission/unplug and native keyboard/mouse
  acceptance remain unverified. No camera/microphone was opened to make tests green.
- Live supplier vision, real logged-in CR editing, the existing view_image size
  policy and paired Live Photo cloud preservation are separate acceptance/work
  items. They are not silently implemented by a Qt dependency change.
- Installed applications, public v2.2.52 assets and online SessionSync deployment
  are not changed. No installer is executed.
