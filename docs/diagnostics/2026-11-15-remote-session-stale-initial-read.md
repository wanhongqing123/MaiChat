# Android remote Session accepts a stale initial history response

## User report

Android and Windows use the same account. Android selects and operates a Session
owned by Windows. The supplied paired images show Windows' final report and
completed tools, while Android still displays earlier progress, running tools and
a long-running thinking timer. Android can display the observation images that
the owner's local desktop panel omitted; that desktop rendering issue is separate.

## Decisive ordinary-controller reproduction

Hold the initial remote history read after it captures a running assistant/tool.
Deliver the owner's completed live broadcast first, allowing it to establish the
pending selection. Then release the captured initial history response. Expected:
keep current completed text/tool/control state and add previously unseen older
messages. The old response must not re-open busy state, an old turn or controls.

The test uses the real Android controller worker and its ordinary `SessionReads`
transport seam under Robolectric. Only native initialization is shadowed. No
SDK/model request, user database, account, server or device operation is performed.
Root's unchanged production baseline with the new fixture actually fails:
`busy=true tool=running`, one test/one failure at the completed-broadcast assertion.
The original XML and logs are in `.cache/android-remote-sync-20261115/`.

The normal Windows test command is Gradle `:app:testDebugUnitTest --tests
com.kongshang.maichat.AIAssistantRemoteSessionSyncTest.completedBroadcastSurvivesDelayedInitialHistory`.
The project uses Gradle 7.5.1/AGP 7.4.2 and the existing Android SDK. An isolated,
checksum-verified official portable Temurin 17 runs this test without modifying
installed Java. Earlier working-directory/CRLF guards, missing SDK environment and
JBR21/GraalVM jlink failures were pre-test harness/toolchain failures, not product
RED evidence. The final ordinary baseline reaches the actual controller assertion.

## Localization

An initial suspicion that only `remoteData.busy` was stale was rejected: the UI's
busy query reads `selectedRemote`, which does receive live updates. The concrete
race instead permits a late initial read through `selectedByBroadcast`, then
replaces known message IDs and runtime state with its older snapshot. There is no
subsequent periodic history read that necessarily repairs that rollback.

The real production MessagePages helper separately reproduced a completed tool
being replaced by running state. Its narrow append-only treatment of delayed
initial history passes its original/new plain-JUnit set (12/0). A complete fix
also needs the controller's runtime/control-state admission guard; helper-only
controller evidence still shows `busy=true tool=completed`.

The first candidate repaired late initial history but the reverse ordering exposed
another real failure: a newer completed history read must supersede an earlier
queued running broadcast. Arrival order cannot be used as data version. The
combined consumer uses the existing message completion, part append/text prefix,
tool progression and image-generation information when merging known IDs.

Root then reproduced completed history accepted first followed by older queued
live progress: `busy=true tool=running`, one test/one failure. The second candidate
adds an owner/login/session scoped end frontier only after explicit owner idle
with no later active assistant/tool in that accepted window. Completed steering
segments do not independently end a turn. Existing mailbox envelope IDs reject
duplicate/reversed receipts within the selected owner; view resets clear those
fences. Actual new turns and active continuations remain eligible.

Root independently verified the exact second candidate under ordinary Gradle and
standard JDK17: Controller 15/0, MessagePages 13/0, remote images 21/0 (49/0 total).
A further actual one-test RED showed a delayed same-turn catalog could still
overwrite the completed selected owner's busy state. The catalog now preserves
the accepted runtime for already ended or strictly older initial turns, while
adopting updated title/device/configuration fields without mutating the transport
object. The next independent 51/0 run covered this direction.

The reverse catalog case also failed: after a genuine new live turn, delayed idle
metadata cleared busy state. Catalog idle now retains the known running state
until one coalesced read-only history confirmation. Actual confirmed idle still
ends the turn. Metadata-only catalog updates do not invalidate that confirmation;
owner/login/online/runtime changes and newer live updates do. Strictly older
initial turns cannot replace later known turns when the idle gap was missed.
No server/wire/C-ABI change is involved.

Final source was independently exercised on Windows with checksum-verified
Temurin 17.0.21+9 and normal project Gradle/Robolectric: Controller 21/0,
MessagePages 13/0, remote images 21/0 (55/0 total, 112.487 seconds). The Mac
producer independently ran the same 55/0. Original RED, partial fixes and failed
intermediate regressions remain in the task cache. Final source review found no
further specific P1/P2.

An additional Android instrumentation case uses the real native initialization,
Controller and Panel with only the existing read-only history transport replaced.
It exercises delayed history, delayed mailbox progress and delayed catalog and
checks visible final text, completed tools and absence of the thinking indicator.
It is restricted to an independent test package, without user login or model work.
Visible view/viewport checks and Activity window-focus checks prevent hidden old
text or another App from satisfying screenshot assertions. Device execution is
pending at this source checkpoint.

## Build inputs and isolation

The source checkpoint is based on main `ed95693d` / Desktop code `c3ea0bbb` on
2026-11-15. Java changes were developed on main. A detached build-only checkout
keeps the committed MobileAgentAdapter and excludes the user's original adapter
WIP. Its two files and the original `.gitignore` remain byte-identical. The clean
baseline native Debug build completed both ARM ABIs with exit 0 in 20 minutes
7 seconds. This prerequisite is not the final Java APK or installed acceptance.
The normal deliverable targets the connected arm64 phone, matching prior local
install artifacts; installation on other ABIs is not claimed.

Setup failures were retained separately. Portable standard JDK17 resolved jlink;
process-local build-tool paths and MSYS prefix handling resolved missing tools and
Windows make path conversion. Unchanged dependencies were checked against prior
source/recipes; no App/Core/Adapter objects were transplanted into the clean tree.
The first migrated x264 configuration still had its original cache install prefix
and wrote that build cache; it was preserved and reconfigured with the private
prefix. Tracked user sources and installed App bytes were unchanged by that
preparation. Subsequent normal builds use existing `E:/OpenSource/msys64/usr/bin`.

## Delivery boundary

Desktop screenshot rendering was independently fixed and delivered in immutable
Desktop 2.2.65. This Android investigation remains separate. The next Android
candidate is 281/2.2.39, built from an isolated checkout using the committed
MobileAgentAdapter, excluding the original unverified adapter WIP. The user then
explicitly requested installation to the connected Android phone. The verified
normal package will update that installation retaining data, without uninstall
or clear. At this source checkpoint the phone still reports 280/2.2.38. Local
Robolectric results are not a completed physical two-device deployment; package,
installation and actual UI receipts must be recorded after those actions.

## Actual installation and remaining on-site failure

Normal 281/2.2.39 was subsequently installed retaining data and verified against
the installed APK hash/process. The original remote session still lacked its
owner's completed report, so this consumer repair was not declared the complete
scene fix. Bounded live evidence then established a separate service size-budget
gap. See [the message-budget investigation](2026-11-15-session-sync-message-budget.md)
for actual artifacts, byte counts, server repair, frame-safe pagination and the
remaining deployment/physical-acceptance boundary.
