# MaiChat 2.2.54 desktop packages

## Authorized scope

The user requested new packages with the Mac peer. Root owns the common desktop
version and frozen source commit, plus Windows EXE and ZIP. The Mac peer owns
Mac DMG and ZIP. This task produces candidate packages for delivery; it does not
authorize a new public GitHub Release or replacement of installed applications.
Published v2.2.53 assets remain the previous delivery.

Functional input is ce90da981fbfdba153bd0712759aad70daa882aa: the Mac player title
restore fix and its regression tests. In native fullscreen the title restore
action now requests a normal window, while a maximize action following an
already requested exit retains its intended behavior. The Windows title chrome
path is unchanged. The common version source is desktop/CMakeLists.txt, now
2.2.54. There is no font product change in this package scope.

## Procedure and expected result

- [x] Commit/push the common version and freeze one SHA for both hosts.
- [x] Normally configure/build the actual Windows and Mac App at that SHA.
- [x] Windows: create fresh portable and installer stages, check PE and model
  closure, extract the actual ZIP and NSIS payload and compare all App files.
  Run ordinary/debug bare-PATH smoke from the stages and actual extracted App
  payloads; never run the installer against the user's installation.
- [x] Mac: create stage, DMG and ZIP, verify actual version/signature/dependency
  and model closure, compare stage/read-only DMG/ZIP payloads, run ordinary/debug
  sandbox smoke from each actual package form.
- [x] Record local artifact sizes and SHA256 values in per-platform manifests
  and checksums. Verify Windows binary split/reconstruction byte for byte.
- [x] Complete candidate file delivery to IM. Original large attachments fail
  with code 2402 and 80 MiB parts time out; retain both failed attempts.
  The final 16 MiB serial transfers are confirmed by SDK acknowledgements:
  Windows 28/28 files and Mac 29/29 files. Recipient downloads are not observed.
  A public GitHub Release remains a separate pending user choice; none is created.

## Existing evidence and remaining boundaries

The exact Player code was already compiled on Windows with deployment/114 PE
checks and the original title chrome slot (3 PASS). Mac core native cases passed
6/0 before the final fixture added DidExit/capture checks. That latest fixture
initially hit a locked desktop. After unlock, it passed 6/0/0 in 27.796 seconds;
the four supplied before/restored screenshots were reviewed on both hosts.
Four controlled real MainWindow video-entry cases open ordinary 1285x723 windows
within the existing 85 percent bound. The user's initial automatic fullsize case
was not reproduced, and its cause remains unexplained. Package smoke proves
startup and deployment separately from these interaction checks.

Only the common version changes after the functional commit, so unchanged
functional suites need not be repeated. Actual new-version package gates must
still run. Retain Windows unsigned and Mac ad-hoc/not-notarized boundaries and
the Mac execution floor actually tested (27.2 arm64, no 14.4 runtime claim).

The separate Windows Noto Sans SC report is confirmed on installed v2.2.53 with
the Large text preset. Investigation continues independently. No candidate may
claim that font report fixed without its own scene reproduction and verification.

## Candidate result

Both platforms built and packaged frozen source
adfff832723595355a47db88430478cc60637d9e. Each passed six actual-package smoke
children. Windows ZIP/NSIS payload equality and Mac stage/readonly-DMG/ZIP
equality passed. See
[candidate gates](../../diagnostics/maichat-desktop-package-254-20261109.md)
and its adjacent JSON for exact artifacts, hashes and remaining boundaries.
