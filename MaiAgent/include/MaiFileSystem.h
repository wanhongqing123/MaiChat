// 文件作用：定义跨平台文件系统操作及错误契约。
// 对外类型：MaiFileEntry、MaiWalkAction、MaiFileSystem；线程、失败和生命周期以类型附近注释为准。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "MaiError.h"
#include "MaiFilePath.h"

// 文件系统操作。**一律走系统 API**，不用 std::filesystem，不用 fstream。
//
// Windows 走宽字符那套（CreateFileW / FindFirstFileExW / ...），
// POSIX 走 open / read / opendir / stat 那套。理由和 MaiFilePath 一样：
// 窄字符的 Win32 API 会经过 ANSI 代码页，中文路径直接出错。
//
// 另外两个好处：
//   - 错误信息是真的。std::filesystem 把 Win32 错误码塞进 std::error_code
//     之后，"拒绝访问"和"路径不存在"都变成一句含糊的话。这里直接拿
//     GetLastError() / errno。
//   - 嵌入式和移动端的工具链未必带 <filesystem>（它是 STL 里挺重的一块），
//     而这个项目就是奔着那些平台去的。
//
// 形状参考 chromium 的 base/files/file_util.h 和 file_enumerator.h，
// 只取这个项目真正用到的那一小撮。

// 一个目录项。
struct MaiFileEntry {
    MaiFilePath path;      // 完整路径
    std::string nameUtf8;  // 只有最后一段，UTF-8
    bool isDirectory = false;
    std::uint64_t size = 0;  // 目录时无意义
};

// 清理预览使用的普通文件快照。identity 是平台文件标识及高精度修改/变更时间的
// 不透明组合，不能用来展示或跨进程持久化；比较时必须同时核对 bytes 和 modified。
// 它能识别同尺寸、恢复秒级 mtime 的修改，不能作为对并发恶意写入者的文件锁。
struct MaiRegularFileInfo {
    std::uint64_t bytes = 0;
    std::int64_t modified = 0;
    std::string identity;
};

// 本次句柄的实际系统 I/O 统计，供诊断普通代码中的分页/块读取开销。
// bytesRead 只计系统返回的字节，不以文件元数据大小或历史缓存页冒充本次读取量。
struct MaiFileReadStats {
    std::uint64_t openOperations = 0;
    std::uint64_t readOperations = 0;
    std::uint64_t bytesRead = 0;
};

// 持有单个不跟随末段链接的原生普通文件句柄；不复制、不共享，不在内部反复打开路径。
// Windows 以宽字符路径打开且不共享删除，持有期间阻止路径替换；POSIX 使用 O_NOFOLLOW，
// validate 另以 lstat 核对路径仍指向该句柄文件。父目录授权由调用方负责。
// 同一实例仅供一个线程同步使用；析构关闭句柄。snapshot/validate 均查询已打开句柄。
// 元数据大小可能是零或过期，readAt 仍实际读到系统返回零字节才设置 eof=true。
// 本类型不是文件锁，无法防御读取后恢复全部文件元数据的恶意并发写入者。
class MaiFileReadHandle {
public:
    static MaiResult<std::unique_ptr<MaiFileReadHandle>> open(const MaiFilePath& path);
    ~MaiFileReadHandle();
    MaiFileReadHandle(const MaiFileReadHandle&) = delete;
    MaiFileReadHandle& operator=(const MaiFileReadHandle&) = delete;
    MaiError readAt(std::uint64_t offset, std::uint64_t maxBytes, std::string& contents,
                    bool* eof = nullptr);
    MaiResult<MaiRegularFileInfo> snapshot() const;
    MaiError validate(const MaiRegularFileInfo& expected) const;
    MaiFileReadStats stats() const;

private:
    MaiFileReadHandle(std::intptr_t nativeHandle, MaiFilePath path);
    std::intptr_t mNativeHandle = -1;
    MaiFilePath mPath;
    MaiFileReadStats mStats;
};

// 遍历时对每一项的处置。
enum class MaiWalkAction {
    Continue,       // 接着走
    SkipDirectory,  // 这个目录不要进去（node_modules 这类靠它剪枝）
    Stop,           // 整个遍历到此为止
};

