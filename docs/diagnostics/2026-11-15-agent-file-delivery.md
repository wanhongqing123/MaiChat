# Existing-file delivery through AI and IM tools

## Operations and contract

Create an ordinary PDF or archive in an owned workspace and call `maiPrepareAgentFile` directly, or call `agent_send_file` with `file_path` and an optional caption. The current AI reply must show a persistent file card; history restoration must retain it. Preview/save requires an explicit click and a fresh workspace/file check. There is no file-content upload to the model or automatic execution.

This is distinct from `maichat_send_media(type=file)`, which sends an existing ordinary attachment to a designated accepted friend through the existing sender. It keeps the ordinary host's 1 GiB non-image limit; current-AI file delivery has its own 512 MiB limit. Neither limit is a new SDK-plan claim.

The ordinary Core API and independent tool share one implementation. The result has exactly `delivery`, `type`, `path`, `mime_type`, `bytes`, `caption` and `name`; delivery is `current_ai_session`, type is `file`, and path is workspace-relative. The existing `agent_send_media` enumeration remains image/video/audio. Validation rejects invalid paths, NUL, non-regular files, escaped symbolic links, empty/oversize files, cancellation and invalid UTF-8 metadata.

## Bugs found during integration

- A POSIX ASCII alias could resolve to a non-UTF-8 basename. Android reproduced successful ordinary metadata containing raw invalid UTF-8 and a real tool `json::type_error` 316. The file tool now rejects input and resolved metadata at its encoding boundary. The identical Android harness returned ordinary/tool `InvalidInput` without an exception after the change. Only owned temporary files were used and removed. APFS rejected creation of the raw filename itself; that platform boundary is preserved.
- Canceling iOS preparation could lose the copied URL at the shared queue's cancellation check, and cleanup from an already canceled Task could be skipped. A separate owned preparation lifecycle now retains its result and cleans only an undispatched copy independently of the parent cancellation. Existing dispatched failure-row attachments remain owned by the ordinary sender. The two deterministic cancellation cases changed from RED to GREEN.
- Android and iOS card parsers now require the six native string fields, including an optionally empty caption, and a valid integer byte count. Malformed metadata does not gain path/peer authority. Unknown extra fields are ignored rather than interpreted as permissions.
- Qt file metadata completion rechecks the owner session directory. A retained hidden A page still receives its metadata and is re-enabled after switching A to B and back. Actual preview additionally requires visible A. A workspace change invalidates the old card path.

## Verification

Windows ordinary Core file/direct/tool cases passed within the 13-suite Core run. Friend file/image host consumers passed the three selected cases, five Qt checks including setup/cleanup. Android friend-host tests passed 4/0; current-AI parser/summary/PDF tests passed 14/0. Apple supplied independent ordinary Core, host, cancellation and card/parser receipts; those are not Windows or Android installation evidence.

The normal Windows file-card suite passed 6/0. Its original PDF check failed because it only observed modal dialogs, while the existing Windows PDF preview is modeless. Only the test observation changed: it now requires the exact owned visible report window, page 1/1 and nonempty rendered pixels (956 x 1353 in the observed run). Negative cases reject any owned preview, including a loading window. Root inspected the actual Windows file-card capture. No production PDF viewer was changed for this observation fix.

Source-before backups, exact incoming patch manifests, RED/GREEN receipts, screenshots and Android syscall/cleanup logs are retained under `.cache/core-send-file-*`, `.cache/android-file-utf8-probe-20261115/`, `.cache/android-current-file-delivery-20261115/`, `.cache/ios-file-source10-strict-20261115/`, `.cache/qt-file-delivery-compose-20261115-7f7bd347/` and `.cache/im-delivery-review-20261114/windows-file-preview-observation-09/`.

These checks use owned files and fake/loopback clients. A live SDK transfer, normal installed App interaction and recipient opening the attachment require separate runtime receipts. No current user store, supplier call or running user App was modified for the tests described here.
