# Desktop card styling, 2026-11-28

## User operation and expected result

Open a creation-center session, run a tool, expand its result, then add local
files/images/video to the draft. The user requested a fresh visual treatment and
explicitly limited the scope to card styling. Tool cards should have a light
border, rounded white shell, soft blue details and color-coded status pills.
Draft attachments should share the lighter border and remove-button treatment.
The page/header/composer frame, contents, approval and disclosure actions,
attachment identity and model payload must retain their existing behavior.

## Source and review

Baseline fd0d7cd7457f1cd7ff83de80471781e3314755e5. Only
MaiChat/desktop/src/agent/AgentChatPanel.cpp changes product code. Metrics use
UiZoom; card QSS targets object IDs to avoid applying card borders to children.
Details preserve platform fixed-font family through UiZoom's TypeWriter path.
Independent review caught the initial Monospace hint being overwritten by
UiZoom; the final card-local fix uses TypeWriter, without changing shared UiZoom.
No test or Core/transport source changed. Offline comparison confirms13 existing
content/attachment/send methods retain their original text. No metadata was
removed or added to the model conversation.

## Windows verification

Normal App and agent_panel_session_test were rebuilt from checked final overlays
in the private r311 lane, which retains its older checkout plus verified source
inputs. Final source LF SHA256 is
aeb0483e686e2740b4e6ab86a279b565f1b4c7017063c42d379bd66551e7b85a.
Build returned0 (24.946s), including windeployqt and the PE import gate.
The official Windows wrapper, native windows QPA and isolated profiles ran the
existing39 attachment/media/text/header/receipt cases plus lazy disclosure,
consecutive tools and two approval/session cases:43 passed,0 failed (12239ms).
The earlier pre-font-correction43/0 result is retained separately.

Developer inspected final actual QWidget screenshots of completed/long tool
receipts and ordinary attachment drafts. Cards, status pills, escaped Windows
paths and separate output lines remain visible; long output retains its existing
truncation notice. Images come from owned fixture data, not user conversations.
The supplied HTML proposal was a design sketch, not an App screenshot. Evidence
is private under .cache/ui-refresh-20261128/windows-final-02/.

This does not claim manual Finder/Explorer drag or mobile runtime acceptance.
No user installation, model/provider request, backend operation or published
4.0.2 release asset is involved. Mac exact-source card verification is recorded below.


Final normal staging and independently extracted ZIP each passed bare-PATH smoke
with owned profiles and matching executable SHA256
8dafaddbe9c80c8ec699710498062ef9814c673cfab48fbcd758b4191093296a.
Package/stage/ZIP exits were0; receipt is private under
.cache/ui-refresh-20261128/candidate-windows-final-01/receipt.json.
This was a private validation package, not a replacement release asset.


## Mac exact-source acceptance

Mac consumed a3ec2d6c45e6e62fbd1c30964b1362b9e37dfead in its private lane.
Normal configure/build returned0 (19.895s/10.954s), the unchanged43-case GUI
consumer passed43/0 (12698ms), and the private-profile App smoke returned0
(2.494s). Core archive bytes were reported identical to fd0; no unrelated Core,
video or fullscreen suite was repeated.

Root verified the supplied290582-byte ZIP SHA256
6d53223b79e989c0a27e68914a4be3b69ddf848c61cc8c73becfaef63ec1ecc1,
all24 manifest entries, three canonical source inputs and root/component tree
bindings against the exact Git commit, and the43-pass log. App/test/helper
binaries were not supplied; their hashes remain Mac-reported, not independent
Root binary rehashes.

Root viewed both final actual-component screenshots:1000x870 and640x870 logical
sizes (Retina images are twice those dimensions). The displayed sample completed
and error cards, status pills, output lines, file/image drafts and remove controls
are readable at both widths. Details report Menlo/fixedPitch=true. These are
owned synthetic stored receipts and local drafts, with no model request or
command execution; they are not screenshots of the user's installed session.
No claim is made that all arbitrary content lengths cannot clip.

The first external visual helper omitted the error field; a follow-up compile
failed due to its field type and a stale helper ran. Those outputs are explicitly
invalid/excluded. Only the corrected helper, successful final build, frozen TU
and final capture are accepted. Product code and the43-case test TU were not
changed for that fixture correction. Private evidence is retained under
.cache/ui-refresh-20261128/mac-a3/. No release package or installation changed.
