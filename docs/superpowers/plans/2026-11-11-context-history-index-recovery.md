# Context history index recovery implementation plan

> For agentic workers: use superpowers:subagent-driven-development; implement the behavior with production-source RED, then review the exact change and verify it.

**Goal:** A persisted tool-heavy turn can continue after hard-budget summary Protocol failure by carrying an honest source index and reading exact original records with `session_history`.

**Architecture:** Keep the existing soft-budget deferral. For the remaining hard-budget Protocol failure, a capable ordinary main request may create a deterministic index checkpoint. The original DB rows remain unchanged. Preserve a valid old summary and every newly covered user text verbatim, including quoted-preview text and attachment metadata; archive stable assistant/tool payloads with exact IDs, states, source fields and byte counts. Keep the current user and protected tail as original context. This is a partial history index, never a claimed semantic summary or evidence of success. No additional model retries, provider proxy, database migration, persisted Runner or new UI/wire protocol.

**Tech stack:** Existing C++ MaiContextCompactor, MaiTurnRunner, Memory/SQLite stores, session_history tool and normal Core CMake cache. Baseline main `739ba3b887b82e411d2cc8536ff8f8e20e767ef2`; published v2.2.57 remains immutable.

## Contract

- Only failed summary/flush `Protocol` is eligible. Cancellation, network/rate limit, storage, source or checkpoint conflict, invalid input and impossible protected-tail capacity remain explicit failures.
- Use the existing one format-repair attempt. Never use the failed model text or a partly successful batch as the fallback checkpoint.
- Add ordinary `Options.allowHistoryIndexRecovery`, default false, and preparation `usedHistoryIndexRecovery`, default false, with full comments. The caller must guarantee exact authorized same-session history access for the actual outgoing request.
- Runner enables recovery only with a bound sessionAgent and the actual `session_history` spec. Final no-tool requests must disable it before preparation. Introduce an explicit private `includeTools` buildRequest argument; `requestFinalAnswer` currently calls buildRequest with `requireFinalAnswer=false` and clears tools afterwards, so that unrelated boolean is not a capability signal.
- Capability gates both creating and reusing a history-dependent checkpoint. Record a stable host-produced mode marker in the existing reference metadata; do not infer mode from arbitrary model prose. A no-history/default caller or true no-tool finalization must rebuild from full original source rather than reuse an index it cannot query. Keep the marker through merges that still depend on indexed old content; clear it only after a true full-source semantic rebuild. No new database column is needed.
- Reuse valid old checkpoint arrays unchanged; append unambiguous host-produced source-index records. Include source message/part identity, role, tool/callId/state, field and original byte size. Do not label error tools as successful, omit all evidence silently, cut user text to fit, or infer unknown screenshot-to-tool ownership.
- Exact references retain complete source identities and existing links/paths. The shown reference list may remain bounded with `has_more`; the model is explicitly told to read DB before relying on omitted output and to avoid rerunning tools merely to recover their output. Old user attachment paths/MIME and quote metadata remain represented; the external file body is not copied into the DB.
- Keep the current user and protected recent/active tool tail intact. Do not archive Pending/Running parts or change part ordering/ownership. Existing image-group source framing remains valid.
- Both candidates keep the strict six-array schema. Generated semantic summaries retain the existing summaryLimit; the deterministic host index is not model output and instead has a scaled inputBudget ceiling. Its complete assembled request (all guards, references, tools and protected tail included) must still pass the scaled hard budget and strict shrink. Never truncate user text to meet a semantic-output limit that does not apply to an index. Cancel, full source/checkpoint identity and CAS guards apply before committing; impossible required material leaves history/checkpoint unchanged and returns explicit failure.
- Remain inside the same prepare/Runner. No loop restart, input re-enqueue, callId replacement or replay of completed tools. Existing Busy state persists through recovery; do not send SessionError/Idle for a successfully recovered turn.

## Task 1: Behavior RED on the original production source

**Owned paths:** `MaiAgent/tests/MaiContextCompactorTests.cpp`; preserve baseline production bytes in `.cache/context-history-recovery-20261111/`.

- Add an old-API end-to-end fixture with the real Runner, owned store, registered history tool and a counted action tool. One action result crosses the hard budget, both normal summary and format repair return invalid data. Expected: the same turn can continue, read the original result from DB, reach a final answer and execute the original action exactly once. The original production must compile and fail that behavior, not fail because the new API field does not yet exist.
- Add preserving-old-checkpoint plus uncovered-user-constraints, true source changes, missing history capability, final no-tool path and protected-tail-overflow cases. Include install-index then reload with default/no-history options and real final no-tool preparation, not only no-history initial creation. Keep original data/call IDs and DB contents as explicit assertions.
- Add a real capacity RED where preserved user text plus metadata exceeds semantic summaryLimit but the actual complete indexed request remains below hard inputBudget and strictly smaller than original. Verify exact user text, original DB fingerprint and current tail remain intact; keep the truly-over-hard negative case.
- Preserve the original failing executable, source hashes, command, PID, exit and stdout/stderr. Existing negative assertions are not relaxed merely to make the suite green.

## Task 2: Implement the smallest ordinary Core fallback

**Owned production paths:** `MaiAgent/Agent/MaiContextCompactor.cpp`, `MaiAgent/include/MaiContextCompactor.h`, `MaiAgent/Agent/MaiTurnRunner.cpp`, `MaiAgent/include/MaiTurnRunner.h`.

- Build a deterministic index from the same immutable source fields/boundary and valid old checkpoint, not failed response text. Preserve full newly covered user text/quote metadata and exact tool states. Bound metadata and decline the fallback if preserving required content would exceed the budget.
- Reuse the source-prefix plus projected-source/checkpoint checks and normal atomic checkpoint advance. A successful index advance resets the prior soft-deferral flag like any successful checkpoint.
- Wire explicit history capability and no-tool finalization in Runner. Keep ordinary non-model use available with opt-in; no compulsory tool-protocol call from compactor.
- Run clang-format only on the five owned paths. Do not change other production, CMake/version, SDK, installed processes, public release or user data.

## Task 3: Review and verify

- Re-run the same fixture and full Compactor target. Verify an independently constructed ordinary preparation can reload the saved index and recover exact original text without re-execution.
- Confirm no-history and final no-tool requests neither install nor reuse a history-dependent index; cancellation/storage/source/CAS guards and actual hard capacity remain enforced. Test busy/idle/error events on the real Runner.
- Obtain independent spec and code review, then build and run the existing necessary 13 Core groups in the normal cache. Do not rerun unrelated Host/mobile/UI groups without a concrete change or required gate.
- Record reproduction, source/working bytes, actual executable/PIDs/commands, results and limits in `docs/diagnostics/maichat-context-history-index-recovery-20261111.md`.
- Root owns unified main commit/push and any later package/source handoff. No new release version is selected by the implementer; v2.2.57 assets/tag stay unchanged.
