# Android Creation image cards reserve excessive blank height

## Reproduction and expected result

The user supplied a photo of the Android remote Creation session at 20:33 on
2026-11-15. Two loaded landscape computer screenshots have a large blank gap
between them and before the following reply text. On the connected OPPO PKD130,
the normal installed App was 282/2.2.40. Its same-account original remote session
was already open; no model/tool task or image upload was started for this check.

Actual screenshot and UI hierarchy at 20:36 reproduced the layout. The first
remote image card was `[48,201]-[672,681]`; its ImageView was 440 by 480 pixels.
The fully visible screenshot occupied about 440 by 275 pixels. The next image
reserved the same 480-pixel height. About 205 pixels of vertical space per card
were not image content. The expected result is a card measured to the complete
image's aspect ratio, followed by ordinary paragraph spacing, with the original
click-to-preview behavior and no image crop.

The base source is main `5744d039` / product `2f92e555`. Evidence is in
`.cache/android-agent-image-spacing-20261115/`, including the inspected actual
device screenshot and hierarchy. This UI request does not authorize the pending
online SessionSync service deployment.

## Cause and repair

`AIAssistantPanel.showAgentMediaCard` used a fixed 220 by 240 dp FrameLayout for
images, with a MATCH_PARENT `FIT_CENTER` ImageView. Only videos later resized the
parent from metadata. A 16:10 screenshot needs approximately 220 by 138 dp, so
the image fit inside the large frame while its unused height remained in the
message layout. This is internal card geometry, not a paragraph margin.

The image branch now reuses `MaiImageThumbnailView` and `MaiImageMessageRow`,
which already measure complete IM images inside the available parent width.
Both the image child and preview parent wrap their measured content. A cached
bitmap delivered before attachment and an asynchronous later bitmap update both
recompute the same view's size. The existing width-aware row also handles narrow
and subsequently expanded parents without clipping or retaining stale height.
Creation keeps its existing rectangular image appearance. Loader request tags,
failure scope/signature checks and the actual ImageView passed to fullscreen
preview remain intact. The video FrameLayout and metadata/playback path remain
the ordinary existing implementation.

## Verification before installation

The new ordinary Robolectric fixture invokes the real private media-card method
and measures actual Views; only controller initialization and image decode
delivery are replaced with owned seams. The baseline actually failed three of
four cases: landscape/asynchronous height 240 instead of 138 dp, and portrait
width 220 instead of 60 dp. The existing video fallback case passed. A separate
narrow-parent case also failed before the production change.

After the change, media-card 5, existing thumbnail 2, image-message-row 6 and
tool-summary 4 cases passed: **17/0**. They cover 16:10 and 16:9 screenshots,
portrait/square/ultrawide images, warm/cold delivery, actual following-text top,
the same View/Bitmap, and parent widths 120/400/0/90/400. The original complete
SessionDrawer suite also passed, including remote image cache/loading/failure,
explicit retry, stale callback and offline recovery behavior. This is not yet an
installed-build acceptance result.

The next normal phone update is 283/2.2.41, built from an isolated checkout that
excludes the original MobileAgentAdapter WIP. The user's existing instruction to
install normal fixes on the phone remains in force. The installed artifact,
process and actual image/text geometry must be checked after that update; the
separate server deployment still requires its pending operational go-ahead.
