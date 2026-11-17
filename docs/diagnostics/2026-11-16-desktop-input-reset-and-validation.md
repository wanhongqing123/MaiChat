# Desktop input reset and watchdog validation

> Historical pre-removal candidate evidence. The dual-executor input SDK candidate below was rejected by the user and removed on 2026-11-17; its SDK/driver results are not acceptance for the replacement system-API core.


This is an implementation and targeted-test record, not a claim of complete Sunshine
parity, installed-driver acceptance, or replacement of the user's running App.

## Reproducible behavior

Hold keyboard/mouse controls, attach a controller, set its buttons/sticks/triggers and
touch contacts, then call the ordinary reset or model `reset_inputs`. The controls must
be released/neutral while the same controller, provider route, generation and output
binding remain connected. Later controller input must still succeed. `release_all`,
task completion/cancellation, target loss and idle expiration continue to tear down
owned devices. An old turn cannot reset a replacement turn's devices.

The reference is Sunshine `bb6930bd782ed3edd2eea9a71d46b42183be9358`, whose reset and
termination are separate operations. Its unrelated streaming/pairing implementation is
not copied. MaiChat uses the pinned MIT libvirtualhid and ViGEmClient adapters already
described in the input parity design.

## Initial evidence and implementation

On Windows, from `main` based on `b974999b` with the ongoing input candidate uncommitted:

- A private exact-source ordinary fixture first exercised a compatibility reset that
  called teardown. Native-HID, ViGEm, Router and Common fixtures all failed their
  retained-device assertions. The implemented reset passed all four. The HID fixture
  uses the real SDK's in-memory backend; it does not inject global input.
- The normal model/controller contract had six failures before the action existed:
  schema, cleanup approval behavior, execution and old-turn/retention checks. The same
  assertions pass after adding the action. The action accepts no additional parameters,
  requires neither a new snapshot nor an approval dialog, and uses the ordinary core.
- New Common and owner-watchdog fixtures each failed two assertions when target
  validation synchronously needed a GUI state read. Both held a state/owner mutex
  while asking UIA/AX to service that request. Common now copies its bound target under
  lock and validates outside it; the owner watchdog also validates outside its lock,
  then checks that the entry still points to the same native owner before expiry.
- Two cleanup-error assertions failed because a legacy cleanup failure was discarded
  after the original dispatch error. The original error code/message now includes the
  cleanup failure; it does not report a known empty input state after uncertain release.

The normal Windows build completed, and the gated eight-suite run passed in 2.54 s:
contracts, ownership, input events (487 checks), Common, HID contracts, Router
(121 checks), ViGEm (157 checks), and Windows touch-frame contracts (124 checks).
These are contract/recording results, not controller-driver or touch-receiver acceptance.

Evidence is retained under `.cache/sunshine-parity-20261116/`, including
`reset-controls-*-red.txt`, `reset-controls-*-green.txt`, `reset-model-red.txt`,
`watchdog-common-red.txt`, `watchdog-owner-red.txt`, `legacy-cleanup-red.txt`,
`reset-final-green-build.txt` and `reset-final-green.txt`.

## Independent concurrency review

Independent scheduling review reproduced an entry race in a private source copy:
cleanup could start between the public cleanup gate and its later epoch snapshot.
Moving the snapshot alone rejects the old request but its failure teardown can then
detach a controller retained by reset. The controller's own failure guard reproduced
the same loss without a scheduling hook: reset initially retained the pad, then the
older operation's failure guard detached it. These were independent concrete failures,
not hypothetical test coverage concerns.

Reset now uses the same nonblocking operation gate as ordinary input. Both the
composite session and the entire controller perform operation serialize with reset;
controller validation and failure cleanup remain inside that operation. A busy reset
returns `Busy` without changing state, timers or epochs. After the ongoing operation
finishes, reset neutralizes controls and retains the pad. `release_all` remains the
stronger asynchronous cancellation/teardown operation and does not wait for that gate.
The public epoch snapshot was also moved ahead of the cleanup gate.

The private Common scheduling case and actual Controller case passed after this
change. Normal regression cases now cover the same Busy/no-side-effect/retention
contract. The normal eight-suite rerun passed in 2.21 s; logs are
`reset-serialized-green-build.txt` and `reset-serialized-green.txt`. Comments and the
test callback lifetime received a subsequent small update; the final integrated build
will rerun the affected tests. No driver/native-delivery claim follows from these tests.

## Runtime and data boundaries

The stock `D:/Program Files/MaiChat/maichat.exe`, PID 56404, remains running and was not
replaced. The protected `.gitignore`, MobileAgent adapter source and its test retain
their recorded SHA-256 values. The normal build cache includes that unrelated adapter
WIP and must not be packaged as a clean release candidate.

Native touchscreen multi-contact delivery remains under investigation. A one-variable
control has now received two DOWNs when its automatic refresh is disabled; the initial
304-byte packet is identical to the failed periodic-refresh control. Effective DPI,
source lifetime and other settings match. This narrows the failure but does not yet
validate sustained hold, move, ordinary up, rapid transitions or a production fix.
No test assertion was weakened to turn API acceptance into delivery. ViGEmBus installation did not start because
the OS elevation request was canceled; no driver, license, UAC policy, App installation
or synchronization service was changed by this work.
