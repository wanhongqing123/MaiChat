# Desktop raster input through FFmpeg

Base: `87c0b93e4c228031de51502e251e79cdd376bdb6`; macOS 27.2 arm64,
source-built Qt 6.12.0. This is a new correction after the archived Qt UI delivery.

Reproduction: open an owned 38-byte, 16×12 lossless WebP using
`ImagePreviewDialog(path)`. Expect every pixel to display, as it already does
through the ordinary FFmpeg first-frame decoder. The frozen Windows product test
reports 4 PASS / 1 FAIL: WebP is empty; JPEG and transparent PNG retain their pixels.
Mac must reproduce this same test before replacing the path constructor.

User intent: compressed user raster recognition, validation and decoding share a
Qt-free FFmpeg core for file and memory input. Qt owns the resulting pixels and
draws them. Preserve original image limits, color metadata, all EXIF transforms,
asynchronous owner/cancellation rules, draft resources and video classification.
Inspection success is not proof of a complete pixel decode. The legacy first-frame
API must still support video-cover consumers. SVG and application resources have
separate UI purposes; output encoders are recorded separately.

Ownership: Mac owns image codec/API, desktop presentation adapter, audited callers
and corresponding tests. Windows coordinator owns shared CMake and Windows gates.
No install, publication, real camera, account, user database or provider request.
The installed application and the previous delivery artifacts remain untouched.

Final status (2026-11-09): implementation and desktop verification complete.
Canonical source is `4879c147e9c7d8d3c4ac03b78a0e1cb30d5adcf1`; final
documentation is `8b717c48e137c4548811d9addbe36e5dc9c4cc9f`. The MaiChat tree is
`516356581af214999128fa08ce72baab7db2a47d`; Core remains
`69e18eb1e4b587b30ad8b467dd3172bea0443b8e`.

The same Mac preview case passed 5/0/0 after the decoder migration. Final ordinary
raster tests passed 48/0/0, including the original policy assertions and eight
new AVIF allocation-budget cases. A forged AVIF item size previously hid a larger
coded frame; fresh native parser checks now reject it before decoder allocation.
LLDB independently observed zero allocator calls for the eight rejected cases and
one for a valid control. Composer's original nine cases also passed unchanged.

The normal Mac build completed 19 targets. Its final 18 registered suites all
passed (CTest 59.74 seconds), using a private HOME and loopback-only sandbox.
Existing platform and live-account skips remain. The Windows coordinator recorded
17/17 affected suites passing and independently verified its stage and actual ZIP.

Mac post-commit normal App build, private app-only stage and strict signature
verification exited zero. The runtime audit examined 26 target Mach-O files with
zero failures. Five model files (four Portrait models plus the existing RVM model)
and ONNX Runtime were present. Ordinary and QT_DEBUG_PLUGINS sandbox smokes exited
zero, PIDs 48687 and 48879; the actual DYLD logs locate all five Qt modules and
Cocoa inside the private stage. Source SDK, build and Homebrew access, outbound
network and real user-data access were denied for these smokes.

Authoritative source executable SHA256:
`064e9c9565d55a92be56618532420db1da5fefd4054bb714a51cb039d92636ec`.
Signed stage executable SHA256:
`1c44ade5a6832da4e998250581d2cf7002f265b14ceec719614a0dacb4e331c8`.
The private App is under
`/private/tmp/maichat-ffmpeg-raster-20261109/post-4879-package/MaiChat-macos-arm64/`.
Original logs and machine-readable receipts remain in the parent task directory;
the final package receipt is `post-ff-final-package-receipt.json`.

The formal SDK profile contains qtbase and qtsvg; the old SDK and physical
qtimageformats source checkout remain preserved. Qt owns presentation pixels;
compressed user raster recognition and decoding use the shared FFmpeg component.
ICC bytes are attached as a profile, without claiming an additional color transform.
SVG/UI resources remain explicit vector/presentation paths. The legacy video-cover
API remains separate from strict image recognition.

Runtime evidence is macOS 27.2 arm64 only, not macOS 14.4 or mobile acceptance.
Real devices, live accounts and paid providers were not exercised. The installed
App, user data and public release were not replaced. Final docs synchronization
requires no reconfiguration, build, tests or staging.

Retired source cleanup (20261109-112308): user confirmed qtsvg remains and questioned
retaining the unused module checkouts. Expected result: the Qt source directory
contains qtbase/qtsvg as its only module checkouts, with no retired source checkout
visible in the project. Each retired checkout was verified clean and absent from
the tracked file list before being moved to macOS Trash. Product code, the formal
SDK, build cache, installed App and archived private package are unchanged.
