// 文件作用：为进程内命令脚本提供复用原生文件系统和路径守卫的普通宿主。
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "MaiCommandScript.h"
#include "MaiFilePath.h"

class MaiFileSearch;
class MaiTextFileCursorCache;

// 两个目录必须是现有绝对目录，workingDirectory 必须位于 accessRoot 内。
// 所有回调按真实路径逐段校验容器边界并拒绝路径中的链接；不扩大边界或改变进程工作目录。
// maxOutputBytes 为 1..16MiB，同时限制单次输入、完整文件读取、写入和命令输出。
// isCanceled 在每次回调和有界循环检查；原生单次 I/O 不能被抢占。
struct MaiFileCommandOptions {
    MaiFilePath workingDirectory;
    MaiFilePath accessRoot;
    std::size_t maxOutputBytes = 1024 * 1024;
    std::function<bool()> isCanceled;
    // 跨宿主续页必须显式共享同一服务，并为不同会话/解码策略提供不同 scopeKey。
    // 空服务由本宿主创建；游标不跨进程。根路径始终额外绑定，不从游标扩大授权。
    std::shared_ptr<MaiFileSearch> search;
    std::string scopeKey;
    std::function<MaiResult<std::string>(const std::string&, const std::string&)> decodeText;
    std::shared_ptr<MaiTextFileCursorCache> textCursors;
    // 只限制 read-text/find/grep 的完整页（含续页事实），不改变 cat/管道/写入字节预算。
    std::size_t maxResultBytes = 64 * 1024;
};

// 返回持有配置值副本的同步宿主，不持有工具协议、进程、工作线程或调用方栈引用。
// 支持 echo[-n]、pwd、cat[单文件或 stdin]、ls[-a][单路径]、mkdir[-p]单目录、
// cp/mv 两个普通文件路径（无覆盖）、rm 单文件、touch 单文件，以及
// head/tail[-n N][单文件或 stdin]、stat PATH、rmdir PATH（仅空目录），以及
// find PATH -name GLOB [--limit N] [--cursor TOKEN]（递归文件，不返回目录），
// grep[-F][-n][-i][-r][-l|-c] PATTERN [PATH] [--limit N] [--cursor TOKEN]。
// grep 默认使用 MaiFileSearch 的有界安全正则，-F 为字面串；目录必须 -r，
// 无 PATH 的 -r 从当前目录检索，其余无 PATH 或 PATH=- 使用 stdin。
// find/grep 的 stdout 是数据，stderr 是 search_status JSON，含 partial/has_more/
// truncated/next_cursor/issues 等事实；页上限会按完整结果预算缩小，后继页须保持参数。
// stdin 在 accessRoot 内独占暂存并在退出时按身份清理；单次有界消费，不返回临时源游标。
// read-text[--encoding E][--offset N][--limit N][--cursor TOKEN] PATH 复用文本读取核心，
// stdout 为 text_page JSON，含完整行或长行片段、行号/行内字节偏移/complete、encoding、
// eof/partial/truncated/stop_reason 和 next_cursor；--cursor 与 --offset 互斥。
// 文本游标绑定真实路径、根、scopeKey 和所请求编码，续读时须保持 encoding 参数。
// scan/output/time 达限时保留真实续读位置，不把预算耗尽伪装成 EOF。
// rmdir 拒绝容器根、初始及当前工作目录和它们的祖先。
// 不支持递归删除/复制、组合短选项、超出安全子集的正则或外部命令回退。
// touch 仅保证普通文件存在，已有文件字节和时间戳保持不变，不声称完整 POSIX touch。
// read/write 按原始字节交付；追加复用 MaiAppendFile 的写锁，不做文本编码转换。
// execute 的普通参数/文件错误以非零 exitCode 返回，支持脚本 &&/||；
// 越界、取消、配置及预算错误立即返回 MaiError，read/write/chdir 的错误均为硬失败。
// 配置非法仍返回具备全部回调的宿主，实际调用返回明确错误；不抛异常。
// 多命令脚本和系统 I/O 失败不是事务；不回滚先前成功命令或已发生的部分写入。
// 真实路径检查不能抵御授权目录被恶意并发替换，不构成内核级沙箱。
// 回调可重入；宿主不共享目录状态，针对同一目标的并发修改由调用方协调。
MaiCommandScriptHost makeMaiFileCommandHost(MaiFileCommandOptions options);
