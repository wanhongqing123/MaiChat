# Desktop IM

Qt 6.12 Widgets based IM client for Windows and macOS.

## Build

```bash
cmake -S MaiChat/desktop -B MaiChat/desktop/build
cmake --build MaiChat/desktop/build
ctest --test-dir MaiChat/desktop/build --output-on-failure
```

Qt is built from the pinned shallow source submodules under `qt/`; no system Qt
SDK is selected. Initialize them once:

```bash
git submodule update --init --depth 1 -- MaiChat/desktop/qt
```

The first CMake configure builds the required shared Qt modules, without upstream
examples/tests, into the fixed `qt/.build/<platform>-<architecture>` cache.
Subsequent configures reuse the same SDK. CMake 3.25+, Python 3.10+, Ninja,
macOS 14.4+ / MSVC 2022 are required; see [Qt source provenance](qt/UPSTREAM.md).

## Run

```bash
MaiChat/desktop/build/maichat
```

## Local agent history and skills

The persistent agent backend keeps authoritative append-only JSONL under the account's
`Agent/Accounts/<owner SHA256>/sessions/YYYY/MM/DD/` directory inside Qt AppLocalDataLocation.
`agent.db` contains indexes and byte references. Keep the entire `sessions/` directory,
including `.journal/`, when backing up history. Legacy databases are migrated read-only;
the original database is retained. Startup prepares storage in a worker with cancellable
progress and fails visibly instead of falling back to an empty account store.

Skills are discovered under the user and project `.agents/skills/<name>/SKILL.md`
directories. Metadata is advertised first; selected contents are loaded on demand and
remain stable for the corresponding user input across follow-ups and restarts.
`skill_install` discovers public GitHub skill folders and README links, then installs a selected folder
at a pinned commit. The default destination is the host user's `.agents/skills`; workspace scope is
explicit. Existing skills are never overwritten, and downloaded scripts/dependencies are not executed.
The next input in the same conversation discovers new skills. Unchanged metadata is not repeated in
every message; historical instructions and selected bodies remain immutable.
Windows command launchers can exit while their Playwright/browser service continues in its owned Job.
Cancellation, explicit deadlines and owner cleanup still reap those background processes.

The desktop account runtime starts new sessions with a small tool catalog. Other
capabilities remain available through bounded `tool_search` discovery and selection;
new schemas take effect on the next model request. Existing session prefixes remain
stable. The usage header shows the last sampling's provider total, not a local input
estimate or accumulated session cost. Missing, derived, and obsolete measurements
remain distinguishable. Remote desktop sessions fetch the same typed measurement
through the existing owner command replies; no service deployment is required.

Desktop no longer registers standalone `file_*` model tools: use `exec_command` for reading/search
and `apply_patch` for text changes. Shared native file algorithms remain available to the mobile
in-process command host; mobile also exposes no standalone `file_*` tools to the model.
Tool catalog v5 preserves older records while retiring desktop file declarations from new snapshots.

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

Windows release validation must also cover the post-login window, rather than
only `maichat --smoke` (which cancels the login dialog). From an MSVC developer
environment, build `login_dialog_test` and run `approvedQrShowsMainWindow` through
`scripts/run-tests-windows.ps1 -BuildDirectory <build> -TestRegex '^login_dialog_test$'
-TestFunction approvedQrShowsMainWindow`, with an isolated test user profile.
This uses owned local HTTP/IM fixtures and creates the actual main-window widgets.
Run the registered `msvc_include_dependencies_test` as well: it checks that a
transitive header-only change rebuilds both C and C++ consumers. The MSVC/Ninja
compiler launcher normalizes localized include markers as bytes, so console and
ANSI code-page differences cannot silently discard header dependencies.

The bundled SDK files come from the domestic Tencent download host `im.sdk.qcloud.com`; see `vendor/tencent-im/README.md`.

## Remote desktop

Windows and macOS builds link the bundled Tencent TRTC SDK. Both platforms can view and
share and control a desktop. The same remote-input protocol supports Windows-to-Windows,
Windows-to-macOS, macOS-to-Windows, and macOS-to-macOS sessions.

The first screen-sharing attempt on macOS triggers the system Screen Recording permission.
Accepting remote mouse or keyboard input also requires Accessibility permission. Grant both
permissions to MaiChat in System Settings when prompted.

## Desktop Agent tools

Commands use the shared Core `exec_command` / `write_stdin` implementation. It provides
bounded output, numeric session IDs and optional native PTY interaction between Agent turns.
Only the owning Session can continue its process; the selected approval policy still applies.
See the Core README for output retention and recovery.

The client-executed `computer` function exposes the 13 Sky window2 methods through
`{method,parameters}`: list_windows, get_window, list_apps, launch_app, get_window_state, click,
press_key, type_text, scroll, set_value, drag, perform_secondary_action and activate_window.
Select a returned `{app,id,title?}` window. `get_window_state` defaults to
`include_screenshot=true, include_text=false`; text-only state reads do not request or return an
image. Native focus, selection, document text and supported element actions remain conditional
provider facts. Coordinates are window-relative logical pixels; image pixel dimensions are
reported separately for DPI conversion. Element indices and screenshot IDs belong to the current
observation. No separate screenshot tool is needed for a window observation.
The ordinary C++ API and model adapter share one implementation. Ordinary screenshot URLs are
data URLs; model output uses image attachments plus `image_attachment_index` instead of duplicating
Base64 in tool text. This function does not enable Responses built-in `computer_call`.
`computer_input` retains the existing advanced display/raw/held/touch/pen operations through the
same controller. Window operations use the `native_input_binding` from the current Sky observation;
display observation is an explicit advanced operation. It does not offer duplicate basic Sky actions.
Approval follows the selected Agent mode. An input or launch receipt is not proof that the app task
finished: observe the actual result before reporting success.
The independent `screenshot` tool and computer observations currently share a resident native
capture backend. Retaining that backend does not stream every captured frame into model requests;
stopping it affects the process, not just one conversation. Password fields, locked screens and
invisible windows are excluded. On macOS, grant Screen & System Audio Recording and Accessibility
to the running MaiChat App. macOS window-image/native-frame alignment remains unverified; the
returned mapping flag stays false and screenshot-derived coordinate input is refused, while native
element operations remain available where the provider supports them.

Web search and fetch remain provider capabilities; ordinary application links still open normally.
HTTP requests use curl and native system proxy lookup. Qt supplies UI only.
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
