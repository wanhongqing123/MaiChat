# Desktop IM

Qt5 Widgets based IM client for Windows and macOS.

## Build

```bash
cmake -S MaiChat/desktop -B MaiChat/desktop/build -DCMAKE_PREFIX_PATH=/path/to/qt5
cmake --build MaiChat/desktop/build
ctest --test-dir MaiChat/desktop/build --output-on-failure
```

On local Homebrew macOS setups:

```bash
cmake -S MaiChat/desktop -B MaiChat/desktop/build -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt@5
cmake --build MaiChat/desktop/build
```

## Run

```bash
MaiChat/desktop/build/maichat
```

## Native IM SDK

The desktop app is an independent Qt client. It does not depend on the Electron app or any local bridge process.

At runtime it loads the bundled native desktop IM SDK by default:

- macOS: `vendor/tencent-im/macos/ImSDKForMac_Plus.framework`
- Windows: `vendor/tencent-im/windows/shared_lib/Win64/ImSDK.dll`
- Windows 32-bit fallback: `vendor/tencent-im/windows/shared_lib/Win32/ImSDK.dll`

You can override the library path when debugging:

```bash
export MAICHAT_SDK_LIBRARY=/path/to/native/im/sdk/library
MaiChat/desktop/build/maichat
```

Sign in with a MaiChat account password or approve the desktop QR code on a signed-in
phone. The account service issues short-lived IM credentials to the desktop client;
the SDK AppID and UserSig come from that authenticated session.

Set `MAICHAT_USE_FAKE_CLIENT=ON` only when developing the UI without a native SDK.

The bundled SDK files come from the domestic Tencent download host `im.sdk.qcloud.com`; see `vendor/tencent-im/README.md`.

## Remote desktop

Windows and macOS builds link the bundled Tencent TRTC SDK. Both platforms can view and
share and control a desktop. The same remote-input protocol supports Windows-to-Windows,
Windows-to-macOS, macOS-to-Windows, and macOS-to-macOS sessions.

The first screen-sharing attempt on macOS triggers the system Screen Recording permission.
Accepting remote mouse or keyboard input also requires Accessibility permission. Grant both
permissions to MaiChat in System Settings when prompted.

## Desktop Agent tools

The Desktop Agent exposes three additional capabilities through ordinary C++ entry points
and model tool calls:

- `desktop_process_start/read/write/stop` keeps a command running between Agent turns. Its
  output uses a byte cursor, is returned in bounded chunks, and retains at most 1 MiB per
  process. Only the owning Session can read, write or stop it. Starting, writing and stopping
  use the selected Agent approval mode. Commands run with the App user's OS permissions. Use the existing
  `shell` tool for short, single commands.
