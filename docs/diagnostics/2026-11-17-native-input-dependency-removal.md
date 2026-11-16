# Native desktop input dependency removal

Date: 2026-11-17. Platform: Windows 11, normal Qt Windows QPA build.
Starting source: `3134495e` on `main`; the input repair described here was
uncommitted at the start of final verification.

## Requested behavior

The user requires removal of the newly introduced input libraries. Ordinary
keyboard/mouse operations, independent edges and their combinations must use
system APIs and one execution core. Virtual controllers and hardware-HID device
emulation are deferred. This is not a claim of complete Sunshine equivalence.

Reproducible acceptance includes a held left button surviving an owned-window
capture, subsequent movement and explicit release; named and typed input sharing
modifier/key bindings; received Unicode text retaining order; five mouse buttons,
relative movement and both wheel axes. The tests must observe the receiving
window, rather than treating successful `SendInput` calls as delivery evidence.

## Source and build removal

Removed the newly added `libvirtualhid` and `ViGEmClient` source directories,
gitlinks, `.gitmodules`/local Git configuration sections, CMake helpers, SDK
bindings, duplicate router and SDK-only probes/tests. The two pre-existing Qt
submodules remain. No replacement SDK was introduced.

The rejected candidate and public vendor source were archived before removal:

- `.cache/native-input-dependency-removal-20261116/rejected-input-candidate-source.zip`
- 2,369,825 bytes; SHA-256
  `f0dfdb1f34299ee54f0d33970510e062a1701d4cf40445175a0437829913f4d8`
- The archive excludes Git metadata and contains only hashes of the three
  protected pre-existing WIP files. It is an audit snapshot, not a release.

`before-removal-lock.json`, `removed-sdk-bindings.json` and
`verified-sdk-removal-targets.json` record the exact removal boundary. Historical
SDK and native-input evidence stays historical; it is not used to accept the
replacement implementation.

The dependency rule is recorded identically in root `AGENTS.md`/`CLAUDE.md`,
commit `3134495e`: a new library requires explicit user confirmation; approved
source belongs in `MaiAgent/third_party` and is directly tracked without a new
Git repository/submodule.

## One native keyboard/mouse core

Ordinary Controller calls and the model tool enter the same owner context and
common policy session. Named/composite commands and `input_events` then dispatch
to the same `WindowsInputSession` and its single `InputScope` ledger. It owns
the resolved native binding, logical aliases and paired releases. There is no
library-backed keyboard/mouse provider or borrowed-state bridge.

The common wrapper owns scheduling, repeat, options, cancellation generations
and target validation. It reads native state instead of keeping another held-key
map. A raw-key release/repeat retains its first-down native binding, even after
layout/options change. Free ordinary APIs invoke the same native kernel.

An empty physical ledger may retain configuration/wheel remainder until release,
task/target change or idle expiry. This policy context does not monopolize the
desktop or pretend to be a virtual device. Expired empty contexts are retired
before an observation can extend them.

## Final verification record

All Windows tests enter `MaiChat/desktop/scripts/run-tests-windows.ps1` with the
normal Qt Windows runtime. Root owns the single build/global-input slot. Owned
receivers record their native message coordinates using the event's `lParam`
and `ClientToScreen`; later cursor samples are admission checks only.

The first SDK-free normal build succeeded. Five contract suites subsequently
passed: public tool contracts, ownership, DTO/validation, common scheduling and
the native implementation with per-session recording hooks. These recording
tests do not constitute OS-input acceptance. Final real-input results and exact
artifact hashes are recorded below after the final source freeze.

Initial final-source runs retained for diagnosis:

- New receiver exited 77 before input because the cursor was outside its owned
  window after foreground activation. Its initializer was corrected to move or
  click only its separately hit-tested owned activation point; later admission
  checks remain strict.
- Existing two-process native regression reached `Numpad7`, reported dispatch,
  but its received-key journal did not yet contain that key. This failed run is
  retained in `native-existing-run`; it is not a passing acceptance result.
- Scripted Agent host tests passed (three test bodies, plus init/cleanup). The
  first native Agent attempt skipped before input because its child was not the
  actual foreground window. A skip is not acceptance.

