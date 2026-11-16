# Windows keyboard scheduling and native pen verification

Date: 2026-11-17. Source base: `b7001a6d`, with the keyboard/pen changes
described here in the working tree. The installed Windows App is not replaced.
This record does not close the separate multi-contact touch implementation.

## Reproduction and expected behavior

- Hold a raw key, allow its configured first delay and repeat period, then
  release it. Actual received key messages must stop; an empty owned state and
  a 600 ms quiet window are both required.
- Use the ordinary Controller and registered model tool with the same owner,
  session and turn. Hold Control, hover a pen, press, observe while still held,
  change position/pressure/tilt, manipulate the logical barrel buttons, and
  release without needing a new observation token. The receiver must see one
  continuous native pointer/source and a contact lifetime above one second.
- Select the eraser directly, release through the ordinary entry, then press
  and cancel through the tool entry. Native UP and empty owned state are both
  required; successful API dispatch alone is insufficient.
- Cover a hovering pen position with another window while leaving the selected
  foreground window unchanged. A nonzero button-only request must be refused
  before child dispatch; terminal cleanup must still work.

## Keyboard evidence and fix

The earlier approximately 230 ms repeat observation was distorted by the owned
Qt receiver's polling event pump. In a same-executable control, a continuous
`QEventLoop` received repeat intervals of 16/31 ms, while a polling pump with
`processEvents` and `msleep(1)` received 219/250 ms. Both retained all 71 native
checks. A separate fresh UIA read measured about 3.81 ms; this evidence does not
justify introducing a UIA cache or another execution route.

The initial repeat deadline previously started after the native DOWN returned.
It now starts at the first dispatch origin. Later periods remain completion
plus period, avoiding catch-up bursts. A blocked native-call regression keeps
the timing distinction observable.

Configuration now preserves Sunshine's fractional default period, `1000/24.9`
ms. Delay accepts 0..2147483647 ms; period accepts positive finite numbers up to
2147483647 ms. Sub-clock-tick periods remain positive after conversion. No new
configuration field or background thread was introduced.

Non-normalized editing codes 0x7a..0x7d canonicalize before user bindings to
0x100..0x103. Unsupported dedicated keys fail instead of becoming F11..F14.
Historical-prefixed and ordinary normalized function keys retain their meaning.
Repeat and UP retain the first native binding.

An independent deterministic admission test also found an old-request race:
release could finish after operation admission but before that request loaded
its cancellation generation. Both ordinary and typed entries now capture the
generation before admission. The same post-CAS release hook failed 6/10 checks
with the old code (both paths emitted a late DOWN) and passed 10/10 with the
two-line correction (both returned Canceled with no native DOWN). This audit
used an exact source copy and recording native transport, with no OS input.
Evidence: `common-admission-edge-audit-20261117/` under the same cache root.

Normal results: DTO/validation 577 checks, Common 237 checks, both zero failures;
public contracts passed. The final actual keyboard/mouse receiver passed all
71 checks, no skip, PID 38696, including five mouse buttons, both fine-wheel
axes, modifier combinations, Unicode and held drag across capture. Final repeat
intervals were 28.24/21.66 ms for a configured 25 ms first delay / 15 ms period;
these are observations, not a real-time guarantee. The owned held-window PNG
was visually inspected.

Evidence: `.cache/sunshine-parity-20261116/repeat-uia-timing/`,
`editing-key-canonicalization/`, and `repeat-config-domain-20261117/`.

## Pen evidence and fix

One project-owned User32 pen source runs create/inject/refresh/destroy on one
resident source thread. Only one synchronous command is admitted. Published
state, fault and cached physical position form an immutable atomic snapshot;
readers do not wait for native dispatch. The caller's cancellation callback is
discarded before the command returns. Background refresh retains only the
source's cancellation epoch.

The initial actual product test received the ordinary pen flow correctly but
failed the eraser check. Setting ERASER alone does not model the Windows tool
switch. The verified sequence is INVERTED in hover, INVERTED|ERASER in contact,
INVERTED on UP and zero on LEAVE. An immediate legal hover before direct eraser
DOWN works without a sleep or target acknowledgement. Tool changes explicitly
end the old contact/range before selecting the new role. Accepted intermediate
states remain published if a later step fails.

Button-only input now hit-tests the child's last accepted physical point. It
cannot trust stale request geometry or foreground identity alone. The normal
regression failed 2 of 271 assertions before the guard and passed 271/271 after
it. Component pure/recording tests passed 262/262; these are distinct from OS
delivery evidence.

A later review found the same boundary on UP carrying a nonzero barrel state:
that request can press a barrel button, so only UP with zero buttons is pure
cleanup. The owner watchdog now also checks the child's cached point for a
window-bound hover; an unchanged foreground window alone cannot prove that an
unrelated no-activate window has not covered it. Display scope keeps its
explicitly broader boundary. These two regressions failed 2/282 checks and
passed 282/282 after the fixes; zero-button UP and cancellation retain their
focus-independent cleanup behavior. The subsequent product native run also
passed, with 134 raw records and 1567 ms native / 1578 ms observed hold
(`pen-product-native-test/run-3/`). The source worker is named at entry through
the project's existing `MaiThread` utility, including cleanup-only calls.

An exact-source interleaving audit subsequently reproduced a cleanup error
being lost within one release request: background cleanup failed its native
UP but destroyed the source, then the arriving command cleaned the empty
source and overwrote that error with success. The fixed writer retains the
cleanup error for its epoch; a later explicit release can retry/clear it.
The same eight-check audit changed from two failures to zero, and the existing
262 component checks also passed against the private candidate. Evidence:
`pen-release-epoch-error-audit-20261117/`; final normal-source verification is
recorded in the final runtime manifest rather than inferred from this audit.

The permanent actual product test passed with zero failures and no skip,
PID 58644, 135 raw native records, 1573 ms native / 1578 ms observed hold. It
covered the ordinary/model shared scope, Control retention, hover/contact,
pressure, tilt/azimuth, position, logical barrel transitions, eraser, token-free
UP/LEAVE, release-all and cancel-all, including actual final native UP messages.
No provider-model inference was used.

User32 exposes one barrel bit: three logical side-button bits are ORed, not
three independent hardware buttons. Tilt/azimuth map to native tilt X/Y; this
adapter does not invent native hover-distance, contact-ellipse or shaft-twist
fields. API-symbol availability is not target-application acknowledgement.

Evidence: `.cache/sunshine-parity-20261116/pen-backend/`,
`pen-button-target-regression-20261117/`, `pen-product-native-test/run-2/` and
the stage/immediate eraser controls in `native-pen-stage-*/`.

## Exact tested artifacts

| Executable | SHA-256 |
|---|---|
| `desktop_computer_pen_native_test.exe` | `3461150dba877b9405169073b68898b27bea2e5d65f24d6c373b0e5202faaf20` |
| `desktop_computer_system_input_native_test.exe` | `b3f52ce20e3c9a155256f816b3ef990c1a1cf2400fe2a59708d2bd48cf9b5c4a` |
| `desktop_computer_input_session_test.exe` | `359bdf3f24b37d97f31379548bb722dc4a06768d2a8393d4910e43af3dea7e3c` |

All normal/OS runs used `run-tests-windows.ps1` and the normal Windows Qt
runtime. Input was restricted to PID/HWND-verified owned fixtures. No SDK,
driver, service, device permission, keyboard layout or installed App change was
made for these tests. Other project WIP and incoming mac-maichat work remain
untouched. Virtual devices and gamepads remain outside the user's required
scope; the directory plan remains document-only.
