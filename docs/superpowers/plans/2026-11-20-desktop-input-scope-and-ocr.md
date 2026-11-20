# Desktop input scope and OCR implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Work on the existing main branch, preserving the unrelated qtbase worktree changes. The user approved the generic design and explicitly requested implementation on 2026-11-20.

**Goal:** Make screenshot coordinates usable with a separately bound receiving window, and provide reusable local image OCR through ordinary and model-tool APIs.

**Architecture:** An optional `execution_window_id` is selected during display observation and belongs to the snapshot, not to individual actions. Display coordinates retain their original mapping; controller evidence and native input validate the receiving window for every new input while cleanup remains possible after focus loss. OCR uses one ordinary, bounded RGBA engine with the already bundled ONNX Runtime and OpenCV geometry; a thin desktop adapter handles existing image decoding, permissions, resource models and JSON.

**Tech Stack:** Existing C++17, Qt6, ONNX Runtime 1.26.0 C API, OpenCV core/imgproc, PP-OCRv5 mobile ONNX detection/recognition assets. No new libraries, MCP, remote model requests, or application-specific input logic.

## 1. Reproduce and freeze input scope

Files: `MaiChat/desktop/tests/DesktopComputerContractsTest.cpp`, `DesktopComputerWindowsNativeSessionTest.cpp`; production under `MaiChat/desktop/src/MaiChatTools/DesktopComputer*`.

- [x] Add owned display/window fixture using display `(0,0,1920,1080)`, receiver `(320,115,1454,800)` and ratios `.568/.8712`. Old scope-less display accepts the point; a bound receiver must reject before native dispatch with requested point and bounds facts.
- [x] Add receiver snapshot, changed geometry/identity, keyboard focus, primitive/batch, drag endpoint, automatic observation and held-target-change cases. Keep intentional unrestricted display and release-only behavior as positive controls.
- [x] Extend `observeDisplay` with a trailing optional execution-window ID, state with the exact observed native window, and action/native scope with the receiver identity. Do not reuse `element_window_id`, whose purpose remains accessibility source selection.
- [x] Validate receiver before dry-run/dispatch and again natively during multi-event input. Do not auto-focus a different receiver. Bind the same receiver to automatic subsequent observations; movement/identity changes require a new observation.
- [x] Preserve key/button UP, watchdog and cancellation cleanup independently of the receiver validator. Reject a receiver change while any controls are held.
- [x] Expose optional window image capture via the existing capture path and return explicit frame metadata. Never substitute native bounds for actual image dimensions.
- [x] Run `run-tests-windows.ps1` for `desktop_computer_contracts` and the Windows native-session test using owned hooks; run state-only native integration with no user application input.

## 2. Ordinary local OCR

Files: new `MaiChat/Media/MaiOcr.h/.cpp` and focused OCR helpers/tests; desktop CMake/model resources updated by the OCR owner only.

- [x] Freeze an ordinary opaque-engine API taking bundled runtime path/model bytes and a model-specific UTF-8 dictionary, then bounded RGBA input and cancellation. Return original raster width/height, ordered text quadrilaterals, confidence and explicit owned-memory release. No Qt, tool JSON, user-window assumptions or network in the engine.
- [x] Pin official PaddlePaddle PP-OCRv5 mobile detection/recognition assets and dictionary configuration with revision, SHA256 and Apache-2.0 notice. Store downloaded assets in the existing tracked model-resource convention; no Python inference dependency enters the product.
- [x] Write meaningful detector/CTC/geometry failures before implementation, then implement model-specific normalization, resize/pad, DB decoding, bounded quadrilateral expansion/cropping, dictionary CTC decode and mapping to the original raster. Bound pixels, candidates, recognition width and total output; cancellation must not publish partial success.
- [x] Reuse existing ORT C API and OpenCV core/imgproc only. Validate tensor names/types/shapes, dictionary/output cardinality, non-finite values, missing runtime/models and malformed inputs.
- [x] Run actual OCR on generated Chinese/English UI-like fixtures, blank images, small text, rotated text and scaled images. Measure coordinates against independently known rendered positions; preserve limitations rather than promising perfect text/button identification.

## 3. Thin OCR tool adapter and packaging

Files: `MaiChat/desktop/src/MaiChatTools/DesktopVisionTools.cpp` or a focused OCR adapter registered through it; desktop resources/build/tests.

- [x] Add `ocr_image` with a read-only accessible local image path. Use the existing bounded EXIF-aware raster decoder and the exact same ordinary OCR core. Reject inaccessible paths, unsupported options and canceled work.
- [x] Return `image_width`, `image_height`, `space=image`, source identity, top-left pixel units, text, confidence and quadrilateral/bounds. OCR output alone does not grant an input snapshot or make identical text unique.
- [x] Test ordinary and tool entry equivalence with the same fixture, path permission, corrupt/oversized image and cancellation cases; verify registry presence through the normal product path.
- [x] Package model files/notices and existing ORT runtime in the normal desktop staging. Rebuild `maichat` and let post-deployment PE/plugin checks complete before launching it.


The Windows foreground-owned receiver mode was executed but returned 77 because owned foreground could not be acquired. Recorded native regression and inactive native state/image tests passed; this is not claimed as positive live receiver input validation.

## 4. Review and delivery

- [ ] Review scope compliance, then code quality across both independent lanes, with no unresolved input bypass, stuck-key, path escape or OCR shape/memory issue.
- [ ] Run only relevant regressions once after the final changes, then normal Windows build, staging smoke and actual ZIP extraction smoke under bare PATH. Keep the installed running process intact until the existing restart question receives approval.
- [ ] Ask the Apple owner to consume exact shared headers and native contract, compile/validate Mac on its own platform and report honest permission/runtime boundaries. Do not claim iOS/Android OCR integration or Mac GUI validation from Windows tests.
- [ ] Update `docs/diagnostics/2026-11-20-desktop-coordinate-ocr-triage.md` with RED/GREEN, exact source, artifacts and remaining target-platform limits. Coordinate the sole main commit/push and preserve all unrelated WIP.
