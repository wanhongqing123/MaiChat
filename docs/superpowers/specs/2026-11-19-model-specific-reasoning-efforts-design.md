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
