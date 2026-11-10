# Mac file_grep 扫描预算与取消修正（2026-11-10）

## 复现需求与实际现场

用户在 Mac 创作中心的 DeepSeek 会话中，指定一个配置文件后看到内容搜索卡片长时间处于运行中。
调用参数中的 `pattern` 为 `11\.164\.94\.213|11\.164\.94\.|164\.94`，
`path` 为 `/Users/hongqingwan/OpenSource`。预期是及时完成，或明确说明预算受限、取消与读取失败。

后续截图已经显示该工具完成，返回了 Chromium HTML/SVG 中符合 `164\.94` 的坐标。
这符合原正则，并非误读正则。本次不改变匹配含义，也没有自动过滤这些结果。
正式注册名为 `file_grep`；本次未读取用户会话数据库确认原始 part 名称。

只读核到现装 `/Applications/MaiChat.app` 为 2.2.56、PID 4175，
exe SHA256 为 `50e8b5bd0db02b4f247879f4e3ef227227631fe11fcae07e70436a33b2deaeba`，
与已发布 Mac 包一致。3 秒 sample 在该工具完成后的模型阶段采到
`MaiTurnRunner::runUnchecked -> requestCompletion -> curl_easy_perform -> poll`；
该栈不能作为 grep 死锁或扫描栈。未采集原扫描的完整起止时间，因此不宣称证明现场耗时的唯一根因。

## 源码证据与修正

基于 `49d0168853f4adcbec7ab3040c3f44241b7e8826`，仅改
`MaiAgent/AgentTools/MaiFileTools.cpp` 和既有 `MaiAgent/tests/MaiToolTests.cpp`。
原 grep 有单文件 2 MiB、200 命中和约 64 KiB 输出限制，却没有整次扫描工作量限制；
取消仅在目录项间检查，取消后仍可返回成功的 `No content matches`。

修正保留原目录搜索、正则和路径权限规则，增加以下行为：

- `path` 接受精确普通文件，说明优先搜索已知文件。
- 协作式预算为 5 秒、50,000 个目录项、32 MiB 总读取量；预算停止返回明确的部分结果。
- 在目录项、读取/解码返回及每行边界检查取消，返回 `Canceled`。
- 仅读取普通文件，跳过遍历中的 FIFO、socket 和文件链接；实际读取仍传入大小上限。
- 精确文件超出 2 MiB 或读取失败时返回明确错误，不把未读取当作完整无匹配。

预算不能抢占单次阻塞 I/O、宿主解码器或 `std::regex_search`。原来的 400 字节行匹配限制未扩改。
目录原有大文件/不可读项跳过行为不在本次精确文件修正中扩展。

## RED/GREEN 与最终 Mac 门禁

所有新增测试均使用自有临时文件、原图示 IP 字面量和普通工具入口，不调用付费模型。

| 阶段 | 实际结果 |
| --- | --- |
| 原生产源 + 首批 6 新用例 | 16 个断言失败；原有用例保持通过；5.933 秒 |
| v1 普通完整工具组 | 24 用例通过；5.929 秒 |
| v1 库 + 精确单文件 P2 probe | 大文件和 POSIX mode-000 拒读两例共 6 个断言失败 |
| v2 普通完整工具组 | 26 用例通过；5.930 秒；mode-000 未跳过 |
| v2 必要 Core 注册组 | 13/13 通过；160.87 秒 |
| v2 正常 Desktop App 构建 | exit 0；原 Qt6 SDK/cache/flags |
| v2 正常 Cocoa 开发 App 启动 | exit 0；PID 76425；1.742 秒；私有 HOME、裸 PATH、仅 loopback 网络 |

POSIX FIFO 用例由自有写端握手解除原实现的阻塞，并断言新实现不打开 FIFO。
POSIX 拒读用例实测 chmod 000 后 native read 失败；不把这些分支算作 Windows 验证。
首次启动观测脚本没有采到 DYLD 标记，未记为通过；保留原日志，改为通过 sandbox 后的
`env` 参数传入 DYLD 观测变量，最终同一 exe 读到实际 PID 和全部六个 Qt/Cocoa 组件。

最终生产源 SHA256 为 `507381a0b403b86ade2f37d02be1b8bf902392a77df9100cd66a441285a855e7`；
测试源 SHA256 为 `2ef5899c6c1959722a102bba0c91dc384f1c12197aef02a08e7c87f95aa22c77`。
正常开发 App exe SHA256 为
`c9f43ce01b1b0c7936ab5bef5b27608b6c3f41843616eae9cb56a2b4112f8260`。
实际 QtCore/Gui/Widgets/Svg/Concurrent 与 Cocoa 插件来自现 normal Qt6.12 prefix。

## 交付边界

原始 RED、v1、P2 和最终 v2 日志分别保全于执行端
`/private/tmp/maichat-grep-hang-20261110`，通过普通 patch/JSON/log 附件交接，不发送 ZIP。
独立临时 index 核准两阶段 patch 的 previous/new blob，真实 index 保持不变。
四份独立旧文档已逐 SHA 保持，未读取或修改用户配置、会话数据库，未停止用户的现装进程。

本次修正未替换现装、未打包或修改已发布 v2.2.56/tag/资产，也没有服务部署；
目前公开 2.2.56 包尚未包含此后续修正。Windows 结果由协调端独立记录。
