# Shell pipeline status and curl redirect RCA

## Superseding user decision: preserve Codex shell semantics

On 2026-11-22 the user explicitly required Codex alignment rather than an extra
default pipeline-failure policy. The earlier strengthening described below is
superseded; its measurements remain historical evidence, not the final contract.

Reproduce before changing production: run an owned Bash producer that writes an
error marker and exits 7, piped into successful `wc -l`, with `login=false` and an
empty owned `BASH_ENV`. Expect native exit 0 by default and the diagnostic retained;
expect exit 7 only when the command explicitly starts with `set -o pipefail`.
The deliberately malformed read-only Git query likewise remains native exit 0
in Bash. PowerShell/cmd retain their own existing status rules. No per-stage
status or stderr-regex inference is added. Startup preparation must not inject
pipefail or override an option chosen by the user's profile.

Implementation plan: first update these regression expectations and observe RED,
remove only automatic policy setup and its now-unnecessary snapshot field, then
run the focused pipeline test and full Shell suite. Preserve the other batches,
current installed application and all existing user work.

Final decision verification: the updated focused test first failed against the
automatic-policy implementation (owned status 7 and Git status 128). After removing
the injection and snapshot-policy field, the same test passed with owned/Git Bash
native status 0, explicit pipefail status 7, and an explicitly handled failure
status 0. The complete `MaiShellToolTests` executable then passed (exit 0), including
startup, workdir, stdin, cross-turn sessions, cancellation, output capture and
process-tree cleanup. This run used the current source and the existing normal
Core gate in `MaiAgent/.cache/build-curl-batch4-20261121/build`. Windows Git Bash/Sh,
PowerShell and cmd were exercised. No native macOS run, newly packaged desktop
acceptance, installation, commit, push or release is claimed for this correction.

## Reproduction plan before implementation

- Date/platform: 2026-11-22, Windows x64; main baseline
  `38d25b9d7a41ad29128e95289f44937c676ca452` plus existing authorized worktree changes.
- Preserve all other batch changes. Initial work is read-only RCA, owned cache
  fixtures and this diagnostic; no production changes until the RCA contract is accepted.
- Curl: use an owned loopback HTTP fixture with an empty-body 302 and a synthetic
  Location/query around 4.5 KiB. Without `-L`, expect status 302 and empty body;
  with `-L`, expect final 200 and known bytes. No GitHub request or real signed URL
  is needed. Record the actual newly built executable/source hashes and distinguish
  them from the user's unidentified installed/runtime build.
- Git pipeline: in this repository, run read-only
  `git log --since=30 days ago | wc -l` through owned Git Bash/Core shell and an
  equivalent default PowerShell pipeline. The date must remain deliberately
  misquoted; record real shell-process status and observed diagnostic text without
  assuming that a successful consumer proves producer success.
- Controlled producer/consumer: a producer emits an owned error marker and exits
  nonzero; the consumer succeeds. Compare ordinary status, native supported
  pipefail, and deliberately handled failure (`||`/`try` handling). Do not infer
  exit status from a `fatal` regex or relabel merged output as stderr.
- Study the frozen Codex source at
  `553c764e8b99fa519e9c6eb9b657875ef0f5e1c1` before choosing policy. Any change must
  retain native exit facts, stdout/stderr order, raw output recovery, lifecycle,
  login startup, workdir and quote protections.

## Evidence and decision

The supplied feedback does not identify an actual GitHub asset URL or bind the
reported calls to a specific installed process. No actual asset URL was requested
or saved. The RCA observed the following local Windows process read-only:

- PID 55288, `D:/Program Files/MaiChat/maichat.exe`, started November 20 at
  23:24:13 +08:00; file last written November 17 07:03:38 UTC, 167,359,488 bytes.
- Installed executable SHA-256:
  `2E79DBE81E9DBF7C700D5ED0C0609F5A0E35DD7F83C58C26FF6AA87770A2653C`.
- The normal development-cache executable observed at the same time was different:
  189,290,496 bytes, SHA-256
  `E8A773FFAC43D26966513B96CE223139E3CB5B08B6029A3C721A28E4CE87FFD0`.
  This was the prior integration artifact, before batch-7 modifications.
- No process was stopped and no installation was changed. These facts establish
  an artifact boundary, not proof that this exact process produced the feedback.

Native versions measured on the RCA host: Git Bash 5.2.37(1), Git
2.51.0.windows.2 and Windows PowerShell 5.1.26100.9711.

Actual read-only repository and owned producer/consumer results:

| Case | Native process status | Observed stages/output |
|---|---:|---|
| Bash `git log --since=30 days ago \| wc -l` | 0 | producer 128, consumer 0; count 0 plus Git's ambiguous `days` diagnostic |
| Same Bash query with native pipefail | 128 | same consumer count and diagnostic |
| Bash owned producer exit 7, successful consumer | 0 | producer 7, consumer 0 |
| Same owned pipeline with pipefail | 7 | native aggregate status reflects failure |
| Pipefail pipeline explicitly handled with `\|\|` | 0 | owned handler executes; failure remains visible in output |
| Default PowerShell native Git pipeline to wc/Measure-Object | 1 | count 0 does not conceal the producer failure in this version |
| PowerShell explicitly handled failure | 0 | owned try/catch succeeds |

The diagnostic script captured separate streams only to establish this RCA; the
product intentionally merges stdout/stderr and cannot label a subset as stderr.
Its raw capture implementation was not changed. No `fatal` regex determines status.

The frozen Codex `core/src/shell.rs` derives `-lc`/`-c` without a general pipefail
injection. Its PowerShell safety-parser `ErrorActionPreference` and hook-specific
`LASTEXITCODE` wrapper are not a universal exec policy. Snapshot restoration may
restore user shell options; that does not prove pipeline-stage observation.

The accepted implementation deliberately strengthens batch-7 behavior rather than
claiming it was already Codex's default:

- Bash/Zsh: configure builtin pipefail after login/rc/cwd setup and before the
  original command. Apply the same pipeline setup to login=false without loading
  profiles. Do not enable `-e` or append an artificial exit-status rewrite.
- Sh: use an `if`-guarded subshell probe, suppress only probe diagnostics, then
  enable native pipefail if supported. Unsupported sh still executes normally;
  actual stage statuses remain unobserved.
- PowerShell/cmd: keep native execution/status behavior. Do not inject global
  error-action preferences. Users retain deliberate handling and script-level
  control; `set +o pipefail` also remains effective in supported shells.
- Return `exit_status_scope=shell_process`,
  `pipeline_stage_status=not_observed`, the configured `initial_pipeline_policy`
  and `output_streams=combined_stdout_stderr`. The policy describes initial setup,
  not a guarantee about every later pipeline or a proof that all work succeeded.

## RED and verification

Before production edits, the new owned Core test failed on the real bad Git query,
controlled producer status, absent handler execution and absent scope metadata;
the log reports `git_bash_exit=0 controlled_exit=0 handled_exit=0`.
After the narrow implementation it reports `git_bash_exit=128 controlled_exit=7
handled_exit=0`. Deliberate `fatal` text in a handled script still returns native
zero; explicit pipefail disabling remains zero; PowerShell remains 1 and cmd's
successful final consumer remains native zero with stage-status disclosure.

Curl required no production change. An owned loopback 302 with a **4608-byte
synthetic RPC query** returns empty-body 302 without `-L`, and returns 200/final
bytes with `-L`. The same request with `output_path` publishes the expected bytes.
The regression uses no GitHub account, real signed URL or external HTTP. Existing
five-redirect, protocol, credential-origin and output-size safeguards remain.

Final relevant gate at 2026-11-22 01:28 +08:00: **47 Shell test entries and the full
Curl embedded-CLI suite passed**, CTest 2/2, 19.52 seconds. The preexisting deliberate
curl cancellation diagnostic (42) is expected and its test passes. The first build
was temporarily blocked by a concurrently edited FileSearch header; its owner
completed that change before the recorded RED and GREEN runs.

Owned cache: `MaiAgent/.cache/build-curl-batch4-20261121/batch7/`:

- `native-pipeline-rca.json` / `.log`: direct native reference behavior.
- `pipeline-red.log`, `pipeline-green.log`: actual Core reproduction before/after.
- `curl-long-location.log`: existing explicit `-L` long-Location verification.
- `full-build.log`, `full-ctest.log`: final relevant suites.
- `receipt.json`: final implementation/test-artifact hashes, measured native shell
  versions and point-in-time installed/development executable observations.
- `final-build.log`, `pipeline-final.log`, `curl-final.log`: the final rebuild and
  focused checks after clarifying the public login=false/pipeline-policy comment;
  both focused executables exited 0. No behavior changed after the full gate.

```powershell
. .cache/qt-ui-boundary-20261108/enter-vs.ps1
cmake --build MaiAgent/.cache/build-curl-batch4-20261121/build --target MaiShellToolTests MaiCurlEmbeddedCliTests --parallel 5
ctest --test-dir MaiAgent/.cache/build-curl-batch4-20261121/build -R '^(MaiShellToolTests|MaiCurlEmbeddedCliTests)$' --output-on-failure -V
```

Native Unix/zsh and an unsupported-pipefail sh were not available for runtime
testing here. Windows Git Bash/Sh, PowerShell and cmd were exercised; no device,
installed application replacement or external provider acceptance is claimed.
The user's diff-stat item is an observation, not a new capability request.
