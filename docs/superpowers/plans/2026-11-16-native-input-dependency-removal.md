# Native desktop input dependency removal

> **For agentic workers:** execute the assigned file scopes in this plan. Keep one
> writer per scope and one build/test owner for the normal Windows cache.

**Goal:** remove the unapproved input SDKs and their Git/build integration while
preserving ordinary PC operations through one native input executor and ledger.

**Architecture:** the Controller and common policy wrapper own validation, ordering,
configuration and cancellation. Both named/composite input and typed primitives call
the same native session. The native session alone owns resolved physical bindings,
logical references and release responsibility. There is no sibling keyboard/mouse
provider, borrowed-state bridge or library-backed fallback.

**Tech stack:** existing C++17/Qt project, Win32 APIs and the existing platform
adapters. No new third-party dependency, driver installation, service change or
communication with mac-maichat is authorized by this plan.

## Scope and preserved work

The user explicitly deferred virtual controllers and hardware-HID device emulation.
Do not continue those SDK/provider implementations. Touch and pen are separate system
input candidates; their incomplete native validation must remain explicit. Ordinary
mouse hold/move/observe/up is retained and is not a touchscreen gesture.

Do not revert unrelated MainWindow, document-thumbnail, Starter, Android or iOS work.
Do not touch the three protected pre-existing WIP files. Do not replace the installed
Windows App or uninstall ViGEmBus as part of source dependency removal.

The full pre-removal operation inventory is
`.cache/desktop-computer-system-api-inventory-20261116/inventory.json`. Its 26 actions,
15 event types, configuration and keyboard names are an interface snapshot, not a
claim that deferred or unvalidated operations work. The mobile-readable Markdown
copy has been sent to the user.

## Frozen ordinary contract

```cpp
makeDesktopComputerInputSession(native, validator);
// Both methods operate on this same native instance:
native->input(request);
native->inputEvents(events, scope, port, options);
// state.rawKeys is generated from the native ledger, never stored by Common.
std::uint32_t maiDesktopInputKeyIdentity(const DesktopInputRawKey& source);
DesktopInputRawKey maiDesktopRemapInputKey(
    const DesktopInputRawKey& source, const DesktopInputOptions& options);
```

Identity retains the complete original 16-bit code and flags. Native first-down
processing resolves remapping and freezes the actual binding. Later up, duplicate
down and repeat use that binding; they do not reinterpret a new layout/options.
Common repeat stores the original source and consults `state.rawKeys`. The native
ledger handles aliases and the last logical reference, including transient chords.

## Task 1: preserve evidence and enforce the dependency rule — Root

- [x] Record the user's explicit approval requirement and direct-source/no-new-Git
  rule identically in `AGENTS.md` and `CLAUDE.md`.
- [x] Archive the current input candidate/public dependency sources without `.git`
  metadata. Keep only hashes of the protected WIP. Receipt:
  `.cache/native-input-dependency-removal-20261116/before-removal-lock.json`.
- [x] Preserve the existing Qt submodules. Remove only the two newly introduced
  input gitlinks/sections and their owned local registration after source backup.
- [x] Remove the SDK binding/helper/test sources from the product tree and all
  build references. Historical failed/positive evidence stays in the cache archive.

## Task 2: the single native Windows executor — Windows worker

Files: `DesktopComputerWindows.cpp`, new private
`DesktopComputerWindowsInputInternal.h`, `DesktopComputerWindowsKeyMap.cpp`, and
`tests/DesktopComputerWindowsNativeSessionTest.cpp`.

- [x] Reproduce old typed unknown-up failure without emitting input.
- [x] Make `WindowsInputSession::input` and `inputEvents` use the same `InputScope`.
  Replace, rather than duplicate, its native binding/hold records.
- [x] Implement relative movement, stationary five-button edges, signed fine
  vertical/horizontal wheel, raw physical keys and Unicode through Win32.
- [x] Project original raw sources from the ledger. Test alias references, first
  binding retention, duplicate/unknown edges and named/raw combinations.
- [x] Use immutable state snapshots for reads during long work. Cancellation
  revokes before cleanup; incomplete cleanup remains visible and retryable.
