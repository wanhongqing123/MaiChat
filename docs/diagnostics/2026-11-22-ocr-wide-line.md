# OCR over-wide detected line: controlled investigation

## Reproduction and expected result

On Windows x64, call ordinary `maiOcrRecognize` and desktop `ocr_image` on an owned 2464×1600 white raster containing a continuous Consolas 24 px line and a separate Arial 48 px `HELLO 12345` label. Repeat at 50% and 25% using area resize, then on a 600×500 left crop. No screenshot, user document, QR code, credential, database or provider API is used.

Expected: recover bounded fragments of the wide line when safe, retain the independent label, return original raster dimensions and fragment quadrilaterals. If recovery cannot cover a detected source line, report `partial=true` and increment `skipped_lines` once for that source line; an all-skipped image must have this metadata even with zero blocks. Cancellation and hard failures publish no result.

## Evidence before production changes

- Time: 2026-11-22 17:06–17:12 Asia/Shanghai; source HEAD `38d25b9d` on `main`, with unrelated pre-existing work preserved.
- Cache: `MaiChat/desktop/.cache/build-qt6-ui-20261109`; targets `mai_ocr_test`, `desktop_ocr_tools_test`, max 3 build jobs. These test processes use bundled PP-OCRv5 mobile `Media/data/ocr/{det.onnx,rec.onnx,dictionary.txt}` and ORT `MaiAgent/build/vendor/onnxruntime-windows-x64/onnxruntime.dll` (1.26).
- Existing baseline: both targets built; CTest 2/2 passed in 5.17 s.
- New owned RED: full, 50% and 25% each return status 6 (`MAI_OCR_LIMIT_EXCEEDED`), exact error `OCR text line exceeds recognition width limit`, count/width/height all zero. Nine assertions fail, covering status, preserved dimensions and useful label at each scale. The narrow crop returns status 0 and two regions including `HELLO`.
- The initial 20 px fixture missed detection at full size but reproduced at both reduced scales. Changing only the owned font to 24 px made all three reproduce; the final line has eight repetitions and fits the raster. This is detector sensitivity, so the width condition must not be described as an absolute pixel limit.

## Root cause

`MaiOcrEngine::recognizeRegion` measures opposing edges of the expanded, original-coordinate detection quad, rotates vertically elongated regions, and computes `ceil(48 * width / height)`. It rejects values greater than 2048 before perspective warp or recognition inference. The exact continuous threshold is width/height > 42⅔ (floating-point/ceil included). A uniformly resized source preserves aspect ratio. Detection additionally always scales its longest axis to 960 and aligns axes upward to 128, which often restores nearly the same detector geometry after downscaling.

`maiOcrRecognize` returns immediately on any region failure, before publishing its owned result. Consequently, one wide region suppresses every other line, including independently recognizable labels. `DesktopOcrService` forwards that error; the JSON tool has no usable result to encode. This path is shared ordinary code, not a tool-only crop problem.

## Frozen implementation plan

1. Keep recognition height 48, padded width <=2048, existing model/CTC validation and original source raster. Export constants for explaining these normalized recognition dimensions.
2. Append `partial`, `skipped_lines`, `segmented_lines` to `MaiOcrResult`; mirror them in `DesktopOcrResult` and tool JSON. `segmented_lines` counts source lines for which at least two fragment recognitions were attempted; `skipped_lines` counts source lines incompletely recovered. Ordinary low-confidence unsplit filtering keeps its existing behavior.
3. Measure and orient a quad once; subdivide along its reading axis. Use the same interpolated source quad for crop and result geometry, including vertically rotated text. Return real fragment blocks; never silently stitch or drop overlap text.
4. Prefer bounded local whitespace cuts near evenly spaced boundaries. Search rectified patches no wider than 192 px at height48, choose a blank column run, and use shared nonoverlapping cuts. If a safe cut cannot be established, skip the source line honestly instead of cutting an unknown character.
5. Cap at eight recognition fragments per source line and 512 recognition inferences per image. Reserve one inference for each remaining detected candidate, so a difficult early line cannot consume the independent labels' budget. Exceeding only a wide-line recovery budget is local/partial. Preserve hard errors for model/runtime/OOM/cancellation and global candidate/result/text limits. Bound recognition elapsed work at 30 seconds, checked between inferences; a single ORT inference remains nonpreemptible and deadline exhaustion is an atomic global limit error.
6. Add RED tests for exact geometry threshold, vertical slice coordinates, original-scale label positions, all-wide zero-block partial results, split recovery metadata, and cancellation after fragment work. Add tool parity tests to prevent metadata from being lost in adapters.
7. Build only the two leased OCR targets and rerun the same fixtures plus existing OCR tests. Inspect owned annotated output where generated. Do not change release/version/tag/index or the concurrent frozen release cache.

