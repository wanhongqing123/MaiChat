// 文件作用：实现会话 JSONL 的只追加记录与有界读取。
#include "MaiRolloutLog.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <unordered_set>
#include <utility>
#include <vector>

#include <json.hpp>

#include "MaiAppendFile.h"
#include "MaiBlockingCheck.h"
#include "MaiFileSystem.h"
#include "MaiRolloutPath.h"

namespace {

using Json = nlohmann::ordered_json;
using ReadChunk = std::function<MaiError(std::uint64_t, std::uint64_t, std::string&)>;
using ApplyRecord = std::function<MaiError(const MaiRolloutRecord&)>;
constexpr std::uint64_t kReadChunkBytes = 1024 * 1024;
constexpr std::uint64_t kRecordBytesLimit = 64 * kReadChunkBytes;
constexpr std::uint64_t kOffsetLimit =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) - 1;

MaiError invalidSource(const char* message) {
    return {MaiErrorCode::InvalidInput, message};
}

MaiResult<MaiFilePath> absoluteSourcePath(const MaiFilePath& path) {
    if (path.isEmpty() ||
        path.value().find(MaiFilePath::CharType{}) != MaiFilePath::StringType::npos)
        return invalidSource("invalid rollout source path");
    // 只解析父目录，再拼回原始末段。不能 resolve 整个路径，否则会先跟随最终符号链接，
    // 使后续原生 O_NOFOLLOW / FILE_FLAG_OPEN_REPARSE_POINT 检查失去意义。
    const auto parent = MaiFileSystem::resolve(path.dirName().isEmpty() ? MaiFilePath::fromUtf8(".")
                                                                        : path.dirName());
    if (!parent.isAbsolute() || path.baseName().isEmpty())
        return invalidSource("cannot resolve an absolute rollout parent directory");
    return parent.append(path.baseName());
}

bool isSupportedType(const std::string& type) {
    return type == "session_meta" || type == "turn_context" || type == "world_state" ||
           type == "response_item" || type == "event_msg" || type == "token_usage_record" ||
           type == "compacted" || type == "inter_agent_communication_metadata";
}

// 禁止重复键，避免解析器覆盖未知 payload 字段后把有损结果当成完整原文。
// 回调始终保留字段，等解析结束统一失败；不能通过过滤回调悄悄修复输入。
MaiResult<Json> parseObject(const std::string& text) {
    if (text.size() > kRecordBytesLimit)
        return invalidSource("rollout record exceeds the 64 MiB limit");
    bool duplicate = false;
    std::vector<std::unordered_set<std::string>> keys;
    const auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
        const auto level = static_cast<std::size_t>(depth);
        if (event == Json::parse_event_t::object_start) {
            if (keys.size() <= level + 1) keys.resize(level + 2);
            keys[level + 1].clear();
        } else if (event == Json::parse_event_t::key) {
            if (keys.size() <= level) keys.resize(level + 1);
            if (!keys[level].insert(value.get<std::string>()).second) duplicate = true;
        }
        return true;
    };
    auto object = Json::parse(text, callback, false);
    if (object.is_discarded() || !object.is_object() || duplicate)
        return invalidSource("rollout JSON must be an object without duplicate keys");
    return object;
}

bool isValidTimestamp(const std::string& stamp) {
    if (stamp.size() != 24 || stamp[4] != '-' || stamp[7] != '-' || stamp[10] != 'T' ||
        stamp[13] != ':' || stamp[16] != ':' || stamp[19] != '.' || stamp[23] != 'Z')
        return false;
    for (std::size_t index = 0; index < stamp.size(); ++index) {
        if (index == 4 || index == 7 || index == 10 || index == 13 || index == 16 || index == 19 ||
            index == 23)
            continue;
        if (stamp[index] < '0' || stamp[index] > '9') return false;
    }
    const auto number = [&](std::size_t offset, std::size_t count) {
        unsigned value = 0;
        for (std::size_t index = offset; index < offset + count; ++index)
            value = value * 10 + static_cast<unsigned>(stamp[index] - '0');
        return value;
    };
    const unsigned year = number(0, 4);
    const unsigned month = number(5, 2);
    const unsigned day = number(8, 2);
    constexpr unsigned days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year < 1970 || month < 1 || month > 12 || day < 1) return false;
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    return day <= days[month - 1] + (month == 2 && leap ? 1u : 0u) && number(11, 2) < 24 &&
           number(14, 2) < 60 && number(17, 2) < 60;
}

