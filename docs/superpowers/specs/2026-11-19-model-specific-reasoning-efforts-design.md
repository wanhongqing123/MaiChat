# Model-specific reasoning efforts

The user requested the complete Fable 5.2 thinking controls after iOS 229 delivery.
Fable supports low/medium/high/xhigh/max with mandatory reasoning; the other four
cloud models retain low/high/max. Preserve the default model, credentials, server
environment and ordinary direct-provider architecture. No new provider request,
library, MCP integration, struct field or data migration is needed.

Add three ordinary catalog functions, with `(model, effort)` argument order for
the latter two: `maiReasoningEffortsForModel`,
`maiIsValidReasoningEffortForModel`, `maiNormalizeReasoningEffortForModel`.
The original one-argument validator keeps its three-value contract. Empty effort
still means inherit. Normalize only the known Fable-only medium/xhigh to high
for another model; leave empty and arbitrary invalid strings unchanged so callers
can reject invalid input rather than silently repairing it.

Explicit create/set pairs must be legal for the effective model before mutation
or HTTP. A model-only change atomically converts inherited/stored medium/xhigh
to high when changing to a model without those levels. Existing old three-level
sessions, default selection, busy and ownership rules remain. Pure reads do not
write records. Effective values and request construction normalize recognized
inherited values for the actual model, while garbage remains InvalidInput.

Qt/Android/iOS menus are model-specific. Labels are low=低, medium=中, high=高,
xhigh=超高, max=最高. Model switching builds a legal tuple before remote ACK;
explicit unsupported ACK tuples are rejected. CAPI session choices add the JSON
array `supportedReasoningEfforts` and expose a model-normalized inherited default;
no C ABI structure layout changes. Stored empty effort retains inheritance.

Root owns catalog/CAPI and all common Windows builds. Protocol worker owns wire
fixtures/implementation; Core worker owns session/runner; host worker owns Qt and
Android; Mac owns iOS and Apple consumer builds. Only Root commits/pushes main.
Use existing local HTTP fixtures and synthetic keys. First reproduce Fable
medium/xhigh being dropped and unsupported other-model values reaching HTTP,
then verify fixed requests, atomic switching, persistence, menus and ACKs.

Primary source: https://openrouter.ai/anthropic/claude-fable-5.2 and its public
model record (`reasoning.mandatory=true`, five `supported_efforts`). The original
three-level integration is retained as the baseline, not described as five-level.

## Subsequent user steering: retrieval and 402 diagnosis

The user also needs ordinary keyword search and URL正文 retrieval while Fable is
the primary model. In the two MaiChat product registration points, use the
existing `registerMaiGlmWebTools` ordinary implementation directly. The tool
schema/descriptions/results explicitly identify GLM, and its search limit stays
70 Unicode characters. Its independent default endpoint remains
`https://open.bigmodel.cn/api/paas/v4`, not the primary-model `/api/v1` endpoint.
Do not select DeepSeek as fallback. The lower-level native-provider wrapper keeps
its existing strict contract; the product no longer uses that wrapper.

The reported OpenRouter 402 cannot be reconstructed from historical metadata:
the previous adapter kept only the supplier message. A free key-info check with
remaining key capacity does not prove wallet or in-flight capacity. The current
App does not explicitly send 128000; its output limit is omitted, and 128000 only
appears in the local context reserve. Do not silently change a product output
limit based on that incorrect assumption.

For future failures retain the original message plus allowlisted OpenRouter
reason/source classification and bounded numeric Retry-After seconds. Known
in-flight exhaustion is RateLimited; a single request exceeding the budget is
distinguished from transient occupancy. Missing/mismatched metadata remains
unclassified, and other suppliers do not inherit OpenRouter semantics. Do not
copy arbitrary metadata/headers, add retries, change keys/credits, or call a paid
model to diagnose this. Reference:
https://openrouter.ai/docs/api_reference/limits#in-flight-spending-budget .
