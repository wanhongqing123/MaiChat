# Completed Session reports rejected by the room-message budget

## Actual reproduction and installed artifacts

On 2026-11-15 the user reported that same-account Android remote Creation showed
earlier progress and running tools while the Windows owner showed its final
report. The earlier consumer-ordering fixes passed ordinary tests but did not
close the real scenario. Source `21c5bd803e9dc636ffd0a624f40f0b80fe5f92a3`
was built in a detached clean checkout and installed over the connected OPPO
PKD130, retaining data, after the user's explicit installation instruction.

- Android: `com.kongshang.maichat`, 2.2.39 / 281, PID 32586. Both local and
  installed APK SHA-256 were
  `bc30e49db94595d05ec2bb13557be8148234308529c3e50fc8f66120ab8e7a69`.
  The normal Debug build type and signing certificate match the previous normal
  installation; production account configuration is enabled and local fixture
  auth is disabled. The complete corrected APK contains 41 verified ELF libraries
  across the repository's ARM ABIs and the six pinned model assets. An earlier
  incomplete portrait-resource candidate was rejected before installation.
- Windows owner: the existing installed process 52672 at
  `D:/Program Files/MaiChat/maichat.exe`, version 2.2.63 from its own startup
  header, executable SHA-256
  `0c3f2f83761653280b3a70a270eb20acbc9800b5515f6877cffd8b36f963ad1b`.
  This process was not stopped, replaced or restarted.
- The updated Android still displayed the original earlier report fragment and
  running-tool/thinking row. The actual scenario therefore remained RED after
  the independent 55-test consumer pass. Screenshots were inspected, not only
  captured. The normal account and original remote session remained available.

## Bounded, read-only on-site evidence

The selected Android controller was inspected through its existing debuggable
process using JDI field reads only. No target method was invoked. Only message
counts, serialized byte counts, runtime booleans and report-presence booleans were
emitted. The VM was resumed in `finally` within 631 ms and the task's local port
forward was removed. This did not read preferences, credentials or an Android
database. The current page contained eight messages; its largest compact Android
JSON was 49,194 bytes, or 49,109 after slash-escape normalization. The owner was
idle with an empty turn ID, but the final report was absent.

To identify the owner-side discrepancy, the selected room's SHA-256 identity was
matched to exactly one local Windows Agent database. The connection used
`mode=ro` and `PRAGMA query_only=ON`, bounded to the latest twenty ordinary
assistant messages. Only public display fields and fixed public image-publication
receipts were used; tool IO, auth configuration, multipart checkpoints and signed
URLs were excluded. Bodies and raw IDs were not emitted or exported. An initial
empty-string-only import filter missed NULL rows and was corrected before using
the result as evidence.

All twenty owner messages were completed and the final report was present. Its
conservative public-message JSON size, including its actual public image receipts,
was **53,402 bytes / 177 parts**; the largest examined message was at least
**67,809 bytes / 256 parts**. These are lower bounds because private tool IO was
not read. They exceed the service's **49,152-byte** single-message limit while
remaining within the desktop's existing 95 KiB complete publish-body budget.

The phone's history API reads the service's already-published room copy; it does
not synchronously fetch the owner's Core store. The real service implementation,
with an owned in-memory fixture, reproduced a 53,402-byte completed update being
rejected with 400 after its smaller running version had been accepted. Thus the
old room copy survives although local completion and later idle presence are
correct. The original on-site publish status was not captured; the rejection is
established by the exact deployed validation condition and ordinary API test.

## Repair and frame limits

The service accepts a complete room message up to 95 KiB, matching the existing
publisher contract, while the existing 96 KiB request-frame limit stays in place.
No body or report is shortened. The post-merge attachment-size check still rejects
oversized merged records atomically and preserves the previous attachment.

A separate limit matters: shared sockets accept at most 1 MiB per complete WS
message. Live events already use 24 KiB chunks, but history responses are one
correlated message. Twenty valid 70 KiB messages exceed that receiver. The server
therefore returns an explicit, small `413 / HISTORY_RESPONSE_TOO_LARGE` with the
same request ID rather than silently returning fewer successful rows or dropping
the connection. Existing clients receive a clear failed request and do not falsely
mark history exhausted. Updated Android, Qt and Swift request eight complete rows
per remote page, including older pages; local native paging remains twenty.
Eight maximum-size messages plus bounded control state fit the existing receiver.

This changes no schema, credential, model route or media-byte path. Messages beyond
the pre-existing 95 KiB client publish budget are outside this bounded repair.

## Verification before deployment

- Node ordinary service: the exact-size report test actually failed 400 versus
  expected 200, then passed with full history/body equality. The response-budget
  test separately failed after the size change alone, then passed with the guard:
  twenty complete 70 KiB reports, explicit oversized-page error, the same live
  socket, and complete `8 / 8 / 4` pages without missing or truncated messages.
