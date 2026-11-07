# Resident Screen Capture Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development. Develop directly on main; Root owns builds and native runs. Preserve installed apps and user data.

**Goal:** Expose screen capture as the model's only screenshot entry and reuse a resident native capture worker across screenshot commands.

**Architecture:** Keep the ordinary maiCaptureScreenshot API and add ordinary stop/state APIs. A lazy process-level worker owns each platform's capture resources and serializes commands. Native window targets remain ordinary internal compatibility/controlled-test primitives; model screenshot schemas no longer select windows. Desktop computer observations provide window/control metadata; all model pixels come from the screen tool.

**Tech Stack:** Windows WGC/D3D11, macOS ScreenCaptureKit/SCStream, C++17, existing MaiAgent tool loop and inline image transport.

## User flow and lifecycle

The first screen screenshot starts the native worker/session. Repeated requests for the same monitor reuse the worker, device and session; each command snapshots the current complete frame and returns the existing workspace-relative image_path plus PNG attachment. Static screens remain usable even when Windows suppresses unchanged frames. The worker continuously consumes updates, and changed content must replace the prior image. Target changes, closure, resize, device errors and explicit stop must not return stale frames. Stop releases the session and joins its worker; a later request may restart.

The screenshot tool accepts capture/stop only, with capture as default. Remove mode/windowTitle and standalone list_windows from the model tool list. Keep window discovery/focus as desktop_computer actions for input targeting, but make tool observations image-free. No cloud video stream or public image upload is started; image bytes still travel directly to the selected provider.

## Tasks

- [x] Add ordinary capture state and idempotent stop contracts, unsupported-platform behavior and a capture callback seam for controlled screen-tool tests. Ensure real Agent dispatch permits stop without a workspace while retaining file-tool guards.
- [x] Add meaningful failing native reuse/static/changed-pixel/target-switch/resize/closed-target/concurrency/stop-restart tests using only owned windows. Public model tests reject window capture arguments and do not advertise list_windows.
- [x] Implement Windows resident worker: owned MTA/D3D/frame pool/session/staging/WIC, bounded waits, update draining, serialized commands, exception-safe completion and deterministic release.
- [x] Implement macOS resident SCStream worker under the same ordinary API, compile it, and verify availability epochs and terminal-state latching with offline RED/GREEN. Earlier owned-window lifecycle results are retained with their exact source boundary.
- [x] Simplify model tools to screen-only, retain correct same-file delivery and HTTP image bytes, and adapt product tests using an explicit owned-window capture source seam.
- [x] Run Windows native lifecycle, actual-display metadata and product-path cases, plus required Core/desktop gates. Record Mac final-source lock/permission/display boundaries separately.
- [ ] Resume final-source macOS native capture on an available authorized desktop; the locked-session failure and earlier-source success are not replaced by offline or Windows results.
- [ ] Review, commit/push and prepare a private package with full staging/actual-ZIP PE/model/bare-PATH checks. Do not install or publish without existing authorization.

## Evidence policy

An isolated source seam proves model routing and image flow, not production full-monitor capture. Ordinary native window tests prove reuse/resource lifecycle with controlled pixels. Do not infer live cloud visual understanding from scripted or local HTTP tests, or conflate capture success with keyboard/mouse acceptance.
