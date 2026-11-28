# 创作中心普通文件拖放交付记录

## 操作与结果

从系统文件列表将本地文件拖到创作中心输入框边框或编辑区，或用“＋→选择文件”，
应出现文件草稿卡片并保留已有文字；只有明确点击发送才提交。
普通文件以 JSON 无损路径引用进入正常 prompt，正文不内联、不上传、不启动文件；
图片/视频保留原导入发送流程。移除、清空、会话切换及析构不得删除外部源文件。

产品基线 `23288f8586f3a8cbd82a83d44a3eea0f57add6eb`，Mac 只改
AgentChatPanel.cpp、对应头和 AgentPanelSessionTest.cpp。
Root before 为 CRLF，Mac before 为 LF；规范化后大小、SHA 和 Git blob 全部吻合。
Root 保留原 before 字节并在私有目录实际应用 full-index 补丁，规范化后逐件等于 post，
随后复制仅三件审定 post，没有覆盖其他 WIP。

## 红绿与独立审查

Mac 原基线真实 RED 为文件卡片 0/期望2。开发期间另有边框拒绝、同事件跨对象
sendEvent 导致 QApplication/PromptEdit 递归的 owned crash，以及 picker 夹具未完成
modal 交互导致的外层90秒超时；原记录均保留，不算修复后产品测试。
最终仅 PromptEdit 直接走同一 MIME 路径，不改共享 ComposerTextEdit/IM；
本地文件强制 CopyAction，Move-only 拒绝，不改变普通文本拖选。

Root 第一轮独立审查发现 P2：每个普通非视频文件都先执行 desktopInspectRasterFile，
其 localFileBytes 在 GUI 线程按全长分配并读取，默认上限128MiB，甚至早于额度检查。
第一版未合入 Root 产品。修正后先去重和检查总量/pending，再以 Unbuffered QFile
单次最多4096B读取文件头，QMimeDatabase 仅做 MatchExtension 和内存候选查询；
只有媒体候选进入原严格检查。前缀严格解析失败不被用作“不是图片”的依据。

v2 专门覆盖未知后缀 PNG/JPEG、8KiB APP段使尺寸超出前缀的 JPEG、TIFF/HEIF/video、
损坏候选，以及11个100MiB sparse普通文档的去重/数量预算。独立复审确认原 P2 已解决，
未发现新增 P1/P2。普通文件 I/O 上界来自实际代码及调用链审查，不冒称系统调用测量或耗时基准。
媒体候选保留既有全文严格检查成本，未扩大成对所有普通文件全文读取。

## 审定供件与 Mac 结果

v2 ZIP：857109字节，SHA256
`4fb6409bc501c97fc16e49294f6fac9271f5bcdd2b1fbe75196d2c9ec1bc13de`。
三件 canonical LF post SHA256：

- cpp：`ff31e69846ffada842b43604e896fef9356953e67cd4ad8ad7bfac3df8c9606d`
- h：`dfb359091a1f7ae92f8e09ba6663195bafd2abaca2f5384c6a5566ddaaf10c12`
- test：`00f741e38379fc7b94c3aa6be2dd0104047a577719c7de36a7f827943b372da5`

Mac 最终同一 binary 的原29项加新10项为39 passed、0 failed，13363ms；
普通 App 重编和私有 profile --smoke 退出0。
Mac App 绑定 SHA256
`0c4344ec23ccf53ad442378dfe28676b52f65874037796a4796987b8961b724b`。
这是供件中的 Mac 构建回执，Root 没有收到 App binary，未独立重哈希。
Root 核验全部供件 manifest、真实发送 JSON 和 owned PNG，前后源码摘要与 post 一致。

## Root Windows 结果

正常 MSVC 构建/官方测试包装器，QPA windows、独立 profile 和自有文件：

- 原产品配新增同用例：build0，viewport 文件卡0/期望2，真实 RED；不是缺 DLL 或编译失败。
- v2正常 App/Qt组件 build0（23.221s），同一组39 passed、0 failed（10331ms）。
- 覆盖 picker、Copy/Move、边框/viewport、路径、缺失Send保留、question答案保留文件、
  explicit Send同turn steer、跨session、source保全，以及旧文本/图/视频/Header/receipt。
- Root 查看两张实际 Windows widget 截图；捕获的 frame/viewport payload 逐条 JSON roundtrip，
  不含自有测试正文，路径均属于独立 profile。Windows使用合法的中文/空格文件名；
  引号与实际换行文件名的真实文件用例由支持该命名的平台完成，未伪称 Windows可创建。
- 正常 package 的 PE 门禁通过，staging 和实际 ZIP 解压目录各用裸 PATH/独立 profile
  启动 --smoke 退出0；两copy精等正常App exe SHA256
  `59fd63ec0842cad2786c2a9437468d74cafb7af82b153123c5517799b6f2f0c8`。

自有原始材料及机器回执在 `.cache/composer-file-drop-20261128/`：
mac-delta/v2、root-baseline.json、windows-red-01、windows-final-01、candidate-windows-01。
以上 UI 使用真实平台 widget 与 MIME 事件注入，picker 使用 Qt 非 native owned 对话框；
不冒称手工 Finder/Explorer 外部拖放验收。未使用真实账号/模型/用户数据，未改线上或用户现装。

同会话另外开展的 Core 能力摘要/描述精简尚有独立改动与验证流程；
本记录绑定的是文件拖放加原 Core 的构建，不能冒称包含尚未交付的 Core 能力摘要。
Core合并后若更改正常App输入，会另做最终组合验证。未出正式新版本或替换公开4.0.2资产。
