# Sky Computer API Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development to implement the independent native components, then integrate and review the actual contracts.

**Goal:** Implement the 13 methods in the installed OpenAI Windows sky window2 API using MaiChat's existing ordinary C++ desktop core and one computer model-tool entry.

**Architecture:** Keep the existing native capture/input implementation, session ownership, cancellation and evidence checks. Add typed application and accessibility facts, a Sky-compatible ordinary facade and a thin model adapter. Existing low-level input capabilities remain ordinary core capabilities; no new MCP, JavaScript runtime, Agent process, third-party library or shell-based UI automation.

**Tech Stack:** C++17, existing Qt host, Windows COM/UI Automation/Shell and macOS AppKit/Accessibility; existing Core/Qt test infrastructure.

## Approved contract

The user's previous interface comparison and instruction authorize all 13 methods: list_windows, get_window, list_apps, launch_app, get_window_state, click, press_key, type_text, scroll, set_value, drag, perform_secondary_action, activate_window. `target=windows` is a property, not a method. Canonical model requests use these method names through computer; do not claim Responses native computer_call support.

Window objects carry app/id/title. get_window_state defaults to screenshot=true/text=false; explicitly text-only observations must not capture pixels. Return window/accessibility/screenshots; include actual focused/selected/document information only when supplied by the native provider. Screenshot IDs and indexed elements belong to the current owner/window observation. Coordinates are window-relative, never guessed full-screen ratios. A failed or interrupted action does not prove no input happened.

## Task 1: Native facts and application operations

- [x] Extend DesktopComputerPlatform.h and DesktopComputerBackend.h with typed application metadata, optional accessibility context, element secondary actions and ordinary value/action entry points.
- [x] Implement list/launch in new DesktopComputerAppsWindows.cpp and DesktopComputerAppsMac.mm. Enumerate native installed-app sources, associate real running windows, use native launch APIs without a command shell. Validate inputs/cancellation and preserve unknown optional usage metadata.
- [x] Add bounded native UIA/AX focus/selection/document facts and element set-value/secondary actions. Re-resolve the observed native reference and reject disabled, stale, password or unsupported elements.
- [x] Add X keysym aliases needed by Sky without changing existing physical-key identity.

## Task 2: One ordinary Sky facade and model contract

- [x] Implement list_windows/get_window/list_apps/launch_app and get_window_state through the same controller/backend; no independent native backend.
- [x] Map latest-observation indexed elements and screenshot IDs to the existing strict evidence/token core. Convert window-relative coordinates using actual native bounds and return explicit unit facts.
- [x] Implement click count/buttons, key/type, two-axis signed scroll, drag, set_value, secondary action and activation. Serialize operations, release owned transient input on interruption, and retain truthful attempt/failure facts.
- [x] Publish the 13 names in computer schema/description; update prompts, capability cards, discovery snapshot migration and affected tests. Preserve old transcript facts and do not retain misleading active aliases.
- [x] Preserve advanced operations in computer_input using the same controller/native_input_binding; no duplicate basic Sky methods.

## Task 3: Validation and delivery evidence

- [x] Write isolated fake-backend contract tests for all 13 methods before completing the adapter: defaults, exact parameter validation, image/text selection, stale IDs, app/window mismatch, coordinate/scroll mapping, unsupported native operations, cancellation and permission boundaries.
- [x] Run RED against old behavior, then GREEN on the implementation. Test application parsing independently with owned data.
- [x] Build the normal Windows App and ordinary test targets from the synchronized source manifest. Run core catalog/rollout/model regressions and desktop controller/tool contracts appropriate to the changed ABI.
- [x] Native owned UIA fixture verified text/value/secondary actions (26 checks); recording fixtures verified actual PNG model handoff. No user screen capture, real app launch or native keyboard/pointer run; macOS compilation/runtime explicitly unverified.
- [x] Record exact source/normal binaries, checks and limits in docs/diagnostics. Do not commit, push, publish, install or stop the user's current App as part of this task.
- [x] Deliver a separately named Windows portable directory/ZIP and verify all three copies with bare PATH; receipts in .cache/sky-computer-20261126/local-handoff.json.

## Research boundary

The installed plugin is proprietary; its documented methods are the reference contract, not source available for copying. The docs do not establish whether all native capture streams persist between observations. Preserve current capture lifecycle during this API alignment; do not hide its status bar. Native scroll deltas must retain documented sign and axis, but do not invent evidence that OS-dependent physical scroll distance matches an unobserved proprietary implementation.
