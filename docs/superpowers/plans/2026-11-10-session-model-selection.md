# Session Model Selection Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development. Develop on main. Root owns shared CMake and Desktop integration; Core/iOS/Mobile Adapter belong to the Mac peer. No new packaging, install or publication is authorized. Do not send ZIP archives or chunks; exchange exact patches and manifests.

**Goal:** Persist the selected main model and effort independently for each session, route four supported cloud models correctly, and use GLM native retrieval for GLM sessions.

**Architecture:** Reuse `MaiSession.model` and add a Core effort column via v10 migration. A shared routed client and cloud capability catalog supply real model/provider/wire behavior. Desktop and Mobile are thin adapters; remote changes use existing owner commands and wait for owner ACK.

**Tech Stack:** C++17/SQLite, Qt6, Swift, existing Mobile JSON/JNI, direct provider HTTP with loopback fixtures.

## Task 1: Preserve current evidence and establish RED

Files: `MaiChat/desktop/tests/AgentControllerTest.cpp`, `AgentPanelSessionTest.cpp`, `DesktopSessionSyncHostTest.cpp`.

- [x] Build the existing Controller/Panel tests in the normal Qt cache. Run `actionPanelMatchesMobileModelAndPolicyFlow` with private capture paths and bundled fonts; personally inspect the current menu.
- [x] Add Controller tests using existing APIs for session isolation, future-only defaults, SQLite restart and busy targets, plus four-model actual HTTP routing. The old implementation compiles and produces 14 real failures; preserve both runs.
- [ ] Add Panel tests for the four main model choices, selected values after switching sessions, and no change to the other session.
- [ ] Build `agent_panel_session_test desktop_session_sync_host_test`; run new Panel cases and the five new Host/RemotePanel cases against the unchanged implementation. Record behavior failures, not missing-interface compile errors.

## Task 2: Integrate the independently tested Core/Mobile implementation

Files held by Mac: Session/Store/Agent/TurnRunner/wire implementations and tests, Mobile Adapter, iOS; new sources below. Root changes only `MaiAgent/CMakeLists.txt` when complete source arrives.

- [ ] Verify exact base/new blobs and raw SHA for the frozen interface and implementation supply. Keep the old source bytes before applying the complete ABI/implementation cohort.
- [ ] Add these five source lines to the existing `maiagent` source list, with no SDK/flag changes:

```cmake
  Agent/MaiRoutedModelClient.cpp
  Agent/MaiCloudModelCatalog.cpp
  AgentTools/MaiGlmWebTools.cpp
  AgentTools/MaiProviderWebTools.cpp
  AgentTools/MaiWebRetrievalTransport.cpp
```

- [ ] Verify Session effort migration/restart, invalid atomic updates, busy isolation, captured model/effort through tool and compaction requests, and exact four-model protocol/effort matrices.
- [ ] Use `MaiToolContext.model` and model-specific tool specs for native provider retrieval; retain all original DeepSeek schema/TLS/cancel/retry assertions.

## Task 3: Desktop ordinary model configuration and routing

Files: `AgentController.h/.cpp`, `DesktopAccountModelClient.h/.cpp`, `AgentControllerTest.cpp`.

- [ ] Provide a `SessionModelConfiguration` query and an atomic `setSessionModelConfiguration(sessionId, model, optional effort)` ordinary setter. Preserve `setModel` as its thin wrapper.
- [ ] Split the immutable initial fallback from the default used only by future session creation. Seed new sessions explicitly; stop overwriting stored model/effort in `submitPrompt`.
- [ ] Delegate account routing to `makeMaiRoutedModelClient`, expose accurate per-model wire/capacity, and obtain only the selected provider's current Key. Do not cache another provider as fallback.
- [ ] Re-run the exact Controller RED cases; add the new effort and wire-query cases after the complete Core cohort is present.

## Task 4: Local and remote UI

Files: `AgentChatPanel.h/.cpp`, `MainWindow.h/.cpp`, `MainWindowLayoutTest.cpp`, `DesktopSessionSyncHost.h/.cpp`, `RemoteAgentPanel.h/.cpp`, and corresponding tests.

- [ ] Add the main model row and four choices inside the existing more-actions panel. Keep effort separate. Read values from the current session when opening/restoring a page.
- [ ] Bind pending Key refresh/selection to the captured session and account generation. Never apply an earlier request to the subsequently selected session. Persist first, then update labels; preserve drafts on failure.
- [ ] Persist effort through the same ordinary setter. Keep image gates tied to the target session's capability, including text-only GLM-5.3 and DeepSeek Pro.
- [ ] Implement owner `get_session`/`set_model` and include authoritative configuration in history responses. Validate owner tuple, request ID, four exact model IDs and busy state; never accept endpoint or Key fields.
- [ ] Add the corresponding RemotePanel row; apply confirmed model/effort only after matching ACK. Preserve confirmed configuration through catalog refresh, and reject old-login/session/late results.
- [ ] Re-run the original Panel/Host cases plus the new RED cases and inspect self-owned before/after screenshots.

## Task 5: Android thin adapter

Files: Android `AIAssistantActionMenu`, `AIAssistantPanel`, `AIAssistantController`, model settings/store and their existing tests.

- [ ] Use the frozen Mobile `set_model`/snapshot contract through the existing JSON JNI. Do not add search HTTP or JNI entry points.
- [ ] Bind outgoing queue and image checks to the target session; preserve local model configuration and failed-change rollback.
- [ ] Build/run available normal Android tests and report the actual connected-device boundary without claiming unperformed installation.

## Task 6: Final integration

- [ ] Run the affected normal Qt targets, at least eight Core suites including storage/model/turn/retrieval and the existing console e2e. Confirm dependency tracking and source/binary identity.
- [ ] Obtain Mac/iOS real consumer build/UI receipts and independently review supplied source and protocol evidence. Use only Fake/local services for UI verification.
- [ ] Record the exact final source, original failures, test results and actual platform/runtime boundaries in `docs/diagnostics/`; commit only the intended implementation/docs while preserving independent work.
