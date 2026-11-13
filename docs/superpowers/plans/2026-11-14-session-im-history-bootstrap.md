# Session IM history bootstrap (supersedes live-only binding startup)

The user now requires a dynamic Session picker and **all existing local friend
conversation records as prior context when binding**. Import both directions in
stored chronological order. Historical records never invoke a model or trigger
automatic replies. After import completes, new live inputs use the existing
exact-receipt forwarding path. No remote task-status protocol is added.

The earlier tested live-only implementation, iOS207 and held v2.2.59 packages are
baselines. They do not satisfy this added history requirement. Publication and
the next iOS install remain held until the new combined scope is verified.

## Common ordinary components and ownership

- Core owner: generic staged no-run context-source API in `MaiAgent`, Memory and
  SQLite stores. `MaiContextImport.h` defines spec(sessionId,sourceKey,
  snapshotId,label), record(sourceId,text,originalCreatedAtMillis), progress
  (importId,sessionId,sourceKey,snapshotId,nextSequence,recordCount,committed).
  Ordinary begin/append/commit/abort methods; no JSON/SDK/IM in public Core types.
- Root: `MaiChat/History` shared SQLite snapshot reader, common record formatter,
  shared router bootstrap barrier, mobile native/CABI/JNI integration and build
  wiring. SQLite I/O runs through existing platform preparation workers, never
  the serial Agent request worker or Main.
- Desktop owner: Qt dynamic picker, ingress/persistence fence and ordinary
  snapshot/import adapter. Android owner: Java equivalents plus durable UUID
  Machine row IDs. mac-maichat: Swift equivalents. No second platform router.

## Atomic context-source semantics

Core stages bounded batches invisibly (at most200 records,512KiB per batch,
256KiB per record). It owns fresh monotonic Core IDs in supplied chronological
order, chunk-sequence/digest idempotency and no-overwrite conflict checks.
Records become context-only User text with explicit quoted historical
sender/direction/time/status provenance, never counterfeit generated assistant,
reasoning, tool calls or media-delivery authority.

Commit switches one source's visible snapshot atomically and invalidates the
old context checkpoint. It preserves all ordinary Session messages. Rebinding
replaces that imported source snapshot rather than duplicating it; this avoids
pretending that mutable legacy local/remote message IDs are durable aliases.
Failed/aborted partial snapshots stay invisible. Begin/append may stage while a
local model turn is active; commit waits for idle with the same starting-turn
race guard. No model, title, semantic compaction or queued-input API runs during
import. A distinct `ContextImported` event refreshes history without becoming a
live user receipt/takeover event. Clear/delete clean import ledgers; abandoned
staging cannot reactivate after restart.

The next real prompt follows normal model-window compaction. All imported raw
records remain persisted, but no claim is made that an arbitrary-size history
fits one model request. Existing next-turn full-history materialization remains
a measured scalability boundary; importing itself must stream bounded pages.

## Persistent source and exact membership

Use one owned read-only SQLite connection with `BEGIN` and an initial SELECT to
pin a WAL snapshot, then ascending `(created_at_ms,id)` keyset pages. No UI
window or current-view search cursor, DB copy, hidden rowid watermark, or
created-at cutoff can substitute for this snapshot. Source errors/cancellation/
path replacement/identity change are explicit failures, never successful empty
pages. Empty authoritative EOF is valid. Bound page count/bytes and per-page
deadline; there is no silent total-record limit or truncation.

The shared reader has fixed schema profiles `desktop`, `android`, `ios`:

- iOS filters persisted sdk_app_id+owner_user_id+direct+peer and uses
  ROUND(created_at*1000) consistently.
- Android filters owner_id+peer_id; direct messages are in a separate table from
  groups. Desktop uses the already attached current owner's DB and peer.
- Android/Desktop legacy storage does not persist SDK provenance. The descriptor
  explicitly says `owner_only_legacy`; current SDK/owner/backend are checked as
  runtime authority. Do not fabricate historical per-row SDK or origin. Import
  the same owned local conversation visible to that account, with truthful
  `local_persisted` coverage; never claim full cloud history.

