# iOS 217: list dragging, group back and account label

The Mac collaborator reproduced two normal Simulator touch failures: releasing a
horizontal drag inside a conversation row opened that conversation, and a group
history's extra SwiftUI drag gesture competed with the native back gesture.

The final patch replaces the row's plain Button with a real tap recognizer and the
same default accessibility action. Group history reuses the existing native scroll
observer to stop following new messages, removing the competing SwiftUI drag.
The public account label uses `maiChatAccountID` and explicitly says MaiChat account;
it does not display the internal IM route ID as a fallback.

Root checked the exact 19,339-byte patch SHA-256
`ad7036d9e5bcbcb2f6e9213de6d8879d6c484ae3d4484ad85deed7b773c629be`,
all four preimages and postimages, then applied that final patch once. The older
13,426-byte candidate is superseded and was not applied. Product postimages:
ChatView `622894f8693daeb472c988706f0d58063c39b75f`, SettingsView
`adf8168de3067ef5f84178a467063f8ba7f1af34`. Root's static delta review found no
concrete P1/P2; Windows cannot execute iOS touch acceptance.

The collaborator reports normal Simulator final 3/0 in 70.754 seconds: list drag
does not open friend/group, true taps and vertical scrolling work, original clear
swipe/cancel works, friend edge/middle/cached/keyboard-draft back works, group old-page
reading retains its position on append and can return to latest/back/reenter. Actual
screenshots and video were inspected there. VoiceOver was not tested.

Normal 2.2.4(217) device build and signature passed; candidate executable SHA-256
`449f6224240734de439cf77e647733344308dce3fb7728927e3b8ae0e3827371`.
The attempted keep-data installation failed with CoreDeviceError4016 because the
physical iPhone was unavailable. This is not a completed device installation or
device regression. Existing authorization is retained for a connected device; no
pairing, permission, desktop application or service was changed here.
