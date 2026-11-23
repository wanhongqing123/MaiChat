# macOS screenshot runtime availability diagnostics

Current integration status: the portable diagnostic/state fixtures pass, Root
registered the ordinary helper source and test through two narrow CMake insertions,
and the normal Windows App rebuilt. The Mac owner independently compiled the
actual `.mm` in the normal App and ran its offline state fixture without a native
stream or TCC access. This corrects classification/diagnostic behavior; it does
not identify the historical false predicate or claim a live capture reproduction.

## Request, reproduction, and expected behavior

- Report date: 2026-11-22, Asia/Shanghai. Analysis host: Windows; no macOS runtime available to this investigator.
- Source inspected: `38d25b9d7a41ad29128e95289f44937c676ca452` on `main`, with unrelated work already present. The screenshot implementation/header/tests were clean at investigation start.
- Reported running artifact: user's real Mac MaiChat application, batch 10 feedback. Its exact executable identity, source revision, macOS version, and failure timestamps have not been independently established here. The local source revision must not be presented as the running artifact's revision.

Reported operations, in order:

1. Run `screenshot` capture.
2. Run `desktop_computer` display observation.
3. Run `desktop_computer` window observation with `include_image=true`.
4. Each image operation reports `GUI session is unavailable for screen capture`.
5. Window and display listing works.
6. Run `screenshot` with `action=stop`, then immediately capture again.
7. The new capture succeeds with an image of 2464 by 1600 pixels.

Expected: report the actual observed desktop fields, check phase, separately observed screen-recording permission, and resident capture state. A runtime desktop/session conflict must not imply missing application configuration. When releasing the resident session is a useful recovery step, describe `screenshot(action=stop)` followed by retry. Preserve existing authorization, capture targets, stream reuse, and stale-frame rejection behavior.

## Verified source call chain

- `MaiAgent/AgentTools/MaiScreenshotTool.cpp`: capture constructs a `DisplayUnderPointer` request and calls `maiCaptureScreenshot`; stop calls `maiStopScreenshotCapture` before capture workspace/model checks.
- `MaiChat/desktop/src/MaiChatTools/DesktopComputerTools.cpp`: `NativeComputerBackend::captureDisplay` constructs `DisplayById` requests; `NativeComputerBackend::capture` constructs `WindowById` requests. Both call the same `maiCaptureScreenshot` implementation.
- The same native backend's `displays()` and `windows()` instead call `maiDesktopDisplays()` and `maiDesktopWindows()`. Listing success does not evaluate every screenshot availability predicate and does not inspect resident stream failure state.
- `MaiAgent/AgentTools/MaiScreenshotMac.mm`: entry checks desktop availability, then separately checks `CGPreflightScreenCaptureAccess`, acquires the resident worker, and queues a command. The worker checks desktop availability again, resolves the target, ensures the native stream, waits for a frame, encodes, and validates the frame stamp. Frame callbacks also check desktop availability. Delivery performs another context check before publishing pixels.

## Root cause evidence and limits

The confirmed diagnostic defect is information loss and error classification, not a proven cause for the original macOS observation.

`DesktopAvailability` records `guiSession`, `onConsole`, `loginDone`, `frontmostKnown`, and `loginWindow`. `desktopCanCapture` requires the first four to be true and the last to be false. The entry point, worker, and frame callback all reduce a failed predicate to the same `NotConfigured` error and text. Neither the rejecting field nor the call site survives into the response. A missing/non-Boolean session dictionary value currently also becomes false, so even a printed boolean needs careful interpretation. The delivery check additionally combines desktop rejection and a negative permission preflight with `||`, producing one generic `NotConfigured` message.

The current file has no `availabilityError` helper. Any new helper must preserve the distinction between an observed desktop rejection and an actually sampled negative permission result. A desktop rejection alone is not evidence that TCC screen-recording authorization is absent, that settings must change, or that the application must restart.

`rejectDesktopLocked` invalidates cached pixels, advances `availabilityEpoch`, sets `failed=true`, stores the error, and clears the `capturing` counter. It does not close `stream_`. A late Complete frame is explicitly prevented from reviving a failed generation. Native stream stop errors and unreadable Complete frames also latch failure; soft Blank/Suspended states invalidate frames without universally making the generation terminal.

Crucially, `ensureNative` already reuses a stream only when the target matches and `failed` is false. On a failed session it calls `closeNative`, clears the failed/error state, advances generation, and rebuilds. Therefore a latched desktop error by itself does **not** establish a permanently stuck implementation that can only be repaired by explicit stop. The next entry/worker availability sample can still reject before reaching that existing recovery path. Repeated frame callbacks may also invalidate a rebuilt stream, but the old report does not identify whether that happened.

