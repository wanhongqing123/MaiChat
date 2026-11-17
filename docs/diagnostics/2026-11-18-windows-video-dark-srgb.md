# Windows SDR playback appears too dark

## Reproduction and source

The user reported that Windows MaiChat played the supplied MOV files darker
than OBS. Read-only probing of the three MOVs showed the same relevant format:
H.264, 1920x1080, yuvj420p, 8-bit, full/pc range, smpte170m matrix and BT.709
transfer/primaries. They are ordinary SDR inputs. User media was not modified
or added to the repository.

Baseline source: `b69c65a74223eecf452ec3e6652f599f5578972a`. Verification uses
the normal Windows build and its real `MaiFfplayVideoDialog`, on NVIDIA D3D11
with the existing 150% display scaling. Display HDR/brightness settings were
not changed. The installed user App was not stopped during this reproduction.

## Decisive evidence

The final Presenter draw pass samples ordinary textures through
`gs_effect_set_texture_srgb`, which decodes their RGB into linear values.
The D3D11 framebuffer flag initially remained false, selecting an ordinary
UNORM render-target view. Writing those linear values directly makes midtones
too dark. Prior pure black/white/red/green tests could miss the defect because
their endpoint channel values survive both transfer functions.

The local OBS reference pairs the sRGB sample with
`gs_enable_framebuffer_srgb(true)` around its draw pass
(`libobs/obs-video.c`, draw_mix_texture). The shared Presenter did not pair its
Windows final draw this way; Android already has its separate output-encoding
selection, while Apple's SDR swapchain enables its own sRGB gamma handling.

Two small, repository-owned H.264 fixtures exercise full-range 601 and
limited-range 709. Both contain constant midgray and no audio. The test opens
the ordinary player, waits for a real frame, captures only its owned physical
center patch outside the HUD, records RGB values, and asserts 128 +/- 8:

| Input | Baseline RGB mean | Fixed RGB mean |
| --- | --- | --- |
| full/601, Y128 U128 V128 | 57 / 57 / 57 | 130 / 130 / 130 |
| limited/709, Y126 U128 V128 | 55 / 55 / 55 | 128 / 128 / 128 |

The same two cases failed before the production change and passed afterward.
Baseline and fixed PNGs were visually inspected. Logs and JSON are retained
under `.cache/video-dark-windows-20261118/`.

## Fix and limits

`MaiChat/Media/MaiGraphicsPresenter.cpp::drawTexture` now enables D3D11's sRGB
framebuffer view for the final draw and restores the captured previous state
on every exit after enabling it. Intermediate conversion remains independent.
No shader math, source range, video file, global brightness or other platform
branch was changed. Independent static review found no missing restore path;
FP16 resources use the same ordinary/linear view and gain no extra OETF.

Test entry: `MaiFfplayWindowsGeometryTest`
`sdrGrayPlaybackKeepsExpectedBrightness`, through `run-tests-windows.ps1` with
the deployed runtime and Windows QPA plugin. The optional
`MAICHAT_WINDOWS_VIDEO_FIXTURE_OVERRIDE` is for an explicitly supplied local
incident clip; default automated runs keep the repository fixture and never
scan user media. Screenshots from private clips must stay outside tracked
files.

The final combined geometry/gray CTest passed in 9.80s. The three supplied MOVs
then each passed the ordinary MainWindow prepared-player entry, presented
frame check, resize/fullscreen/restore/minimize cycle and owned-window capture.
One private playback capture was visually compared with a nearby frame decoded
directly by FFmpeg; this was a visual comparison, not frame-exact pixel equality
or a new OBS capture. Private captures/reference frames remain outside tracked
files, and the source videos were not rewritten.

This result establishes the Windows ordinary SDR defect and its repair. HDR,
other OSes and physical HDR brightness are not claimed as tested by these
gray cases. The corrected build has not yet replaced the running user App.
