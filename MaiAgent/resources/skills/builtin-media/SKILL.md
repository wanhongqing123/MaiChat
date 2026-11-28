---
name: builtin-media
description: Plan local audio/video transforms and inspect FFmpeg help before complex filters or encoding.
---

# Local media workflows

Use the real `ffmpeg` and `ffprobe` tools available in this App. Use `tool_help`
with `{"arguments":["media","-h"]}` for this platform's registered tools;
load exact schemas through `tool_search` before invoking a deferred tool.

For a local edit, probe the input with `ffprobe` first: `{"path":"input.mp4"}`.
Use the actual duration, stream indexes, frame rate and color information. A
filename extension or requested size does not establish the source format.

FFmpeg takes an argument array without the program name. Read focused help
before building a complicated command:

```json
{"arguments":["-h"]}
{"arguments":["-h","filter=scale"]}
{"arguments":["-h","encoder=libx264"]}
{"arguments":["-filters"]}
```

Help returns `status`, `text` and `text_truncated` automatically. Read both the
status and text: an unknown topic may have explanatory text with status zero.
A codec example does not guarantee that codec exists in this build. If help is
truncated, narrow to the relevant filter/encoder instead of assuming it is a
complete manual. For diagnostics from an actual transform, `capture_log:true`
returns the existing bounded diagnostic log, rather than enabling help capture.

A simple resize with an explicitly new destination is:

```json
{"arguments":["-i","input.mp4","-vf","scale=1280:-2","output-resized.mp4"]}
```

Choose a fresh output path and keep the source. Stdin and overwriting are
unavailable. Resolve permissions through the current workspace and normal tool
approval; a Skill does not grant access. When combining clips, compare their
streams and time bases before selecting concat/copy versus re-encoding. Use
focused filter help for crop, subtitles, timing, audio mixing or analysis; do
not invent an all-purpose image/video CLI for unrelated tools.

After a successful conversion, probe the new output, verify it is nonempty and
check its duration/streams against the request. A destination left after failure
is not a completed artifact. Deliver through the registered agent file/media
presentation tool only when the user requested it. Provider-backed media tools
require their own capability discovery and any paid-operation approval.

## Time ranges and multiple inputs

For a frame-accurate segment, verify that the requested range fits the probed
input, then put `-ss` after `-i` and supply duration with `-t`, for example:

```json
{"arguments":["-i","input.mp4","-ss","12.5","-t","8","segment-new.mp4"]}
```

This example allows re-encoding; do not silently add `-c copy` when exact cut
boundaries matter. Stream-copy cuts depend on keyframes. For a no-audio source,
do not reference a nonexistent audio stream in a filter graph.

Before concatenating clips, check resolution, pixel aspect ratio, frame rate,
color/HDR mode and audio layout/sample rate. If they differ, normalize each
input explicitly and use the concat filter, after reading
`{"arguments":["-h","filter=concat"]}`. Reset segment timestamps as that help
requires. Never guess an extra output label: filter outputs must match the
filter's declared output count, and input labels use zero-based input indexes.
For a failed graph, use the returned graph/diagnostic context to correct the
actual command instead of repeatedly changing unrelated pixel formats.

For analysis such as volume or black-frame intervals, read that filter's help,
choose a bounded range and set capture_log=true. The tool's diagnostic tail may
be truncated; do not interpret missing earlier lines as proof of no events.
