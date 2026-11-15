# Bound Session waiting-state startup

## Report and reproduction

The user reported that input received through a bound friend takes much longer
to display the Session's waiting/thinking text than input sent directly from the
Creation composer. Both routes should display the same waiting state once Core
accepts a new turn, before the first model chunk. Input still awaiting history,
attachment preparation or admission is not falsely labeled a running turn.

Use the normal Windows Qt target and an owned IM/model fixture. Bind the friend,
open its Session panel, send the same short prompt through the local composer or
the real bridge ingress, and hold the model before it emits any text/reasoning.
The model-entered semaphore proves the request has reached model execution.
Check the visible `agentThinkingCard`, Stop button, draft, and eventual cleanup.

Baseline source is Desktop 2.2.63 `d9736686`; current main `618811fc` adds only
release documentation. Decisive RED (`red-02.txt`): local input immediately shows
waiting/Stop; bound input has reached the gated model but neither UI state is
visible after 305 ms. Three cases pass, one fails, including Qt init/cleanup.
An earlier fixture attempt consumed a semaphore in a repeatedly evaluated QTRY
predicate; its failure was harness-only and is preserved separately.

## Cause and narrow change

`AgentChatPanel::onSend` directly calls `setRunning(true)` and
`startPendingThinking` after local acceptance. The bound bridge submits directly
to Core. Its user-message event appends the user bubble but does not execute
`onSend`. Core previously did not publish a start-status event, so this route
could not start the same waiting UI before later output/history reconciliation.

Core now publishes existing `SessionStatus` with `detail=running` and the exact
initial turn ID after input/assistant persistence and active-turn registration,
outside its runtime lock and before starting the worker. No event enum value or
payload layout changes. The controller delivers `turnStarted` on the GUI thread.
The panel checks both current Session and exact current active turn before using
its existing waiting widget. Local immediate feedback remains and deduplicates
against the shared notification. Reasoning content and message forwarding are
unchanged; stale completed/replaced turns cannot re-open waiting state.

## Verification status

Initial same-case GREEN: local 1 ms, bound 9 ms, both while first model output is
still withheld. Both show Stop, and the remote route preserves an unsent local
draft. The owned screenshot was viewed and confirms these visible states. This
measures local acceptance/UI behavior, not end-to-end IM network latency.

Normal final source/App/Bridge/Panel and Core consumers were rebuilt, including
the post-build gate for three runtime binaries, 114 PE imports and Qt plugins.
The new fixture also checks start notification identity, registration before
model execution, background Session isolation, switch/back restoration, Stop,
and queued start after the owned turn has already finished. Full Bridge passed
68/0/0, focused Panel passed 12/0/0, and all twelve ordinary Core suites plus
console validation exited zero. The newly built normal App passed a separate
bare-PATH/private-profile smoke, actual PID 43932, executable SHA-256
`1a887c3050885e4de9a651eccab1e7f558191c9b95eb21724a6bfb9c1a3a3ec6`.

Final evidence is under `.cache/session-thinking-start-20261115/`, including
`red-02.txt`, `bridge-full.txt`, `panel-focused.txt`,
`validation-receipt.json`, `app-smoke-receipt.json`, and the viewed
`bound-thinking-final.png`. The source8 review patch is 15185 bytes, SHA-256
`567b4a0d07514281e74cc1d7075b605405271fa44b522a5d7a1ad80a0f7b776f`.
It contains seven production files and one regression file; it does not include
the independently owned Mac media work. No installed user App has been stopped
or replaced, and the already published 2.2.63 assets remain unchanged. This is a
verified source candidate, not a claim that the installed 2.2.63 has been updated.
