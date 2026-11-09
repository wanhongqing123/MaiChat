# Owned JPEG EXIF fixtures

These assets contain only a generated 96 x 64 RGB image. No user media, camera,
product build or installed application is involved. Products and formal tests
are unchanged.

## Layout and oracle

The source has six solid 32 x 32 cells, read left to right:

| Row | Left | Middle | Right |
| --- | --- | --- | --- |
| Top | RGB(224,32,32), red | RGB(32,192,64), green | RGB(32,64,224), blue |
| Bottom | RGB(32,192,192), cyan | RGB(192,32,192), magenta | RGB(224,192,32), yellow |

The source layout and explicit EXIF output-to-source coordinate formulas produce
the expected pixels. Qt, FFmpeg and Pillow's EXIF transpose helper are not used
as the orientation oracle. The coordinate origin is top left, x grows right and
y grows down. Orientation 1..4 displays 96 x 64; 5..8 displays 64 x 96.
The tag definitions are independently documented by [Adobe's TIFF namespace](https://developer.adobe.com/xmp/docs/xmp-namespaces/tiff/)
and [CIPA DC-X010-2017, Table 3](https://www.cipa.jp/std/documents/e/DC-X010-2017.pdf).

`manifest.json` gives each fixture's hash, dimensions, EXIF tag, transform,
expected corner RGBA, inset sample locations, tolerances and corresponding raw
RGBA oracle. `expected-orientation-N.rgba` is top-down, tightly packed RGBA8,
with no header and stride = expected width * 4. RGB absolute error is at most
12 per channel for JPEG comparisons; alpha must be exactly 255. Test the inset
corner samples, or all pixels against the raw oracle with that tolerance.

Pillow encodes the source **once**, at quality 95 with 4:4:4 sampling. Each of
the eight EXIF variants inserts a 36-byte APP1 segment into those same bytes.
Removing that inserted segment recovers the base JPEG exactly; every variant
has identical SOS/entropy/EOI bytes. Only the EXIF Orientation value differs.
The generator verifies these identities without decoding. A Pillow base-image
decode checks JPEG error only; it does not supply expected orientation pixels.

## Trusted ICC fixture

`sRGB2014.icc` is copied without changes from the already present pinned Qt
upstream test resource, not generated or inferred from a filename. The manifest
records the exact repository path, commit, SHA-256, ICC description and embedded
copyright. LittleCMS parses that source profile and successfully builds an RGB
transform. This does not claim independent ICC conformance certification or
display color-management accuracy.

`exif-orientation-6-srgb2014.jpg` uses the same JPEG scan and Orientation 6,
with one APP2 ICC segment. Its expected display pixels match orientation 6,
and a decoder must preserve the profile's exact bytes if it exposes ICC data.

## Reproduce or verify

Run from any working directory:

```sh
python /path/to/owned-fixtures/generate_fixtures.py --verify-only
```

Verification needs only Python's standard library and does not modify files.
Regeneration omits `--verify-only` and requires an already installed Pillow with
JPEG and LittleCMS support. It writes only this script's own directory. First
generation copies the pinned repository ICC resource; later regeneration can
use the included exact profile. Encoding with a different Pillow/libjpeg version
may change JPEG hashes, so use the supplied assets for exact cross-platform tests.

`SHA256SUMS` covers every file except itself. No test or build was performed
against a product decoder; future FFmpeg tests must establish decoder behavior.
