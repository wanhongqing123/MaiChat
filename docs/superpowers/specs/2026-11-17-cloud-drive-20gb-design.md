# CloudDrive 20GB / 100MB design

The user explicitly selected decimal video 20,000,000,000 bytes and image
100,000,000 bytes, removed the persistent privacy/file-size hint, and authorized
implementation. Reserved quota may retain a short label. Existing per-account
quota, Session attachments, temporary images, IM limits and published v3.0.1
assets remain independent. No new library or third-party Git is introduced.

house owns services/shared C++/Windows/Android; mac owns iOS/macOS. Shared
behavior is verified once on Windows, with platform-specific adapters verified
by their owner. A service deployment is a separate final operation after a
concrete reviewed candidate, tests, backup/rollback instructions and existing
service authorization have been checked.

## Existing implementation and selected approach

Keep the existing signed OSS Multipart uploader and fixed 8MiB parts. A 20GB
video uses 2385 parts. Increase only the internal completion-body allowance to
1MiB; public metadata requests remain small. Cloud-specific policy is split
from the unchanged Session policy. Quota continues to be checked atomically per
request; a configured quota smaller than the per-file ceiling is valid, and
the effective available capacity still limits uploads.

Increasing an OSS processing quota was rejected as an unknown external
dependency. Returning broken thumbnails for large originals was rejected.
Instead, new image clients prepare a bounded JPEG with existing platform image
helpers and supply it to the same C++ multipart core. Original image bytes are
preserved. The server handles metadata/signing only, never image byte transfer.

## Frozen host-facing thumbnail contract

Append this borrowed string field to `MaiCloudDriveOptions`:

```c
const char* cloud_drive_thumbnail_path_utf8;
```

It is an absolute path to a caller-owned JPEG, at most 2,000,000 bytes and no
larger than 320 pixels on either axis. Hosts prepare it on their existing image
worker using the current bounded decoder. They keep this file and pass the
same path/content during an incomplete upload or resume, and remove it only
after the durable completed receipt or explicit upload cancellation cleanup.
It lives alongside the existing checkpoint, not in a new storage root.

The option is cloud-image-only. Session/temporary-image calls reject a non-null
thumbnail option. New ordinary and Agent image upload adapters provide it.
An image larger than 20,000,000 bytes requires it before remote initialization;
legacy smaller uploads without it retain existing behavior. A failed bounded
thumbnail preparation is an explicit pre-upload error, not a ready cloud item
with a broken list image. Videos retain their existing thumbnail behavior.

## Frozen control protocol

The original `POST /v1/cloud-drive/videos/uploads` accepts optional
`thumbnail_size_bytes`, positive and at most 2,000,000, for images only. The
server generates a separate random JPEG object key under the existing account
`cloud-drive/` prefix. No new bucket/RAM prefix is needed. The thumbnail is
associated metadata, not a second visible cloud-drive file. Reserve both byte
sizes atomically without changing the account quota.

When supplied, initialization additionally returns:

```json
{"thumbnail":{"upload_id":"opaque OSS id","size_bytes":12345,
"part_size":8388608,"parts_total":1}}
```

Use the same original media ID for these new cloud-only control routes:

- `GET /v1/cloud-drive/videos/uploads/{id}/thumbnail/parts`
- `POST /v1/cloud-drive/videos/uploads/{id}/thumbnail/parts/1/sign`
- `POST /v1/cloud-drive/videos/uploads/{id}/thumbnail/complete`

Their response shapes match the existing main-part/list/sign/complete shapes,
with the thumbnail upload ID and byte geometry. Sign requests retain exact MD5
and length binding. The existing internal signer init/list/sign/complete/HEAD
operations are reused; no alternate upload transport or proxy is introduced.

The client uploads and verifies the one-part thumbnail first, then handles the
original parts. Main completion refuses a declared but unverified thumbnail.
Only after both HEAD verifications may the original row become ready. The
existing thumbnail-link endpoint returns a signed read link for the associated
JPEG; existing small historical images may retain the original OSS thumbnail
processing fallback. No unknown OSS image-processing quota is required for
new large images.

## Identity, persistence and deletion

Persist thumbnail key, upload ID, byte size and readiness with the original
record. Include auxiliary bytes in used/reserved accounting. Both keys must be
known before network initialization. Lost init responses, cancellation,
completion/delete races and account deletion reuse exact-key multipart
discovery, abort, delete, HEAD and the existing cleanup hold/revision rules for
both objects. Do not release quota or remove the metadata until both resources
are proven cleaned. A thumbnail complete closes its multipart ID, avoiding a
replayable simple-PUT URL after deletion.

Extend the existing checkpoint with optional thumbnail identity, fingerprint,
size and completion state. Old version-1 checkpoints without a thumbnail stay
readable. Bind the retained thumbnail to the explicitly supplied host path and
content; never read an arbitrary path injected into a checkpoint. Failure or
cancel retains resumable state; a completed receipt covers both resources.

## Read/preview and platform adapters

Apply cloud policy consistently to upload, source staging, checkpoint parsing,
read-link validation, rename/move responses, downloads and Range VOD. Preserve
bounded part memory and image pixel limits. The 100MB image ceiling fits the
Windows encoded-image default; original previews remain bounded to 4096 pixels.
Remove the persistent hint instead of replacing it with longer text. Format
specific error limits from shared media information rather than duplicate old
numbers. Keep concise reserved-quota information.

Android uses explicit 64-bit POSIX file offsets on both published ABIs. Its
non-seekable source path still needs a full durable local copy and space check.
Platform image adapters retain their existing pixel budgets; the cloud quota
does not authorize unbounded image decode.

## Verification and rollout boundary

First reproduce failing tests for 20GB/100MB boundaries, internal completion
above 32KiB, unchanged Session limits, missing/incorrect thumbnail, thumbnail
cleanup races, checkpoint resume, large Range offsets and Android 32-bit
offsets. Use sparse/virtual sources and local TLS/fake OSS; do not upload tens
of GB or use user media to prove arithmetic. Run the shared contract on Windows
once, plus affected Windows UI and Android-specific native/host checks. mac
consumes the frozen interface for iOS-specific generation/lifetime/UI checks.

Prepare service source/tests, migration and rollback notes before deciding any
real deployment. No live quota changes, paid quota application, new libraries,
user database inspection or modification of old Release assets is included.
