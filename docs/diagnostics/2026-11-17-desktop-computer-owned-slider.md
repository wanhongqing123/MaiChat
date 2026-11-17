# Owned browser slider input regression

Date/platform: 2026-11-17, Windows native desktop. Source baseline:
`8aefd68b3097fb37a495846d680d25fc45b5e886` plus the two test-file changes and
focused Windows test-runner fix. No input production implementation changes.

The reported third-party site is unknown. This regression therefore verifies
the input mechanism on an owned loopback page in a real private Edge window.
It does not establish acceptance by a third-party CAPTCHA or a live visual
model. No paid model task is used.

## Operation and expected result

Enter through the existing registered `desktop_computer` tool with real JSON
arguments. Observe the display, locate the owned handle/target from PNG pixels,
send a deliberately short atomic drag and confirm rejection. Reload the owned
page, then send independent mouse down/move operations, keep the button held
across observations, make bounded pixel-guided corrections, observe alignment
while still held, and only then send mouse up.

The page must report no untrusted input, one final release, alignment and a
successful result. The test releases its owned inputs, stops only its owned
browser Job, and removes the private profile. User browsers and the installed
MaiChat process are preserved.

Command (through the required runtime/error-mode wrapper):

```powershell
$env:QT_QPA_PLATFORM = 'windows'
$env:MAICHAT_RUN_COMPUTER_DISPLAY_NATIVE = '1'
$env:MAICHAT_COMPUTER_NATIVE_EVIDENCE_DIR = '<private evidence directory>'
& MaiChat/desktop/scripts/run-tests-windows.ps1 `
  -BuildDirectory .cache/build-qt6-ui-20261109 `
  -TestRegex '^desktop_computer_display_native$' `
  -TestFunction @('--browser-slider-workflow')
```

Use the configured source Qt platform-plugin directory, as in the recorded
run. The normal `maichat` build and runtime DLL deployment gate completed
before running the native target.

## Observations

Evidence: `.cache/desktop-computer-owned-browser-slider-20261117/actual-r2/`.
The native receipt has exit code 0 and no failures: seven moves, one down,
one up, zero untrusted events, final aligned/success, owned Job stopped and
private profile removed. The held screenshot shows `held: true`, releases 0,
aligned true; the final screenshot shows held false, releases 1, success.
Both screenshots were visually inspected.

Two test infrastructure failures were distinguished from product failures:

1. The first run rejected a stationary confirmation frame solely because its
   DDA sequence did not increase. It already showed aligned/held with zero
   releases. A fresh observation can legitimately reuse the stationary DDA
   frame; final confirmation now requires a fresh snapshot ID and a nonzero,
   nondecreasing frame sequence. All alignment/held oracles remain in place.
2. The second native run returned 0, but Windows PowerShell treated its normal
   stderr evidence line as a terminating `NativeCommandError` under the test
   wrapper's Stop preference. The focused invocation now temporarily permits
   native stderr and returns the actual process exit code, restoring the
   preference afterward. Real native fixtures prove stderr+0 returns 0,
   stderr+7 returns 7, and a missing executable fails. No process failure is
   suppressed.

The final combined rerun through the fixed wrapper returned 0 in 6.63s.
Receipt: `actual-r3/browser-workflow-3da817b0-a5f8-48bc-b631-41749788ef98/`.
The native result again has no failures, seven moves, one up, zero untrusted
events, success, and confirmed owned Job/profile cleanup. The unrelated
installed Windows App remains PID 56404 at `D:\Program Files\MaiChat\maichat.exe`.
No third-party CAPTCHA success is claimed from this local regression.
