# Mac video opening: owner must map before HUD — 2026-11-15

User clicks the Creation video card, sees a brief central/intermediate window and then a larger relocated player. New phone recording144115250159782623-1794749122-496014169.mov is15335529bytes/9.9367s/~30fps/1920x1080 with rotation-90. Around3seconds its independent controls appear first, followed by a small dark surface and a larger native Space-like transition. Recording contact sheets were bounded local analysis outputs. No screenshot/TCC permission or user-data operation was used.

Actual running user bundle was /Applications/MaiChat.app2.2.64/PID29719, executableSHA256abd17c4fceee7605719707194fee87f5934f7eb63358dd54c1c8aedf3c88b186. Its relevant player scopes equal public265. It was not stopped/replaced. The root's new independent Windows screenshot/fixture is coordinated separately.

## Decisive ordinary native ordering reproduction

A test-only probe calls the original AppKit orderFront/orderWindow/makeKeyAndOrderFront implementations unchanged and records only the test's exact video owner and already-created owned HUD handles; it is restored before teardown. The fixture uses a newly generated1920x1080 SDR file and ordinary MainWindow.openVideoWindow. No model/provider/SDK/account/userDB is involved.

Baseline20native ordering records included8HUD orders before owner.visible, at owner native frame480x270@(400,372) while Qt already held960x540. Owner animation policy was default0 for those first8calls. Only later did the owner map at960x540@(275,230) with animation disabled2. The actual regression failed1case (Qt2pass/1fail). This is distinct from the previous R7 sampler, which started checking dimensions only once the owner was native-visible and therefore missed its independently visible HUDs. Owner-visible rectangles alone were already stable; this evidence must not be mislabeled as a second visible owner-frame resize.

An earlier test-only attempt to require MainWindow native fullscreen did not reach its DidEnter precondition. It is retained as a fixture precondition failure, not a product RED or a statement about the user's computer. That incremental experiment was archived and removed from the final test source. Embedded FFplay resetFfplayOptions also explicitly resets fullscreen0, so stale shared fullscreen state was not adopted as the root cause.

## Minimal implementation

Production scope is only MaiFfplayVideoDialog.cpp/h. On Mac, keep initialShowPending true while show() synchronizes Cocoa geometry and the original Metal viewport. Suppress independent HUD visibility through that mapping interval, establish native styling before any HUD ordering, then show/reveal the existing HUDs after the owner has mapped. A GUI-only initialOwnerMapping guard prevents reentrant completion if Cocoa flushes queued callbacks inside show. Failure/cancel generation checks and direct QWidget.show behavior remain. Windows keeps its original pending-clear/show/geometry branch in this frozen piece, and the MacLayer/renderer/core implementation is unchanged.

The same native ordering regression now records22calls,0earlyHUD/0default-animation, with14HUD ordering calls after a visible owner. Its visible owner rectangle is uniquely(275,230,960,540). Final normal Mac six-selector cohort passed Qt11/0/0: ordering, original SDR/720/1080/nativeHDR prepared geometry, native viewport, failure/cancel, close/reopen and ordinary frameless chrome. The viewport sampler observed64visible native samples with0mismatch. These are actual native call/geometry and GUI results, not continuous WindowServer/compositor pixel capture of the user's account.

## Local artifact and handoff

The normal App and test target built successfully. One private app-only candidate was made with the official script, adapting only its private project root and APFS clone copies. No DMG/ZIP or duplicate validation App was created. Strict signing/runtime closure, unchanged five original model hashes, and two fresh-profile bare-PATH ordinary/debug normal App smokes all passed; actual smokePIDs77142/77342. Candidate version remains2.2.65 plus this exact local R8 overlay, not a new public release.

Candidate: /private/tmp/maichat-video-open-r8-20261115-7g7e09ox/private-candidate/dist/MaiChat-macos-arm64/MaiChat.app

Signed executableSHA256: 52449252a97e8919761f288b21d905a9a0268ce44dd56de565bd0ad23cc35edf

Exact4-path frozen preimage/new/bytes/SHA manifest and independent production2/test2 slices: /private/tmp/maichat-video-open-r8-20261115-7g7e09ox/frozen-handoff/r8-source-test-manifest.json. Dialog cpp311f953f2198aa482416b0a3d1c44f3bb36b6f69/hdb52adca0cd55bdb1e463c4e47493fdc57b73f60 and tests1ea945f5/a8129976 are frozen. Root/house-maichat is authorized to take over the subsequent Windows branch changes against those precise blobs; this Mac actor will not concurrently edit the two production files. Root's Windows actual HWND/DPI fixture independently showed early HUD coordinates; it is not counted as Mac verification or allowed to overwrite the frozen Mac semantics without an exact amendment.

The original installed264 and public265 dist remain unchanged. Final release/tag/asset activity requires the coordinator's future exact source/lease; no GitHub or online service write occurred. SyncR2 remains pending user deployment authorization. Disk-cleanup completion is unrelated to deployment authorization.


## Windows follow-up consumed without changing Mac semantics

The coordinator supplied a2820byte rawCRLF patch/SHA2565ebfddb2585bc24fa3b47ccedcc249fc2707e55c15000c81017fb449afd672ef, canonicalLF2751bytes/SHA256a354f68e0102556456cd8ef886bb39b614bbaa1b39cd17b1d7024d06b2ec4d0b. Exact R8 preimages311f953f/db52adca were checked, and only the two delegated production files were applied once. New canonical blobs are cpp86e5672efe6aa89d0437c4c5738d316a845f004a and header1256831d410582e46a9c1324249fd8b45f51ac04. Every addition is guardedQ_OS_WIN: a windowsOwnerMapping transaction around setVisible, HUD visibility suppressed during first native map, then ordinary layout/reveal after QDialog mapping has returned. MacR8 implementation and original Mac test blobs stay unchanged.

Independent static review found no concrete P1/P2 in that supplied delta. The Windows-only standalone fixture was reviewed as an artifact, not inserted into the Mac tree without its target definition and not claimed to run on Mac. Two nonblocking test-output issues (empty environment still creates a '-tag' output basename; optional screenshot save not asserted) were passed to the coordinator and subsequently corrected in its independent test.

The Mac normal consumer target rebuilt successfully in21.377seconds. Only the needed native-order and failure/cancel fixtures were rerun: Qt4/0/0 including init/cleanup,22native orders,0earlyHUD and0default-animation orders. The existing private R8 Mac candidate was not duplicated/repackaged for Windows-only source additions; installed and public bundles remained unchanged.

The coordinator reported source commit267723bf24c08abfcb7f77b86ca097cee4c0de5c, actual Windows new4/0 and original7selectors9/0, normal build/runtime gates0, actual owned PNG inspected, and a private ZIP in progress. Its separate desktop_computer150%DPI two-position capture-to-pointer case passed without changing input production code; this does not prove a third-party slider fix and is not relabeled as a Mac run. SyncR2 still lacks a deployment lease. Exact apply and Mac consumer receipts: `/private/tmp/maichat-video-open-r8-20261115-7g7e09ox/windows-followup/`.
