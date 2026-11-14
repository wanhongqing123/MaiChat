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

Normal 279 / 2.2.37 was then built from
`af855aa8de85f1e630eef24f03dc46ce3eb026f0` and installed with data retained.
APK SHA-256: `c577fa12e23ca138ee2260d7d5f43fda91c2b65fd64c21fbc25d297ccfc83f35`.
The new Agent JNI exactly matches the stripped normal build output, retains its exported
ABI and runtime dependencies, and does not import the API30 wrapper. The other 25 native
libraries, model bytes and signing certificate match the verified baseline.

The ordinary zygote App at PID 29462 displayed multiple existing remote conversation
images and opened their normal image preview. This closes the actual Android App-policy
boundary left by the run-as probe. Evidence:
`.cache/android-native-noreplace-20261115/normal279/rendered.png`, `preview.xml` and
`runtime-physical.json`. No security policy or user file was changed. Existing valid
publication reads do not alone prove a production24h expiry or every republication path.
