# Bundled local OCR models

These public model assets are distributed by the official PaddlePaddle organization,
under Apache License 2.0 (see `LICENSE`). They contain no user images or credentials.
The runtime is the product's existing ONNX Runtime 1.26, loaded in process; OpenCV
core/imgproc provide image geometry. No additional inference engine is included.

| File | Official source / pinned revision | SHA-256 |
| --- | --- | --- |
| `det.onnx` | [PP-OCRv5_mobile_det_onnx](https://huggingface.co/PaddlePaddle/PP-OCRv5_mobile_det_onnx/tree/e6f4fa85f00e168c862bc462aebca69eef9b3d3d), `inference.onnx` | `a431985659dc921974177a95adcfbb90fd9e51989a5e04d70d0b75f597b6e61d` |
| `det.inference.yml` | Same detection revision, `inference.yml` | `98069072e1b6b37d727fd9d9f11725faa46d6ea0de012f2ed26caea011c37699` |
| `rec.onnx` | [PP-OCRv5_mobile_rec_onnx](https://huggingface.co/PaddlePaddle/PP-OCRv5_mobile_rec_onnx/tree/ed152b8b495f84de93cda5709d768548a9127622), `inference.onnx` | `da72dc72ca4dc220df0dfde68c1dedc31c58d3e76a25871122e5056227d50092` |
| `rec.inference.yml` | Same recognition revision, `inference.yml` | `5dfeb2777f6d0db8177d8128a8acfcf6e6276dc4ac73ea3bf0dc06d6a5e85d8e` |
| `dictionary.txt` | Extracted only from the pinned recognition `PostProcess.character_dict` | `d1979e9f794c464c0d2e0b70a7fe14dd978e9dc644c0e71f14158cdf8342af1b` |
| `LICENSE` | [PaddleOCR Apache license](https://github.com/PaddlePaddle/PaddleOCR/blob/main/LICENSE), retrieved 2026-11-20 | `3840c5c0c61c294264d2dd77b8777be6ddd90121ef4e0e64abcd22edea581d6e` |

Dictionary extraction preserves YAML sequence order and scalar text, unquotes
single-quoted YAML scalars (including doubled apostrophes), writes UTF-8 without
BOM and one LF after each entry. It has 18,383 entries, including ten flag graphemes
that contain two Unicode scalar values. The CTC decoder prepends the blank label
and appends ASCII space, for 18,385 model classes. Do not substitute another version's
dictionary: initialization checks the exact output class count.

The model configurations specify BGR input. Detection uses resize-long 960, dimensions
rounded up to multiples of 128 (the official `DetResizeForTest.resize_image_type2`
contract, with a resulting longest edge of 1,024), normalization scale 1/255,
mean [0.485, 0.456, 0.406],
standard deviation [0.229, 0.224, 0.225], DB threshold 0.3, score threshold 0.6,
and expansion ratio 1.5. Recognition uses height 48, minimum padded width 320,
aspect-preserving dynamic width, normalization (pixel/255 - 0.5)/0.5, and CTC.
Resource budgets in `MaiOcr.h` intentionally constrain inference and fail explicitly
instead of silently omitting over-budget text. Image processing and polygon expansion
are original host code using existing OpenCV geometry, informed by the official
[PaddleOCR DB postprocessor](https://github.com/PaddlePaddle/PaddleOCR/blob/main/ppocr/postprocess/db_postprocess.py)
and the already vendored OpenCV `modules/dnn/src/model.cpp` DB geometry.

OCR returns text regions; regions are visual evidence and do not establish button
semantics or whether an area accepts clicks.
