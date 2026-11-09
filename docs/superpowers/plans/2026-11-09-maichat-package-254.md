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

- [ ] Commit/push the common version and freeze one SHA for both hosts.
- [ ] Normally configure/build the actual Windows and Mac App at that SHA.
- [ ] Windows: create fresh portable and installer stages, check PE and model
  closure, extract the actual ZIP and NSIS payload and compare all App files.
  Run ordinary/debug bare-PATH smoke from the stages and actual extracted App
  payloads; never run the installer against the user's installation.
- [ ] Mac: create stage, DMG and ZIP, verify actual version/signature/dependency
  and model closure, compare stage/read-only DMG/ZIP payloads, run ordinary/debug
  sandbox smoke from each actual package form.
- [ ] Record local artifact sizes and SHA256 values in per-platform manifests
  and checksums. Deliver candidates without creating a tag or public Release.

## Existing evidence and remaining boundaries

The exact Player code was already compiled on Windows with deployment/114 PE
checks and the original title chrome slot (3 PASS). Mac core native cases passed
6/0 before the final fixture added DidExit/capture checks. That latest fixture
hit a locked desktop and remains pending; it is not a passing visual acceptance.
The real MainWindow entry's initial fullsize behavior also remains unexplained.
The Mac peer will recheck the desktop and complete those checks when available.
Package smoke proves startup and deployment, not these window interactions.

Only the common version changes after the functional commit, so unchanged
functional suites need not be repeated. Actual new-version package gates must
still run. Retain Windows unsigned and Mac ad-hoc/not-notarized boundaries and
the Mac execution floor actually tested (27.2 arm64, no 14.4 runtime claim).

The separate Windows Noto Sans SC report is confirmed on installed v2.2.53 with
the Large text preset. Investigation continues independently. No candidate may
claim that font report fixed without its own scene reproduction and verification.
