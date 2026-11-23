# 工具组分类交付审计与独立主线增量

时间：2026-11-22 20:43（Asia/Shanghai）；Windows；本轮只准备可审查补丁，没有构建、安装、启动或替换 App。

用户现场：截图仍显示通用的 `工具调用 2 次`，需要核对此前承诺的内置/shell 分类是否进入实际交付。可复现操作是打开含连续工具调用的 Desktop 创作中心本地或远程会话，观察默认折叠标题，点击展开及折叠，再缩窄窗口。以 `ssh_exec` 和 `curl` 均完成的同组为例，预期为 `内置工具调用 2 次 · 已完成`；若同组再有一个失败的 `shell`，预期为 `内置工具调用 2 次 · shell 调用 1 次 · 有失败`。图标和箭头仍由现有控件添加。

本轮源码证据：开发工作区 HEAD 为 `38d25b9d7a41ad29128e95289f44937c676ca452`，分类修改存在于未提交 WIP。冻结的公开产品源码基线为 `f47b912c0966792f9b6c7227204a700806135e95`，从 `.cache/r311/src` 对应 Git 对象读取；该版本共享函数只接收 states，三个生产调用点也只传 states，因此输出通用摘要。不能据此宣称截图对应的安装包已升级，也不能把开发工作区测试通过等同于公开版本交付。

准备结果位于 `.cache/toolgroup-mainline-20261122/`：

- `production.patch`：4 个生产文件，7,058 bytes，SHA-256 `7f89cbe2bb24507701a4fc6db849c271d24a99e894b6d637191fe8dc6f333e12`。
- `tests.patch`：2 个测试文件，12,547 bytes，SHA-256 `5bc1483f906df9056e57c958bd8cdd2bcdf2ad70555bf52bbe817e40e3354e53`。
- `combined.patch`：上述合并，19,605 bytes，SHA-256 `9b61526c3373525246bd6410394927a34de150991a42ce9e16e83a018b5f24f4`；198 行增加、8 行删除。
- `manifest.json`：6 个文件实际 before/after SHA-256、Git blob ID、读取到的 Root WIP 文件 SHA-256及三个调用点位置。
- `before/`、`after/`：从冻结 Git 对象提取的原文及预期结果；均为 LF 字节。`verify/` 是独立应用补丁后的私有副本。
- `prepare_delta.py`：可复查提取逻辑；`verification.txt`：本次补丁检查结果。

生产范围严格为共享 `AgentToolGroupWidget.cpp/.h`、本地 `ToolGroupCard::setSummary` 和远程 `setTools`/`refreshDetails` 两个摘要调用。`AgentChatPanel.cpp` 在 `setSummary` 以外恢复后与 f47 原始 blob 字节一致，远程文件在目标摘要区域以外亦一致。因此本轮没有带入 QR、runtime 或其他 WIP，也没有改动 f47 已含的图片去重及其他产品行为。源码主工作区、公开基线工作区、Git index、main、发布目录和构建缓存均未改动；仅写本轮私有目录及本审计文件。

分类复用已有实现的精确名单：`shell`、`shell_input` 和 `desktop_process_start/read/write/stop` 为 shell；`ssh_exec`、`curl`、`openssl`、`computer`、`desktop_browser` 及其他名称为内置。不使用前缀推断。公开 f47 仍注册 desktop_process 工具，补丁完整保留它们的生产实现和注册，没有带入 Root WIP 的工具退役；`shell_input` 在此仅为标题分类可识别名称，不要求其在公开基线提供执行能力。共享函数保留默认空名称参数，未提供名称、缺名或数量不匹配时故意保留通用标题。运行中、等待授权、有失败、部分取消、未知、完成的原有优先级、展开状态和移动端文本均不变。

测试增量只提取与上述 UI 行为直接相关的已有回归：

- `RemoteToolDetailTest::groupSummaryCategories_data/groupSummaryCategories`：实际远程面板的纯内置、纯 shell、混合、失败、SSH 例外、shell_input 名称、旧进程名称、精确匹配、缺名、折叠切换及窄窗；`legacyGroupSummaryKeepsStatusPriority` 覆盖原优先级和旧签名。保留既有详情刷新用例的一条分类断言。
- `AgentPanelSessionTest::toolGroupCategoriesStayWithinTheirOwnGroup`：自有 SQLite 历史的三个由正文隔开的工具组，断言次数不跨组累计，且展开前后标题一致。

