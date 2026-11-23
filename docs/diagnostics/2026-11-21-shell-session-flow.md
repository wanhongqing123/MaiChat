# Core shell session flow

This records the initial batch-6 milestone. The subsequent interpreter/login,
fresh rc/ENV startup, five-minute empty polling and live-tail preview extension is
documented in [shell-environment-parity](2026-11-21-shell-environment-parity.md).
Its later contract supersedes the initial fixed cmd/sh selection below.

## Reproduction and expected behavior before implementation

- Windows x64, current `main` working tree; baseline commit
  `38d25b9d7a41ad29128e95289f44937c676ca452`.
- Build/run the owned `MaiShellToolTests` executable from an independent Core
  cache. Child fixtures are modes of that executable, without provider/network
  calls, user credentials, PowerShell, PTY or interactive windows.
- Launch a fixture that writes a marker and waits on stdin using `shell` with
  `yield_time_ms=0`. Expect JSON `state=running` and an opaque process session ID.
  Old behavior blocks; the new schema and `shell_input` are absent.
- Send bounded stdin through `shell_input`; poll with empty chars. Expect only new
  output, exit status after completion, and rejection under a different Agent
  session. Ordinary Agent turn cleanup must leave the command alive.
- Resolve `workdir` using the existing tool workspace policy. Commands with the
  same text and different directories must have different approval scopes.
- Terminate a fixture with descendants, cancel a wait, reach the hard timeout,
  delete its owning Agent session and destroy the manager/Agent. Expect the whole
  owned process group/Windows Job to stop and handles/threads to be reclaimed.
- Start twelve live processes for one owner; the thirteenth must fail. Verify
  bounded stdin, 1 MiB tail ring/discard accounting, 16 MiB capture compatibility,
  bounded preview estimates and existing persisted `tool_output_read` recovery.

## Design

`MaiShellSessionManager` is an ordinary Core API shared by the paired shell tools.
Each launch owns its worker, stdin queue, cancellation state, native process group
or Job and timeout. Calls borrow cancellation callbacks only for their synchronous
wait. Workers never retain a tool context, caller cancel pointer or output-store
pointer. A completed response freezes raw bytes and the adapter saves that chunk
through the existing output store before advertising its handle.

The manager retains a capped 16 MiB prefix capture for existing output recovery
plus a 1 MiB rolling tail. Its internal cursor advances per interaction; gaps and
discard counters are explicit. There are at most twelve live processes per Agent
session and twenty-four live or unread processes per manager. Consumed terminal
records are pruned at 128; completed records do not consume the per-owner live
limit. Normal turn completion does not terminate processes. Actual session
deletion and Agent/manager destruction perform idempotent owner cleanup.

## Implemented contract

- `shell` keeps `command` and its existing command/read-only approval rules.
  Contextual approval keys bind the full command and canonical `workdir` in a
  serialized tuple. `timeout_ms` remains a hard tree limit, default 120000 and
  clamped to 1000..600000. `yield_time_ms` defaults to 10000 and accepts 0..30000.
- A command completing during the initial yield keeps the existing human preview,
  notes, output-store handle and execution facts, adding a `[shell_result]` footer
  with `exit_status`. Otherwise it returns JSON `session_id`, `state=running` and
  `output`, with `execution.status=Running` and `shell_input` recovery guidance.
- `shell_input` requires `session_id`; `chars` omitted/empty polls without approval.
  Nonempty input and explicit termination use owner/process/exact-input approval
  scopes. Its yield defaults to 250ms for writes and 5000ms for polls, bounded to
  0..30000. `terminate=true` cannot accompany input. No PTY, reused shell, login
  mode, alternative shell selection or implicit stdin EOF is implemented.
- `max_output_tokens` defaults to 10000, clamped to an effective 48 KiB preview
  using an explicitly approximate four-bytes-per-token calculation. Metadata and
  recovery notes are outside that preview budget; no tokenizer precision is claimed.
- Each frozen raw chunk is saved synchronously through `tool_output_read` when a
  valid current tool output-store identity is present. The original 16 MiB prefix
  cap remains; the tail ring can expose later bytes through another input poll.
  `ring_discarded_bytes` describes ring eviction, while `discarded_bytes` reports
  unread gaps that can no longer be recovered. No model-controlled cursor is used.
- Windows starts suspended with restricted inherited pipe handles, attaches the
  kill-on-close Job before resuming, and uses overlapped bounded stdin so a child
  that does not read cannot stall cancellation. Terminal cleanup verifies the Job
  has no remaining active process. POSIX starts a process group and requests
  SIGTERM followed by SIGKILL for explicit termination/cancellation/timeout.
