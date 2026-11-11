# Tool output retention and execution recovery implementation plan

> **For agentic workers:** Use superpowers:subagent-driven-development for independent components and review each component before integration.

**Goal:** Run a shell command once, retain its bounded complete output for subsequent reads, and give the model factual execution and recovery information.

**Architecture:** Keep the existing in-process MaiAgent and native tools. Store large output separately from message previews in the same session backend. A typed execution record remains separate from raw tool output, so existing media JSON and artifact consumers retain their contract. Ordinary C++ callers and model tools use the same read API.

**Tech Stack:** C++17, existing memory/SQLite stores, native Windows/POSIX processes, existing CMake test targets. Work directly on main; preserve the existing .gitignore change and ignored upstream references.

## Approved scope and observable acceptance

- [x] A self-owned command writes an execution counter and more than 100 KiB of output. The initial observation is bounded; subsequent reads recover the final marker, including after reopening SQLite. The counter remains one.
- [x] Output handles are immutable and restricted to the current session. Invalid offsets and cross-session reads fail. UTF-8 scalar boundaries and invalid source bytes are represented truthfully.
- [x] Capture remains bounded: at most 16 MiB per output, 64 MiB per session, and 256 MiB overall. Eviction, capture truncation, and unavailable storage are explicit. Clearing/removing a session also clears its retained output. Full output is saved when the command returns; a process crash before that point does not claim durable output.
- [x] Command exit status, start failure, timeout, cancellation and uncertain side effects are distinct. Existing permission refusal and cancellation semantics remain intact.
- [x] An accepted asynchronous task keeps its task ID and receives query-status guidance after a recoverable query failure. No automatic replay of shell commands or paid task submissions is introduced.
- [x] Raw media/artifact JSON stays unchanged. Trusted execution metadata reaches the model separately, survives store reopening, and participates in history/checkpoint identity.

## Tasks

### 1. Establish failing behavioral checks

- [x] Extend the shell regression with a self-owned long-output command and a counter; assert an output handle exists and rereading does not execute the command again.
- [x] Add execution-status checks for launch failure versus nonzero exit and interrupted execution; record actual failures against the old implementation before changing behavior.

### 2. Retained output core and stores

- [x] Add MaiToolOutput.h / Agent/MaiToolOutput.cpp with typed record/query/page and shared validation/paging helpers.
- [x] Extend MaiSessionStore with default NotSupported methods, implemented by memory and SQLite stores. Keep raw output in BLOB storage, immutable by ID, with bounded retention and lifecycle cleanup.
- [x] Add memory/SQLite parity, persistence, ownership, quota and byte-boundary checks.

### 3. Execution contract and recovery

- [x] Add MaiToolExecution.h / Agent/MaiToolExecution.cpp; optional metadata in MaiToolResult and MaiToolPart. Serialize via string boundaries, without JSON types in public headers.
- [x] Persist metadata in an additive SQLite migration; old messages remain valid.
- [x] Populate factual pre-execution versus post-execution states in MaiTurnRunner, preserve existing provider task recovery, and publish metadata to the model after the tool-results batch without rewriting raw output.
- [x] Change interrupted-running recovery guidance to inspect current state; never advise blind replay. Retain the original UI state numbering.

### 4. Shell and native read integration

- [x] Increase bounded shell capture to the retained-output limit; keep a small initial preview and save completed/partial output before returning. Preserve output on timeout/cancel and mark incomplete capture.
- [x] Check timeout/cancellation even while output continuously arrives on both process platforms.
- [x] Add tool_output_read plus MaiAgent::readToolOutput, session-bound and using the ordinary store API. Inject the session store in the tool context.
- [x] Existing tool-detail reads may resolve retained output through the verified source part, preserving their public identity/revision/page limits.

### 5. Integration, review and evidence

- [x] Run the same failing scenarios against the new implementation, including a native Windows child process and fake-model continuation using the real SQLite store.
- [x] Run required core suites and console e2e because storage changes. Build the ordinary Windows desktop consumer; do not replace the installed application or contact paid providers.
- [x] Review correctness, cancellation, quotas, migration compatibility and media JSON compatibility. Format modified C++ and write docs/diagnostics with exact source/build/test boundaries.

No MCP/Skill integration, new service, process-topology redesign, version bump, release, or installation is part of this work.
