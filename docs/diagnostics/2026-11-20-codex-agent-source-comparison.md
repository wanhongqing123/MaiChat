# Codex agent coordination versus MaiAgent

## Scope and evidence

Requested comparison: `reference/codex/codex-rs/core/src/agent` against the current
MaiAgent implementation. Local Codex Git root is `reference/codex`, frozen at
`553c764e8b99fa519e9c6eb9b657875ef0f5e1c1` (2026-11-13). MaiChat main is
`cdf82acffde9565ae4cec88e4ca37db75d76bbef` during this audit. This is local-source
inspection, not a claim about the latest upstream or all deployed Codex configurations.
No implementation, build, runtime test, user data, credential or model request was used.
The existing qtbase changes are preserved; this report is the only new repository file.
Product MCP integration is excluded from findings and recommendations.

The Codex directory has57 Rust files:45 non-test-named files/8388 lines and12
separate test files/9219 lines. Inline tests may also occur in the former count.
Thus over half of the counted lines already belong to separate tests, and V1,
V2, local and host compatibility paths account for additional volume.

`agent/api.rs:1` explicitly describes coordination of one agent tree. The full
inference/tool loop lives elsewhere: `core/src/session/turn.rs:218` runs a turn,
`:649` samples, and `:1894` implements the sampling request. It is inaccurate to
compare this directory alone with all of MaiAgent, or to equate file count with
reasoning capability. Our main responsibilities are split across
`Agent/MaiAgent.cpp` (1826 lines), `MaiTurnRunner.cpp` (1071), context/store/tool
modules and `AgentTools/MaiSubAgentTools.cpp` (300).

## Existing MaiAgent equivalents

- Ordinary and model-tool subagent operations share core entry points:
  `MaiAgent.cpp:972,1022,1036,1055,1072` and `MaiSubAgentTools.cpp:91`.
- Depth limits, tool-loop/repeated-call protection, per-session execution,
  cancellation and late-turn protection already exist: `MaiAgent.h:235,254`,
  `MaiTurnRunner.cpp:346,684`, `MaiAgent.cpp:569,1661`.
- Approval fails closed and a child's new session does not inherit the parent's
  session waiver: `MaiTurnRunner.cpp:894,904,972`, `MaiAgent.cpp:1013`.
- Provider usage is parsed, although not yet consumed as a shared tree budget:
  `MaiModelClient.h:177`, `MaiResponsesClient.cpp:678`, `MaiTurnRunner.cpp:534`.
- History search/paging and compaction are substantial existing capabilities:
  `MaiConversationHistory.cpp:98`, `MaiTurnRunner.cpp:491`, and checkpoint CAS
  in `MaiSqliteStore.cpp:1092`. Do not classify all history/recovery as absent.
- Provider specialist tasks already have persistent completion delivery:
  `MaiSpecialistTask.h:78`, `MaiAgent.cpp:823,912`. These are not interchangeable
  with a configurable general-purpose explorer/reviewer agent role.

## Priority1: complete the subagent execution contract

### A. Bind results to the requested turn and terminal outcome

`MaiAgent.cpp:1045` waits for active work to disappear, and `:1091` searches
backward for any nonempty assistant text. `MaiSubAgentTools.cpp:156` returns that
text without a turn receipt or terminal error. The runner knows errors and emits
them (`MaiTurnRunner.cpp:198`), but they are not represented in this report.

Static scenario: first turn produces a result; the follow-up turn fails before
producing text; waiting can skip the empty new assistant message and return the
old answer. A nonexistent child also yields false (`MaiAgent.cpp:1040`), which
the tool describes as still working (`MaiSubAgentTools.cpp:151`). These scenarios
need failing harnesses before any implementation; they were not run in this audit.

Codex uses terminal Completed/Errored/Interrupted status (`agent/status.rs:9`),
the turn ID/outcome (`session/mod.rs:2879`) and `agent/control/completion.rs:93`.
Recommended contract: task/session/turn IDs, state, error and exact result source;
distinguish timeout, absence, cancellation, failure and successful completion.

### B. Use the existing inbox for busy-child messaging

`sendToSubAgent` calls `MaiSendPrompt` (`MaiAgent.cpp:1032`); with default
`rejectWhenBusy=true` (`MaiAgent.h:240`), active children return Busy (`:1663`).
The general durable queue already exists (`MaiAgent.cpp:1584,1594,1610`), so this
is an integration gap, not the absence of a queue throughout the system.

Codex V2 differentiates QueueOnly and TriggerTurn (`agent/control/api.rs:154`,
`tools/handlers/multi_agents_v2/send_message.rs:43`, `followup_task.rs:43`). Its
terminal callback can notify a parent mailbox (`agent/control/completion.rs:111`).
Recommended contract: separate accepted message delivery, interrupt/steer and an
idle follow-up; preserve sender/source and never silently turn a message into
human authorization. Reuse the current input core rather than a second queue.

### C. Make capacity and lifecycle state agree

