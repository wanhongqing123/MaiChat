# Cloud Responses and OpenRouter Fable 5.2

User-authorized scope: add OpenRouter Claude Fable 5.2, use the existing cloud
model credential authorization scope, and move all product cloud models to
Responses. Preserve the default model and account/session ownership. No MCP,
new third-party library, model proxy, or additional execution core.

## Routes and host contract

- `glm`: `glm-5.3` and `glm-5.3-flash`, Responses,
  `https://open.bigmodel.cn/api/v1`.
- `deepseek`: `deepseek-flash` and `deepseek-v4-pro`, Responses,
  `https://api.deepseek.com`.
- `openrouter`: `anthropic/claude-fable-5.2`, Responses,
  `https://openrouter.ai/api/v1`; exact limits 1,000,000/128,000 tokens,
  images and function tools, always-on reasoning supporting low/high/max.
- Mobile `modelProvider` stays `remote`; configuration adds
  `openrouterBaseUrl` beside `glmBaseUrl` and `deepseekBaseUrl`.
- Credential response adds optional string `api_keys.openrouter`; absence
  means empty. Preserve the three old string fields and credentials. Never
  use another provider's key as fallback. The native host callback is
  `openrouter_api_key` with the same key-result shape as existing callbacks.
- Provider keys remain in the existing restricted credential service; the
  authenticated account control plane distributes them under its existing
  authorization. Clients call providers directly. The supplied key remains
  with the Mac operator and is not copied to source, tests, logs or Root.

Existing generic Chat library support is not an implicit cloud fallback.
GLM cloud migration changes both wire and base URL. Official GLM Responses
ends on completed/failed/incomplete/error without a `[DONE]` sentinel; low
and medium efforts are interpreted as high by that provider. Model image
capabilities remain independently verified; a generic API example does not
enable vision for every GLM model.

## Response continuation

Reuse `MaiResponsesClient` and existing context/store/turn infrastructure.
A plain data `MaiModelContinuationState` has `format`, `provider`, `model`
and opaque `payload` strings. Format is `responses.output.v1`; payload is
the complete ordered successful response output array. Public headers have
no JSON type or provider SDK. Add optional state at the end of model messages
and reasoning parts, and append an `onContinuationState` sink callback.

Visible reasoning text stays separate. Preserve original output item IDs,
call IDs, order, encrypted content, signatures and unknown metadata values.
On matching provider/model replay the complete output instead of rebuilding
the same assistant text and calls; append tool results in their existing
call-ID order. Do not derive signed state from displayed reasoning, truncate
state, duplicate its text/calls, or replay it into a different provider/model.
Malformed matching-route state is a protocol error. Old records without state
retain their existing behavior.

Validate terminal success, the full output and every tool call before publishing
one complete state and then tool calls. Canceled/failed/incomplete/unterminated
responses do not publish replayable state or execute generated tools. Snapshot
reasoning text can complete earlier deltas without duplicate display.

Each response has its own reasoning-part state carrier, including encrypted-only,
empty-text and no-tool responses. Persist metadata-only updates explicitly.
SQLite uses additive continuation columns with empty old-record defaults;
source fingerprints include present state while keeping old no-state hashes.
Context boundaries and compaction preserve one response and its entire tool
batch/results together. Input budgeting counts opaque replay data and avoids
double-counting projections replaced by it.

OpenRouter is stateless: send full effective history with `store=false`, no
`previous_response_id`. GLM also uses the existing client-owned history rather
than enabling its optional server-side storage.

## Verification and responsibilities

Root owns shared fields/catalog/context/store, common Windows validation,
services and Win/Android integration. Protocol worker owns Responses/Chat wire
adapters and protocol fixtures. Mac owns iOS/Mac hosts, Apple consumer builds,
the sole bounded real OpenRouter probe and credential-bearing deployment.
Only Root stages/commits/pushes main; all workers preserve existing WIP.

Use synthetic local HTTP/SSE and fake credentials for common tests: provider
matrix, no-DONE GLM completion, output replay/signatures, interleaved calls,
partial/error/cancel, metadata-only persistence/reopen, compaction boundaries,
account changes/key refresh, old credential responses and all-empty failures.
Run existing Core regression suites and normal product consumer compilation.
Mac platform tests cover only Apple-specific integration; Android covers its
native configuration/menus/ABI rather than repeating common protocol tests.

Official references:

- https://docs.bigmodel.cn/cn/guide/develop/responses/introduction
- https://openrouter.ai/docs/api_reference/responses/overview
- https://openrouter.ai/docs/guides/best-practices/reasoning-tokens
- https://openrouter.ai/anthropic/claude-fable-5.2/

Mac real probe: two synthetic Responses requests completed HTTP 200 and an
echo tool round-trip, with no user input or saved response secrets. No reasoning
item was actually returned, so signed-state behavior still requires adversarial
synthetic fixtures. This is not a completed application or deployment acceptance.
