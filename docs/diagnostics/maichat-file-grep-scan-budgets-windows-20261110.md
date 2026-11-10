# file_grep 有界扫描与取消：Windows 独立验证

## 需求与现场口径

用户反馈 Mac 的 DeepSeek 内容搜索长时间 Running，工具实际参数搜索整个 OpenSource 目录，正则包含一个较宽的数字子串。后续截图已显示工具完成、命中 Chromium SVG 坐标；该匹配符合原正则。现场 sample 的主要模型线程栈在 requestCompletion/curl，不证明 grep 死锁，也没有原扫描完整起止时间。

本次目标是限定扫描工作量、允许精确文件路径、正确响应取消并如实报告受限/未读取结果。测试使用自建临时文件和同一字面模式，不读取用户会话数据库、不调用付费模型，也不停止现装 App。

## 最终源码与范围

- 基线：`49d0168853f4adcbec7ab3040c3f44241b7e8826`。
- 生产源 `MaiAgent/AgentTools/MaiFileTools.cpp`：55364B，SHA256 `507381a0b403b86ade2f37d02be1b8bf902392a77df9100cd66a441285a855e7`，Git blob `d0d2c1779a04209e3030c4bd5929726bae734273`。
- 原测试 `MaiAgent/tests/MaiToolTests.cpp`：39211B，SHA256 `2ef5899c6c1959722a102bba0c91dc384f1c12197aef02a08e7c87f95aa22c77`，Git blob `4b09c9f50a8a8f6bad1e8b4427eb35e81d6a6a86`。
- 仅修改上述两个源文件；不改 header、CMake、SDK、flags 或版本。两次普通 patch 均核准长度/SHA、previous/new raw 与 Git blob；先逐阶段应用测试、保留旧生产实现复现 RED，再应用对应生产修正。

## 修正契约

1. 已知具体文件时可将 `path` 指向该普通文件；原目录、glob、编码、路径权限与正则含义保持。
2. 协作式上限为 5 秒、50000 个目录项、32 MiB 原始总读取量。单文件仍限 2 MiB，实际读取也传上限。受限无命中明确标为部分搜索，不报完整 `No content matches`。
3. 在目录项、读取/解码返回及逐行边界检查取消，最终返回 `Canceled`，不会把取消记作成功完成。
4. 拒绝读取非普通文件。精确文件超限、元数据失效或真实读取失败返回明确错误；目录原有大文件/读失败跳过行为不扩改。

这是协作式检查，不能抢占单次阻塞 I/O、宿主解码器或 `std::regex_search`。原 400 字节行匹配限制未改。

## Windows RED → GREEN

环境为 Windows x64、MSVC2022，复用正常 Core Release cache；没有替换编译器、修改测试断言或放大 timeout。

| 阶段 | 结果 | 实际运行 |
| --- | --- | --- |
| 原生产 ac244154 + v1 新测试 | 23 例中新增 5 例失败，共 15 断言；原 18 例保持 | exit1，5.806s，PID44768 |
| v1 生产 52b84955 + 同测试 | 23/0 | exit0，5.770s，PID46116 |
| v1 生产 + v2 精确大文件新例 | 24 例中仅新增 1 例/3 断言失败 | exit1，5.791s，PID968 |
| 最终 v2 + 同 v2 测试 | 24/0 | exit0，5.858s |

P2 是独立审查发现：v1 新精确文件入口会把超过 2 MiB 或读失败的未扫描文件报作完整无匹配。Mac 对同 v1 库复现两例 6 断言失败，Windows 对可执行的大文件分支复现 1 例 3 断言失败。最终 v2 保留全部原例并收紧该单文件分支。

POSIX FIFO 与 mode-000 真实拒读两分支没有在 Windows 编译或计入，不能把 Mac 的 26 例写成 Windows 26 例。Mac 最终 26/0、真实权限拒读未 SKIP，另有同源 Core 13/13 PASS160.87s；见 [Mac 原诊断](maichat-file-grep-scan-budgets-macos-20261110.md)。

最终 Windows Core 三目标正常重编/链接后，直接完整 `MaiToolTests` 运行一次；注册的 `MaiTurnRunnerTests` 106.68s、`MaiToolLoopTests` 15.24s 均 PASS，2/2 共121.92s，未重复跑工具组。最终工具 exe SHA256 `872c96bde46d4a59c8b7c76193d071eef9bc82890cd1283ba9d5d4094c4b21c4`。

## 实际开发构建

正常 Desktop App 增量构建 exit0、9.825s，windeployqt 和 3 binaries/114 PE imports 门禁完成后才启动。裸 Windows PATH/私有数据环境的单次 `--smoke` 为 PID2476、版本2.2.56/Qt6.12.0、exit0、2.005s；五项 Qt/QWindows 模块从该 normal cache/bin 加载。实际 exe SHA256 为 `a003e63c3dad5fe856e76b5508f8afb1472d0128aa764a63ec756887e9b32697`。

Mac 正常开发 App 为另一个实际产物：SHA256 `c9f43ce01b1b0c7936ab5bef5b27608b6c3f41843616eae9cb56a2b4112f8260`，Cocoa smoke PID76425/exit0。Root 已核对收到的六项真实 dyld 模块行、原日志和源指纹；不是现装 signed exe `50e8b5bd…deaeba`。

Root 逐项核准 Windows 19 份原件、四个独立 cohort 的日志/保全 exe、最终 Core/桌面实际 exe；Mac 的 16 份列出原件长度/SHA、13 组原 CTest 及实际 smoke 也逐项核准。证据存于 `.cache/grep-running-20261110/`，机器摘要见同目录诊断 JSON；现场原 sample 仅私有归档，不提交完整用户进程栈。

## 交付边界

本次源码修正与开发构建已验证，但未替换用户当前 App、未打新包或修改已发布 v2.2.56/tag/八件资产。版本仍为2.2.56的开发 exe 不能当成公开包已经更新；公开包不含此后续修正。未重新调用真实 DeepSeek 搜索用户目录，也未用单元测试宣称现装界面已经修复。
