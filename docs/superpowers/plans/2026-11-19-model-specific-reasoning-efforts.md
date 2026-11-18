# Model-specific reasoning efforts Implementation Plan

> **For agentic workers:** Execute the existing authorized increment in parallel
> file leases; Root serializes native builds and commits on main.

**Goal:** Expose all five Fable efforts without sending its extra values to GLM,
DeepSeek or local/custom routes that retain the existing three-value contract.

**Architecture:** Ordinary catalog helpers drive Core, CAPI and Qt behavior.
Platform hosts adapt menus and remote tuple validation, without new structs.

**Tech Stack:** Existing C++17, Qt, Java/Android and Swift/iOS; no dependencies.

- [x] Inspect official model record and current hard-coded validators/menus.
- [x] Freeze helper signatures, inheritance, explicit rejection and switch rules.
- [x] Protocol tests first: `MaiModelClientTests --reasoning-efforts` RED.
- [x] Catalog: modify `MaiCloudModelCatalog.h/.cpp`; retain original single-value
  API and add model-specific list/validation/known-level normalization.
- [x] Core: `MaiAgent.cpp`, `MaiTurnRunner.cpp`, `MaiAgent.h`; existing Core/Runner
  tests cover Fable choices, explicit invalid pairs, model-only atomic high,
  busy isolation and persisted values after reopen.
- [x] Wire: `MaiResponsesClient.cpp` and `MaiModelClientTests.cpp`; strict rejection
  before HTTP and unchanged Fable medium/xhigh payloads. Run the same RED selector.
- [x] CAPI: `MaiMobileAgent.cpp` and `MaiMobileSessionModelTests.cpp`; verify normal
  configure/create/get/set/snapshot and supported list through the existing C ABI.
- [x] Qt/Android: model-specific controls, effective labels and ACK validation;
  use existing controller/panel/session fixtures and Java tests before fixes.
- [x] Apple: consume exact shared delta; native medium/xhigh host fixtures and
  normal consumers, without repeating common protocol or paid provider requests.
- [x] Root: run targeted GREEN and related common regression; normal Windows
  runtime deployment gate and Android ABI build; review and aggregate exact source.
- [x] Record tested source/build/device boundaries, commit and push main; a new
  device installation follows the existing authorized Apple delivery workflow.

Additional authorized checks:

- [x] Product retrieval: existing GLM Search/Reader registration in
  `AgentController.cpp` and `MaiMobileAgent.cpp`; all primary models use its
  declared GLM schema, ordinary implementation and GLM-only key callback.
  `productGlmWebToolsStayIndependentOfPrimaryModel` must fail on the prior product
  and pass without real credentials/network after the registration change.
- [x] Future 402 diagnostics: `MaiResponsesClient.cpp` and
  `MaiModelClientTests --openrouter-402`; synthetic RED then GREEN for safe
  metadata, bounded numeric Retry-After, supplier isolation and zero automatic
  retries. Preserve output-budget behavior and unknown-error truthfulness.

- [x] Verify normal iOS 230 Release/signature/install and independent device version.
- [ ] Verify ordinary launch and independent PID after the device is unlocked;
  the first launch was explicitly Locked and no new-bundle process was observed.
