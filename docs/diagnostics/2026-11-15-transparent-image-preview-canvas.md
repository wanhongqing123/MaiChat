# Transparent PNG preview canvas

## Reproduction and expected result

Open an IM image whose PNG has transparent outer pixels, such as the binding-dialog capture. The original source has alpha zero around the white rounded dialog. Its transparent margin must have a readable light canvas in the preview; opening and closing snapshots must use that same canvas. Opaque images must retain the existing dark canvas, including opaque pixels stored in an RGBA container.

## Evidence and change

The source PNG was reported as RGBA 1040 x 960, with alpha zero at its outer corners. The user screenshot's dark margin sampled RGB 8, 9, 11, exactly the existing ImagePreviewDialog canvas. A private probe of the actual Dialog reproduced this directly, without SDK conversion or a JPEG hypothesis.

The Dialog now checks actual pixel alpha once on load and caches its canvas color. Actual transparency selects white; opaque RGBA and RGB keep RGB 8, 9, 11. It never modifies or flattens the source image. MainWindow's existing opening and closing snapshot labels use the cached canvas, so transparent pixels do not reveal the underlying conversation during handoff. The check does not repeat during resize or paint.

## Verification and limits

Windows 11, Qt 6.12, normal desktop target. The original private Dialog probe failed the transparent-border case; after the change all four cases passed: transparent border, opaque RGBA, opaque RGB and opaque 4K. Source pixels remained identical; the 4K one-time constructor measurement was approximately 21.2 ms. Root inspected the captured transparent and opaque examples.

The normal MainWindow test target compiled successfully. A focused run on 2026-11-15 passed seven checks with no failures or skips, including actual transparency, original native-pixmap preview, single maximized preview, and normal/maximized conversation-background stability. Evidence is under `.cache/image-preview-alpha-20261115/normal-ui-regression-01/`; the probe RED/GREEN and screenshots are preserved alongside it.

These checks cover rendered Dialog pixels and the existing preview operations. They do not establish continuous native desktop frame capture or Mac physical acceptance. No video player, SDK image encoder, source image bytes, user installation or running user App was changed for this fix.