`MaiAgent.h:259` describes a whole-tree `maxOpenSubAgents`, but `MaiAgent.cpp:988`
counts only children whose direct parent matches (`:962`). With max2/depth2,
root2 children plus each child's2 grandchildren permits6 open subagents.
The public API is documented thread-safe (`MaiAgent.h:210`), yet count/create/
put (`MaiAgent.cpp:988-1011`) has no combined spawn reservation. Concurrent direct
API calls admit a source-level interleaving; this has not been runtime reproduced.

The closed set is runtime-only (`MaiAgent.cpp:260,1083`) and is used for list and
capacity (`:1062,990`). SQLite session fields (`MaiSqliteStore.cpp:1643,1682`) do
not persist it. A recreated host can consequently treat old closed children as
idle/open again. Define close, interrupt, reopen and capacity release together.

Codex separates shared active execution from resident children
(`agent/control/runtime.rs:230`), and reserves pending+resident capacity under
one lock (`residency.rs:229`, real spawn at `spawn.rs:693`). V1 stores Closed
(`legacy.rs:50,67`); V2 interrupt has another intentional lifecycle. Do not mix
these generations or claim every Codex execution-admission operation is atomic.

## Priority2: cost control and effective inheritance

### D. Account usage across a delegation tree

Our `MaiTurnRunner.cpp:534-538` uses input usage to correct a context estimate;
it does not accumulate parent+children's response consumption into a task budget.
Context capacity, max output and max tool iterations are different controls.

Codex records real response usage (`session/mod.rs:5816`,
`agent/control/budget.rs:12`), adds configured weighted accounting
(`rollout_budget.rs:48`) and acknowledges reminders after history insertion
(`session/rollout_budget.rs:27`). Its contract is response-after accounting, not
a strict future-token reservation or guaranteed no-overshoot cap
(`agent/host/usage.rs:25`). A shared optional tree budget is useful for our cost
concerns without changing default models, output caps or paid-tool authorization.

### E. Offer optional context and constrained roles

Current spawn only accepts name+prompt and creates a fresh child history
(`MaiSubAgentTools.cpp:69`, `MaiAgent.cpp:998-1018`); child context loads only its
own session (`MaiTurnRunner.cpp:384`). This isolation is valid, but not selectable.
Codex can choose fork all/none (`multi_agents_spec.rs:649`,
`agent/history.rs:203`) and marks inherited records for recovery (`history.rs:258`).
Keep prompt-only available; full-history inheritance can increase cost and must
not copy raw persisted messages without source/authorization semantics.

Our copied model/effort/agent fields are persistent preferences, not a captured
effective invoking step (`MaiAgent.cpp:1003-1006`). An empty effort lets a child
re-read the host default after the parent already captured another value
(`MaiTurnRunner.cpp:248-255`). `session.agent` in this audited core does not select
role-specific prompts/tools. Codex captures effective settings
(`agent/child_config.rs:122`) and allows constrained role overrides
(`agent/role.rs:33-123`), with permissions refreshed from the invoking authority
(`child_config.rs:197`). This is suitable for read-only explorer/reviewer or
lower-cost model choices, while child capabilities only shrink from the parent.

### F. Preserve authorization origin separately

The child receives a parent-generated delegation as an ordinary User message
(`MaiAgent.cpp:1017,1672`); no separate root authorization evidence accompanies it.
Codex's Guardian receives bounded root evidence (`user_authorization.rs:244`,
`guardian/prompt.rs:108,169`), and sender identity comes from trusted delivery
(`sender_context.rs:24,85`). This concerns provenance for complex delegation,
not a claim that our existing human approval is bypassed. Our compaction already
warns against treating tool output as authorization (`MaiContextCompactor.cpp:50`).

## Useful later work, not a blanket rewrite

- Results over8KiB are clipped with a suggestion to read the child's session
  (`MaiSubAgentTools.cpp:25,44`), but model `session_history` is current-session
  only (`MaiSessionTools.cpp:91-170`). Data remains available through ordinary
  API or the child, so add a scoped paginated child-result read rather than
  weaken arbitrary cross-session access.
- Codex tree paths/roster (`agent/registry.rs:165`, `control/target.rs:45`,
  `session/world_state.rs:154`) help larger teams. Our direct-child-ID isolation
  is currently intentional; broader routing needs a defined authority boundary.
- Host admission/accounting injection (`agent/host/ownership.rs:15,39`,
  `tasks/admission.rs:44`) can help a cross-platform host impose a single task
  policy, but does not itself supply distributed transport or retry guarantees.
- Roles and lifecycle test matrices are valuable. Separate tests already exceed
  half the referenced directory's counted lines; our existing subagent tests
  cover depth/same-parent counts/close/cross-parent denial but omit the exact
  stale-result, tree-total and busy-message scenarios above.

Codex V2 is wired but conditional, not universally default:
`features/src/lib.rs:1660` defaults it off, and `core/src/config/mod.rs:1748`
also considers model/version settings. The source also documents best-effort
completion and non-atomic execution check/guard (`agent/api.rs:114-135`), so it
must not be sold as a perfect or universal reliability baseline.

Recommended order: structured turn results and errors; durable messaging plus
lifecycle/capacity consistency; shared usage budget; optional effective-context/
role inheritance; then larger-team registry and host-policy extension. No new
dependency, protocol rewrite or production change is authorized by this audit.
