# Sunshine input parity implementation plan

> Superseded by the user on 2026-11-17. The libvirtualhid/ViGEmClient dependency approach in this historical document is rejected and must not be executed. Both added SDK/Git integrations have been removed. See `docs/superpowers/plans/2026-11-16-native-input-dependency-removal.md`; ordinary input uses one system-API native core, and virtual devices are deferred. Any new dependency requires explicit user confirmation.


> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Check items only after the stated evidence exists.

**Goal:** Match the actual input functionality of fixed Sunshine `bb6930bd` in ordinary C++ and `desktop_computer`, then preserve MaiChat's composable operations on that foundation.

**Architecture:** Retain current target observations and ownership. Add platform-neutral event types, a private C++23 pinned MIT libvirtualhid adapter, a common input session and a strict model codec; native capability and validation status stay separate.

**Tech Stack:** C++17 application API, private C++23 input-library adapter, Windows User32/virtual HID backends, existing Qt test runtime gate.

---

## Task 1: Freeze upstream and the parity ledger

- [x] Resolve Sunshine `bb6930bd` and its actual dependency gitlinks from official source.
- [x] Inspect current ordinary and model input contracts against server behavior.
- [ ] Save every server capability, policy, lifecycle and feedback row with its source, unit, current implementation and native acceptance status in `docs/diagnostics/2026-11-16-sunshine-input-parity.md`.
- [ ] Distinguish unused protocol declarations and platform limitations from actual server behavior.

## Task 2: Build the pinned input dependency

**Files:** `MaiChat/desktop/third_party/libvirtualhid`, `MaiChat/desktop/cmake/MaiChatVirtualHid.cmake`, main desktop CMake.

- [ ] Fetch exactly `2234c0079a6a7b0e2f25990d3c705be85bcd37fd` and required pinned source dependencies, preserving licenses.
- [ ] Add a private source-built target with examples, installers and tests disabled; no configure/build downloads.
- [ ] Keep C++23 requirements local to the adapter; do not expose dependency headers in C++17 public types.
- [ ] Compile the library with the repository MSVC toolchain and run a non-injecting runtime capability probe.
- [ ] Record missing-driver/license errors honestly; do not install a system component as part of the build.

## Task 3: Types and event validation

**Files:** `DesktopComputerInputTypes.h/.cpp`, focused `DesktopComputerInputEventsTest.cpp`.

- [ ] Add failing tests for signed relative deltas, sub-detent wheel values, raw code width, touch pointer IDs/unknown angles, controller endpoints, non-finite sensor samples and invalid enum values.
- [ ] Implement one platform-neutral typed event contract and validator used by ordinary and model entry points.
- [ ] Define owned resources separately from currently pressed controls. Test a neutral attached controller remains owned until detached.
- [ ] Define feedback sequences, device generation and explicit queue overflow reporting.

## Task 4: Native adapter and common session

**Files:** `DesktopComputerHid.h/.cpp`, `DesktopComputerInputSession.h/.cpp`, platform factory integration.

- [ ] Add recording-adapter RED cases for no-warp relative/wheel/button events, cross-entry holds, touch/pen state, all controller fields and every feedback kind.
- [ ] Translate typed inputs through the library's real device APIs, including seven profiles and profile capability checks.
- [ ] Serialize per-session input and retain device/native bindings across calls. Capture must leave holds intact.
- [ ] Add repeat/remap policy, device rebind and configuration behavior from the fixed ledger rather than treating packet names as the whole feature list.
- [ ] Check cancellation before event dispatch; return partial count/error and release all owned resources after failures.
- [ ] Test callback lifetime, output queue overflow, late feedback after detach/rebind, all-resource cleanup and watchdog/turn cancellation.

## Task 5: Ordinary controller and model entry

**Files:** `DesktopComputerInputProtocol.h/.cpp`, `DesktopComputerTools.h/.cpp`, controller and ownership tests.

- [ ] Write a test that submits the new capability/event/feedback actions through the registered tool and observe the current unsupported-action failure.
- [ ] Add `input_capabilities`, `input_events` and feedback parsing/serialization; existing actions retain their contracts.
- [ ] Route model and direct ordinary event submissions through the same session. Do not put native dispatch in JSON parsing.
- [ ] Validate snapshot/target/workspace/turn once before a batch, consume before attempted dispatch and never re-dispatch because subsequent capture fails.
- [ ] Keep cleanup available without a live token; reject cross-owner/generation access and preserve existing native-window focus behavior.
- [ ] Test every event family from both entries, malformed inputs and release after expired tokens.

## Task 6: Target-platform acceptance

**Files:** owned native receiver fixtures and diagnostic receipts.

- [ ] Build and run the affected existing controller/ownership/input suites through the Windows runtime gate.
- [ ] Inspect actual receiver events for relative motion, fine horizontal/vertical wheel, raw key flags/repeat, native touch frames and pen hover/contact/cancel.
- [ ] Inspect controller device identity/state and reverse output on an actually installed supported provider. A missing provider leaves this item open.
- [ ] Exercise mixed input/capture and cleanup without a fixed business recipe, preserving held input while observing.
- [ ] Match source revision, staged dependencies and actual test process. Do not replace the user's running Windows App without authorization.
- [ ] Obtain independent delta review, resolve concrete findings, and record every remaining platform/hardware boundary before claiming parity.
