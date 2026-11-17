# Desktop release integration for 3.0.1

The user authorized current Windows/Mac packages and GitHub publication, then
explicitly selected **3.0.1**. The initial 2.2.66 preparation never created a
tag, draft or assets. Mobile patches and installed applications remain outside
this Desktop publication.

## Reviewed source inputs

Base: `c06a9d5c`, including native input implementation `efd27a7e`.

- Existing completed Starter work: combine screenshot/computer-operation copy,
  remove the basic QA card, keep actual registry/model availability filtering.
- Complete Desktop PDF/Markdown first-screen artifact thumbnails: ordinary
  preparation/render APIs, binding/view, Windows immutable-PDF bytes path,
  CoreGraphics adapter, Markdown prefix and full-original click behavior.
- Latest peer navigation delta: six files, 10672 bytes, SHA-256
  `5d4dedbf9141d5184a76450de3ae715a1f743c9bac552ff701e7468194282858`.
  Exact preimages and all six canonical postimages verified; applied once.
  Same brand blue for both states, hollow/filled selection shapes and resources.

Unrelated `.gitignore`, MobileAgent adapter/tests, Android/iOS and historical
diagnostics WIP are preserved. The two locked Qt source-overlay files are an
expected SDK state, verified separately against the existing source lock.

## Release-blocking regressions closed

1. **Loading geometry:** actual production Panel file card could paint before
   the outer Markdown layout caught up: filename288x6, detail288x5, preview312x17.
   Root normal Windows RED recorded 18 bad paints across initial/retry loading.
   View layout notifications now synchronously notify the owned card through
   existing layout handling. Callback copies/QPointer guards protect deletion
   and rebind. The same test then passed with initial18/retry12 paints and no
   clipped geometry; full-original clicks and failure collapse remain.
2. **UTF-8 EOF:** the stateful one-shot decoder silently retained incomplete
   trailing bytes. Three owned valid-body-plus-EOF fragments produced normal
   RED106/3. Stateless complete-file decoding rejects those cases; valid BMP,
   supplementary Unicode and BOM controls remain.
3. **Prepared lifetime:** completed watchers retained full prepared PDF pages
   outside the final-image cache. Root normal weak-lifetime RED132/3 covered
   normal, cleared and failed results. Old result consumption now happens before
   generation/owner checks and any external completion; it never clears a new
   future created by a reentrant callback. Root normal GREEN132/0 closes all
   three cases. The final-image LRU is not a total process-memory guarantee.

All fixes had independent source review. No new library, SDK, driver or thread
architecture was introduced for them.

## Fresh Windows integration checks

- Normal target build exit0.
- Starter109/0; thumbnail132/0; Markdown document target passed.
- Full file-delivery Windows UI8/0, no skip; loading/ready/failed/retry,
  original PDF/MD preview, workspace/owner changes and restored history included.
- Affected navigation Windows UI12/0, including all six icon states, filled
  cloud and fixed caption while settings scroll. Peer Mac13/0 remains historical
  UI evidence and is not substituted for the new Mac package gates.
- Owned final document-card and navigation captures were visually inspected.

Evidence: `.cache/maichat-release-20261117/`, including `thumbnail-utf8-red.txt`,
`thumbnail-memory-red.txt`, `thumbnail-loading-red-qtest.txt`,
`thumbnail-loading-green-qtest.txt`, `integration-unit-green.txt`,
`file-delivery-full-green-qtest.txt`, `nav-green-qtest.txt`,
`document-cards-final.png` and `nav-final.png`.

These checks precede the version-only package freeze. Complete 3.0.1 App builds,
actual archives, runtime/closure/signature checks and server asset readbacks are
separate required release gates recorded by the release plan/manifests.
