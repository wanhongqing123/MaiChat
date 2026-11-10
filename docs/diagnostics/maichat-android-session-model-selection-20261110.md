# Android 会话模型与思考强度适配

## 输入与复现

- 时间：2026-11-10；平台：Windows，Android Gradle 7.5.1、AGP 7.4.2、SDK 34、JBR 17.0.14。
- 基线提交：`2c5a81b394f4b0b7dfc8f9698f1b4df1498bfa8a`。本轮 Android 适配仅 Java 与测试，没有 JNI/Core/MobileAgentAdapter、工程版本、CMake 或服务改动。
- 预期操作：创作中心右上角“更多”→“主模型”直接选 GLM 5.3、GLM 5.3 Flash、DeepSeek Flash、DeepSeek V4 Pro 或本地模型；“思考强度”的 low/high/max 仍是独立展开组。切换会话恢复各自选择，选择失败保留原值和草稿。
- 最小旧行为：给 State 同时提供会话 A 的 GLM Flash/max、会话 B 的 DeepSeek Pro/low 和账号默认 DeepSeek Flash。A/B 都错误显示账号默认；模型菜单只有云端/本地；Pro 被当作可接收图片；GLM 配置走 Responses。

## 原因与修正

- 原 Controller 把选择存成账号级 settings.json，每次修改重建 Agent；State 与发送队列读取全局 model。现在会话显示、附件门禁和队列从目标会话快照读取，派发前再用 get_session 核对原目标的配置。
- 菜单把可见 State 交给普通 Controller API，避免 Controller 已发布下一页、UI 尚未绘制时重读全局选中页。完成/失败只改原可见 owner 的错误状态，弹窗完成回调另外校验账号代际、完整 owner 与操作身份。
- 模型选择使用普通共享 JSON `set_model`；空白页显式选择使用 `create(model, reasoningEffort?)`。模型单独选择省略 effort，保留数据库中的 inherit；思考强度选择显式传值。两者不 configure、不写账号默认。
- 账号初始化固定 DeepSeek Flash，同时登记 localModelName 延迟槽。旧全局 llama 偏好不强加到会话，实际 local 会话在选中/恢复时才经现有校验器准备文件。prepare_local_model 仅可信本机宿主调用，按 nativeEpoch 缓存完成状态，避免每条输入重哈希大权重。
- GLM 的固定 Chat 与 DeepSeek 的固定 Responses 使用共享 Mobile 的路由；Java 只提供配置端点与现有账号 Key 回调。旧 test-model 仍只用于原来显式的自有 Chat 夹具，不计作四个正式型号的路由验收。
- Android owner 接收 get_session/set_model 时只接受模型/effort 等白名单字段，拒绝 endpoint、Key、文件路径和 prepare 操作。
- 独立复审补出的冷启动 local owner 边界：远端 prompt/steer 经过普通 `executeSessionRequest`，先 get_session 读取真正目标；仅目标为 local 时由可信宿主解析/校验固定权重并准备，再派发输入。云会话不扫描或准备其他 local 历史；viewer 始终不传路径。
- 独立复审补出的空白页交接边界：显式模型/effort 选择创建会话后，沿首次发送已有的 emptyDraftSession 标记交给原 Panel 迁移逻辑，文字和附件一起从空 owner 移到新 id；若用户已转到其他页，不接管该页。
- Android viewer 复用相同弹窗，只保留四个云模型与思考强度，并显示所属设备。进入会话后读取 get_session，set_model 经现有 command/owner slot+login+session 回执通道；HTTP 转发成功不提交界面值。仅匹配请求的完整执行 ACK 才更新账号私有内存缓存。旧读取、旧登录、错误 session/model/effort/wire、超时与失败不覆盖原选择；目录刷新与离线保留已确认值。未知模型显示“未读取模型”，不冒认手机默认；远端空 effort 显示“跟随默认”。viewer 不发送本地准备、路径、端点或凭据。
- 操作权限变更保留原账号默认的 configure/原子设置事务。它不把当前会话的模型/effort 写入默认；该既有操作仍会重建 Agent/推进 epoch，有 busy/回滚门禁，本轮没有扩大其语义。

## 实际验证

原日志位于 `.cache/session-models-20261110/`，不包含真实用户数据库、凭据或模型调用。

