# Compact builtin capability summary: Core validation

Validation date: 2026-11-28, Windows x64, Asia/Shanghai. Core edits started against
`a31dcbd7`; the independent desktop file-drop commit `f0e65450` landed during this
work. Core changes remained unstaged and uncommitted throughout this validation.

## Reproduction and expected behavior

Create a new discovery-enabled conversation using the real `MaiAgent` runtime and
an in-process fake model. Register one public deferred `catalog_action`, one hidden
action, and the normal `tool_search` bootstrap. The first actual request must show
the public action's name and purpose in base instructions, while its complete
schema remains absent. A premature call and a same-response call after selection
must be rejected. Selection makes the schema available in the next request; then
the action executes once. Instructions must remain byte-identical throughout.

The ordinary Core summary API must return those same bytes. Preparing the summary
must not construct parameter schemas, execute tools, read Skill bodies, request
credentials or access the network. Preparation must precede request context fitting
and token measurement. A 4096-token fake-model window and its estimator inspect
that same prepared request in the integration test.

## Root cause and implementation

The previous runtime exposed bounded schemas but supplied no lightweight index of
the remaining real registered tools. A model therefore had to know an unavailable
name or guess that searching would help. Separately, Seedance's `description()`
called `specialistInfo()` and its credential callback, even for pure metadata
enumeration.

`MaiModelToolCatalog::capabilitySummary` now reuses
`registry.metadata(actualModel)`. Its independent
`model-tool-summary-v1-<model SHA-256>` session-context record uses the existing
insert-if-absent winner and is cached only after successful persistence. It does
not prepare the declaration snapshot. Summaries are immutable per session/model;
existing v5 declaration snapshots, descriptions, appended schemas and messages are
preserved. NotSupported backends retain instance-lifetime freezing only.

Rendering sorts exact real names, folds whitespace in purposes, preserves UTF-8
boundaries and stays within 8192 bytes. Purposes use up to 128 bytes, then 64 bytes,
then names only if necessary. Oversized catalogs omit complete entries and retain
the explicit reachable `tool_search` browse/offset/limit route. Unusual names use
reversible JSON quoting. The text states that selected schemas arrive in the next
request and grants neither permissions nor readiness claims.

The optional `prepareToolSummary` callback is appended at the end of Runner
Dependencies. It runs once on the worker after the actual model is fixed, alongside
Skill preparation, before `buildRequest`, context fitting and measurement.
All C++ consumers must rebuild for the changed aggregate layout. Existing Stop,
steering, discovery selection and execution gating retain their existing paths.
Seedance descriptions are now static; explicit discover retains configuration
reporting.

## RED/GREEN evidence

The owned evidence directory is `.cache/core-summary-20261128/`. Builds used the
normal MSVC toolchain, the pre-existing `.cache/r311/build-msvc` lane, and the
repository `MaiChat/desktop/scripts/run-tests-windows.ps1` wrapper. The lane was
exclusively assigned to Core during these checks. Only the 20 Core source/test
files below were synchronized; no desktop file was copied or edited.

| Receipt | Build | Tests | Observation |
|---|---:|---:|---|
| `red-receipt.json` | 0 | exit 8 | Catalog 2 failures: credential callback read / missing paid contract; runtime 16 failures: missing actual-request summary |
| `api-red-receipt.json` | 0 | exit 8 | 13 failures against the empty summary scaffold, including ordering, model isolation, failure and winner behavior |
| `initial-green-receipt.json` | 0 | 2/2 | Initial implementation passed catalog and actual runtime |
| `regression-green-receipt.json` | 0 | 12/12 | Store, InputQueue, TurnRunner, ToolLoop, Catalog, DiscoveryRuntime, UnifiedExec, Tool, Edit, ApplyPatch, SkillRuntime, SkillInstaller |
| `final-green-receipt.json` | 0 | 4/4 | Runtime, InputQueue, Catalog, DiscoveryRuntime after final Core and Seedance changes |
| `capture-green-receipt.json` | 0 | 2/2 | Captured initial actual request and current builtin payload measurements |
| `review-green-receipt.json` | 0 | 2/2 | Distinct concurrent candidates, NotSupported reopen and legacy full-declaration preservation included |