class MaiFileSystem {
public:
    // ── 查询 ────────────────────────────────────────────────────
    static bool exists(const MaiFilePath& path);
    static bool isDirectory(const MaiFilePath& path);
    // 不跟进最后一段链接；删除文件时据此拒绝可能误删目标的符号链接。
    static bool isSymbolicLink(const MaiFilePath& path);
    // 拿不到大小时返回 false，size 不动。
    static bool fileSize(const MaiFilePath& path, std::uint64_t& size);
    // 修改时间以 Unix 纪元起算的秒数表示，不跟随最后一级符号链接。
    // 路径不存在或元数据不支持时返回 false，且不修改 `seconds`。
    static bool modifiedTime(const MaiFilePath& path, std::int64_t& seconds);
    // 不跟随最后一段链接，只接受磁盘普通文件。失败不改 out；调用方还须检查父目录边界。
    static bool regularFileInfo(const MaiFilePath& path, MaiRegularFileInfo& out);

    // ── 读写 ────────────────────────────────────────────────────
    // 整个读进来。maxBytes 为 0 表示不限；
    // 超过就读到上限为止并把 truncated 置位——工具层要据此告诉模型"你看到的不是全部"。
    static MaiError readFile(const MaiFilePath& path, std::string& contents,
                             std::uint64_t maxBytes = 0, bool* truncated = nullptr);

    // 从原始字节偏移读取有界文件块，参考 Chromium File::Read(offset, ...)。
    // maxBytes 为 1..1MiB；offset 必须能由有符号 64 位文件偏移表示。偏移越过 EOF 返回空串。
    // 不跟随末段符号链接，只接受磁盘普通文件；读取前后核对身份、大小及修改/变更时间。
    // 文件发生变化则返回 InvalidInput，失败清空 contents，不交付混合版本或半截结果。
    // 本操作阻塞线程；调用方仍须验证父目录授权边界，并为多块读取保留自己的文件快照。
    static MaiError readFileRange(const MaiFilePath& path, std::uint64_t offset,
                                  std::uint64_t maxBytes, std::string& contents);

    // 覆盖写。父目录不会自动创建，调用方自己先 createDirectories。
    static MaiError writeFile(const MaiFilePath& path, const std::string& contents);

    // 原子地创建空文件。已存在时失败，绝不覆盖；父目录由调用方先创建。
    static MaiError createEmptyFile(const MaiFilePath& path);

    // 删掉一个文件。**只删文件，不删目录**：递归删目录是另一种危险程度的操作，
    // 要的时候单独加一个名字里说清楚的函数，不要让这个悄悄兼职。
    // 文件本来就不存在时返回 NotFound，而不是当成成功——调用方据此能分清
    // 「删掉了」和「压根没有」。
    static MaiError removeFile(const MaiFilePath& path);

    // 将完整临时文件交付到同一文件系统内的新路径，不替换现有目标。
    // 失败时源路径仍由调用方持有；目标父目录必须已存在。本操作会阻塞线程。
    // Android 宿主通过 Java 文件接口完成此操作，适用于 App 私有工作区内、
    // 所有发布者都使用同一宿主桥接的场景；不保证与绕过桥接的写入者跨进程无覆盖竞争。
    static MaiError publishNewFile(const MaiFilePath& source, const MaiFilePath& destination);

    // ── 目录 ────────────────────────────────────────────────────
    // 逐级创建，已存在不算错。
    static MaiError createDirectories(const MaiFilePath& path);

    // 递归遍历。
    //
    // **不跟进目录符号链接**。跟进的话，
    // 一个指回上级的链接就能让遍历无限转下去——chromium 在 POSIX 上是靠记 (dev, ino) 防这个，
    // 我们直接不跟进，既简单又和 std::filesystem 的默认行为一致，
    // 换过来不会改变现有语义。
    //
    // 遍历顺序不保证，和 chromium 的 FileEnumerator 一样。glob 要排序的话自己排。
    // 与 Chromium FileEnumerator 的错误状态一样，onError 允许调用方区分真正空目录与遍历失败。
    // 目录打开、枚举和条目元数据失败时同步报告失败路径及错误，然后继续其它目录；
    // 正常空目录不报告错误。回调不被保留，省略时维持原先尽力遍历的行为。
    static void walk(const MaiFilePath& root,
                     const std::function<MaiWalkAction(const MaiFileEntry&)>& visit,
                     const std::function<void(const MaiFilePath&, const MaiError&)>& onError = {});

    // ── 其它 ────────────────────────────────────────────────────
    // 解析成真实路径：消掉 "."/".."，并且**解析符号链接**。
    // 路径不存在时尽力而为——把存在的那一段解析掉，剩下的做词法规范化。失败返回空路径。
    static MaiFilePath resolve(const MaiFilePath& path);

    // 系统临时目录。测试用。
    static MaiFilePath temporaryDirectory();

    // 递归删除。测试收尾用；删不掉不报错，因为那时候已经没人在意了。
    static void removeRecursively(const MaiFilePath& path);
};
