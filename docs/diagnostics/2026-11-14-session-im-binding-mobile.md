# Session binding across desktop and mobile

Current delivery state: Windows/macOS2.2.60 is published as the latest release;
all eight assets and four downloaded metadata files passed unauthenticated
verification. iOS2.2.4/209 is installed with preserved data and normal
launch/process correspondence verified. Android Java60/0 and the ordinary
arm64 Release2.2.33/275 build, artifact consumption and private signing pass,
with no new Android installation. Desktop history/maintenance gates are closed,
including real SQLite abort, restart and writer-lock recovery.

Base source is
`d2d4ffb33eb338244240831c5cf3fc7fffe9dd0c`; no new release has been published.
The frozen native14-file patch is SHA256
`3eda4fd3a5d92fc60588b39cba909a34431b7227d57f2941107d89e30e7d5318`,
followed by the Apple linkage delta
`7d5c0b7f56a71bf40a3b6fc3cc33c1a6096c03069c154d24968e923eaa1e8980`.
The desktop19-file patch is
`ce73e7e38071f766c610ff2c5ef6b9c7ad901890a9e16dd04aec05cf7e62bf73`.
iOS final cancellation delta is
`4ec1d07c0760c1c3010a4cbb6ebf7b6a92277185424210e033774eb0ce57b6ac`;
its ios-only-overlay full tree is `76360a1d5848a8f3d9d9ae074c0b25c1ca1660d2`.
These are source identities, not installation or public-release claims.

## User operation and expected result

In a friend's private conversation, open the composer attachment panel and choose
the purple sparkles **绑定会话** entry. Select one local Creation Center Session
from the dynamic list. First import both directions of the existing local
persisted conversation as quoted context, without a model request or old-message
reply. After the complete import, new Human messages enter in arrival order and its
exact reply returns as Machine IM. Cancel preserves the draft; unbind removes
automatic forwarding. Ordinary quoted replies remain available. The old
independent **AI 帮我回复** suggestions disappear on every platform.

The PC welcome tool list is unrelated: mobile welcome cards remain unchanged.
The local application must be running and online; this does not promise a
background service after iOS suspension.

## Observed baseline failures

- iOS normal Simulator/UIKit, owned GroupChat account/friend fixture, private
  chat → attachment panel: 1 test / 1 assertion failure, 12.733 seconds,
  runner exit 65. The old reply-suggestion tile still exists. No model request
  or actual IM send was invoked. Original image confirms `sparkles`, purple
  RGB(.51,.36,.88), in the first position of the second row.
  Raw log received as `1794586452-94555731.log`. A prior wrong-class selector
  ran zero tests; it is a fixture mistake, not a passing or failing feature test.
- Android original production Java: four attachment-panel unit cases, three
  expected assertion failures (old enum, label and independent suggestion
  bridge/state), one dispatch-disable case passes. Original log/XML retained
  under `.cache/android-session-im-binding-20261114/legacy-reply-red.*`.
  No App/native build, ADB, real account or model invocation in this check.
- Source tracing found a second ordering gap: media SDK callbacks expose the
  ready payload only after asynchronous download while a later text is ready
  immediately. Queueing only at that stage cannot preserve original live FIFO.
  Desktop has the same boundary in `TimSdkRemoteIMClient::handleIncomingMessage`
  and the media URL handlers. This finding is source evidence; platform
  Both the desktop and shared mobile owned regressions now exercise this order.

## Implementation boundaries

`MaiChat/SessionIM/MaiSessionIMRouter` owns the shared metadata queue, binding
epochs, exact Core receipts and output dispatch authority. Platform adapters
announce a live input before SDK downloads, fill the owned payload when ready,
and abandon failed preparation. The shared Reserved head holds order without a
Swift, Java or Qt routing queue. Immediate no-handle C ABI control permits
revocation and SDK-boundary checks without reentering the backend serial queue.

Regular submission remains ordinary `MaiSendPrompt` on an idle Session. It does
not steer a running local turn. Only final canonical assistant body and verified
`agent_send_media/current_ai_session` results can leave the Session. Private
observations, reasoning, raw tools and arbitrary paths are excluded. SDK dispatch
acknowledgment and network-delivery receipt remain different facts; uncertain
delivery never causes an automatic retry.

The exact in-process fields and file ownership are in
`docs/superpowers/plans/2026-11-14-mobile-session-im-binding.md`.

