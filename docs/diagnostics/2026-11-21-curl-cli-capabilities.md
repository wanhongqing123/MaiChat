# curl CLI capability batch 4

- Date/platform: 2026-11-21, Windows x64, owned loopback HTTP fixture only.
- Source baseline: `38d25b9d7a41ad29128e95289f44937c676ca452`, working tree on `main`.
- Reproduction entry: `MaiCurlEmbeddedCliTests`, built separately from the desktop cache.

## Reproduction and expected results before implementation

1. Start the test-owned HTTP server; call the curl tool on `/redirect` without flags.
   Expect HTTP 302. Repeat with `-L`; expect the final HTTP 200 body. A redirect loop
   must fail within five redirects, and `--max-redirs 6` must be rejected.
2. Write a JSON file under the owned temporary workspace; send `-d @payload.json`
   to `/echo`. Expect the file bytes as POST body, with no expansion of bytes into
   diagnostic output or command arguments. Preserve independent raw `-T` upload.
3. Send `-F upload=@input.bin -F title=example` to `/multipart`; expect parsed file
   bytes and literal field value. Reject traversal, inaccessible files, oversized
   bodies, NUL, multipart mini-language escapes and excess fields/arguments.
4. Exercise bounded user-agent and cookie string/file input plus a workspace cookie
   jar. Expect cookie round trip, with Set-Cookie absent from returned headers and
   verbose flags unable to print request credentials. Reject unsafe cookie paths.
5. Follow a redirect to another local host name using sensitive custom headers or
   explicit cookies. Expect the final response without forwarding either credential
   to the new origin. Same-origin redirects must preserve the supplied credentials.
6. Accept redundant silent/include/verbose flags with the existing structured result;
   reject unsupported flags with a safe explanation and a supported-options list.

## Investigation

The existing parser only permits URL, request/header, literal data, upload, head
and compressed. It rejects every requested new flag. `-d @file` currently sends
the literal `@file`; upstream `--data-raw` intentionally does not open files.
The existing response-header allowlist already excludes Set-Cookie. HTTP(S)
protocol restrictions alone cannot stop forwarding arbitrary custom secret headers.

## Validation and runtime boundary

Verified at 2026-11-21 22:03 +08:00 against current working-tree sources on the
baseline commit above. No branch, commit, push, desktop-cache writes, application
replacement or account operation was performed by this curl implementation worker.

The independent build is `MaiAgent/.cache/build-curl-batch4-20261121/build`.
Its local wrapper reuses the checkout's existing OpenSSL static archives read-only;
the Agent, embedded curl and tests are compiled from the current checkout.

```powershell
. .cache/qt-ui-boundary-20261108/enter-vs.ps1
cmake --build MaiAgent/.cache/build-curl-batch4-20261121/build --target MaiCurlEmbeddedCliTests MaiHttpRequestTests MaiDownloadFileTests MaiUploadFileTests --parallel 5
ctest --test-dir MaiAgent/.cache/build-curl-batch4-20261121/build -R '^(MaiCurlEmbeddedCliTests|MaiHttpRequestTests|MaiDownloadFileTests|MaiUploadFileTests)$' --output-on-failure -V
```

- Initial RED: the real local-fixture executable exited 1 with eight failed
  assertions: `-L`, JSON `@file`, multipart, cookie/UA/jar, cross-origin handling,
  path denial and oversized `@file` denial. See `red-build.log`, `red-tests.log`.
- A second RED proved that existing `-I` included raw Set-Cookie in its body:
  four failed assertions, covering both `-I` and `-X HEAD`.
  See `head-red-build.log`, `head-red-tests.log`.
- A third RED proved explicit `-X POST` must remain POST across a 303 redirect;
  its request body is removed, while implicit POST becomes GET. One assertion
  failed before correction. See `method-red-build.log`, `method-red-ctest.log`.
  A 307 fixture separately verifies that the JSON file body is preserved.
- Final GREEN: all four listed test suites passed, 2.59 seconds; curl suite
  0.28 seconds. See `final-build.log`, `final-ctest.log` in the independent cache.
  The existing cancellation test intentionally reports curl status 42 to stderr;
  it succeeds and subsequent requests still work.
- Test process: `build/bin/MaiCurlEmbeddedCliTests.exe`, 8,594,944 bytes,
  SHA-256 `C99A93CED6D9E9A5374F9339C5428F645C93E28A610F33321EB170E136AE9EAD`.
  `provenance.json` records source commit, branch, timestamp and exact binary path.
  This was the newly built executable launched by CTest, with its own HTTP server.

The fixture verifies default no-follow, relative redirect following, exactly six
requests before the five-redirect limit, smaller user limits, non-HTTP redirect
denial, byte-preserving JSON file bodies, multipart files and literals, raw upload,
cookie-string/file round trips, new cookie jars, same-origin credential retention,
cross-origin credential removal, HEAD cookie sanitization, field and argument
bounds, unsupported-option redaction, NUL/control rejection and existing external
files denied even under a broader host file-access context. Oversized data/cookie
files and multipart files are rejected without HTTP transmission.

## Implementation and deliberate limits

- `-L` is opt-in. Up to five hops are executed individually by the same embedded
  curl entry, with one overall timeout and existing HTTP(S), connect-timeout,
  response-size, method and TLS-verification bounds. Every Location is parsed and
  validated with the existing curl URL API. No unrestricted-auth or insecure TLS
  option is exposed.
- Origin includes scheme, host and effective port. All supplied custom headers,
  user-agent and explicit cookie inputs are permanently dropped when it changes.
  The previous hop's private cookie jar is loaded only for a same-origin hop.
  This also covers arbitrary secret header names that libcurl cannot identify;
  the vendored `lib/http.c` only specially protects Authorization and Cookie.
- `-d @file` uses a resolved `--data-binary @absolute-path`, so file bytes and
  newlines are preserved without inserting contents into argv or diagnostics.
  Files are limited to 256 KiB. Raw `-T` upload remains a separate request mode.
  File authorization and size are checked before curl reopens the canonical path;
  this does not freeze source contents or eliminate concurrent replacement races.
- Multipart supports at most 12 fields, safe `name=@file`, literal `name=value`,
  and explicit `--form-string`. File lists and semicolon file parameters such as
  `;headers=@file` are rejected; they cannot introduce unchecked indirect reads.
  Aggregate multipart input is bounded by `max_size_mb`; input has at most 32 args.
- User-agent is at most 1024 bytes; cookie-string/path args at most 4096 bytes;
  cookie input files and private jars at most 256 KiB. Cookie jars require a new
  authorized workspace path and are published from private staging without
  replacing existing data.
- Silent/include/verbose flags retain the existing structured response. Native
  verbose is never enabled; raw stderr is withheld, with exit status retained.
  Returned headers omit Set-Cookie; HEAD's raw header body is cleared as well.
  Application response bodies remain the requested server's response bytes.

Tests use only owned loopback HTTP fixtures and temporary files, without provider
keys or real external HTTP. Desktop installation/UI, real TLS, iOS, Android and
macOS were not exercised by this worker; no device acceptance is claimed.
