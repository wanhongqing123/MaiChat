// 文件作用：按 Codex 外壳追加会话权威记录，并提供字节定位和索引重放。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "MaiError.h"
#include "MaiFilePath.h"
#include "MaiTime.h"

// 定位整条 UTF-8 记录：byteOffset 指向左花括号，byteLength 包含结尾的单个 LF。
// ordinal 来自文件内记录，不是数据库自增值；所有值均属于同一个 rollout 文件。
struct MaiRolloutLocation {
    std::uint64_t ordinal = 0;
    std::uint64_t byteOffset = 0;
    std::uint64_t byteLength = 0;
};

struct MaiRolloutCursor {
    std::uint64_t byteOffset = 0;
    std::uint64_t nextOrdinal = 0;
};

// payload 是保留未知字段的完整序列化对象；上层按 type 解码，不从显示文字猜协议状态。
struct MaiRolloutRecord {
    MaiRolloutLocation location;
    std::string timestamp;
    std::string type;
    std::string payload;
};

// 追加/读取原始记录，不实现 SQLite 投影或模型上下文裁剪。每条完整记录固定外壳
// timestamp/ordinal/type/payload；不保存实时文字 delta，不覆盖任何已提交字节。
// 除 sourcePath 外所有入口会阻塞，必须在允许文件 I/O 的线程调用。同一实例可跨线程串行使用；
// 不同实例/进程依赖原生写锁分配 ordinal，数据库不能反向分配或修复序号。
// create/open 不修剪损坏尾部；损坏或不完整记录明确失败，原文件保持不变。
class MaiRolloutLog {
public:
    // 仅创建新的权威日志；父目录自动创建，已有非空日志拒绝覆盖。
    // metadata 必须是含非空 id/session_id 的完整对象；调用方负责其业务字段来源。
    static MaiResult<std::unique_ptr<MaiRolloutLog>> create(MaiFilePath path,
                                                            const std::string& metadata,
                                                            MaiMillis timestamp);
    // 打开并验证已有日志及单调序号，不在不存在的路径创建文件。
    static MaiResult<std::unique_ptr<MaiRolloutLog>> open(MaiFilePath path);
    ~MaiRolloutLog();
    MaiRolloutLog(const MaiRolloutLog&) = delete;
    MaiRolloutLog& operator=(const MaiRolloutLog&) = delete;

    // 创建或打开时固定的绝对源路径，父目录规范化但不跟随最终文件的符号链接。
    // 返回借用引用，生命周期与本实例相同；创建后不变，可跨线程读取，不加锁且无文件 I/O。
    const MaiFilePath& sourcePath() const;

    // 追加一个对象并 flush 后返回位置。支持用户指定的八种顶层记录类型；session_meta
    // 只由 create 写入一次。失败不会推进返回游标，系统失败可能遗留未提交尾部。
    MaiResult<MaiRolloutLocation> append(const std::string& type, const std::string& payload,
                                         MaiMillis timestamp);
    // 仅供正式存储的暂存事务：追加完整记录并分配真实位置，但不执行持久化 flush。
    // 返回成功只说明本进程可读取字节，不能发出完成事件或提交引用它的 SQLite 元数据。
    // 调用者必须在权威元数据提交前对所有触及日志调用 confirmDurable；失败保留源尾部，
    // 不得据此声称回滚原始字节。其它写者仍可看见完整暂存记录，业务可见性由提交账本决定。
    MaiResult<MaiRolloutLocation> appendBuffered(const std::string& type,
                                                 const std::string& payload, MaiMillis timestamp);
    // 在本地互斥锁和原生写锁内重新验证当前尾部，并再次 flush 已有完整记录。
    // 供追加 flush 失败后的不确定提交确认使用；成功才可把已有记录视为持久化。
    // 不追加字节，不分配 ordinal；损坏、不完整或被截断的日志失败且不自动修剪。
    MaiError confirmDurable();
    // 精确读取完整历史记录；核对长度、换行、对象外壳及 ordinal，不加载其它会话。
    // 该入口不需要写句柄，SQLite 投影可在普通读取时直接使用。
    static MaiResult<MaiRolloutRecord> read(const MaiFilePath& path,
                                            const MaiRolloutLocation& location);
    // 仅按字节范围读取，返回源文件实际 ordinal，不核对数据库中的预期 ordinal。
    // 与 read 共用有界 I/O 和完整记录校验；byteLength 包含 LF，单条最多 64 MiB。
    // 调用方负责将源记录的会话、领域对象身份与索引关联；不持有写锁，不读取其它会话。
    static MaiResult<MaiRolloutRecord> readAt(const MaiFilePath& path, std::uint64_t byteOffset,
                                              std::uint64_t byteLength);
    // 在一个不可变前缀快照内从游标顺序读取。成功返回源文件实际下一游标。
    // apply 同步借用记录，不可在其中对同一实例重入；投影事务原子性由调用方负责。
    // apply 或源验证失败返回错误，调用方不得把尚未提交的索引标为成功。
    MaiResult<MaiRolloutCursor> replay(
        MaiRolloutCursor cursor,
        const std::function<MaiError(const MaiRolloutRecord&)>& apply) const;

private:
    MaiResult<MaiRolloutLocation> appendInternal(const std::string& type,
                                                 const std::string& payload, MaiMillis timestamp,
                                                 bool durable);
    struct State;
    explicit MaiRolloutLog(std::unique_ptr<State> state);
    std::unique_ptr<State> mState;
};
