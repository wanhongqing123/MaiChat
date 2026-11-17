# CloudDrive 20GB / 100MB Implementation Plan

> **For agentic workers:** Use subagent-driven development with the frozen spec
> and file leases below. Work directly on main and preserve all existing WIP.

**Goal:** Complete decimal 20GB video/100MB image cloud upload, resume, read and
preview, with safe image thumbnails and unchanged Session/IM/temp policy.

**Architecture:** Extend the existing shared multipart core with an optional
caller-owned JPEG source. The account service binds a hidden thumbnail object
to the original and uses the same signed OSS part protocol and cleanup rules.

**Tech Stack:** Existing C++/curl/image helpers, Node/SQLite/ali-oss, Qt, JNI/Java.
No new library, service proxy, bucket or third-party Git.

## Task 1: Service policy and associated thumbnail — services agent

Files: `services/auth/{cloud-drive-service.js,server.js,store.js}` and related
cloud/session tests; `services/oss-media-signer/{index.js,server.js}` and tests;
the two service README files.

- [x] Write failing boundary tests for video 20,000,000,000 / +1, image
  100,000,000 / +1, unchanged Session 2GB/20MB and unchanged quota configuration.
- [x] Run focused Node tests and preserve the expected old-policy failures.
- [x] Split cloud policy from Session options; make quota/per-file ceilings
  independent while keeping transactional available-capacity enforcement.
- [x] Test and raise the internal completion body to 1MiB in both adapters;
  reject excessive parts and preserve fixed length/MD5 binding.
- [x] Add thumbnail metadata/migration and the exact routes in the spec. Tests
  cover hidden listing, combined reservation, thumbnail-first completion,
  unsupported target/ownership, lost init, cancel, delete/late-complete and
  partial cleanup failure. Only write implementation after each failure is seen.
- [x] Run service suites once on house and provide reviewable deployment,
  migration and rollback material. House supplies the frozen candidate;
  mac owns the authorized existing-identity production transport.

## Task 2: Shared C++ protocol and limits — Root

Files: `MaiChat/Network/MaiCloudDriveClient.{h,cpp}`, `MaiCloudVod.{h,cpp}`,
network tests/local TLS fixture, `MaiChatTools/MaiCloudDriveTools.cpp` and tests.

- [x] Add old-policy failing tests, including Session isolation and large Range
  totals. Run through the Windows test wrapper/normal build environment.
- [x] Append the thumbnail path option and publish exact header/manifest to mac.
- [x] Separate cloud versus Session constants and update staging, checkpoints,
  read/move/download/VOD validation and relevant contracts/descriptions.
- [x] Extend checkpoint and shared list/sign/PUT/complete helpers with the
  thumbnail resource suffix. Validate local JPEG size/fingerprint, match init
  geometry, upload thumbnail first, resume both resources and keep cleanup/receipt
  behavior. Do not put upload logic into any host adapter.
- [x] Run local TLS/fake service cases for complete bodies and both multipart
  resources; test unchanged old checkpoints and source/ownership mismatch.

## Task 3: Windows UI and ordinary thumbnail helper — Windows agent

Files: `desktop/src/ui/CloudDriveDialog.cpp`,
`desktop/src/MaiChatTools/DesktopCloudDriveTools.cpp`, a focused ordinary
desktop thumbnail helper/CMake entry if required, and existing related tests.

- [x] Check the real UI hint and retain ordinary/Agent interaction checks.
  The persistent label was already absent on the Windows baseline; the
  reproduced Windows defect was the obsolete size tooltip.
- [x] Keep the hint absent, retain concise reserved-quota rendering, and derive
  specific limit errors from shared media information.
- [x] Use an ordinary shared host helper for UI and Agent to create an atomic,
  bounded 320-pixel JPEG beside the checkpoint and set the C API path option.
  Preserve the source and thumbnail on incomplete upload; remove only owned
  temporary data after durable success/explicit cleanup.
- [x] Keep source-image pixel budgets and 4096-pixel original preview bounds;
  align cloud read/save/preview limits and inspect the actual owned UI frame.

## Task 4: Android adapter — Android agent

Files: `MaiCloudDriveJni.cpp`, `MaiCloudDrive.java`, `MaiCloudDriveAgentTools.java`,
`MaiCloudDriveDialog.java`, existing Android tests and a narrow image helper if
needed. Preserve unrelated Android WIP.

- [x] Reproduce/test signed 64-bit offsets beyond 2GiB; switch only affected
  reads/seeks to a verified 64-bit API and compile both published ABIs.
- [x] Remove the persistent hint, keep short reserved quota, use native policy
  for ordinary/Agent read/upload errors and avoid stale 10GB-full text.
- [x] Prepare the bounded JPEG with existing native/platform helpers and pass
  its durable path through JNI to the same shared uploader. Keep URI permissions,
  nonseekable staging, cancel/resume and owned-file cleanup semantics.
- [x] Run platform-specific tests/builds in the house-owned Android slot after
  Root freezes the header. Do not rerun shared Windows contract suites.

## Task 5: Integration/review/rollout — Root and mac

- [x] mac owns iOS generation, option lifetime, UI and post-225 build/install;
  shared five-file source supplied and Apple compilation completed. Metadata
  confirms 227 installation; OS Locked still prevents launch/PID confirmation.
- [x] Root reviews service/core/host deltas, runs shared logic once on Windows,
  appropriate UI/Android-specific checks, and records exact source/artifacts.
- [x] Review quota isolation, thumbnail orphan/cleanup races and rollback notes.
  Check the concrete existing service authorization before any deployment.
- [x] Preserve published Desktop v3.0.1 assets, user WIP, existing online quota,
  Session/IM/temp policy and unrequested system/service state.
- [x] Receive live paired Auth/Signer source/health/effective-cap receipts and
  the existing-identity tiny dual-object complete/read/delete canary. Effective
  limits are 20GB/100MB, quota remains 500GB. No full-size wire-transfer claim.
- [ ] Receive iOS 227 unlocked launch/platform connection receipt and final
  Mac App/Android APK consumer build results.

Evidence and explicit validation boundaries:
`docs/diagnostics/2026-11-17-cloud-drive-20gb-integration.md`.
