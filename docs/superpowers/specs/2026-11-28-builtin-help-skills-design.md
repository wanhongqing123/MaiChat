# Builtin grouped help and bundled Skills

User authorization: implement the reviewed FFmpeg v2 plan and extend built-in
tool discovery with group-level -h and cross-platform bundled complex guidance.
Baseline653efe25; direct main development, preserve unrelated work.

## Reproducible user flow

A new creation-center session receives a small capability index. The model can
select tool_help through existing tool_search, call {arguments:["-h"]} to list
groups, or {arguments:["media","-h"]} for a group's short help. Results contain
only currently public/model-compatible tool names and short purposes. Selection,
permission checks and execution still belong to existing tool_search/registry
and tool executors. No general OpenCV command interpreter is introduced.

Detailed platform-neutral workflows are built-in Skills, discoverable/readable
through existing skill_read. Only metadata enters the ordinary skill directory;
complete body is on demand. User workspace/home Skills are never replaced.

## Shape

A pure ordinary C++ helper derives groups/help from registry.metadata(model).
A thin tool_help adapter parses the bounded -h argv and calls it. The new tool
is deferred: it does not expand the existing bootstrap schema set. Fresh
capability summaries mention the real help route without rewriting old frozen
summary/schema snapshots. Unknown public extensions belong to general; hidden
and incompatible tools never enter results. Help does not execute tools, create
schemas, read credentials or fetch network data.

Single-source Markdown lives in MaiAgent/resources/skills. CMake embeds those
exact UTF-8 bytes into the shared core, used by Desktop/iOS/Android/CLI. Builtin
Skill entries have an explicit Builtin scope and stable builtin: IDs, not host
filesystem permissions. Their self-contained bodies have no relative resource
or script links. Existing manager discovery/read APIs own them; no parallel
Skill executor or APK extraction is needed. The manager opt-in defaults off for
ordinary embedders, while App/CLI hosts enable it next to skillsEnabled. It
requires no platform-dependent user directory writes.

Keep first-run metadata cheap: media, vision, network/security, files, agent
workflow guidance (five self-contained Skills). Bodies use actual current schemas
and help/discovery entry points and do not assert an unavailable codec, provider,
permission or installed system executable. FFmpeg examples use the newly verified
help only after that implementation lands.

## FFmpeg independent writer

Mac exclusively owns bridge/help printf outputs, MaiFfmpegTools header/cpp,
real embedded-engine tests and all existing MaiFfmpegEngine host initializers.
Follow reviewed v2, preserve normal conversion schema/result/approval. Root
owns grouping, resources/manager/build integration and unified commit/push.
No backend/provider/user media, installation or public release operations.

## Acceptance

Help ordinary API and tool adapter return matching groups; actual registry gates
and unusual names/limits/invalid argv/cancel are exercised. Hidden/schema-bomb/
credential-bomb tools prove metadata-only access. Existing catalog immutable
snapshots and real4k budget fixtures still pass. Builtin Skill discovery/body read,
manager ownership, duplicate names and body/count budgets use existing APIs and
must preserve user files. Validate bytes in generated resource and each available
normal consumer, not only source presence. Final Windows/Core/Desktop plus
Android JNI compilation; Mac real engine and normal App consumer. iOS runtime
only if a device/simulator is available; otherwise record that boundary.
