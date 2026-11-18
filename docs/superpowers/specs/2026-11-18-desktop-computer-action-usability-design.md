# Desktop computer action usability

Approved by the user on 2026-11-18 after discussion of window operations,
snapshot-bound control IDs and coordinate spaces. Work remains on main.

## Goal and preserved behavior

An Agent can restore/minimize a discovered window, choose an observed control
by ID, and recover from precise validation or interruption feedback. The
existing system-API controller/native input session remains the only input
executor. No third-party library, driver, shell fallback, foreground-policy
bypass or user-display setting is introduced.

Existing ratio-based calls, focus, display keyboard input, input_events and
steering remain supported. Steering finishes the current tool and asks the
model to reconsider queued calls; Stop cancels and releases owned controls.
No input batch promises transactionality or exact receiver acknowledgement.

## Public actions and observations

Add `minimize` and `restore` to window-target actions. Keep `focus`; an
additional activate synonym is unnecessary. Each state action requires a
current observation token and exact window identity. Minimized/cloaked windows
can produce metadata-only observations, but no screenshot or ordinary pointer,
keyboard, verify or focus action can use unavailable input geometry.

A window-state result contains before/after state and the actual focus result
when readable. A successful request consumes the old observation and instructs
the caller to observe again. Native requests use normal OS APIs, bounded state
verification and cancellation. Cloaked windows are reported, not moved across
virtual desktops. Failed or denied focus is reported honestly.

Window observations retain their existing normalized element bounds and add
an opaque `element_id` for a native element with a re-resolvable identity.
IDs are unique to the observation, rather than reusable e1/e2 indices. Only
click, double_click and right_click initially accept IDs; coordinates and
element_id are mutually exclusive in tool JSON. The ordinary controller uses
the same click path after resolving the chosen element to a native point.

Window examples (new properties/actions):

```json
{"action":"observe","target":"window","window_id":"123"}
{"action":"restore","target":"window","window_id":"123","snapshot_id":"snapshot-A"}
{"action":"click","target":"window","window_id":"123","snapshot_id":"snapshot-B","element_id":"element-B-opaque"}
```

For an independent popup/taskbar, use its discovered HWND/ID as the inspection
source of a display observation. `element_window_id` is an accessibility-source
selector, not a second input target. This bounded one-window inspection avoids
an unbounded desktop-wide UIA tree and allows existing display-scoped input to
switch foreground normally without trying to force-focus the taskbar/menu.

```json
{"action":"observe","target":"display","display_id":"65537","element_window_id":"456"}
{"action":"click","target":"display","display_id":"65537","snapshot_id":"snapshot-C","element_id":"element-C-opaque"}
```

Without element_window_id, display observation keeps its current behavior.
Display mutations retain that inspection source in the next observation when
it is still present; a missing provider/source is reported and must not erase
a successfully dispatched input receipt.

## Coordinate and identity contract

Model-facing ratio parameters remain fractions of the selected observed window
or captured display. Add explicit labels to native screen bounds. Windows UIA
bounds are physical screen pixels; Mac AX uses native screen points. Captured
PNG dimensions remain separate from native pointer bounds. The model selecting
an element ID does not perform that conversion.

The controller binds an element to session, snapshot, selected scope and native
source-window identity. Immediately before input it resolves the native
identity again and checks role/label/automation identity, enabled/offscreen
state and bounds. A changed, missing, ambiguous, recycled or out-of-scope
element is rejected without input and requires a new observation. The selected
center must be in the target bounds. Native runtime IDs/references are internal
and never accepted from model arguments.

Windows can re-resolve UIA RuntimeId by bounded fresh traversal. Mac can retain
an opaque AX reference in the snapshot and query that exact object again.
Snapshots own such references; replacement/clear/destruction drops them. No
unbounded cross-session native-element cache is added.

## Accessibility diagnostics

Preserve role, label and normalized bounds. Add enabled/offscreen and optional
automation ID, plus accessibility status/provider, visited/returned counts,
count-known flag, truncation and limit reason. Label-less controls with a native
identity may be observed; unavailable identity means no invented ID.

Use current window-list class/owner metadata to select separate Qt popups and
taskbar windows. A selected application's ControlView is not assumed to contain
the taskbar. Native traversal is bounded by existing node/return limits and an
explicit time budget with per-provider timeouts where supported. The result
must distinguish an empty/partial/provider-error observation from complete
coverage. Coordinate input remains available when accessibility is unavailable.

## Error, cancellation and execution facts

Decimal ID parsing reports the actual field (`display_id`, window_id or
element_window_id). Validation reports unknown fields, missing host workspace,
wrong target selector and invalid types separately. Workspace is host context,
not a model-supplied replacement parameter.

Release/Stop revokes the matching turn's observation even if observation never
created a native input session or held a key. Late cleanup for an older turn
must preserve a replacement turn's observation. Stop and clear release only
owned synthetic state; they cannot release physical user keys.

The existing success receipt keeps dispatched/input_state/accepted_events and
next observation. Failures must not invent completed event counts: expose
whether input dispatch or a window operation was attempted, the current owned
ledger and whether a fresh observation is needed. An attempted native event
may have partially reached the receiver; an error is not permission to retry
the whole workflow blindly.

## Ownership and verification

- Root owns shared DTOs/controller/tool validation/schema and main integration.
- Windows implementer owns DesktopComputerWindows.cpp and owned native fixture.
- Mac owner owns DesktopComputerMac.mm and Apple-only consumer validation.
- Shared deterministic contracts run once on Windows. Platform compilation and
  native API/owned-window behavior are checked by the responsible owner.
- Fake-backend tests cover identity, state-only observations, locator XOR,
  stale/disabled/moved elements, field diagnostics, no-holds Stop and late cleanup.
- Owned Windows Qt controls cover real UIA IDs, a popup outside its parent,
  minimize/restore, element click, layout change and cancellation. No global
  Win+D test, user window, user DB or paid model is needed.
- Existing browser/held-input and ordinary input regressions remain intact.

The other completed Windows repairs (SDR brightness and paused video generators)
stay separate. Font-size candidates remain private until the user's choice.
The running installed App is not replaced by this source work.
