# Android CloudDrive 20GB / 100MB adapter

Date: 2026-11-17. Base common commit: `8aefd68b3097fb37a495846d680d25fc45b5e886`.
Root must record the final integrated source and product artifact separately.

Expected operation: normal cloud uploads and Agent workspace uploads share the
ordinary MaiCloudDrive client. New images need one immutable private original
snapshot and a retained bounded JPEG before remote init. Pause/failure retains
resources with a durable checkpoint; completed/discarded calls clean only owned
auxiliaries. Legacy checkpoints do not gain a thumbnail mid-upload. The quota
card has no persistent privacy/size hint; actual reservations have a short label.

Observed RED cases: the normal component still displayed its old hint and a
hardcoded 10GB quota error (2 failures, original navigation passed); a 20 GB disk
budget was rejected by the old 2 GB Java constant (1 failure); an undecodable Agent
image was acknowledged and queued before preparation (1 failure).

JNI now passes only the borrowed thumbnail path to the frozen C API. Session
calls keep it null. Random reads use explicit off64_t/pread64 with range checks,
independent of the32bit default off_t. No alternate transport is introduced.

New images use existing nativeStageFd for a bounded private snapshot adjacent to
the checkpoint. Video seekable descriptors remain direct reads. JPEG generation
reuses the normal viewer JNI decoder, preserving HEIC, EXIF transforms and its
pixel budget. Metadata binds original stable ID/name/size/full SHA256 and the
JPEG size/SHA256. No arbitrary filesystem path is read from the core checkpoint.
Agent preparation runs on its existing tool worker before accepted, then passes
one owned pair to the normal queue without restaging or decoding. UI remains
asynchronous. A local auxiliary lease protects only file lifetimes, not remote
multipart state. Native media information supplies file policy and error labels.

JVM checks cover actual Android views, Native Graphics JPEG output, identity,
legacy behavior, cancellation/cleanup, Agent pre-ack failure and prepared handoff.
Exact suite counts/source hashes are in the cache handoff. Invalid image decoder
warnings are intentional fixture input, not a hidden product failure.

The JNI translation unit compiled for both published ARM ABIs with NDK 27 / API 26.
An owned API 29 AVD on port5580 ran two private no-network probe APKs in 32-bit x86
processes. Both use the real internal JNI reader with unrelated JNI code removed
by linker GC. A sparse3,000,000,000 offset read was low=true/high=false with the
original source and low=true/high=true with pread64. No 3 GB physical payload or
online large transfer was performed. The probe also checked an earlier ordinary JPEG
lifetime adapter revision and an owned UI screenshot; current host changes are
covered by the current JVM/compile evidence, not claimed as final product APK
execution. It is not an ARM device run or a
full product CloudDrive E2E acceptance. Shared codec/transport tests remain owned
by the Windows integration, without a duplicate Android contract run.

Both complete product native consumers (armeabi-v7a and arm64-v8a) linked successfully
against the current working-tree core candidate. Both export the new thumbnail JNI
entry and contain the associated-thumbnail protocol. Product APK execution and the
new control protocol still need the final Root-owned integration. The physical Oppo, original AVDs, installed user
apps/data, unrelated WIP and published Desktop Release were not changed. No live
configuration, user database or deployment was accessed.

Evidence: `.cache/cloud-drive-20gb-android-20261117/`.

Review recovery fix: a retained checkpoint with a declared thumbnail no longer
fails in the host merely because local JPEG/metadata is absent or corrupted.
The host still checks original identity and derives only the adjacent path; the
shared core queries authoritative-ready state before deciding whether it needs
local JPEG bytes. This host case was actual RED1 to GREEN. New-checkpoint cached
corruption remains an error. No shared protocol test was repeated on Android.

Source-ID alias review: the native file adapter stores the passed absolute path,
while Java canonical paths can resolve Android /data/user/0 to /data/data. A
durable-checkpoint host comparison incorrectly blocked equal original content.
The host now preserves name/size/fullSHA and delegates stable-ID rebinding to
the core, while pre-init metadata still enforces exact source identity. A JVM
absolute/canonical path-alias case was actual RED1 to GREEN; the corresponding
Android29 symlink was confirmed read-only on the owned AVD.
