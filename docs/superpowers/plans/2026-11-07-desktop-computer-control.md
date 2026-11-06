# Desktop Computer Control Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Root owns all builds and native GUI runs; edit directly on main as required by the repository.

**Goal:** Let the Creation Center Agent discover a PC window, operate real keyboard/mouse input, and read back the result through the product tool path.

**Architecture:** Extend the existing DesktopComputerController and platform input functions. Ordinary C++ and model tools share one dispatcher. Window IDs, fresh observations, focused-window checks, cancellation and truthful receipts remain explicit; browser automation and remote transport reuse this execution boundary in subsequent work.

**Tech Stack:** C++17, Qt 5, Windows SendInput/UIA/WGC, macOS CGEvent/AX/ScreenCaptureKit, existing MaiAgent tool and approval pipeline.

## Reproduction and expected result

Use a disposable editor owned by the acceptance fixture and a real Creation Center panel/session. Discover its window, observe by ID, focus and click the edit area, enter Chinese Unicode text, send Select All, replace text, press Enter, and verify actual focused text. Double click, right click, drag and scroll use the same observed-window boundary. Duplicate titles, stale snapshots, cancellation and foreground refusal must produce explicit failures and never success receipts.

The reported CR loop used clear as text deletion; clear actually forgets the observation. Preserve that meaning and provide replace_text (including an empty replacement) plus key chords. No real CR, account credentials or paid model requests are needed for acceptance.

## Tasks

- [x] Add and run a failing desktop computer tool contract test for list/focus/double_click/right_click/drag/replace_text/verify and key chords.
  - Files: desktop/tests/DesktopComputerContractsTest.cpp, desktop/CMakeLists.txt.
  - Command: run-tests-windows.ps1 -BuildDirectory .cache/build-release-62111144 -TestRegex '^desktop_computer_contracts$'.
- [x] Add native input dispatch, shared key parsing and focused-text reading.
  - Files: desktop/src/MaiChatTools/DesktopComputerPlatform.h, DesktopComputerKeys.h/.cpp, DesktopComputerWindows.cpp, DesktopComputerMac.mm.
  - Keep legacy free functions as adapters. Track only synthetic held inputs; release them on every exit. Check cancellation and focus while a multi-event action is running.
- [x] Extend native screenshot capture with WindowById, preserving existing display/title behavior.
  - Files: MaiAgent/include/MaiScreenshot.h and AgentTools/MaiScreenshot{Windows.cpp,Mac.mm}; tests/MaiScreenshotToolTests.cpp.
  - Test zero/invalid IDs, duplicate titles and a selected disposable window. Never fall back to display capture for an invalid ID.
- [x] Extend the ordinary controller and existing model tool.
  - Files: desktop/src/MaiChatTools/DesktopComputerTools.h/.cpp and new DesktopComputerBackend.h.
  - Support window discovery/ID selection, dry-run, fresh-snapshot dispatch and focused-text verification. An injectable backend exercises the same controller without global input in deterministic tests.
  - Preserve clear as observation reset. Return dispatched/verified separately; a successful injection is not business persistence.
- [ ] Complete deterministic controller tests and product-path acceptance.
  - Files: desktop/tests/DesktopComputerContractsTest.cpp, DesktopComputerToolsTest.cpp and AgentPanelSessionTest.cpp as appropriate.
  - Exercise ordinary APIs and tool JSON, stale/consumed tokens, duplicate windows, wrong focus, secure inputs, empty replacement, Unicode/chords, mismatch and cancellation.
  - Use the real Creation Center/controller/tool/result loop with a scripted model and disposable editor process; no paid or provider request.
  - Deterministic contracts and the real Agent/UI loop with an injected editor backend pass. The separate real-native Creation Center fixture is implemented; actual input acceptance remains pending Windows foreground preparation and an unlocked Mac desktop.
- [ ] Review, build and deliver evidence.
  - Run normal complete desktop build, deployed-runtime PE checks, focused tests and relevant Agent/SessionSync/MainWindow regressions.
  - Windows build/runtime PE, ten Core suites, five affected desktop suites and two deterministic computer suites pass. Three Windows native suites explicitly skip before input; Mac native discovery fails in the locked session. Keep these boundaries visible until the same native cases run with the required desktop conditions.
  - Core screenshot inputs changed: execute required actual Core gates at the final source. Ask the Mac collaborator to compile and run affected native cases; keep unavailable native conditions explicit.
  - Inspect actual isolated UI screenshots. Record source, binaries, process ownership and remaining boundaries in docs/diagnostics/.
  - Commit/push on main. Any private export must pass staging/actual ZIP bare-PATH smoke, PE closure and packaged models. Published v2.2.52 and installed apps remain unchanged.

## Boundaries

The first implementation targets local PC operation through Creation Center. It does not claim a new independently deployed remote input service, production SessionSync deployment, existing-browser attach, or access to locked/secure desktops. Existing remote Session commands must execute tools on their owner; expanding transport or browser attach requires its own end-to-end evidence.