## Implemented result and verification

After the parent froze the contract, the ordinary core gained bounded quad subdivision with local whitespace searches. The desktop ordinary API and tool share the same metadata. Recognition still uses the existing bundled models and runtime, with hard errors and cancellation publishing no result. `MaiOcrResult` and `DesktopOcrResult` have appended fields; all consumers must be rebuilt together rather than mix old compiled callers with the extended structs.

At 17:18 Asia/Shanghai, the two rebuilt targets passed CTest 2/2 in 8.10 s (`mai_ocr_test` 3.86 s; `desktop_ocr_tools_test` 4.21 s). `git diff --check` reported no whitespace errors. The final owned native observations were:

| Input | Status | Blocks | partial | skipped_lines | segmented_lines |
| --- | --- | ---: | --- | ---: | ---: |
| 600×500 narrow control | OK | 2 | false | 0 | 0 |
| 2464×1600 original | OK | 4 | false | 0 | 1 |
| 1232×800 (50%) | OK | 4 | false | 0 | 1 |
| 616×400 (25%) | OK | 1 | true | 1 | 0 |
| 16384×160 all-wide line | OK | 0 | true | 1 | 0 |

The full and half-size images return three genuine fragments and the independent label. Quarter-size area filtering leaves no safely identified whitespace boundary, so that source line is explicitly skipped and the label remains. Tests also check the 2048/2049 normalized threshold, vertical source slicing and rotation, actual label positions, finite/in-bounds coordinates, nonoverlapping neighboring fragment quads, a canceled call publishing fully zero metadata/geometry, existing malformed models/CTC validation, and ordinary/tool parity for all new metadata.

Owned artifacts are under the leased cache's `ocr-wide-line-results/`. `ocr-wide-line-owned-annotated.bmp` was losslessly converted to PNG with built-in System.Drawing for inspection; the PNG was viewed. Three red fragment outlines cover the owned continuous line, and a separate outline covers `HELLO 12345`. No user pixels are present.

Artifact SHA-256 at that run:

```text
mai_ocr_test.exe       3DCFC75F03CAA36E17D0C6B59645B9DE4974E52F942AA2A200B93B735C17D8F9
desktop_ocr_tools_test 3525DF7FF353FEA1C00669C839544F148F774EEDC6274D7B75EB2FD3BB79E0B3
onnxruntime.dll       F7390553D354692035DB8521D684EAEC0DEC9FEBFAD9493CC5F20B0538F01601
det.onnx              A431985659DC921974177A95ADCFBB90FD9E51989A5E04D70D0B75F597B6E61D
rec.onnx              DA72DC72CA4DC220DF0DFDE68C1DEDC31C58D3E76A25871122E5056227D50092
```

## Verification boundary

Root final integration subsequently rebuilt the ordinary Windows `maichat`,
`mai_ocr_test` and `desktop_ocr_tools_test` targets against the extended structs.
The final two OCR targets passed again (CTest 2/2, 8.53 seconds); a private staging
and actual ZIP extraction passed byte equality and separate bare-PATH smoke.
The development executable SHA256 is
`aa3cf53426865a5bbbd909af783fd994c7224f95a502427d79155ee9c8668575`.
This uncommitted development tree retains metadata 3.1.0; it is not public 3.1.2.

