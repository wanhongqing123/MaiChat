# Accepted peer avatar before SDK friendship synchronization

## Reproduction

The account service returns an accepted peer, the SDK friendship list is empty, and conversation metadata contains an old face URL. The SDK's general user-profile fixture has the new face URL. Refreshing the account list must independently query the accepted peer profile and update its avatar without changing the account-service nickname or friendship authority. Later older friend/conversation metadata must not roll it back.

## Cause and change

The account backend does synchronize SDK friendships, but asynchronously. An accepted peer can legitimately remain outside the SDK friendship list while synchronization is pending or retrying. The previous desktop fix refreshed only that SDK list; its only general profile query concerned self. Android already had an accepted-ID metadata path independent of SDK friendship.

Desktop now exposes an ordinary `refreshUserProfiles` metadata operation and calls it with accepted peer IDs after publishing the account relation. The SDK adapter normalizes/deduplicates IDs, excludes self and batches at 100. It uses separate peer-request and login generations on the QObject owner thread, validates account/login again at completion and accepts only IDs actually requested by that batch. It does not create SDK friendships or grant message permissions.

Explicit peer profile URLs outrank SDK friend URLs, which outrank conversation URLs. Empty profile fields retain the existing non-clearing behavior. The separate peer request generation does not cancel the self-profile callback. The request includes the existing compatibility force-update field; the vendored C header marks that field deprecated, so a forced live-network refresh is not claimed from fake SDK tests.

## Verification

The owned normal App test first failed because no peer profile request occurred. After the change, the ordinary Windows App and targets built successfully, including three-binary/114-PE deployment checks. Complete App tests: 48/0; SDK tests: 40/0; original avatar render selectors: 7/0; binding suite: 54/0; file UI suite: 6/0. New SDK cases cover 101 IDs split into 100+1, owner-thread callbacks, foreign IDs, stale requests/logins, synchronous logout between batches and empty/error fallbacks.

Evidence, original RED and source hashes are retained under `.cache/desktop-accepted-peer-avatar-20261115/`. Tests use owned Account/Fake SDK/SQLite data and loopback images. No real profile service, model request or running user App was used. This closes the reproduced metadata path; current installed-client acceptance and actual SDK freshness remain separate checks.
