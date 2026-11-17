# macOS disk-space audit — 2026-11-15

User asked why storage is exhausted again. This pass reads filesystem/process/simulator metadata only; no user file content or database was inspected, and no file, simulator, App or system snapshot was deleted/stopped. The only process stopped was this turn's slow whole-home du metadata walk after targeted counts had completed.

Data volume df:926GiB total,874GiB used, available fluctuating2.7–3.3GiB,100% rounded capacity. APFS container reported99.8% allocated. Inode usage26–30%, so the reported shortage is bytes rather than inode exhaustion. Active swap used about13350MiB of14336MiB allocated. Three local snapshots are macOS update snapshots; no Time Machine thinning/removal occurred.

Targeted directory du totals (directory accounting, not additive unique APFS extents or a guaranteed reclaim amount):

| Directory | Approximate size |
| --- | --- |
| /private/tmp |47GiB |
| ~/Library/Developer/CoreSimulator/Devices |69GiB |
| ~/Library/Developer/Xcode |28GiB |
| Xcode/iOS DeviceSupport subset |18GiB |
| Xcode/DerivedData subset |8.8GiB |
| ~/Library/Caches |26GiB |
| ~/OpenSource/chromium |140GiB |
| ~/OpenSource/MaiChat |28GiB |
| ~/.android |16GiB |
| ~/.gradle |4.8GiB |
| ~/.r2c |760MiB |

MaiChat-specific old temporary generated directories include ios-inline-question-20261109 at5.8GiB (simulator-derived3.5/device-derived2.3), ios-picker-toggle-20261114 at3.7GiB (two1.8GiB device210 builds) and ios-device-update-c14a7d08-20261111 at3.6GiB (two1.8GiB App/build copies). Release263/264/265 each retain staging/archive/extraction/proofs and are roughly1.1–1.5GiB per task; some copies are APFS clones, so du overcounts unique recoverable blocks.

Read-only simctl found four old dedicated MaiChat test devices still Booted: Batch1, Inline Questions20261109, Rotation Fresh20261103 and Seek Feedback20261031. They were not shut down or reset. Persistent simulator userdata must not be treated as a disposable build cache. No xcodebuild/ninja/cmake process was reported by the targeted process-name checks at that time.

The existing2026-11-07 cleanup record reports about46GiB actually freed and48GiB then available. Subsequent task bundles, build copies and retained test devices/caches have accumulated; these current totals alone cannot attribute the entire later45GiB decrease to one directory. This round also retained several older full iOS builds and multiple release validation copies without finishing their lifecycle cleanup. Prioritize proven completed generated build/product/archive copies after checking active paths; retain latest usable packages, current shared build cache, source, diagnostic receipts and all user/App/simulator data. No deletion authorization was inferred from the user's question.


## Authorized cleanup completed

After the user explicitly instructed immediate cleanup, three bounded batches removed 48 completed generated paths. Each batch checked the exact owned paths against open files first;0active targets were found. Removed items were old iOS Build/index/module caches, superseded App bundles, completed old Desktop dist/ZIP extraction/archive copies, and already-delivered transport fragments. Task roots, source/index backups, logs, screenshots, source patches and hash receipts were retained. No mounted path/symlink target, user App, simulator userdata or repository source was removed.

Directory accounting for these paths was 33.95GiB; summed before/after df changes across the three batches were 16.86GiB. These differ because of APFS shared extents and concurrent system activity. Final actual available space was 23.31GiB. Do not attribute all free-space fluctuations since the earlier3.3GiB observation to these rm operations alone.

Latest public2.2.65 DMG/ZIP and the retained App executable were rehashed to their original frozen values. The complete latest265 dist, latest iOS212 task, desktop full-config cache and Core gate remain. The repository's entire porcelain status and empty index were unchanged after cleanup. Running Apps and all four simulators were not stopped; account/chat/database data was untouched. Historical temporary executable paths removed here still refer to the source/hash receipts recorded when they ran, not to a new verification.

Per-path manifests, per-batch exit/space records and final safe proof: `/private/tmp/maichat-storage-cleanup-20261115/`. Future verification should reuse the retained normal cache and keep only the newest usable product plus small diagnostics after each completed delivery.
