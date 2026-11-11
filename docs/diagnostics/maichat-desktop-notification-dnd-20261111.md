# Desktop new-message do-not-disturb

Date: 2026-11-11. Baseline: `7eb79ce80243cbe32ee6d2e56a4391fab66fdde3`.
Platform: Windows 11, normal Qt 6.12.0 Release Desktop cache
`MaiChat/desktop/.cache/build-qt6-ui-20261109`.

## Reproduction and change

In Settings, enable **免打扰模式**. New IM messages still arrive, persist, and increase unread counts; the ordinary system message notification and its attached system sound are not submitted. Disable the switch: only future messages may notify, without replaying silent history. The per-device QSettings boolean `maichat.preferences.do-not-disturb` defaults to false and is not placed in account or WS payloads.

The application already commits reception, SQLite persistence and state/unread updates before emitting `incomingMessageArrived`. MainWindow's same-UI-thread handler now reads the current preference before recording notification delivery. No extra asynchronous notification queue was added. Enabling clears delivery tracking and the remembered notification-click target, leaving the tray visible. Calls, invitations, operation feedback and error toasts remain outside this policy.

The setting uses the existing settings-row control style. The explanatory text is:
“开启后不弹出新消息通知；消息仍会正常接收，未读数量保留。”

## Evidence

Raw evidence and source hashes: `.cache/notification-dnd-20261111/desktop/`.

- `red-final-fixture/host.txt`: normal old production and the owned fixture produce 7 PASS / 2 FAIL. A queued incoming message, with the preference enabled before dispatch, still submits one notification; Settings has no DND control. The owned SQLite and unread assertions precede the first failure and pass.
- `green-final/`: normal required two-target build exits 0 in 26.828s; host PID22584 exits 0 in 4.549s, 9 PASS / 0 FAIL; full notification model PID46688 exits 0 in 0.214s, 11 PASS / 0 FAIL.
- The same host case verifies the latest setting at dispatch, default false, no silent-history replay, silent messages not consuming the delivery gate, on clearing the previous gate/click target, visible tray, owned SQLite/unread preservation, and persistence through recreating the settings reader/window.
- `green-final/owned-settings.png` was visually inspected: the row, explanatory text and enabled checkbox fit the existing Settings page. Only owned Fake IM identities are present.
- `windows-public-cancel.txt`: PID23000 exits 0 in 1.055s, 3 PASS / 0 FAIL. Native Qt logging shows the same tray handle receiving the owned notification and subsequently `showMessage("", "", NoIcon, 1)` while the tray remains visible.

The first exploratory fixture lacked an explicit other selected conversation and file-based QtTest logging; its exit-only record is retained under `red-old-production/` and is not the final RED evidence. No production changes preceded the logged `red-final-fixture` run. An existing QFont point-size warning and the existing QtConcurrent deprecation warning remain in the original logs; neither was relabeled as a new failure.

## Windows cancellation boundary

The [Microsoft NOTIFYICONDATA contract](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ns-shellapi-notifyicondataw) documents NIF_INFO plus empty szInfo as balloon removal. Installed Qt's Windows implementation at `qt/qtbase/src/plugins/platforms/windows/qwindowssystemtrayicon.cpp:206` only inserts a space when the title is nonempty, so BOTH empty public strings reach NIM_MODIFY/NIF_INFO with empty szInfo. `QSystemTrayIcon::showMessage` does not filter empty strings when the tray is visible.

This verifies the public withdrawal request for the current balloon; it does **not** guarantee deletion of Notification Center history or all OS-delayed queues, nor prove that system policy displayed a balloon. Other platforms do not receive an empty message as a guessed cancellation operation.

## Verification limits

Only private QSettings/profile, owned SQLite and Fake IM data were used; OS notification calls contained owned test messages. No real account, DB, configuration, notification-center history, paid request, installed App, clipboard or device was operated. Only required notification/MainWindow test targets were built; no full App, package or installation was performed. These results verify the Desktop test consumer on Windows; macOS UI/native delivery and the other client implementations have separate ownership and evidence.
