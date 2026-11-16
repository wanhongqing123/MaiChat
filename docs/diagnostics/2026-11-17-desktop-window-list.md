# Windows desktop window discovery

Date: 2026-11-17. Starting source: `30ac3a82` on `main`.

The user reports that `desktop_computer(action=list)` sometimes omits windows.
The original application/window is no longer recalled. Reproduce the confirmed
filtering defects with owned windows, without claiming the original incident's
exact cause has been identified.

## Reproduction and expected behavior

Create ordinary visible top-level windows owned by the test process: normal,
minimized without activation, a 64 by 32 popup, an untitled window, an owned
dialog/popup and a floating tool window. Enumerate via the ordinary platform
function and production Controller. Every HWND/PID must be discoverable.

Also create 205 titled, non-minimized, offscreen top-level windows and require
every owned identity to be returned. The old native implementation silently
stops at 200 entries. Message-only windows, child controls and genuinely hidden
internal windows remain outside the top-level visible-window inventory.

Enumeration must not restore, activate, move, click or otherwise operate any
window. In particular, a minimized window must remain minimized and the actual
foreground window must not be changed by `list`.

Only owned window metadata is logged. The native enumeration necessarily sees
the desktop's other window metadata, but the fixture never prints or persists
those entries and does not read their controls or application data.

## Source evidence

`DesktopComputerWindows.cpp::maiDesktopWindows` currently excludes iconic,
untitled and smaller-than-80-by-50 windows. It silently returns a partial vector
after 200 accepted entries and ignores the result of `EnumWindows`.

`DesktopComputerController::windows` and the model tool merely project this
vector. They do not apply another filter or fixed-length output truncation.
Failure to query an application executable path already keeps the window and
is not the cause of these confirmed omissions.

## Narrow correction

Keep discovery independent of input eligibility. Include those top-level windows
and add native class/owner/tool/minimized/cloaked/geometry metadata. Preserve the
real empty title instead of inventing a title that cannot be selected exactly.
An HWND is usable for untitled-window selection.

Do not expose minimized iconic coordinates as pointer geometry. Such rows have
`bounds_available=false` and zero bounds. Window observation must reject
minimized, cloaked or unavailable geometry before issuing a snapshot; a later
status transition must invalidate the previous observation. The existing
foreground, identity, permission and input checks remain in force.

Remove the native silent 200-entry cutoff and report an actual enumeration
failure as an error. The current model path returns the full enumerated vector;
no new pagination protocol is needed for this correction. Report the returned
count and document the Windows discovery scope.

No new third-party library, driver, system permission, installed-App replacement
or mac-maichat interaction is part of this task. Preserve unrelated WIP and the
previously committed native keyboard/mouse repair.

## Verification

Normal Windows build via the existing cache and deployed-runtime test gate:

- Tests-only baseline: public contracts failed 52 checks; owned native discovery
  failed 828 of 2870 evaluated checks, without a precondition skip. Missing rows
  cannot exercise their subsequent metadata assertions, so baseline and passing
  check totals differ.
- Corrected source: public contracts passed; owned native discovery passed all
  3002 checks, without a skip (PID 50276).
- The fixture created 216 exact owned HWNDs. All 213 expected visible top-level
  identities were found by ordinary enumeration and production Controller,
  including all 205 offscreen test windows. The three intentionally hidden,
  child and message-only identities remained excluded.
- Foreground and minimization state remained unchanged, with zero activation
  and zero injected input. No foreign-window metadata or screenshots were saved.
- Model contracts cover returned count and state/class/owner metadata, genuine
  empty titles selected by ID, refusal to issue snapshots for unavailable
  geometry/state, state changes while observing, and old-token rejection with
  cleanup restricted to the caller's held inputs.

Evidence: `.cache/desktop-window-list-20261117/{contracts-red.txt,
contracts-green.txt,native-red/,native-green/}`. The root-owned native receiver
and public-contract sources are recorded in the source/artifact manifest beside
these logs. Independent source review found no additional concrete P1/P2.

The user's original application/state was not available, so this verifies the
confirmed filtering mechanisms rather than claiming reproduction of that exact
historical application. Hidden/tray-only applications and nested UI controls
are not all top-level windows; `observe` supplies a selected window's UIA tree.

This is a source/test correction. It does not replace the installed Windows App,
restore windows, change input permissions or resume the paused mobile/UI work.
