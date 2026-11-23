# Screenshot-driven context checkpoint audit

## Reproduction and expected evidence before changes

The user supplied three screenshots of the MaiChat development conversation's
compaction explanation, showing source_message_count=134, six summary arrays,
exact_references total_count=2540 and about30 inline samples. Some open questions
reflect older task status. The user asks whether this is current behavior and
whether it was aligned with Codex. At the start of this audit no production change had been selected.

Compare the frozen public3.1.2 source f47b912c with Root's uncommitted main38d25
worktree and the local Codex reference553c764e. Trace checkpoint persistence,
request injection, reference budgeting/retrieval, and covered-source versus raw
tail boundaries. Use only owned test state; ask the Mac owner for targeted
read-only counts/IDs/runtime identity, not user text, keys, tool arguments or
request bodies. Do not infer the installed source or real checkpoint from a
model's screenshot explanation.

Expected findings must distinguish a stored checkpoint from an ordinary chat
message, a bounded reference sample from lost original history, and an old
summary boundary from later raw corrections or unrelated cross-session replies.
The Codex comparison must name its local and remote paths accurately, rather
than treating any shared budget default as full implementation equivalence.

## Verified source facts

- Both public f47 and the current Root compactor ask for six arrays: goals,
  constraints, decisions, progress, open_questions, source_notes. This is a
  MaiChat schema, not Codex's required output format.
- The store persists the current checkpoint in context_checkpoints with source
  boundaries/fingerprint, revision, summary and full reference_text. It does not
  delete original messages. The request receives a generated
  earlier_conversation_summary data message, separate from ordinary stored chat.
- session_history currently reads original messages/parts only. It has no mode
  for reading the stored checkpoint or paging its exact reference array. Its
  guidance instead suggests source-history search/list_parts. The full index
  remains stored, but “read the rest of this index by cursor” is not an exposed
  operation. This is a real inspectability/retrieval gap.
- Inline references are selected as whole entries within a token budget,
  prioritizing user links. has_more reports omitted samples.30/2540 is not a
  semantic retention ratio or proof that98.8percent of the original was deleted.
- The valid checkpoint is prefixed to build(checkpoint)'s original uncovered
  tail. Newer corrections after the source boundary should remain in that tail
  until a later incremental checkpoint covers them. An actual missing correction
  requires source-boundary/request evidence; it cannot be proved from an old
  open_questions entry alone.
- The published code retains current/recent original turns. Root's extra layer
  selecting earlier real user messages independently of the lossy summary is
  present only in the unpublished worktree. It prioritizes the first request and
  recent corrections, exposes retention counts/has_more, and remains budgeted.

## Codex comparison checked locally

The previous published alignment includes raw-window90percent trigger and
95percent effective input semantics, with explicit reserves tightening capacity.
The local reference Codex's local compact path also retains selected recent user
text within a20,000approx-token budget alongside a handoff summary, persists
compaction/replacement-history metadata, and emits start/complete events. Codex
also has remote compaction paths; the local behavior is not asserted for every
provider/remote path. Neither design makes semantic summarization lossless.

Local sources: reference/codex/codex-rs/core/src/compact.rs and
codex-rs/prompts/templates/compact/prompt.md. MaiChat sources:
MaiContextCompactor.cpp, MaiContextCheckpoint.h, MaiSqliteStore.cpp,
MaiConversationHistory.h and MaiSessionTools.cpp. Runtime metadata verification
and any further change proposal are recorded separately when available.

## Targeted Mac metadata result

The Mac owner read only the specified session in one transaction. Its actual
running App is still3.1.1, PID438, executable SHA256
`f5144cc2e3f12e2dd72a9b70e3c94d1c719c400014263bc22a7a05c45a5a68cb`,
matching published source af01f9ca. It is not the newer private protection/UI code.

The current persisted checkpoint is revision1, created2026-11-21 22:32:21.507+08,
covering134 complete messages, with no partial/preserved-user boundary. The store
contains210 messages;76 are after that boundary (38 user,38 assistant, no pending
inputs). Three later user messages contain the fixed desktop_process keyword;
keyword presence does not prove correction semantics or actual model consumption.
No request-body log was available, so actual checkpoint use and current included
reference count remain unknown. Do not claim tail messages were omitted solely
because an older summary contains unresolved questions.

The persistent summary arrays have counts5/10/19/15/8/12 for goals/constraints/
decisions/progress/open_questions/source_notes; the persistent reference index
has2540 entries. Several screenshot-reported array counts differ (for example,
decisions10 versus stored19, goals4 versus stored5). Thus the model's claimed
verbatim display is not a verified checkpoint export. With no recorded request
projection, this difference alone cannot identify whether the model omitted
items in its explanation or whether another input-projection difference exists.

Only source IDs/counts/roles/runtime identity were returned; no summary strings,
user text, tool arguments/outputs, private paths or keys were exported. No other
session, UI/source/main mutation, model call or SSH operation was involved.

The accurate alignment verdict is partial: published budget policies were
aligned, while the six-array semantic summary and reference display are MaiChat
designs. The extra original-user protection and compaction UI remain unpublished.
A shared implementation checklist and runtime/source identity must accompany
future alignment claims; earlier broad “already aligned” wording was inaccurate.

## Implementation contract after investigation