The physical-key recorder was subsequently made read-only, in addition to
disabling its input method. Setting the Qt attribute alone could be re-enabled
by QLineEdit. This affected only the owned non-text probe; the real text editor
kept normal IME behavior. The cancellation regression now first observes an
actual owned right-Control down, then cancels the next request and requires the
paired up with no late Shift/F24 down. It no longer counts internal validation
callbacks to guess when a down has happened.

The new receiver also exposed a real cross-entry mouse defect: named down used
`mouse:left`, while typed up looked for `typed_mouse:1` and returned success
without releasing the named hold. The product fix makes both entry points use
the same canonical button identity. Five-button, both-direction and duplicate
down contracts went from 15 failures to zero; raw-key aliases were unchanged.
The real hold/capture/move/up regression then passed without weakening its
received-event or final-state assertions.

The receiver only retries the exact Common admission Busy result, before any
native effects, for at most two seconds. Every attempt is recorded. It does not
retry target/focus/partial-dispatch errors or replay a model token. The successful
run recorded 27 admission retries for F6 up, then required the actual up, empty
state and a 600 ms quiet window. Measured repeat dispatch on this host was about
230 ms; this is not proof of a 15 ms end-to-end repeat period.

Final SDK-free normal build and runtime results:

| Check | Result | Evidence |
|---|---|---|
| Final normal build | exit 0 | `normal-native-delivery-build.txt`, plus receiver read-only fixture rebuild |
| Contracts, ownership, DTO, Common, Windows hooks | 5 suites passed | `normal-delivery-contracts.txt`; DTO 512, Common 188, native hooks 184, all zero failures |
| Retained pointer-candidate recording contracts | 124 checks, zero failures | `pointer-candidate-pure.txt`; fake transport only, no device or OS input, not native Touch acceptance |
| Real same-session system input receiver | 71 checks, zero failures, no skip | `system-native-delivery/system-native-receipt.json`; PID 46996 |
| Real two-process native workflow | passed, 12.65 s | `native-existing-readonly.txt`; parent 49140, child 47896, HWND 7474600 |
| Scripted Agent host contracts | 3 test bodies passed | `agent-script-delivery-qt.txt` (5 including init/cleanup) |
| Agent through native owned editor | passed, no skip | `agent-native-final-qt.txt`; parent 43832, child 61576, HWND 4001382 |
| Active SDK symbols / generated build references | zero / zero | `final-sdk-symbol-scan.json`, `final-source-artifact-manifest.json` |

The Agent-native setup called ordinary `SetForegroundWindow` only for the
PID/executable-verified fixture child. The test then independently observed that
child as foreground and idle; no injected setup input, thread-input attachment
or forced foreground mechanism was used. A ScriptModel drives these Agent
tests; they verify the real tool/native/UIA chain, not a provider model's visual
reasoning or paid inference.

The owned held-window PNG and the final two-process editor/creation-center
images were visually inspected. Source and eight binary/runtime-artifact SHA-256
entries are in `final-source-artifact-manifest.json`. All owned test processes
exited; the installed PID 56404 and protected WIP hashes remained unchanged.

Evidence root: `.cache/native-input-dependency-removal-20261116/`.

## System and delivery boundaries

The installed Windows App was not replaced or stopped. At verification start it
was PID 56404, `D:/Program Files/MaiChat/maichat.exe`, version 2.2.65. Build-cache
executables are test artifacts and include unrelated preserved WIP; they are not
approved installation packages.

The previously installed ViGEmBus driver is a separate system change. This source
repair neither uses it for ordinary input nor uninstalls it. A libvirtualhid
broker/driver was never installed or activated. No additional driver, service,
layout/IME switch, device pairing or system permission was introduced here.

Touch/Pen remain separate system-input candidates with incomplete native
acceptance. Earlier sustained-touch failures are not relabeled as fixed or as
mouse-drag failures. Deferred controller/profile/feedback DTOs are not active
providers; unsupported requests must fail explicitly. Full current tool actions
are listed in `2026-11-17-desktop-computer-operations.md`.

Unrelated UI/thumbnail/Starter/Android/iOS changes and protected WIP remain
preserved. Incoming mac-maichat work is deferred per the user's instruction.
The directory reorganization remains a document-only proposal.
