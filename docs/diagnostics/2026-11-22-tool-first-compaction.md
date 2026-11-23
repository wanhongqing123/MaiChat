# Tool-first compaction and dialogue retention

The user clarified the priority: reduce tool-related output first and preserve
dialogue as much as possible. Only if that is insufficient should earlier
assistant narration be semantically reduced, with user requirements,
corrections, prohibitions and unfinished work prioritized. Do not modify or
delete the per-account agent.db or any original field.

Current evidence: sourceFields in MaiContextCompactor sends complete tool input,
output and error fields to the summarizer in bounded chunks. The Runner's ordinary
tool-result projection does not change these original compactor source fields.
The screenshot's80-96 percent values describe raw byte shares, not measured
vendor tokens, semantic retention or guaranteed cache savings.

Implementation plan:

1. Owned regression: tool-heavy history with unique user/assistant text should
   advance a checkpoint preserving that text without invoking a summary model.
   Verify raw history, trusted failed/unknown execution and middle-of-output
   references remain recoverable; retain a genuine pre-change RED.
2. Add a deterministic first candidate in the existing compactor: six-array
   checkpoint with literal source-tagged body records and compact tool receipts.
   Try decreasing tool preview budgets before reducing body. Preserve tool state,
   independent execution facts, incompleteness and exact source IDs. A changed
   checkpoint marks historical tool projection explicitly; no success is inferred.
3. If a literal candidate cannot fit, summarize complete dialogue fields with
   projected tool receipts, prioritizing user constraints over assistant prose.
   Previously literal records become source fragments for this first semantic
   reduction, rather than an oversized previous-summary blob. Only synthetic
   tool-projection offsets are tagged as projection offsets; raw lookup uses
   message/part IDs and the original tool field names.
4. Fingerprints, original references and failure fallback indexes must continue
   to use the complete raw source. Preserve CAS, cancellation, source-change,
   recent-tail, call/result and image-boundary checks. All candidate and semantic
   requests must fit the same actual complete-request budgets and strictly shrink.
5. Run new priority regressions and the existing compactor/Core suites. Adapt
   obsolete full-tool-payload expectations to raw-store recovery, while keeping
   semantic failure tests pressured by actual dialogue when they need to enter
   the semantic stage. Preserve all unrelated WIP; do not publish/install.

Boundaries: semantic compression cannot guarantee zero information loss.
Verbatim retention applies to valid UTF-8 fields and unchanged stored sources;
invalid legacy bytes remain in original history with exact recovery. Missing or
initially truncated source data is not fabricated. Explicit text-loss options
retain their existing host contract. No live user DB, vendor/paid model, desktop
capture, SSH or real-operation replay is used for validation.


## User refinement: incremental20 percent, not eager minimization

The final clarified policy supersedes the first512/128/0-per-tool prototype:
each trigger aims to reclaim about20 percent of the complete before request,
then stops. For example900000 becomes about720000, measured in the selected
model's token count or explicit estimate, not raw screenshot byte ratios. The
hard input cap can demand a larger reduction if the before request is already
beyond that cap. Protected/irreducible data can make20 percent unattainable;
do not delete current instructions or fabricate a completed result to meet it.

Implementation now retains full unselected tool fields and body records. It
stores per-source/per-field preview budgets in the checkpoint's host-owned
literal records, then continues from those budgets on later triggers. Older
outputs are reduced first, then inputs; errors retain a small diagnostic floor,
and independent execution/task/output identity facts remain intact. Each step
measures the full candidate, including instructions, references, media, summaries
and protected tail. A bounded candidate search fills back this round's retained
bytes up to the target; it never exceeds the budget saved at the previous
checkpoint. Older tool image pixels are omitted one observation at a time only
when needed, keeping source paths; user-image pixels remain protected. The
current uncommitted candidate may restore some just-selected bytes to compensate
for image granularity, but no earlier committed omission is restored.

Only after tool payloads reach their necessary skeleton does semantic reduction
begin. Select a UTF-8-safe prefix/range of older assistant text, retaining all
unselected source text; select older user text only after eligible assistant
text is exhausted. Carry exact source offsets/ranges for raw recovery. Send only
selected body ranges plus prior semantic summary to the summarizer. If its
actual output is shorter than reserved, fill back adjacent original body from
this same uncommitted range to stay close to20 percent. Commit once after all
source/CAS/cancel/budget checks, never once per candidate. Original DB records
and account/session layout are unchanged.

The capability declaration is a host-side plain C++ option. Runner sets it only
when the real request and bound Agent facade expose current-session history
recovery. A host without that capability neither installs nor reuses a checkpoint
whose omitted detail depends on it; its existing semantic path remains available.
This is an execution-capability guard, not a model-controlled preference or a
second product workflow. Unknown capacity still follows existing no-guess rules.

Owned exact-counter measurements already obtained before the final broad run:
18000 ->14400, then18500 ->14800, with0 summary stream calls and nonincreasing
old-tool budgets; scale case900000 ->719963 and925000 ->739995, also0 calls.
These use an explicit deterministic test tokenizer, not vendor usage. A separate
semantic-stage fixture checks selected assistant ranges plus their retained
suffix recover the original body, and exact user text/failed execution survive a
model response that omits them. Source/CAS/cancel/write and SQLite reopen tests
are being completed; broad validation is not yet declared passing.
