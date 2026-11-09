# Desktop user bitmap decoding through FFmpeg

> **For agentic workers:** Use the agreed file ownership below. Implement and
> verify each step before integrating the next. Work directly on `main` and
> preserve independent changes.

**Goal:** Read and identify desktop user raster images through the ordinary
FFmpeg media implementation, including file and memory inputs, while Qt presents
owning pixels and retains its UI resource/SVG responsibilities.

**Architecture:** Reuse one ordinary image codec component and one thin Qt
Core/Gui presentation adapter. App, Agent, UI and portrait code depend downward
on these components. Retain the existing generic first-frame C ABI; new strict
raster APIs identify image content and enforce each caller's existing limits.

**Tech stack:** Existing native FFmpeg, C-compatible media API, Qt 6 Core/Gui/
Widgets, normal MSVC and Cocoa builds, owned fixtures and local HTTP tests.

## Accepted direction and ownership

The user explicitly requested FFmpeg for WebP/JPEG/PNG after the Qt UI boundary
work. This is a follow-up to `87c0b93e4c228031de51502e251e79cdd376bdb6`; that earlier
delivery does not claim coverage of the newly found Qt decoding paths.

- Mac owns `MaiChat/Media/MaiImageDecode.h/.cpp`, new
  `desktop/src/ui/DesktopRasterImage.h/.cpp`, and the user-raster adapters in
  ImagePreviewDialog, MessageImageLoader, MainWindow, ComposerTextEdit,
  AgentChatPanel, RemoteIMApplication, AddContactDialog, LoginDialog,
  CloudDriveDialog, MaiChatHostTools, DesktopVisionTools and DesktopPortraitService,
  with their behavioral tests.
- Root owns `desktop/CMakeLists.txt`, `desktop/cmake/MaiImageComponents.cmake`,
  Windows integration, independent reproduction, source freeze and delivery.
- The frozen proposal is preserved in the received
  `144115251141768230-1794182319-23109974.json`. The ordinary additions are
  `maiImageInspectFile/Memory` and `maiImageReadFile/Memory`, `MaiImageReadOptions`
  and `MaiImageInfo`; the existing result layout and legacy entry remain stable.
- Inspect reports content identity, dimensions and policy checks; it is not a
  claim of a complete valid decoded frame. Read success supplies that frame.
  Cancellation callback/context are borrowed only for the synchronous call.
- Ordinary limits remain explicit: avatar upload 20 MiB/100 MP; portrait
  128 MiB/32 MP/20000 axes; vision 50 MiB/12 MP; downloaded avatar 4 MiB. Existing
  generic decoder limits must not silently override these caller policies.
- No installation, publication, real camera, user media/database or paid supplier
  operation is required. Earlier private packages remain intact.

## Task 1: Reproduce the actual preview failure

- [x] Compile the actual `ImagePreviewDialog.cpp/.h` in the private Windows
  fixture under `.cache/ffmpeg-user-bitmaps-20261109/preview-source`.
- [x] Through `run-tests-windows.ps1`, run `owned_preview_raster` under qwindows:
  the frozen test reports 4 PASS, 1 FAIL, 0 SKIP. Only the 38-byte WebP has an empty
  source image; JPEG and transparent PNG pass pixel checks.
- [x] Preserve the initial non-saturated PNG premultiplication fixture mismatch
  separately. The frozen fixture uses saturated red and exact alpha, with zero
  RGB tolerance for PNG/WebP. Do not count that initial fixture error as a decoder
  regression.
- [x] Send the unchanged test and original RED logs to Mac. Archive SHA256:
  `2eec513090a28d009b5ff2b2fe2ceb7f6bee000abda413b423b1bb8d4a6c60cb`.
- [x] Run the identical test after integration; inspect an owned widget capture.

## Task 2: Establish the ordinary dependency boundary

- [x] Create `maichat_image_codec` from the existing single copies of
  `MaiImageDecode.cpp` and `MaiImageEncode.cpp`, with AUTOMOC/AUTORCC off, public
  Media headers and private native FFmpeg dependencies.
- [x] Remove those two sources from `remote_im_desktop_ui`; keep VideoCircleMark
  in its existing layer. Do not compile the codec again per consumer.
- [x] Make the existing `MaiImageDecodeTest` available on Windows and Mac and
  link it directly to the codec and Qt Gui, without the whole desktop UI graph.
- [x] Once the frozen adapter source arrives, add `maichat_image_presentation`
  from `DesktopRasterImage.h/.cpp`, public Core/Gui plus codec dependencies, and
  link App/UI/Agent/portrait downward. No target may link backward into desktop UI.

## Task 3: Integrate file/memory raster handling

- [x] Review the frozen ordinary header before integrating the Mac implementation.
  File and memory share the bounded input/format/frame/RGBA implementation;
  no URL/playlist input is opened as a side effect of reading a local bitmap.
- [x] Verify content/demuxer/codec checks preserve supported raster types and
  HEIF/AVIF still-image/grid semantics while rejecting ordinary video and audio.
- [x] Cover file/memory agreement, the tiny WebP, JPEG, PNG/WebP alpha, all EXIF
  orientations, ICC lifetime, invalid/truncated input, original caller budgets,
  cancellation and Unicode paths. Metadata copying is not color conversion.
- [x] Route explicit previews, copy-image, avatars, user-message history,
  attachment validation and composer resource/draft loading through the adapter.
  Preserve original attachment paths and encoded bytes; no Qt decoding fallback.
- [x] Keep already-decoded clipboard pixels, SVG/UI resources, painting and
  purpose-specific encoding distinct from compressed user-image decoding.

## Task 4: Verify the actual products and freeze

- [ ] Normally compile all affected header consumers and ordinary components on
  Windows/Mac. The ordinary codec must have no Qt import requirement.
- [ ] Run the frozen preview reproduction and native/presentation image tests,
  then existing loader, MainWindow, AgentPanel, application/avatar, host tools,
  QR, cloud-drive, vision and portrait regressions as applicable.
- [ ] Inspect an owned image-preview/composer UI capture; never capture another
  user window or reuse an image-generation result as a decoder fixture.
- [ ] Record source/binary hashes and original RED/GREEN evidence in a new
  diagnostic. Keep the prior Qt direct-plugin defect and `87c` evidence historical.
- [ ] Commit/push only the reviewed new scope, and verify any new private package
  from staging and actual ZIP with dependency/model/bare-PATH gates. Do not
  replace installed apps or public assets.
