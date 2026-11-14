# Android remote-image account identity

2026-11-15, normal Android device PKD130 / Android 15 / arm64.

## Reproduction and expected result

1. Keep the existing authenticated account and open Creation Center.
2. Open its computer-owned `Imported conversation` and scroll to an existing image part.
3. Let the ordinary image loader read that part; use the same card for the post-fix check.

The image must use the current authenticated account service identity while retaining
the image's original publisher tuple. A normal login must not produce an account-switch
error merely because its IM identifier differs from its public account identifier.

## Observed build and evidence

- Installed normal version: 277 / 2.2.35, PID 18123 at capture.
- APK SHA-256: `3825745f11c55d8da99d97468ab30a158462bbd748df06905be342ae1fb76d44`.
- Source: `38480181` plus the exact frozen single-image client overlay, subsequently
  committed in `57b92599e1e67b3f6698d6a30ff5e2e6d43a7c5d`.
- Two existing image cards displayed the account-switch error. Owned local evidence:
  `.cache/android-image-republication-20261115/normal277/image-before.png` and
  `image-older.xml`. No account database, tokens or application preferences were read.
- The default Controller-to-adapter regression and two login-epoch cases compiled and
  failed naturally: `.cache/android-image-auth-identity-20261115/red.log`, 3/3 failures.

## Cause and correction boundary

`AIAssistantController.accountUserId` is the IM login identity used for the existing
workspace. The image request and default adapter also used it as `account_id`, while
`MaiSessionAttachments.requireImageAccount` compared it with `MaiChatAuthClient.accountId()`.
Those identifiers belong to different domains and can legitimately differ.

The correction must capture a separate authenticated account-service identity and login
generation for image requests, cache partitioning and adapter validation. It must preserve
the IM workspace identity and reject old results after account replacement or same-account
relogin, including replacement during credential refresh or native download. The original
publisher login remains part of the immutable image receipt; it is not the reader identity.

This issue is independent of expired-image republication and does not justify weakening
the account guard or uploading an otherwise healthy image again.

## Verification boundary

The correction in `23f8ee74747f8480f0a4c78ede469a9aea854952` passed 43 targeted
Controller/cache/adapter cases and 28 affected drawer/history cases, with no failures or
skips. Normal version 278 / 2.2.36 built successfully; its APK SHA-256 is
`806397e6360c85e505eb9007c1f2e6d759e008260356464f41e41ea1eb157c5f`.
The signature matches the installed baseline and all 26 native libraries and the bundled
model match the previously verified normal package. Data-preserving installation succeeded;
ordinary launch was verified at PID 21925.

Physical old-image reads no longer fail with the account-switch error. They next reached
a generic network/service failure, preserved in
`.cache/android-image-auth-identity-20261115/normal278/images.png`. That remaining failure
is being diagnosed separately; this checkpoint does not claim visible-image acceptance or
server-side renewal success. The earlier 63 passing client tests were insufficient to
establish the real account-domain boundary.

The subsequent scalar-only observation reported `FILE_ERROR`, HTTP 200 and curl code 0.
The separate Android atomic-file cause is recorded in
`2026-11-15-android-image-atomic-save.md`; the generic UI text did not identify that cause.