An intermediate `curated-green` command named a test target absent from this
desktop build graph and exited before compilation. This was a command-selection
error, not product RED; the corrected regression command above built and passed.
Formatting used clang-format 16 on changed ranges; `git diff --check` passed.

`final-captured-test-output.log` is the owned copy of the final CTest output before
returning the lane to Root. It records an initial `glm-5.4` request with **one
bootstrap schema and a 337-byte summary** containing `catalog_action` and its real
purpose. The standard builtin fixture measured **44 tools / 26052 declaration
field bytes**, versus **7 initial tools / 7352 bytes**, plus a **5314-byte summary
/ 1788 estimated tokens**. The summary omitted no builtin in that fixture.
These are fixture measurements, not guarantees for every host registry/model.

Catalog checks cover both memory and SQLite, same-session reopen, new model/session
isolation, actual-model/hidden filtering, zero schema creation for summary-only
calls, credential callback count zero, concurrent distinct first-save candidates,
read/write faults and no failed-candidate activation, 600-entry bounds and honest
omission guidance, a UTF-8 truncation boundary, empty/unreachable discovery, legacy
full declaration stability, and NotSupported instance/reopen semantics.

## Description curation and retained contracts

All 74 description implementations in `MaiAgent/AgentTools/*.cpp` were inspected;
17 methods changed across these 13 files:

| File | Changed tools / contract retained |
|---|---|
| `MaiAgentSendFileTool.cpp` | `agent_send_file`: current persistent card, workspace-relative path, no upload/execution/mutation, retained file lifetime |
| `MaiAgentSendMediaTool.cpp` | `agent_send_media`: show again in current reply, exact camera path, no vision requirement, Photos/IM boundary, owning video task ID |
| `MaiArkMediaTools.cpp` | `seedance_video`: static metadata, paid confirmation, model limits, prepare/asset-only paid submission, Active/type/group/person validation, cancellation/timeout-before-payment, task cancel/delete and delivery boundaries |
| `MaiEditTool.cpp` | `file_edit`: unique/replace-all matching, indentation, UTF-8/NUL, original/result 8 MiB, bounded output |
| `MaiExecCommandTool.cpp` | exec_command/write_stdin: selected Windows shell syntax and path handling, closed stdin/PTY, wait-vs-lifetime, bounded output/recovery without relaunch, owner/background/cancel and approval semantics |
| `MaiModelStudioTools.cpp` | `wan_video`: no paid delegate/revise or silent model switching; discover/diagnose and existing-task checks remain reachable; PENDING-only cancel and delivered-local-record-only delete retained |
| `MaiScreenshotTool.cpp` | `screenshot`: pointer display only, no computer input binding, coordinate boundaries, per-call capture approval, persistent-session stop cleanup, exact output metadata and same-file delivery |
| `MaiSkillInstallTool.cpp` | `skill_install`: real discovered selection, pinned source, user/workspace destinations, no overwrite/arbitrary destination, atomic publication, no script/permission/dependency execution, next-input discovery |
| `MaiSubAgentTools.cpp` | `spawn_agent`, `wait_agent`, `send_input`, `close_agent`: same directory and approval, immediate ID, timeout without task restart, retained sub-agent context, stop/free-slot/result retention; `list_agents` unchanged |
| `MaiTemporalWatermarkTool.cpp` | `media_remove_watermark`: all regions/precise frames, verified clean references, foreground/ONNX behavior, new output and retained source/audio, review estimated pixels, no repair loops |
| `MaiTimeTool.cpp` | `current_time`: current date/time action |
| `MaiTodoWriteTool.cpp` | `todowrite`: whole-list replacement and exactly one in-progress item |
| `MaiViewImageTool.cpp` | `view_image`: local supported formats, bounded JPEG/model-image support, display route only when available, existing prompt image attachments and screenshot-path ambiguity |