bool hasMetadataIdentity(const Json& payload) {
    for (const char* field : {"id", "session_id"}) {
        const auto value = payload.find(field);
        if (value == payload.end() || !value->is_string() ||
            value->get_ref<const std::string&>().empty())
            return false;
    }
    return true;
}

MaiResult<MaiRolloutRecord> decodeRecord(const std::string& bytes, std::uint64_t offset) {
    if (bytes.empty() || bytes.size() > kRecordBytesLimit || bytes.back() != '\n' ||
        bytes.find('\n') != bytes.size() - 1 || bytes.find('\r') != std::string::npos)
        return invalidSource("rollout record must end with exactly one LF");
    auto parsed = parseObject(bytes);
    if (!parsed) return parsed.error();
    const auto& envelope = parsed.value();
    // 外壳字段及顺序固定；payload 内的未知字段仍完整保留，不按业务结构重新拼装。
    const char* fields[] = {"timestamp", "ordinal", "type", "payload"};
    if (envelope.size() != 4) return invalidSource("invalid rollout envelope fields");
    std::size_t index = 0;
    for (auto field = envelope.begin(); field != envelope.end(); ++field)
        if (field.key() != fields[index++]) return invalidSource("invalid rollout envelope order");
    if (!envelope["timestamp"].is_string() || !envelope["type"].is_string() ||
        !envelope["payload"].is_object() ||
        !(envelope["ordinal"].is_number_unsigned() ||
          (envelope["ordinal"].is_number_integer() &&
           envelope["ordinal"].get<std::int64_t>() >= 0)))
        return invalidSource("invalid rollout envelope value types");
    MaiRolloutRecord record;
    record.timestamp = envelope["timestamp"].get<std::string>();
    record.type = envelope["type"].get<std::string>();
    record.location = {envelope["ordinal"].get<std::uint64_t>(), offset,
                       static_cast<std::uint64_t>(bytes.size())};
    if (!isValidTimestamp(record.timestamp) || !isSupportedType(record.type))
        return invalidSource("invalid rollout timestamp or record type");
    if (record.type == "session_meta") {
        if (offset != 0 || record.location.ordinal != 0 ||
            !hasMetadataIdentity(envelope["payload"]))
            return invalidSource("session metadata must be the first record with an identity");
    } else if (offset == 0 || record.location.ordinal == 0) {
        return invalidSource("rollout must begin with session metadata at ordinal zero");
    }
    if (record.location.ordinal == std::numeric_limits<std::uint64_t>::max())
        return invalidSource("rollout ordinal has no representable next cursor");
    record.payload = envelope["payload"].dump();
    return record;
}

MaiResult<std::string> encodeRecord(const std::string& type, const std::string& payload,
                                    MaiMillis timestamp, std::uint64_t ordinal) {
    if (!isSupportedType(type)) return invalidSource("unsupported rollout record type");
    auto stamp = MaiRolloutPath::formatTimestamp(timestamp);
    if (!stamp) return stamp.error();
    auto object = parseObject(payload);
    if (!object) return object.error();
    if (type == "session_meta" && !hasMetadataIdentity(object.value()))
        return invalidSource("session metadata requires nonempty id and session_id");
    if (ordinal == std::numeric_limits<std::uint64_t>::max())
        return invalidSource("rollout ordinal would overflow the next cursor");
    Json envelope = Json::object();
    envelope["timestamp"] = std::move(stamp.value());
    envelope["ordinal"] = ordinal;
    envelope["type"] = type;
    envelope["payload"] = std::move(object.value());
    auto bytes = envelope.dump();
    if (bytes.size() >= kRecordBytesLimit)
        return invalidSource("rollout record exceeds the 64 MiB limit");
    bytes.push_back('\n');
    return bytes;
}