Preserve stored row ID and optional actual remote ID separately. Include text,
sender, direction, stored status/time, safe media name/MIME/size/local-reference
metadata and quote digest/source ID. Missing media remains unavailable metadata;
no download, export, pixel decoding or automatic send. Strip approval tokens,
signatures and control authority. Historical origin is data, not live eligibility.

## Raw SDK ingress fence and overlap

Raw SDK arrival precedes asynchronous lookup/download/Main persistence. A mere
flush of already queued DB writes misses this gap. Each platform must persist a
safe initial metadata row BEFORE downloads and track a trusted ingress sequence.
At bootstrap, capture a persistence fence and await all earlier raw metadata
writes on the ordinary persistence path before opening the snapshot. Ready
callbacks enrich that same row and retain the original live admission ticket;
first-insert eligibility comes from metadata persistence, not the later upsert.
The fence waits on metadata, never network download, and is account/SDK/login
generation bound. Failed media still has a truthful historical metadata row.
After the real metadata DB transaction, pure `persisted_input` control echoes
bridge_id/scope/input_id, `inserted`, `source_row_id`, optional `remote_id`.
An already persisted row (`inserted:false`) is a benign replay, not a failed
preparation: discard its live reservation but retain it in historical context.
If a page matches an admitted raw input whose DB eligibility is not known yet,
`history_chunk` returns `page_accepted:false,reason:"awaiting_ingress_metadata"`
before staging or advancing sequence. Retry that same owned page after metadata
confirmation. Only confirmed new live inputs are excluded from context. Source
read repeats its one outstanding page until Native append acknowledges it.

Native first creates a provisional binding epoch marked `history_pending` and
publishes it, then requests the host fence/snapshot. Live metadata is admitted
immediately under that epoch; the same shared router blocks reservation while
history is pending. Snapshot rows whose actual source/remote ID matches an
admitted live input are excluded from the context snapshot and later become
exactly one live prompt. Because admission precedes persistence and a fixed
snapshot, this closes the raw-before/ready-after gap without timestamp guesses.

The existing32 RAW in-flight pending limit stays bounded (including unknown
metadata eligibility). A33rd raw ticket is explicit backpressure even if a later
DB lookup might have classified it as a replay. Queue overflow, expiry or a
failed preparation during bootstrap aborts that binding/import explicitly and
reports a local error; it must never silently complete after dropping inputs.
Original IM rows remain in the normal DB. No historical row occupies a live
slot, and no old row is replayed as a model task. The bootstrap itself has a
bounded total deadline and a maximum of two source readers; it does not spawn
an executor. Cancellation/old scope aborts staging and closes the reader.

## Mobile in-process field contract

The existing `activate` additionally registers trusted `history_source`:
`{profile,path,provenance}`. `profile` is the fixed platform schema name,
`path` comes only from the ordinary account DB owner, and `provenance` is
`owner_sdk` (iOS) or `owner_only_legacy` (legacy Android/Desktop). Models cannot
provide or change these fields. Friend eligibility and existing scope rules stay.

`bind` still takes peer_id/session_id and the exact picker scope. Validation
conflict preserves the previous binding. A successful new binding returns a
provisional item with `history_state:"pending"`, `import_id`, imported count,
and a one-shot owned item in `history_tasks`. Same ready binding is idempotent. Native
model-tool bind returns accepted/pending promptly and uses this same path; it
never calls Swift/Java host callbacks recursively to scan history.

All normal projections contain `history_tasks` for trusted one-shot host execution.
Each immutable task carries bridge_id, whole scope, peer_id,session_id,binding_epoch,
task_id and `import_id:""` (the Core import does not exist before snapshot open).
The original whole task NEVER changes during the job. Host publishes the projection, then awaits its ingress/DB
fence, then performs source I/O on existing preparation workers.

New `maiMobileIMHistorySnapshot(const char*)` is owned-JSON C ABI without a raw
backend handle; free via maiMobileAgentFree. It performs source I/O and MUST be
called on preparation workers, not SDK callbacks/Main/Agent serial worker:

