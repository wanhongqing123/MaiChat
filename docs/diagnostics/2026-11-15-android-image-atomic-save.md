# Android image download atomic publication

2026-11-15, PKD130 / Android 15 / arm64; normal 278 / 2.2.36 at
`23f8ee74747f8480f0a4c78ede469a9aea854952`.

## Reproduction

Open an existing image part in the same-account computer conversation in Creation
Center. The request passes the corrected account guard but the card displays a generic
network/service error. It should display the fully downloaded image and preserve any
file already occupying the destination.

## Root-cause evidence

At `MaiSessionAttachments.downloadTemporaryImage` after the native call, three separate
JDWP scalar evaluations returned status 5 (`MAI_CLOUD_DRIVE_FILE_ERROR`), HTTP status 200,
and curl code 0. No locals, response body, URL, token or preferences were inspected.
The debugger was resumed, detached and its forwarding entry removed. Initial batched
jdb evaluations mixed asynchronous results and are excluded from the evidence.

`TemporaryFile::commit(false)` used POSIX `link` to publish the complete file without
overwriting an existing destination. Android app-data security rejects hard links.
An isolated file probe under the normal app UID reproduced `link` permission denial;
rename succeeded. A second NDK API26 probe included the actual production implementation:
the original source failed with errno 13, and the corrected source passed. Both retained
late competing files and symlink victims, rejected an existing target, and left no probe
files behind. Direct helper launch failures were recorded separately as launcher
preconditions, rather than product failures.

Evidence is under `.cache/android-native-noreplace-20261115/`, including
`device-red-final.log`, `device-green-final.log` and `freeze-receipt.json`; the actual
App scalar observation is `normal278/native-result-metrics.json` under the account
identity diagnostic cache.

## Correction and checks

Only Android's non-replacing publication uses `renameat2(RENAME_NOREPLACE)` through its
ABI-specific syscall number. The bionic wrapper requires API30, so it is not imported
by this API26 product. Failed or unsupported calls fail closed. There is no
exists-then-rename fallback. Windows and other POSIX publication, replacing saves,
flush/fsync, size verification and cancellation retain their existing behavior.

- The existing normal arm64 API26 archive compiled successfully.
- The production source and existing test compiled for armeabi-v7a API26; neither ABI
  imports the API30 `renameat2` wrapper.
- The ordinary download API passed its existing private localhost TLS fixture, including
  the new late-writer and canceled-new-file cases. Normal desktop libraries were read
  only and remained byte-identical.

These checks include the actual run-as file policy but do not substitute for a normal
zygote App process. Normal 279 / 2.2.37 build, installation and the original image retry
remain the final verification step at this source checkpoint. No Android security
policy or user file was changed.