// 按原始字节扫描，每次只取至多 1 MiB；单条记录可跨块，但不能超过 64 MiB。
// 未到完整 LF 不推进游标；任何失败均不返回可以误提交的成功游标。
MaiResult<MaiRolloutCursor> scanRecords(const ReadChunk& read, MaiRolloutCursor cursor,
                                        std::uint64_t end, const ApplyRecord& apply = {}) {
    if (end > kOffsetLimit || cursor.byteOffset > end)
        return invalidSource("invalid rollout scan range");
    std::uint64_t position = cursor.byteOffset;
    std::string pending;
    while (position < end) {
        std::string chunk;
        const auto error = read(position, std::min(kReadChunkBytes, end - position), chunk);
        if (error) return error;
        if (chunk.empty() || chunk.size() > end - position)
            return invalidSource("rollout ended before the committed snapshot");
        position += static_cast<std::uint64_t>(chunk.size());
        std::size_t begin = 0;
        while (begin < chunk.size()) {
            const auto newline = chunk.find('\n', begin);
            const auto finish = newline == std::string::npos ? chunk.size() : newline + 1;
            const auto count = finish - begin;
            if (count > kRecordBytesLimit - pending.size())
                return invalidSource("rollout record exceeds the 64 MiB limit");
            pending.append(chunk, begin, count);
            begin = finish;
            if (newline == std::string::npos) continue;
            auto record = decodeRecord(pending, cursor.byteOffset);
            if (!record) return record.error();
            if (record.value().location.ordinal < cursor.nextOrdinal)
                return invalidSource("rollout ordinal is duplicated or regresses");
            if (apply) {
                const auto applied = apply(record.value());
                if (applied) return applied;
            }
            cursor.byteOffset += static_cast<std::uint64_t>(pending.size());
            cursor.nextOrdinal = record.value().location.ordinal + 1;
            pending.clear();
        }
    }
    if (!pending.empty()) return invalidSource("rollout has an incomplete trailing record");
    return cursor;
}

class WriterLock {
public:
    explicit WriterLock(MaiAppendFile& file) : mFile(file), mError(file.lockWriter()) {}
    ~WriterLock() {
        release();
    }
    const MaiError& error() const {
        return mError;
    }
    void release() {
        if (!mError && !mReleased) {
            mFile.unlockWriter();
            mReleased = true;
        }
    }

private:
    MaiAppendFile& mFile;
    MaiError mError;
    bool mReleased = false;
};

}  // namespace

struct MaiRolloutLog::State {
    MaiFilePath path;
    std::unique_ptr<MaiAppendFile> file;
    MaiRolloutCursor cursor;
    mutable std::mutex mutex;

    MaiError refresh();
};

MaiError MaiRolloutLog::State::refresh() {
    const auto length = file->size();
    if (!length) return length.error();
    if (length.value() < cursor.byteOffset)
        return invalidSource("rollout was truncated after a verified prefix");
    const auto scanned =
        scanRecords([this](std::uint64_t offset, std::uint64_t count,
                           std::string& bytes) { return file->readAt(offset, count, bytes); },
                    cursor, length.value());
    if (!scanned) return scanned.error();
    if (scanned.value().byteOffset == 0) return invalidSource("rollout has no session metadata");
    cursor = scanned.value();
    return {};
}

MaiRolloutLog::MaiRolloutLog(std::unique_ptr<State> state) : mState(std::move(state)) {}
MaiRolloutLog::~MaiRolloutLog() = default;

const MaiFilePath& MaiRolloutLog::sourcePath() const {
    return mState->path;
}

