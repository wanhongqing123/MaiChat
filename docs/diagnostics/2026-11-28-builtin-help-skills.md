# Builtin grouped help, bundled Skills and FFmpeg help

Date2026-11-28; source baseline653efe250951f906c2de9556c0d1dab5d925cd96.
User authorized implementation of FFmpeg design v2 (SHA256
9ab3eacd9d85a49c47d4718e215360c939c21893c593270f480cede44d7fa5ca)
and grouped short help plus cross-platform bundled complex-workflow Skills.
No service, provider, user media, installation or public-release operation is included.

## User flow and implementation

Fresh sessions receive compact metadata. Select tool_help through tool_search,
then call arguments:["-h"] or ["media","-h"] (also vision/network-security/files/
agent/general). Groups contain only actual public/model-compatible registrations.
This is metadata help, not a promise of a native command line for every tool.
The ordinary maiBuiltinToolHelp and model adapter share the same renderer; no
schema construction, tool execution, credential snapshot or network fetch occurs.
The deferred help tool does not enlarge the existing bootstrap schema set.
Saved catalog/summary prefixes retain their immutable bytes.

Five self-contained SKILL.md resources are compiled from one shared source set,
without writing/extracting into user skill directories. The normal manager has
an explicit opt-in and Builtin scope with stable builtin: IDs. Metadata is cheap;
existing skill_read and ordinary manager read/readCurrent return bodies on demand.
Existing user/workspace duplicate names remain ambiguous and exact IDs work.
Catalog ownership, count/frontmatter/body limits and UTF-8 checks remain active.
App hosts and CLI enable the opt-in; ordinary manager users remain disabled by default.

## Evidence and actual failures

All private evidence is under .cache/builtin-help-skills-20261128/ and the prior
owned .cache/core-summary-20261128/help-* and skill-builtin-red* logs.
The original real-engine empty-help probe remains in the earlier FFmpeg boundary
diagnostic; source presence or exit0 alone is never treated as help delivery.

Group help first failed actual registry exposure, then full behavior tests showed
37 failures against an unimplemented ordinary helper. Normal Help/Catalog/
DiscoveryRuntime3/3 passed after implementation. Skill tests first showed15
failures with compiled resources but the old manager, then passed with real
resource integration. Initial CMake-target and stale-mtime/link preparation
failures are kept separately; they are not behavioral RED evidence.

Independent review found a real CRLF resource bug: CMake text parsing normalizes
newlines but HEX preserves raw CRLF, so an LF-only metadata delimiter would omit
all builtins on a fresh Windows checkout. Resources now have a Git LF rule and
the manager also accepts CRLF. An actual owned CRLF media resource was rebuilt
through the normal lane and the unchanged MaiSkillTests passed1/1. The normal
LF inputs were restored by the next canonical copy/configure.

First combined Core/Skill validation passed14/15 targets, with a genuine new4k
failure before sampling: the Skill directory's fixed explanatory footer exceeded
the automatic2% window budget. The formatter now uses a compact equivalent
contract before omitting entries, preserving untrusted metadata, explicit-only
selection, discovery access, no-permission and omission facts. No body is loaded
into that directory, and an impossibly small budget still explicitly fails.

The same real4096-token fixtures then passed with actual output reserve128
(input budget3891) and512 (input budget3584). With bundled Skills enabled,
baseline/feature input estimates were3060/3384 and3244/3568 (second case adds550
ASCII base-instruction bytes); summaries972B, initial8 and deferred39, all streams1
and Completed. Original no-Skill fixtures also remain and pass:2670/2972 and
2854/3156, summaries905B, initial7/deferred38. These observations do not guarantee
that arbitrary additional instructions/user input fit the same small window.

Normal post-fix Skill/SkillRuntime/DiscoveryRuntime/ModelClient wrapper4/4 passed.
ModelClient's existing desktop executable lacked CTest registration; registration
was added so the actual normal wrapper runs it, rather than counting a target
build as a passed test. Its local fake HTTP/SSE protocol fixture passed32.92s.
No actual provider was requested. Other required Core tests had passed in the
combined run; relevant Skill/runtime targets were rerun after the formatter fix.

Source/generated-array byte comparison is independent of manager read equality.
Final normal platform/FFmpeg consumer evidence will be appended after validation;
preliminary Mac stage1 source and tests must not be called final acceptance.


## FFmpeg final integration and Windows argument-vector defect

