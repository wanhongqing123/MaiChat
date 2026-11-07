# Native Screenshot Handoff Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development. Work directly on main. Root owns builds and native UI runs; preserve installed apps and user data.

**Goal:** Let a Creation Center Agent use native screenshot tools, inspect their image result, and display the same saved file without writing a shell screenshot helper.

**Architecture:** Preserve the existing WGC/ScreenCaptureKit and tool-image request pipeline. Save screenshot files inside the session workspace and expose a relative image path usable by agent_send_media. Align screenshot model availability with the existing image-view tool. Give desktop models a short native capture/delivery instruction after the result contract works.

**Tech Stack:** C++17, MaiAgent filesystem/path APIs, existing Chat Completions/Responses clients, Qt desktop AgentController and deterministic tests.

## Reproduction and expected result

Create an isolated test window. Request its screenshot with the real screenshot tool, inspect the returned PNG, and give the exact returned workspace-relative path to agent_send_media. Both tools must reference the same file. The next image-capable model request must contain actual image bytes. No shell command, paid provider, full user-screen capture or app replacement is needed for the automated acceptance.

Current source saves screenshot to the system temporary directory and returns only dimensions in text. ContextBuilder attaches pixels but does not expose the path. agent_send_media rejects absolute/out-of-workspace files. desktop_computer already saves in the workspace but also omits its image path. These are confirmed contract gaps; the user's current model/runtime and any provider-specific image failure remain unconfirmed.

## Tasks

- [x] Add failing screenshot contracts for model availability, workspace-contained relative image path and same-file media delivery. Add an owned-window-only test entry to avoid capturing user display content.
- [x] Save screenshot under the session workspace using MaiFilePath/MaiFileSystem and existing path containment checks; expose path/type/dimensions while preserving image attachments. Reject unsupported image models consistently before capture.
- [x] Add a failing desktop_computer image_path contract, then expose its existing workspace screenshot path. For include_image=false return no usable image path.
- [x] Add combined real image-tool/Agent-loop/local HTTP body coverage with a valid PNG for both supported wire protocols. Verify actual decoded bytes and tool/result/image ordering, not just a filename.
- [x] Add concise desktop native screenshot/delivery guidance and a product-path regression that uses the returned file without shell.
- [x] Run focused RED/GREEN, normal complete desktop build, required Core gates and affected UI tests. Request Mac compile/no-injection checks; preserve locked-desktop/native-input boundaries.
- [x] Record exact evidence and remaining model/runtime questions, commit and push. Any private package must pass staging/actual-ZIP bare-PATH smoke, PE and model checks; do not install or publish it.

## Boundaries

Capturing an image, delivering a file to the user, placing image bytes in an HTTP request, and a live provider understanding pixels are separate observations. Do not claim paid/live visual comprehension from a fake server test. Do not bypass model capability, capture permission or foreground restrictions with shell scripts.

## Follow-up scope

This completed correctness stage is followed by the user-requested resident screen-capture worker and screen-only model tool surface. Its owned-window fixtures do not claim that follow-up has been implemented. No stage-A private package is exported before the follow-up.
