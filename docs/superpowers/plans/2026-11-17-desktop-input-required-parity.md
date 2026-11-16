# Required non-device desktop input parity

The user's confirmed scope is to complete the ordinary PC input capabilities
represented by the pinned Sunshine reference, using system APIs. Virtual
hardware/HID devices, virtual keyboard/mouse devices and gamepads are excluded.
No new third-party library or driver is authorized. This replaces treating
unfinished touch/pen or keyboard timing as indefinitely pending candidates.

Current host and acceptance platform: Windows. Do not claim Mac native runtime
acceptance from this Windows environment. Do not contact mac-maichat or resume
the paused UI/mobile work.

## Work and ownership

1. Root: complete window discovery (normal/minimized/tiny/untitled/owned/tool,
   no silent 200 cap, accurate state/geometry, no focus changes). Keep model
   discovery distinct from eligibility for observation/input.
2. Keyboard/Common worker: compare the pinned Sunshine repeat and ordinary
   key/mouse policy; measure the actual COM/UIA dispatch costs before optimizing.
   Preserve a fresh secure-input/focus check and the existing cancellation gate.
   Match the first-repeat scheduling origin without catch-up event bursts.
3. Pointer worker: identify the existing sustained-touch failure with owned
   native receiver evidence, then implement touch and pen through User32. Keep
   all native input under the same owner/session, normalized coordinate contract,
   cancellation epoch and cleanup responsibility. No SDK/device emulation route.
4. Root: integrate frozen adapter APIs in the existing Windows native session,
   update the tool contract, run the normal build and actual owned-window
   keyboard/mouse/touch/pen regressions serially.

Workers do not share product-file write leases. Root alone owns CMake, public
headers, DesktopComputerWindows.cpp/Scope integration and normal-cache/OS input
slots. Private record/compile is allowed; real-input experiments require an
explicit Root slot with exact commands and own HWND/PID preconditions.

## Required acceptance evidence

- Keep existing ordinary keyboard/mouse unit, ownership, tool and 71-check native
  regression, including held buttons across capture and mixed-entry release.
- Prove individual raw keys, modifier combinations, repeat on/off/first delay,
  scan/side/layout binding, both fine-wheel axes, relative/absolute motion and
  all five buttons against received native events. Test unknown/duplicate edges
  and failed/canceled cleanup without taking user-held controls.
- Prove touch down/hold/move/up beyond the prior 500 ms failure, multi-contact
  full frames with stable IDs and independent add/remove, contact properties,
  cancel and cleanup. API return success alone does not satisfy delivery.
- Prove pen hover/contact/up, position, pressure, rotation/tilt, tool/button
  transitions, cancel and cleanup against native messages/state.
- Complete ScriptModel-to-native Agent integration using the same core. Do not
  claim provider-model reasoning from a scripted integration test.
- Record actual implementation/OS limits individually. Do not call an untested
  or failed item equivalent, and do not substitute sleep, clipboard, extra
  driver/SDK or a second executor to manufacture passing results.

Frozen reference: Sunshine `bb6930bd782ed3edd2eea9a71d46b42183be9358`.
Implementation and acceptance evidence belong in `docs/diagnostics/` with
source/fixture/artifact identities. Installed Apps and system settings remain
unchanged until a separately authorized normal delivery.
