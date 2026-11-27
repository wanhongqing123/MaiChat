# Mobile Shell-only File Tools Plan

**Goal:** Implement the user's revised single model entry for mobile file operations: in-process exec_command plus apply_patch, with no separately exposed file_* tools.

**Architecture:** Keep native file algorithms and data limits in shared C++; the mobile host removes file_* registrations. Add command-line find/stat/rmdir/read-text and robust grep using existing search/reader cores. Share bounded cursor stores across tool calls with host-owned session/root bindings. Preserve App-only deletion and existing read/import of authorized external resources; external directory mounts remain a separately documented missing platform integration.

**Constraints:** No third-party addition, OS shell, process, PTY or thread exit. No user database/device installation or replacement. Work directly on main and preserve existing WIP.

- [x] Update actual mobile model-request tests to require exec_command/apply_patch and reject all file_* declarations; remove mobile registration and stale prompt guidance. Full loopback execution remains a separate verification boundary below.
- [x] Add CLI search/state/text/empty-directory operations, reuse ordinary cores and verify real files, regex errors, cancellation, encodings, long-line continuation and scope rejection.
- [x] Persist search/text cursor services in the thin tool, bound by session and access root; no serialized untrusted cursor internals or repeated body/schema injection.
- [x] Run shared normal tests and Android core/adapter syntax checks; record the actual private mobile linking failure rather than claim loopback/device success.
- [x] Update current documentation and record the original failures and verification boundary. Do not rewrite earlier historical delivery records or claim external mounts/full Bash support.
- [x] Run the scoped mobile loopback contract and command/device targets on the connected ARM64 phone using rebuilt current Core/Mobile/CLI, without stubs or new platform macros. Five final native targets and six focused App-private UID checks passed. This does not claim the entire old mobile suite or normal MaiChat UI/provider flow passed; see the 2026-11-27 physical validation record.

The native-file worker owns MaiFileCommands.{h,cpp} and its tests. The script worker owns the mobile adapter and its integration tests. Root owns cursor cache, thin tool, build registration and verification.
