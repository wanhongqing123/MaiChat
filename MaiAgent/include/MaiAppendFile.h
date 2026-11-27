// 文件作用：提供日志权威文件的原生追加、持久化和跨写者互斥。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "MaiError.h"
#include "MaiFilePath.h"

// 仅追加的普通文件；不创建父目录，不跟随最终路径的链接或 Windows reparse point。
// 路径使用平台原生编码，POSIX 新文件权限为 0600（仍受 umask 限制）。
// 父目录授权和文件替换防护由调用方负责；Windows 句柄不共享删除。
// 单个实例仅供一个线程同步使用，不可复制。不同实例可在线程或进程间争用写锁。
// 所有 I/O 都会阻塞；不得在事件总线禁止阻塞的回调中调用。
// 协作写者必须使用同一写锁；本类型无法防御绕开接口的截断或任意字节覆盖。
// 析构释放本实例的写锁并关闭句柄，不自动 flush；调用方自行决定提交持久化边界。
class MaiAppendFile {
public:
    static MaiResult<std::unique_ptr<MaiAppendFile>> open(MaiFilePath path);
    ~MaiAppendFile();
    MaiAppendFile(const MaiAppendFile&) = delete;
    MaiAppendFile& operator=(const MaiAppendFile&) = delete;

    // 阻塞直到取得排他写锁；对同一实例重复加锁返回 Busy，不能递归。
    // Windows/Linux OFD 锁覆盖保留的极高偏移；Apple/BSD 及不支持 OFD 的旧 Linux
    // 使用原生 flock advisory 锁。两者均允许普通读句柄同时读取不可变的旧字节。
    // OFD/flock 按打开文件描述绑定；关闭其它独立读句柄不会撤销写锁。
    // POSIX 锁为协作锁，所有写者必须使用相同平台锁协议；不支持跨 fork 复用本实例。
    MaiError lockWriter();
    // 未持锁时为空操作；仅在当前实例的同步使用线程中调用。
    void unlockWriter();

    // 以下方法要求本实例已成功 lockWriter，否则返回 InvalidInput。
    MaiResult<std::uint64_t> size() const;
    // 单次读取 1..1MiB；偏移越过 EOF 成功返回空串，失败清空 out。
    // 大文件由调用方逐块读取，偏移和范围必须能由有符号 64 位偏移表示。
    // 若旧 POSIX 平台仍使用 32 位 off_t，则进一步限制到原生偏移范围。
    MaiError readAt(std::uint64_t offset, std::uint64_t maxBytes, std::string& out);
    // 返回追加前的文件长度；允许空串。绝不覆盖已有字节，不自动 flush。
    // 系统写入中途失败可能留下部分尾部，调用方必须按日志恢复规则处理，不能假设回滚。
    MaiResult<std::uint64_t> append(const std::string& bytes);
    // 将系统缓存交给原生 FlushFileBuffers/fsync；新文件的父目录持久化不在本契约内。
    MaiError flush();

private:
    struct State;
    explicit MaiAppendFile(std::unique_ptr<State> state);
    std::unique_ptr<State> mState;
};
