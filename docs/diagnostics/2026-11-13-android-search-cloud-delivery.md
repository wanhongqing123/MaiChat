# Android 搜索修复及云盘入口交付

时间：2026-11-13。源码基线 `583a07bc48c53759a089ae8097184730eedf2a94`，候选由其上明确的
Core/Android/共享移动工具修改构建；实际消费文件 SHA 清单保存在
`.cache/android-search-release-20261113/release-build-receipt.json`。
Desktop/iOS 仍在修改的文件不属于该包构建输入。

## 内容与验证

- IM 搜索/get 改为当前账号本机单聊库查询，peer 等条件先过滤后 LIMIT，支持稳定续页和明确错误。
  七字段 DTO 不解析附件/审批数据、不准备图片；独立 worker、WAL/索引与独立五秒计时保护避免等待主线程。
  自有 SQLite/Robolectric 回归最终 35 项通过，详见 `android-im-message-history-20261113.md`。
- 原生文件工具复用普通读取/搜索 API。Windows 普通 Core 11 组最终全通过；实际 NDK Release 编入
  新 reader/search/文件工具/共享 Host 对象，并核对 archive、链接符号与包内构建标识。
- 云盘根页及子目录整组手动加号/上传菜单移除；Agent 上传、已有任务暂停/继续/取消和普通媒体保存保留。
  十项自有云盘回归包含在上述 35 项中，instrumented Java 仅编译成功，没有创建或安装另一个测试 App。

正常 ARM64 Release 构建退出 0，57.813 秒；签名前 APK SHA-256：
`64a52c41b5a0191d0c896f78f8a18d96f0e3c8f0696bf24f06267a5bb078d2e1`。
包内 26 个共享库、七个 CPU 后端、无 NDK libz 桩，完整 1,396,198,496 字节 GGUF
SHA `57a1085840f497d764a7fc5d346922dbde961efb54cc792ea81d694fd846a1d8` 与许可/来源资产均核同。

## 真机结果

目标 OPPO PKD130 / Android 15，原包 `com.kongshang.maichat`，2.2.32 / 274。
使用与现装兼容的既有本地开发签名，验证 alignment、签名、非 debuggable、非 testOnly，
再以 `adb install -r --no-incremental` 覆盖安装：68.858 秒、exit 0、实际 `Success`。

签名候选及设备实装 APK SHA-256 全等：
`fa7a8f15ce27be0ed015c298fc4bb7d193485c9fbf95b931cce850b0819868c0`。
原 appId 10043 保留，仅有一个 MaiChat package；普通启动成功，回读进程 7696。
没有卸载原 App、清数据、安装隔离 App 或改变系统安装安全设置。
本次没有记录到安装器点击动作；不能据此承诺其它机型或未来每次更新都不会出现确认界面。

在确认正常 MaiChat 位于前台后进入“云盘”，实际空根页标题、配额和导航保留，加号不存在，已查看原始截图。
根页零媒体/零文件夹，未为验收创建真实文件夹或上传文件；子目录由普通代码/自有 UI fixture 回归覆盖。
没有直接查询真实聊天数据库或触发模型请求，性能数据来自自建测试库，不能当作该手机硬件的搜索耗时承诺。

安装、设备 APK 哈希、版本/UID/PID、签名/内容证明及空根页原图均保存在
`.cache/android-search-release-20261113/`。这是本地正常候选的真机交付，不是商店发布。

## 最终模板兼容及共用编码修正

普通工具组未覆盖 Qwen XML 参数提取；随后使用实际 factory + 当前 vendored Qwen3.5 模板/grammar
纯解析，发现根级 anyOf 只允许空调用。旧正常库真实四断言失败；平面 object schema 修正后，正常
`MaiFileToolSchemaTests` 精确解析全部参数。原生二选一校验仍拒空参数和同时传 pattern/query。
Mac 普通读取还发现系统 iconv 不支持 GB18030 补充平面，现读/搜共用严格平台解码后端，Apple
无损 CF 回退与原始正/负样本均通过。上述五个受影响普通目标最终全通过，Mac 普通库及实际工具也通过。

因此前一候选没有作为最终包封板；保留它的完整失败/通过与安装记录，最终再对同一个原包覆盖一次。
本轮正常 Release 增量退出 0，30.777 秒；新包再次核验所有 Native/模型/许可、签名和原证书兼容。
签名前 SHA：`0cdcc807e7a526eee9a481c11aa52ca8b890261b1404d20f58fc0fce2657c2f6`。
最终签名及设备实装 SHA 全等：
`890f2acb05f799fab576d5cd3c626d4aa293209eb5226ccb117a2ea23ae4bac6`。
覆盖安装 63.870 秒、exit 0 / Success，普通启动进程 11238，appId 10043 保持，仍只有一个 MaiChat 包。
最终明细在 `.cache/android-search-release-final-20261113/`；没有安装第二份 App，也没有卸载或清数据。
