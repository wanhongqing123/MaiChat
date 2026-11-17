# Root local WIP reconciliation

2026-11-18, main only. The user authorized completing local source/document
commits and normal pushes. Release tags/assets, installed Apps, services and
drivers were outside this source-reconciliation task.

## Preservation and merge

Root started at `ed13d88c25737f15348a307aa36fc85b85866161`, with 24 regular
modified/untracked files and one existing dirty Qt SDK gitlink. The index was
empty. All 24 files were saved with raw SHA256 and canonical before/after Git
blobs, a complete source ZIP and a replayable patch; private replay reproduced
every postimage. A path-scoped stash also preserves the original 24 files.
Neither the snapshot nor stash was deleted.

Mac finished its Android/iOS/document integration first, reaching
`10fe0c649aac3917d8ab6c3b9ba3450ddcfc55ae` through ordinary pushes. Root then
fast-forwarded to that exact commit. Five older iOS local files were retained
only in the backup: Mac's final DocumentFileCard/navigation implementation is
authoritative. ChatView's private three-way result was byte-identical to the
remote version. The older private-renderer/test variants were not reapplied.

The Android intersection was only MainActivity. Mac's video orientation-toggle
callback and Root's shared Markdown-preview entry are separate hunks; private
three-way merge returned 0 and retained both. The other nine Android artifact
files had no remote changes. Eighteen Root-only files plus the merged activity
were restored. Shared CloudDrive source and the committed Mac iOS tree stayed
unchanged.

## Verification and resulting commits

- Six focused Android JVM suites passed: 36 tests, 0 failures/errors/skips.
  AndroidTest Java compilation exposed two references to the removed cloud
  video-size constant. The fixtures now obtain the native media policy, with
  an explicit availability assertion. The same compile task then passed in
  9 seconds. The already-green 36-case result was retained, not counted twice.
- The original artifact candidate's 42 JVM cases and 7 owned native PDF cases
  remain tied to their exact source locks. They are historical evidence, not
  a claim of a new product UI or physical-device run. The merged source was
  not installed on the user's Android device.
- The two MobileAdapter files passed independent static review. Mac verified
  their exact blobs using the existing arm64 iOS Simulator native consumer:
  `MaiMobileAgentTests --history-display-only`. The same candidate test with
  the old production object failed; the new object passed. Other archive
  members were byte-identical. This covers passive imported-history snapshot,
  remote snapshot, pagination and recreation contracts, not direction/quote/
  media UI consumption. See the separate Apple-native diagnostic.
- Historical input SDK plans are explicitly superseded by the user-directed
  system-API implementation. Earlier Android adapter drafts are marked as
  historical and point to the final CloudDrive integration record. No removed
  third-party dependency or deferred virtual-device capability was restored.

Local receipts, source snapshots, stash identity and merge matrix are retained
under `.cache/wip-source-handoff-20261117-ed13d88c/` and
`.cache/wip-reconcile-20261117-10fe0c64/`. The commit history records the separate
test-policy repair, Android artifact work, MobileAdapter projection and final
documentation/reference-ignore cleanup.

## Existing SDK application state

Qt SDK HEAD and the main gitlink remain
`025bdad181de241e81bf853c8a2d7bf3d19261a9`. Its two-file dirty diff is exactly
the already-committed 2,771-byte
`MaiChat/desktop/qt/patches/windows-packet-surrogate-order.patch`, SHA256
`a66f64b74b19c5b10e2a0f6892451bd8617d05be18429c401174d8ecb97e67de`.
The lock file binds its pre/postimages; the existing builder validates and
applies it idempotently and fingerprints the applied SDK source state.

This working SDK application state is preserved. It is not another unsubmitted
product patch and does not require a nested Git commit. It must not be reset
merely to produce a cosmetically clean submodule status. Local upstream review
repositories remain ignored and are not product dependencies or committed
third-party repositories.