- [x] For window-scoped stationary down/wheel, reject an outside target before
  any emission. Display scope permits intentional cross-window input. Owned up
  and cleanup continue after focus loss.

Use injected private native-call hooks for deterministic contract checks. Run global
input only in an explicitly handed-off owned receiver slot. Do not claim Touch/Pen
native acceptance from API presence or an accepted injection call.

## Task 3: policy without another held-key table — Common worker

Files: `DesktopComputerInputSession.h/.cpp` and
`tests/DesktopComputerInputSessionTest.cpp`.

- [x] Replace the legacy/Hid pair with `native_`; remove Router/Hid construction,
  borrowed-state merging and the Common native-held-key map.
- [x] Retain option inheritance, explicit defaults, delay, repeat, epoch and scope
  checks. Do not apply remapping again before native dispatch.
- [x] Delegate state/capabilities/feedback/reset/release to the same native object.
  Native calls and accessibility validation never run under the Common state lock.
- [x] Preserve Busy reset without effects and asynchronous stronger release.
- [x] Migrate SDK-specific mocks to a single recording native session; preserve
  cancellation, no-late-repeat, scope loss, failed cleanup and first-binding cases.

## Task 4: Controller/context/build cleanup — Root

Files: `DesktopComputerTools.h/.cpp`, `DesktopComputerPlatform.h`,
`DesktopComputerInputTypes.h/.cpp`, `DesktopComputerInputOwnership.h/.cpp`, and the
input-only portions of `desktop/CMakeLists.txt`.

- [x] Route production mutation through the same native session; do not leave a
  Controller one-shot bypass to another executor. Existing free functions must
  invoke the same native kernel, not maintain their own second hold table.
- [x] Preserve empty-hold session configuration and wheel remainder until explicit
  cleanup, task/target change or idle expiry, without pretending it is a device or
  monopolizing the desktop. Test reuse and cross-owner behavior.
- [x] Keep independent input DTO/validation code C++17. Register only native/shared
  code; delete input SDK targets, includes, links and SDK-only tests/probes.
- [x] Keep document-thumbnail and Starter CMake changes intact; never restore the
  whole CMake file to remove input changes.
- [x] Describe deferred virtual-device features accurately; do not advertise
  unavailable provider/profile behavior as current support.

## Task 5: ordinary/model contracts — Contract worker

File: `tests/DesktopComputerContractsTest.cpp`.

- [x] Replace SDK-backed configuration doubles with one native recorder.
- [x] Test omitted options versus explicit defaults using ordinary keyboard/wheel
  behavior, including a session with no current physical hold.
- [x] Keep snapshot, coordinates, turn, owner, dry-run, rejection, call counts and
  cleanup assertions. Reset concurrency uses ordinary raw-key work, not a gamepad.
- [x] Archive controller/SDK-only expectations as deferred rather than pretending
  those tests passed or deleting ordinary functional requirements.

## Verification and completion gate

- [x] No input SDK header, namespace, CMake target or gitlink remains in active
  product/test build paths. No replacement library was fetched or introduced.
- [x] Native session and Common deterministic tests pass; Controller/ownership/
  input protocol tests pass. Dependency-free build graph is inspected.
- [x] Re-run the owned Windows basic keyboard/mouse/Unicode/hold-through-capture
  cases relevant to the changed executor, through the deployed-runtime test gate.
- [x] Check protected WIP hashes and unrelated feature sources remain unchanged.
- [x] Report precisely what was removed, what is verified, and which Touch/Pen or
  virtual-device items remain outside completed acceptance.

Normal build commands, after all assigned sources are ready:

```powershell
. .cache/qt-ui-boundary-20261108/enter-vs.ps1
cmake --build MaiChat/desktop/.cache/build-qt6-ui-20261109 --target desktop_computer_contracts_test desktop_computer_input_ownership_test desktop_computer_input_events_test desktop_computer_input_session_test -j 4
& MaiChat/desktop/scripts/run-tests-windows.ps1 -BuildDirectory .cache/build-qt6-ui-20261109 -TestRegex '^desktop_computer_(contracts|input_ownership|input_events|input_session)$' -VerboseOutput
```

No installed-App update or new system change is part of this completion gate.
