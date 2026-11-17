# Windows standard-font size audit

The user reported that the Windows standard font looked much larger than the
other platforms. This record is a diagnosis and private visual comparison;
the production font baseline and user preferences were not changed.

## Actual Windows configuration and isolated measurement

Only the three appearance preferences were read: application-level
`ui/zoomFactor` was 1; textSize and font-family were absent. The same allowlisted
fallback values were also absent, yielding standard/system defaults. Other
settings, user databases, credentials and process environment were not read.
The actual installed window was per-monitor DPI aware and reported DPI 144.

A cache-only QLabel/QTextEdit/Markdown fixture compiled the production UiZoom
implementation, registered the same 14 bundled font files as main, and matched
DirectWrite, PreferNoHinting and PassThrough. QSettings used a private Ini
directory. No MainWindow, login or account context was constructed.

Qt reported logical DPI 96 and DPR 1.5. Standard with application zoom 1 gave
16 logical px / 12pt for labels, text edits, documents and Markdown; zoom 1.5
gave 24px / 18pt. There was no additional 144/96 multiplication.

The system family resolved to Microsoft YaHei UI for both Latin and Chinese.
Its original platform GeneralFont was 9pt / 12px, which the application
explicitly replaces with the shared 16px baseline. This is 33% larger than that
machine's native default, not evidence of a second DPI scaling pass. The
historical 2026-11-06 baseline change from 13 to 16px is another relevant
artifact-comparison factor.

## Font and platform comparison

At 16px, Microsoft YaHei UI had Chinese ink height 16 and line height 21;
Noto Sans SC had ink height 15.33 and line height 24. A fixed mixed sample was
3.2% wider in the system family. Line height alone is not the glyph-size scale.

Mac repeated its owned settings consumer with the complete main font
registration. Its standard remained 16px, logical DPI 72 / DPR 2; Chinese
resolved to Noto Sans SC, line height 24. Earlier PingFang measurements came
from a QtTest launch without the font-registration step and must not be used
as evidence of the installed App's fallback. These fixtures do not establish
the subjective physical-size equivalence of different displays or installed
platform builds.

## Reviewable Windows candidates

The same fixed-width widgets and Markdown spacing were privately rendered at
14, 15 and the current 16 logical px. Respectively: ink heights 14/14.67/16,
line heights 18/20/21 and Markdown content heights 244/255/292. The comparison
image was visually inspected and sent to the user. A Windows-only smaller
baseline was proposed; 14 versus 15 remains a user preference decision, with
other platforms and system display scaling unchanged.

Measurements, fixture source/binary/font hashes and owned screenshots remain
under `.cache/windows-font-standard-20261118/`. No source-level font repair,
normal build reconfiguration or installed-App replacement is claimed.
