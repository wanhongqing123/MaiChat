# CloudDrive 20GB / 100MB integration

Date: 2026-11-17. House owns Windows/Android and the common protocol; mac owns
Apple hosts and the existing pinned ECS deployment transport. Common behavior
is tested on Windows once. Published Desktop v3.0.1 remains at
`71a81e33c5d75588103ae2a701a14448bc4c3ee4` and its assets are unchanged.

## Reproduction and expected behavior

Upload a cloud video up to 20,000,000,000 bytes or an image up to 100,000,000
bytes. Resume interrupted uploads, open/download originals and display bounded
image thumbnails. Reject one byte above each cap before reading the source.
Session video/image limits remain 2,000,000,000 / 20,000,000 bytes. Remove the
persistent privacy/size hint; keep concise reserved-capacity feedback.

The image and its <=320-pixel / <=2,000,000-byte JPEG use one immutable source
snapshot. Both ordinary and Agent entry points use the existing shared C++
multipart uploader. The associated JPEG is hidden from the file listing;
reservation, completion, deletion and cleanup account for both objects.

## Evidence and fixes

Local evidence directory: `.cache/cloud-drive-upload-20gb-20261117/`.

- Auth policy, routes, migration and lifecycle suite: 234 passed / 0 failed.
  Signer product suites: 46 / 0. The broader operations suite retains one
  Windows directory-fsync EPERM failure, reproduced on unchanged baseline;
  this is not reported as a passing full operations suite.
- Common Windows local-TLS cases passed: cloud base 77.65s, thumbnail 20.20s,
  unchanged Session image 7.21s, Session latency 4.21s and VOD 10.82s.
  Virtual 19GB totals and offsets above 4GiB avoid a 20GB network fixture.
- Independent review found an unexpected companion response leaking the newly
  created upload, and a Session VOD renewal inheriting the cloud limit.
  Both were reproduced; the same two focused cases passed in 32.13s after
  aborting the invalid new upload and restoring the Session-specific fence.
- Concurrent thumbnail completion reproduced a destructive cleanup race:
  one caller lost its ready CAS after another caller completed the child.
  The original result was 404 and two object deletions. Resource-aware live
  settlement now returns ready without deleting either object; the Auth
  suite above includes this regression.
- Windows native dialog lifecycle passed in 24.90s. Its initial timeout came
  from checking a prepared but still hidden playback owner; the original
  first-frame assertion remains after waiting for the owner to map.
- Agent pause reproduced duplicate paused/uploading entries because Qt used
  forward slashes and native checkpoint enumeration used backslashes.
  A canonical private identity key fixes the duplicate and makes repeated
  pause idempotent; the Agent lifecycle passed in 23.24s.
- Both ordinary and Agent HTTP 413 mappings still mentioned old numerical
  limits. Controlled responses reproduced both messages; the same focused
  cases passed in 3.40s and 1.91s with current-limit/available-space wording.
  The final normal Windows App relink and 114-PE-import runtime gate passed.
- Android JVM suites: 21 / 0. Both ARM product native targets compile/link.
  A private API 29 32-bit emulator reproduced the old >2GiB reader failure;
  `off64_t` / `pread64` passed the same 3GiB-offset case. This does not claim
  an ARM physical-device or complete product APK cloud E2E run.
  Final review also reproduced a durable-checkpoint path-alias rejection:
  `/data/user/0` and `/data/data` can identify the same snapshot. The host now
  checks name, length and full SHA, leaving source-ID rebinding to the core;
  pre-checkpoint metadata still requires the exact identity. The added host
  regression failed before this fix and passes in the 21-case result.
- Apple host results are owned by mac: 12 native cases, full shared-source
  App Debug compilation, Release 227 build and strict signature checks.
  Missing auxiliary files with a durable checkpoint delegate remote-ready
  recovery to the core. Completed-task cleanup no longer waits behind an
  unrelated provider's global image-preparation lock.

## Deployment boundary

The reviewed service candidate contains five changed JS files and a pinned
27-file runtime closure. Outer archive SHA256:
`3dafe43949b17257b283809d2be7c4b1e6698bcf2d63f51e6ad83b2548a659d6`.
It was handed to mac for execution using the existing identity for
8.134.253.45. No new dependency, key, bucket, provider quota or Sync source
change is part of the candidate. The configured 500GB quota stays unchanged.

The operator must preserve a consistent SQLite backup, validate the actual
runtime/source before switching Auth and Signer together, and read back the
effective caps and service health. A database backup must never overwrite
the live database during rollback. After thumbnail-bearing rows are exposed,
the old Auth cleaner is not a safe hot rollback target.

Mac's controlled-deployment report identifies Auth PID 173186 and Signer
173183. Active process values and the active factory read back 20,000,000,000 /
100,000,000 / quota 500,000,000,000. Sync PID 173208 retains its old working
directory and entry hash. Both database online backups passed quick_check and
the five-column migration rehearsal passed on backup copies; no live restore
was performed. Existing configuration/drop-in/dependency hashes are unchanged.

The first apply reported a transient health URLError immediately after Sync
start and retains exit 1. A separate bounded check of the new services then
returned 0; there was no old-cleaner rollback. Auth/Sync `/health` returned
200; the Signer unauthenticated POST probe correctly returned 403. The final
safe receipt's five source hashes match the local frozen service candidate.

The existing owned canary uploaded a 777-byte original and 776-byte associated
JPEG. Main completion before thumbnail completion returned 409. Completing
thumbnail then main produced one visible card; both direct OSS reads matched
their SHA and the JPEG needed no OSS image processing. Ordinary DELETE returned
200 with cleanup_pending false; both signed reads returned 404, the card
disappeared, and the owned session logged out. No new identity was created.
Second-account isolation and full-size 20GB/100MB network transfer were not run.

iOS metadata now reports 3.0.1(227) and a new installed bundle. The first install
timed out and that receipt is preserved; OS launch is explicitly Locked, so no
launch/PID or physical-device cloud E2E claim is made yet. Mac's normal App
consumer build and Android's normal APK build are in progress. Original
unrelated workspace changes and the installed Windows App remain preserved.
