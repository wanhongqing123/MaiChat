# Three-feature candidate review

The user requests one combined check: built-in/Shell tool disclosure, append-only
model messages until compaction, and incremental20-percent compaction. The
shipping candidate is exact main11a4 plus these three feature deltas. Root's
development tree remains main38d25 with unrelated preserved WIP; a Root build
does not establish acceptance of a clean requested-main candidate.

## Scope

1. Tool UI: the existing four-production/two-test classification patch. Exact
   shell/shell_input/legacy process names classify as shell; ssh_exec/curl and
   remaining host tools classify as built-in. Counts stay within their group;
   original status ordering and disclosure behavior remain. Public process-tool
   registration is retained. Prior Windows and Mac candidate checks passed.
2. Context: the five-file image-prefix patch, plus the native-response boundary
   correction described below. Under unchanged configuration, prior source data
   and attachment bytes, every earlier neutral model message must stay identical
   when history is only appended and no checkpoint advances. Explicit settings,
   source edits, asset changes and compaction are separate transitions. Provider
   cache-hit rates are not measured by these regressions.
3. Compaction: per-checkpoint20-percent relative target, gradual tool fields,
   persistent monotone preview budgets, protected body and selected old assistant
   ranges only when tool detail is exhausted. Hard input limits still apply;
   image/record granularity can require a little more reduction. DB originals,
   execution facts, source hashes, reference index and current tail remain.

The composite additionally needs only the shared pure preview DTO/helper from
MaiToolOutput, not the unrelated central output-budget implementation. No MCP,
new library, per-session database split, provider call or operation replay is
introduced.

## Native prefix reproduction and fix

Owned actual Agent/FakeModel requests: response1 proposes two tool calls with no
text, response2 proposes another, response3 finishes, then another user turn.
The old Builder merged later calls into the earlier assistant tool_calls batch;
the exact full-message-prefix assertion failed. A compile typo in the initial
test setup was corrected before retaining the genuine logical RED executable.

Successful native responses without provider continuation now reserve and save
an empty non-display response boundary before their parts. The existing reserved
continuation ID is allocated before text/reasoning IDs, so SQLite ORDER BY id
replay preserves placement. The marker is empty reasoning with no continuation
and created==0, recognized by an ordinary named predicate. Genuine streamed
empty thoughts have real timestamps and cannot split an opaque carrier from
its projections. No fabricated provider state, variant or DB schema is added.
Failed/canceled responses do not receive the marker. Builder seals earlier
batch/body at this boundary; unsigned tool pairs remain compactable using the
existing policy, while signed response groups retain their old atomic guard.

New tests compare role, body, reasoning, images, calls/arguments, continuation,
model/configuration and tool specs across every request, including the next
user turn. Memory and owned SQLite replay/reopen are covered. Existing raw part
count expectations were updated to account for the hidden marker while keeping
execution/effect assertions. Desktop rendering already hides reasoning and
ignores it for visual tool-group boundaries, so the visible group remains intact.
Logs/snapshots: .cache/request-prefix-audit-20261123/.

## Validation and delivery boundaries

Root native correction: tool loop, Runner and Compactor CTest3/3 passed before
the final broad run; full Core13 and normal desktop/UI build are being rerun.
The priority20 main backport preserved force/retryAdvice and the generic start
notification callback. Mac normal App compiled successfully with that backport.
Its first owned --tool-first run produced two real failed assertions: a stale
fixed-quarter checkpoint-byte threshold and a same-turn after value14414 versus
target14800 with an overly tight200-token lower tolerance. Neither is declared
passing. Owned-only image/budget diagnostics are pending to establish whether
the latter is the irreducible image quantum under the no-resurrection rule.

Native-only overlay and a19-file composite are prepared from immutable before/
after snapshots. Native overlay is independently applicable after priority20;
composite freeze waits for the two assertion issues to be resolved with evidence.
No release, installation, running-App replacement, Git index/commit/push or
source-branch integration has been performed by this review.

Important exclusion: Root's older unmerged central dynamic output-budget WIP can
change large previous previews as aggregate budgets move. It is not part of this
three-feature main candidate, and this review must not claim that unrelated WIP
passes the append-only contract. It needs separate reconciliation before merge.


## Subsequent Windows results and main-only corrections

