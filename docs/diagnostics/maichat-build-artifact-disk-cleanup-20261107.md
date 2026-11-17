# Mac 构建产物磁盘清理

2026-11-07，Asia/Shanghai。用户报告 Mac 磁盘再次耗尽；操作前 main 为
`b95e1ff006628bbf1301d7ec42749f6417442f36`，工作区干净。

`df` 显示可用空间约 2.4 GiB。检查发现 `/private/tmp` 中 MaiChat/MaiAgent
历次任务目录累计约 142 GiB，其中 244 个临时 App bundle 约 74 GiB。
这些数字是目录占用统计；APFS 共享副本会使它们大于实际可回收空间。
根因是多轮验证保留了完整 App、重复模拟器构建目录、打包副本与旧编译缓存，
交付后没有及时清理。

清理前检查进程及打开文件，排除当前构建、最新移动端产物及使用中的文件。
两轮共清理 112 个生成产物路径，目录统计约 108 GiB：

- 旧模拟器构建产品副本、旧完整 App bundle。
- 已完成交付的本地打包、解压验证目录及上传分片。
- 已被当前构建替代的 CMake 目录及仓库内未跟踪的历史 desktop build/dist。
- Android `.gradle/cxx-before-repository-split` 旧缓存备份。
- 旧测试 IPA 和未运行的专用测试 AVD 快照；保留 AVD userdata。

清理后 `df` 可用约 48 GiB，实际净释放约 46 GiB；无删除失败。
没有启动构建或测试，没有停止程序，没有修改产品源码或安装程序。
本次只新增这份诊断记录。

保留的可复用产物及证据：

- `/private/tmp/maichat-desktop-portrait-20261105/full-config` 当前桌面构建。
- `/private/tmp/maichat-pdf-core-gate` 当前 Core 构建。
- `/private/tmp/maichat-resident-screenshot-mac-20261107-15e573c9` 最终回执、源码备份及证据。
- 最新 `device202-final/MaiChat.app`、当前 Android 构建和模拟器 userdata。
- `/Applications/MaiChat.app`、仓库源码、现有截图/录像/测试日志。

最终证据 ZIP 的 SHA256 再次核对为
`f18bf96a2326ae6d9c04dd6899330398fb78a9619a488d06e9bad2539c159554`。
逐项清理清单、实际空间变化及保留的少量产物元数据位于
`/private/tmp/maichat-storage-cleanup-20261107`。
历史回执中的部分临时可执行文件路径已清理；其执行结论仍对应当时记录的源码与哈希，
不能据路径不存在否定旧结果，也不能把旧结果当作重新执行。

后续构建复用固定目录；每个平台保留一套当前可用构建和最新交付产物。
隔离验证优先只复制必要对象或小夹具，避免复制包含完整模型资源的 App。
交付后清理重复打包、上传分片和旧 App 副本，保留小体积日志与哈希回执。
大构建前检查剩余空间，空间不足时先清理可再生成的历史产物。
