# Mobile Session IM binding contract

User scope: replace the independent reply-suggestion feature with Session binding
on Windows, macOS, iOS and Android. Use the existing iOS purple `sparkles` icon.
Only the welcome tool-directory layout remains desktop-only. Release 2.2.59 is
held at its existing immutable tag; the completed scope will use a new version.

## Reproducible behavior

In an accepted friend's private conversation, open the composer attachment panel,
choose **绑定会话**, select one local root Session and confirm. The conversation
shows the selected Session and **解除绑定**. Cancel leaves the draft unchanged.
Only new live Human messages from that friend enter the Session, FIFO after it
is idle. Its exact assistant response and explicitly delivered media go back as
Machine messages to that friend. Historical rows, group messages, machine
messages and control frames never start a turn. No remote task-status messages.

One local Session can bind one friend. Local Send/Stop, unbind, removed friend,
logout, backend replacement, deleted Session or changed workspace revokes queued
and not-yet-dispatched replies. Unbind itself does not interrupt model work.
This uses the running local application; it is not an iOS background service.

## Ownership and architecture

`SessionIM/MaiSessionIMRouter` is the only shared queue/authority/receipt state
machine. Desktop retains its Qt, file-preparation and SDK adapters. Mobile uses
`MobileAgentAdapter/MaiMobileSessionIM` around the same router and ordinary
MaiAgent API. Swift/Java adapt UI, accepted-friend snapshots, live SDK events,
ordinary attachment import and guarded Machine SDK sends. No platform duplicates
the routing state machine, no new RPC/MCP, no server model proxy.

Root owns Mobile native/CABI/JNI and build wiring. The kernel owner additionally
owns new `MaiMobileSessionIM.h/.cpp` and focused tests. Desktop owner adapts Qt;
Android owner writes Java; mac-maichat owns Swift and Apple validation. Real Git
index, commits, version and release writes remain Root-only.

## Native request protocol (frozen field names)

All regular calls use the existing `maiMobileAgentRequest` on its existing serial
worker. Request `op` is `im_session_binding`, with an `action` below. JSON is an
in-process language boundary, not a model-accessible transport.

`activate` takes top-level `sdk_app_id`, `owner_id`, `auth_generation` and
`friends` (array of accepted peer IDs, never auto-created chat contacts).
Unsigned integers are nonzero decimal strings. Response contains `ok:true`,
`bridge_id`, `scope` and the normal list projection. `scope` is an immutable
object with `sdk_app_id`, `owner_id`, `auth_generation`,
`controller_generation`; native supplies the last field. A configured backend
accepts one account identity; changing owner requires ordinary reconfiguration
or replacement. Same identity may refresh the accepted-friend set.

Every following request echoes `scope` and `bridge_id` from activation:

- `list`: return local root `sessions:[{id,title,busy}]`,
  `bindings:[{peer_id,session_id,session_title,epoch}]`, `has_bindings`,
  `pending_count`. No model request, IM send, read marking or draft mutation.
- `set_friends`: `friends` refreshes the accepted set; removed peers unbind.
- `bind`: `peer_id`, `session_id`; native validates accepted peer, root Session,
  canonical workspace and exclusivity. Return the list projection.
- `unbind`: `peer_id`; return the list projection.
- `receive`: `input_id` from the immediate `admit` control below, plus
  `peer_id`, `message_id`, `sender_id`, `recipient_id`,
  `expected_binding_epoch`, `expected_session_id` captured at original live
  ingress, before queueing or asynchronous media import. Native checks that the
  peer still has this exact binding before admission; rebind cannot adopt a late
  ready event into the new Session.
  `origin` (`human`/`machine`/`unknown`), `live` bool, `group` bool,
  `control_frame` bool, `text` string, optional `images`/`videos` using ordinary
  send's `{path,mimeType}` arrays. Paths have already been imported into the
  bound Session workspace through the ordinary attachment-import API. Generic
  files/audio use existing path-reference text. Quote digests may be included in
  text, never guessed as Core quote IDs. Call only when all required media is
  ready, preserving the original live identity across readiness callbacks.
  This fills the prepared owned payload for the already admitted input; it must
  not perform a second admission. Return `accepted` bool and `input_id` decimal
  string when filled. The kernel's Reserved head waits for its payload, so a
  later ready text cannot overtake an earlier downloading image/video. Native
  pump expires unready inputs after 120 seconds without another thread.
- `pump`: inspect current routes, start idle work via ordinary `MaiSendPrompt`,
  and return `deliveries` (below). Reuse the normal frontend worker/poll while
  bindings exist even if the creation UI has no listener. No extra executor.
- `takeover`: `session_id`; revoke same-Session authority without stopping it.
- `logout`: revoke all authority. Regular reconfigure/destroy also revokes.

