# Desktop Input Semantics Implementation Plan

> **For agentic workers:** Use independent file leases, tests first, and Root's
> single normal-build/native-input slot. The user has authorized this convergence
> following the five-item review; do not create another approval or branch flow.

**Goal:** Implement and verify the single parameter/state contract in the
same-day desktop-input-semantics design.

**Architecture:** Canonical source identities and prepared frozen modifier plans
live in the existing native alias/binding core. Touch direction conversion uses
one geometry definition; existing correct pen transitions are verified directly.

**Tech Stack:** Existing C++17 core, Qt Windows test runtime, Win32 APIs, existing
recording transports; no new dependencies.

## 1. Canonical raw identities

Files: `DesktopComputerInputTypes.cpp/.h`,
`tests/DesktopComputerInputEventsTest.cpp` under `MaiChat/desktop/`.

- [x] Add prefix/plain and editing-alias contract tests without product changes.
- [x] Root normal-gate RED: 679 checks, 17 failures.
- [x] Share canonical-code normalization between identity and remapping, keeping
  the legacy marker on the effective native request.
- [x] Run normal GREEN and actual native-alias recording contracts.

## 2. Source modifier meaning and frozen native plans

Files: `src/MaiChatTools/DesktopComputerWindowsScope.h`,
`tests/DesktopComputerWindowsNativeSessionTest.cpp`.

- [x] Freeze tests covering source/remapped modifier families, native ownership,
  repeat selection, alias release and security plans before product changes.
- [x] Root runs actual normal RED for those assertions.
- [x] Prepare temporary bindings through the raw resolver, derive source state
  from aliases, and cache the selected bindings in the existing alias entry.
- [x] Validate each planned effect in order and secure-attention combinations
  before the first native emission; retain existing physical-state guards.
- [x] Run normal GREEN and independent source/security review.

## 3. Repeat/UP convergence across Common scheduling

Files: `tests/DesktopComputerInputSessionTest.cpp`; production Common changes
only if a reproducer proves a missing rule after canonical identities propagate.

- [x] Verify equivalent source encodings do not restart or duplicate repeat.
- [x] Verify a remap update cannot rewrite held target/synthetic bindings.
- [x] Verify releasing a source modifier does not resurrect it during repeat.
- [x] Keep exact admission-Busy, cancellation, quiet-window and cleanup checks.

## 4. Pointer parameters and existing state rules

Files: `src/MaiChatTools/DesktopComputerWindowsTouch.cpp/.h/Internal.h`,
`tests/DesktopComputerWindowsTouchTest.cpp`,
`tests/DesktopComputerWindowsPenTest.cpp`.

- [x] Run independent cardinal, 45-degree, anisotropic, negative-origin,
  unknown-angle and missing-axis tests against the old parameter mapping.
- [x] Change only the proven direction conversion/numerical defects; preserve
  source report, admission, epoch, cached cleanup and ownership implementation.
- [x] Verify existing Pen UP/hover/leave/button-only/eraser/barrel rules directly;
  do not rewrite correct behavior to match a fixed upstream backend quirk.

## 5. Final integration and evidence

Root owns `DesktopComputerWindows.cpp`, `DesktopComputerInputSession.cpp`,
`CMakeLists.txt`, public schema comments and all ordinary/native runtime runs.

- [x] Apply clang-format only to owned changes and inspect the diff.
- [x] Build the normal targets, then run nine local contract/Agent suites through
  `MaiChat/desktop/scripts/run-tests-windows.ps1` with Windows QPA.
- [x] Run owned keyboard/mouse, pen, touch, cross-process editor, display and
  ScriptModel native integration serially. No paid-provider inference.
- [x] Observe actual loaded Qt plugin identity and record source/artifact hashes.
- [x] Update operation/comparison docs with implemented semantics and remaining
  intentional extensions/OS limits; never replace them with a blanket 1:1 claim.
- [x] Verify protected WIP hashes, partial-stage any shared CMake changes, and
  commit only owned work directly on main. Leave the installed App unchanged.
