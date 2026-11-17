# macOS video controls stranded on another Space

## Report and actual artifact

On 2026-11-15 the user reported that video controls disappeared during playback, while a detached title/footer remained over Creation. The installed /Applications/MaiChat.app was confirmed read-only as 2.2.62, PID 52422, executable SHA256 c99f6bf27fcd16120b0a406740f22f8926ea8f7dbf1e03f9128068ebc5fa4127, identical to the published payload. This report is not attributed to an older installation. The agent did not stop, replace, or operate that player or the real film.

## Reproduction and decisive RED

An owned clip is opened through ordinary MainWindow.openVideoWindow. The test player is not passed through the helper that sets MoveToActiveSpace or provides stronger native activation. After a real DidEnter notification, the video is key, visible and onActiveSpace; Qt activeWindow is the player and both HUDs are logically/native visible. Nevertheless both HUD native windows have onActiveSpace=false, occlusion=false, order=-1, no parentWindow and collectionBehavior=258. Their Qt transientParent alone did not form an AppKit window group. The first two runs retain the original failure. An early activation timing variant is recorded separately and is not the deciding evidence.

The earlier R4 tests placed the player on the current desktop using an activation helper, and tested ordinary-window z-order without asserting HUD native Space membership. Those passing tests did not establish this user operation. The original 2.2.62 publication remains unchanged.

## Fix

Only MaiGraphicsMacLayer.mm changes product behavior. It binds the persistent native HUDs as children of their actual video NSWindow and removes MoveToActiveSpace/FullScreenPrimary from the HUD policy while retaining FullScreenAuxiliary. Native child-window assignment occurs only when the owner differs, and policy assignment only when needed. Existing Qt objects, flags, parent QObject chain, playback paths, window level, HUD non-resizable behavior and R3 fullscreen observer/close sequencing remain unchanged. Dialog cpp/h and MaiVideoHud.h are unchanged.

## GREEN and necessary regression

The same ordinary entry now brings the HUDs into the actual fullscreen Space, with visible occlusion and the same native owner/width. Returning to Creation restores its original Space: video and HUDs are off that active Space, and both HUDs are hidden. Returning to video restores both kinds of control icons, including 992 bright pause pixels and 120 bright more-button pixels. Real AppKit button events pause the actual playback and open/close the real menu. Closing completes DidExit and deletes the old Qt owner/HUD objects; reopening binds fresh identities to the new owner.

The final normal-library run passes 14/0/0 in 41.698 seconds, including the new roundtrip and original close/reopen, native nine-edge resize, keyboard, title/min/max/fullscreen restore, initial maximized/fullscreen restore, IM wide/narrow navigation, HDR geometry and native resize checks. Build and run both exit zero. Tests use owned content, Fake IM and no paid providers, real SDK sends, user DB or Clipboard. No TCC request or screen recording was used.

## Native cache and fixture boundaries

The ordinary Qt SDK itself retains closed WindowServer numbers/NSWindowController objects with contentView=nil, visible=false and parentWindow=nil. A separate QWidget+MaiVideoHud baseline without addChildWindow reproduces this; original logs are retained. Therefore the final lifecycle test requires deletion of the old Qt objects and absence of old native UI/content/parent links, rather than falsely claiming SDK cache identifiers disappear. The initial overly strict identifier failure is retained.

Fixture-only failures are preserved: a native menu test initially omitted a timed close for its nested event loop and reached timeout124; it now follows the existing timer pattern. Initial test-window placement is stabilized only for the fixture MainWindow, and its normal Space policy is restored before opening the product player. No player policy/activation helper masks the tested entry. No older-OS or continuous compositor-frame claim is made.

## Delivery state

R5 production and tests are frozen as one product source plus two original test sources. Final release/installed-artifact identity will be recorded when the successor build is delivered; current installed 2.2.62 is not claimed updated by these source tests.

## Actual Creation page visual follow-up

Final visual inspection showed that the initial fixture MainWindow defaulted to Messages, even though it called the same ordinary video entry. Five test-only lines now click agentNavButton and require agentPage to be visible before opening the owned video. The complete native Space/UI/close/reopen scenario passes again, 3/0/0 in 5.135 seconds. The agent personally viewed the resulting Creation screenshot; its title is Creation and no detached title/footer is present. The original 14/0/0 affected regression cohort remains recorded separately; the product and native test helper did not change for this follow-up.

The normal App candidate build also exits zero. It is a private 2.2.62 plus exact R5 overlay build and has not been published or installed. A successor release must carry its actual frozen source/version; existing public v2.2.62 assets and the installed user application remain untouched.
