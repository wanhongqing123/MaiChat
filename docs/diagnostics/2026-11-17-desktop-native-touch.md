# Windows sustained multi-contact input

Date: 2026-11-17. Source base `b7001a6d` plus this task's keyboard, pen and
touch changes. Acceptance uses the Windows development build. The running
installed App, system drivers and services are unchanged.

## Reproduction and expected result

Use the ordinary Controller and registered `desktop_computer` tool with the
same owner/session/turn. Press two contacts in one input batch, keep both held
for at least 1250 ms while observing and capturing the owned window, move one,
release the other, add and cancel a third, then release the survivor. The
receiver must see stable native identities/source and full peer frames; native
UP and an empty owned state are both necessary.

Start a new ten-contact source. After all ten real DOWN messages arrive, send
UP for caller ID 100 and DOWN for ID 200 in the same batch. The old actual UP
must precede slot reuse, the other nine contacts must remain continuous, and
the replacement must join a complete ten-contact frame. Final release must
produce real UP messages for every remaining contact.

## Root cause evidence

The old cold multi-contact implementation could get successful system API
returns without any contact messages. An older single-contact stream also
showed an actual native UP about 500 ms after its last valid report, before
the caller requested UP. Neither API success nor polling a later cursor was
adequate evidence of a sustained contact.

Independent direct-User32 controls separated this from the project composer,
SDK layout, pressure/area fields, receiving process, Windows manifest and
registration flags. A one-contact positive control and an open-loop normal
source-report cycle delivered stable two-contact input. No target application
acknowledgement, target hook, new SDK, driver or permission change was needed.
The final source-cycle control passed with two actual DOWN messages, 54 raw
records and empty owned state.

The source now owns one contact ledger and one resident writer. It commits a
seed contact, advances through the ordinary 50 ms source report cycle, then
expands contacts using complete peer frames. Hover-to-contact births follow
the same rule. This is input-report scheduling, not a delivery fence or an
arbitrary startup sleep. The entire request is validated before effects;
accepted partial states remain truthful if later work fails.

Cancellation revokes the source epoch before admission. A synchronous guard
rechecks the selected target and every relevant point while a command is
waiting; it is cleared before responding. Background reports retain only
owned source state. Immutable state/fault/physical-position snapshots remain
readable while native work blocks. The parent merges them dynamically and the
existing owner watchdog checks all cached points for window scope.

A cleanup race was found during pure verification: background
cleanup could report an error, destroy the source successfully, then have the
same release command clear that error by cleaning the empty source again.
The cleanup result is now associated with its revocation epoch; a new explicit
release is required to retry/clear it. Failed releases retain their own source
information and never claim known-empty success prematurely.

## Verification

- Initial source-writer regression: 135 checks / 5 failures; new published-state
  and cleanup boundary: 304 / 2. Final component: 304 / 0, also eight consecutive
  private runs. Removing the seed-report transition caused 28 failures.
- Parent recording contracts: actual normal RED 31 / 323, then 323 / 0 after
  routing, dynamic snapshots, full preflight, synchronous target rechecks and
  cleanup were integrated. These recording checks do not claim OS delivery.
- Actual product chain: **62 checks, zero failures, no skip**, PID 58520,
  owned HWND 396816, 211 raw records. Both initial contacts exceeded 1250 ms:
  native lifetimes 1460/1718 ms and observed lifetimes 1406/1719 ms. Native and
  observer clocks are compared within their own domains.
- Real received pressure values 410/819, contact rectangles, orientation,
  screen/client coordinate round trips, stable peers, independent UP/cancel,
  cold ten contacts and full-capacity same-batch replacement all passed.
- The held owned-window capture was visually inspected. It is a neutral native
  receiver surface; it is not acceptance of a third-party slider or puzzle UI.

Final concurrency review found a separate background admission defect: a
custom recording transport could reenter release during a periodic report and
wait on its own writer. The exact owned recording process reached its reentry
marker and timed out at 3500 ms; only that PID/executable was terminated, with
no OS input. Background reports and deferred cleanup now claim the same
admission gate as commands. Release revokes immediately and reports Busy while
the writer completes cleanup; it does not wait on that native work from the
GUI caller. Generation checks bracket report dispatch. Component coverage is
now 318 checks / zero failures, including report/deferred-cleanup reentry,
live state reads, no late contact and the original failure/retry cases. Only
explicit cleanup retries the exact Busy result within a bound; real Internal
errors are still asserted directly and are never retried into a passing test.

All native runs use `run-tests-windows.ps1`, the normal Qt Windows runtime,
a continuous event loop and PID/HWND-verified owned windows. A ScriptModel is
not needed for this fixture; the registered tool runs directly alongside the
ordinary entry, with no provider-model inference. Later whole-Agent scripted
regression is a separate integration check.

Evidence roots:

- `.cache/desktop-system-pointer-20261117/touch-source-writer-tdd/`
- `.cache/desktop-system-pointer-20261117/direct-user32-source-cycle-probe/`
- `.cache/sunshine-parity-20261116/touch-parent-tests-20261117/`
- `.cache/sunshine-parity-20261116/touch-product-native-test/run-1/`

The current capability response reports system-API availability. It does not
claim that an arbitrary application accepted a particular gesture. Virtual
hardware/HID devices and gamepads remain excluded by the user. Directory work
remains a proposal only; unrelated task changes and mac-maichat input remain
untouched.