The user continues the context-compression improvement task. Fill inspectability
without changing the persisted six-array format or claiming full Codex parity:

1. Add a plain C++ current-session checkpoint reader and Agent facade, plus
   `session_history(mode=checkpoint)`; the existing default history mode remains
   byte-for-byte compatible in meaning. Sections are metadata, summary(default),
   references. No new tool name, arbitrary session argument or filesystem access.
2. Return real revision/creation/source boundaries/known array counts and full
   stored reference count. Bind continuation pages to that revision. A changed or
   removed version fails explicitly; no silent mixture of two checkpoints.
3. Read the exact stored summary/reference strings by existing-style UTF-8-safe
   byte cursors, default8KiB, max64KiB source bytes. Invalid legacy bytes preserve
   exact raw_base64. Fragments are labeled as fragments, not complete JSON/URLs.
   The tool's serialized envelope also has a64KiB hard cap and can shorten the
   source page while reporting actual progress honestly.
4. A stored snapshot is not proof of the last request's input. Do not claim its
   complete reference array was fully inlined. Metadata parsing beyond1MiB
   summary/32MiB references is skipped with unknown count/format flags; raw paging
   still works. No source-body/fingerprint rescan or model call is performed.
5. Inject the actual checkpoint revision and creation time alongside its current
   source bounds, and explain directly how to read the exact archive. Keep the
   summary as historical data, not a new instruction or permission grant.
6. Owned tests first reproduce the old tool's unknown-mode failure, then cover
   persistence/reopen,2540 references, original tail unchanged, revision conflict,
   cross-session rejection, malformed budgets/UTF-8, serialized-byte cap and zero
   model/side-effect execution. No real user DB/body, provider or SSH calls.

The internal revision/cursor is automatic plumbing: no user-facing version chooser
or extra approval step is introduced. The user can simply ask to see the saved
compression content. These changes are not added to the frozen/public3.1.2.


## Owned implementation and integration verification (Windows)

The new plain checkpoint reader, MaiAgent facade and session_history checkpoint
mode are implemented in the uncommitted main38d25 worktree. The original tool
executable first rejected mode=checkpoint (real RED, exit1), retained in
.cache/checkpoint-tool-20261122/old-tool-real-red.log. Reader and tool focused
suites then passed with owned memory/SQLite fixtures. They reconstruct all2540
references, preserve UTF-8/raw bytes and source history, reject foreign selectors
and changed revisions, and enforce the complete serialized64KiB envelope.

The combined normal Core build passed. First integration run found seven
assertion failures in two soft-compaction fixtures: the longer truthful archive
retrieval guard and revision/time metadata moved their previously calibrated
requests above the8192-window hard limit. No production budget was relaxed.
Reduce owned uncovered-round filler300 to285 bytes, and the recovery call's
synthetic argument12900 to12300 bytes. Keep every existing boundary, no-replay,
no-advance, unchanged-request/fingerprint and failure-state assertion. Measured
complete requests are now7635 and7592 tokens respectively, still between the
actual7372 trigger and7782 hard limit. New assertions tie injected revision,
created time and through-message ID to the stored checkpoint; the current raw
user requirement remains exact. The same compactor suite passes0 failures.

Final combined CTest:12/12 PASS,35.23s (Core8 plus reader, checkpoint tool,
app_storage and pure screenshot availability). The ordinary console executable
and model-client fixture were normally rebuilt; owned-loopback console E2E
passed every check. Logs are in .cache/checkpoint-tool-20261122/ under
integration-build.log, integration-tests.log (original failure),
compactor-retest.log, integration-green.log and console-e2e.log.

Storage boundary: the legacy bool store getters do not always distinguish
absence from an unreported backend read failure. The reader preserves reported
backend errors; lastWriteError may itself be a prior write error. This change
does not claim to repair that existing store error contract. Successful reads
ignore stale errors; a bound missing checkpoint fails Busy instead of returning
an empty replacement. No real user database or model request body was read.

This work does not change the semantic six-array summarizer, make compression
lossless, prove what the installed App sent in its previous request, or establish
full Codex parity. Native Mac checkpoint runtime, mobile UI and installation
acceptance are not claimed. Public3.1.2 and the user's existing process remain
unchanged. Desktop consumer rebuild and its result are recorded separately.

Normal Windows maichat and both GUI targets subsequently built successfully in
MaiChat/desktop/.cache/build-qt6-ui-20261109; the new reader, facade, session tool,
compactor and direct UI consumers were recompiled. Normal deployment/PE gate
passed. This is the current dirty38 worktree, not the frozen public build.
Source/artifact hashes and preserved-main/Qt/original-doc checks are in
.cache/checkpoint-tool-20261122/integration-safe.json. No new package, native Mac
checkpoint runtime or installed-App verification is claimed.


Later runtime-only update: after the audit, the Mac owner found PID438 gone and
PID60328 running the published3.1.2 App, executable SHA256
fd356045c18c0ff8d76f380e1cf64c782cbd7da555ca5300a5e7b0e93fd61634
(source f47). No new checkpoint/user DB inspection was performed. Earlier
checkpoint counts belong to the earlier observation; this update does not prove
which checkpoint the new process consumed. The owner did not install or stop
this App; the time/operator of the replacement are unknown. The published f47
still lacks the private reader/classification changes.