The complete Root Core selection passed13/13 in64.70s after the native boundary
fix. Normal maichat and both GUI targets built and deployment/PE gate passed.
Windows QPA classification tests remote14/0 and local4/0 passed on the rebuilt
candidate. Root personally viewed the new local three-group and remote mixed
failure screenshots; hidden markers did not create visible content or split
the visual tool group. These are owned component records, not user App state.

Mac diagnostics establish the two test-contract issues precisely. First, source
80042B yields checkpoint20488B and whole input27358->7781, satisfying the actual
7782 hard budget; forcing the checkpoint itself below20010.5B would discard more
than required by the clarified policy. Second, same-turn image count2->1, old
saved tool budget638->638, tool pixel1->0, whole input18500->14414. An indivisible
image explains the386-token overshoot beyond14800; prior tool bytes cannot be
restored past their committed budget merely to fill space. Corrected tests keep
the upper target bound and prove prior budgets are all at their ceilings and
hypothetically restoring the exact omitted image would exceed the target. No
production algorithm change or generic tolerance increase was used.

A separate main-only header adaptation error placed the predicate declaration
before its type definition. The isolated forward declaration fixes it without
changing source semantics; exact Main header SHA43d95ee043bb715f6a2197e4bee94dbc8ad2069f7bcff0b9ba20087c0745f1ec.
Old failed compile/test logs remain evidence. The corrected Main test hash is
14a6cb4bd46cfe6d9d409e70593ec35910373fe435450dc875a0581f867f95d3.
Actual Mac recompile and two scoped selectors are still pending at this entry.

The final main19 archive is373039B/SHA256
d38759c6572ccad3748d0e8cef4d237212ab49dd87ff8e6ebdf7e8dcbd696548;
composite.patch166728B/SHA256
1d26f84551ff11f69ead4e499b7c5878bff438954fd2ad19e1095aca3a9e67a8.
Actual forward/reverse/reapply and19-file before/after byte comparisons pass;
these static checks do not substitute for the pending target build/runtime
results. No source integration/commit/release/installation is implied.


## Final reported Mac execution results

After the isolated header declaration fix, normal App compiled/linked exit0 in
28.313s. Both exact corrected owned selectors then executed successfully against
the same new normal Core archive: --tool-first exit0/1.616462s (eight entry-helper
invocations, not assertion count) and --request-prefix-only exit0/0.659010s
(memory and owned SQLite reopen, FakeModel/PrefixTool only). No other main or
production delta was added on Mac. The original two assertion failures,
owned-only printf diagnostic, and original missing-forward declaration build
failure remain historical evidence. Final archive intake will follow separately.

Observed main20 results:18000->14400,18500->14800,900000->719976,
925000->740000, program-only stages with0 summary streams. Same-turn18500->14414
passes the proven indivisible-image case: all earlier tool budgets are at their
committed ceiling and restoring that exact omitted picture cannot fit the target.
The selectors performed no user DB/screen read, real vendor, SSH or real tool
operation; they used ordinary Core calls and owned fake fixtures.

Candidate source19 after images and exact11a4 before blobs were independently
verified by Root (root-artifact-intake-safe.json). The same19 manifest was sent
to the validation owner for cumulative private-source closure. Three-feature
candidate validation is now complete, subject to archiving the final receipts;
there is no installation, publication, commit/push or source-branch integration
claim. Unrelated Root output-budget WIP remains excluded as stated above.


## Final evidence archive intake

Mac archive37372B/SHA256
b1fed3690801c8a88aabc2048a415cbf7ef85e716a7491a7dbdced5230287c6e
was independently verified and extracted into the owned mac-validation folder.
Every evidence-manifest entry matches bytes/SHA; the standalone safe JSON,
composed19-source manifest and composition-input list are byte-identical to the
original Root copies. Final normal App and two selector result JSON/logs confirm
the reported exit0 values. The Native selector's empty stdout is expected; its
exact source/main selector and exit result provide the evidence, not a fabricated
assertion count. Mac classification GUI and image-retention baseline were prior
stage validations with unchanged source hashes; they were not rerun this round.
The archive also retains original failed assertions, owned-only diagnostic diff,
failed native header build and the two explicit corrections. No Mac production
change beyond Root's supplied source/correction artifacts was introduced.

mac-intake-safe.json records the independent archive/source/receipt checks.
Candidate validation and evidence archiving are now closed. No source merge,
commit/push, installation or release was performed. No additional test or probe
is pending for this three-feature review.
