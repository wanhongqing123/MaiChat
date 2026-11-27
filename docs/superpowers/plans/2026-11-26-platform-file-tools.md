# Platform File Tools Implementation Plan

**Goal:** Preserve native file operations for iOS/Android without Shell, while desktop uses exec_command/write_stdin and apply_patch. Strengthen concrete file_edit failure and output boundaries without claiming that every possible file workflow is complete.

**Architecture:** Keep one shared C++ file implementation and the mobile adapter's existing explicit registration. Add an opt-out at builtin registration for the desktop host, preserving apply_patch. Desktop discovery and welcome cards must not retain removed file_* declarations; version the immutable catalog instead of rewriting old records. File editing remains platform independent, with strict validation and bounded responses.

**Tech Stack:** Existing C++17/native filesystem/JSON/Qt; no new dependency, branch, service, user database or installed-App replacement.

## Acceptance

- [x] Desktop available tools and discovery do not offer standalone file_* operations; apply_patch and exec_command/write_stdin remain. The welcome page describes the actual desktop path.
- [x] Shared/default builtin registration and the mobile adapter retain native file_read/glob/grep/edit/create/write/move/copy/delete/stat, without exposing an OS Shell on mobile.
- [x] Add failing tests for platform separation before registration changes. Preserve explicit custom-host testing of native file access/approval, rather than presenting those fixtures as desktop defaults.
- [x] Preserve old catalog records, initialize a new version so removed tools cannot remain pinned in loaded snapshots.
- [x] Reproduce and fix file_edit malformed parameters, cancellation, replacement growth and bounded UTF-8 response issues with original bytes checked on rejection.
- [x] Run normal shared tools/edit/search/reader/catalog tests plus desktop Controller/Panel/Starter targets, build the App and verify staging. State mobile real-device and POSIX runtime boundaries explicitly.

## Work ownership

Root owns platform registration, catalog, desktop tests/prompts and normal build. The existing shell-background worker handles the independent file_edit tests/implementation in a private build; it must not mutate or lock the normal archive. No commits or pushes are authorized by this task.

## User clarification: in-process command scripts

The user explicitly selected command-line syntax and requested an in-process simulated shell on mobile. Existing embedded curl/FFmpeg entry points are not such an interpreter; they remain separate tools until explicitly connected through their validated ordinary APIs.

- [x] Add a neutral, callback-based script parser/executor with complete syntax validation before side effects. Support quotes/variables, assignment, sequence/short-circuit, bounded pipes and redirects. Reject unsupported control flow/substitution/background syntax explicitly.
- [x] Add a shared native command host for ls/cat/cp/mv/mkdir/rm/touch/echo/head/tail/literal grep, reusing native filesystem/search/append cores. Keep App access boundaries and original bytes.
- [x] Register mobile-only exec_command as a thin synchronous adapter; do not expose process IDs, PTY or write_stdin. Keep specialized native file_* tools for advanced capabilities and document partial command semantics.
- [x] Verify ordinary core and model adapter on owned real files, errors/cancellation/budgets, and Android cross compilation. Mobile physical-device behavior remains unverified unless actually run.

The skill-catalog worker owns the neutral parser/header/tests. The Playwright worker owns the file command host/header/tests. Root owns thin tool/registration/build. No new third-party library is introduced, and no OS/thread termination is used as command completion.

## Explicit external-resource boundary

The user requires deletion only inside the App container. Existing photo/document originals retain no delete/move/overwrite entry. Existing import/export-to-workspace paths remain available; their copies are ordinary App files. This delivery does **not** claim external directory mounts are implemented.

- [ ] A subsequent platform integration must add real iOS directory security-scope/bookmark and Android SAF tree-read grant backends before ls/cat/copy-in can address an external authorized directory directly. External originals must not receive delete/move/overwrite permissions. This is separate from the completed container command subset.
