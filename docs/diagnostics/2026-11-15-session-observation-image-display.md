# Persisted observation images missing from the desktop Session

## Report and scope

The user supplied paired screenshots: screen-share/computer-observation images
are visible in the friend's IM conversation and Android's remote Creation view,
while the Windows owner's local Creation view shows only text and tool groups.
The same account is used on Android and Windows. The separate Android stale-tail
and running-state report is being investigated independently.

This investigation does not attribute image deletion to binding. Inspection of
v2.2.62/v2.2.63/v2.2.64 finds no assistant typed-image rendering branch in either
live part updates or history replay. Previously working Markdown/media-tool cards
and User-image rendering are separate paths. Existing `MaiImagePart` data is
already in Core and the ordinary IM bridge can successfully deliver its file.

## Decisive reproduction

Using the normal Qt UI library from release source `21b86325`, a private copy of
the ordinary Bridge fixture feeds a generated 320x180 red PNG through the real
registered image tool. Core creates the actual persisted assistant image part.
The next model call is gated before any output. Both bound and unbound routes
retain the image file, but the Session has zero red image pixels; the bound route
also has exactly one successful ordinary IM image send. Both rows fail their
visible-pixel assertion (2 passes/2 failures including init/cleanup). No real
desktop capture, paid provider, account or user database is involved.

Private evidence: `.cache/bound-observation-ui-20261115/red.txt`, the viewed
red/green owned-panel images, `formal-image.txt`, and subsequent full regression
logs. The private probe links the normal library; it does not substitute a UI
implementation. Original source `21b86325` remains publicly immutable.

## Cause and change

The controller delivers non-text/reasoning part updates using its existing typed
part notification. `refreshToolCard` previously accepted only `MaiToolPart`, so
it returned for an actual `MaiImagePart`. The assistant history loop likewise
handled only Text/Reasoning/Tool. User-image rendering does not cover assistant
observations.

Live updates and history now render ordinary persisted image parts through the
same existing media-card renderer. IDs derive from the exact persisted part,
preventing duplicate cards after another update or replay. Imported history's
passive projection stays excluded; no filenames are discovered by parsing raw
tool arguments or envelopes. Missing/unreadable/oversized image files produce an
unavailable-image label. Image decoding still uses the existing asynchronous
loader. No source file is moved/deleted, no Core message is rewritten, and no
new IM transport is introduced.

The explicit video-delivery adapter retains its original video-circle eligibility
check and passes an optional publishing action to the shared renderer. Ordinary
image rendering requires no fabricated tool receipt. Two intermediate compiler
errors during that extraction are retained in the private build logs; the final
normal build and behavioral checks passed.

## Verification

The same bound/unbound probe becomes 4/0: both actual panels contain 61,050 red
pixels while the next model response remains withheld; the bound route still
delivers one image and the unbound route delivers none. The screenshot was viewed.
The formal original fixture retains its original IM/store assertions and adds
real panel pixels, an unbound row, switching away/back and a new panel's history
replay, with exactly one image-open control. Its three rows pass (5/0 including
init/cleanup). Full Bridge passes 69/0/0; focused original Panel/media cases pass
10/0/0, including video artifacts, PDF, ordinary media, pasted images, progress,
reasoning suppression and waiting-state controls.

The rebuilt normal App passed its three-binary/114-import deployment gate and
an owned bare-PATH/private-profile smoke: PID 32408, exit zero, executable SHA-256
`b0d0ed03b5f0f60a1bb5c34f837c3dd793d8aa551a76e7cb9d287e72eabc9562`.

This is a verified source candidate after public 2.2.64. It is not a claim that
the user's installed App has been replaced or that the separate Android remote
snapshot ordering bug is fixed. No user capture service or running App was stopped.