MaiResult<std::unique_ptr<MaiRolloutLog>> MaiRolloutLog::create(MaiFilePath path,
                                                                const std::string& metadata,
                                                                MaiMillis timestamp) {
    maiAssertBlockingAllowed("MaiRolloutLog::create");
    auto bytes = encodeRecord("session_meta", metadata, timestamp, 0);
    if (!bytes) return bytes.error();
    if (path.isEmpty()) return invalidSource("empty rollout path");
    if (!path.dirName().isEmpty()) {
        const auto error = MaiFileSystem::createDirectories(path.dirName());
        if (error) return error;
    }
    auto absolute = absoluteSourcePath(path);
    if (!absolute) return absolute.error();
    path = std::move(absolute.value());
    auto file = MaiAppendFile::open(path);
    if (!file) return file.error();
    auto state = std::make_unique<State>();
    state->path = std::move(path);
    state->file = std::move(file.value());
    WriterLock writer(*state->file);
    if (writer.error()) return writer.error();
    const auto length = state->file->size();
    if (!length) return length.error();
    if (length.value() != 0) return invalidSource("cannot create over a nonempty rollout");
    const auto added = state->file->append(bytes.value());
    if (!added) return added.error();
    const auto flushed = state->file->flush();
    if (flushed) return flushed;
    state->cursor = {static_cast<std::uint64_t>(bytes.value().size()), 1};
    writer.release();
    return std::unique_ptr<MaiRolloutLog>(new MaiRolloutLog(std::move(state)));
}

MaiResult<std::unique_ptr<MaiRolloutLog>> MaiRolloutLog::open(MaiFilePath path) {
    maiAssertBlockingAllowed("MaiRolloutLog::open");
    // 先使用不创建文件的原生读入口；普通缺失路径在追加句柄打开之前就失败。
    // Windows 读句柄同时禁止末段重命名。父目录及 POSIX 外部替换仍由宿主约束。
    auto existing = MaiFileReadHandle::open(path);
    if (!existing) return existing.error();
    auto absolute = absoluteSourcePath(path);
    if (!absolute) return absolute.error();
    path = std::move(absolute.value());
    auto file = MaiAppendFile::open(path);
    if (!file) return file.error();
    auto state = std::make_unique<State>();
    state->path = std::move(path);
    state->file = std::move(file.value());
    WriterLock writer(*state->file);
    if (writer.error()) return writer.error();
    const auto error = state->refresh();
    if (error) return error;
    writer.release();
    return std::unique_ptr<MaiRolloutLog>(new MaiRolloutLog(std::move(state)));
}

MaiResult<MaiRolloutLocation> MaiRolloutLog::append(const std::string& type,
                                                    const std::string& payload,
                                                    MaiMillis timestamp) {
    return appendInternal(type, payload, timestamp, true);
}
MaiResult<MaiRolloutLocation> MaiRolloutLog::appendBuffered(const std::string& type,
                                                            const std::string& payload,
                                                            MaiMillis timestamp) {
    return appendInternal(type, payload, timestamp, false);
}
MaiResult<MaiRolloutLocation> MaiRolloutLog::appendInternal(const std::string& type,
                                                            const std::string& payload,
                                                            MaiMillis timestamp, bool durable) {
    maiAssertBlockingAllowed("MaiRolloutLog::append");
    if (type == "session_meta") return invalidSource("session metadata may only be created once");
    std::lock_guard<std::mutex> local(mState->mutex);
    WriterLock writer(*mState->file);
    if (writer.error()) return writer.error();
    const auto refreshed = mState->refresh();
    if (refreshed) return refreshed;
    auto bytes = encodeRecord(type, payload, timestamp, mState->cursor.nextOrdinal);
    if (!bytes) return bytes.error();
    if (bytes.value().size() > kOffsetLimit - mState->cursor.byteOffset)
        return invalidSource("rollout append would overflow the file offset");
    const auto offset = mState->file->append(bytes.value());
    if (!offset) return offset.error();
    if (offset.value() != mState->cursor.byteOffset)
        return invalidSource("rollout changed outside the writer protocol");
    if (durable) {
        const auto flushed = mState->file->flush();
        if (flushed) return flushed;
    }
    MaiRolloutLocation location{mState->cursor.nextOrdinal, offset.value(),
                                static_cast<std::uint64_t>(bytes.value().size())};
    mState->cursor = {location.byteOffset + location.byteLength, location.ordinal + 1};
    return location;
}