Intentionally longer descriptions retain their only reachable usage contracts:
App storage preview ownership/expiry/exclusions and execution selection; Ark asset
verification/upload/Active lifecycle; session history checkpoint/byte paging and
untrusted data; mobile shell's finite command language; curl's actual supported
argument subset; local llama's supported flags/status/unload ownership; patch
syntax and partial-failure/overwrite semantics. CV descriptions use branches for
different real actions, so their combined source literal size is not the size of
one exposed tool. DeepSeek/GLM web contracts keep billing, retry and input limits.

No tutorials were moved to an undiscovered Skill or nonexistent help endpoint.
FFmpeg's no-stdin/new-output/capture_log contract remains intact. Native `-h` is
not advertised: the separate owned probe showed help on stdout rather than the
tool's captured output. No FFmpeg stdio, execution parser or second core was added.

## Validation boundaries

These are Windows Core tests with synthetic, in-process model responses and owned
temporary storage. They prove the prompt/declaration and storage contracts, not
that every real model will decide to search. No real provider request, paid media
operation, user database, Key provider or environment configuration was accessed.
Root owns the subsequent combined full-Core/App/JNI/package validation and final
commit/push. No installed App replacement, formal release, desktop UI, HTTP2,
MaiResponsesClient or MaiModelRetry change is part of this Core work.

Root's independent source review found no P1/P2 issue. One final wording refinement
was handed to Root: change view_image's "User attachments" to "User image
attachments", to distinguish prompt images from ordinary file-path references.
The following inventory and raw hashes describe the exact Core handoff tested by
`review-green`; Root's later wording change and combined rebuild have their own
source/test binding.

The full source-method inventory and exact baseline/after raw SHA-256 values follow.
Literal-byte counts sum source strings across conditional branches; they are not
runtime tool lengths or token counts.


