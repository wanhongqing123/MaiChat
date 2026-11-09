# macOS player returns to full screen after restoring a window

2026-11-09. User reports macOS only: open a video, the player initially fills the
screen, click the windowed control, and the player returns to full screen.
Expected: one exit/restore operation reaches a stable ordinary window, with no
new native full-screen entry after the exit completes. Playback source, position,
pause, speed, audio and the existing video layer must remain intact.

Diagnosis began at main `0c49588c87a87b6e71c70b7fd3d3020ebe7f9e75`; the published functional
source is `36db08d1efb67a20b5511b56ae66b359378c5f78`, version 2.2.53. The installed
Mac App is also 2.2.53, executable SHA256
`2441cb61c93865d5c0ad882e2f6cefee775d8560fa0ac92d05e186a97f5406f7`, PID 45971 at
inspection. It is not stopped or replaced during diagnosis. Host: macOS 27.2
arm64, Qt 6.12.0. Existing independent diagnostics remain preserved.

Reproduce with an isolated Cocoa player and owned synthetic/local fixture;
sample Qt window flags plus the actual NSWindow full-screen style and native
Will/Did notifications. Cover the initial already-full-sized/full-screen state
and the title restore control, bottom full-screen control and Esc. Keep the
original player assertions. A final transient false Qt flag alone is not a
successful restore: observe the stable state after native completion.

Initial audit: the main entry calls show(), the first decoded video size is
bounded to 85% of available geometry, and the shared full-screen coordinator
was originally tested on Qt 5. Native notifications and Qt 6 synchronization
must be observed before selecting a fix. Private original sources and normal
test-build logs are in `/private/tmp/maichat-mac-player-restore-20261109/`.

## Reproduction and minimal correction

The user confirmed the upper-right maximize/restore control, and currently sees
no corresponding issue on Windows. The original normal-window title-control and
HLG/native-layer cases pass on Qt 6: 4/0/0, 21.110 seconds.

A new test starts the player already maximized or natively full screen, posts one
real NSWindow mouse click to its own title restore button, and watches both Qt
and NSWindow state. The original implementation gives 3/1/0. Maximized startup
restores correctly. Native full-screen startup briefly reaches 640x480, then the
DidExit completion expands it again to 1512x897, with Qt state=WindowMaximized.
There is no second native WillEnter: the perceived return to full screen is a
queued maximization of the newly restored window. The spy confirms one click.

Cause: the Mac title action computed its first queued windowed target as
!isMaximized(). Native full screen and maximize are mutually exclusive, so this
incorrectly requests maximization. The correction consults the current requested
full-screen intent: restore to normal when still full screen, retain an ordinary
maximize click if Esc has already requested exit, and continue coalescing later
clicks. The Mac full-screen title icon/tooltip now also indicates restore. The
bottom full-screen control and Esc continue restoring their original prior state.
No Mac layer, public header, backend or shared CMake changes are required.

A first constant-false candidate passed the two new cases but broke the existing
Esc-then-immediate-maximize assertion at original line 530. That failure is kept;
the original assertion was not weakened or delayed. The intent-aware production
correction passes all four selected cases plus init/cleanup: 6/0/0, 23.611 seconds.
The original HLG layer mismatch remains 0 pixels, with zero extra animations and
the same layer identity, pause position and speed.

## Remaining native visual verification

The final new fixture additionally waits for a real DidExit notification before
its stable observation and supports optional captures of the owned player only.
The normal App, Player test and ui_zoom_test targets build successfully. One
earlier combined build named UiZoomTest incorrectly; the correct existing target
is ui_zoom_test, and the failed command is preserved rather than counted green.

The final capture run encountered a newly locked Mac: native diagnostics report
frontmost=com.apple.loginwindow, active=0. Its two initial cases fail the explicit
foreground guard; the two original native transition cases also fail in that
environment (2/4/0). This does not replace the prior actual Cocoa 6/0 evidence.
The user has been asked to unlock; the latest DidExit/capture fixture must be
rerun in the real desktop. No skip, timeout threshold or product behavior was
changed to bypass the locked screen. The installed App and v2.2.53 release assets
remain unchanged. The two-file delta is supplied for independent review while
that visual check is pending.

The Windows coordinator independently reviewed and applied the two canonical
files with no code blocker. The initial-state test seeds a native full-screen
or maximized player; it establishes the restore defect, not why the ordinary
MainWindow entry initially fills the screen on the user's Mac. That entry still
calls show() and applies the existing 85% size bound. After unlock, verify the
actual IM/video opening entry and record its first Qt/native state separately,
then complete the final DidExit/capture run.

The coordinator reports normal Windows App and Player-test builds exiting zero,
the 114-PE import gate passing, and the unchanged usesSharedFramelessPlayerChrome
case passing 3/0/0. Those results establish the bounded Windows checks, not a
Windows reproduction of the Mac restore sequence. No font production source
is part of this fix.

The user subsequently authorized committing and pushing this repair with the
remaining native visual/entry checks explicitly pending. The commit contains
only the player implementation, its regression test and this diagnostic. It
does not bump a version or change the v2.2.53 tag, packages or installed App.