MaiError MaiRolloutLog::confirmDurable() {
    maiAssertBlockingAllowed("MaiRolloutLog::confirmDurable");
    std::lock_guard<std::mutex> local(mState->mutex);
    WriterLock writer(*mState->file);
    if (writer.error()) return writer.error();
    // 完整但尚未确认持久化的尾部不能仅凭可读就被当成提交成功；
    // 先在相同写锁内验证源文件，再重试原生 flush，期间不分配序号或追加字节。
    const auto refreshed = mState->refresh();
    if (refreshed) return refreshed;
    return mState->file->flush();
}

MaiResult<MaiRolloutRecord> MaiRolloutLog::read(const MaiFilePath& path,
                                                const MaiRolloutLocation& location) {
    auto record = readAt(path, location.byteOffset, location.byteLength);
    if (!record) return record.error();
    if (record.value().location.ordinal != location.ordinal)
        return invalidSource("rollout location ordinal does not match source");
    return record;
}

MaiResult<MaiRolloutRecord> MaiRolloutLog::readAt(const MaiFilePath& path, std::uint64_t byteOffset,
                                                  std::uint64_t byteLength) {
    maiAssertBlockingAllowed("MaiRolloutLog::readAt");
    if (byteLength == 0 || byteLength > kRecordBytesLimit || byteOffset > kOffsetLimit - byteLength)
        return invalidSource("invalid rollout record byte location");
    auto reader = MaiFileReadHandle::open(path);
    if (!reader) return reader.error();
    if (byteOffset != 0) {
        std::string previous;
        const auto error = reader.value()->readAt(byteOffset - 1, 1, previous);
        if (error) return error;
        if (previous != "\n") return invalidSource("rollout location is not a record boundary");
    }
    std::string bytes;
    while (bytes.size() < byteLength) {
        std::string chunk;
        const auto error =
            reader.value()->readAt(byteOffset + static_cast<std::uint64_t>(bytes.size()),
                                   std::min(kReadChunkBytes, byteLength - bytes.size()), chunk);
        if (error) return error;
        if (chunk.empty()) return invalidSource("rollout location extends past EOF");
        bytes += chunk;
    }
    return decodeRecord(bytes, byteOffset);
}

MaiResult<MaiRolloutCursor> MaiRolloutLog::replay(
    MaiRolloutCursor cursor, const std::function<MaiError(const MaiRolloutRecord&)>& apply) const {
    maiAssertBlockingAllowed("MaiRolloutLog::replay");
    if (!apply) return invalidSource("rollout replay requires an apply callback");
    std::lock_guard<std::mutex> local(mState->mutex);
    WriterLock writer(*mState->file);
    if (writer.error()) return writer.error();
    const auto refreshed = mState->refresh();
    if (refreshed) return refreshed;
    const auto snapshotEnd = mState->cursor.byteOffset;
    auto reader = MaiFileReadHandle::open(mState->path);
    if (!reader) return reader.error();
    // 源文件已在写锁内验证到完整 EOF。提前释放 OS 锁，投影回调不会阻塞其它实例追加。
    // 独立读句柄只消费不可变前缀；不能使用会因正常追加而失效的全文件 mtime 快照。
    writer.release();
    const ReadChunk read = [&](std::uint64_t offset, std::uint64_t count, std::string& bytes) {
        return reader.value()->readAt(offset, count, bytes);
    };
    if (cursor.byteOffset > snapshotEnd)
        return invalidSource("rollout replay cursor is beyond the committed snapshot");
    const auto prefix = scanRecords(read, {}, cursor.byteOffset);
    if (!prefix) return prefix.error();
    if (prefix.value().nextOrdinal != cursor.nextOrdinal)
        return invalidSource("rollout replay cursor ordinal does not match source prefix");
    return scanRecords(read, cursor, snapshotEnd, apply);
}