| File / description ordinal | Before literal bytes | After literal bytes | Decision |
|---|---:|---:|---|
| `MaiAgentSendFileTool.cpp` #1 | 345 | 248 | Curated |
| `MaiAgentSendMediaTool.cpp` #1 | 895 | 493 | Curated |
| `MaiApplyPatchTool.cpp` #1 | 562 | 562 | Retained |
| `MaiAppStorageTool.cpp` #1 | 1580 | 1580 | Retained |
| `MaiArkAssetTools.cpp` #1 | 1631 | 1631 | Retained |
| `MaiArkMediaTools.cpp` #1 | 2102 | 1520 | Curated |
| `MaiArkMediaTools.cpp` #2 | 248 | 248 | Retained |
| `MaiCurlCliTool.cpp` #1 | 565 | 565 | Retained |
| `MaiCvImageTools.cpp` #1 | 1688 | 1688 | Retained |
| `MaiCvVideoTools.cpp` #1 | 486 | 486 | Retained |
| `MaiDeepSeekWebTools.cpp` #1 | 914 | 914 | Retained |
| `MaiDownloadFileTool.cpp` #1 | 287 | 287 | Retained |
| `MaiEditTool.cpp` #1 | 552 | 389 | Curated |
| `MaiExecCommandTool.cpp` #1 | 1558 | 1180 | Curated |
| `MaiExecCommandTool.cpp` #2 | 616 | 476 | Curated |
| `MaiFfmpegTools.cpp` #1 | 358 | 358 | Retained |
| `MaiFfmpegTools.cpp` #2 | 425 | 425 | Retained |
| `MaiFileTools.cpp` #1 | 336 | 336 | Retained |
| `MaiFileTools.cpp` #2 | 145 | 145 | Retained |
| `MaiFileTools.cpp` #3 | 111 | 111 | Retained |
| `MaiFileTools.cpp` #4 | 79 | 79 | Retained |
| `MaiFileTools.cpp` #5 | 388 | 388 | Retained |
| `MaiFileTools.cpp` #6 | 275 | 275 | Retained |
| `MaiFileTools.cpp` #7 | 228 | 228 | Retained |
| `MaiFileTools.cpp` #8 | 96 | 96 | Retained |
| `MaiFileTools.cpp` #9 | 480 | 480 | Retained |
| `MaiFileTools.cpp` #10 | 647 | 647 | Retained |
| `MaiGlmMediaTools.cpp` #1 | 286 | 286 | Retained |
| `MaiGlmWebTools.cpp` #1 | 613 | 613 | Retained |
| `MaiHttpRequestTool.cpp` #1 | 373 | 373 | Retained |
| `MaiInProcessShellTool.cpp` #1 | 1347 | 1347 | Retained |
| `MaiLlamaTools.cpp` #1 | 677 | 677 | Retained |
| `MaiMiniMaxMediaTools.cpp` #1 | 415 | 415 | Retained |
| `MaiModelStudioTools.cpp` #1 | 1795 | 530 | Curated |
| `MaiModelStudioTools.cpp` #2 | 277 | 277 | Retained |
| `MaiModelStudioTools.cpp` #3 | 280 | 280 | Retained |
| `MaiModelStudioTools.cpp` #4 | 296 | 296 | Retained |
| `MaiNetworkIpTool.cpp` #1 | 229 | 229 | Retained |
| `MaiOpenSslCliTool.cpp` #1 | 281 | 281 | Retained |
| `MaiOpenSslTool.cpp` #1 | 221 | 221 | Retained |
| `MaiOpenSslTool.cpp` #2 | 225 | 225 | Retained |
| `MaiPdfTool.cpp` #1 | 426 | 426 | Retained |
| `MaiProviderWebTools.cpp` #1 | 197 | 197 | Retained |
| `MaiQuestionTool.cpp` #1 | 338 | 338 | Retained |
| `MaiScreenshotTool.cpp` #1 | 1315 | 787 | Curated |
| `MaiSessionTools.cpp` #1 | 292 | 292 | Retained |
| `MaiSessionTools.cpp` #2 | 185 | 185 | Retained |
| `MaiSessionTools.cpp` #3 | 1515 | 1515 | Retained |
| `MaiSessionTools.cpp` #4 | 506 | 506 | Retained |
| `MaiSessionTools.cpp` #5 | 210 | 210 | Retained |
| `MaiSessionTools.cpp` #6 | 224 | 224 | Retained |
| `MaiSessionTools.cpp` #7 | 352 | 352 | Retained |
| `MaiSessionTools.cpp` #8 | 258 | 258 | Retained |
| `MaiSkillInstallTool.cpp` #1 | 1101 | 688 | Curated |
| `MaiSkillTool.cpp` #1 | 262 | 262 | Retained |
| `MaiSshTool.cpp` #1 | 449 | 449 | Retained |
| `MaiSubAgentTools.cpp` #1 | 426 | 184 | Curated |
| `MaiSubAgentTools.cpp` #2 | 173 | 112 | Curated |
| `MaiSubAgentTools.cpp` #3 | 236 | 83 | Curated |
| `MaiSubAgentTools.cpp` #4 | 80 | 80 | Retained |
| `MaiSubAgentTools.cpp` #5 | 160 | 82 | Curated |
| `MaiSystemResourcesTool.cpp` #1 | 236 | 236 | Retained |
| `MaiTemporalWatermarkTool.cpp` #1 | 1033 | 655 | Curated |
| `MaiTimeTool.cpp` #1 | 136 | 63 | Curated |
| `MaiTodoWriteTool.cpp` #1 | 269 | 108 | Curated |
| `MaiToolOutputTool.cpp` #1 | 522 | 522 | Retained |
| `MaiToolSearchTool.cpp` #1 | 284 | 284 | Retained |
| `MaiUploadFileTool.cpp` #1 | 313 | 313 | Retained |
| `MaiVideoGeometryTool.cpp` #1 | 360 | 360 | Retained |
| `MaiVideoMattingTool.cpp` #1 | 309 | 309 | Retained |
| `MaiViewImageTool.cpp` #1 | 368 | 277 | Curated |
| `MaiWebFetchTool.cpp` #1 | 178 | 178 | Retained |
| `MaiWindowListTool.cpp` #1 | 159 | 159 | Retained |
| `MaiZlibTool.cpp` #1 | 277 | 277 | Retained |


