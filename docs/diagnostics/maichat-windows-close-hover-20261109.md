# Windows main-window close hover

2026-11-09. Baseline main: `c111d319df90e2a14f3c525f10163277f5431ea8`.

## Requested interaction

The user points to the red rectangle on the main window's upper-right close
button and asks to remove that hover appearance. Move the cursor into the close
button: its background and X should retain their idle appearance. Moving away
and clicking to close must continue to work.

## Source and reproduction

The Windows-only `WindowCaptionButton::paintEvent` in MainWindow.cpp draws the
red hover fill and white X. It is not the operating system's caption or a QSS
rule. The click connection separately invokes MainWindow::close.

A private fixture links the normal UI libraries, creates the real MainWindow
with a FakeRemoteIMClient and no organization/settings identity, and moves the
cursor only while its own HWND is foreground. It captures idle/hover/leave and
clicks the owned close button. At DPR 1.5, the original hover caption has the red
fill; hover differs from idle, leave matches idle, and close succeeds. The
original visual requirement check exits 1 (PID 36208).

The initial fixture did not explicitly invalidate the button after synthetic
mouse entry, so its screenshot stayed at idle despite underMouse=true. That
attempt is retained, not counted as a reproduction. The final fixture repaints
the real close widget after each move before taking screen pixels.

## Change and verification

Exclude Close from the hover predicate, remove its unused red hover color and
keep its ordinary dark X on hover. Explicit pressed feedback remains; minimize,
maximize/Snap, button geometry and click connections are unchanged. Other
platforms and the independent video-window close control are outside this
Windows main-window request.

Verification uses before/after owned-window captures, the existing native
caption action/layout cases and a normal App build with deployment/PE checks.
No production color test is added for this reversible appearance change.

Private evidence: `.cache/caption-hover-20261109/`.
The already delivered 2.2.54 candidate at adfff832 does not contain this later
change. No installed user application or distributed package is replaced.

## Result

- Normal `maichat` and `main_window_layout_test` builds exit 0. App deployment
  verifies three binaries, 114 PE imports and Qt plugins.
- The original caption layout, settings-scroll and native actions slots pass:
  5 PASS / 0 FAIL / 0 SKIP, including init/cleanup, in 1.506 seconds. The existing
  font point-size warning is retained in the original test log; no font behavior
  is changed for this appearance request.
- The same private visual fixture exits 0 after normal relinking, PID 40728.
  Hover and leave caption pixels both exactly match idle, and clicking closes
  its owned window. The before/after caption and complete-window PNGs were viewed.

The tested source snapshot and resulting App SHA are recorded in verification.json.
This source change takes effect in a subsequent package; it is not added to the
previously sent frozen candidate or the installed user application.