- `desktop_browser` opens a separate visible Chrome or Edge profile. It reads bounded page
  text and elements, captures a PNG, and acts only on an observed element with the same URL,
  snapshot and label. It does not use the user's normal browser profile or execute
  model-supplied JavaScript. Opening, screenshots, clicking and typing follow the Agent approval mode.
  After an action, observe again to confirm the outcome. A new `open` starts a clean profile;
  two Agent browser Sessions may be open at once. Chrome requires a separate profile for
  remote debugging ([source](https://developer.chrome.com/blog/remote-debugging-port)).
- `desktop_computer` selects one visible window by exact title, returns a screenshot and
  bounded Accessibility or UI Automation elements, then accepts a click, text, supported key
  or scroll for that same window and observation token. Approval follows the selected Agent mode.
  The result of an action only means input was sent; observe again to verify the
  screen. Password fields, locked screens and invisible windows are excluded. On macOS,
  grant Screen & System Audio Recording and Accessibility to the running MaiChat App.

The browser uses Chrome or Edge; installing Qt WebSockets is required when building the
Desktop App. Windows packages must include `Qt5WebSockets.dll`.
Full Access skips prompts for these general tools; billable provider submissions still
require confirmation.

## Windows 免安装绿色包

无需在目标机器安装 Qt 或 VC++ 运行库（要求 Windows 10 及以上 64 位）。先完成 Release 构建，然后：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package-windows.ps1
```

产出 `dist\MaiChat-win64\`（解压即用目录）和 `dist\MaiChat-win64-<日期>-<git短哈希>.zip`。脚本会：

- `windeployqt` 旁挂 Qt 运行时（platforms/styles/imageformats 等）；
- 从本机 VS Redist 显式拷贝最高版本的 VC++ CRT（`msvcp140.dll`、`vcruntime140*.dll` 等，app-local 部署）；
- 按 `vendor/tencent-im/windows/shared_lib/Win64/` 相对路径附带 ImSDK.dll（exe 按此路径探测）；
- 生成 `使用说明.txt` 并压缩打包。

Windows 重链 `maichat` 目标会先用 `windeployqt` 给 `build/bin` 旁挂 Qt
运行时，再用 `dumpbin` 核对 PE 直接依赖与 Qt 插件；检查失败时构建会失败。
**构建退出码为 0 之前不要启动 `build/bin/maichat.exe`**，否则加载器可能先弹
缺 DLL 的系统错误窗，程序日志还没机会记录。交付验收要从完整 staging 和
实际 ZIP 解压目录分别运行 `--smoke`，使用只含 `C:\Windows\System32` 的
裸 PATH，并核对退出码为 0、没有系统错误窗。

产出的 zip 不提交进仓库（`dist/` 已被 gitignore），通过 GitHub Releases 发布下载：
<https://github.com/wanhongqing123/MaiChat/releases>。发布时显式指定
`--repo wanhongqing123/MaiChat`，确保 tag 指向已经推送的产品源码提交。

## macOS 免安装包

先完成 Release 构建：

```bash
cmake -S MaiChat/desktop -B MaiChat/desktop/build \
  -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt@5 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build MaiChat/desktop/build --target maichat --config Release
```

DMG 的 Finder 布局通过纯 Python `ds_store` 写入，并在写入后重新读取核对。
在打包机准备 Python 3.10+ 的独立环境（依赖只用于打包，不随 App 分发）：

```bash
python3 -m venv /private/tmp/maichat-dmg-tools
/private/tmp/maichat-dmg-tools/bin/python -m pip install \
  -r MaiChat/desktop/scripts/requirements-dmg.txt

MaiChat/desktop/scripts/package-macos.sh \
  --python /private/tmp/maichat-dmg-tools/bin/python
```

`--build-dir` 和 `--out-dir` 可传绝对路径，或相对于 `MaiChat/desktop` 的路径。
脚本不重新编译 App；默认读取 `build/bin/MaiChat.app`。`--app-only` 只输出旁挂依赖
后的 App，不要求安装 Python 打包依赖。缺依赖时，DMG 打包会在修改 staging 前退出
并提示安装命令。

产出 `dist/MaiChat-macos-arm64/` 和
`dist/MaiChat-macos-arm64-v<版本>-<日期>-<git短哈希>.dmg`。DMG 内包含应用和
`Applications` 快捷入口，脚本会：

- 使用 `macdeployqt` 旁挂 Qt framework；
- 把原生 IM SDK framework 放进应用内置 vendor 目录；
- 把 TRTC、TXFFmpeg、TXSoundTouch framework 放进 `Contents/Frameworks`；
- 用系统 `hdiutil` 生成 DMG，写入固定 Finder 安装窗口和 `Applications` 拖拽入口；
- 校验 `CFBundleVersion` 与 `CFBundleShortVersionString`，确保 Launchpad 可索引；
- 对 `.app` 做 ad-hoc codesign，避免嵌入 framework 因未签名无法加载；
- 压缩生成可分发的 DMG。

当前脚本生成 ad-hoc 签名，不包含 Developer ID 签名或 Apple 公证。发布说明需明确
这一点；`codesign --verify --deep --strict` 通过不代表 Gatekeeper 会接受该包。

发布前，从最终 DMG 挂载或解压出的 App 核对版本、签名、依赖与 `--smoke`，确认
与已验收的构建一致；校验清单只包含分发产物及摘要，不包含本机账号、配置、日志或
开发签名材料。DMG 和 SHA256SUMS 作为 GitHub Release 附件，不提交进源码仓库。
使用已登录的 GitHub CLI，并明确指定仓库，例如：

```bash
release_tag="v2.2.34"
release_commit="FULL_PUSHED_SOURCE_COMMIT"
release_dmg="/absolute/path/to/MaiChat-macos-arm64-v2.2.34-YYYYMMDD-COMMIT.dmg"
release_checksums="/absolute/path/to/SHA256SUMS"
release_notes="/absolute/path/to/release-notes.md"
gh release create "$release_tag" "$release_dmg" "$release_checksums" \
  --repo wanhongqing123/MaiChat --target "$release_commit" \
  --title "MaiChat $release_tag" --notes-file "$release_notes"
```

替换示例路径和提交后执行；不要将 `multi-ai-code` 等其他仓库作为发布目标。
