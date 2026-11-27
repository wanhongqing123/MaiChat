# GitHub Skill 安装与 Playwright 实际宿主验收

日期：2026-11-26；Windows x64。源码基线 `be9711aeac49e2be0c1c2f37069cc910ff90fc90`，
包含现有工作区修改；不代表这些修改已提交或发布。

## 需求、操作与预期

用户把 `https://github.com/microsoft/playwright` 发给 MaiChat，要求安装浏览器 Skill。
预期模型能检查仓库及 README，定位真正的 Skill，安装完整文件，在同一会话的下一条输入中
发现和读取它，并按正文执行网页任务。不能把框架安装、Skill 安装和浏览器依赖安装混为一件事。

本次使用真实 `AgentController` 和宿主默认工具注册。测试注入可控模型回复，实际执行
`skill_install`、`skill_read`、`exec_command` 以及浏览器；不是直接调用安装器后伪造工具结果。
没有调用真实模型供应商，因此不能将结果表述为某个模型的自主规划或工具选择验收。

## 公开来源与版本

- 框架 README 的官方链接指向 [microsoft/playwright-cli](https://github.com/microsoft/playwright-cli)。
  实测发现框架还有多个维护用 Skill，不能凭仓库名称选取。
- 使用 `skills/playwright-cli`，固定提交 `dc5f4d5616ab5a28ded12fd7101a4d49bd59a820`。
  原始 11 文件、71,541 字节逐字保留；安装器额外写一份来源清单。
- 验收运行依赖仅在 `.cache/playwright-skill-20261126`：`@playwright/cli 0.1.25`、
  Playwright `1.66.0-alpha-2026-11-18`、Chrome Headless Shell `159.0.8154.6`（revision 1255），
  使用现有 Node `24.14.0`。没有全局 npm 安装，也没有修改用户实际 Skill 目录。
- 原始 Skill 通过 Codex 官方 skill-installer 下载到隔离目录作预装对照；URL 场景完全不预拷贝 Skill，
  由 MaiChat 工具实际发现并下载。

## 实现与边界

普通 C++ `MaiSkillInstaller` 和薄工具入口复用现有元数据解析器、curl 与原生文件系统，
不要求 Python、Node、Git、MCP 或新的独立服务来安装 Skill 文件。

发现只返回候选目录、固定提交和 README GitHub 链接；由模型判断语义关联，没有硬编码
Playwright 仓库映射。默认用户 `.agents/skills`，明确工作区范围才写工作区 `.agents/skills`；
路径来自宿主，不接受任意目标目录。当前仅支持公开 HTTPS GitHub 来源。

下载有文件数和字节上限，拒绝链接、子模块、路径穿越及大小写冲突；在活动 Skill 目录之外独占建暂存目录，
完成验证后同文件系统原子无覆盖发布。已有目录保持原样。失败仅清理自己创建的暂存目录，
不运行下载脚本，也不把 frontmatter 的 allowed-tools 当作权限授予。

新用户输入边界刷新发现结果，同一输入中的工具轮次保持快照不变。每条输入保存目录摘要，
与前一有效输入比较；目录不变时不会反复注入目录更新。删除、恢复和重启续聊有独立回归。
原始 baseInstructions、历史输入上下文和已选正文不重写。

## 真实发现的 Shell 根因与修复

修复前，用户级和工作区级 Skill 均正常读取，CLI `open` 返回 0，下一独立命令的
`snapshot` 却报告浏览器未打开。Windows worker 在根命令正常结束时无条件终止 Job，
误杀了浏览器后台服务。这与 Skill 解析无关。

现将前台完成与资源彻底回收分开：前台及时返回原始退出码，后台继续由原 owner 持有；
管道与 ConPTY 均覆盖。取消、stopOwner、析构、显式硬期限、12/24 容量及有界输出契约保留。
对照 Codex 固定源码与当前工具环境的独立命令实测，见
[后台生命周期核对](2026-11-26-codex-shell-background-reference.md)。
当前 Codex 的独立 `open`、`snapshot`、`close` 成功；没有把另一次 Chrome/Edge 配置报错
混同为 MaiChat 的后台误杀故障。

## 实际验收证据

证据根目录 `.cache/playwright-skill-20261126`。正常构建使用 `.cache/r311`，没有替换运行中的旧体验版。

| 场景 | 实际结果 |
| --- | --- |
| `host-url-install` | 34.966 秒，exit 0；从原始框架链接发现官方 CLI、安装固定目录，再在同一会话下一输入读取完整正文并操作网页；14 次模型请求、1 次独立网页 POST、10,367 字节 PNG |
| `host-preinstalled-final` 工作区隐式 | 10 次模型请求、1 次 POST、11,179 字节 PNG、failures 0 |
| `host-preinstalled-final` 用户显式 | 10 次模型请求、1 次 POST、10,850 字节 PNG、failures 0；两场景合计 18.576 秒，exit 0 |
| 正常 Core 回归 | `normal-regressions/results.json`，11 个目标全 0，包含 Skill、安装、原子发布、Shell、PTY、输入队列、目录和 rollout |
| 补充持久化/发现 | `normal-extra/results.json`，JournalRuntime 与 ToolDiscoveryRuntime 全 0 |
| Console E2E | `normal-console/receipt.json`，exit 0，10.771 秒 |

实际宿主测试程序 SHA-256：`e51b8389f88c0e54f88aed30c602f511584c6b1efbf0c651b179aa9cbaf5bd1c`。
截图是私有 localhost 自有页面，核对输入、动态控件引用、点击结果、DOM/title、POST 和 PNG，
随后关闭自己的命名浏览器会话。没有使用用户浏览器配置、系统剪贴板、真实账号或用户数据库。

Windows 正常 App 构建和 staging 已完成，运行库检查为 3 个二进制、116 个 PE 导入；
最终 4 个 Qt 目标全 0，Panel 138 通过、0 失败；Controller 76 通过。
staging、实际 ZIP 解压、Downloads 交付目录分别用裸 PATH 执行 `--smoke`，三份均 exit 0。
193 个构建输入与工作区、正常构建副本的 SHA-256 全等，见 `final-normal-source-manifest.json`。
macOS 未编译/运行；Android 仅验证新增 POSIX 文件系统组件的 NDK 语法，不宣称跨平台实机通过。

Qt 首轮有一处预览断言失败（137 通过、1 失败），原始结果完整保留在 `qt-regression`。
只读定位使用同一 exe：默认离屏 800×800 屏幕下，960×720 图片按屏幕限制解码，
原尺寸断言不成立；1200×900 屏幕下 live、历史恢复、键盘打开和缺图路径全部通过。
两者中心像素均为预期颜色；没有改宽产品解码限制或删除断言。最终整组回归使用显式
1200×900 离屏屏幕，记录在 `qt-panel-screen`；A/B 证据在 `preview-readonly-probe-relative`。

## 本地交付

- 启动：`C:/Users/18034/Downloads/MaiChat-local-20261126-skills-playwright/maichat.exe`。
- ZIP：`C:/Users/18034/Downloads/MaiChat-Windows-local-20261126-skills-playwright.zip`，264,587,108 字节。
- ZIP SHA-256：`57c3487f8410adbc20b7e1c4f9840c0e21ef9ef44034d64b4c1a42708dd2da10`。
- 三份 EXE SHA-256：`f581129de7b6fb67fea4ffa041adb0f3ae6fe64c85e411b24fa954a375aa4524`。
- 机器回执：`.cache/playwright-skill-20261126/local-handoff.json`。

未提交、推送或发布。旧包保留，PID 67168 仍运行原 `Downloads/MaiChat-local-20261126/maichat.exe`；
本次未停止它。体验新版需先自行退出旧版，避免单实例机制激活旧进程。
