# 删除 desktop_browser 专用能力

## 用户要求与操作预期

用户明确要求删除 `desktop_browser`，即隔离 Chrome/Edge 页面的打开、观察、截图、点击和输入专用能力。
模型工具目录、欢迎入口和可执行注册中均不再出现此工具，不保留改名后的同类入口。
网页搜索/抓取、通用电脑操作及 App 普通链接打开是独立功能，本次不删除。

2026-11-26，源码基线 `be9711ae` 加已有未提交修改；本轮保留既有 OpenAI、Responses、上下文和联合存储 WIP。
改前限定备份在 `.cache/remove-desktop-browser-20261126/before/`，清单 `before-manifest.json`。

## 删除范围

- 删除 `DesktopBrowserTools.h/.cpp`，从 AgentController 去掉 include 和注册。
- 删除固定的“浏览器操作”欢迎入口及过时说明，更新当前 README 和 Unreleased 记录。
- 调用方核对确认 `MaiCdpTransport` 仅供该工具使用，因此同时删除其头、实现、专用测试及 CMake 静态库目标。
- 删除 `DesktopBrowserToolsTest` 专用文件和目标，清理 HTTP fixture 列表中的旧目标。
- 保留 SessionSync、WebSocket++/Asio、curl、系统代理、电脑操作和普通链接路径。
- 不修改历史聊天或历史审查证据。现存历史工具名称展示用例不代表工具仍可执行。

六个专用源/测试文件共删除 1928 行；当前产品源及构建配置无 DesktopBrowser/CDP 注册或依赖残留。

## 验证

操作为打开实际生产 AgentController/欢迎面板，检查工具注册与模型可见规格。

- 在旧实现上新增“不可查找/不在工具清单”的断言，两个实际场景均失败，明确失败项为 `desktop_browser`。
- 删除后同一用例及面板回归通过：6 passed / 0 failed。
- StarterCatalog 142 checks / 0 failures；验证网页检索和电脑操作独立入口仍存在。
- RemoteToolDetail 31 passed / 0 failed，Controller 定点 3 passed / 0 failed。
- 测试首次额外执行时用了不存在的 selector，保留失败回执；更正为源码中真实存在的定点后通过，未改断言。
- 正常 `maichat`、Controller、Panel、Starter、详情构建/链接通过；PE/Qt 检查通过，3 个二进制、114 个导入。
- 新生成依赖图无已删除的源、库、测试目标；App 中 `desktop_browser` 的 ASCII 和 UTF-16 字符串计数均为 0（辅助观测，不代替注册测试）。
- 独立只读审查没有具体 P1/P2。
- 正常打包 staging、实际 ZIP 解压目录及交付目录分别裸 PATH smoke，均退出 0。

证据：`.cache/remove-desktop-browser-20261126/` 下的 `red/`、`final-green/`、`normal-build.log`、`package-validation.json` 和 `local-handoff.json`。

## 本地交付

新目录 `C:\Users\18034\Downloads\MaiChat-local-20261126-no-browser\`，已解压，可手动启动 `maichat.exe`。
App SHA256：`88b80dd9779e441cfb10ca4b804dceec40133c72dd462074ea97e4e8795b6beb`。
ZIP 261301766 bytes，SHA256：`8a95925e8bce3ab499a2f5576e5b0ede4e4f39671e7f188b03811708aee2840b`。
旧体验目录与运行进程 PID 67168（2026-11-26 00:04:19 启动）未停止、覆盖或替换。
用户要体验删除后的版本，应先自行退出旧进程，再从新目录启动；本轮未自动安装、提交或发布。

此包包含已验证的 OpenAI 客户端入口，但后台 OpenAI 凭据尚未部署；不是 Key 已上线的证明。
**正式会话存储仍为 SQLite agent.db，JSONL 正式接入、迁移及恢复没有完成，也没有在此包启用。**
目前路径由 `AppLocalDataLocation/Agent/Accounts/<账号 SHA256>/agent.db` 生成；Windows 通常位于 `%LOCALAPPDATA%\MaiChat\Desktop IM\Agent\Accounts\...`。
未来 JSONL 计划在相同账号根目录内使用 `sessions/YYYY/MM/DD/rollout-<时间>-<UUIDv7>.jsonl`；这是目标布局，不是当前已经生成的文件。
