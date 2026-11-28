# Desktop Composer Local Files Integration Plan

> **For agentic workers:** Use superpowers:executing-plans to follow these integration checkpoints. Mac owns the three implementation files; Root reviews, verifies Windows, and commits on main. Do not create a branch/worktree or ask for another design approval: the task and implementation scope are already authorized.

**Goal:** Integrate local file draft cards and desktop drop handling without automatic submission, uploads, source deletion, or regressions to media/text inputs.

**Architecture:** Mac supplies exact before/post files and a narrow patch based on product23288. Root verifies the canonical Git base and all supplied hashes, reviews changes, then copies only three accepted posts into main and an existing isolated normal build lane. Core/IM/shared ComposerTextEdit remain unchanged.

**Tech Stack:** Existing Qt/C++17, Windows normal MSVC/Ninja lane, official Windows Qt test wrapper, owned fixtures and private profiles. No new dependencies.

## Checkpoint 1: Requirements and baseline

- [x] Review `docs/superpowers/specs/2026-11-28-desktop-composer-local-files-design.md` and the actual `+`, MIME, cleanup and Send/question paths.
- [x] Bind Root's three files to23288 using canonical LF Git blobs; preserve original CRLF bytes in `.cache/composer-file-drop-20261128/baseline/`. Other existing WIP is outside this change.
- [x] Verify first Mac ZIP712137B/SHA e3906f7c26d104ac8112b3733ea22e96c51db567c20ac2a58e7ff0a9e0ccbd52, manifest, before/post and actual patch application. Initial private byte gate encountered only Git CRLF output; normalized content matched every post. No product source was written.
- [x] Independent review identifies P2: nonvideo ordinary files invoke GUI-thread whole-file raster inspection before quota checking; default encoded budget128MiB. Return to Mac for bounded candidate detection and earlier quota checks before integration.

## Checkpoint 2: Review revised supply

- [x] Save and safely extract the revised supplied ZIP under `.cache/composer-file-drop-20261128/mac-delta-v2/`, after checking its supplied exact size/SHA. Reject traversal/symlink entries.
- [x] Verify all manifest entries, three before Git blobs, canonical Root bytes, and exact revised post bytes. Keep the original Mac RED, border failure/crash and picker fixture timeout as separate evidence.
- [x] Review the correction: quota and dedupe before content probing; at most4096B unbuffered file head for MIME candidate selection; supported suffix fallback; full original validation only for media candidates. Check unknown-extension PNG/JPEG and dimensions beyond the prefix. Prefix inspection failure must not silently reject a genuine media candidate.
- [x] Independent reviewer rechecks the revised classification and the original source-preservation, JSON, question/steer, draft and CopyAction boundaries. Resolve important defects before commit.

## Checkpoint 3: Integrate only verified posts

The integration helper checks an allowlist of the three paths, all before/post hashes, HEAD contents and real patch reconstruction in a private directory before writing product files:

```powershell
D:/Python314/python.exe .cache/composer-file-drop-20261128/integrate-reviewed.py .cache/composer-file-drop-20261128/mac-delta-v2
git diff --check
git diff --stat
```

- [x] Run this only after the revised review gate passes. Expect exactly `AgentChatPanel.cpp`, its header and `AgentPanelSessionTest.cpp` as product changes.
- [x] Verify shared ComposerTextEdit, Core, video, existing Header/receipt behavior and unrelated WIP were not overwritten. Preserve source files outside the integration allowlist.

## Checkpoint 4: Windows normal build and runtime

The prepared runner copies exact accepted inputs into the existing isolated lane, enters normal VS tooling, builds App and Qt test target, and invokes the official test wrapper with all slots declared in Mac's final receipt:

```powershell
D:/Python314/python.exe .cache/composer-file-drop-20261128/run-windows.py .cache/composer-file-drop-20261128/mac-delta-v2 windows-final-01
```

- [x] Require buildExit0 and testExit0; read actual QtTest output, not just wrapper success. Use QPA windows and an owned profile, never real accounts/models/files.
- [x] Cover editor frame/viewport, Copy/Move-only, actual file picker entry, ordinary and candidate media files, max count, missing-before-send, source preservation, question answers, same-turn explicit steer, cross-session drafts, literal text/media, Header and receipt regressions.
- [x] Inspect generated owned screenshots and captured model payload. Verify paths round-trip and document bodies are absent. Record the difference between injected MIME/widget verification and an external Explorer/Finder manual drag.

## Checkpoint 5: Candidate startup and delivery

- [x] After normal App deployment/PE gate, use the established private candidate packaging/smoke helpers in a fresh output directory. From staging and actual ZIP extraction run bare-PATH --smoke with distinct owned profiles and exact App/Qt/PID bindings. Never start a bare build executable before deployment checks.
- [x] Record Mac component/runtime evidence and Root Windows evidence, including P2 correction and all known limits, in `docs/diagnostics/`. Core/console/JNI tests are not repeated for unchanged code.
- [x] Bind every delivered product file's canonical bytes to the verified normal build input; check staged paths explicitly, commit feature plus records, push main, and verify remote SHA.
- [x] Give Mac final exact SHA and three-source hash/tree binding (Git/IM delivery record). If source bytes are unchanged from its validated v2, do not repeat successful builds only for a documentation/commit identity change.
- [x] No new formal release, tag/asset replacement, user App replacement, backend operation or fullscreen capture is included in this task.
