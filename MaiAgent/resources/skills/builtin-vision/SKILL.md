---
name: builtin-vision
description: Inspect images, OCR and video-analysis results using the actual platform vision tools.
---

# Image and vision workflows

Start with `tool_help({"arguments":["vision","-h"]})`, then load the selected
tool's exact schema through `tool_search`. Only the tools actually listed are
available here. OpenCV operations are separate typed tools; there is no generic
OpenCV command line or automatic `-h` support on every tool.

Use `view_image` for an accessible image when direct visual inspection can
answer the question. A model must support image inputs; if the tool rejects the
selected model, report the limitation instead of claiming to have seen pixels.
For OCR or geometric measurements, prefer the registered specialized tool and
read its returned confidence/coordinates. Screen capture and local image files
have different permission and path boundaries.

For a complex edit, inspect the input dimensions/orientation and identify the
region or property the user wants changed. Select the narrow operation that
matches it, save to a new output, then inspect the result at a useful scale.
Respect the operation's channel, alpha, mask and coordinate conventions from
its actual schema. Do not substitute detection labels for visual evidence or
pretend a bounding box establishes an object's identity.

For video analysis, use the actual duration/frame timing from ffprobe and the
registered sampling/analysis interface. Explain whether evidence covers selected
frames or the full timeline; a few frames cannot prove nothing happened between
them. Keep source media intact and do not download or upload it unless the
requested tool workflow and permission allow that operation.

If image preparation or analysis fails, preserve the reported error and choose
a supported size/format or another available narrow tool. Never call a paid
provider merely to test whether a local vision feature is working.

## Choose the measurement before transforming

For screenshot differences, use cv_image_compare only if it is registered. Its
size normalization is not spatial registration or HDR/ICC color management.
When same-size screenshots differ by a small translation, use the available
cv_register_translation to estimate shift/confidence first; that result alone
does not warp or save either image. Describe both the measurement and its limits.

For finding a known UI patch, cv_match_template assumes nearly matching scale
and rotation. Use the actual template bounds and returned similarity; it is not
semantic object search. For a photographed page, cv_document_corners can return
found=false; require an actual clear quadrilateral before proposing a perspective
transform. Detection of a corner does not itself produce a corrected document.

When building masks, use a registered cv_threshold_mask with the schema's
threshold method and output_path rules, then inspect its new binary PNG before
passing it to another operation. It is a threshold mask, not a learned semantic
segmentation. Similarly, cv_detect_edges produces a new edge map; contours are
geometric regions and do not establish object or person identities.
