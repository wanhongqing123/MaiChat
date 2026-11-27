# 文件读取与搜索边界回归

- 时间：2026-11-26，Windows x64，MSVC 14.44.35207，Release `/O2 /MD`。
- 工作分支：`main`；本轮未建立分支、添加依赖、修改用户数据或替换桌面进程。
- 源码基线：`be9711aeac49e2be0c1c2f37069cc910ff90fc90` 加当前工作区；具体编译输入以各阶段 SHA-256 manifest 为准。
- 产物和证据目录：`.cache/basic-read-search-20261126/`。这些是隔离测试产物，不是安装包。

## 可复现操作与预期

1. 创建内容分别为 `needle\r` 和 `needle\r\n` 的普通文件，通过普通 `MaiFileSearch::searchText` 搜索字面 CR。预期只命中前者，末尾独立 CR 不得作为 CRLF 分隔符丢失。
2. 创建 `.gitignore` 和 `source.txt`，令 `maxBytes` 恰好等于忽略规则的字节数。预期规则读取消耗预算，正文不能额外读取；结果必须明确不完整。新建文件名查询的 `readBytes` 包含规则读取，复用冻结索引时为零。预算不足以读取根忽略规则时停止采集，不交付假完整列表。
3. 创建带 LE/BE BOM 的 UTF-16 文件，在 `file_grep` 指定相反端序。预期和 `file_read` 一样返回 `InvalidInput`，指出 BOM 冲突；自动识别仍正常命中。

## 根因与修复

- `textLines` 无条件剥除行尾 CR，包括没有 LF 的最终行；现在仅在实际存在 LF 时剥除配对 CR。
- 目录采集每个 `.gitignore` 最多读 64 KiB，但此前没有计入总 `maxBytes` 或 `readBytes`。现在规则和正文共享总输入预算；规则预算耗尽时停止采集并标记 `ignore_read_budget`。复用索引时清零本次规则 I/O，避免复报历史读取量。
- 工具适配器的整文件 UTF-16 解码此前没有检查相反 BOM，导致成功返回乱码并伪装成无命中；现在与流式 reader 一样拒绝明确冲突。

## 验证证据

- `red/manifest.json` 和 `red/MaiFileSearchTests.run.log`：新增普通入口断言出现 8 个预期失败；同批 `MaiTextFileReaderTests` 为零失败。
- `green/`：搜索与 reader 两套普通入口完整测试均为零失败；最终规范化源码另在 `final/` 复核。
- `model-red2/`：完整 `MaiToolTests` 出现 4 个预期 BOM 冲突断言失败，自动解码断言通过。
- `model-green/`：完整 `MaiToolTests` 通过。`result.json` 记录可执行文件与归档 SHA-256；`manifest.json` 记录冻结的头文件、当前四个覆盖模块及测试源码。
- 模型工具测试先将既有 116 个核心 TU 的归档和全部文件型链接输入复制到本轮私有 `deps/`。从复制归档明确移除旧的 `MaiFileTools`、`MaiFileSearch`、`MaiTextFileReader`、`MaiFileSystemWindows` 成员，再加入冻结源码编译的新成员；`archive-members.txt` 验证每个覆盖模块只有一份。未修改原归档或原链接依赖。
- 普通入口测试从同一冻结源集直接编译全部相关的六个实现 TU，不依赖旧核心对象。
- `final/` 的搜索和 reader 完整测试均为零失败；`frozen-owned-files.json` 记录本轮交付五个代码文件的哈希，并逐一核对其内容与最终测试源集一致。
- 既有测试同时覆盖 UTF-8/UTF-16/GB18030、长行续读、offset/EOF、文件快照变化、取消、扫描和输出预算、完整行后截断、正则工作量、隐藏项、忽略规则、分页重放和路径边界。

## 验证边界

本轮验证限于 Windows 原生测试可执行文件。未完成 iOS、Android、Qt 实际运行安装产物的实机验收，未访问真实 UNC 共享；符号链接测试沿用系统权限允许时执行的既有用例。自定义安全正则只提供其公开声明的子集，ASCII 之外不做大小写折叠。文件系统与权限检查仍是尽力防御并发修改，不构成文件句柄级沙箱。

模型工具继续保留独立的 `file_read` / `file_glob` / `file_grep` 扩展名称；普通 API 的搜索和文本分页实现由模型适配器复用。本轮没有把扩展描述成 Codex 默认必备工具，也没有新增普通代码之外的第二套搜索实现。

上游按 `E:/OpenSource/codex` 的 Git 对象 `3d86e43b075adeba2193ec5c22ac92f55afd82e6` 只读核对，未使用另一个 `reference/codex` 的当前 HEAD。该版本 `core/src/tools/handlers/shell_spec.rs` 提供 `exec_command`；`core/gpt-5.2-codex_prompt.md` 第 5 行推荐 `rg` / `rg --files`，缺少命令时使用替代方法。这个精确对象的工具树没有 `read_file.rs`、`grep_files.rs`、`list_dir.rs` 内置处理器，不能把扩展白名单中的同名词汇误当成默认可调用的本地文件工具。
