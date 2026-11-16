# Separate My and Settings pages: Windows consumption

The user's final preference is a Settings gear immediately below My in the sidebar,
with independent pages and selected states. My contains the account/avatar and account
actions; Settings contains appearance, notifications and remote-desktop preferences.
The later visual refinement uses bounded cards and concise labels. The earlier
top-right Settings/back design is superseded and was never applied by Root.

## Exact source

Root replayed the final 46,759-byte patch once after matching all three preimages.
Patch SHA-256: `34d75acd25d9c697716da810d522ebd7ee71e7f4c554a02607372a4568b62ab9`.

| File | Preimage | Accepted postimage |
| --- | --- | --- |
| `MainWindow.cpp` | `2fc63ca22c73d5d5f15cc91356d75479ab311753` | `76f56d2bd074f5c0162ad71d553cdcc31f1da051` |
| `MainWindow.h` | `f5bb7b8a10aebf7365fb76bd9d283c2b4cb66251` | `7cf43bb82331dc9985eb60e83e9110865ac09efa` |
| `MainWindowLayoutTest.cpp` | `c646dadc9f481f17475b0c12bdaceeaca7ee846a` | `051a8150ce853b35c6bf6bd5b98a175c5b92116e` |

Both Git blobs and canonical-LF SHA-256 values matched the reviewed manifest. The
private independent static review found no concrete P1/P2. This does not consume
either superseded 20,261- or 25,130-byte candidate.

## Windows execution

On 2026-11-16, Windows 11 / normal Qt 6.12.0 cache
`MaiChat/desktop/.cache/build-qt6-ui-20261109`, the normal UI targets compiled and
linked successfully. The focused test entered through `scripts/run-tests-windows.ps1`
with `QT_QPA_PLATFORM=windows` and the deployed native platform plugin.

Fourteen affected selectors passed, including three Windows caption controls/scroll
paths, separate My/Settings navigation, original nickname/account presentation,
remote-desktop preferences, avatar update/tiny-WebP, logout confirmation, avatar
navigation, whole-UI zoom, rounded card borders and both notification preference
contracts. QtTest totals were **16 passed, 0 failed, 0 skipped**, including init and
cleanup, in **6,252 ms**. No account action was performed against a real account.

The actual `my-page.png` and `preferences-page.png` were inspected. The sidebar gear,
separate selection, concise card contents and top-right Windows caption controls are
visible and unclipped. The fixture also checks minimum-window containment. Peer
normal Cocoa validation of the exact same three source postimages was 16/0/0; Root
does not claim to have run that Mac test.

Evidence:

- `.cache/settings-page-review-20261116/compact-1794821224/applied-root-receipt.json`
- `.cache/settings-page-review-20261116/compact-1794821224/windows-affected.txt`
- `.cache/settings-page-review-20261116/compact-1794821224/windows-images/`
- `.cache/sunshine-parity-20261116/ui-document-main-integrated-build.txt`

## Delivery boundary

The stock Windows App, PID 56404 at `D:/Program Files/MaiChat/maichat.exe`, was not
stopped or replaced. This is source consumption and real Windows fixture validation,
not an installed-App update. The normal cache also contains explicitly preserved
unrelated MobileAgent adapter WIP and is not a clean distributable. Those three
protected files were neither staged nor included in this UI change. Directory
reorganization remains a document-only proposal.
