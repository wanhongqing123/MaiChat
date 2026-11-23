# Shell environment and interpreter parity

## Reproducible expectation before implementation

- Baseline: Windows x64, main `38d25b9d7a41ad29128e95289f44937c676ca452` plus
  the completed batch-6 working-tree changes. No branch/commit/push.
- Reference is the local Codex checkout frozen at
  `553c764e8b99fa519e9c6eb9b657875ef0f5e1c1`: `shell-command/src/shell_detect.rs`,
  `core/src/shell.rs` and `core/src/shell_snapshot.rs`.
- A default `shell` command using PowerShell syntax should execute on this Windows
  machine through Windows PowerShell when pwsh is unavailable. `shell=cmd` should
  preserve explicit cmd syntax. Before the change, default execution always uses
  `cmd.exe /d /c` and there are no shell/login parameters.
- The ordinary argv builder should give POSIX zsh/bash/sh `-lc` for login and `-c`
  otherwise; PowerShell should add `-NoProfile` only for login=false, followed by
  `-Command`; cmd must retain AutoRun suppression and correct command framing.
  The final launch is `/d /s /c` with a separate outer command quote pair.
  Confirm with deterministic typed tests and
  actual existing Windows PowerShell, without creating/editing user profiles.
- POSIX default detection must read the account shell with `getpwuid_r` and not
  rely only on an environment variable a GUI process may lack. Native argv must
  be fully prepared before fork. Windows must launch a canonical executable via
  `lpApplicationName`, with proper non-cmd argv quoting and existing Job/pipe guards.

## Design and environment boundary

`MaiShellEnvironment` resolves interpreter type and executable using the ordinary
Core file APIs and existing PATH. Model-provided shell paths select a recognized
type before discovery, matching Codex's `get_shell_by_model_provided_path` behavior.
The native process inherits the current environment; login startup uses the chosen
interpreter plus the direct child startup helper below. No inherited environment
values are written to source, logs, snapshots or fixture files. No host/user profile
is copied, created or modified; tests create only owned synthetic rc files.

The Windows discovery order is pwsh on PATH/common installed path, then powershell
on PATH/system location, with cmd as the final default fallback. macOS selects the
account shell and then zsh/bash; other POSIX selects account shell then bash/zsh;
the final POSIX fallback is /bin/sh. Explicit unavailable selections fail clearly.
The latter is a deliberate fail-closed difference from Codex's unknown-type fallback.
Windows launches the canonical executable via `lpApplicationName`; POSIX preserves
the final interpreter entry name, so `/bin/sh` resolving to Bash/BusyBox retains
its `argv[0]` mode. All POSIX argv storage is prepared before fork.

`maiShellStartupCommand` prepares the original command without evaluating host
environment values:

- Login Zsh sources readable `.zshrc` from `ZDOTDIR`, then `HOME` as fallback.
  Login Bash sources readable `$HOME/.bashrc` only when `BASH_ENV` is empty, matching
  `shell-command/src/startup.rs`. The shell's normal login startup still runs.
- Login Sh uses the non-evaluating `ENV` expansion from
  `shell-command/src/shell_snapshot.rs` and the readable/non-directory source guard
  from `shell-command/src/shell_snapshot_capture.rs`. Supported path forms include
  literals, `~/`, `$VAR`, `${VAR}`, `${VAR:-default}` and `${PATH%%:*}` with supported
  suffixes. Unsupported expressions remain literal; there is no `eval`. Relative
  source names use `./`. MSYS drive-letter absolute paths are accepted as an explicit
  interoperability extension, still quoted and never evaluated.
- These POSIX helpers restore the requested working directory after rc startup and
  put the original command on a new input line so configured aliases can be parsed.
  Scratch variables/helpers are removed. No aliases, functions or exports are
  captured by the host or serialized into later launches.
- Login PowerShell restores the requested directory with module-qualified
  `Microsoft.PowerShell.Management\Set-Location -LiteralPath`, using correctly
  escaped single quotes. A local catch explicitly exits on failure, including
  when a profile has a global `trap { continue }`. This is a workdir protection
  beyond Codex's plain argument derivation. `login=false` keeps `-NoProfile` and
  the original command, and cmd has no added startup helper.
- Cmd keeps `/d` to suppress AutoRun and adds `/s` plus a whole-command outer quote
  pair. This preserves a quoted executable path together with quoted arguments;
  the actual reproduction failed under the previous raw `/d /c` framing.