`CaptureRegistry::stop` advances its epoch, removes the resident capture, stops/joins its worker, and closes the native stream. The next permitted capture can construct a new resident capture. This explains what the stop operation changes; the observed recovery does not identify the previously false desktop predicate or original rejecting phase.

`MaiScreenshotCaptureState` currently exposes counters plus `workerRunning` and `capturing`, not terminal failure, native stream ownership, or the last availability observation. `stream_ != nil` means a native stream object exists; it does not establish that ScreenCaptureKit is delivering frames. `capturing` is set on successful stream start or complete frame and cleared on several invalidations, so it is not a permission receipt or a complete native stream lifecycle enum.

No screenshots, desktop input, TCC changes, pairing changes, application restarts, real user database access, SSH calls, or release-build operations were performed in this investigation.

## Agreed typed diagnostic contract and implementation

The root agent approved the narrow diagnostic change after investigation, with the existing gate and capture lifecycle preserved. `MaiScreenshotAvailability.h/.cpp` supplies ordinary C++ observation, snapshot, history, gate, and error helpers. The macOS entry, worker, callback, and delivery checks use the shared evaluator. `MaiScreenshotCaptureState` now exposes `failed`, `nativeStreamPresent`, and typed availability history alongside the existing counters.

| Observation | Required representation |
| --- | --- |
| Check phase | Explicit Entry / Worker / Frame / Delivery value |
| Five desktop fields | Per-field True / False / Unknown, retaining the existing fail-closed predicate; Unknown must not be printed as a positively observed false |
| Permission preflight | Unknown / Granted / Denied; only report a result if `CGPreflightScreenCaptureAccess` was actually sampled |
| Resident state | `failed`, existing `capturing`, and truthful native stream ownership (`nativeStreamPresent`); use `streamActive` only if it has a separately documented lifecycle observation |
| Failure provenance | Preserve the rejecting phase and snapshot, independently from a later/current diagnostic sample |
| Read-only diagnostics | No worker startup, capture, permission request, or state reset |

Classification and recovery:

- Runtime desktop rejection: `Busy`, identify all observed desktop fields and the rejecting phase. Do not recommend permission changes from this evidence. Explain that the desktop may need to become available; when recovering the resident capture, use stop then retry.
- Actually sampled negative screen-recording preflight: `NotConfigured`, retain the existing permission instructions and existing request behavior. Callback checks must not add a permission request or silently treat an unsampled preflight as denied.
- Resident failure with desktop and permission currently available: identify it as resident failure/conflict, preserve the original cause and use an actionable stop/retry message. Preserve the current `ensureNative` recovery behavior; do not add another retry/rebuild mechanism in this diagnostic change.
- Cancellation, invalid target, stale generation/frame, and genuine native failures retain their existing semantics unless a narrowly specified test requires a classification correction.

