# Windows desktop input final verification

Date: 2026-11-17. Input implementation commit: `94f785d7`, based on
`b7001a6d`. This records the implemented Windows system-API operations and their
development-build verification. It does not establish exact Sunshine semantic
parity; the subsequent `2026-11-17-sunshine-maichat-input-comparison.md` lists
known differences. It does not replace the installed App or claim
Mac native acceptance. The reference is Sunshine
`bb6930bd782ed3edd2eea9a71d46b42183be9358`; virtual hardware/HID devices,
virtual keyboard/mouse devices, gamepads and controller feedback were explicitly
excluded by the user.

The ordinary Controller and model tool use one owned native session. Keyboard,
mouse, touch and pen use their corresponding Windows APIs. No input SDK, extra
Git dependency, broker or driver was introduced. New third-party dependencies
require explicit user confirmation under the tracked repository rules.

## Accepted behavior

- Named/raw keyboard input, Unicode, modifier combinations, scan/side binding,
  configurable fractional repeat and first-delay scheduling share the native
  held-key bindings. Unsupported dedicated hardware mappings fail explicitly.
- Mouse absolute/relative movement, five buttons, both fine-wheel axes, taps,
  complete drags and separate down/move/up retain ownership across observation
  and capture. Ordinary and typed mouse entries can release each other's
  button holds. Named-key and raw-key identities remain separate leases;
  one does not automatically release the other's held key identity.
- Touch supports up to ten contacts, stable opaque caller identities, pressure,
  contact area, orientation, full peer frames, sustained holds, independent
  updates/cancel and full-capacity same-batch UP/new-DOWN slot reuse.
- Pen supports hover/contact, position, pressure, tilt/azimuth, eraser selection,
  logical barrel transitions and cleanup. Windows has one barrel bit; hover
  distance, ellipse and shaft twist have no mapping in this adapter.
- Cancellation epochs, failure/retry state, target checks and live snapshots are
  shared by the ordinary/tool session. Background report/repeat work cannot
  adopt an old request into a new cancellation generation or reenter its writer
  synchronously. Cleanup errors remain visible for their release epoch.
- Window discovery includes minimized, small, untitled and floating/owned top
  level windows without a silent 200-result cap. Discovery does not grant input
  eligibility or valid geometry for a minimized/cloaked target.

## Final runs

All runs used the normal build cache and `run-tests-windows.ps1`. Root ran GUI
and native-input cases serially against owned, identity-verified fixtures.

| Verification | Result |
|---|---|
| Normal input targets and final fixture rebuild | exit 0 |
| Tools, contracts, ownership, DTO, Common, scripted host, Touch, Pen, native-session suites | 9/9 passed |
| DTO / Common / native-session recording | 577 / 241 / 323 checks, zero failures |
| Touch / Pen components | 319 / 262 checks, zero failures |
| Actual ordinary/typed keyboard and mouse | 71 checks, no failure or skip |
| Actual ordinary/tool pen | no failure or skip; 135 raw records; 1569 ms native hold |
| Actual ordinary/tool multi-touch | 62 checks, no failure or skip; 210 raw records |
| Cross-process native editor, physical keys, Unicode, gestures and UIA readback | passed, 12.73 s |
| ScriptModel Agent through real tool and separate native editor | 3 QtTest results passed, no skip, 2.268 s |
| Whole-display native click/type/Enter/fresh capture/readback | passed, no skip |
| Owned window-discovery matrix | 3002 checks, zero failures, no skip |

Final evidence root:
`.cache/desktop-input-required-parity-20261117/`. It contains build/CTest logs,
`final-system-input/`, `final-pen/`, `final-touch/`, `final-two-process/`,
`final-agent-native-r2/`, `final-display-native-r2/` and `final-window-list/`.
Owned held captures, the completed Agent conversation/editor and the final
display image were visually inspected. The Agent uses a ScriptModel; no paid
provider inference or claim about model visual reasoning is implied.

## Failed controls and fixture corrections

The initial writer, admission, target-cover and cleanup-error regressions remain
recorded in the keyboard-and-pen and native-touch diagnostics. They were fixed
against the same assertions, not reclassified as successful API dispatch.

The broader final run also exposed three fixture assumptions. The native tools
fixture mixed Qt logical points with physical window bounds; its conversion now
uses the owned HWND's client geometry and current snapshot ratios. Repeat-disable
and token-free touch-UP fixtures can meet legitimate background admission Busy;
only the precise no-effect Busy is retried within a bound. Existing stopped-repeat,
caller-lifetime, actual-input and empty-state assertions remain. Other Busy or
Internal errors still fail. A deterministic repeat barrier proves the condition.

The first Agent-native setup skipped before input because showing the parent
panel took focus from its child. The fixture now makes one ordinary activation
request for the PID/HWND-verified child, then independently requires actual
foreground and 200 ms with all controls released. There is no forced focus,
thread-input attachment or setup keyboard/mouse injection. The subsequent run
passed. The first display attempt omitted its opt-in and skipped before input;
the explicitly opted-in run passed. Both skipped controls remain in the logs.

## Actual loaded runtime and source identity

`loaded-runtime.json` records the three actual acceptance PIDs, executable
SHA-256 values and loaded Qt module paths/hashes. All loaded
`bin/platforms/qwindows.dll` with SHA-256
`47bcb160c01ffe4a171c6147b5b8b07063088c9db26756b757d2581f99ebc549`.
Core/Gui/Widgets identities also match across the three processes. This is
loaded-module observation, not an inferred SDK path or import-table claim.

The SDK/build-output plugin has a different whole-file hash, `cae7f6df...`.
`runtime-plugin-identity.json` proves that only three PE/debug timestamps differ:
six changed bytes in total; all functional code/data/resources are identical.
Normalizing only those three timestamps yields the same full-file SHA-256,
`c11717cb76e5797bad23f3471d4092c38fd493c98fce23c9a790f084c8ab72d3`.
The actual Qt source HEAD, tracked patch digest, locked postimages and installed
fingerprint also match. No runtime DLL was replaced to manufacture this result.

`final-source-artifact-manifest.json` records exact source/blob and artifact
hashes. `final-safety-audit.json` verifies no active rejected SDK references,
only the original Qt Gitlinks, the three protected WIP hashes and the unchanged
installed App PID/path. Other task WIP remains preserved. Directory work is
still proposal-only, and mac-maichat tasks/messages remain paused.

These are application-receiver and native-state results on this Windows host.
They do not promise hard real-time repeat, universal acceptance by every
third-party application, or functionality supplied only by the excluded virtual
devices.
