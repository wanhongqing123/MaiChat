# Desktop user raster decoding through FFmpeg

Status: source committed as `4879c147e9c7d8d3c4ac03b78a0e1cb30d5adcf1`;
Windows and Mac final consumer/private-package gates pass. Baseline is
`87c0b93e4c228031de51502e251e79cdd376bdb6`; the preceding delivery and its private
packages remain historical, with their recorded Qt decoding limitations.

## Request and reproducible acceptance

The user requested FFmpeg for WebP/JPEG/PNG and identified the optional
`qtimageformats` source module as unnecessary for this direction. User raster
files/bytes must share the ordinary Media decoder; Qt owns and presents decoded
pixels. UI resource images, SVG vector rendering, painting and purpose-specific
encoding are distinct uses.

Use only generated fixtures. Open the owned 38-byte WebP through the actual
`ImagePreviewDialog(path)`, compare all pixels against the known color, and repeat
with JPEG and transparent PNG. Exercise composer resource/draft loading, user
message history, avatars, QR, cloud-drive images and tool input validation through
their real adapters. Preserve original attachment bytes and caller-specific
limits. Do not classify a video or playlist as an image merely because FFmpeg
can return a first video frame.

## Windows evidence so far

Platform: Windows 11 x64, MSVC 2022 14.44, pinned Qt 6.12.0. Root evidence directory:
`.cache/ffmpeg-user-bitmaps-20261109`.

- The actual original preview component compiles/links. The frozen qwindows test
  reports **4 PASS / 1 FAIL / 0 SKIP**, with only the tiny WebP returning an empty
  source image. JPEG and transparent PNG pixel checks pass. The original PNG
  mixed-color premultiplication fixture mismatch is retained separately; it is
  not counted as a product failure.
- The new `maichat_image_codec` ordinary target is built from one copy of Media
  decode/encode sources. Its existing PNG/invalid-file test, previously registered
  only on Mac, passes as a normal Windows target.
- The first supplied native raster implementation and unchanged Mac test compile
  and link normally on Windows. The same **23 PASS / 0 FAIL / 0 SKIP** verifies
  file/memory WebP/JPEG/PNG pixels, independent EXIF 1..8 expectations, ICC bytes,
  budgets, cancellation and corrupt/truncated fixtures. These 23 cases do not
  establish compatibility for every advanced format.
- The old SDK against the new no-qtimageformats runtime contract reports
  **4 PASS / 1 FAIL** because WebP remains registered. The same test source under
  the new SDK passes the normal registered CTest in **0.25 seconds**.
- Pure Python SDK profile/cache tests pass **11/11** after their recorded RED.
  They do not stand in for a real SDK build.

## Ordinary and presentation boundaries

`maichat_image_codec` has no Qt dependency. `maichat_image_presentation` contains
the small Core/Gui `DesktopRasterImage` adapter and the common document-image
adapter; self-contained SVG uses an explicit QtSvg renderer. App, UI, Agent, portrait and the
Windows PDF page adapter link downward; implementations are not compiled again
per consumer. The existing generic first-frame ABI remains available for explicit
video-cover use; strict user-raster callers use the new Inspect/Read contracts.

Inspect checks content identity, dimensions and policy. Read success supplies a
complete first frame. A successful Inspect is not proof that compressed pixels
are valid. Read's optional metadata result comes from the same input snapshot.
Cancellation is polled on the calling thread and borrowed only during the call.

The original caller policies remain explicit, including avatar 20 MiB/100 MP,
portrait 128 MiB/32 MP/20000 axes and vision 50 MiB/12 MP. The ordinary result
retains ICC bytes; this is not a claim of ICC conversion or HDR tone mapping.

## SDK retirement and actual cache failure

Only qtbase and qtsvg remain in the module lock. The retired qtimageformats
checkout is preserved and ignored; only its gitlink is removed. The new SDK uses
`install-widgets-native-images` and `installed-widgets-native-images.json`, while
reusing the module build directories. A complete pre/post hash comparison confirms
the old installed SDK's **4325 files / 99665737 bytes** and original stamp remain
unchanged. The new SDK's image plugin set is qgif/qico/qjpeg/qsvg; optional decoder
plugins are absent. QtBase may still build modules that the product does not link.

