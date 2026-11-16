# Desktop input primitives implementation plan

> **For agentic workers:** Use superpowers:subagent-driven-development where a platform consumer can be delegated; execute dependencies sequentially and preserve the existing main checkout's WIP.

**Goal:** Provide general individual mouse/keyboard operations and their composition, with image observation independent of held input state. No task-specific recipes or site logic.

**Architecture:** Keep the ordinary controller as the shared entry for model and non-model callers. Native input sessions own synthetic down/up pairs across calls; the controller binds one held session to a target and excludes competing input owners. Capture never releases buttons. Explicit release, task termination, errors and bounded inactivity clear only owned inputs.

**Tech Stack:** C++17, Qt, Windows SendInput, macOS CGEvent/Accessibility, existing tool contracts and owned native fixtures.

## Contract and scope

- Preserve existing click, double_click, right_click, drag, move, type, replace_text, key, scroll, focus and verify behavior.
- Add mouse_down/mouse_up with left/right/middle/back/forward button selection, key_down/key_up for a single named key, input_state and release_all.
- Permit generic button selection for click/double_click/drag where platform support exists; retain right_click as the existing compatibility action.
- Support horizontal and vertical scroll through an axis parameter; retain the existing vertical default.
- Held keyboard modifiers combine with pointer, wheel and key operations. An independent key tap does not release previously owned held modifiers.
- Each mutating action still needs a valid bound target/token, except release operations which must work after cancellation, token expiry or loss of foreground. Captures and returned observations preserve held state.
- Report tracked buttons_down/keys_down and hold expiry. This reports injected ownership, not a claim about physical hardware state or application acceptance.
- Only one owner may hold global desktop input. Other sessions/controllers may observe but cannot inject into that hold. Changing the bound input target requires releasing first.
- Refuse adopting buttons/keys already physically held before our down. Errors release the current session's synthetic holds; never issue a blanket OS release.
- Use a 120-second inactivity deadline, renewed by valid owner operations/observations. Cancellation, turn completion, clear and controller destruction release the owner's inputs. Never retain a MaiToolContext/cancel pointer between calls.
- Native APIs remain platform adapters; unsupported system keys or platform buttons fail explicitly. Windows is validated here; Mac receives independent consumer review/verification when available.
- No running user App replacement, release packaging, account/data modification or service deployment in this scope.

## Task 1: prove the missing generic composition

**Files:** `MaiChat/desktop/tests/DesktopComputerScreenshotChainNativeTest.cpp`, `MaiChat/desktop/tests/DesktopComputerContractsTest.cpp`.

- [x] Add an owned native case using actual tool calls: mouse_down -> observe -> move -> mouse_up; assert the receiver sees held movement and no early release.
- [x] In the same owned surface exercise key_down Ctrl -> click -> key_up and compare event modifier flags, plus independent middle/right buttons and both scroll axes.
- [x] Run the new case against the baseline and retain the actual unknown-action RED. Existing tests still cover atomic drag and screenshot preservation independently.
- [x] Add deterministic session/target ownership, cancelled release, expiry, partial-error and destructor cleanup tests using a recording native-session seam.

## Task 2: native sessions and portable ownership

**Files:** `DesktopComputerPlatform.h`, `DesktopComputerBackend.h`, `DesktopComputerTools.h/.cpp`, and a focused portable held-session unit beside those files if needed.

- [x] Define `DesktopNativeInputSession` with input, state and releaseAll methods and a native factory; default fake backends explicitly reject stateful input until they implement the seam.
- [x] Bind session state separately from one-use observations so observe can renew a token without releasing inputs.
- [x] Serialize input, enforce one global owner, and add the inactivity watchdog with callback-free stored state and deterministic cleanup.
- [x] Wire JSON actions, buttons, axis and state fields to the same ordinary API. Validate exact allowed fields before dispatch; retain old defaults and bounds.

## Task 3: platform adapters

**Files:** `DesktopComputerWindows.cpp`, `DesktopComputerMac.mm`, `DesktopComputerKeys.h/.cpp` only for shared canonical key identity if required.

- [x] Retain owned native releases and Mac CGEventSource across calls; copy per-call request data and clear cancellation callbacks before returning.
- [x] Separate persistent downs from transient shortcut/click pairs. Targeted up releases the original recorded native key, not a newly resolved layout binding.
- [x] Distinguish owned modifiers/buttons from unexpected external holds; preserve the latter's preflight guards.
- [x] Add supported mouse buttons and horizontal wheel mapping. Pointer movement chooses the appropriate dragged event on macOS when a button is owned.

## Task 4: lifecycle and model instructions

**Files:** `agent/AgentController.cpp`, `MaiAgent/AgentTools/MaiScreenshotTool.cpp`, `docs/development/desktop-computer-coordinates.md`.

- [x] Share the ordinary controller with host lifecycle handling; release on interrupt, idle/error, clear and destruction without holding dangling per-call data.
- [x] Replace fixed task sequences in the system/tool descriptions with capabilities, coordinates, focus and token/state rules. The model chooses whether to click, type, move or observe.
- [x] Document actual primitive support, atomic versus stateful actions and platform boundaries without prescribing a business workflow.

## Task 5: verify and freeze

- [x] Run controller contracts, host lifecycle tests and owned Windows native individual/composition cases; Mac records compile/seam with AX0 boundary. Use exact new tokens and real native receiver events rather than cursor position alone.
- [x] Recheck cancellation, stale geometry/tokens, secure input, Unicode and legacy pointer/key regressions. Run the required eight Core suites if shared screenshot description changes.
- [x] Record source, executable and loaded platform DLL hashes, process IDs, actual screenshots and results in `docs/diagnostics/`; distinguish Windows execution from Mac review.
- [x] Preserve WIP3 hashes, stage only task files, commit on main and push. Runtime installation remains a separate pending authorization.
