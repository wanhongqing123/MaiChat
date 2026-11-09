# Windows builtin curl trust and remote failure visibility

## Report and acceptance

On 2026-11-09 the user reported an installed Windows client's builtin curl
failing in a session viewed from iPhone. The expanded tool card only displayed
`curl · 失败`. The model said that root certificates were missing, but the
original curl diagnostic was not available in the phone UI. That explanation
alone does not establish the affected machine's exact TLS error.

Reproduction: invoke the ordinary builtin curl factory with an empty CA path
against an owned TLS server whose authority is available through a private
Windows-native-store adapter. Expect a verified handshake. Incorrect authority,
malformed/missing CA files, hostname mismatch and an untrusted chain must still
fail. Separately, publish a failed curl part from the desktop owner and expand
its remote tool card: an actionable code/category must be visible without
publishing request parameters, credentials, responses or raw stderr.

The starting source is `7b41ccf088edd29482abe12edeb159a59f0d0c85`
(a documentation-only successor of `631f5d9c`). Evidence is retained privately
under `.cache/curl-ca-portability-20261109/`.

## Located causes

Windows builds use vendored curl 8.11.1 with OpenSSL 3.5.9, not Schannel.
Ordinary builtin registration calls the curl factory without a CA path.
`MaiCurlCliTool.cpp` previously added only an explicitly supplied `--cacert`;
it did not request `--ca-native`. The CLI's default native-store flag is false.
In contrast, the OpenAI/Responses model clients already request
`CURLSSLOPT_NATIVE_CA` when their explicit CA path is empty.

This is a tool-specific trust configuration gap, not proof that every package
omits CA bytes. The verified 2.2.53/2.2.54 Windows executables contain the complete
188900-byte `:/maichat/auth/cacert.pem` resource, SHA256
`a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505`.
However, the builtin CLI does not consume that Qt resource, and those packages
have no standalone `curl-ca-bundle.crt` for its file search.

The second cause is independent: the Core runner retains the curl failure JSON
in `MaiToolPart.error`, but `DesktopSessionSyncHost` omitted ordinary tool errors
from its remote history projection. iOS already preserves and displays received
`error` text when the tool group is expanded. The desktop remote panel additionally
dropped the field in its own projection and displayed only tool name/state.

## Changes

- On Windows only, an empty factory CA path now adds `--ca-native`. A nonempty
  factory path continues to use only `--cacert`, so native trust cannot mask a
  bad or missing explicit CA file. Peer/hostname verification and the model's
  permitted CLI argument set are unchanged.
- The desktop host accepts only a finite integral JSON `exit_status` in 1..255
  from an errored curl part. It projects static summaries for 6/7/28/60/77 and a
  static code-only summary for other valid codes. Parsing is bounded before
  allocation to 98,560 bytes, covering a 16 KiB curl log's worst JSON escaping.
  It never forwards input/output/log_tail, URLs, headers or response bodies.
  Other tools' redaction and the question protocol remain unchanged.
- The desktop remote panel retains this bounded curl-error summary in its
  fingerprint and renders it as plain text. A late summary with the same tool
  id/state refreshes the existing expanded card. iOS source was not changed.

## Windows verification

The existing Core test target was built from current checkout sources through
the normal Core gate. Its opt-in local fixture uses two owned loopback TLS
servers and freshly generated one-day certificates. Only the test executable's
`CertOpenSystemStoreA` import is adapted to return private memory ROOT/CA stores;
curl's original Windows importer and TLS verifier still run. No test authority
is installed in the actual Windows trust store. A raw `--ca-native` control
must handshake successfully and open both memory stores before any tool result
is accepted as evidence.

The test executable was relocated, with empty working directory, System32-only
PATH, no CA/proxy environment overrides and no `curl-ca-bundle.crt`. Original
production: control exit 0/native opens 2; default tool exit 60/native opens 0.
Five assertions failed across the default/native branches. After the change:

| Case | curl exit | Native-store opens |
| --- | ---: | ---: |
| Default authority | 0 | 2 |
| Explicit correct CA | 0 | 0 |
| Explicit unrelated CA | 60 | 0 |
| Explicit malformed CA | 77 | 0 |
| Explicit missing CA | 77 | 0 |
| Explicit CA, wrong hostname | 60 | 0 |
| Native trust, wrong hostname | 60 | 2 |
| Native trust, untrusted chain | 60 | 2 |

The local TLS assertions were unchanged. Additional public-control logging was
added to the test after RED; it does not change these assertions. All eight TLS
cases and the original HTTP checks then exit 0. Ordinary registered Core gates
also pass 9/9 in 229.00 seconds: Core, Store, ModelClient, TurnRunner, Tool,
Question, ToolLoop, Permission and CurlEmbeddedCli. The TLS matrix is opt-in and
is recorded separately from the default HTTP suite.

Remote host projection uses the original history APIs and temporary SQLite with
fake sensitive fields. Original production records 16 PASS / 6 FAIL; the same
20 data rows plus setup/cleanup record 22 PASS / 0 FAIL. A separate real remote
panel regression first fails 2 PASS / 1 FAIL, then passes 3/0 with the same
assertions. Its owned screenshot was viewed: the expanded card shows the code
and category. The full registered desktop host suite passes; the raw QtTest log
records 74 PASS / 0 FAIL / 0 SKIP in 20058 ms. The registered CTest run takes
20.37 seconds. Initial offscreen startup could not find a deployed QPA plugin;
its Qt modal stack was retained and only that owned process was stopped. The
actual RED/GREEN UI runs use Windows QPA, not that setup failure.

Normal `maichat` and host-test targets build with exit 0. Post-build deployment
verifies three binaries, 114 PE imports and Qt plugins. The existing installed
application is not stopped or replaced.

## Public control and remaining boundaries

Only a few credential-free public HEAD requests were made. The first direct
OpenSSL/native control to example.com failed 60, so it did not validate the tool.
System Schannel HEAD succeeded. A later OpenSSL/native control imported ROOT/CA,
reported verify_result 0 and HTTP 200; the corrected default tool also succeeded.
The cause of this control's environment change was not established. Normal OS
chain/root retrieval by Schannel is a possibility, not a measured conclusion.
The deterministic loopback RED/GREEN is the evidence for the production change.

The affected user's original stderr, endpoint and process environment remain
unavailable. This fixes the confirmed default trust gap; it does not establish
that every server chain or machine trust store is healthy. No verification is
disabled, and there is no insecure retry. Other HTTP tools' trust policies are
outside this change. iPhone rendering was traced in source, not accepted on a
real iPhone in this Windows task. No version, CMake, published asset or earlier
2.2.54 candidate package was changed or relabeled.

Upstream behavior: [curl certificate verification](https://curl.se/docs/sslcerts.html)
and [CURLSSLOPT_NATIVE_CA](https://curl.se/libcurl/c/CURLOPT_SSL_OPTIONS.html).
