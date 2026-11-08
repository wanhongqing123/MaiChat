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
not download source or install Qt system-wide. Build/install files stay under
`qt/.build/<platform>-<architecture>` and are reused by subsequent invocations.
Examples, upstream tests and precompiled headers are disabled. One shared Release
SDK is retained; no complete App or model copies are made by this bootstrap.

Required tools: CMake 3.25+, Ninja, Python 3.10+, a C++17 compiler; MSVC 2022 for
Windows. The pinned qtbase declares macOS 14.4 as its minimum supported version.
This dependency profile is for the Windows and macOS Widgets desktop product;
the iOS/Android native products do not consume this Qt SDK.

Modules: qtbase, qtsvg and qtimageformats. The product links Core, Gui, Widgets,
Svg and UI task scheduling; it does not link or package Qt Network, WebSockets,
Sql or Multimedia. HTTP/TLS use curl, WebSockets use WebSocket++, database access
uses the shared SQLite, and media uses FFmpeg/FFplay plus native camera adapters.

Upstream source and its license texts are preserved in each module, including
`LICENSES/` and third-party notices. Qt is built as shared libraries. No upstream
source patches are applied. Local tracked source edits cause the incremental
bootstrap to rebuild, while a different commit requires updating the lock file.

References:

- https://github.com/qt/qtbase
- https://www.qt.io/blog/qt-6.12-released
- https://doc.qt.io/qt-6/build-sources.html
- https://doc.qt.io/qt-6/configure-options.html
