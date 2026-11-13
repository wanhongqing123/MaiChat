#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "MaiError.h"

// 固定的正式本地库布局，不接受调用方提供 SQL、表名或列名。
enum class MaiIMHistoryProfile { Desktop, Android, IOS };
enum class MaiIMHistoryDirection { Incoming, Outgoing, Unknown };

struct MaiIMHistorySnapshotOptions {
    MaiIMHistoryProfile profile = MaiIMHistoryProfile::Desktop;
    std::string databasePath;  // 宿主验证过的当前账号数据库绝对 UTF-8 路径。
    std::string ownerUserId;
    std::string peerUserId;
    std::uint64_t sdkAppId = 0;  // iOS 必填；旧 Android/Desktop 不声称按 SDK 分库。
    std::size_t maxRecords = 200;
    std::size_t maxPageBytes = 384 * 1024;
    std::size_t maxTextBytes = 256 * 1024;
    std::uint64_t pageTimeoutMillis = 5000;
    std::uint64_t lifetimeTimeoutMillis = 300000;
    // reader 持有共享所有权；取消可从任意线程设置，不捕获宿主或后端指针。
    std::shared_ptr<std::atomic<bool>> canceled;
};

struct MaiIMHistoryMedia {
    std::string kind;  // image / voice / file / video
    std::string name;
    std::string mimeType;
    std::string localPath;
    std::string coverPath;
    std::uint64_t sizeBytes = 0;
    std::uint64_t durationMillis = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // 仅表示本地引用元数据结构有效；不证明文件存在、可解码或允许向外发送。
    bool hasLocalReference = false;
    bool metadataUnavailable = false;
};

struct MaiIMHistoryQuote {
    std::string messageId;
    std::string senderId;
    std::string digest;
    std::string kind;
};

struct MaiIMHistoryRecord {
    std::string rowId;
    std::optional<std::string> remoteId;  // 仅使用真实独立 remote_id 列，不猜测旧主键。
    std::string fromUserId;
    std::string toUserId;
    MaiIMHistoryDirection direction = MaiIMHistoryDirection::Unknown;
    std::string status;  // 保留存储取值；Desktop 的整型状态转为十进制字符串。
    std::int64_t createdAtMillis = 0;
    std::string text;
    std::string kind;
    std::vector<MaiIMHistoryMedia> media;
    std::optional<MaiIMHistoryQuote> quote;
    bool captionAbove = false;
    // 损坏或超出可选字段预算时仍保留该行，只提供固定字段名，不返回原始坏 JSON。
    std::vector<std::string> unavailableMetadata;
};

struct MaiIMHistorySource {
    MaiIMHistoryProfile profile = MaiIMHistoryProfile::Desktop;
    std::string canonicalDatabasePath;
    std::string ownerUserId;
    std::string peerUserId;
    std::optional<std::uint64_t> sdkAppId;
    std::string provenance;  // owner_sdk / owner_only_legacy
    std::string coverage;    // local_persisted；不代表完整云端会话。
};

struct MaiIMHistoryPage {
    std::uint64_t sequence = 0;  // 首次成功读取为 0；失败不会产生可交付页面。
    std::vector<MaiIMHistoryRecord> records;
    std::size_t serializedBytes = 0;  // 保守的 UTF-8 JSON 字符转义后记录预算。
    bool hasMore = false;             // false 是在固定快照内实际读到 EOF，不是总记录截断。
};

// 同一实例只允许一个 I/O 调用者串行 open/read/析构；只有 cancel 可跨线程。
// open 使用单个 READONLY/NOMUTEX SQLite 连接，query_only + BEGIN + 首次 SELECT
// 实际固定 WAL 快照。read 以升序 (毫秒时间,id) 键集读取；全量不设隐含记录上限。
// 不读取、解码、复制或下载任何媒体；不输出审批字段、签名或控制权限。
// Windows 额外保留 Core 无共享删除的文件句柄；POSIX 核对 HAS_MOVED 与规范路径。
// 不比较 mtime/大小，普通 WAL 写入和 checkpoint 不会被误报为路径替换。
// 缺库为 NotFound，取消为 Canceled，期限为 Busy，路径替换为 InvalidInput，
// SQL/存储损坏为 Internal；均显式失败，不能当作成功空页。失败后实例不可继续读取。
class MaiIMHistorySnapshot final {
public:
    static MaiResult<std::unique_ptr<MaiIMHistorySnapshot>> open(
        const MaiIMHistorySnapshotOptions& options);
    ~MaiIMHistorySnapshot();
    MaiIMHistorySnapshot(const MaiIMHistorySnapshot&) = delete;
    MaiIMHistorySnapshot& operator=(const MaiIMHistorySnapshot&) = delete;

    MaiResult<MaiIMHistoryPage> read();
    void cancel();
    const MaiIMHistorySource& source() const;

private:
    struct Impl;
    explicit MaiIMHistorySnapshot(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> mImpl;
};
