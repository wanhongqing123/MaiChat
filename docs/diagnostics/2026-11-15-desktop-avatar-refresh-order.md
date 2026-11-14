# Desktop contact avatar refresh and startup

## Reproduction and expected result

On Windows, connect IM before constructing MainWindow, then open a conversation whose avatar is not cached. The list must request and render the remote avatar. Deliver a fresh SDK friend face URL followed by an older conversation face URL; the fresh friend avatar must remain authoritative. Refresh account-service friends while connected; SDK avatar metadata must refresh as well.

The user reported different avatars for the same contact on desktop and mobile. These tests reproduce three concrete desktop defects; they do not establish that every possible cross-device profile discrepancy has the same cause.

## Root-cause evidence and change

- The window enabled avatar requests only on a future connectionChanged signal. A window created after login never received that event. Synchronize only avatar admission and prefetch from the current connection state, without replaying binding recovery side effects.
- Conversation metadata could overwrite a newer nonempty friend profile face URL. Keep friend avatar authority within the current login, serialize vendor callback updates on the QObject owner thread, and reject stale account, login and request generations. Empty profile fields retain the existing non-clearing behavior.
- The account-service refresh returned before requesting SDK metadata. Refresh that independent metadata after publishing the current accepted-friend relation, with the original connection generation guard. The account service remains authoritative for friendship and display names.

A proposed viewport repaint defect passed against the original implementation. No speculative viewport repaint change was made.

## Verification and boundaries

Base source: `3908b464c17104b28ea37ffd13104b2755a97eea`, with this isolated avatar change. Windows 11, MSVC 14.44, Qt 6.12; ordinary desktop cache `MaiChat/desktop/.cache/build-qt6-ui-20261109`.

The normal App and corresponding test targets built successfully, including the App deployment and PE dependency gate. The old implementation failed the cold-start request and fresh-profile ordering cases; original controls remained green. On 2026-11-15, the same ordinary test binaries were run again: renderer 7 passed, application 5 passed, SDK 4 passed; no failures or skips. Evidence is retained under `.cache/avatar-sync-20261114/`, including `commit-verification-20261115/results.json` and the original RED logs.

Tests use owned SQLite, loopback avatar images and fake SDK callbacks, including stale requests, relogin and worker callbacks. No real account data, paid model request or real IM send was used. This establishes the reproduced code paths, not physical acceptance of the user's current installed App. Source hashes and installed/running product verification remain separate from this test receipt.