## Verification so far

- The final shared router passed 13 focused C++17 groups using owned fixtures,
  including eligibility, FIFO/Busy, 32 pending, exact receipts, revocation,
  bounded 512 dedupe, concurrency, changed-binding late imports and pre-download
  admission/readiness FIFO. Normal Apple Simulator consumption also passed13/0.
- The mobile ordinary native runtime passed12/0 against the normal Windows Core
  library and12/0 as a normal Simulator consumer. Cases include exact receipts,
  metadata-first FIFO, preparation expiry, changed bindings/backend generations,
  idempotent refresh, model-input parity, abandoned output handoff, Core bypass
  takeover, late SessionError denial and relative media across workspace moves.
- The native mobile model-tool adapter passed strict arguments, mutation
  approval and pre-dispatch cancellation tests against the normal Windows Core
  library. One earlier test assertion treated `optional<bool>` as its contained
  value; the assertion was corrected to `started == false`. That first result
  is a test mistake, not a product RED. The final strict invalid-argument approval
  and pre-dispatch cancellation assertions subsequently passed. Normal Apple
  binding-tool tests and the real C ABI owned SQLite/admit/receive/takeover
  contract selector also exited0.
- Desktop final Bridge group34/0 includes a genuine held-video late-error RED
  (4 SDK sends instead of3), then the same case GREEN after checking failed
  receipt authority. UI8/0, SDK34/0, new layout case3/0 and unchanged tool22/0
  are retained from their appropriate source cohorts. Windows original images
  were inspected: new sparkles entry, local Session picker, bound label and
  unbind action remain visible at980 logical width.
- A deep private Windows test profile produced a280-character agent DB path
  which failed to open and triggered the existing in-memory fallback. Changing
  only the owned profile path to a212-character DB path made the same persistence
  case pass. This is separate evidence of a preexisting long-path/fallback risk;
  it does not prove normal product immunity and is not fixed in this change.
- Normal iOS Debug compiled the new Native and Swift path. UIKit3/0 exercises
  the old-entry replacement, binding/draft isolation, and real ordinary Native
  plus AppState with owned Fake SDK/local Responses model. An initial AX parent
  identifier hid the unbind test selector; that cohort's1FAIL is retained and
  the identifier fix passed. The final expected-cancellation banner delta was
  followed by a single host test0/11.328s, preserving unaffected UIKit results.
  Twelve Swift files were checked byte-for-byte and through exact previous/new
  private Git trees at each received delta; the real index was unchanged.
- The initial Apple native-test link missed Security for FFmpeg SecureTransport;
  its genuine link failure is retained. Adding Security to the imported
  avformat target's Apple interface fixed normal CMake consumers without changing
  Core or the Xcode App's existing Security linkage.
- Android integration/unit tests, new device installation and final packages are
  still in progress. No new desktop version is published.

## 207 install and subsequent real-device observation

The normal signed iOS2.2.4/207 build was installed over206 with the same bundle
and compatible entitlements: install exit0/12.380s, ordinary launch exit0/.454s,
PID58659 and executable/bundle URL correspondence were read back. Built signed
executable SHA256 is
`e2efd4b5b7bafc740247bddf835e889eba94f31e8d503fdcd85fb96a89ca36b8`.
No device container bytes, database or credentials were read. Its exact trees:
ios `d02e5f5055bb92b97b4a3ffd5b682429ff69434c`, Mobile adapter
`aca0d657356ceff519d3d4e47d9067abc7cd2c23`, shared SessionIM
`e7c07f83d503e1933ad113934f0761b41a549aa7`, Core unchanged `dc3f528c...`.
This was d2d4 plus the frozen source overlay, not a claim that d2d4 contains it.

The user then reported that207's binding list was loaded but an earlier
configuration-preparation error remained visible. An owned normal fixture
reproduced both the competing first-open condition (19 of20 concurrent callers
rejected) and the retained error after successful refresh: actual RED exit65,
9.168s, original log received as `1794591733-36173105.log`. The Swift owner is
fixing shared first-open waiting and distinct transient-load/warning state.
That real-device scene is not closed by the earlier UIKit3/0; a newer normal
build and preserved-data upgrade are required. Publication remains held.

## Installation and publication boundary