The Mac owner independently consumed the verified source-only packet on an f47
private baseline, rebuilt normal App/ABI consumers, and ran the same supplied
owned BMP at full/50%/25%. Counts/partial/skipped/segmented results matched the
Windows table, and HELLO remained at each scale. Original-coordinate fragment
geometry and nonoverlap checks passed; annotated images were inspected by that
owner. The Mac Qt tool test target is guarded by WIN32&&OPENCV, so no Mac runtime
tool-JSON test is claimed: the adapter compiled in the normal App and its shared
JSON metadata path was reviewed; ordinary engine inference was actually run.
The old guard-related unknown-target failure is retained as a test-configuration
boundary, not counted as an OCR production failure.

The evidence covers controlled Windows ordinary API/tool tests and Mac ordinary inference/normal compilation; it is not acceptance of the user's screenshot, an installed app, Mac runtime tool JSON, iOS or Android. No running user application has been stopped or replaced. `partial` describes detected wide-line recovery, not a guarantee that the detector found every text element or that every recognized character is correct. Whitespace selection is a bounded raster heuristic; unsafe boundaries remain partial rather than silently joining text. The 30-second recognition deadline is checked between bounded work units and cannot interrupt an individual ORT inference.

## Review follow-up: distinguish solid foreground from whitespace

An independent review identified that a vertically uniform column could be either background or a full-height glyph stroke. The original `max(column)-min(column)<=16` predicate accepted both. At 17:27–17:30 Asia/Shanghai, the selector was extracted unchanged into an internal bounded pixel helper, and two owned 192×48 light/dark fixtures were added: every interior column had varying ink except a four-column, full-height solid foreground run at the nominal center. Both assertions requiring rejection failed before the heuristic changed; the media test reported exactly two failures, while the earlier recovery observations remained unchanged.

The corrected selector requires independent background evidence. For a 48-high patch it samples the top and bottom four rows, requires their medians to agree within 12 grayscale levels and at least 80% of each margin to match that shared background within 12. The body must also contain at least 48 contrasting pixels differing by 32 or more, so a completely uniform patch cannot establish a background/foreground distinction. A candidate remains a run of at least two columns with vertical range at most 16, and now **every candidate pixel must additionally match the sampled background within 12**. The predicate is symmetric for light and dark themes. Marginless, textured, low-contrast or ambiguous patches remain partial rather than permitting uncertain cuts. The 192×48 bounded patch size, model dimensions and inference budgets are unchanged.

The new tests reject full-height foreground in both themes, choose a separately cleared background gap at columns 70–74, reject inconsistent top/bottom margins, and reject fully uniform patches. The first focused GREEN rebuilt both OCR targets and passed CTest 2/2 in 9.66 seconds (4.70 s media, 4.91 s desktop). The defensive, normally planner-unreachable width error also now reports the actual measured normalized width, recognition height 48, maximum width 2048 and source quad dimensions instead of the old unmeasured generic message.

Final rebuild and verbose CTest at 17:31 passed 2/2 in 8.49 seconds (4.11 s media, 4.33 s desktop), retaining the same full/half/quarter/all-wide result table above. Current test executable SHA-256 values supersede the earlier pre-review artifacts:

```text
mai_ocr_test.exe       D128E9060C138988ABAC8963DFF5BFCAE5439748F10E0C8B36E1FAB5E299AC09
desktop_ocr_tools_test B6E772B0FCB976600671820296B6983795CC8CB576BBA14A711167D05527ADD6
```

Reproduce from the repository root in PowerShell:

```powershell
. .cache/qt-ui-boundary-20261108/enter-vs.ps1
cmake --build MaiChat/desktop/.cache/build-qt6-ui-20261109 --target mai_ocr_test desktop_ocr_tools_test --parallel 3
ctest --test-dir MaiChat/desktop/.cache/build-qt6-ui-20261109 -R '^(mai_ocr_test|desktop_ocr_tools_test)$' --output-on-failure
```
