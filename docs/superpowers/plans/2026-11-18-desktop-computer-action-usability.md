# Desktop Computer Action Usability Implementation Plan

> **For agentic workers:** Use subagent-driven-development. The user already
> approved this design and ordinary main integration. Do not re-ask execution
> or branch choices. Preserve the known Qt SDK overlay.

**Goal:** Add safe window-state and element-ID operations to the ordinary
controller/model tool while improving accessibility and cancellation feedback.

**Architecture:** One shared controller resolves evidence to native coordinates;
existing owned input sessions dispatch clicks. Backend extensions provide only
window state and fresh accessibility evidence. No second input executor.

**Tech Stack:** Existing C++17, Qt, Windows UI Automation/Win32 and Mac AX/AppKit.
No new dependency or driver. Direct main development, source leases below.

## Task 1: Freeze interfaces and preserve contracts — Root

Files: DesktopComputerPlatform.h, DesktopComputerBackend.h,
DesktopComputerTools.h; shared spec above.

- [x] Append native element identity/reference and accessibility-snapshot DTOs;
  preserve six-field aggregate initializers and old elements() overrides.
- [x] Declare Minimize/Restore state requests and optional backend methods.
- [x] Add elementId, optional display inspection window and execution-progress
  fields to ordinary APIs; old callers retain defaults.
- [x] Publish exact header contract to Windows/Mac owners before their edits.

## Task 2: Shared controller/tool and deterministic contracts — controller owner

Files: DesktopComputerTools.cpp and .h; DesktopComputerContractsTest.cpp.

- [x] Add fake-backend failing cases: metadata-only minimized observation,
  restore/minimize verified state, dry-run with zero native effects, changed PID,
  cloaked denial and consumed token.
- [x] Add failing window/display element tests: opaque IDs, one current source,
  coordinates XOR ID, cross-session/snapshot rejection, changed label/geometry,
  disabled/offscreen/missing/ambiguous identities, no identity fallback.
- [x] Add no-holds observation -> release -> old-token rejection and the inverse
  late-old-release -> fresh replacement-token success cases for both scopes.
- [x] Add precise malformed display_id/observe-field/host-workspace cases and
  execution-attempt facts. Invalid parameter cases must make zero backend calls.
- [x] Build/run desktop_computer_contracts_test through run-tests-windows.ps1
  before implementation; preserve actual expected RED assertions.
- [x] Implement state-only observations and bounded source metadata; create
  snapshot-local element maps. Match fresh source identity and element before
  deriving ratios/native point. Keep input ownership and secure-field guards.
- [x] Add minimize/restore to schema/validation and retain focus; improve errors
  and examples. Never add alternate shell/UIA Invoke input paths.
- [x] Run the same contract executable GREEN and existing input/ownership suites.

## Task 3: Windows evidence and state APIs — Windows owner

File: DesktopComputerWindows.cpp. New owned test:
desktop/tests/DesktopComputerActionsNativeTest.cpp (Root registers target).

- [x] Add UIA RuntimeId/automation/enabled/offscreen evidence and bounded status,
  using fresh ControlView traversal for re-resolution; keep COM lifetimes local.
- [x] Add normal Win32 minimize/restore with identity/cancel/readback checks;
  no AttachThreadInput/foreground-lock or privilege workarounds.
- [x] Create an owned Qt window/button/anonymous control and separate QMenu.
  Run controller/tool calls off the GUI thread while pumping only the fixture.
- [ ] Verify observed ID clicks a real button, old IDs reject after movement/
  replacement, popup source can use display scope, and window minimize/restore
  preserve identity while invalidating prior actionable evidence.
- [ ] Save owned screenshots/receipts. Do not inject a global Win+D or inspect
  real user windows for testing. Root serializes normal Ninja/GUI consumers.

## Task 4: Mac source adapter — mac-maichat

File: DesktopComputerMac.mm only, based on frozen headers.

- [x] Implement AX evidence/reference re-resolution and honest status/limits;
  no fake stable identity from label plus rectangle.
- [x] Implement normal minimize/restore and report refusal; no TCC/permissions
  changes, new SDK/Git fetch, installed-App replacement or force-focus path.
- [x] Compile current ordinary consumer and validate owned native APIs where
  existing authorization permits. Reuse Windows shared-contract results.
- [x] Return exact delta/manifest and boundaries to Root, no independent push.

## Task 5: Integration and model guidance — Root

Files: desktop/CMakeLists.txt, new test registration, relevant controller/tool
guidance and docs/diagnostics/2026-11-18-desktop-computer-actions.md.

- [x] Register the owned native target with existing runtime gate and explicit
  Windows QPA/opt-in controls; build normal maichat before native execution.
- [ ] Run owned native, contracts, input-session/ownership and existing browser
  relevant regressions once on merged source; widen only for a concrete failure.
- [x] Independently review spec compliance and code quality. Document actual
  action fields, normalized/native coordinate labels and partial-error recovery.
- [x] Stage only leased files, commit main and normal push; verify remote SHA.
  Preserve user data/Apps/release tag/assets and existing SDK application state.

Exact deterministic target commands use:

```powershell
& MaiChat/desktop/scripts/run-tests-windows.ps1 `
  -BuildDirectory .cache/build-qt6-ui-20261109 `
  -TestRegex '^desktop_computer_contracts$'
```

Root records the actual native target command and its explicit owned-fixture
opt-in when that target is registered. No user confirmation is needed for
already-approved source changes or these isolated tests.

## Verification boundary

Shared contracts, ownership, input events and input-session regressions passed
on Windows. The inactive owned native state/accessibility fixture passed
16 checks without requesting focus or injecting input. The full click/popup
fixture remains pending because normal Windows foreground activation was
refused and the user is away. The browser regression stopped at its existing
visible-tab prerequisite; this is recorded as not passed. Do not mark either
interactive case green from the inactive fixture.

Mac rebuilt all 36 agent consumer translation units with the new ABI and
linked the complete archive. Its permission-unavailable/cancellation guards
passed 14 checks; authorized AX/state/click paths and a fresh complete Mac App
package were not verified. No permissions, installed apps or release assets
were changed. See the diagnostic records for exact source/runtime identities.