A real product-cache configure first selected the new `Qt6_DIR` while
Core/Gui/Widgets/Svg still pointed to the old SDK. Merely changing Qt6_DIR did not
remove cached package locations. A second cache audit also found CoreTools,
GuiTools and WidgetsTools resolving through the old CMAKE_PREFIX_PATH. The fix
forgets stale Qt package directories and prepends the selected prefix before
importing it; the same Windows cache then selects the new prefix for all eleven
Qt6 package/tool directories. The external qtsvg build similarly clears
old Qt package locations before selecting its new prefix.

Generated old build/bin Qt runtimes/plugins were archived before deploying the
new profile for tests. This does not change the old source SDK, installed App or
earlier private package. Packaging gates reject retired optional image decoder
plugins rather than importing them from a stale SDK or stage.
Raw PPM/RGB fixture bytes are explicitly marked binary so cross-platform Git
checkouts cannot normalize pixel bytes as text line endings.

## Consumer regressions and independently reproduced failures

- The expanded native test initially reports Windows **39 PASS / 1 FAIL**. A
  10000-by-10000 gray PNG contains only value 127, including every pixel in the
  native decoded full-range gray frame. Reducing it to 8-by-8 with optimized
  LANCZOS produces RGB values 125..130. An independent probe holds input, decoder,
  range and filter fixed: adding SWS_ACCURATE_RND restores all 64 pixels to 127;
  BITEXACT and a private scalar control do so too. Changing full to limited range
  worsens the result. This is a scaling-rounding issue, not evidence for changing
  the frame's color range. The production fix adds ACCURATE_RND to the existing
  LANCZOS paths; the unchanged 40 cases then pass normally on Windows.
- The presentation suite initially reports **8 PASS / 1 FAIL**. Windows drive
  paths such as `C:/.../image.webp` have a one-letter QUrl scheme and did not enter
  the composer's local-file branch. The ordinary UTF-8 file decoder succeeds;
  the resource loader needs a drive-path correction. This is a real path issue,
  not a test directory alias. Explicit drive recognition, host/UNC rejection and
  preserved draft identity make the unchanged nine cases pass on Windows.
- Root's unchanged HTML/PDF suite records **6 PASS / 25 FAIL**, then **31 PASS /
  0 FAIL** after routing file/data rasters to owning pixels and preventing
  QTextDocument/provider fallback with a typed null image. The temporary document
  used for preview sizing has the same policy. Two identical vector/MIME helpers
  were then extracted to one component; the unchanged 31 cases pass again.
- The Windows PDF adapter initially imposed new logical source-axis limits that
  incorrectly rejected WinRT's DPI-scaled output. At 140% scaling, a requested
  1587-by-2246 page produces a 2222-by-3144 PNG, with 40781 encoded bytes. Removing
  that mistaken added constraint preserves the original 50 MiB encoded limit and
  the ordinary pixel budget. The unchanged original PDF suite goes **4 PASS /
  1 FAIL** to **5 PASS / 0 FAIL / 0 SKIP**.
- The first 15 normal Windows consumer suites record **13 PASS / 2 FAIL**. One is
  the composer drive-path failure above. The cloud-drive fixture fails before
  starting product code because its existing OpenSSL CLI is absent from the test
  PATH; adding the installed Git OpenSSL tool for tests alone makes the unchanged
  suite pass in 22.46 seconds under qwindows. The other thirteen pass, including
  Preview **5/0**, MainWindow **159/0/1** (live avatar omitted), AgentPanel **73/0/0**,
  application/avatar, host/vision/portrait, QR/login, loader and runtime tests.
  The cloud JPEG preview is captured from its own widget and visually checked.