- Ordinary `maiRunProcess` retains its existing blocking behavior for media and
  other callers. Its existing Windows UTF-8/OEM formatter is shared for completed
  shell text; raw snapshots remain byte-preserving. Streaming or capped UTF-8
  fragments are escaped as `\xHH`, rather than silently dropped.
- Generic `onSessionClosed`/`onAgentShutdown` tool hooks run only on actual session
  deletion or Agent shutdown after active work threads finish. Both shell tools
  share the same manager. Normal `onTurnFinished` remains a no-op for them.

## Validation

Initial actual old-tool RED: six assertions failed for absent `shell_input`, new
schema parameters and ignored `workdir`. Subsequent real native tests first caught
the output-pipe throttle: a 4 KiB pipe plus unconditional 10ms sleeps could not
finish a 24 MiB fixture in five seconds. The correction uses a bounded 64 KiB
Windows pipe and immediately continues after real output progress, checking input,
cancellation and timeout between bounded drains. A separate RED caught resumed
timeout messages incorrectly reporting 120000 instead of the original 1200ms.

Independent Core commands:

```powershell
. .cache/qt-ui-boundary-20261108/enter-vs.ps1
cmake --build MaiAgent/.cache/build-curl-batch4-20261121/build --target MaiShellToolTests MaiCoreTests MaiToolLoopTests MaiPermissionTests MaiToolOutputReadTests --parallel 5
ctest --test-dir MaiAgent/.cache/build-curl-batch4-20261121/build -R '^(MaiShellToolTests|MaiCoreTests|MaiToolLoopTests|MaiPermissionTests|MaiToolOutputReadTests)$' --output-on-failure -V
```

The 2026-11-21 22:41 Windows Core gate passed all five suites in 14.25s; shell ran
31 cases in 10.33s. Its real native flood consumed 24,576,000 payload bytes in
499ms (`total_output_bytes=24576059`, including fixture markers). The tests cover
stdin/polling across ordinary turns, foreign-owner denial, exact approval scopes,
workspace resolution, preview budgets, overflow/capture recovery, twelve-live
capacity, cancellation without stdin transmission, timeout, explicit termination,
and actual parent/descendant PID cleanup after session deletion and Agent shutdown.

Logs are under `MaiAgent/.cache/build-curl-batch4-20261121/`:
`shell-final-build.log`, `shell-final-ctest.log`, `shell-provenance.json`.
The provenance records the exact rebuilt executable, source commit and source
hashes. The separate test worker also runs the same native cases from its own
Core cache, without using the desktop build directory.

Final independent verification at 22:49 uses the final encoding correction and
33 shell test functions, including 132 completed/consumed process records through
cache pruning (4263ms), completed incomplete UTF-8 and running UTF-8 fragments.
All shell functions and eight additional Core suites passed. Its flood result was
24,576,000 bytes in 296ms, with both discarded-gap counters and the persisted tail
marker verified. The final adapter hash is
`01FFC1D5FC528240EABC7CEDA68763EA037728D96E32028B1C1A8FCE09F2B8AC`;
the manager hash is
`F46F27680A53483D2F4EC7F2FCEE392EF489579CC67D45E35AB47FAB8B280052`.

The independent receipt is
`MaiAgent/.cache/build-file-batch2-20261121/batch6-validation-receipt.json`.
It checks all twelve source hashes against the built artifacts and records the
actual final test executable SHA-256
`A3FB5B0D9DC98C229D42287A2B615C2E182E5FBDAA82372583F82760636F64CF`.
`batch6-MaiShellToolTests-final.log` and the eight other `batch6-*-final.log`
files contain the actual runs. The initial six-failure RED transcript was copied
from the original tool result into `batch6-baseline-red-transcript.txt` and is
explicitly labeled a transcription. The encoding regression was also demonstrated
with an actual counterfactual old-adapter object, built only in the independent
cache: two assertions failed before the fix; see
`batch6-fragment-counterfactual-red.log`. Production source was never reverted
during the concurrent desktop build to obtain this evidence.

The shared `MaiTool` virtual-interface addition requires coherent rebuilding of
derived tools and tests. A stale independent-cache object initially caused an ABI
crash; rebuilding all consumers resolved it. Such an artifact is not counted as
runtime evidence for the updated source.

## Verification boundary

Windows native Core execution uses the actual rebuilt executable and owned fixture
directories/processes. No real model/provider request, external network, user key
or user data is used. POSIX was neither compiled nor run in this Windows-only
verification; iOS is explicitly excluded from fork/exec compilation. Device and
desktop installation/UI acceptance are owned separately and are not claimed here.
