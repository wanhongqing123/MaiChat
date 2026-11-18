# Cloud Responses / OpenRouter implementation plan

The user has authorized the provider, existing cloud authorization scope and
all-cloud Responses protocol. Routine implementation follows the approved
existing architecture; do not request those approvals again.

- [x] Verify official GLM/OpenRouter endpoint, completion and reasoning contracts.
- [x] Freeze provider/key/config field contract with Apple owner.
- [ ] Root: add plain continuation DTO/header fields and cloud catalog metadata.
- [ ] Protocol owner: failing synthetic Responses replay/no-DONE/error fixtures;
  implement exact ordered output replay and terminal state publication.
- [ ] Root: failing storage/context/compaction/runner fixtures; persist and budget
  state, preserve response/tool groups and backward-compatible old records.
- [ ] Service owner: failing fake-key compatibility fixtures; add optional
  OpenRouter credential field and bounded four-provider response handling.
- [ ] Host owner: Windows/Android routes, current-account key parsing, five-model
  menus and existing session/default/busy semantics; use Responses for all cloud.
- [ ] Apple owner: iOS/Mac hosts and Apple consumer tests from exact shared source.
- [ ] Root: common Windows Core regression and normal product build; Android
  platform-only validation; independent review and exact source handoffs.
- [ ] Prepare concrete service deployment and rollback materials. Mac operator
  applies the user's key in the existing restricted service configuration only.
- [ ] Aggregate exact platform deltas, commit/push main and report tested limits.

All CMake/Ninja and Windows GUI validation is serialized by Root. Node-only
service fixtures may run independently. No new dependency or MCP integration.
Do not change installed apps, release assets or defaults without their task's
existing authorization; do not log credentials or real continuation payloads.