iOS206, Android2.2.32/274 and the immutable d2d4-based v2.2.59 desktop
packages are preserved as the previous tested baseline. iOS207 now contains
binding but has the real-device recovery issue above. GitHub draft444409099 remains unpublished with zero
assets. The existing v2.2.59 tag will not move; after source freeze and normal
validation the new complete scope will receive an unused version (planned
v2.2.60). Mac installed-App replacement/restart is not authorized by publishing.

## Local history bootstrap scope and current verification (2026-11-14)

The user subsequently required dynamic Session lists and the complete existing
local friend conversation as prior context at bind time. Historical rows must
not execute model requests. This supersedes the earlier live-only behavior;
207 and the old v2.2.59 packages remain earlier baselines, not delivery of this
new requirement. The shared contract is
`docs/superpowers/plans/2026-11-14-session-im-history-bootstrap.md`.

The implementation now uses one owned read-only WAL snapshot reader for the
three fixed storage profiles, ordinary Core invisible staging/source replacement,
and one shared coordinator/router. Raw SDK metadata is persisted before media
downloads, with an account/generation-bound fence and transaction receipts.
Actual persisted duplicates stay context-only. Snapshot/live overlap is excluded
only after confirmed new live eligibility. Legacy Desktop/Android provenance is
explicitly `owner_only_legacy`; coverage is `local_persisted`, not cloud history.

Root normal Windows verification: shared reader originally13/0, router15/0,
formatter0, mobile coordinator20/0 including owned SQLite450-row repeated rebind,
abort and backend-restart bounded cleanup. Core lifecycle/weak-worker and purge
tests pass. The existing nine Core regression groups were run: initial Store
and ToolOutputStore migration failures were traced to replaying migration12 on
a test database retaining its columns; an owned minimal duplicate-column RED
and idempotent migration GREEN are retained. InputQueue's two failures were a
stale test helper that counted structured tool observations as human prompts;
explicit observation assertions and the original human FIFO checks now pass.
The affected Store/OutputStore/InputQueue reruns pass. Shutdown-related
Specialist/SubAgent/TurnRunner/InputQueue reruns also pass. Current normal
console end-to-end passes through stdin/stdout, loopback scripted model, durable
history and hard-kill recovery. These are synthetic local checks, not paid-model
or dual-account IM network tests.

Mac ordinary iOS fixtures exercise205 two-way same-ms rows across pages,
raw-before-bind persistence, duplicate replay and pending-period overlap:
206 context records cause zero model calls, followed by exactly one new live
request seeing the history. RAW33 explicitly fails the bootstrap and preserves
normal IM records. Five affected UIKit/host cases pass on the simulator.
The actual Tencent raw receive entry was exercised with owned SDK message
subclasses without SDK login: the ordinary SQLite transaction finishes before
file URL lookup; missing media does not remove its historical metadata row.

An additional real SQLite writer-lock fixture exposed a fence defect: a failed
metadata write finishing before fence capture disappeared into the new baseline.
iOS now retains debt per peer/source identity until that same record commits;
the same RED-to-GREEN unit cohort3/0 is retained. Android's ordinary tracker
already retained that failed future; its corresponding failure-before-capture
and benign duplicate tests pass in the54-case Java cohort. Root received the
Swift revision through exact private tree `dfd72066f36f6c188da57a20a000bb79b6c5147f`,
preserving the real index.

Root separately reproduced two reader failures: iOS nullable/empty legacy peer
rows were omitted, and keyset OR continuation repeatedly scanned earlier rows.
The query now matches the ordinary iOS CASE peer expression/index and adds the
logically redundant timestamp lower bound before the strict timestamp/ID keyset.
The actual reader's owned20k-row SQLite VM trace changed from36,586,155 to465,850
steps; all15 reader groups and mobile20/0 pass. No real database was queried and
no new index/schema was introduced for this correction.

Independent review then identified two additional P2 cases requiring closure
before delivery: terminal history commit could mark ready before sweeping an
expired live preparation (Root actual20 pass/3 new failures), and Android's
extra projection-refresh exhaustion could lose already-issued one-shot tasks.
Both are being corrected with the same affected entry points. Desktop's new
history pipeline and final platform integration are still in progress. No208
installation or new desktop publication is claimed by these intermediate gates.

## Normal 208 installation and subsequent platform gates