- Independent review then reproduces a separate AVIF allocation-budget failure.
  The existing 407-byte 96-by-64 fixture correctly fails a 4000-pixel policy.
  Changing only its ispe display size to 16-by-16 makes file/memory Read succeed,
  including with 16-pixel source-axis caps; the same native libdav1d still decodes
  96-by-64 (6144 pixels). It owns its allocator and does not run get_buffer2, while
  the original max_pixels setting was twice the policy and post-decode cropping
  hid the native dimensions. The minimal probe and both owned fixtures are
  retained. The fix checks actual AV1/HEVC packet dimensions with the public
  parser before opening a pixel decoder, rejects multiple/hidden/spatial AV1
  framing that cannot supply one primary frame, and gives self-allocating codecs
  the exact pixel limit. The native frame is checked again before crop. Budget
  lifetime also extends past decoder destruction. The same eight file/memory
  Inspect/Read pixel/axis regressions fail before the fix and pass afterward;
  the full normal Windows suite reports **48 PASS / 0 FAIL / 0 SKIP**. Original
  cases, normal HEIF padding and gray value 127 remain unchanged. Two independent
  read-only reviews find the reproduced allocation path closed.
- A private runner compiles one new main against the actual normal libraries and
  the new SDK, then presents only its own ImagePreviewDialog and ComposerTextEdit.
  Its WebP preview, insertion and fresh-editor draft captures are visually
  checked. This evidence uses explicit file URLs; the unchanged native-path test
  separately verifies the Windows drive fix. No full-screen capture is used.

## Final verification and boundaries

- Final Windows normal build succeeds for the App and seventeen test targets.
  All **17/17** affected registered suites pass in **161.00 seconds** under
  qwindows, including the existing local-TLS fixture with its required CLI tool.
  Native raster is **48/0/0**, presentation **9/0/0**, preview **5/0/0** and
  document-resource regression **31/0/0**. MainWindow retains its single live-avatar
  skip; Login retains five existing optional/live skips. These internal skips
  are separate from the 17 passing registered suites.
- Mac reports **19 normal targets built / 18 registered suites passed** in
  **59.74 seconds**, using the new SDK with all ten platform-specific Qt package
  and tool locations updated. MainWindow is **156/0/4**, Login **31/0/5**, native
  **48/0/0**, presentation **9/0/0**, preview **5/0/0** and document **31/0/0**.
  The post-checkout ordinary/debug sandbox smoke receipts and raw DYLD logs agree
  on the exact source/signed EXE hashes, PIDs **48687/48879**, exit 0 and all five
  Qt frameworks plus Cocoa loaded from the new stage. Its package gate checks
  **26 Mach-O files / four models**. This is an app-only private stage, not an
  installed or published DMG; runtime execution is only on macOS 27.2 arm64.
- Windows reconfigures/builds after the source commit. OpenCV's generated Git
  version string recompiles `src/system.cpp` and relinks the App; no functional
  program source changes. Staging and actual ZIP extraction have **81 identical
  files**, **37 PE binaries each** with no missing/retired imports, and preserve
  four portrait models plus RVM. Ordinary/debug bare-PATH smoke children exit 0
  for both directories; their five loaded Qt/QWindows modules come from the
  corresponding package. The exact EXE/ZIP hashes are in the delivery JSON.
- Root checks Mac's raw 18-suite CTest log, both smoke receipts against the
  original DYLD logs, and its final post-checkout package receipt. Source/stage
  hashes agree throughout; all five model hashes (four portrait plus RVM) match
  the Windows package. The gate's separately listed TRTC SDK driver resource is
  outside its 26 target Mach-O checks, as recorded in the receipt.
- The expanded implementation adds targeted animated WebP/APNG, ICO same-item
  selection, TIFF orientation/strict strip errors and HEIF item-padding cases.
  Both platforms record 48 passing native cases after the separate allocation
  fix. The initial 23 cases alone do not establish these advanced-format results.
- Final consumer regression, source freeze and package checks are recorded in the
  [delivery receipt](maichat-ffmpeg-user-bitmaps-delivery-20261109.json).
  Widget/composer captures are owned fixtures,
  not a live model or account-session test.
- No real camera, user media/database, paid supplier, installed-process replacement,
  installation or public release is performed. Mobile execution and macOS 14.4
  runtime acceptance are not supplied by these desktop fixtures.
