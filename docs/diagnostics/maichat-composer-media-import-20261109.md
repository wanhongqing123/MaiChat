# Composer media import blocks later sending

2026-11-09. Source baseline `f968057b4d541917ca7fd5d896e8e6a4a20a727c`.

## Report and expected interaction

Windows creation center stays at “正在导入媒体（1）”. Each Send appends “媒体还在导入，
请稍候再发送” and neither text nor media can proceed. The original file and input
route are not yet provided. The user asks why a local copy can block and whether
this stage contacts Aliyun.

Import itself copies an existing local file into the workspace ComposerMedia
directory; it does not call Aliyun. Sending to a configured model and cloud-drive
actions are separate paths. Local import currently uses the same global Qt pool
as other background jobs, including network operations.

Expected: unrelated background work must not indefinitely queue local media
import. A user can cancel an import, retain typed text and send again. Failure
or timeout settles the count, and a late completion cannot resurrect canceled
media. Successful image/video imports retain their original session ownership.

## Evidence before implementation

The ordinary pasted-image end-to-end slot passes 3/0/0 in 507 ms with a generated
32x24 PNG, temporary workspace and CapturingModel. With an owned semaphore holding
the only global-pool worker, the new slot fails 1 real behavior assertion (2 PASS,
1 FAIL, 0 SKIP): the same small PNG never reaches the composer while the unrelated
worker remains blocked. RAII releases only the owned blocker on every exit.

This proves a mechanism that produces the reported symptom. It does not prove
that global-pool saturation is the affected user's exact environmental trigger.
The original pending counter only settles after the QFutureWatcher completion;
QFile::copy has no timeout and cancellation is checked only after it returns.
FFmpeg thumbnail work starts after this count settles and uses a separate pool.

## Fix scope and verification plan

Use a bounded, named application-owned pool for local composer copies. Copy in
chunks with a per-import cancellation flag and atomic destination commit. Add a
cancel action and a 60-second UI deadline; settlement is idempotent per import.
Blocked OS I/O may return later, but its stale result cannot reattach media or
keep the composer blocked. No thread is forcibly terminated.

Verify the original image/video/session-switch slots, the held-global-pool
regression, cancel/late-result and actual Qt timer recovery. All test data are
owned temporary files and fake model requests. Record normal builds and owned
UI screenshots before claiming delivery. No existing installation or previous
package is replaced by this source investigation.

Private evidence: `.cache/media-import-hang-20261109/`.

## Windows result

Normal App and test builds exit 0, including deployment of three binaries and
the 114-import/plugin gate. The final registered `agent_panel_session_test`
passes: 77 PASS / 0 FAIL / 0 SKIP, QtTest 11.207 seconds, CTest 13.50 seconds.
The original image, video, original-session completion, question/steering and
draft cases are retained. Existing font point-size warnings remain in the logs.

The new tests exercise:

- Media import and model submission while the global pool is held by another
  owned worker. The unrelated worker remains blocked until the test releases it.
- Cancel and actual Qt timer expiry while an owned worker holds the import pool;
  the deadline test restarts the real production timer at 20 ms. Typed text is
  retained and a text-only request is accepted before the held copy can run.
- Late worker completion leaves the new draft unchanged and cannot add media.
- A physically completed copy whose GUI callback is still queued, followed by
  cancel and immediate panel destruction, removes its owned directory and keeps
  the user's source file.

Review found the last cleanup race in the first candidate. That exact fixture
fails with an orphaned directory (5 PASS / 1 FAIL in the focused run). Cleanup
now queues independently of the panel/watcher on cancellation and timeout;
the unchanged race assertion passes in the final 77-case suite. A separate test
compile error from querying the non-Q_OBJECT QFutureWatcher template was fixed
to query QFutureWatcherBase; that setup failure is not counted as behavior RED.

The four owned pending/recovered screenshots were personally viewed. The
pending card exposes “取消导入”; cancellation or timeout removes that card,
preserves the editor text and leaves sending available. Full logs and exact
source/executable hashes are retained in the private evidence directory.

## Mac result

The received Mac evidence identifies source
`631f5d9c5d83910e68d9b9d8ae24b2443016879c`. The three supplied product/test
blobs match that commit. On macOS 27.2 arm64, Qt 6.12.0 with offscreen QPA, the
normal App and panel-test targets build with exit 0 in 27.294 seconds. The focused
run records 9 PASS / 0 FAIL / 0 SKIP in 1199 ms; the registered full panel suite
records 77 PASS / 0 FAIL / 0 SKIP in 5392 ms. CTest reports 5.51 seconds; the
receipt's 5.734 seconds measures the enclosing process. These QtTest totals
include setup and cleanup records.

The four final pending/recovery PNGs were independently viewed. Pending shows
the import count and cancel action with the existing editor text; cancel or
deadline removes the card, retains that text and leaves the send action visible.
These final images come from a private diagnostic probe with only two added
80 ms event-loop waits before the pending/recovered grabs. Its source was compared
with the committed test and its 4 PASS / 0 FAIL / 0 SKIP log was verified; this
repeats two existing cases for visual evidence, not additional functional coverage.
The formal suite's instant grabs remain archived and are not used as proof of
the laid-out pending card. Product and formal-test sources were not changed by
that visual probe.

The received ZIP is 239171 bytes, SHA256
`9206a03a0a6921c052f684c6775ae224150e51a214adf579d3c5d084a2311642`.
All 25 files named by its manifest pass size/SHA256 checks; the 26th ZIP entry is
the manifest itself. The external and archived receipts are byte-identical.
Original logs, instant grabs and the four final probe images are archived under
`.cache/media-import-hang-20261109/mac-proof/`. This verifies isolated Mac widget
fixtures and a normal build, not installed-App or native Cocoa acceptance; no
new Mac package was made or installed. macOS 14.4 runtime compatibility was not
tested.

## Remaining boundaries

The reported original media and input route have not been supplied, so the
affected machine's exact stall trigger remains unconfirmed. The controlled
thread-pool failure is reproduced and fixed; recovery also handles a copy that
returns late. The UI deadline cannot forcibly interrupt a blocked kernel read.
Panel destruction does not join the pool, but application shutdown can still
wait for outstanding OS I/O.

No provider call, OSS upload, existing user database, camera or installed
application is used for these fixtures. The earlier adfff832 2.2.54 packages
do not contain this later source fix, and have not been replaced or relabeled.
