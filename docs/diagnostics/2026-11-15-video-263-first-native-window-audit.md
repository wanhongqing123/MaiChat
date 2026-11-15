# Desktop 2.2.63 first native video-window audit

## Reopened report and ownership

After public 2.2.63 delivery, the user supplied a new 7.5-second phone recording
(`144115251141768230-1794721605-52282664.mov`, 1920x1080, 30 fps). The reported
symptoms are partial presentation before enlargement and a much larger initial
Mac player than on Windows. Mac confirmed its actual running process is the
formal 2.2.63 dist App, PID 10791. This report is not attributed to the earlier
private R5 App. The supplied recording was viewed as a local contact sheet.

Mac owns Dialog/MacLayer/Main opening functions and their tests. Root's work here
is a read-only Windows comparison. No product/test source, normal build output,
installed App or public release asset was modified for this audit.

## Reproduction

Link a private probe against the existing normal d973 Desktop UI library, with
the original target's compiler definitions/includes and link libraries. The UI
library SHA-256 is
`6fc650bb589bb95cad2516f42ed9c037fee5e092316981a8a8423df268bd6952`.
Main has since received only the release documentation commit `618811fc`;
Desktop/Core product files still match release source
`d9736686998990503256fc6db4998e2bdabb553d`.

Each run starts a fresh private profile and owned Fake-IM MainWindow, selects
the actual Creation navigation button, then calls ordinary `openVideoWindow`
with a generated solid-color 16:9 H.264 file. No fullscreen request or player
geometry override is made. Record Qt Show/Resize/Move and Win32
WM_SHOWWINDOW/WM_WINDOWPOSCHANGING/WM_WINDOWPOSCHANGED/WM_SIZE for the owned
player/surface. `GetWindowRect` records native physical coordinates. Wait for
real presented frames and another second before closing only these owned windows.

## Observed Windows results

The Windows screen is 1707x1067 logical, available area 1707x1019 logical,
DPR 1.5 and native window DPI 144. All three processes exited zero and their
Creation/open checks succeeded.

| Generated source | Settled Qt size | Settled native size | PID | Presented frames |
| --- | --- | --- | ---: | ---: |
| 640x360 | 640x360 | 960x540 | 52172 | 32 |
| 1280x720 | 1280x720 | 1920x1080 | 50676 | 31 |
| 1920x1080 | 1451x816 | 2177x1224 | 54056 | 31 |

All three final Qt window states are zero (ordinary window). At the Qt Show
event, logical geometry is already final, but `GetWindowRect` still reports
720x405 and `IsWindowVisible` is false. Approximately seven milliseconds later,
the native WM_SHOWWINDOW(show=true) callback observes the final native rectangle.
Native events for which `IsWindowVisible` is true contain only one size per run,
equal to the final size above. This probe did not observe a visible native
small-to-large resize on Windows.

This is native event evidence, not a first-compositor-frame recording. It does
not establish that every initial video pixel is correct or that no visual flash
can occur. The ordinary helper does not override QWidget flags, force key/window
placement, or resize the player to fit an evidence screenshot.

## Contract and previous coverage gap

`MaiFfplayVideoDialog::setSize` uses the same algorithm on both platforms:
available logical screen size times 0.85 limits source width/height, with minimum
control size constraints. This path does not divide source dimensions by DPR.
There is no platform-specific default fullscreen request in this calculation.
Matching source dimensions and actual Mac logical available area are still
needed to decide whether the reported default-size difference follows this rule.
Near-screen-sized geometry alone is not native fullscreen evidence.

The previous `videoOpensAtPreparedGeometry` SDR fixture is 96x64 (confirmed by
ffprobe); its Mac HDR branch generates 192x108. Its assertions compare Qt Show
geometry with settled geometry. The current native
trace proves that those Qt values do not establish the native rectangle at that
same instant. The old passing regression is retained, but it is not used to
dismiss the new report. Mac's first native mapping/compositor investigation is
still open; no root cause or fix for that report is claimed here.

Private evidence: `.cache/video-opening-263-20261115/`, including
`windows-summary.json`, the three dimension-specific JSON event traces,
`build-receipt.json`, and the viewed recording contact sheet. The first private
probe compile failed because Windows LONG values needed explicit JSON integer
casts; that diagnostic is preserved separately. Only the private probe changed.

## Subsequent reviewed R7 candidate

The preceding paragraphs retain the original 2.2.63 observations. Mac's subsequent
large-source probe reproduced final Qt/native window geometry 1285x723 while the
retained Metal layer and parent presentation viewport were still 480x270 at
335 ms; they caught up at 474 ms. The native window rectangle did not change.
That is a layer-geometry failure, separately from the supplied visible recording;
the sampled owner was not on the active Space, so it is not claimed as a foreground
compositor recording. A client-size/flush-only intermediate attempt still failed.

The final narrow candidate uses Qt's known client size, immediate container layout,
disabled implicit actions and a transaction flush, plus a same-GUI-turn layer sync
after prepared show. It disables the native default popup animation. It leaves the
Metal backend responsible for drawable size and preserves the existing fullscreen
Space/HUD ownership. Root's independent static review found no concrete P1/P2.
The two production and two test blobs were applied only after manifest preimage
and new-blob verification, preserving the independent thinking-start source8.

Mac reports 16/0/0 in its normal affected group, including 65 mapped layer samples
with zero mismatch, existing drawable pixels, HDR, resize edges, cancel/reuse,
close and Space cases. Its private normal App build/package and four startup
checks passed; the current user PID 10791 and distributed 2.2.63 App were untouched.

The common ordinary default now caps at 960x540 logical and 75% of available area,
subject to the existing minimum control dimensions. Windows' new original three
probe sizes all exited zero: 640x360 remains 640x360 logical (960x540 native), and
1280x720/1920x1080 both become 960x540 logical (1440x810 native at DPR1.5).
Each visible native event stream still has exactly one size and ordinary window
state zero. Actual PIDs were 45124, 10568 and 48572, with 32/31/32 presented frames.
The amended original Qt selector passed all three rows, plus init/cleanup (5/0/0).

New Windows evidence is `.cache/video-opening-r7-20261115/`. Incoming production
patch SHA-256 is `a71d3ab9a5ff0b44725a807d64217aa075f5189e305adc26bf2d5d83f2d8e5ca`;
test patch is `3da66e800fb153507ec5a531e3212b9d96db418b58a44cf48e7f5baf4cc1b0e9`.
These are verified source/private candidates for the successor; public 2.2.63
remains immutable. Continuous Mac compositor and installed-candidate acceptance
are not claimed by this record.
