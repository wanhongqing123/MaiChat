# Compact Builtin Capability Summary Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development for Core implementation and independent review. Work directly on main; Root alone commits/pushes. Do not touch the concurrently integrated desktop composer files. No additional user approval is required for the explicitly requested implementation.

**Goal:** Give the model a lightweight, truthful builtin capability index before it chooses tool_search, and shorten builtin tool descriptions without removing operational contracts.

**Architecture:** Reuse `MaiToolRegistry::metadata(actualModel)` and an independent immutable session/model summary snapshot in `MaiModelToolCatalog`. Runtime prepares this once on the worker thread and appends it to the existing base-instruction pipeline before context preparation/measurement. Tool schemas remain bounded/append-only and execution stays in the existing registry/tool classes.

**Tech Stack:** Existing C++17, session context insert-if-absent, registry metadata, shared Core tests. No new library, service, model proxy, paid API call, global HTTP policy, or current Codex skill installation.

## Task 1: Frozen summary and direct API

**Files:** `MaiAgent/include/MaiModelToolCatalog.h`, `MaiAgent/Agent/MaiModelToolCatalog.cpp`, `MaiAgent/tests/MaiModelToolCatalogTests.cpp`.

- [x] Add a normal Core entry point returning `MaiResult<std::string>` for a prepared capability summary; document worker/thread/storage/failure/budget contracts in the header, no header implementation or JSON exposure.
- [x] Write tests with fake public/hidden/model-gated tools. The first summary names real deferred tools and their purpose/discovery route; no parameters schema generation, tool execution, Skill body read, network or configuration secret query occurs.
- [x] Use a separate versioned session/model context key and insert-if-absent winner. Reopen/concurrent preparation returns the same bytes; save failure does not activate a candidate. Preserve old v5 declaration snapshots, append ordering and existing history.
- [x] Render deterministic bounded metadata: one-line UTF-8-safe short purposes and exact names. Avoid mutable loaded/status/time fields. Preserve honest discovery guidance under tight budgets; do not silently truncate names or byte sequences.
- [x] Callers must still select via tool_search and receive the schema in a later request. Summary metadata confers neither execution eligibility nor permissions, and does not promise credentials/backend readiness.

## Task 2: Runtime injection

**Files:** `MaiAgent/Agent/MaiAgent.cpp`, `MaiAgent/include/MaiTurnRunner.h`, `MaiAgent/Agent/MaiTurnRunner.cpp`; targeted Runner/Runtime test TU if needed.

- [x] Add an optional internal summary-preparation callback at the END of Dependencies if required; document that all C++ consumers must rebuild. Keep existing aggregate fields and cancellation behavior.
- [x] Prepare once in the worker before normal requests, alongside the existing Skill preparation pattern. Add it to `baseInstructions` before `buildRequest`/context fitting/request-size accounting; do not append after measurement or inject into desktop welcome cards.
- [x] Capture a real initial model request with a fake model: summary visible and schemas still only bootstrap. After tool_search selection, next request appends selected schema while the summary bytes remain stable. Unknown undeclared calls remain rejected.
- [x] Retain Stop/steer/recovery behavior and actual-model/session isolation. This is unrelated to HTTP2 retry classification and must not modify it.
- [x] Verify the real builtin registry in a4096-token fake-model window against a no-summary baseline, not only the minimal single-tool test. A confirmed2670→4442-token regression requires a context-aware initial render budget. Preserve first-save bytes on later budget requests; do not recrop persisted summaries or v5 declarations.

## Task 3: Description curation

**Files:** Relevant `MaiAgent/AgentTools/*.cpp` descriptions, with a precise changed-file inventory in delivery notes; no blanket generated rewrite.

- [x] Compare the pinned Codex shell/view_image/apply_patch examples against our actual contracts. Curate long builtin descriptions into action, input/output form and essential constraints; leave necessary parameter descriptions/schema restrictions, permissions, payment, overwrite, lifecycle and recovery rules intact.
- [x] Audit all builtin descriptions for tutorial/example/duplicated parameter lists; change the ones that benefit, explain any intentionally longer complex coordination descriptions. Do not use first-period truncation or a universal hard length as a substitute for semantic edits.
- [x] Preserve existing frozen descriptions for existing v5 snapshots. New declarations/snapshots use the curated source descriptions. Do not silently overwrite old schemas solely to reduce tokens.
- [x] Do not advertise a Skill/help path until registered/discovered and packaged. Do not claim native ffmpeg `-h` reaches tool output without the separately bound owned probe; current curl wrapper is a limited argument interface, not unrestricted curl CLI.
- [x] Removing tutorial examples must leave a sufficient directly reachable usage contract. No second command parser/execution core is added.

## Task 4: Validation and review

- [x] Use the existing catalog and Runner test targets for genuine RED/GREEN. No paid supplier calls or real user data. Source fixture-only compilation/error records remain distinct from product RED.
- [x] ROOT owns the shared `.cache/r311` build lane while composer verification runs. Core implementer must not build there concurrently; provide exact inputs/target list and let Root schedule it, or use an explicitly owned pre-existing separate lane with no worktree/branch/source deletion.
- [x] Check catalog persistence/memory+SQLite, legacy handling, model isolation, bounds/UTF-8, storage failure, no schema materialization, direct API/model request consistency and actual request injection.
- [x] Root runs required shared Core suites after unified rebuild, normal App/console/JNI as appropriate, relevant composer regressions again for the combined build, and owned staging/ZIP smoke. Preserve unrelated Qt WIP.
- [x] Independent spec and code review before Root commit/push; update diagnostics with exact source/build/test bindings and platform limitations. No formal release or installed App replacement is included. Git/IM delivery records bind the final commit; Mac same-source consumption is reported separately.