1. `android-red-gradle.log`：正常编译；15 例中 12 PASS、3 FAIL，分别是四模型菜单、Pro 图片限制、GLM wire。
2. `android-session-red-gradle.log`：两例 State 会话投影均真实 FAIL。
3. `android-final-java-gate.log`：本地适配阶段的生产/instrumented Java 正常编译，8 类 48 例、0 FAIL、0 ERROR，15s；原件固定在 `android-java-stage1/`，不替代远端增量结果。
4. `android-remote-red.log`：远端未读模型错误显示手机默认的原例真实 1 FAIL；`android-remote-green.log` 对应修正与 ACK/菜单初轮 18/18 PASS。
5. `android-frozen-java-gate.log` 是复审前阶段：生产 Java 与 instrumented Java 正常编译，8 类 55 例、0 FAIL、0 ERROR，14s。该阶段原源/patch/manifest 与 JNI 原 so 固定在 `android-pre-p2/`，不当作后补源码的验收。
6. `git diff --check -- MaiChat/android`：通过；Git 的 LF→CRLF 提示原样保留，未改变全局换行配置。
7. `adb devices -l` 为空；没有 Android 手机或模拟器连接。新增本地/远端真实 View 的 instrumented 菜单用例只编译，没有执行；远端 View 素材是显式自有投影夹具，不冒充 WS 执行验证。
8. 共享 Core/Mobile 完整输入到件后，沿原 `app/.cxx/mobile-agent-adapter/RelWithDebInfo/3k666l63/arm64-v8a` cache 正常构建 `maichat_agent`：**140 项增量、exit 0**，包括 `maichat_mobile` 静态库与最终 JNI 链接。NDK `27.3.13750724`、SDK CMake `3.22.1`、原 RelWithDebInfo/GRAPHICS ON/test OFF 保持；没有另配裁剪 flags，也没有 assemble APK。
9. 首轮 JNI `libmaichat_agent.so` 为 323749328 B，SHA256 `4d70eeb3d6b557db1558e4bb2924e891b18a9fe256e0d3cc87d63b59ffa0842e`。NDK llvm-readelf 确认 AArch64；llvm-nm 在该 so 读回 Routed/Catalog/ProviderWeb 与 JNI nativeRequest 定义。原日志 `android-native-arm64-build.log` 与 `android-native-arm64-receipt.json` 保全；原 curl deprecated、llama 和新 transport unused 常量警告保留，Android 适配未改这些共享源。
10. 两项复审 P2 的 `android-p2-red.log`：普通宿主转发边界只抽出原直接 invoke、尚无修复时，真实 5 例中 4 FAIL（enqueue/steer 冷 local 未准备、实际 Panel 文字清空、实际附件 owner 未迁移）；cloud 不准备 local 例 PASS。原源、helper bytecode、XML 与日志保存在 `android-p2-red/`。修正后原断言全部通过；加异页保护后 `android-p2-green.log` 为 6/6 PASS。
11. 最新 `android-final-p2-java-gate.log`：生产/instrumented Java 编译 exit 0，**9 类 61 例、0 FAIL、0 ERROR，14s**。保留原队列、回答/补充、回滚全部断言。新增 BoundaryTest 使用真实 Panel/Composer/附件容器与自有 State；仅最终 JNI 调用、固定权重准备为普通宿主边界夹具，不声称执行了 Android JNI 或加载本地权重。
12. 共享 Compactor effort 修正到件后，原 arm64 normal cache 仅 **3 步**（Compactor.o→maiagent.a→JNI.so）增量、exit 0。Compactor Git blob 精确 `6aacd74da2453936fa3185a48788f5aeb29068e1`。该阶段实际 JNI 为 323751336 B，SHA256 `d7a75c230c5e7fe11710c84f19b83f0af30dd16222edb7ab9de3c786e15d1e2e`；NDK nm 的四标记与 readelf AArch64 均通过。日志 `android-native-arm64-final-link.log`、回执 `android-native-arm64-final-receipt.json` 与首轮分开保留，原 so 已另存 `android-final-p2/libmaichat_agent.so`。
13. 最后共享 Chat 代理修正的 `MaiOpenAiClient.cpp` 到件，Git blob 精确 `208d1889f42eee6c6cacac704d9ba027ea08d6f7`、raw SHA256 `f83cadb6afaf43c2d3e9289d6f1eebe6bfc0036208c714596d51599b1de4fb48`。原 cache 仅 **3 步**（该 cpp→maiagent.a→JNI.so），exit 0，未变更 header/ABI/CMake/SDK/flags。最新实际 JNI 为 **323823000 B**，SHA256 `2abc9bff139e67ca0399e856a5b7e9708f682e65b9f6f148ffb6021ca7b62f68`；实际 so 再核四个符号及 AArch64 通过。13 Java 源 raw SHA 全部仍等 61/61 冻结表，未重复 Java 测试。原日志 `android-native-arm64-chat-proxy-link.log`（PowerShell UTF-16 原字节）与 `android-native-arm64-chat-proxy-receipt.json` 单列；这次是 Android 编译/链接验证，代理运行回归由其他平台原例证明，不能扩大为 Android 设备代理验收。

## 验证边界

本机没有构建/安装新 APK，没有启动真实账号、用户 DB、付费模型或设备输入。Java 单测与正常 arm64 JNI 编译/链接均已验证；这些不代表 Android 上四型号两个协议的运行收发、SQLite v10 执行迁移或 Android 视觉体验已验收。没有将别的平台结果扩大为 Android 设备通过。
