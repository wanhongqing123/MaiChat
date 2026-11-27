# Android 创作中心真实上下文用量

日期：2026-11-27（Asia/Shanghai）。源码基线：`be9711aeac49e2be0c1c2f37069cc910ff90fc90`，工作区已有大量未提交改动，本次没有提交或替换他人改动。

可复现操作和预期结果：打开创作中心，创建一个隔离会话，通过自有回环 Responses fixture 返回 `input_tokens=111`、`output_tokens=22`、`total_tokens=133`。任务完成后，输入框上方应显示最近主请求 `133 used / 实际型号窗口` 和 Core 算出的剩余百分比；点击后看到输入、缓存、输出、思考、消费和累计明细。第二轮和工具循环增加累计时，窗口占用仍取最近主请求。切换空会话不能显示上一会话的 133；重新打开原会话从同一权威事实恢复。未知窗口、缺失 usage、失效窗口/型号不能伪造百分比。

定位证据：Core 已有 `getModelContextUsage`、版本化 `encodeContextUsage` 和 `maiContextRemainingPercent`；原移动 `snapshot`/`remote_snapshot` 没有计量投影，Android State/Panel 没有对应控件。已有 `AIAssistantPcParityInstrumentedTest` 的回环 usage 仅验证 JSONL，原回执明确记录 `usageUiVerified=false`。缺口位于移动适配和 Android 展示链路，不是供应商计数解析。

实现：移动 snapshot 复用 Core codec 保留每项来源、窗口来源/版本、最近主请求及本轮/会话累计，增加绑定会话身份和 Core 计算的百分比。小窗口不传播 Core 零余量哨兵。JNI 原样传输 UTF-8 JSON；Java 不重做基线算法，不按消息长度估算 token。State 按会话/型号核对；Panel 在输入框上方提供状态条、占用条和可点击的明细。切页关闭旧明细，视图销毁时释放窗口。远端刷新缺失计量时清除旧值。

已执行验证：使用 Android Studio 自带 JBR `javac/java` 和已有 Gradle 缓存中的 JUnit 4.13.2、org.json 20240303，不运行 Gradle，不加入依赖。新测试先在缺少 presenter 时得到编译失败，再实现并得到 `OK (7 tests)`。覆盖缺失/实测零、unknown window、Derived、当前与累计分离、压缩后失效、切会话/型号、非法数字/来源。相关已跟踪文件 `git diff --check` 通过。

待集成运行的验证入口：`MaiMobileAgentTests --context-usage-only` 走真实 configure/send/持久化/重开链路；Android `AIAssistantPcParityInstrumentedTest` 增加真实 JNI 数值、可见标签、进度条、点击明细的断言，并将完整真实窗口截图保存为隔离目录中的 `context-usage-details.png`。原有 panel 内容图继续保留。测试只使用独占 Context/目录和回环合成凭据，不访问真实账号或付费模型。

验证边界：本子任务没有运行原生构建、Gradle、安装或 ADB，没有宣称设备验收通过；正常 App 的构建、安装、运行进程核对和 PKD130 实机截图审查由主代理的独占验证流程执行。当前 standalone Java 通过不能代替这些验收。
