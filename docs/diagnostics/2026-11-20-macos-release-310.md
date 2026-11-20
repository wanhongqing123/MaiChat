# macOS arm64 3.1.0 release supplement

The user requested the missing macOS artifacts on the already public v3.1.0
release. The exact source remains `b20688bab97eee2880450aba05d73db53b37d5ed`,
annotated tag object `78730c5e0b8086e1ef851378dc7472f557b9647e`, release ID
452673037. No release/source tag or existing Windows asset was replaced.

## Target-platform producer

The Mac owner used clean main source at
`/private/tmp/maichat-release-3.1.0-20261120/source`. Preparation initially failed
exit128 because a copied old Qt .git pointer was invalid. Offline clean clones
of the pinned qtbase/qtsvg modules repaired the input rather than falsifying an
SDK stamp. Normal configure then passed in237.686s, normal1567-step Release App
build in408.942s, and the normal package script in51.13s.

The signed App has342 regular files and37 links. The stage, actual ZIP extraction
and readonly-mounted DMG match in bytes, symlinks and modes; all pass deep strict
signature and dependency checks. Dynamic audit covers25 arm64 load images;
actual static link inputs cover83 archive arguments/77 distinct archives and7
direct objects, with no declared minimum above14.4. No LC minimum was edited to
fake compatibility. Original verified14.4 ORT SHA is
`06d76fe8c3d8647b5a8362c011b95aa527e9731d21087bef526735184894777c`;
signed bundled ORT SHA is
`894a4bc0e50cec2cc81a01afc2b0028b6be78531b07808ba90ea14ccc29c5858`.

All three isolated bare-PATH App smokes and packaged-ORT owned3-case OCR probes
passed. An initial DYLD observation assertion failed while App startup itself
passed; setting only the print variable through env argv inside the unchanged
sandbox restored raw child-stderr module evidence. No App entitlement, signing,
sandbox or assertion was weakened. The actual Qt/Cocoa paths were bound to each
App PID; OCR separately established bundled ORT1.26.0 use.

| Copy | Smoke PID / seconds | OCR PID / seconds |
| --- | --- | --- |
| signed stage |12018 /0.707 |20145 /0.820 |
| extracted ZIP |14558 /0.792 |22877 /0.671 |
| readonly DMG |15621 /3.587 |24260 /1.811 |

## Uploaded assets and independent intake

| Asset | ID | Bytes | SHA-256 |
| --- | ---: | ---: | --- |
| `MaiChat-macos-arm64-v3.1.0-20261120-b20688bab.dmg` |772191805 |233724481 |`5b5930e6efd25a9f8ff629eee8b5e4fdcf988b32b1bc466816572719a70fcb62` |
| `MaiChat-macos-arm64-v3.1.0-20261120-b20688bab.zip` |772207184 |234249601 |`97f43e63f0779a498d8f6cf0a457bf637c0665a0fd6f020cc00a97d96401d6a9` |
| `release-manifest-macos-arm64.json` |772232249 |3566 |`72609999e127cdb68ab5c92bf4ffd13273d786d0ebba0327b573e2373c7a95df` |
| `SHA256SUMS-macos-arm64.txt` |772232635 |332 |`b9bd274f54de458830dfe90e3971470c2349d62faac4aa3ee06fd362e214c5cc` |

Root independently verified8 uploaded server assets, exact sizes/digests,
downloaded Mac metadata byte equality and SUMS entries. Mac's four source-tree
IDs match the existing Windows manifest. All four Windows asset IDs, names,
sizes, digests and states remain unchanged. Anonymous public latest readback
confirms v3.1.0 with8 assets. The title/body now cover both platforms and full
UTF-8 body content was read back exactly. A temporary Windows-side text-encoding
mistake in an interim notice was corrected without changing any binary asset.

## Boundaries

Mac signing is ad-hoc, not Developer ID or notarized. Actual execution was on
macOS27.2 arm64;14.4 is a target and inspected minimum, not older-system runtime
acceptance. Intel, real AX receiver/holds/popup/foreign-hit input and window-image
origin calibration remain unverified. No paid model API, private user image,
installed App, service, phone, pairing or TCC change was made. Root public/archive
metadata checks complement target-platform evidence; they do not pretend Root
executed macOS on Windows. Local intake records are under
`.cache/maichat-release-20261120-310/mac-supplement/`.