| Source path | Baseline raw SHA-256 | Final raw SHA-256 |
|---|---|---|
| MaiAgent/Agent/MaiAgent.cpp | 05de425dc1590ee5564c9f8931902b3d81d125affe5a74a59953742abd94cef2 | 2e2911955a0e343c0c9443f897632453b00a1afc0f683fe4702f54bd0cf07707 |
| MaiAgent/Agent/MaiModelToolCatalog.cpp | e749e049d033d8c8db78709df0c13630c67deedaabdba8cc1f99e9fe71834db1 | d989f75e4ae56902570a1475ef0126dc454d5133e93104d77c61298d8f2cd08a |
| MaiAgent/Agent/MaiTurnRunner.cpp | 71e6dcf276389b77fd76040a907fbb865890f6b3920719acbfc5f569d9da373a | e041719b13946104d731ae954c32364dc8e03a225c940dd86b9eeb07e1b5d5a4 |
| MaiAgent/AgentTools/MaiAgentSendFileTool.cpp | 3f9c5280939c020708825c6afd63eb424357bc80ddbf494958890ecc4271894a | 8200f459a2dfd17b4ce70757ccf8e2964dad7cbf6e2c950d10fa3cc6e7e4c8d6 |
| MaiAgent/AgentTools/MaiAgentSendMediaTool.cpp | e1355caee99d148591c6c59ab6e8839056f4916929491e607fb5f9e9efa94fe8 | 72de95a9d8fdd81aca37af7ad1858ba5b34df0ae2acfb2e0203a7f2393545610 |
| MaiAgent/AgentTools/MaiArkMediaTools.cpp | a5ed0520f339d5549306beb252e7518a5cb3cd7dd31caae988679d8800fd6ea0 | 315b60f2f8ee287391fd1664e66a8473b8d656af069064dce7f4b700f5ab9941 |
| MaiAgent/AgentTools/MaiEditTool.cpp | f5f155b20dd2e0459aee810335abf2702ab2be7f42817f0cf26a2b448845a3a9 | 8ecfac060607fef646c546cd64d92911718679d97da4dc720e455a31cb87f9f2 |
| MaiAgent/AgentTools/MaiExecCommandTool.cpp | cefefc1ca0ca434ed16bae1ad7c93b9b0df8f5f158aba4466970f2be5eb87172 | 7fb836b73cc814ca1a816e4f98257fd8d2c7757042c2aba830e5818d5fa69bc8 |
| MaiAgent/AgentTools/MaiModelStudioTools.cpp | 9620bfd062bafcbb7badc9007ee2abb5b63d420a41d1710409ee93ead95c1912 | a53d4ab05ea36cb308b15bc324eb33b083e1e551d0541eb0abed0964e805f2b3 |
| MaiAgent/AgentTools/MaiScreenshotTool.cpp | 6dd0ab502f6901dfc6bfa3bfdae5c2cd99c218ff9e2a7f85c8de8a8284246a6e | 04e20ad23d280e372b1be1da34b65fd3367b93dc4f85fe3c5a95c18d190bdda7 |
| MaiAgent/AgentTools/MaiSkillInstallTool.cpp | f1e3a5fd057be70d8f4281599fed9078989a1c5a3181bf2f339562b98f33dfe3 | 5dabee55aeb514a6be24c24d2a3a1cfb788f943ca19b72585fde7cae7eb02441 |
| MaiAgent/AgentTools/MaiSubAgentTools.cpp | f21394579212cceebc0fe78094eec6c84b8ef3ce709f4b467cd786943af16387 | 52aa913610a743dfd274406cbec23c3ab2f68d679059e07ea68510ff5d676f32 |
| MaiAgent/AgentTools/MaiTemporalWatermarkTool.cpp | 9c8a505b4d2828f1a7407c14463c6953263a91d8c8c8c6c54acd72d1f0e92823 | d2319c768b5c0c9c488506c9bc4d7b0c9e17c9fb56c4fcf043909512a71b576c |
| MaiAgent/AgentTools/MaiTimeTool.cpp | 65e69e4eff356faad6a4744a0a963e8936cdb02ebd6220acf7297637e58c23dc | b7292885b8b071edf07b7c50392a394a1ce29544f12dae2401654aaa748d080e |
| MaiAgent/AgentTools/MaiTodoWriteTool.cpp | d290161c8e470ba6f81ac9933a5c2cc01bb5ea12d1bd8796cb4808787044e6e4 | 3191ab5288574f2cf02c7db854b540d460c0e3fd9980757d4f8c9b27dd9e7e61 |
| MaiAgent/AgentTools/MaiViewImageTool.cpp | a9ae4d432e19fb2a3b37eab5d7a6614bd4ccab2b1351135ac1245b3db9380f8f | 82a671fbf16f558cc7758119fddf2b24f5cb91456719a4a153dba89f27fa09c6 |
| MaiAgent/include/MaiModelToolCatalog.h | f19bd6e6edbcc16a975047b058cbd9f47e64d2561ba3443ece7cc27d38c7296d | e9e24e5186a6d5294a756b3b893c682101ea25bd625813ae7654719a7ce5db77 |
| MaiAgent/include/MaiTurnRunner.h | ee18ac77a85b95dc4fa1c80b30d3a3c40f9cbf6794f362f68e2a1a74498bf26b | 3b2e2f3de76d85be01756377eb74204db2a0f9ca91da0eb69c4e61672548d8fb |
| MaiAgent/tests/MaiModelToolCatalogTests.cpp | 74072c06ddffd317fa6c1c2c42d7f23a45eae84025511ec3063a047e0f0eaa21 | e59d1d892405f86d49b4afbf59b1617095f0e9aee962775b462fc411bed5e6d3 |
| MaiAgent/tests/MaiToolDiscoveryRuntimeTests.cpp | fd321f4500e0b3016ccb19821a1b9759a4c4657fa3d2e38f5ca7143c70b465aa | 71d91064bbaadfb7509b9b145b440ab29d978d95d8925838c2135690f3c79d2e |

