# 原生设置页普通存储 C ABI（2026-11-05）

需求：iOS、Android 的“我的 / 存储管理”在没有聊天 Agent 时也能统计可管理目录、预览并清理旧缓存或临时文件。原生 UI 不应构造 `MaiTool.execute` 请求；必须与现有 `app_storage` 工具共用 `MaiAppStorage` 普通核心。

复现与预期：在 `/tmp` 下创建互不嵌套的 temporary、cache、workspace 三目录，加入超过 24 小时和近期文件。打开服务并预览不删除文件；显式预览 cache 后执行同一 token 只删其固定候选。预览后新增/改动的文件、模型、`gallery-*` 以及整个 Workspace 均保留。跨实例、被覆盖、已使用或关闭句柄的请求失败；Workspace 删除及未知协议字段始终拒绝。

源码基线为 `main / eee43402aff22caeb4b0c2688e381ced569f7e63` 的未提交工作区。本次独占新增 `MaiChat/MobileAgentAdapter/MaiAppStorageBridge.h/.cpp`、普通测试及运行脚本；`MobileAgentAdapter/CMakeLists.txt` 只新增 `MaiAppStorageBridge.cpp` 的移动静态库编译行。没有修改 `MaiAgent` 普通核心或工具 collector，也没有修改 Portrait、JNI 或原生 UI。

调用链为：

```text
Swift / JNI host
  -> maiMobileAppStorageCreate / Request / Destroy / Free
  -> MaiAppStorage::preview / cleanup
  -> MaiFileSystem

app_storage Tool（原有）
  -> MaiAppStorage::preview / cleanup（同一核心）
```

桥接 header 是 `MaiChat/MobileAgentAdapter/MaiAppStorageBridge.h`。其句柄独立于 Agent 生命周期，每实例拥有固定身份、三个固定根目录和核心预览状态。宿主先创建真实、绝对且互不嵌套的三个 App 自有目录；创建不扫描/创建目录，根目录安全检查仍由核心在每次操作执行。账号或 Workspace 改变时必须销毁并重建句柄。请求应在后台串行队列执行；销毁会撤销句柄、请求取消并等待正在进行的操作退出。取消不会回滚已经删除的文件。响应使用独立的 owned allocation，必须调用 `maiMobileAppStorageFree`。

薄 JSON 适配严格拒绝未知字段、重复字段和错误类型：

```json
{"action":"preview","target":"app"}
{"action":"preview","target":"temporary"}
{"action":"preview","target":"cache"}
{"action":"cleanup","preview_id":"token-from-same-handle"}
```

`target` 缺省为 `app`。`app` 汇总是只读的，不允许执行其 token；清理必须来自明确的 temporary/cache 预览。需要“全部安全清理”的宿主可持有两个独立句柄分别预览 temporary/cache，在展示精确候选合计并确认后逐个执行，各自仍只删除其固定集合。一个句柄内后续成功预览覆盖旧 token，不能在确认与执行之间刷新同句柄的汇总。

成功返回 `{ok:true,output:{...}}`；失败返回 `{ok:false,errorCode:"...",error:"..."}`。preview output 包括 `preview_id`、`summaries`、`candidate_bytes`、`candidate_files`、`limited`、`min_age_hours`、`cleanup_allowed` 和固定为 false 的 `workspace_cleanup_allowed`。每条 summary 包括 `target/bytes/files/eligible_bytes/eligible_files/excluded_recent_files/excluded_recent_bytes/excluded_protected_files/excluded_policy_files/truncated`。不返回候选绝对路径。cleanup output 为 `removed_files/reclaimed_bytes/skipped_files`。UI 应显示实际 `candidate_*`，不要把超过 1000 候选上限前的 `eligible_*` 当作本次可回收量，也不要把所列目录占用描述为安装包或完整 App 总占用。

保护边界：固定默认保留 24 小时；不开放 `scope/patterns/min_age_hours` 覆盖。`protectionComplete` 始终 false，完全不请求 Workspace 清理。原有核心维持十分钟、单次 token、根目录/会话绑定、高精度文件快照、普通文件及链接边界。适配层没有第二套文件候选算法；句柄关闭后标识不再复用，迟到请求不解引用已释放对象。

验证平台为 macOS 27.2 / arm64，普通 C++17 独立二进制；没有加载聊天 Agent、模型或用户 App。执行：

```sh
python3 MaiChat/MobileAgentAdapter/tests/run-app-storage-bridge-tests.py --output /private/tmp/maichat-app-storage-bridge-20261105
cc -x c -fsyntax-only MaiChat/MobileAgentAdapter/MaiAppStorageBridge.h
```

普通回归输出 `MaiAppStorageBridgeTests: PASS (0 failures)`。覆盖只读汇总、分目录清理、近期文件保留、ONNX/gallery 保护、Workspace 原片/中间文件全部保留、预览后改动/新增文件保护、1000 候选准确上限、跨句柄/覆盖/重放 token、严格 JSON、重叠/缺失目录和关闭句柄边界。C header 语法检查成功；直接将 header 当主文件检查只产生 `#pragma once in main file` 提示。核心过期路径 `MaiAppStorage.cpp` 的 `age < 0 || age > 600` 被原样复用，没有为测试等待十分钟或改时间源。

源码 hash、编译命令、运行时间与普通二进制 hash 保存于 `/private/tmp/maichat-app-storage-bridge-20261105/manifest.json`。此验证不等于 iOS/Android UI、JNI 或设备验收；两端由各自协作者继续接入。没有启动、停止、替换或安装用户 App，没有读取真实用户存储。