这些用例只使用合成记录/现有测试设施，不执行 shell 或网络，没有引入 shell 会话、工具注册变更、compaction、QR 测试或任何新依赖，不需要新增 CMake 目标。后续聚合者应在目标主线实际构建上至少执行 `remote_tool_detail_test groupSummaryCategories legacyGroupSummaryKeepsStatusPriority busyRefreshLocksCurrentStateBeforeTheNextSnapshot` 及 `agent_panel_session_test toolGroupCategoriesStayWithinTheirOwnGroup consecutiveToolsShareDisclosureWithoutChangingReceipts`，再完成正常 App 本地/远程操作及截图核对。

本轮验证：运行 `python .cache/toolgroup-mainline-20261122/prepare_delta.py` 成功。脚本使用仅对本次命令生效的 `git -c core.autocrlf=false -c core.eol=lf apply --no-index --check --whitespace=error --directory=.cache/toolgroup-mainline-20261122/verify .../combined.patch` 检查，并在同一私有副本实际应用补丁；6 个结果逐字节等于 `after/`，反向 `--reverse --check` 也成功。三个生产调用均实际携带相应工具名。曾发现 Git 从嵌套目录运行会报告 skipped、Windows autocrlf 会改变物理换行；已用显式私有目录前缀、命令级 LF 配置和结果字节核对消除此类假通过，未更改任何仓库配置。

验证边界：本次没有重编译或运行这些测试，也未操作任何安装版进程。已有开发 WIP 的测试及合成组件截图记录见 `2026-11-21-tool-group-categories.md`，它们不能代替本轮主线增量的目标平台验收。目前仅形成可审查、可应用的主线兼容增量；尚需主线聚合、目标构建回归、正式 App 本地/远程验证、源码提交与安装产物及运行进程核对，不能宣称已合并、已发布或用户安装版已更新。iOS/Android 未修改、未验证。


## Subsequent Windows and Mac validation

Windows Root normally compiled and linked maichat, remote_tool_detail_test and
agent_panel_session_test; the normal deployment/PE check passed. On the actual
Windows QPA backend, remote tests14/0 and local tests4/0 passed. Owned screenshots
of the mixed failed group and the three local groups were visually inspected:
category counts, status, disclosure and group boundaries are correct. Evidence:
.cache/checkpoint-tool-20261122/*category-results.txt,
category-runtime-safe.json and category-captures/. The first invocation exited0
with empty default Qt stdout; explicit -o file output was then used to record
actual totals. This Root build is38 plus preserved WIP; it does not establish
acceptance of the standalone public-main delta or an installed product update.

The Mac owner subsequently reports local3/0 and remote14/0 for the standalone
classification delta in its private f47+verified batches8/10 build. These are
remote-reported results; the final receipt/screenshots are still being prepared.
Do not equate these tests with the currently installed App.

Runtime identity update from that owner: PID438 is no longer present. Current
PID60328 runs ./MaiChat; /Applications/MaiChat.app is3.1.2 with executable SHA256
fd356045c18c0ff8d76f380e1cf64c782cbd7da555ca5300a5e7b0e93fd61634,
matching the published Mac3.1.2 manifest/source f47. This public build still lacks
the classification delta. The task did not install or stop the App; the change's
time/operator are unknown. The earlier3.1.1/af01/PID438 observation remains a
historical observation and must not be restated as the current runtime. No user
DB or desktop screen was read for this update. No classification commit,
publication or installation is claimed.


Final Mac evidence intake: archive238075B/SHA256
3f3b9ffad5ce3bd0ea376fd99d343bd20b46bfbef3e0fb86297474181f300185
was independently checked. All16 evidence-manifest entries match bytes/SHA;
the standalone safe JSON and original Root patch manifest are byte-identical
to their archived counterparts. Logs confirm the real old-UI local RED1,
local GREEN3/0/0.951s and remote GREEN14/0/1.479s. Root personally viewed the
archived local three-group and remote mixed-failure screenshots. The Mac owner
viewed seven selected images. Logs/receipts are retained under
.cache/toolgroup-mainline-20261122/mac-validation/ with intake-safe in
mac-intake-safe.json. The verification window is closed; no additional production
delta was required. This closes candidate validation, not mainline integration,
publication or installed-App delivery. Installed state remains the owner's
reported public3.1.2/f47/PID60328; that executable does not contain this delta.