Root wording handoff: MaiViewImageTool.cpp tested handoff SHA-256 4cd416af67092a364d37b377ee06548884b61a29646542fec53724e770217343; final Root wording SHA-256 82a671fbf16f558cc7758119fddf2b24f5cb91456719a4a153dba89f27fa09c6. Only that wording differs from the 20-file source binding in review-green; Root owns the combined verification.

## Subsequent real-builtin small-window regression

Root requested an additional pre-commit capacity check using the actual builtin
registry rather than the earlier one-action fixture. The original 4096-token
integration test had only one bootstrap schema and a 337-byte summary; it did not
establish that the standard builtin declaration set plus summary would fit.

`builtin-window-red2-receipt.json` records a successful normal build and a genuine
expected-success test failure (wrapper exit 8, two assertions). Both fresh sessions
used real builtins, seven initial declarations, model `glm-5.4`, identical short
user/base instructions, the default `MaiModelClient` estimator, and fake-model
limits `{4096,128}`. The baseline fixed an empty summary before the turn to model
the prior request behavior without modifying production code.

| Case | Largest estimated input tokens | Summary bytes | stream calls | Outcome |
|---|---:|---:|---:|---|
| Baseline without summary | 2670 | 0 | 1 | Completed |
| Feature with current summary | 4442 | 5314 | 0 | Failed before sampling |

