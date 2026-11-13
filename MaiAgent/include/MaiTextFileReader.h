// 文件作用：提供不依赖工具协议的、有界且可续读的文本文件分页。
#pragma once

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "MaiError.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"

// 停止原因与 EOF 分开：输出、行数和扫描预算用尽都允许通过 nextCursor 继续。
enum class MaiTextFileStopReason { EndOfFile, LineLimit, OutputLimit, ScanLimit, TimeLimit };

// 原始文件位置必须停在完整编码字符之间；lineUtf8Offset 是该行已消耗的 UTF-8 字节数。
// targetLine 保留首次请求的目标行，允许扫描预算在尚未找到目标时也取得实质进展。
// 游标是同一进程内的值对象，调用方可序列化这些字段，但不得修改后继续使用。
// sourcePath 绑定平台原生路径；snapshot 在每次续读前后核对，源文件变化返回 InvalidInput。
// 它不持有文件句柄、宿主回调或取消指针；不能充当针对并发恶意写入者的文件锁。
struct MaiTextFileCursor {
    MaiFilePath sourcePath;
    MaiRegularFileInfo snapshot;
    std::uint64_t byteOffset = 0;
    std::uint64_t lineNumber = 1;
    std::uint64_t lineUtf8Offset = 0;
    std::uint64_t targetLine = 1;
    std::string encoding;
};

// 一个逻辑行的完整内容或连续片段，不含 LF 或 CRLF 分隔符。
// byteOffset 为该片段在当前行的 UTF-8 偏移；complete=false 表示仍处于同一行。
// 首片段偏移为零；接收方按 number、byteOffset 拼接即可恢复超长行，不会重复或丢失字符。
struct MaiTextFileLine {
    std::uint64_t number = 1;
    std::uint64_t byteOffset = 0;
    std::string text;
    bool complete = false;
};

// 每次调用的预算和临时宿主接口。读取期间会同步调用回调，返回后不保留它们。
// offset、limit 为正数；limit 最多 10000；maxOutputBytes 为 4..1MiB；maxScanBytes 至少 4。
// 扫描预算限制本次推进的原始字节数，允许至多一个完整字符的前瞻及 64KiB 缓冲预读。
// maxDuration 是本次调用的协作式期限；在每块及解码返回后检查，不能抢占阻塞系统调用或宿主解码。
// cursor 存在时沿用其中目标行、编码和文件位置，offset 必须仍为默认 1。
// auto 优先 BOM，否则按有界前缀选择 UTF-8/平台旧编码。后续存在歧义或损坏时明确报错，
// 调用方可指定编码重试；不会在半个字符处切片，也不会用替代字符掩盖损坏数据。
// decodeText 接收至多 64KiB 且结束于完整字符边界的无状态旧编码块，须严格返回有效 UTF-8。
// 每个源字符必须对应一个 UTF-8 标量，ASCII 及行分隔符保持原样；不满足时明确报错。
// 实现缓存块内原始字符与 UTF-8 标量边界，避免逐字调用 JNI 或其它宿主桥接。
// 支持 gb18030、gbk/cp936、windows-1252、latin1/iso-8859-1、shift-jis 和 system。
// Android 必须由宿主为旧编码提供此回调。未知或有状态编码明确返回 NotSupported。
struct MaiTextFileReadOptions {
    std::uint64_t offset = 1;
    std::size_t limit = 500;
    std::size_t maxOutputBytes = 256 * 1024;
    std::uint64_t maxScanBytes = 32 * 1024 * 1024;
    std::chrono::milliseconds maxDuration{5000};
    std::string encoding = "auto";
    std::optional<MaiTextFileCursor> cursor;
    std::function<bool()> isCanceled;
    std::function<MaiResult<std::string>(const std::string&, const std::string&)> decodeText;
};

// 空文件和已越过末行的 offset 返回空页及 eof=true。末尾 LF 不制造额外空行。
// 普通末行即使没有换行也 complete=true；每页内容总字节数不超过 maxOutputBytes。
// scan/output/line/time 限制返回已读内容及 nextCursor；取消返回 Canceled，不冒充 EOF。
// EOF 必须来自实际零字节读取。元数据 size=0 的非空虚拟普通文件允许有界单页读到 EOF；
// 若需要跨调用续读则返回 NotSupported，因为其元数据不能证明内容快照稳定。
struct MaiTextFilePage {
    std::vector<MaiTextFileLine> lines;
    MaiRegularFileInfo snapshot;
    std::string encoding;
    std::optional<MaiTextFileCursor> nextCursor;
    MaiTextFileStopReason stopReason = MaiTextFileStopReason::EndOfFile;
    bool eof = false;
    MaiFileReadStats io;
};

// 无共享可变状态，可在多个线程中为不同调用并行使用；调用方负责路径授权和父目录边界。
// 只接受不跟随末段链接的磁盘普通文件。阻塞 I/O 沿用 MaiFileSystem 的线程契约。
// 不抛异常；参数、编码、源文件变化返回明确错误；读取前后验证快照，避免返回混合版本。
class MaiTextFileReader {
public:
    // 严格转换一段完整旧编码字节，不依赖文件句柄或工具协议；结果必须是完整有效 UTF-8。
    // 支持本文件列出的旧编码及别名；Windows auto/system 使用当前 ACP，POSIX auto 使用 GB18030。
    // 宿主回调优先，接收小写且下划线转横线后的原编码名，保留 auto/system/cp936 等原有含义。
    // 回调须严格解码，不得静默替换损坏字节；本方法再次验证返回的 UTF-8 完整性。
    // 不缓存回调；调用同步执行，可跨线程并行处理不同输入。完整大文件的内存预算由调用方限制，
    // read() 内部只以至多 64KiB 的完整字符块调用同一实现，转换失败不交付损坏的半截文本。
    static MaiResult<std::string> decodeLegacyText(
        const std::string& bytes, const std::string& encoding,
        const std::function<MaiResult<std::string>(const std::string&, const std::string&)>&
            decodeText = {});
    static MaiResult<MaiTextFilePage> read(const MaiFilePath& path,
                                           const MaiTextFileReadOptions& options = {});
};
