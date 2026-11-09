# Windows Noto Sans SC title/status appearance

2026-11-09. Baseline main: `0c49588c87a87b6e71c70b7fd3d3020ebe7f9e75`;
published desktop version: 2.2.53, frozen source `36db08d1`.

## User report and required result

The user marks the conversation section heading, chat title, execution timer
and timestamps as having an incorrect font on Windows. The selected preference
is “黑体”; other choices have not shown the issue. The current choice maps to
`notoSans` / Noto Sans SC, not Windows SimHei. Ordinary message text appears less
affected in the supplied images. Mac has no corresponding font report.

Expected: these UI text roles use the chosen font with stable readable glyphs,
including after a live preference change and application restart. They should
not silently depend on unrelated installed fonts. Preserve draft/text state and
the separate code font.

The user confirms the latest installed delivery, v2.2.53. The new settings
screenshot also establishes Noto Sans SC with the Large text preset. Whether
the issue survives a complete application restart is still pending; the user
does not need to interrupt ongoing work to answer that question.
The current Windows App is not running on this agent's host. No user settings,
chat database, credentials or existing logs are read during this investigation.

## Controlled evidence

Private artifacts: `.cache/font-heiti-20261109/`. A native Windows Qt 6.12.0
process uses the normal built UI libraries, the exact 14 qrc font resources in
product order, owned labels and a real MainWindow backed by FakeRemoteIMClient.
Both startup with Noto Sans and switching through the real settings combo are
tested at this host's DPR 1.5. Captures are only of owned widgets, and are viewed.

The marked thin appearance has not been reproduced with the default native
font path on this host. Requested style names are empty and stretch is zero,
so stale styleName/stretch is not established as the cause. No production font
behavior has been changed from this observation.

The marked headings/title, timer and inline Markdown timestamp request
600/DemiBold. Separate attachment/approval messageTimeLabel timestamps request
500; the actual timestamp route must be distinguished when comparing the scene.
The bundled Sans faces
are Regular400, Medium500 and Bold700; the Serif bundle also has a real600 face.
An independent no-window probe checks actual glyph tables/outlines and the
DirectWrite face axes/simulation flags, rather than trusting QFontInfo alone:

| Requested600 path | Actual face/axis | Bold simulation |
| --- | --- | --- |
| Windows default native | Bundled Medium CFF, weight500 | Yes |
| Windows GDI matching database | Installed variable glyf/fvar, weight500 | No |
| Mac Cocoa | Medium500 reported; OS/2 weight500 | Not measured by the Windows API |

Both Windows drawing engines in that experiment are DirectWrite. Selecting the
GDI database does not prove GDI rasterization. Its variable font's base OS/2=100
and PostScript name ending Thin do not mean the rendered instance is Thin:
the actual DirectWrite weight axis is500, and glyph outline area is close to the
directly loaded bundled Medium. Adding an explicit wght600 request did not change
the observed face/outline in this narrow probe.

The Mac peer supplies a separate font-only Cocoa JSON using the same seven Noto
faces, private HOME, no visible window or user preferences. It maps requests
400/500/600/700 to Regular400/Medium500/Medium500/Bold700. This establishes a
platform matching difference; it does not by itself identify the user's issue.

## Source observations and next gate

The font preference, role sizes and most UI layout are shared. Windows and Mac
use different platform font databases and drawing implementations. Qt 6.12 uses
400/500/600/700 weights directly; Qt 5's0..99 comments are stale, not a reason to
rewrite current numeric values. The Windows startup string fontengine=directwrite
is not a recognized Qt6 selector; the present build defaults to the DW database,
but an external platform option can change matching. These observations must not
be promoted to a proven scene root cause.

Continue with the user's actual version/startup behavior and actual matched face
or platform options. Do not adjust all text sizes, force a new weight globally,
replace fonts, or claim the screenshot fixed from API weight numbers alone.
Published assets and installed applications remain unchanged.

## Large-preset cold-window follow-up

The two new source PNGs are 3840x2088. Viewing the original pixels confirms a
hairline appearance in the heading, title and timer; this is not inferred only
from a thumbnail. The Large preset is 1.125: at UI factor1, title is23 logical
pixels, metadata14 and body18.

The follow-up private ColdFontProbe uses the product's 14 qrc registrations,
application hinting policy, MainWindow, FakeRemoteIMClient activityReceived
signal and actual RemoteIMActivityBubble. Each process starts fresh. It saves
its first saved screen capture with QScreen::grabWindow before diagnostic QFontInfo,
QRawFont or QTextLayout queries. Startup Large, runtime Standard-to-Large and
runtime System-to-Noto-Sans all complete at DPR1.5 without reproducing the
hairline appearance. A separate cold GDI-database run is thinner than native,
but still does not reproduce the reported hairline. Reports, screenshots and
commands are under `.cache/font-heiti-20261109/*-unicode/` and
`cold-runs-unicode.json`. Probe SHA256:
`3b32fd4fa8f7fe0d191b41815381880e831b61e3cb79d81d6e33d75eb0d8ca44`.
The new matching reports cover the two title labels, timer painter font and App
font. They do not record a dedicated matched timestamp face; the captured inline
timestamp's appearance is only a visual comparison.

The first private fixture accidentally contained question-mark replacement text
in its newly scripted contact/message literals because of PowerShell pipeline
encoding. Those attempts are retained separately. The final fixture uses ASCII
C++ Unicode escapes and was rebuilt; all four final captures contain the intended
Chinese text. Product resource strings and timer implementation were unchanged.
Constructing the normal UI also generated eight local TRTC constructor log files
and one temporary SDK file in the original working directory. These owned probe
side effects were moved intact with a SHA manifest into the private cache; no
account login, remote desktop session or real user data was used. Future probe
working directories must be private.

An independent no-window cache-order experiment found no native pixel change
from the old probe's early font observations. Its GDI control did change when
styles were enumerated before registration, so that older GDI capture is not a
valid cold-start equivalent. The new GDI capture has no such pre-enumeration.
The Qt5 QSS parser maps600 to legacy75/Bold, whereas Qt6 directly requests600;
this can explain a changed QSS weight path, but the timer's explicit DemiBold
enum already had that meaning before migration. It cannot explain all reported
areas by itself. No font product change is justified by this evidence yet.

The separate 2.2.54 candidate package freezes adfff832 on top of the Mac player
restore fix. It contains no font correction and must not be described as fixing
this Windows report. Further diagnosis needs the affected process's matching
context or the user's pending restart observation, rather than more uncontrolled
changes to font weight, family or hinting.