- `{action:"open",task:<whole owned history_task>}` returns ok,snapshot_id and
  source descriptor after actual read snapshot is pinned.
- `{action:"read",snapshot_id}` returns ok,snapshot_id,sequence,records,
  has_more,coverage/provenance. Records are immutable DTOs bounded by page budget.
- `{action:"close",snapshot_id}` releases it. Scope invalidation also cancels it;
  late I/O owns its state and never writes another account's result.

Existing regular `op:im_session_binding` adds:

- `history_opened`: original whole task + snapshot_id, after the fence/open succeeds.
  Return normal projection plus `history_progress:{task_id,import_id,snapshot_id,
  next_sequence,record_count,live_overlap_count,committed}`. The newly assigned
  import_id is progress, NOT a replacement task. Always echo the original task
  in subsequent calls. Sequences/opaque identifiers are decimal strings where numeric.
- `history_chunk`: whole task + whole returned page. Native validates exact job,
  snapshot and sequence, formats safe quoted records and stages Core batches.
  has_more=false is the only terminal page; commit may wait for current turn idle.
- `history_failed`: whole task + bounded reason; abort the provisional import.

Normal projections use `history_progress_all` ARRAY for summaries; specific
history event replies use `history_progress` OBJECT. No field changes JSON type.
Each `sessions[]` item also contains the effective trusted `workspace_key`.
An ordinary UI bind must send `expected_workspace_key` from its displayed row;
scope/workspace changes invalidate that selection. The model tool retains its
three action fields and resolves the trusted workspace internally.

After successful commit Native marks the same epoch ready and releases its live
barrier. Failed source/commit never reports ready or writes a ready preference.
Commit uses an owned one-shot authorization lease immediately before Core SQL,
then releases the authority mutex. Cancellation that wins before the lease
aborts staging. A commit already authorized may publish context after a later
revoke, but can never mark that revoked binding ready or dispatch queued inputs.
Core Busy waits for normal idle/pump without making a tool wait for itself.
Dynamic projections expose phase/progress locally, with no extra remote states.
Immediate takeover/unbind/logout revokes pending history/live authority first;
ordinary worker cleanup aborts Core staging and closes snapshots. Never hold an
authority mutex across SQL, host callbacks, joins or file I/O.

The optional projection `history_cleanup_pending` is a boolean (missing means
false). Keep the existing ordinary pump running while it is true, even with no
bindings, until obsolete staged/superseded context copies are removed. Cleanup
uses at most two transactions per cycle, each deleting no more than 200 old
messages, and checks the cycle time budget between calls. It never deletes the
source IM history, current published context, ordinary Session messages or an
active pending import. Cleanup failure reports a local warning and retries with
backoff instead of declaring completion.

Desktop background imports use the copyable `MaiContextImportWorker` value
capability on existing preparation workers. It weakly references Core Runtime;
it must not retain a raw Agent pointer, extend an idle model/host lifetime or do
SQL in a Main callback. Agent shutdown revokes future calls first. A transaction
already in flight may finish, but cannot publish late events after facade close.

An unresolved raw metadata write failure remains a source-identified persistence
debt even if it finishes before fence capture. Only a successful real write of
that same message (or explicit source-generation invalidation) resolves it; an
unrelated success or a newly captured counter baseline cannot prove completeness.

## Dynamic picker and validation

Refresh on ordinary Session create/delete/title/binding changes, preserve the
selected Session ID and scroll position, and disable confirmation if it vanishes.
Pin bridge+whole scope/workspace generation to the displayed list; a stale
selection cannot silently ride a newer controller. Keep draft untouched.

Owned tests must cover >200 rows/both directions/same-time order, real pinned
snapshot with concurrent WAL writes, missing DB/cancel/account change, raw
pre-bind media metadata fence, live overlap and32 overflow, invisible partial
staging/idempotent page retry/source replacement, zero model calls on import,
and one later live request seeing the completed history. Test real SQLite and
ordinary Core/CABI paths; do not query production chat databases.
