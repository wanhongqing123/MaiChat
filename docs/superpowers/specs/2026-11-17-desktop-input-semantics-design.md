# Desktop input parameter and state contract

The user authorized converging the five primitive-level differences identified
in the Sunshine comparison. The 25 model actions remain MaiChat's separate
efficiency interface. Windows system APIs and one owned native execution core
remain the implementation; no new third-party library, driver or Git checkout
is authorized. The existing main-branch and protected-WIP rules apply.

## Selected contract

Literal reproduction of every fixed-backend quirk would regress known cleanup,
eraser, button-only and contact-boundary safety. A compatibility mode would add
a second set of rules. Use one documented primitive contract instead, with
Sunshine's source-key normalization and requested modifier meaning as the
reference, retaining explicit safe binding and native pointer-state handling.

1. A raw key's logical identity is its canonical source code and flags, before
   user remapping. Strip the legacy marker to its low byte; canonicalize the
   specified non-normalized editing codes only without that marker. Keep the
   caller's original DTO representation and the native remap marker. Equivalent
   source encodings share one logical lease, while different source keys that
   remap to the same native key remain distinct leases on one binding.
2. On the first raw DOWN, derive held source modifier families from the existing
   alias ledger. Compute the missing requested modifier set from those families.
   Resolve each temporary modifier through the same remap/native resolver as
   the target. Keep existing Meta synthesis as a documented MaiChat extension.
   Do not introduce another held-modifier map.
3. Freeze the target and selected synthetic modifier native bindings at first
   DOWN. Repeats use that plan; releasing an explicitly held source modifier
   must not cause repeats to resurrect it. Reuse currently held/physical native
   bindings without acquiring or releasing the user's controls. UP uses the
   original binding regardless of later options or layout.
4. Prepare and validate the complete temporary plan before effects. Security
   checks use actual native modifiers in emission order, not a logical mask or
   a future Win key. A mapped ordinary character must not bypass protected-input
   checks. Reject a plan producing Ctrl+Alt+Delete before emitting any part.
5. Pointer positions remain ratios of observed native bounds, with endpoint
   `origin + ratio*(size-1)`. Touch rotation is a screen-plane direction from
   +Y, increasing consistently with the existing pen `(-sin(theta),cos(theta))`
   basis. Windows orientation is `(rotation+90)%360`. This direction metadata
   is independent of pressure, area, clipping and viewport aspect ratio.
   Contact axes remain normalized ellipse diameters; independently project the
   rotated NDC ellipse into the viewport using its hypot bounding box. Do not
   infer a new direction from covariance eigenvectors or copy the upstream
   rectangle approximation.
6. Pen UP ends contact and retains the accepted in-range hover pose; LEAVE
   exits range. Contact-to-hover emits UP. BUTTON_ONLY preserves accepted pose,
   contact and pressure/tilt; logical barrel bits remain OR-combined and eraser
   role changes retain their legal transition. These are explicit safe state
   semantics, not claims of bit-for-bit fixed-backend equivalence.

## Reproducible acceptance

- Prefix/plain aliases and editing aliases: one logical DOWN, one repeat plan,
  cross-encoding UP, no second native DOWN; independent keys sharing a remap
  retain their separate leases.
- Source Alt remapped to A, followed by B requesting Alt: no unintended native
  Alt. Missing Ctrl remapped to A: temporary A surrounds B, and future remap
  changes do not rewrite an already held B repeat plan.
- A non-modifier remapped to Ctrl still handles requested Shift. An original
  modifier remapped to a character does not synthesize its own packet mask.
- Existing named/native/physical modifiers are not released by the call.
  Meta-to-character protected-focus and mapped Ctrl+Alt+Delete plans emit zero
  native events. Existing cancellation and failed-UP retry assertions remain.
- Touch cardinal/45-degree directions, known zero/unknown, non-square viewport,
  negative origin, missing axes and desktop clipping have independently
  calculated expected outputs. Pen transitions have exact contract assertions.
- Root runs the normal Windows build and gated contract suites, followed by the
  owned native keyboard/mouse, pen/touch, cross-process and scripted-Agent cases
  serially. Record exact source/artifact/runtime hashes and installed-App limits.

No installed App replacement, keyboard-layout setting, service, permission,
device or unrelated UI/mobile/directory change is part of this work.
