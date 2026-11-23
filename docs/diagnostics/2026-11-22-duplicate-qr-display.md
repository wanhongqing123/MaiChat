# Duplicate QR image investigation

Current finding: one generated file was presented by both its typed image part
and a later explicit media-delivery receipt. The owned reproduction now includes
both sources; the same narrow desktop presentation correction passes on Windows
and in the Mac owner's private normal build. The user's existing Mac installation
has not been replaced.

## Reproduction and expected result before changes

- Date: 2026-11-22; investigation host Windows x64, reported host macOS.
- Local main: `38d25b9d7a41ad29128e95289f44937c676ca452`, with existing unrelated
  authorized work preserved. Read-only fetch found remote main `0875e487`.
- The user's expanded screenshot shows one SSH command and **one**
  `maichat_generate_qr_code` invocation, followed by two identical QR images.
  It does not show `view_image` or a second generation invocation. The earlier
  generate-then-view hypothesis is therefore not the reported call sequence.
- Do not decode, log or copy the user's QR payload, credentials, server command
  or workspace paths into fixtures. Use `https://example.invalid/owned-qr` instead.
- Drive the ordinary AgentController, real QR tool and AgentChatPanel using an
  owned deterministic model. Approve only the owned generation call. Expect one
  completed QR tool, one stored image part and one visible image, both live and
  after reopening/recreating the panel. Replay repeated part notifications too.
- Include a final Markdown image reference to the generated path to distinguish
  inline text rendering from typed image rendering. Keep the actual stored text
  and tool results unchanged. No SSH connection, external model or paid API.
- Establish a failing reproduction before selecting a production fix. A local
  passing test is not proof that the user's installed Mac build is fixed.

## Local result and remaining evidence

The normal Windows `maichat` target completed its build, windeployqt and PE checks.
The ordinary `agent_panel_session_test` target exercised the real QR tool and
widgets with an owned model (no provider request). The focused case passed:
one generation, one stored image, one synchronized image and one visible image.
Three repeated notifications, session switching and a fresh panel still yielded
one image. The final Markdown image reference rendered its alt text, not a second
image in this source version. The existing independent QR encode/decode suite
also exited 0. No production correction was selected on this evidence.

Evidence: `.cache/qr-display-20261122-test.txt` (3 QtTest entries passed, 0 failed,
0 skipped), and `.cache/qr-display-20261122/owned-qr-one-generation-{live,restored}.png`.
Both owned screenshots were inspected locally; neither contains user content.

At this initial stage, remaining evidence was the reported Mac runtime version/source
and whether switching away/back retained the two images. If persistent, compare the
two stored image part identities and file identities; if transient, compare the
one stored part against the two UI row identities. Do not remove images by
filename/content globally: that could hide an intentional later resend. The
initial local result did not establish the Mac root cause or an installed fix.

## Mac evidence and narrowed correction plan

The Mac owner supplied a targeted read-only receipt at 16:10 +08:00. Running
3.1.1 PID 438 matches the published executable manifest (source `af01f9ca`).
The target reply contains one QR generation, one typed image part, and a later
completed `agent_send_media` receipt. Both image paths resolve to the same
canonical file. There is no `view_image` and no Markdown image reference. The
media receipt becomes its own card, so it disappears from the earlier two-tool
disclosure (shell plus QR). The two card keys are derived from different part IDs.
No historical emit count or actual UI row inspection is claimed by the receipt.

Before changing production, extend the owned case to perform the real
QR-generation -> `agent_send_media` sequence for the same file, retaining the
caption. Expect one image live, after repeated events, after session switching
and after fresh-panel replay. Different canonical files, even byte-identical
copies, and the same file explicitly sent in a later assistant message must
remain separate. All stored parts and tool outputs must stay unchanged.

The correction is a desktop presentation index owned by the retained conversation
page, scoped to assistant message and canonical local file identity. Reuse the
first card and its chronological anchor; merge explicit caption text without
removing image controls. Do not merge across messages/sessions, infer identity
from a basename, hash file content globally, mutate Core storage, or alter
video/audio publication eligibility. File size/mtime participate in the key so
a metadata-changed file is not intentionally collapsed under an old path.

## Correction and Windows acceptance

The new real generation-plus-delivery case failed before the correction with
`imageCount()=2`, expected 1. The product change is confined to
`AgentChatPanel.cpp/.h`: a conversation-page index reuses the first image card
for the same message and canonical file/version metadata, merges unique caption
text, and preserves its initial timeline anchor and image controls. Original
tool/image parts, input/output bytes, model requests and publication transports
are unchanged. Existing video/audio paths remain separate.

The final owned test waits for the visible delivery caption before counting
images, so a queued Qt notification cannot pass via a transient original-only
state. Three cases cover generation alone, generation then same-file delivery,
and two byte-identical files at distinct paths. They also cover repeated part
notifications, session switching, fresh-panel replay, original stored part and
input/output preservation, real image-preview opening, and a later assistant
reply delivering the same file again. The matching-file case shows one card;
the distinct-file case shows two, and a later reply adds its own card.

