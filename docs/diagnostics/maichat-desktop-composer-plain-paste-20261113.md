# Desktop 创作中心粘贴保留字面文本

## 复现与预期

2026-11-13，用户报告 Mac 创作中心输入框粘贴后带入来源的黑底、字体和链接样式。
预期为粘贴文字、换行、中文、emoji 与 Markdown 字符，使用输入框自己的样式；
图片、文件仍先走既有业务 MIME handler，不把附件变成文字或丢失。
本轮只用自有 HTML/Markdown/plain MIME，不读取或覆盖现场剪贴板、用户数据库或截图中的链接。

## 根因

共享 ComposerTextEdit 未设置 acceptRichText=false；普通 IM 和远端输入框分别显式设置了，
本地 Agent PromptEdit 没有，因此普通 Qt 回退会导入 HTML/富文本样式。
另经两端源码审读，Qt 6.12 QWidgetTextControl 的首个 text/markdown 分支不检查该属性，
仅设置 false 不能保证 Markdown 字面不变。

## 修改范围与合同

仅 ComposerTextEdit.cpp/h 和原 AgentPanelSessionTest.cpp 三源。
Mac 以 `15074abadcdd33db56a00be2e779fa70e1ccef60` 加补丁验证，Windows 以
`8b512fbc47083edb884d5a31dba73af343a42674` 加同三源验证；两个基线的 Desktop/Core 输入一致。
plain patch 为 12537 bytes，SHA256
`3ecddc2315ff5d40ae8f2965d11351192f9fa8bd56cc5eb00892e3f77cb5c66c`。
Root 独立核准三项 previous/new blob、canonical 长度及 SHA 后应用；Windows 的 CRLF 原字节另记，
不把它们冒充 Mac 的 LF 原字节。

- 共用输入框默认关闭富文本接收，保留文档 API 的内联附件能力。
- handler 始终先收到完整原 MIME；canInsert 查询不调用有副作用的 handler。
- 有 text/plain 时以其为准，先收敛成 text-only MIME 再交 Qt。
- Markdown-only 按原 UTF-8 字面输入，canInsert 同步支持；HTML-only 无纯文本不导入。
- 有 handler 的 imageData+HTML/no plain 仍可进入图片路径；原本地文件路径判定保留。
- 没有新增 HTML 解析、网页读取、真实模型请求、版本或发布操作。

## Mac 原用例

原 Makefiles cache 缺两个 flags.make 首次 exit2/0.332s，仅为生成文件问题，不计功能 RED。
同目录正常重新 configure 后，新用例配旧生产实际 4PASS/6FAIL（含 hooks），668ms、exit6：
HTML/qrichtext 背景、Markdown 字面/换行、HTML-only 与实际 Panel 富文本属性失败均保留原日志。

换新生产后同新槽 10/0；同轮增加两个既有安全布局槽，最终 12/0、859ms，进程 3.747s/exit0。
正常 App+target 增量 build exit0/28.765s，执行端 exe SHA256
`60e5eac0ca61ca9bc3efc824d3bec0ed6c42d8a7f4a9c923572067d054d5ab4d`。
Root 独立读回原 RED/GREEN txt 与小回执，亲看两张实收自有 Panel 图：旧黑底/大字体、修正后普通正文，
换行、中文、emoji 和 **literal** 字符均保留。IM 图片只作展示核对，不声称与 Mac 原 PNG 字节相同。

## Windows 普通消费者

原 Qt6.12/MSVC14.44 Release cache 正常构建 maichat 与 agent_panel_session_test，exit0/28.398s。
post-build windeployqt 和 PE 门禁为 3 binaries、114 imports 及 Qt plugins 通过。
新三槽（其中 MIME 数据六行）加原 composer 布局与最小高度两槽，含 hooks 实际 12/0/0skip，
Qt 989ms、进程 2.108s、PID17396；真实 Windows QPA，私有 USERPROFILE/APPDATA/LOCALAPPDATA。
这些旧布局槽与 Mac 的补充布局槽不同，不把两端称为完全同一十二例。

完整 staging 与实际 ZIP 解压包分别裸系统 PATH 普通 --smoke，PID45388/23368，均 exit0、
版本2.2.58/Qt6.12，五个实际 Qt 模块均在对应包内；启动日志实际落在私有 smoke-profiles。
build/staging/ZIP exe 都为 165798912 bytes，SHA256
`e36d008556c42527c9581e88704ff22538f49c1575d315e58faec2980078a605`。
八十一个文件逐 SHA 相同并保留完整索引；ZIP241654351 bytes，SHA256
`1e3ca88b425146f8da5327631f6346cdd5b0ab4defad0039c6c9b0220b51e99e`。

Root 实际读回上述回执和原测试日志，亲看 Windows 自有 QWidget 子树 PNG；
没有运行会改系统 clipboard 的旧图片/视频粘贴槽，没有全组回归或现装替换。
普通 Windows/Qt 启动不宣称零注册表写入。原字体告警保留，不扩大修改。
本机证据在 `.cache/composer-plain-paste-20261113/windows/`。

## 交付边界

源码修复、Mac/Windows 正常消费者和 Windows 两包启动已验证，未发布或安装到用户现装。
Mac 现装进程仍为执行端报告的原2.2.58，不能把新候选的通过冒充它已更新。
云盘入口移除是独立需求，范围待用户确认；它的未提交 Android 改动不包含在本次 Desktop 交付。