The final five product subtrees match Root's exact canonical source:
Core `37e6783a52494fe76fef9df981af92290ca5fa3b`, Mobile adapter
`f4ee11b6bf7571b778b890b36c92b157c8cc3827`, History
`8a23692d013d86d4f45516aa6cad7aad3e48b18d`, SessionIM
`f3d3bff0cd3c12c1bb2e8ea3075df622cc233036`, iOS
`3aa5c00502a3110fc999938259a456e6d06a5cee`.
They are a frozen overlay on d2d4 pending the unified commit, not code already
contained in that base commit. Apple normal consumers verify reader15/0,
Native23/0, the true CABI selector0 and the final two history/raw-SDK UIKit cases0.

The normal signed208 build was installed over207: install0/12.448s,
launch0/.468s, PID58926, and after three seconds the process executable matched
the installed bundle URL. Strict signature/profile and ten upgrade entitlement
comparisons pass. The built signed executable SHA256 is
`706435d891b0ee1e20386b184007c1a6868f645cc07271cab8521e1955846ca4`;
this is not a claim of reading device container executable bytes. No uninstall,
container/DB read or fixture launch arguments were used. The first build's
receipt wrapper KeyError/exit1 is retained separately from its successful
BUILD SUCCEEDED log; a fresh ordinary incremental build confirmed exit0/18.128s.

The Native expiry review finding is closed by identical23-case RED20/3 to
GREEN23/0. Android's one-shot refresh-exhaustion issue is also closed by the
real controller/worker/service fixture using an owned Native transport: initial
four cases fail, an additional retry-list task case fails, then all five pass
and the final59-case cohort has zero failures with unchanged inputs. The ordinary
arm64 Release build exits0/53.146s, with unchanged source inputs. Its existing
local build init script targets arm64 only; no32-bit build is claimed.

Desktop ordinary six-target build and suites pass189 cases with594 source
hashes unchanged. Actual picker/whole-window PNGs were reviewed. The451-row
two-way context bootstrap makes zero model calls; the next live request sees
all451 records. A later focused41-case cohort uses SQLite Core and an ordinary
GUI heartbeat, and covers a metadata failure completed before fence capture.
Independent review found that aborted Desktop imports lacked a purge/restart
schedule; that final narrow correction is still being verified. These owned
tests do not establish a real two-account IM network E2E or the unconfirmed
physical-device local-model empty-answer root cause.

## Final code gate closure before desktop packaging

Desktop maintenance now preserves the same job across transient storage errors,
does actual abort before bounded32-row purge, and uses a Qt timer for retry
without sleeping in its preparation pool. Existing Session-list refresh publishes
all cached local root IDs, including collapsed rows; startup maintenance consumes
that signal before the first ordinary refresh. It adds no Main-thread history
SQL. The owned SQLite REDs retained200 partial rows and23 startup rows; GREEN
cleans both while preserving ordinary Core and original IM rows. A6500ms owned
writer lock verifies retry after a real abort storage error. A final distinct
RED exposed repeated abort returning NotFound for already-aborted state2;
NotFound now clears only the abort ID and still purges that same Session.
The full final Bridge target is44/0 with594 source hashes unchanged. UI10/0,
tool22/0, SDK35/0, App44/0 and DB38/0 earlier production paths remain covered;
Mac's `/var` versus `/private/var` test-only alias assertion is corrected to
canonical file identity and its original same-slot passes3/0. Mac also consumed
the final repeated-abort data row3/0 after its earlier Bridge43/0.

The same storage-failure audit found Native's abort queue was swapped away on
an SQL failure. Its genuine SQLite-lock RED kept200 rows and incorrectly reported
cleanup pending false. The coordinator now retains pending aborts, tries at most
once per ordinary handle with a one-second backoff, and includes that queue in
the existing history_cleanup_pending boolean. Already-aborted NotFound stops
repeated abort but still schedules Session purge. The unchanged test changes
from original23 pass/new1 failure to24/0, with200 rows finally removed and pending
honest. Apple normal Native24/0 and the real CABI selector0 also pass.

Swift and Android classify history_cleanup_failed as local automatic background
maintenance, not a failed live-message submission. Both have an actual RED then
same-case GREEN retaining the real live warning from the same or later projection.
Swift's final iOS tree is `3425d8abf24fc07ab2c4ed978c14503cf80c43af`;
the final Mobile adapter tree is `a16a042d` (full identity is retained in the
source freeze and platform receipts). Core, History and SessionIM trees above
are unchanged. No new public wire field was added.

