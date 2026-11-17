# Sunshine input parity

> Superseded by the user on 2026-11-17. The libvirtualhid/ViGEmClient dependency approach in this historical document is rejected and must not be executed. Both added SDK/Git integrations have been removed. See `docs/superpowers/plans/2026-11-16-native-input-dependency-removal.md`; ordinary input uses one system-API native core, and virtual devices are deferred. Any new dependency requires explicit user confirmation.


## Requirement and reproducible acceptance

The user's requirement is the complete input functionality implemented by the fixed
Sunshine revision, exposed through both ordinary C++ and `desktop_computer`. MaiChat's
composite operations extend that baseline. Existing absolute clicks, named keys and
cross-call holds are an intermediate implementation, not proof of complete parity.

For each capability, send an event through the ordinary API and the model adapter to
an owned receiver, inspect the receiver's native event and resulting state, then
cancel/end the owning turn and inspect cleanup. Feedback travels in the reverse
direction from the virtual device to a bounded, session-owned event stream. A schema,
enum, recording backend or successful build alone does not establish native delivery.

The benchmark is Sunshine
[`bb6930bd782ed3edd2eea9a71d46b42183be9358`](https://github.com/SunshineStream/Sunshine/tree/bb6930bd782ed3edd2eea9a71d46b42183be9358),
with its libvirtualhid dependency
[`2234c0079a6a7b0e2f25990d3c705be85bcd37fd`](https://github.com/LizardByte/libvirtualhid/tree/2234c0079a6a7b0e2f25990d3c705be85bcd37fd).
The benchmark covers the server's actual input implementation and configuration,
not unrelated video/audio encoding, pairing or networking. Protocol fields that the
fixed server does not consume are recorded separately. Upstream defects are evidence
to investigate, not behavior to reproduce deliberately.

## Implementation choice

Use the same pinned MIT input library behind a private C++23 adapter. Keep MaiChat's
public input structures and the rest of the application C++17. Preserve current
window/display observations, ownership, turn cancellation and existing actions.
No Sunshine GPL source is copied into MaiChat.

Writing an independent driver and every controller report format would add a second
implementation of an already available library. Restricting the work to SendInput
would leave Raw Input and advanced controller profiles unimplemented. The library
adapter covers the full available backend, while existing native actions remain the
backward-compatible path and share ownership with the new primitives.

The MIT library and the separately licensed Windows driver package are distinct.
The application builds the library from pinned source without network downloads at
configure/build time. It does not bundle, install, activate or modify the restricted
driver package. Native validation requiring a system installation is a separate,
concrete approval after the application implementation and test package are ready.

## Components

* `DesktopComputerInputTypes.h/.cpp`: platform-neutral event, device, capability,
  state, feedback and result types; validation before narrowing or dispatch.
* `DesktopComputerHid.h/.cpp`: the only application adapter that includes
  libvirtualhid headers; device creation, input translation, backend capability and
  output callbacks. Its public boundary does not expose dependency types.
* `DesktopComputerInputSession.h/.cpp`: serialized ownership of the legacy native
  session and HID devices, cross-entry key/button conflict checks, cancellation,
  repeat/remap policy, bounded feedback and release of all owned resources.
* `DesktopComputerInputProtocol.h/.cpp`: model schema, strict argument decoding and
  receipts for the same ordinary event API. No OS injection or separate state machine.
* Existing controller: validate observation/target/turn/workspace once per submitted
  event batch, consume the input token before dispatch, and retain the distinction
  between dispatched input and a failed subsequent observation.

Do not reorganize unrelated controller, capture, UI or mobile code. Existing actions
retain their published coordinate and token semantics. New position-independent
relative, wheel and button primitives do not silently warp the pointer.

## Input and output contract

The event API includes absolute and relative motion, five mouse buttons, vertical
and horizontal wheel distance in 120 units per detent, raw physical key events and
Unicode text, multiple touch contacts, pen state, controller attach/state/touch/
motion/battery/detach and feedback. Controller profiles include generic, Xbox 360,
Xbox One, Xbox Series, DualShock 4, DualSense and Switch Pro, subject to the selected
backend's actual capability. Baseline configuration and lifecycle behavior are
tracked in the accompanying parity matrix.

Input batches are ordered and contain explicit events, not a prescribed business
workflow. Observation timing remains the caller's decision. Capture never releases
held input. A drag can therefore be expressed as down, movement, capture, adjustment
and up, while the existing atomic drag remains available.

Absolute/contact positions are normalized against observation-bound native bounds.
Relative motion uses signed deltas, not normalized absolute positions. Wheel distance
is independent of pointer position. Motion sensor units are accelerometer m/s² and
gyroscope degrees/s. Unknown angle/tilt fields remain unknown rather than becoming
zero. Native pointer coordinates and encoded image dimensions are distinct.

Device handles are session-owned identifiers with generations, never caller-supplied
OS handles. Controller output includes rumble, trigger rumble, RGB/player LEDs,
adaptive-trigger payloads and reporting requests where the fixed server implements
them. Polling feedback includes sequence and overflow information; it cannot renew
a screenshot token or impersonate a different device generation.

## Ownership and failure

Keep the existing controller/session/turn/target arbiter. Extend owned state to raw
keys/buttons, contact/pen streams, connected devices, repeat jobs and failed cleanup.
A neutral connected device still owns resources and must not disappear because its
buttons are up. Cleanup releases only this session's synthetic state, works after
focus or token loss, and attempts every resource even if one release fails.

Record native bindings before successful downs can escape. Repeated downs are
idempotent unless the explicit repeat policy requests a repeat. Named and raw paths
must not release one another's holds accidentally. Per-call cancellation callbacks
are never retained by devices, timers or output callbacks. Queued work uses its own
revocable lifetime. An error after dispatch means partial or unknown outcome and
requires observation before retry, not automatic resubmission.

Capabilities distinguish implemented code, runtime availability, backend delivery
mode, profile limits and native validation. In particular, no installed HID broker
does not disable the Windows system touch/pen path. Driver-backed Raw Input and
controller acceptance remain unverified on this machine until a real backend exists.

## Current validation boundary

This document starts the new exhaustive parity work at MaiChat `95b65d6e` on
2026-11-16. Previous c38 input tests validate the earlier keyboard/mouse subset.
They do not validate the new families. The stock Windows App is not replaced and no
system driver, service, license or user data is modified by this design work.
