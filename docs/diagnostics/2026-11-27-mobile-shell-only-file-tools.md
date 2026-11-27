# 移动文件操作统一为进程内命令

后续已补 Android 真机和独立 App UID 验收，并修复真机暴露的普通文件发布/移动路径。
结果见 [真机验收](2026-11-27-android-physical-shell-validation.md)；以下 Windows 链接缺口是初始阶段边界，
不是当前 Android 目标不能运行的结论。外部授权目录挂载和正常 MaiChat 完整 UI 流程仍未验收。

日期：2026-11-27；基线 `be9711aeac49e2be0c1c2f37069cc910ff90fc90`，保留已有未提交工作。
用户修订上一轮方案：移动端不再需要独立 file_* 模型工具，而由仿真命令环境执行文件操作。
这取代上一轮“移动端保留公开 file_*”的产品注册策略，不删除历史事实或共享文件算法。

## 操作与期望

模型通过移动 `exec_command` 执行 `find . -name '*.txt'`、`grep -n 'pattern' path`、
`read-text --encoding gb18030 --offset 1 --limit 100 path` 等命令；精确文本修改使用 apply_patch。
新的移动模型请求不再出现任何 file_* 声明，也不能从提示词学到不存在的旧入口。
读取/搜索达限时提供完整的分页事实，跨调用游标仍受会话、容器、路径、编码及源快照绑定。
App 外相册/文档原件不获得删除、移动或覆盖权限；当前授权目录挂载仍未接通，沿用授权导入副本。

## 实现

- MobileAgent 注册进程内 exec_command 与 apply_patch，移除 registerMaiFileTools；保留既有
  媒体交付、独立 curl/FFmpeg 和其他真实宿主能力。旧 file_* 历史记录不改写。
- MaiFileCommands 增加 find/stat/rmdir/read-text 及正则 grep，复用 MaiFileSearch、
  MaiTextFileReader 和原生文件系统，不经旧工具 JSON 调度。
- search/text cursor 服务由薄工具持有，跨调用共享但按宿主身份绑定；缓存有容量及过期限制。
- 完整结果渲染预算与文件/管道字节预算分开，确保模型输出上限不会切掉分页元数据。
- read-text 支持跨调用长 UTF-8 行片段与旧编码读取，源文件变化和跨会话重放明确拒绝。
- find/grep 的 stdout 保持数据，stderr 提供 search_status JSON；read-text 返回 text_page JSON。
  stdin 正则搜索使用 App 根内独占暂存，单次有界消费，不返回即将删除的源游标。

语法仍是已有受限子集，不是完整 Bash；不开放进程、PTY、后台、命令替换或控制流。
curl/FFmpeg 当前仍是各自改造的内嵌入口，尚未挂到解释器内，不能把其存在当作 Shell 已支持任意命令。

## 验证边界

新 native 命令私有测试覆盖 2MiB 中文长行跨宿主拼回、旧编码、源变化、跨 scope 续页拒绝、
1KiB/16KiB 完整页预算、安全正则、取消、当前/初始目录删除保护和 stdin 异常暂存清理。
原 FAIL 与修复后的日志在 `.cache/mobile-file-commands-20261126/search-text-{red,green}-run.log`。

真实 MobileAgentTests 的 loopback 合同和旧编码/审批用例已随入口更新，新增
`--file-command-contract-only`。当前 Windows 私有移动 lane 缺少既有内嵌 OpenSSL CLI、
平台相册注册等链接符号，合同测试没有运行。两个源文件编译成功不能冒充完整合同执行成功。
编译与链接日志在 `.cache/mobile-file-contract-own/{before,after}`；不加假 stub 或平台宏绕过。

本轮没有操作现装、用户数据、真实相册、文档或付费模型；移动真机与 iOS 编译仍需单独验证。

## 正常构建与回归结果

正常 Windows App build 0，3 个二进制/116 个 PE 导入检查通过。共享核心 8 个目标全 0：
CommandScript、FileCommand、InProcessShellTool、TextFileReader、FileSearch、ApplyPatchTool、
ModelToolCatalog、Tool。薄工具补测同实例跨调用文本续读，确认第二行、跨会话拒绝和源变化拒绝，
exit 0。这覆盖普通命令与实际模型工具适配，不冒充完整移动 C ABI 执行。

Android NDK 28 ARM64/x86_64 API26：命令解析器、文件命令、薄工具、文本游标缓存均语法编译 0；
移动适配层两个目标也语法编译 0。上述不等于 APK 链接、安装或手机实测。

证据 `.cache/mobile-shell-only-20261127`：`build.log`、`normal-regressions/results.json`、
`tool-lifetime-final/results.json`、`android-syntax.json`、`android-mobile-adapter-final-syntax.json`。
本轮未创建新的移动包或 GitHub Release，也未提交/推送；现装 Windows PID 27200 保持不动。