The startup helper is adapted from the frozen reference above. It runs afresh per
new process; normal profiles may independently source the same files according to
the user's configuration. This differs from Codex capturing and replaying cached
shell state. Profile edits and startup side effects are consequently observed at
different times; cached state replay and credential-filtered snapshots are not
implemented or claimed.

The adapted Sh ENV loader comes from OpenAI Codex, Copyright 2025 OpenAI, under
Apache License 2.0. The frozen source's `LICENSE` and relevant OpenAI Codex `NOTICE`
are the license/attribution inputs for distribution; no Ratatui/UI code is used.
Distribution copies are maintained in `MaiAgent/third_party/codex-shell/` and
desktop packaging is validated separately by the integration owner.

PTY remains outside scope. Codex's snapshot support explicitly excludes PowerShell
and cmd (`core/src/shell_snapshot.rs`, `write_shell_snapshot`); this change uses the
direct non-snapshot startup path with the explicit workdir protection above.

Empty `shell_input` polls now accept up to 300000ms; initial waits, writes and
termination remain capped at 30000ms. Output preview uses head and tail. For a
capture exceeding 16MiB, the manager also returns a display-only live ring tail
with its absolute stream offset. This does not consume the raw-output cursor or
append missing middle bytes to the persisted record: the original 16MiB prefix
snapshot stays partial, and remaining raw tail bytes still require another poll.

## Validation

The initial expanded-contract RED found eight failures, followed by four dedicated
PowerShell punctuation-approval failures and nine direct-startup-helper failures.
Additional actual reproductions found quoted cmd executable/argument framing,
PowerShell cwd restoration, global-trap continuation after failed restoration,
Sh `ENV` startup and missing true tail display in the first >16MiB response.

Windows native tests use existing Windows PowerShell and explicit cmd, including
quoted argument round trips and interactive stdin. Owned Git Bash/Sh child fixtures
run `--noprofile --norc -c`/`-c` with only synthetic rc/ENV files, validating aliases,
functions, exports, PATH updates, apostrophes in cwd, directory restoration and
non-evaluation of command substitution. The parent test process does not alter
HOME or user startup files. These fixtures validate the startup helper, not native
macOS login/profile behavior. POSIX/macOS account discovery and fork execution were
neither compiled nor run on this Windows host.

Final independent verification at **2026-11-22 00:12 +08:00** passed all **46 shell
test functions and eight additional Core suites**, with zero skips. The final
native flood processed 24,576,000 payload bytes in 419ms, and 132 completed records
crossed the pruning boundary in 4254ms. The receipt verifies the current source
hashes of the frozen implementation and test revision at that gate against the
actual rebuilt executable:

- `MaiAgent/.cache/build-file-batch2-20261121/codex-parity-validation-receipt.json`
- `codex-parity-MaiShellToolTests-final.log` plus its eight Core-suite final logs
- Final test executable SHA-256:
  `6623F0775027403F4CEF3CDA777A93704BF7B5ADBAAA7B0A62447A38724E4801`
- Environment implementation SHA-256:
  `AAE27C14D050C94A441D44CDF07B637CF07D94D0333B26DC83701AB6037AF54F`
- Manager implementation SHA-256:
  `6534E19AA9CB3179BCE0C968FD176E66FDCD99C53F41AA82B5585FA73709B3A7`

The exact RED logs in that independent cache are `codex-parity-red.log` (8),
`codex-approval-red.log` (4), `codex-startup-red.log` (9),
`codex-cmd-quote-red.log` (2), `codex-ps-cwd-counterfactual-red.log` (4),
`codex-ps-guard-red.log` (2), `codex-sh-env-red.log` (3) and
`codex-capture-tail-red.log` (1). The PowerShell cwd counterfactual used an isolated
old environment object with the corrected fixture; production source was not
cycled back during concurrent integration work.

An additional independent Core cache built and ran the same 46-function suite:

```powershell
. .cache/qt-ui-boundary-20261108/enter-vs.ps1
cmake --build MaiAgent/.cache/build-curl-batch4-20261121/build --target MaiShellToolTests --parallel 5
& MaiAgent/.cache/build-curl-batch4-20261121/build/bin/MaiShellToolTests.exe
```

Its logs are `shell-parity-complete-build.log` and
`shell-parity-complete-tests.log`: exit 0, zero skips, 24,576,000 bytes in 233ms,
132 completed records in 4347ms. Its final functional run preceded only the
attribution-comment addition; the separate final receipt above includes that
comment in the source/artifact hashes. Formatter and `git diff --check` passed.

No branch, commit, push, provider call or external HTTP was used. The document
filename retains the November 21 start date; final validation crossed midnight.
