# Video first native presentation and desktop default size — 2026-11-15

## Reproducible operation and expected result

Creation → click a local HD video badge. The user supplied a 7.5-second phone movie, `144115250159782623-1794721443-822591962.mov` (11,190,916 bytes). Around2.4–2.6s the dark video region covers part of the final area before spreading. User also reports Mac nearly fills the display whereas Windows opens smaller. Expected: one normal window, prepared viewport from first native map; no small-to-large presentation; a common moderate default on both desktops. Fullscreen/maximize stay explicit operations.

Actual user process was read-only identified as PID10791 from the final public 2.2.63 local dist App, executable source `d9736686998990503256fc6db4998e2bdabb553d`. It was never stopped, replaced, or used as an automated fixture. Existing v2.2.63 tag/assets stay unchanged.

## Root-cause evidence and limits

- Ordinary MainWindow.openVideoWindow with an owned1920x1080 H.264 fixture, current normal Cocoa target; no extra player activation helper.
- Baseline first native-mapped sample at335ms: Qt/NSWindow/surface1285x723, but Metal frame, Metal presentation and parent presentation bounds480x270. Correct at474ms. New ordinary test was2pass/1fail. Native/window geometry itself did not change.
- A first attempt using Qt client size and a CoreAnimation transaction/flush alone remained RED with3 mapped mismatches. Qt wraps the supplied Metal layer in QContainerLayer; its deferred layout still restores the old content frame.
- Final synchronization includes the container/native subtree layout in the same GUI turn after Qt show has applied pending native geometry. Final same-source affected run sampled65 native-mapped frames at960x540: client/Metal/drawable/parent presentation sizes all match from first sample, zero mismatches. CoreAnimation animation counts are zero.
- The first baseline sample had native visibility but no active-Space/occlusion receipt yet. Therefore these are native mapping/layer measurements, not a claim of continuous WindowServer compositor pixel recording. The phone movie remains independent user-visible evidence. No Screen Recording/TCC request or permission bypass was made.
- Mac owner window now explicitly opts out of AppKit popup animation before ordering. This is the desired no-zoom presentation contract; native animationBehavior=2 is observed. It does not replace the existing native fullscreen/close state machine or claim that default animation alone was the proven layer defect.
- Initial missing CATransaction import produced compile exit2, preserved in `layer-fix-build.log`; after adding the required import, normal builds exit0. Earlier REDs remain separate from final GREEN.

## Common default window policy

The prior code was shared: source size fitted to85% of available logical screen area. Independent Windows baseline: logical1707x1067, available1707x1019, DPR1.5, nativeDPI144; source640/1280/1920 widths yielded Qt640/1280/1451, all ordinary state0. Native visible geometry was already settled at WM_SHOWWINDOW. This is not evidence of Mac entering native fullscreen or a second implementation.

The new common cap is960x540 logical points, bounded further to75% of a smaller available screen, preserving the original source-aspect/minimum-control fit. Small material keeps the existing minimum constraints; explicit customized/docked/maximized/fullscreen guards are unchanged. New720p/1080p original-entry data rows first failed width<=960 against the old policy, then pass after the common change.

## Changes and actual verification

Only production files MaiFfplayVideoDialog.cpp and MaiGraphicsMacLayer.mm plus their two existing test sources. No MainWindow/Renderer/Media/SDK/provider change. Patches, previous/new blobs and raw/canonical SHA256 are in `/private/tmp/maichat-media-open-smoothness-20261115-09pekztz/frozen-handoff/r7-final-manifest.json`.

Normal Mac target:16pass/0fail/0skip (17.963s), including smallSDR,720p,1080p,nativeHDR, native first-map layer sampling, preparation failure/cancel/reuse, actual drawable pixels while resizing/paused/playing, HDR video rect,9edges, fullscreen-close opacity and Creation native fullscreen Space HUD/icon/real native control hits. This is an affected group, not an unrelated UI/model/device sweep.

Private App normal build0 (6.498s), app-only packaging0 (33.135s), strict signature/runtime/five-model reference and ordinary/debug isolated smoke all0. Actual smoke PIDs11209/11411; signed executable SHA256 `7611a969353b79b6b1d059b5719a7cd0d22894bd73e859aa2c0e9f0c43d0ce34`.

Candidate: `/private/tmp/maichat-media-open-smoothness-20261115-09pekztz/private-candidate/dist/MaiChat-macos-arm64/MaiChat.app`. It is2.2.63 plus the exact private R7 overlay, not a newly published version. Finder selected it for the user; no launch or quit was performed. Source and diagnostics/index remain preserved on main. Windows new-cap comparison and coordinator independent review are recorded separately; they are not substituted for Mac compositor capture.

## Concurrent independent work

Coordinator thinking-start8-source candidate was materialized and blob-verified only in a private review directory. Read-only review found no concrete P1/P2; it was not applied to this media candidate or used to claim Apple runtime results. Future combined publication requires a new frozen committed source and a new Release/asset lease, preserving the public2.2.63 release.
