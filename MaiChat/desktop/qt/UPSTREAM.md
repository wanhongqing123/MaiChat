# Qt source dependency

The desktop runtime is built from the Qt 6.12.0 source modules in this directory.
`sources.lock.json` records the exact upstream commits; the Git submodules use
shallow checkouts, avoiding a copy of Qt's complete development history.

```sh
git submodule update --init --depth 1 -- MaiChat/desktop/qt
python3 MaiChat/desktop/scripts/build-qt.py
cmake -S MaiChat/desktop -B MaiChat/desktop/build -DCMAKE_BUILD_TYPE=Release
cmake --build MaiChat/desktop/build --parallel 4
```

The desktop CMake configure also invokes the same incremental bootstrap. It does
not download source or install Qt system-wide. Module build trees remain at
`qt/.build/<platform>-<architecture>/{qtbase,qtsvg}` and are reused. The lock's
`sdk_profile: widgets-native-images` selects the independent
`install-widgets-native-images` prefix and `installed-widgets-native-images.json`
stamp under that same build root. Existing `install`, `installed.json`, and
retired module build/source directories remain untouched. A lock without
`sdk_profile` retains the original `install`/`installed.json` paths. Profiles must
be single ASCII names, starting with a letter or digit and containing only
letters, digits, underscores or hyphens; the fingerprint includes the profile.
Profile reconfiguration of external modules clears only cached `Qt6*_DIR`
package lookups before selecting the new `Qt6_DIR`. This avoids retaining old
Core/BuildInternals/Tools paths while keeping compiler and feature options.
Examples, upstream tests and precompiled headers are disabled. The new shared
Release SDK does not require a second copy of the module build trees, complete
App or models.

Required tools: CMake 3.25+, Ninja, Python 3.10+, a C++17 compiler; MSVC 2022 for
Windows. The pinned qtbase declares macOS 14.4 as its minimum supported version.
This dependency profile is for the Windows and macOS Widgets desktop product;
the iOS/Android native products do not consume this Qt SDK.

Formal modules: qtbase and qtsvg, at the pinned commits in the lock. The retired
qtimageformats checkout is preserved locally and ignored; it is not a module
input to this profile. Qt retains UI rendering, SVG, framework PNG/JPEG resources
and image encoding uses. Ordinary user WebP/JPEG/PNG decoding uses the shared
FFmpeg path. The product links Core, Gui, Widgets, Svg and UI task scheduling;
it does not link or package Qt Network, WebSockets, Sql or Multimedia. This does
not claim that qtbase itself stops building its Network or Sql modules. HTTP/TLS
use curl, WebSockets use WebSocket++, database access uses shared SQLite, and
media uses FFmpeg/FFplay plus native camera adapters.

Upstream source and its license texts are preserved in each module, including
`LICENSES/` and third-party notices. Qt is built as shared libraries. The pinned
Windows packet-text overlay in `patches/` keeps complete Unicode pairs in
the same WSI event queue as preceding BMP characters, prevents a retained packet
record from replaying a different character, and accepts literal LF text in
multiline controls without turning it into an Enter command. Normal IME handling
is unchanged. Its digest and exact file pre/postimage Git blobs are locked in
`sources.lock.json`. Bootstrap applies it once or verifies the already-applied
postimage, preserving unrelated work and rejecting mismatched preimages. Other
platforms do not apply this overlay.
The patch directory pins LF with `.gitattributes` so a clean Windows Git checkout
with `core.autocrlf=true` retains the exact digest-locked application bytes.

SDK fingerprints include applied patch digests plus tracked diff and untracked
source content digests. A dirty-source SDK cannot be reused as a clean-source
SDK after edits are reverted. Local tracked source edits still force incremental
Ninja; a different upstream commit requires updating the lock file. Validate the
overlay rules with `python3 MaiChat/desktop/tests/test_qt_source_patches.py`.

References:

- https://github.com/qt/qtbase
- https://www.qt.io/blog/qt-6.12-released
- https://doc.qt.io/qt-6/build-sources.html
- https://doc.qt.io/qt-6/configure-options.html
