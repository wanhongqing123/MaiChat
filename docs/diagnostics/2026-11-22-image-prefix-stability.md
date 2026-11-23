# Image prefix stability repair

User-approved design: retain user and tool image pixels and exact attachment
identity until MaiContextCompactor advances the stored source boundary. Ordinary
builder rebuilds must not rewrite an emitted image message according to newer
turns, the total image count or sliding8/12 limits. Use a timeless image label,
remove the prepended omission count and let compaction remove covered images.
Keep the per-account database and session filtering unchanged.

Baseline: requested main11a4e287c750bdbb953f0cb6d3c8a59fd6116004; Root remains
main38d25 plus preserved WIP. Read the requested commit from GitHub into the
existing private object repository; do not checkout or merge Root or the release
worktree. Save byte-exact before files in .cache/image-prefix-20261122/.

Reproduction: build after a user image, append tool screenshots to the same
assistant across the eighth/ninth image, append text-only and image-bearing
user turns across the twelfth/thirteenth user image, then append more tools.
Compare complete model message fields, not just counts: every earlier message
must remain an exact prefix when the source is only appended and compaction
does not advance. Verify a partial or full compaction boundary can remove only
covered image parts while retaining exact tail pixels and source provenance.

Implementation plan (inline; no new branch, dependencies, DB or UI workflow):

- [x] Add the multi-request prefix regression to the existing image test target;
  run against unchanged builder and retain the real assertion failure.
- [x] Remove count/turn-dependent image selection and recent-image options;
  use fixed per-message labels and retain every unarchived pixel. Preserve
  existing source/part ownership, quotes, call/result pairing, reasoning and
  independent explicit text-budget behavior.
- [x] Replace superseded image-eviction expectations with retained-pixel and
  compaction-boundary assertions; run image, compactor, tool loop/wire and Core8.
- [x] Prepare a minimal requested-main delta independent of other Root WIP,
  verify its application on private copies, and record source/build/test scope.

Validation does not prove a vendor cache hit. Pixel encoding still assumes
immutable attachment files at their stored paths; no real provider is called.
No commit, release or installed-App update is implied by candidate tests.


## Evidence and results

The unchanged Root builder failed the new append-only test (exit1) with changed
model-message0 after the ninth screenshot and later user turns. The actual RED
executable and its SHA/log are retained; no production source reversion or
artificial failure substitute was used. The original image test source was
byte-identical between Root-before and requested main11a4.

Fix: all unarchived user/tool image references remain in their original order;
no Current/Earlier labels, sliding8/12 selection or prepended omission statistic.
The user label is always Image attachment N with its part ID/workspace_path.
Tool observations retain JSON provenance and now also expose the exact path.
A pathless legacy attachment has a fixed local unavailable marker, never a
counter at the beginning of the request. The obsolete two rolling-image Options
fields were removed; no in-repo production callers used them. Ordinary callers
must rebuild against the current C++ header. Text opt-in byte-budget behavior
and existing tool output projection are unchanged.

The regression compares40 consecutive builder snapshots spanning35 image
references (16 user/19 tool), including ten screenshot steps appended to the
same assistant, fourteen later image-bearing user turns and intervening
text-only turns. It compares role, content, reasoning, image path/MIME,
call IDs/arguments and opaque continuation. Every prior model message remains
identical and the vector grows. Full/partial compaction source boundaries are
separately allowed to drop covered images; retained tails remain stable after
another turn. Existing steering tests now require old and new pixels in later
requests rather than expecting clearing. The initial broad run identified only
three superseded old-pixel expectations in one steering case; those now assert
exact retained paths and unchanged original attachment text.

Windows normal Core build passed and final CTest passed13/13 in62.85s: Core8,
image builder, tool loop, actual owned-loopback model wire suite and the two
existing checkpoint-reader/tool suites. Existing partial compactor and raw-source
atomicity assertions remain in place; only its obsolete maxRecentToolImages=1
configuration was removed. Normal maichat/agent_panel_session_test build and
normal dependency deployment/PE check passed after rebuilding direct consumers.
No clipboard or desktop-input test, live capture, user DB, SSH or vendor model
was used. No GUI behavior changed or installed-App acceptance was claimed.

An independent requested-main patch has five files,29562B/SHA256
dd29898054363e464589503b04746e03804058a01611cef9f0d03c7b68cd607c.
Its private before copies come from the exact GitHub11a4 commit, and actual
apply/reverse-check/result bytes all pass. No Root-only output projector,
checkpoint reader, classification, OCR or screenshot production delta is
included. Main-after source and per-file SHA/blob identities are in
.cache/image-prefix-20261122/manifest.json. Candidate ZIP86590B/SHA256
9266837d7264f250b14c6804bdba154ad3c2c2f753874065badfd4661ef117d2
was sent to the authorized Mac validation partner for independent private
normal compilation and owned Core checks. That result is pending and must be
recorded as a combined private tree, not a deployed or clean-main build.

Root remains main38d25 with its original WIP; the Qt changes and three original
diagnostic documents are still byte-identical. windows-safe.json records current
source hashes, executable hash and exact evidence paths. Public3.1.2 is unchanged.
This fixes deterministic image translation; actual vendor cache-hit rates were
not measured. Stored image paths must continue to refer to immutable attachment
bytes; this task does not snapshot arbitrary files overwritten outside the
attachment lifecycle. Explicit nonzero text-loss policies and a deliberate
compaction boundary still replace input and are not claimed to be append-only.


Mac interim report: exact five postimages applied once; normal maichat build
passed in seven steps, including Builder/Compactor/TurnRunner/Agent recompilation,
new Core archive and normal App link/deployment. Prior batches and classification
hashes remained unchanged. The standalone owned image test project configured
successfully and was compiling against that new archive; its result/receipt is
pending. No real tool, vendor model, user DB/screen, installation or release was
used. This report does not replace final evidence intake or assert Mac tests
passed before their result arrives.


Mac final execution result (reported by the validation owner): the existing image
suite's20 owned helpers normally compiled/linked against the newly built Core
archive and ran exit0 in0.459s. Normal App build remains exit0; all five postimage
SHA/blob values still match the frozen manifest, with no additional production
delta or P1/P2 findings in the limited review. No image files, live provider,
model/tool, user DB or screen were accessed by the owned Core test. This resolves
the interim pending execution result above; final safe/log archive intake is
still pending and will be recorded separately. Windows thirteen-suite evidence
is already local. Defaults are append-stable for the tested immutable source
sequence; explicit lossy text policies retain their documented old behavior.
No vendor cache-hit measurement, installed product update or publication is
claimed. Per-account agent.db/session_id filtering stays unchanged.


Final Mac archive intake:9074B/SHA256
0655faa42be2f1de8003e624d7099ec4ed028cd9e211278dc25faf5a7ea91230
matches the supplied file. All evidence-manifest entries pass byte/hash checks;
the standalone safe JSON and original Root candidate manifest match the archived
copies byte-for-byte. All five recorded postimage hashes match the frozen Root
main candidate. Normal App build, owned compile/link logs, owned CMake linkage
to the new normal archive and the exit0/0.458987s result were read. The image
suite prints nothing on success, so its empty stdout is not used as an assertion
count; the20 helper invocations are from the unchanged, hash-bound test main.
The validation archive is retained in .cache/image-prefix-20261122/mac-validation/;
mac-intake-safe.json records this independent intake. Candidate validation is now
closed on both platforms. Source integration/publication/installation are separate
and have not been performed by this task. No further tests or probes are pending.