Example runtime text (illustrative fields, not the old event's actual values): `screen capture temporarily unavailable; phase=frame; guiSession=true; onConsole=true; loginDone=true; frontmostKnown=false; loginWindow=unknown; screenRecordingPermission=unknown; failed=true; nativeStreamPresent=true. Make the desktop available, then retry; if the resident capture remains unavailable, use screenshot(action=stop), then capture again.`

The error and diagnostic snapshot must not contain window titles, images, application identifiers, tokens, or other user content.

## Test plan and verification boundary

Before production edits, an owned harness mechanically extracted the unchanged production `DesktopAvailability`, `desktopCanCapture`, and entry rejection statement. It compiled against the real `MaiError.cpp`. All five independent rejecting-field fixtures failed with the expected original defect: each returned `not_configured: GUI session is unavailable for screen capture` instead of Busy plus field evidence. This established RED without capturing anything or replacing native behavior with a guessed implementation. The preserved baseline source SHA-256 is `1D44F72AAA439024F1FC9815D9D5E32629290EF4A6D787AF48AB43D84CC7CAA0`.

Required pure cases:

1. Each desktop field independently blocks capture and appears accurately in the diagnostic, including Unknown values from absent observations.
2. Every runtime rejection uses Busy and includes the phase and stop/retry recovery, while a negative preflight alone uses NotConfigured.
3. Unsampled permission remains Unknown and produces no permission-setting instruction.
4. A resident failure snapshot preserves its original phase/fields even after a later healthy desktop observation.
5. A native stream object can be present with failed=true/capturing=false; no field claims active delivery solely from object ownership.
6. A current healthy snapshot does not reuse a previously failed frame generation. Existing native white-box frame invalidation tests remain applicable.

Existing `MaiScreenshotSessionTestsMac.mm` has an offline `MAI_SCREENSHOT_STATE_TEST` path with synthetic samples that tests generation/availability invalidation without starting a native stream or reading user pixels. It still requires macOS Apple frameworks and cannot run on this Windows host. Cross-platform tests should compile only the ordinary pure diagnostic core in a new owned tiny harness, without shared caches, new dependencies, or changes to the release artifact.

Verification at 2026-11-22 17:30 +08:00 used only `.cache/screenshot-runtime-state-20261122/build`, configured with the Visual Studio 18 2026 generator, x64, MSVC 19.51.36260, and Windows SDK 10.0.28000. No existing shared/release build cache was used.

```text
cmake --build .cache/screenshot-runtime-state-20261122/build --config Release --target ScreenshotAvailabilityTests ScreenshotNativeStateTests
ScreenshotAvailabilityTests.exe
screenshot availability failures=0 (pure fixtures; no capture/UI)
ScreenshotNativeStateTests.exe
extracted production state failures=0 (no Apple APIs/capture/UI)
```

The tracked pure fixture test covers each rejecting field; every Unknown field across all four phases; Unknown/True/False permission observations; desktop-priority runtime classification; stop/retry wording with an explicit active/unlocked desktop prerequisite; failed stream-object ownership without claiming active delivery; and actual history updates preserving the last rejection after a healthy observation.

The second owned harness mechanically extracts the actual portable `CaptureCounters`, `StreamState`, frame-stamp checks, invalidation, and `checkAvailabilityLocked` from the edited `.mm`, excluding Apple APIs. It verifies rejection/frame invalidation, the returned post-rejection state, preserved history after a healthy check, inability to revive the old frame, and zero worker/session starts. This compiles real state-update code; it is **not** compilation of the complete Objective-C++ translation unit.

The offline Apple fixture was updated for the typed observations and includes missing/non-Boolean/False/True session-dictionary provider cases. Those provider cases and the existing synthetic Apple sample tests remain unexecuted here. Formatting and `git diff --check` passed for the scoped edits. The repository's format file declares only Cpp, so `.mm` formatting used the same rules with an assumed `.cpp` filename; the initial direct `.mm` formatter call rejected that language configuration and was corrected without changing the configuration.

Verified harness executable SHA-256 values:

- `ScreenshotAvailabilityTests.exe`: `6A482AAD47F745041650DA0E07F26A9C638A2375BA15728C237A4ECC0650D2AE`.
- `ScreenshotNativeStateTests.exe`: `305F97FCC9E8EAEAE04A89F5E118F8ABD5674FE86A22C54E16FBF33CFFEE2ED1`.

At worker handoff, the dirty repository `MaiAgent/CMakeLists.txt` was intentionally
left for Root's controlled integration. Root subsequently registered
`AgentTools/MaiScreenshotAvailability.cpp` after `MaiScreenshotTool.cpp` and added
`MaiScreenshotAvailabilityTests` to the ordinary test list. Removing only those
two insertions reproduces the original CMake WIP bytes exactly.

Pending actual macOS validation: run the same reported image operations and stop/retry sequence against the rebuilt application, record executable/source identity and per-phase observations, then verify native stream reuse, generation rejection, and no stale image delivery. Until then, this is source-based localization plus verified portable diagnostic/state behavior, not macOS acceptance or a confirmed explanation of the historical false predicate.

## Final independent Mac and Root intake

On the private f47 baseline, all 13 source-only postimages matched the packet
hashes and the normal Mac App compiled/linked with new `.mm`, helper and ABI
consumers (25 steps). A standalone owned project then ran the pure availability
test and `MAI_SCREENSHOT_STATE_TEST` fixture that includes the actual production
`.mm` rather than an extracted substitute; both exited 0. Its call graph was
checked not to create SCStream, read the current desktop/session, request TCC,
start/stop a user's capture, or inject input. Native live capture remains untested.

Root verified the final 37,334-byte evidence ZIP SHA256
`a98b90ce7d2cc4c5b7d59197bfc7f657abf22a69a1e08023f9879f26929c9414`,
all 23 manifest entry sizes/hashes and byte equality of the separate safe JSON.
Root read the pure/offline logs directly; the new ordinary OCR comparison is
recorded separately in `2026-11-22-ocr-wide-line.md`. Evidence intake is under
`.cache/post312-validation/mac-validation/`, including `root-intake-safe.json`.

Root preserved old aggregate field order by appending the new capture diagnostics
after the original counters. Final normal Windows build and private staging/ZIP
bare-PATH smoke passed; Core8 plus storage plus pure availability passed 10/10.
The user's installed Mac PID 438, main 11a4e and public 3.1.2 were untouched by
this validation. These new source changes remain uncommitted and unpublished.