Successful lists and mutations contain `ok`, `bridge_id`, `scope`,
`sessions`, `bindings`, `has_bindings`, `pending_count`. Pump also contains
`deliveries` (array, possibly empty). Errors contain `ok:false`, `errorCode`,
`error`; no successful empty projection for stale identity/failed DB operations.
The frontend can persist explicit bindings in its existing account preferences;
restore by explicit bind after activation, never replay cached messages.

## Deliveries and reentrant-safe authority

Each delivery is immutable and contains `bridge_id`, `scope`, `token`,
`dispatch_index`, `input_id`, `source_message_id` (the original SDK message ID),
`peer_id`, `session_id`, `workspace_key`,
`user_message_id`, `assistant_message_id`, `turn_id`, plus `kind` (`text`,
`image`, `video`, `file`), `text` for body or `path`, `mime_type`, `caption`
for media. Numeric token/index fields are decimal strings. Native obtains exact
submit-thread Core user/assistant receipts, never a latest-message window.
Native only delivers final assistant body and successful `agent_send_media`
receipts for `current_ai_session`, revalidated with `maiPrepareAgentMedia`.
Private observations, reasoning, raw tool output and arbitrary Markdown paths
are excluded. Native voice output uses the ordinary Machine file path.

New C ABI `char* maiMobileSessionIMControl(const char* request)` takes NO raw
backend handle and returns owned UTF-8 JSON freed with `maiMobileAgentFree`.
It touches only the weak lifetime registry and shared authority, never Core,
host callback, database or a serial worker. It is safe at the SDK boundary and
inside a host callback; never recursively call regular backend request there.

- `{action:"admit",bridge_id,scope,peer_id,message_id,sender_id,recipient_id,
  expected_binding_epoch,expected_session_id,origin,live,group,control_frame,
  has_content}` at the original live SDK arrival, BEFORE downloads or worker
  queueing. This queues metadata only and returns `{ok:true,accepted:true,
  input_id:<decimal string>}`; ineligible/duplicate/full admission returns
  `accepted:false` with `reason`. Carry this input ID through all preparation.
  It preserves original arrival FIFO, including not-yet-ready media, without
  a platform routing queue. No plaintext or file I/O occurs here.
- `{action:"abandon_input",bridge_id,scope,input_id}` removes an input still
  Pending/Reserved when SDK download/import fails or is canceled. It cannot
  revoke an already Running/Finishing turn. Wake the normal frontend worker
  afterward; control itself never invokes a host or schedules work.
- `{action:"claim", delivery:<entire delivery>}` immediately before the actual
  SDK create/send: returns `{ok:true,allowed:true}` exactly once only while the
  delivery's account/binding/workspace/receipt is current. Also validate current
  frontend owner, accepted friend and SDK login epoch on the SDK thread. Claim
  is the dispatch linearization point; later revoke cannot recall a send already
  issued. A rejected/abandoned delivery is acknowledged, not retried.
- `{action:"ack",delivery:<entire delivery>}` after dispatch or abandonment:
  exactly-once index accounting advances FIFO only after all text/media items
  terminate. Issue this immediately after the real SDK send call returns or a
  dispatch is abandoned; do not wait for network onSuccess/onError. Ordinary
  pending history rows still adopt only real SDK receipts asynchronously. A
  separate native 30-second output handoff deadline abandons missing dispatch
  acknowledgments and rejects late claims without retrying, driven only by the
  existing pump. SDK errors/uncertain delivery receipts do not auto-resend.
- `{action:"takeover",bridge_id,scope,session_id}` is the immediate thread-safe
  local Send/Stop hook, before queuing ordinary Core work.
- `{action:"logout",bridge_id,scope}` is the immediate account/lifetime revoke.

The weak registry plus owned copied delivery avoids raw C context/borrowed
pointer capture in late SDK callbacks. Destroyed/replaced backend tickets are
rejected. Machine outgoing rows are persisted via the same ordinary SDK send
path, with explicit Machine origin and final identity guard; they must not enter
the local-Human takeover marker. Only real nonempty SDK IDs/timestamps can be
called an exact receipt; unknown outcome is reported without automatic resend.

## Model entry and verification

`maichat_session_im_binding` uses the same ordinary native/router API for
list/bind/unbind with flat object schema and normal approval/cancellation gates.
It does not call the frontend request queue from a Core callback. Accepted
friends are supplied by the trusted immutable host projection above.

Verify the old reply tile's actual RED then same-case GREEN; binding UI cancel,
selection, bound state, unbind and draft isolation; shared live-only filtering,
FIFO32/dedupe512, exact receipts, local takeover and account/backend/workspace
revocation; fake SDK dispatch/ack including late media callbacks. Use owned
stores, scripted models and SDK fakes. Preserve the existing d2d4 / iOS206 /
v2.2.59 packages as tested baseline. New source requires new normal target
validation, device install and v2.2.60 packages before claiming that scope ships.