- Full auth-service suite on Windows Node 24.14.0: **221/0**, 6.74 seconds. The
  existing pre-merge 400 and post-attachment-merge 413 boundaries were moved to
  the new budget, retaining rollback and attachment-preservation assertions.
- Android ordinary Gradle / standard JDK17: **56/0** (Controller 21, pages 14,
  remote images 21). The new remote eight-row/local twenty-row case was first
  actually RED. Full eight-row pages continue; a short final page ends paging.
- Qt ordinary Windows target build: exit 0, 23.817 seconds. Seven selectors:
  **9 passed, 0 failed**, 5.584 seconds, using the normal compiled target and an
  offscreen QPA fixture. Its complete seventeen-message history is `8 / 8 / 1`.
  This is logic acceptance, not a new installed Windows App claim. The first
  test-launch runtime-path problem was corrected separately, not labeled a
  product RED. The external Mac ordinary target also passed 9/0.
- Swift: full-source parse and an extracted actual older-loading method with
  owned async seams passed remote eight/local twenty behavior, including cached
  rows. This is not a complete iOS App build or physical-device acceptance.

## Live baseline and deployment boundary

Fresh read-only SSH preflight superseded the earlier image-generation runbook:
Auth 165340 and Sync 165353 run from
`/opt/maichat/releases/prepare-image-generation-20261115-57b92599/auth` using
`/usr/bin/node-22` (22.23.0). Media remains process 151848 in the previous media
release. Image-generation behavior is already live and must be preserved.

The actual production service/server files exactly match this change's committed
preimages: service SHA-256
`30fcd6c68928fa30a6cc02f7615df04a45428df1a5338cf8c60c3dd314ea7445`, server
`cd1869c1069e69a66851d31d904f7659a8cd02af9233cfd957d14edb9ee01dd9`.
The unchanged store/dependencies were fingerprinted. No environment, credentials,
production DB or unit body was read during that preflight; no remote write occurred.

The prepared deployment scope is the two budget-related JS files in a unique
release directory, keeping the existing working directory, environment, user and
unchanged dependencies. A task-owned ExecStart override points to the new absolute
entry, preserving relative DB-path semantics. Only Sync needs a brief restart;
Auth, Media, feature gates and the Windows App remain running. Rollback removes
only the verified task override and restores the old code entry, without restoring
or editing a database. Deployment still requires its explicit operational go-ahead
and a final same-session physical comparison. Local passes do not close the scene.

## Verified client and deployment-package checkpoint

The complete repair was committed and pushed as
`2f92e555f9bce7e227b9262afc084f13af67a8ce`. Independent Mac Node 22.19.0
verification then passed the same complete 221 tests in 10.654 seconds. An
initial private test copy omitted an unchanged signing dependency; its module-load
failures were retained separately, then the complete baseline copy passed. No
production change was used to obtain that result.

The connected phone was subsequently updated in place to **2.2.40 / 282** from
that exact source. Normal Debug build completed with exit 0 in 33.103 seconds;
package signature/alignment, 41 ELF/source build IDs and pinned models passed.
The APK is 1,689,012,354 bytes, SHA-256
`51a197dfc3c82bc132842eba3988bb54bd1410206613b215539554db5de8d80b`.
`adb install -r` exited 0 in 64.013 seconds, installed APK SHA matched, version
read back as 282/2.2.40, and MainActivity launched as PID 5572. No uninstall,
data clear or additional permission grant was performed. This verifies the client
update; it does not assert that the server's missing report has been repaired.

Independent deployment preflight found that a proposed `92-...` override would
precede the existing `99-prepare-image-generation-20261115-57b92599.conf`.
That package was superseded before any remote write. The corrected task override
is `99-z-message-budget-2f92e555.conf`, ordered after the existing 90 and 99
overrides, both of which remain intact. The exact two changed JS files, unchanged
store and ExecStart contents remain byte-identical to the reviewed candidate.

The corrected six-file regular-only package is 15,417 bytes, SHA-256
`4b3c0593a191b96e7032235e24e9606e2f5ec7581a5d0d2a09afca4b3f384eb1`.
Its manifest requires the new override to remain lexically last and supersedes
the earlier package. Independent package review passed without repeating
unchanged tests. The payload contains no credentials, environment or database.

At this checkpoint the user has been asked for the operational go-ahead for the
brief online Sync restart. No approval has yet been received. No remote upload,
directory creation, unit write or restart has occurred. Auth, Sync, Media and the
original Windows App remain running. Final same-session acceptance is pending
the server update and ordinary owner retry.
