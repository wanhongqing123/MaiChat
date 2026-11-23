# 中心工具预算与存储摘要的 Core 回归夹具集成

时间：2026-11-22 17:47（Asia/Shanghai）。Windows x64；main
`38d25b9d7a41ad29128e95289f44937c676ca452` 叠加既有未提交工作。
本次只修改 `MaiTurnRunnerTests.cpp` 和 `MaiContextCompactorTests.cpp` 的相关测试。

## 复现、预期和证据

原始记录保留在 `MaiAgent/.cache/build-file-batch2-20261121/batch9-storage-core-tests.log`：
八套 Core 测试六过二败，Compactor 有 44 条失败断言。当前工作区再次运行旧夹具，
日志另存 `MaiAgent/.cache/core-budget-integration-20261122/` 的 `runner-red.log`、
`history-red.log`、`soft-red.log`，没有覆盖原始记录。

预期是保持新的存储默认摘要和中心预算合同，同时继续覆盖原文留存、会话隔离、
调用归属、准确执行一次、源索引发现、原文补读、失败原子性和压缩后继续同一轮。

根因分三类：

- 旧 Runner 夹具把真实 `app_storage` 默认 dry-run 当作全量清单，要求 971 条路径及
  大于 100 KB 原文；新默认摘要在此次自有目录为 644 字节，自然不会触发中心投影。
- 历史聚合夹具配置已知大窗口，却继续要求固定 256 KiB 和逐行均分 256 KiB。
  当前实现对未知窗口才使用 256 KiB 兜底；已知窗口按完整请求的 95% 输入余量分配，
  单条上限 64 KiB、聚合安全上限 4 MiB。已知 1M 窗口下 300000 字节小行合法。
- Compactor 的硬限、软限及最终提示夹具依赖工具正文制造压力。中心投影后硬限场景
  摘要调用为零，软限请求估计 6097/6486 低于触发点 7372，最终请求从期望的 7781
  降为 1535。这是测试触发条件失效，不能以增大存储默认输出或放宽原有断言解决。

## 修改

- 将大清单改为显式自有 `owned_verbose_inventory` 测试工具，产生 971 个合成路径，
  原始 JSON 280690 字节。保留完整落库先于事件、只执行一次、结构化 preview_id、
  tool_output_read 分页及跨会话拒绝断言；真实文件只留自有哨兵核对没有删除。
  同时检查 `partial=true`、`skipped_lines=1`、`next_offset=100`、`scan_complete=false`
  在原文及模型投影中的值，覆盖 OCR/存储完整性和游标标量的中心透传。
- 聚合历史同时覆盖已知 1M 与未知窗口。保留每条 64 KiB、未知聚合 256 KiB
  及均分边界、原文落库；已知窗口核对完整请求估计，并明确保留 300000 字节小行。
- 硬限触发改用 48000 字节自有调用输入。软限三个模式使用有界调用输入，观察正文
  为 800/500 字节；最终提示夹具使用晚到的调用输入精确校准裸请求至 7781。
  原有用户约束、源 ID 发现、执行次数、CAS、摘要失败、取消、恢复及错误断言保留。

## 验证与边界

唯一构建目录：`MaiAgent/.cache/build-file-batch2-20261121`，最大并行 3。
实际执行该目录 `bin/` 下新链接的 Windows 测试程序；源码不是发布副本。
没有改动 `.cache/r311` 的发布源或发布构建，没有安装、启动、停止或替换桌面程序。

定点日志：`central-green.log`（零失败）、`history-green.log`、`soft-green.log`。
清单两种窗口投影为 33795/4074 字节；已知 1M 历史为 300000/597792 字节；
未知窗口 400 行为 196800 字节，1000 行无法容纳必需包裹时明确拒绝。
硬限五模式原有断言全部通过；软限普通/更高用量/恢复分别为
main 3/2/4、summary 2/4/5，恢复原文请求估计 7645，位于 7372..7782；
最终裸请求为 7781，加入最终提示后的请求未发出。

最终 Runner 标量白名单变更后，重新构建全部十个消费者并跑一次完整门禁：Core 八套加
`MaiAppStorageToolTests`、纯 `MaiScreenshotAvailabilityTests` 共 10/10 通过，8.66 秒。
日志为 `core8-storage-availability-metadata-green.log`，变更前的 10/10 记录也单独保留。
最终完整回归日志及产物身份另存同目录 receipt。测试不调用供应商或外部网络、不访问
用户数据、不抓屏；既有 Compactor 完整测试包含本机 loopback HTTP 假模型协议测试。
不宣称桌面 UI 或发布安装产物实机验收。编译器仍报告既有非本次修改行的 C4244。