Normal App build, runtime deployment and 114-PE-import verification passed.
Related UI gate: 10 QtTest entries passed, 0 failed/skipped (including video,
audio and cached-page regressions); the final timing-only adjustment is also
checked by the three QR cases. Owned screenshots under
`.cache/qr-display-20261122/final/` were viewed directly. The same-file screenshot
has one QR and its caption; the distinct-path control has two.

A local development staging and its actual extracted ZIP have identical 84-file
payloads and passed separate bare-PATH `--smoke` runs, PIDs 8352 and 50492.
Executable SHA256: `48aa64e4ca0ae93aa60f9f793e1d8c1a165b2007791e40afddd4b9e23c419f1f`.
This is the local uncommitted development tree (metadata 3.1.0), not a new public
release or the user's installed 3.1.1. The isolated Qt known-folder probe passed;
original Qt work and three preexisting documents retained their recorded hashes.
Safe receipt: `.cache/qr-display-20261122/package/validation-safe.json`.

No user program was stopped/replaced, no user media or database was rewritten,
and no real model/SSH/API request was made by the owned regression or packaging
checks. Root has not committed or pushed this correction.

## Independent Mac validation

The Mac owner consumed only the 5,501-byte QR UI patch
(`19aff6fc80af38316539b7c1e5c314eec4ad4778d5a0032069c304567b50a7b8`)
in a private normal-source/build lane, leaving the user's installed PID 438 and
main worktree untouched. An independent static review reported no P1/P2 issue.

Two fixture corrections were required before Mac reproduction: wait for the
actual delivery caption before counting cards, and canonicalize the temporary
workspace so `/var` vs `/private/var` aliases do not produce an escaping relative
path. Both corrections are mirrored in Root's fixture. The real tool's workspace
boundary was not relaxed. The initial caption/path fixture failures are retained
but do not count as duplicate-image RED evidence.

The corrected Mac r4 fixture then produced a valid old-code RED: actual 2 images,
expected 1, exit 1 in 0.771 seconds; its owner inspected the two-image captioned
screenshot. The same frozen product patch is now being checked with normal
`maichat` and `agent_panel_session_test` targets.

The Mac owner then reported normal App/test build exit 0 and all three QR data
rows GREEN (5 QtTest entries including init/cleanup, 0 failures, 2.613 seconds).
The frozen 5,501-byte product patch was unchanged. The same-file case displays
one image with caption; different-file control displays two. Repeated events,
session return, cold-panel replay, later-message resend, original stored parts
and tool bytes/identities, and the real ImagePreviewDialog passed. The owner
inspected the owned live/restored/different-file screenshots and confirmed no
P1/P2 issue in the two UI hunks. Installed PID 438 remains untouched; no commit,
push or release is claimed. Safe receipt and the additional existing media/page
regression results are supplied separately by the Mac owner.

Additional Mac regression boundary: `inlineMediaBubblesRespectMessageBoundary`
passed. The preexisting `sentImagesOpenNormalPreviewLiveAndAfterRestore` did not
finish: its owned test process PID 65202 remained in the old test's open lambda
through `MessageImageLoader::deliver` to `QDialog::exec` for more than 90 seconds.
The Mac owner retained its log/sample and terminated only that owned process.
This old preview gate is **not passed**, and its cause/relationship to this patch
is not established. No product logic or assertion was changed to bypass it.
The new QR case's real ImagePreviewDialog open/close did pass. The two remaining
cached-page cases were subsequently run separately and passed:
`changedCompletedMessageRefreshesItsCachedPage` and
`clearedCachedSessionDoesNotRestoreRemovedHistory` (exit 0, 0.781 seconds).
Together with the inline-media case, three additional narrow regressions passed.
This boundary does not turn the three passing QR cases into a full Mac UI-suite
acceptance claim. The user's installed PID 438 remains untouched.

## Final Mac evidence intake

Root independently verified the 733,830-byte evidence ZIP SHA256
`b02c2170c2c523f7c1e2f460a4ca2173b77743a1bf0f4cac44f7f4d7c5023690`,
all 16 manifest entries' sizes and hashes, and exact equality of the archived
safe receipt to the separately received JSON. The archive contains seven owned
screenshots, logs, a sample of the stalled owned modal test, the owned fixture
fragment and diagnostic; it contains no additional production patch.

Root read the actual Mac RED, QR GREEN and cached-page logs, and inspected the
Mac two-image RED screenshot and one-image restored GREEN screenshot directly.
The final intake is `.cache/qr-display-20261122/mac-final/root-intake-safe.json`.
The old modal test ended by SIGTERM/143 and remains explicitly unpassed. No
additional build, test expansion, installation, commit, push or publication is
needed for evidence intake; the unchanged narrow production patch remains in
Root's existing main worktree for subsequent source/release integration.
