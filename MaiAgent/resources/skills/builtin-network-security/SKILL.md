---
name: builtin-network-security
description: Choose local network, TLS, certificate and archive tools with explicit data and output boundaries.
---

# Network and security workflows

Browse `tool_help({"arguments":["network-security","-h"]})` and use
`tool_search` to load the exact schema of a listed tool. This App may expose a
bundled curl/OpenSSL command or typed crypto tools depending on platform. Do
not assume that a system shell executable or native `-h` exists for every entry.

Separate local inspection from external actions. A digest or certificate parse
can be local; curl, upload, SSH and HTTP requests can transmit user data. Use the
actual tool's URL, host, method, body and permission contract. Never paste a
credential into a URL, a command-line argument, a report or a temporary example.

For a certificate problem, inspect the certificate and its validity/chain
information before proposing configuration changes. Do not disable TLS
verification to make a request appear successful. For hashing, choose the
registered digest algorithm and compare the complete bytes/hash from the same
artifact; matching filenames are insufficient.

For HTTP diagnostics, distinguish transport error, status, content type and
application error. An HTTP200 alone does not establish API success, and an
HTML proxy failure is not valid JSON. Preserve redirect/protocol and body-size
limits. Avoid repeated external probes when a local fixture or returned error
already answers the question.

For compression/archive work, preserve input files, choose new destinations and
respect expansion/path limits. A compression codec is not automatically an
archive extractor. Ask for the actual operation schema before attempting a
format or mode that the tool has not declared.

Report exactly what was observed. These instructions do not authorize remote
writes, service restarts, data export or vendor calls beyond the user's task.