Mac supplied stage1 and stage2 as distinct source packets. Root checked all25 and8
manifest members, exact before/post source bytes and the private component proof's
22 entries. The Mac proof binds one final component binary to real help and full
media tests, with independent stdout/stderr markers; formatting/cancellation
injection cases use controlled executors with the real C text bridge and are
not reported as real media cancellation. Actual archive/executable bytes were
not supplied to Root. The11-archive2393-member minimum-version audit is therefore
Mac-reported evidence, not independent Root binary reinspection. The rejected
27.2-minimum dependency candidate remains excluded from acceptance.

Windows normal App/PE and original embedded media regression passed, but the
new real no-topic help branch crashed. An initial direct launcher had not used
the required Windows test wrapper; its empty output was wrongly suspected to be
a launch preparation issue. The corrected official CTest runtime gate confirmed
Access violation. The original binary was preserved with SHA256
6866d1befc6c730354ae85a8d573dd80ae586c6ef3ebf9b993301ef9421b550d;
local-only CDB under that runtime gate captured ucrtbase!strlen. No memory dump
or symbol-server fetch was made. The owned debugger test was diagnostic only
and is absent from canonical product CMake.

Source inspection independently found cmdutils split_commandline's OPT_EXIT path
reads argv[argc] for optional help topics, while runEngine provided only argc
pointers. The shared ordinary/tool/ffprobe wrapper now reserves n+1, appends
nullptr and still passes the original argument count n. This also preserves
legacy probes using argv[argc-1]. Controlled ordinary2/model5-argument assertions
check the same C argv convention; the real first-help call remains before those
controls. Same Windows scenario became GREEN, with6/6 relevant help/full/catalog/
Skill/runtime targets passing. Pre-fix Mac/Android passes are not evidence that
reading beyond an argv array was safe.

A permanent desktop CTest helper launches --help-output-smoke through the existing
run-tests-windows.ps1 Qt/plugin/error-mode gate, keeps native streams separate,
requires exact three-line stdout and both restored logger markers on stderr.
The nonzero status or timeout fails that gate. It does not redirect App-global
file descriptors. The new check is distinct from the original full media test.

## Platform and artifact checks before unified Mac/iOS consumers

Final Windows normal App/CLI/PE build and17 Core/Skill/embedded suites passed;
after the argv fix the relevant6 targets and final App/help/full/discovery3-target
run passed. Console stdin/stdout e2e uses only a local fake Responses server.
Final private staging and actual ZIP extraction both passed bare-PATH smoke at
App SHA256 b1ea2b7cc731d61a9dda3e3832574fb36f4b55e77dd1f01d1d9cbacdb0c1d8ca.
The pre-argv candidate is explicitly historical/rejected, never a release upload.

Android arm64 normal JNI was rebuilt, then the same source tests linked against
that production core. On an existing arm64 physical device, owned native processes
in /data/local/tmp passed group help, builtin Skills, help smoke and original
media/cancel regression before and after the argv fix. Pushed artifact hashes and
stdout/stderr were checked. No user App was installed, started, replaced or stopped;
no user data/account/media was read. The emulator available here was x86 only and
is not claimed as an arm64 runtime result. The first loader fixture accidentally
included a system zlib link stub from build intermediates; it was preserved under
an excluded name, with system zlib resolved normally. The same product binaries
then passed; this was a fixture loader issue, not a product-source fix.

Independent comparison verifies all five original Markdown byte sequences in the
generated resource arrays AND the final normal Windows App and Android JNI .so.
This establishes packaging beyond a source-file or same-store read equality check.
Root binds38 changed/new canonical input files to the normal r311 overlay lane;
the lane's older Git HEAD is not misrepresented as a clean release checkout.
Windows runner preparation errors (case-duplicated PATH hiding LLVM) and Android
bash/Meson/MSYS argument-conversion setup errors are retained separately; no
compiler target, source or library was bypassed to produce a pass.

## Actual multihop context-window boundary

The initial4k Complete observations above only cover first sampling. An additional
normal Agent fake-provider flow selects tool_help via tool_search, reads grouped
help, then optionally reads builtin-media before sampling again. Every sampled
request is independently checked against the actual window/reserve input budget.
At4096/128 and4096/512+550base, first sample3384/3568 succeeds but the next step is
refused by the existing local capacity guard (InvalidInput with explicit budget/
capacity wording, history retained). It does NOT complete the help loop. The first
probe incorrectly expected the provider-only ContextWindowExceeded code; its two
failed assertions were corrected to the existing local contract, without changing
Runtime/catalog behavior. At8192/512+550base all4 steps complete, maximum sampled
input4906 against hard7680. No arbitrary-small-window or arbitrary-user-input
completion guarantee is made.

Whole integrated Mac and iOS normal consumers will be bound to the unified commit;
Mac FFmpeg component passes alone do not establish those final integrations.
No published tag/assets, installer, backend, credentials or supplier requests changed.
