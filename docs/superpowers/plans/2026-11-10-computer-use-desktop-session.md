# Computer Use Desktop Session Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development. Work directly on main. Root owns shared APIs, controller/tool integration, CMake and builds; workers edit disjoint Windows, macOS and contract-test files.

**Goal:** Let the model observe and operate the whole interactive desktop, intentionally switch applications and receive the resulting screen in the existing Agent loop. The overall goal is real model-driven computer tasks; this first increment is not the complete acceptance claim.

**Architecture:** Extend the existing controller/backend and desktop_computer adapter with a display target. Reuse the resident WGC/ScreenCaptureKit worker and native input. Screen observations bind display identity, native input bounds, PNG dimensions, frame time/sequence and a Session snapshot. Display actions may change foreground applications; window-scoped actions retain their identity checks.

**Tech Stack:** C++17, Qt6 host, existing MaiAgent screenshot worker, Windows SendInput/UIA and macOS CGEvent/AX.

## First increment

- [x] Declare DesktopNativeDisplay, explicit desktop input scope, Move, standalone modifiers, controller display observation and screenshot DisplayById. Root initially supplies NotSupported controller stubs so new behavior tests can link and fail.
- [x] Add ordinary API and tool behavior tests in DesktopComputerContractsTest.cpp. An injected backend supplies two displays, including negative origin, a real owned PNG and deterministic frame metadata. Require display listing/observation, correct image-to-input mapping, cross-window actions without activation, foreign/consumed snapshots and changed geometry rejected, and post-action image feedback. Retain a dispatched receipt if that later capture fails. Record actual RED before replacing controller stubs.
- [x] Extend DesktopComputerWindows.cpp and MaiScreenshotWindows.cpp. Enumerate HMONITOR identities; select the exact display in the existing WGC worker. Validate interactive desktop/cancellation/display geometry per gesture while allowing intentional focus changes. Preserve legacy window checks, user-held-input checks and paired synthetic releases.
- [x] Extend DesktopComputerMac.mm and MaiScreenshotMac.mm with the same contract using CG display coordinates and resident ScreenCaptureKit. Preserve permission and console-session checks; no new capture worker. Latest Mac source remains subject to target-platform compilation.
- [x] Implement displays/observeDisplay/controller dispatch and model tool routing in DesktopComputerTools.cpp. Use list_displays, observe(target=display,display_id) and display-scoped actions. Return the same PNG through ordinary APIs and model image results. Window observe remains metadata-only in the model tool.
- [x] Run original window contracts and new display contracts, normal App deployment and relevant Agent/image regression. Final Core nine-suite gate passed. Owned Windows display capture/input/readback passed with one resident worker/session; Mac compilation and RecordingBackend contracts passed. Actual visual-model tasks and Mac native input remain separate acceptance work.

## Fixed observation contract

Display observation JSON contains target=display, decimal-string display_id, bounds{x,y,width,height} in native pointer coordinates, image_width/image_height in PNG pixels, frame_sequence, frame_received_at_ms, snapshot_id, workspace-relative image_path, mime_type=image/png and has_image=true. Input ratios map to the observed native bounds, independent of PNG downscaling or Retina size. A post-action observation supplies a new snapshot; a failed post-action capture leaves the old snapshot consumed and returns dispatched_observation_failed plus observation_error. Whole-desktop input generations reject images captured during an in-flight batch and invalidate other controllers' old display tokens. Source-frame timestamps prevent a pre-input frame delivered late from satisfying freshness.

## Remaining work toward the overall goal

1. Complete cross-action mouse/key state, long gestures, clipboard semantics and control ownership using existing native/remote primitives; stop/cancel releases owned held input.
2. Add condition waits, progress and application-transition recovery, takeover and completion evidence on the existing Agent loop.
3. Exercise ordinary GUI launching and existing browser sessions. Native launchers and CDP session/navigation improve reliability; generic screen/keyboard operation must remain independently usable.
4. Use target-machine Agent execution through existing Session sync where appropriate; a direct remote frame/input adapter can reuse current remote transport.
5. Validate actual visual-model tasks in owned environments: desktop to editor to Save As/reopen, existing test browser navigation/dialogs, cross-application transfers and display/focus changes. Scripted executor tests and actual-model results are reported separately.

2026-11-10 progress: the ordinary native executor completed a real Windows Save As, independent disk read, editor clear and native Open/reopen with exact UIA text (one resident worker/session, 95 frames). The original native click/type/Enter case also passed on the same binary. This is task-level native evidence; actual visual-model execution and browser tasks remain pending. See `docs/diagnostics/maichat-computer-use-document-workflow-20261110.md` for original failures, guard corrections and precise binary evidence.

No paid model request or real-account operation is test setup. Native input is allowed only after verifying the fixture owns the target and interactive conditions. Installed apps and prior release assets are not replaced by these builds.