The capacity error states that the current instruction and protected recent tool
observation exceed the selected model's input capacity; original history is
preserved. The fake model only emits a fixed text answer. No tool, credential
provider or network request was executed. The owned full output is
`builtin-window-red-captured-output.log`. An earlier command used a nonexistent
`Succeeded` enum value; its compilation failure is not counted as product RED.

This is a confirmed regression. The narrow proposed fix is an optional first-render
summary byte budget informed by the actual model's known context window, with the
existing 8192-byte default for unknown windows. It must retain metadata-only
preparation, exact names, UTF-8 bounds, discovery guidance, first-save freezing and
the existing v5 declarations. Existing saved summary bytes must not be silently
trimmed in place. Production code was unchanged when this regression was reported
to Root; the final fix and subsequent validation need a separate source binding.

## Adaptive first-render budget fix and GREEN

Root authorized the narrow fix after the genuine RED above. Only five Core files
changed for this correction: `Agent/MaiAgent.cpp`,
`Agent/MaiModelToolCatalog.cpp`, `include/MaiModelToolCatalog.h`,
`tests/MaiModelToolCatalogTests.cpp`, and
`tests/MaiToolDiscoveryRuntimeTests.cpp` (all under `MaiAgent/`).

The ordinary API accepts optional `firstRenderByteBudget=8192`, clamped to
512..8192 bytes only while rendering an absent candidate. Runtime derives it from
the actual model's context-window metadata as `window / 8 * 3`, with the same floor
and cap; an unknown window uses 8192. Preparation still reads only registry
metadata and never constructs schemas for the budget calculation. The existing
independent v1 context key and insert-if-absent winner remain unchanged. A cached
or persisted summary is returned byte-for-byte even when a later call requests a
different budget; v5 declarations are unaffected.

This limits the additional directory cost. It does not guarantee that arbitrary
base instructions, user input, already-frozen summaries or schemas fit any chosen
model. The complete request still passes through the existing authoritative
context/request budget checks.

`budget-green-receipt.json` records normal build exit 0 and **4/4 passing targets**:
MaiToolDiscoveryRuntimeTests, MaiModelToolCatalogTests, MaiInputQueueTests and
MaiTurnRunnerTests. The full owned output is
`budget-green-captured-output.log`. The true builtin fixture now records:

| Window / output limit / base | Baseline input tokens | Feature input tokens | Feature summary | Deferred names retained | Sampling |
|---|---:|---:|---:|---:|---|
| 4096 / 128 / empty base | 2670 | 2957 | 859 bytes | All 37 | Both one stream, Completed |
| 4096 / 512 / 550 ASCII base bytes | 2854 | 3141 | 859 bytes | All 37 | Both one stream, Completed |

The 1536-byte initial cap chooses the existing names-only fallback for this
registry; it does not omit entries. Both requests retain exactly seven initial
schemas. The extra catalog test verifies that a 1536-byte first winner stays
unchanged under later 8192/512 calls and reopen, and that a larger default-8192
first winner stays unchanged under later 512 calls and reopen. Schema-call count
for these summary-only paths remains zero.

Changed ranges in those five files were formatted, and `git diff --check` passed.
Root exclusively owns view_image and finalized its formatting to raw SHA-256
`c71fd4a0ce65c9eed69c4f816d23495b3e1fda8faebebc00ee42c645e7dda87c`;
the earlier `82a671...` binding above is historical, with identical literal text.
No view_image edit was made during this budget correction. The current full
20-file raw source binding is `.cache/core-summary-20261128/budget-manifest.json`
(`budgetAfterSHA256`) and the budget GREEN receipt. The exclusive build lane was
returned to Root after copying that evidence. No stage, commit or push occurred.