Android final60 tests pass, normal Release02 exits0/34.758s with unchanged inputs,
and the private2.2.33/275 arm64 signed candidate is1,598,447,163 bytes, SHA256
`8210f71107494afd9bbbfd9050b78b989058de3e0827378a095a16885839803d`.
Only the new Mobile coordinator object required recompilation;11 reused objects
were checked against their original source/commands/hash. Four archives, the
final link,18 defined-symbol groups, three dynamic JNI exports, stripped APK
build ID `68178a06f04069b665a5368cd423b8c70d0901d0`,26-library closure,
seven CPU variants, model/license bytes and original signing certificate all
match. The signed APK was independently rehashed by Root. The earlier signed
candidate and its receipts remain unchanged; neither was installed or published.

All known findings from these scoped reviews are closed in source. This does
not expand the real-network/device verification boundaries above. The next
stage freezes the unified2.2.60 code commit and verifies actual desktop packages;
the final209 install and release publication are recorded separately.

## Published delivery (2026-11-14 local time)

[MaiChat2.2.60](https://github.com/wanhongqing123/MaiChat/releases/tag/v2.2.60)
is public/latest, release ID444789500, published2026-11-13T22:28:19Z.
The immutable code commit is `32a606d307636be06a715ada7f64ae7d0ff49761`;
annotated tag object is `b3648b7a7d53b3670192d82230ab7532c9b4f666`.
This diagnostics follow-up changes documentation only; the released product
trees stay MaiChat `59c0924db35057933d823ecc188bc74a2908371e`, Desktop
`0caac66328fbd79e0ccdf2d638e248e85220e848`, Core
`37e6783a52494fe76fef9df981af92290ca5fa3b`.

Windows normal native-client Release configure/build both exit0. Normal
windeploy and the114-import runtime gate pass before any App launch. Actual ZIP
and extracted NSIS each contain the same81 application files, with six NSIS
support files separate; all three recursive PE closures and five pinned models
pass. Three real copies each pass ordinary and plugin-debug smoke with private
USERPROFILE/AppLog and package-local Qt modules, six independent PIDs in total.
No installer or existing user App was executed or replaced. Source executable
SHA256 is `0c77561479c065eb1623c1487c6011b5b85076e0a925958378854a5b3fa84d31`.

Mac's local staging, read-only mounted DMG and actual ZIP extraction agree on
338 files and37 symlinks. The26-MachO dependency/arm64/strict-signature gates,
five cross-platform models and six independent ordinary/debug package smokes
pass. Per-PID raw AppLogs use each private CFFIXED_USER_HOME; HOME/XDG were not
set and no historical env dump is claimed. Actual driver parameters and sandbox
rules are retained. Subsequent non-App read-open probes are EPERM; the directory
write-open probe is EISDIR and is not called a sandbox-write denial. Twenty-two
commands in the final successful formal cohort exit0. Two earlier private
venv/helper wrapper failures remain separate, without rebuilding frozen payloads.
The signed executable SHA256 is
`48427bf4dd19c12c87ad54a97e144138818cba1b3748b77e368e56eb2b6bc93a`.

Root's draft readiness check passes before publication. The subsequent public
check passes at2026-11-13T22:28:20Z using GET only, without authentication or
cookies: exact eight asset names/bytes/server SHA256/state, tag peeling/source
trees/body/latest ID, four actual metadata downloads and checksum contents,
five cross-platform model identities, and the previous2.2.58 formal body/assets/
tag unchanged. The held2.2.59 tag/draft was not repurposed or populated.

The final iOS209 upgrade over208 succeeds (install0/11.424s, ordinary launch0,
PID59100 and three-second process/bundle correspondence). Normal Release exits0/
108.689s, built signed SHA256
`e4d331ccfab09cafdda4d1a7e8ef7c265fcb45ae38497970eb9143b9b395704b`.
Twenty ordinary Native objects, nine release-marker negatives with Debug
positives, signature/profile and ten compatible upgrade entitlements are checked.
Its d2d4-plus-overlay build matches the five product trees in the released32a
commit; it is not claimed to have been built after changing that checkout HEAD.
No uninstall, container/DB read or credential access occurred.

Delivery limits remain explicit: Mac is adHoc/not notarized and tested on27.2;
some bundled dependencies declare minimum26 despite the App's14.4 declaration.
Windows binaries are not Authenticode signed. Android's verified candidate is
arm64 only and not installed. Real dual-account network binding E2E and the
physical local-model empty-answer cause remain outside these completed checks.
