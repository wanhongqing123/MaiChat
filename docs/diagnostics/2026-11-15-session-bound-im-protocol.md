# Session-bound IM delivery and input admission

## Reproduction and expected behavior

Bind an accepted IM peer to an owned creation session. Send text and cached image/video/file inputs. While the model is waiting for a tool, send several more messages. Public completed response text must arrive before the tool finishes; subsequent messages must reach the same owned turn in order. Automatic messages must not acquire a display quote. The existing execution activity must retain one task start time across phases.

An ordinary model question must accept the peer's answer. A tool permission requires the existing explicit approval interaction, never ordinary text such as `A`. Unbinding, changing account/workspace, local takeover and successfully stopping the exact turn must revoke outstanding sends and inputs. Historical imports remain context only and must not create model calls.

## Evidence and implementation

The original desktop sender published only at turn completion and attached a quote. The native producer also waited for completion. Busy inputs waited for idle rather than steering the owned turn. Behavioral RED cases reproduced delayed public text, missing steering/continuation receipts, and implicit quote behavior.

Core now publishes typed receipts for persisted complete public text parts and actual consumed steering inputs. Private reasoning, raw tool arguments and failed/canceled response tails are not public text. A successful stage remains deliverable if a later response fails, provided its binding authority was not revoked. Explicit completed file/media results use ordinary preparation APIs; observations such as `view_image` do not become automatic attachments.

The shared router appends ordered dispatch entries while Running and seals separately. A progress acknowledgement does not retire the turn. Native exposes only the outstanding head for a token. Consumers preserve the complete original envelope for claim/ack. The independent activity lane uses the existing v2 online-only codec, exact ownership permits, monotonic sequences and terminal cleanup; it does not add chat-history/unread entries.

Approval uses the existing v2 request/decision/resolved interaction. Hosts privately map public random tokens to the exact pending permission, session, message, part, turn and binding epoch. Only fresh persisted Human control frames enter that path; they do not become prompts. Questions remain ordinary public question text with an exact question ID and a separate Core question reply.

Independent review identified a real workspace admission gap after asynchronous validation. A deterministic native validator gate reproduced it: an old prepared followup still entered Core after the session directory changed. Optional `expectedDirectory` fields now guard Send, Steer, Permission reply and Question reply within the Core runtime lock, before the user store or gate settlement. Empty guards preserve ordinary caller behavior. Hosts retain the originally authorized raw directory; they never substitute a newly changed directory. Rejected replies do not consume pending requests.

Stopping from either the GUI or an ordinary worker now revokes the exact interrupted turn through a thread-safe watch before SDK dispatch. A queued cleanup cannot revoke a later turn. Controller error state is stored and read through synchronized value snapshots, including worker failure paths.

## Verification and boundaries

Windows 11, MSVC 14.44, Qt 6.12, ordinary desktop cache. Candidate 07 built the normal App and passed deployment checks for three binaries and 114 PE dependencies. Shared router: 18 passed; native producer: 32 passed; all 13 selected Core suites compiled, linked and ran successfully. The Core set includes ToolLoop, input queue, image context, model client, session rename, ordinary file/media tools, tool execution/output and the affected image providers.

The subsequent Controller error-snapshot and final accepted-peer avatar consumer run passed the complete desktop binding suite: 54 passed, no failures or skips. SDK tests passed 40 cases, including explicit approval metadata and the preserved manual-quote controls. Evidence and original failures are retained under `.cache/im-delivery-review-20261114/`, `.cache/sdk-machine-approval-20261115/` and `.cache/desktop-accepted-peer-avatar-20261115/`.

Android Java consumers passed 46 unit cases; the normal arm64 275 / 2.2.33 APK built and passed manifest, ELF dependency, runtime, model, alignment and signature checks. An owned Android CLI built from the same ordinary Debug cache then passed the real loopback request (including the independent file schema and paid-image pause) and the original 32 native cases. Only temporary programs and files were used; the running 274 App was not stopped or installed over. The test-only missing SQLite include was corrected, the test option was restored OFF, and package/JNI/main-archive hashes stayed unchanged. A graphics relink matched the normal unstripped baseline and is distinguished from the APK's stripped entry.

That CLI and package verification are distinct from installed Java/SDK conversation acceptance. Apple separately consumed the exact Native/Core source piece and reported native 32/0, router 18/0 and an actual loopback model-request contract. Its receipts remain platform-specific.

All model/IM fixtures above use owned state, fake SDK/model implementations or loopback. They do not prove a live two-account SDK conversation. Physical App, signed installation and runtime-process receipts must be recorded separately; earlier iPhone build 211 must not be described as containing these changes.