| Budget-correction file | Validated raw SHA-256 |
|---|---|
| `MaiAgent/Agent/MaiAgent.cpp` | `b92b8f2753eea9fe50225b0fc21e6e4f075c0926cc5859c61c6dfc17264b1e36` |
| `MaiAgent/Agent/MaiModelToolCatalog.cpp` | `b94407202f3f6e19fe9aaf3181a2b1d0edd78e25f6ce32dd397311b35668a909` |
| `MaiAgent/include/MaiModelToolCatalog.h` | `585be6dc72f01a027523f5eda1b0942ff1a971236980e8ab7c050d39548b74b1` |
| `MaiAgent/tests/MaiModelToolCatalogTests.cpp` | `499dbb316dfdf526621579a5dcb7edcf4c9b4d281bb402247b6c83ebd310ea29` |
| `MaiAgent/tests/MaiToolDiscoveryRuntimeTests.cpp` | `73e322b1df6edf26756cbabd1db7189622ab3d7c95a8f10f8736c945fee1ea19` |


## Root final combined validation

Root independently reviewed catalog/runtime and all13 curated description files with no remaining
P1/P2 finding. A final wording correction specifies **User image attachments**, since ordinary
file attachments are references. clang-format split that same literal without changing its
concatenated bytes; the final view_image source hash is c71fd4a0ce65c9eed69c4f816d23495b3e1fda8faebebc00ee42c645e7dda87c.

After the adaptive-budget implementation, normal Windows unified build0 and **19/19** Core/Controller
suites passed (78.46s), including the required eight, catalog/discovery, journal/runtime facts,
unified exec, tools/edit/patch and Skill/runtime/installer. Receipts and full output are
`root-budget-final-19-{build,test}.log` and its receipt in the owned Core evidence directory.

Review caught a fixture terminology issue: the old two-field context-limit initializer set
a maximum output limit, not the actual input reserve. Root strengthened only the test fixture
to set the third reserve field and observe the real ContextStatus input budget. Final normal
wrapper **2/2** catalog/runtime targets pass with reserve128/input-budget3891 and
reserve512/input-budget3584; baseline/feature stream once and Complete in both cases.
The observed input-token pairs remain2670/2957 and2854/3141, with859-byte summaries containing
all37 deferred names. The callback merely copies event metadata; it performs no guarded store reads.
`root-reserve-proof-captured.log` preserves that exact output and the updated test source binding
is in `root-reserve-proof-receipt.json`. Production code did not change in this fixture correction.

Normal combined Windows App/CLI build and console e2e pass. The combined ordinary-file/media/text/
Header/receipt GUI regression is **39 passed /0 failed** (QPA windows, private profile,9552ms),
recorded in `combined-budget-final-composer/`. Android arm64-v8a normal JNI build exits0 (15.927s);
no Android installation or device runtime claim is made. No iOS runtime was available here.

Final normal package PE gate passes; staging and actual ZIP extraction each pass bare-PATH
--smoke with independent owned profiles and exact PID/exe/Qt-module checks. Both copy hashes
match App SHA256 **d0a7f87e39791b8d629bef996e36c5d34c8950cd8eac6a59823493b4b3888bd1**.
The final private receipt is `candidate-windows-final-02/receipt.json`. Earlier pre-budget GREEN
and candidate receipts are retained as historical evidence, not substituted for this final build.

Source verification binds all20 Core edits and the existing f0e65450 composer inputs to the
normal build lane. The lane retains a74470f39 checkout plus canonical checked overlays; it is
not represented as a clean release checkout. The final Git commit binds those same canonical
source bytes. Mac will consume that exact commit for normal App/Core validation; prior Mac
file-drop39/0 only validates the unchanged composer v2 source, not this new Core.

No published4.0.2 tag/assets, user installation, backend, HTTP2 classification or FFmpeg output
plumbing was modified. The old independent fullscreen failure remains tracked separately.
No provider/model/paid media request or real credential access was performed for this work.
