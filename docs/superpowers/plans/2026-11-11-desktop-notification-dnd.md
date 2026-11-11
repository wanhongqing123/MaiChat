# Desktop notification do-not-disturb implementation plan

**Goal:** The existing Settings page offers a per-device switch that suppresses new-message notifications and their attached sound, while reception, persistence, unread counts, badges and the tray remain unchanged.

**Approved contract:** Root's cross-platform contract fixes the key `maichat.preferences.do-not-disturb`, boolean default false. Desktop stores it in existing QSettings, never account or WS payloads. The final incoming-message notification handler reads the latest preference. Enabling clears notification delivery gates and the remembered notification peer; disabling only enables future messages. Existing calls, invitations, action feedback and error toasts are outside this feature.

**Architecture:** Reuse MessageNotification for ordinary preference access and delivery policy; add the existing settings-row checkbox in MainWindow. On Windows only, an empty public QSystemTrayIcon message withdraws the current balloon without hiding the tray. This does not promise removal of Notification Center history. Other platforms do not receive empty notifications.

**Repro / expectation:** Queue an owned incoming Fake IM message, enable the preference before dispatch, then process events. No ordinary notification is submitted; the message remains in the owned database and unread count. Disable: no historical notification is replayed, while the next new message can notify. Enable after an earlier notification: clear delivery state and stale click target, preserve the tray, and persist the switch through recreating the window/settings reader.

- [x] Add MainWindowLayoutTest cases using private QSettings, owned SQLite, Fake IM and notification-submit observations; run against unchanged production for real RED.
- [x] Implement minimal ordinary preference/policy and Settings switch. At delivery read latest flag before tracking. On enable clear tracker/peer and withdraw current Windows balloon.
- [x] Run the same host cases plus full MessageNotificationTest and existing notification click cases in the normal Desktop cache. Inspect an owned settings screenshot.
- [x] Record raw logs, exact source SHA, ordinary Windows cancellation evidence and verification boundaries in `.cache/notification-dnd-20261111/desktop/` and a short diagnostics note. Do not commit, package, replace installed Apps, or access real accounts.

Official balloon withdrawal contract: https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ns-shellapi-notifyicondataw (NIF_INFO with empty szInfo). Qt's installed Windows backend maps both-empty title/message to that operation. This is a current-balloon operation, not a cross-platform notification-history API.
