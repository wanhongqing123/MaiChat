# Responses-only Implementation Plan

> Production edits and shared builds execute serially; independent Core/Desktop/Mobile test migrations use superpowers:dispatching-parallel-agents with disjoint files. Work on main as required by AGENTS.md.

**Goal:** Remove the product's Chat Completions network implementation and keep one Responses path with verified item, error, and usage contracts.

**Architecture:** Keep the neutral model interface and native local GGUF runtime. The shared network factory constructs only Responses. Existing native output snapshots remain authoritative for replay. Provider usage normalization retains provenance and business errors never create successful responses.

**Tech Stack:** Existing C++17 Core, libcurl, nlohmann JSON, SQLite, Qt and mobile adapters. No new dependencies, services, or branches.

## Reproduction and expected behavior

- A default MaiModelConfig requests `/responses`, never `/chat/completions`.
- A GLM HTTP 200 body containing authentication error code 1001 fails with a classified error, without publishing successful items, usage, continuation, or tools.
- Responses input/output items keep identity, order, source provenance, and opaque state across tool calls and SQLite reopen.
- Usage records preserve supplied input/output/total/cache/reasoning values and distinguish missing fields from provider-reported zero. Derived total is marked derived.
- No Chat Completions product implementation, selection, fallback, test fixture, or live test remains. Vendored upstream internals and native local chat templates are not network protocol implementations.

## Work and validation

- [ ] Preserve existing dirty files and record baseline inventory/hashes in `.cache/responses-only-20261125/`.
- [ ] Add a dedicated ordinary Core protocol suite; compile against the existing archive and observe the default-route and HTTP-200-business-error failures.
- [ ] Delete `MaiOpenAiClient.cpp/.h`; move the generic factory and wire-name helper to the Responses implementation. Remove the Chat enum/config/default and update callers.
- [ ] Migrate protocol-independent tests to genuine Responses SSE/items; remove obsolete Chat-specific tests and duplicate loops. Retain image, retry, TLS/proxy, cancellation, tool pairing, and opaque-state assertions.
- [ ] Update Desktop/Mobile/Android/iOS fixtures and configuration validation to Responses. Remove stale public documentation claims.
- [ ] Add normalized usage fields and provenance, preserve original numeric detail fields, and handle HTTP 200 business errors. Test absent/null/zero/derived values and unsuccessful output isolation with loopback servers.
- [ ] Verify native item replay and existing dialogue-preserving/413 regressions against one fresh normal Core archive.
- [ ] Build the normal Windows App, run required Core suites and affected Desktop/Mobile tests available locally, and validate deployed dependencies and isolated smoke startup. Report unavailable Apple/Android device validation explicitly.
- [ ] Review the final diff and document source/build identity, exact checks, remaining boundaries, and compatibility handling. Do not modify user databases, stop installed apps, or publish a release without a corresponding user instruction.

## Scope boundaries

The previously approved JSONL-plus-SQLite storage and provider-derived context display remain active follow-up work; this protocol change establishes their shared wire contract and does not silently replace them with a token-growth optimization. Existing uncommitted dialogue-preservation and unrelated UI changes must remain intact.

The user's later context-sending and PTY instruction is also active: implement a small product-selected direct tool catalog plus discovery on the same registry, and explicit optional PTY support in MaiShellSessionManager. Codex does not universally expose seven tools or default all execution to PTY; pin and document the selected behavior. Do not combine these changes with persistent-log format migration or introduce MCP/services/new dependencies.
