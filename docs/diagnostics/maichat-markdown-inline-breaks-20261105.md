# Markdown 表格中的 HTML 换行

2026-11-05，macOS arm64；源码基点 `main / eee43402`，保留现有全部未提交修改。

## 复现与预期

在 IM 和创作中心显示下列消息：

```markdown
| 用例 | 预期 / 实际 |
| --- | --- |
| **失败用例**：<br>批量缩略图<br><br>失败信息 | **预期**：<br/>320×180<br />**实际**：<BR>320×240 |
```

预期：换行留在各自单元格内，双换行保留空行，粗体继续生效；行内代码、代码块、
转义的 `\<br>` 和 `&lt;br&gt;` 保持字面量。历史消息正文及复制 Markdown 原文不改写。
普通段落、列表和引用采用同样的行内呈现规则。

## 定位证据

- 用户截图明确显示表格单元格直接绘制 `<br>`。
- Desktop 的 md4c 使用 `MD_FLAG_NOHTML`，禁止行内 HTML 语义；`MD_TEXT_HTML` 也仅追加原文。
- iOS Foundation 正确标记 `.inlineHTML`，现有显示层没有消费该属性，直接绘制标签。
  本机原生解析探针确认 `a<br>b` 的标签 run 带 `.inlineHTML`，代码及实体转义不带该属性。
- Android 已有 CommonMark/Markwon，未给 HTML 换行节点添加呈现适配。

## 本轮边界

只把解析器识别的无属性 HTML `br` 标签呈现为硬换行，支持大小写和常见自闭合写法；
不对整段消息做字符串替换，不改变表格行结构，不执行 HTML。
同步完成现有“我的”页面及 Portrait v8 的整包回归；设备与正式产物验证结果待补。

## 实施与最终证据

三个实际呈现适配都只消费解析器识别的行内 HTML 节点：Qt md4c 开启行内解析但保留块 HTML
禁用；Swift Foundation 根据 `.inlineHTML` 属性识别，源码位置属性用于区分相邻标签；
Android 为 CommonMark `HtmlInline` 注册 Markwon visitor。代码、反斜杠转义、注释和标签
属性中的文字不被全文替换。Qt 原来的命名实体显示行为未扩大修改。

旧 Qt 实现对同一用例稳定失败：得到 `Expected:<br>320x180<br><br>details`；新实现得到
`Expected:\n320x180\n\ndetails`，表格仍为两列。正式 CTest 的 document/layout 两目标通过；
独立定位 harness 的 16 项解析及 17 项排版检查通过。正常 MainWindow 加本地 Fake IM 数据
已实际截图检查，代码示例仍显示 `literal <br>`，表格显示真实换行。

iOS 2.2.4（194）最终 App：五项 UI 回归通过，覆盖正常 Root 的 IM 与创作中心表格、主题
实际切到深色、偏好/字号/存储/关于/退出取消，以及三项原有人像流程。两项 Core 测试通过。
已查看最终截图及录屏采样；首次视觉核对发现列宽估算过窄，已恢复原有列宽策略并复测。
测试使用离线内存账号；新测试路由也纳入既有 standalone guard，避免请求未初始化的 Agent。

“存储预览没出现”的初轮断言实际是系统 popover 隐去取消行；完整 UI hierarchy 已证明精确
文件数及字节确认存在，改为验证并点击系统外部取消区域。没有修改产品确认逻辑来迎合测试。

Android 2.2.31（268）：两项 JVM Markdown 测试通过；八项人像回归、三项原有我的/偏好/
普通 JNI 回归及新增正常 IM 表格用例通过。新增用例补齐旧纯数据测试宿主缺少的远控 idle
对象，按单元格内容验证而不依赖 Android span 的枚举顺序。主 App 与测试 APK 都来自本树。
初轮截图放 cache 后未保留；验收截图改存测试专用 files 目录，并实际读回检查。后续 Android
存储页的圆角/边框/紧凑按钮视觉修订另做正式 APK 覆盖与四项 UI 回归，见我的页记录。

主要证据目录：

- `/private/tmp/maichat-markdown-br-20261105/`：旧版失败、正式 Qt 构建/CTest/真实 MainWindow
  截图、两端 Core/JVM 检查及 Android 正式 APK、日志和最终指纹。
- `/private/tmp/maichat-my-markdown194-final-20261105/`：最终五项 iOS UI 结果、录屏、18 张
  截图及录屏采样图。
- `/private/tmp/maichat-beauty194-20261105/device-final-reviewed/MaiChat.app`：最终真机包，
  11 个 Mach-O 严格验签通过，随包 2B GGUF 校验通过；独立指纹为同目录上一级的
  `device-reviewed-artifact.json`。

真机安装尝试返回 CoreDevice 4016，iPhone 连接不可用；没有宣称安装成功。macOS 2.2.48
完整包签名及原生 `--smoke` 退出 0，尚待用户确认替换当前 2.2.34 进程；Windows 未构建或
运行验收。全部代码仍为 `eee43402 + working tree`，保留原有未提交工作，未新增提交或推送。

最后 Android 存储布局修订后的冻结 268 APK 已再次安装，完整 MySettingsInstrumentedTest
四项通过（11.077 秒），再次查看截图并重新启动正常 MainActivity。此前八项人像验证的
处理核心未改；没有把 adb 自身的退出 0 当作 JUnit 成功，而是核对 `OK (4 tests)` 及失败数。
最终包、源码、进程及验证边界统一记录于 QA 根目录的 `delivery-manifest.json`。

## 15:52 真机安装完成

用户明确要求安装最新 iOS App 后，iPhone 14 Pro Max 已恢复 available (paired)。复核当前
iOS 及共享处理源码与最终指纹一致，11 个 Mach-O 哈希及整包严格签名有效，直接覆盖安装
已冻结的 2.2.4（194），没有重建或混入其他版本。

安装命令成功；按 bundle ID 回读设备版本为 2.2.4／194，前台启动成功，实时进程查询确认
PID 32177 的可执行文件位于此次安装返回的同一 App 容器。没有卸载、清理用户数据、代拍照
或发送模型请求。本次确认安装和启动，不扩大为真机手势或美颜审美验收。

独立回执：`/private/tmp/maichat-beauty194-20261105/device194-physical-verification.json`；
安装、版本、启动及实时进程原始结果分别保存为该目录的 `device-*-requested.json`。
统一交付清单已更新，移除此前 iPhone 安装失败的当前阻塞标记；Mac 重启确认仍未收到。
