#include "MaiIMHistorySnapshot.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <utility>
#include <json.hpp>
#include <sqlite3.h>
#include "MaiFilePath.h"
#include "MaiFileSystem.h"

namespace {
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;
constexpr std::size_t kPathBytes = 8192;
constexpr std::size_t kJsonBytes = 32768;
bool identifier(const std::string& value) {
    return !value.empty() && value.size() <= 512 && value.find('\0') == std::string::npos;
}
bool reference(const std::string& path) {
    if (path.empty() || path.find("://") != std::string::npos) return false;
    for (unsigned char ch : path)
        if (ch < 32 || ch == 127) return false;
    for (const auto& part : MaiFilePath::fromUtf8(path).components())
        if (part == MaiFilePath::fromUtf8("..").value()) return false;
    return true;
}
void unavailable(MaiIMHistoryRecord& row, const std::string& name) {
    if (std::find(row.unavailableMetadata.begin(), row.unavailableMetadata.end(), name) ==
        row.unavailableMetadata.end())
        row.unavailableMetadata.push_back(name);
}
struct Row {
    sqlite3_stmt* statement;
    std::map<std::string, int> columns;
    explicit Row(sqlite3_stmt* value) : statement(value) {
        for (int i = 0; i < sqlite3_column_count(value); ++i) columns.emplace(sqlite3_column_name(value, i), i);
    }
    int index(const char* name) const {
        auto i = columns.find(name);
        return i == columns.end() ? -1 : i->second;
    }
    int type(const char* name) const {
        int i = index(name);
        return i < 0 ? SQLITE_NULL : sqlite3_column_type(statement, i);
    }
    std::string text(const char* name, std::size_t maximum, bool& valid, bool numeric = false,
                     bool allowNul = false) const {
        int i = index(name);
        if (i < 0 || type(name) == SQLITE_NULL) return {};
        if (type(name) != SQLITE_TEXT && !(numeric && type(name) == SQLITE_INTEGER)) {
            valid = false;
            return {};
        }
        int n = sqlite3_column_bytes(statement, i);
        if (n < 0 || std::size_t(n) > maximum) {
            valid = false;
            return {};
        }
        auto p = sqlite3_column_text(statement, i);
        if (!p && n) {
            valid = false;
            return {};
        }
        std::string value = p ? std::string(reinterpret_cast<const char*>(p), std::size_t(n)) : std::string();
        if (!allowNul && value.find('\0') != std::string::npos) {
            valid = false;
            return {};
        }
        // Optional bad UTF-8 becomes unavailable metadata here; it must not
        // invalidate an intact historical body at page serialization.
        try {
            (void)Json(value).dump();
        } catch (const std::exception&) {
            valid = false;
            return {};
        }
        return value;
    }
    std::uint64_t number(const char* name, std::uint64_t maximum, bool& valid) const {
        if (type(name) == SQLITE_NULL) return 0;
        if (type(name) != SQLITE_INTEGER) {
            valid = false;
            return 0;
        }
        auto v = sqlite3_column_int64(statement, index(name));
        if (v < 0 || std::uint64_t(v) > maximum) {
            valid = false;
            return 0;
        }
        return std::uint64_t(v);
    }
    bool present(std::initializer_list<const char*> names) const {
        for (auto name : names) {
            int i = index(name);
            if (i < 0 || type(name) == SQLITE_NULL) continue;
            if (type(name) == SQLITE_INTEGER && sqlite3_column_int64(statement, i) == 0) continue;
            if (type(name) == SQLITE_TEXT && sqlite3_column_bytes(statement, i) == 0) continue;
            return true;
        }
        return false;
    }
};
std::string jsonText(const Json& j, const char* key, std::size_t maximum, bool& valid) {
    auto i = j.find(key);
    if (i == j.end() || i->is_null()) return {};
    if (!i->is_string()) {
        valid = false;
        return {};
    }
    auto s = i->get<std::string>();
    if (s.size() > maximum || s.find('\0') != std::string::npos) {
        valid = false;
        return {};
    }
    return s;
}
std::uint64_t jsonNumber(const Json& j, const char* key, std::uint64_t maximum, bool& valid) {
    auto i = j.find(key);
    if (i == j.end() || i->is_null()) return 0;
    if (!i->is_number_integer() || (!i->is_number_unsigned() && i->get<std::int64_t>() < 0)) {
        valid = false;
        return 0;
    }
    auto n = i->get<std::uint64_t>();
    if (n > maximum) {
        valid = false;
        return 0;
    }
    return n;
}
void finishMedia(MaiIMHistoryMedia& media, bool valid) {
    if (!media.localPath.empty() && !reference(media.localPath)) {
        media.localPath.clear();
        valid = false;
    }
    if (!media.coverPath.empty() && !reference(media.coverPath)) {
        media.coverPath.clear();
        valid = false;
    }
    if (media.name.empty() && !media.localPath.empty())
        media.name = MaiFilePath::fromUtf8(media.localPath).baseName().toUtf8();
    media.hasLocalReference = !media.localPath.empty();
    media.metadataUnavailable = !valid || !media.hasLocalReference;
}
MaiIMHistoryMedia columnMedia(const Row& row, const char* kind, const char* path, const char* name, const char* mime,
                              const char* cover, const char* size, const char* seconds, const char* width,
                              const char* height) {
    MaiIMHistoryMedia m;
    m.kind = kind;
    bool good = true;
    m.localPath = row.text(path, kPathBytes, good);
    if (name) m.name = row.text(name, 1024, good);
    if (mime) m.mimeType = row.text(mime, 256, good);
    if (cover) m.coverPath = row.text(cover, kPathBytes, good);
    if (size) m.sizeBytes = row.number(size, std::numeric_limits<std::int64_t>::max(), good);
    if (seconds) m.durationMillis = 1000 * row.number(seconds, std::numeric_limits<std::uint64_t>::max() / 1000, good);
    if (width) m.width = std::uint32_t(row.number(width, std::numeric_limits<std::uint32_t>::max(), good));
    if (height) m.height = std::uint32_t(row.number(height, std::numeric_limits<std::uint32_t>::max(), good));
    finishMedia(m, good);
    return m;
}
MaiIMHistoryMedia jsonMedia(const Row& row, const char* column, const char* kind) {
    MaiIMHistoryMedia m;
    m.kind = kind;
    bool good = true;
    auto raw = row.text(column, kJsonBytes, good);
    auto j = good ? Json::parse(raw, nullptr, false) : Json();
    if (!good || !j.is_object()) {
        m.metadataUnavailable = true;
        return m;
    }
    m.localPath = jsonText(j, m.kind == "video" ? "localPath" : "localFilePath", kPathBytes, good);
    m.coverPath = jsonText(j, "coverPath", kPathBytes, good);
    m.name = jsonText(j, "fileName", 1024, good);
    m.mimeType = jsonText(j, "mimeType", 256, good);
    m.sizeBytes = jsonNumber(j, "sizeBytes", std::numeric_limits<std::int64_t>::max(), good);
    m.durationMillis = 1000 * jsonNumber(j, "durationSeconds", std::numeric_limits<std::uint64_t>::max() / 1000, good);
    m.width = std::uint32_t(jsonNumber(j, "width", std::numeric_limits<std::uint32_t>::max(), good));
    m.height = std::uint32_t(jsonNumber(j, "height", std::numeric_limits<std::uint32_t>::max(), good));
    finishMedia(m, good);
    return m;
}
MaiResult<MaiIMHistoryRecord> decode(sqlite3_stmt* statement, const MaiIMHistorySnapshotOptions& options) {
    Row source(statement);
    MaiIMHistoryRecord r;
    bool good = true;
    r.rowId = source.text("id", 512, good);
    r.fromUserId = source.text("from_id", 512, good);
    r.toUserId = source.text("to_id", 512, good);
    bool desktop = options.profile == MaiIMHistoryProfile::Desktop;
    r.status = source.text("status", 128, good, desktop);
    r.text = source.text("body", options.maxTextBytes, good, false, true);
    auto direction = source.text("direction", 128, good, desktop);
    if (!good || !identifier(r.rowId) || !identifier(r.fromUserId) || !identifier(r.toUserId))
        return {MaiErrorCode::InvalidInput, "History row has invalid or oversized required text"};
    if (source.type("created_ms") != SQLITE_INTEGER)
        return {MaiErrorCode::Internal, "History row has an invalid timestamp"};
    r.createdAtMillis = sqlite3_column_int64(statement, source.index("created_ms"));
    if (options.profile == MaiIMHistoryProfile::IOS) {
        double t = sqlite3_column_double(statement, source.index("source_created"));
        if ((source.type("source_created") != SQLITE_INTEGER && source.type("source_created") != SQLITE_FLOAT) ||
            !std::isfinite(t) || std::abs(t) >= double(std::numeric_limits<std::int64_t>::max()) / 1000)
            return {MaiErrorCode::Internal, "History row has an out-of-range timestamp"};
    }
    if (direction == "0" || direction == "INCOMING" || direction == "incoming")
        r.direction = MaiIMHistoryDirection::Incoming;
    else if (direction == "1" || direction == "OUTGOING" || direction == "outgoing")
        r.direction = MaiIMHistoryDirection::Outgoing;
    else
        unavailable(r, "direction");
    auto remote = source.text("remote_id", 512, good);
    if (good && !remote.empty() && identifier(remote))
        r.remoteId = remote;
    else if (!good || !remote.empty())
        unavailable(r, "remote_id");
    good = true;
    r.captionAbove = source.number("caption_above", 1, good) == 1;
    if (!good) unavailable(r, "caption_above");
    if (options.profile == MaiIMHistoryProfile::IOS) {
        for (auto p : {std::pair<const char*, const char*>{"image_attachment", "image"},
                       {"voice_attachment", "voice"},
                       {"file_attachment", "file"},
                       {"video_attachment", "video"}})
            if (source.type(p.first) != SQLITE_NULL) r.media.push_back(jsonMedia(source, p.first, p.second));
        if (source.type("quote") != SQLITE_NULL) {
            good = true;
            auto raw = source.text("quote", kJsonBytes, good);
            auto q = good ? Json::parse(raw, nullptr, false) : Json();
            if (q.is_object()) {
                MaiIMHistoryQuote v;
                v.messageId = jsonText(q, "messageID", 512, good);
                v.senderId = jsonText(q, "senderID", 512, good);
                v.digest = jsonText(q, "digest", kPathBytes, good);
                v.kind = jsonText(q, "kind", 64, good);
                if (good && !v.digest.empty())
                    r.quote = std::move(v);
                else
                    unavailable(r, "quote");
            } else
                unavailable(r, "quote");
        }
    } else {
        if (desktop ? source.present({"has_image"})
                    : source.present({"image_path", "image_width", "image_height", "image_size"}))
            r.media.push_back(columnMedia(source, "image", "image_path", nullptr, nullptr, nullptr, "image_size",
                                          nullptr, "image_width", "image_height"));
        if (desktop ? source.present({"has_voice"}) : source.present({"voice_path", "voice_duration"}))
            r.media.push_back(columnMedia(source, "voice", "voice_path", nullptr, nullptr, nullptr, nullptr,
                                          "voice_duration", nullptr, nullptr));
        if (desktop ? source.present({"has_file"})
                    : source.present({"file_path", "file_name", "file_mime", "file_size"}))
            r.media.push_back(columnMedia(source, "file", "file_path", "file_name", "file_mime", nullptr, "file_size",
                                          nullptr, nullptr, nullptr));
        if (desktop ? source.present({"has_video"})
                    : source.present({"video_path", "video_cover_path", "video_duration", "video_width", "video_height",
                                      "video_size"}))
            r.media.push_back(columnMedia(source, "video", "video_path", "video_name", nullptr, "video_cover_path",
                                          "video_size", "video_duration", "video_width", "video_height"));
        if (source.present({"quote_msg_id", "quote_sender", "quote_digest", "quote_kind"})) {
            good = true;
            MaiIMHistoryQuote q;
            q.messageId = source.text("quote_msg_id", 512, good);
            q.senderId = source.text("quote_sender", 512, good);
            q.digest = source.text("quote_digest", kPathBytes, good);
            q.kind = source.text("quote_kind", 64, good);
            if (good && !q.digest.empty())
                r.quote = std::move(q);
            else
                unavailable(r, "quote");
        }
    }
    for (auto& m : r.media) {
        if (desktop) {
            bool flagValid = true;
            source.number(("has_" + m.kind).c_str(), 1, flagValid);
            if (!flagValid) m.metadataUnavailable = true;
        }
        if (m.metadataUnavailable) unavailable(r, m.kind);
    }
    r.kind = r.media.empty() ? "text" : r.media.size() == 1 ? r.media.front().kind : "mixed";
    return r;
}
std::size_t recordBytes(const MaiIMHistoryRecord& r) {
    // Same strict JSON string escaping, with conservative field/container overhead.
    auto bytes = [](const std::string& s) { return Json(s).dump().size(); };
    std::size_t n = 512 + bytes(r.rowId) + bytes(r.fromUserId) + bytes(r.toUserId) + bytes(r.status) + bytes(r.text) +
                    bytes(r.kind);
    if (r.remoteId) n += bytes(*r.remoteId);
    for (const auto& m : r.media)
        n += 256 + bytes(m.kind) + bytes(m.name) + bytes(m.mimeType) + bytes(m.localPath) + bytes(m.coverPath);
    if (r.quote)
        n += 128 + bytes(r.quote->messageId) + bytes(r.quote->senderId) + bytes(r.quote->digest) + bytes(r.quote->kind);
    for (const auto& s : r.unavailableMetadata) n += bytes(s) + 1;
    return n;
}
std::string columns(MaiIMHistoryProfile p) {
    if (p == MaiIMHistoryProfile::IOS)
        return "id,remote_id,from_user AS from_id,to_user AS to_id,text AS body,direction,status,"
               "CAST(ROUND(created_at*1000.0) AS INTEGER) AS created_ms,created_at AS "
               "source_created,caption_above,"
               "image_attachment,voice_attachment,file_attachment,video_attachment,quote";
    std::string s = p == MaiIMHistoryProfile::Desktop
                        ? "id,NULL AS remote_id,from_user AS from_id,to_user AS to_id,text AS "
                          "body,direction,status,created_at AS created_ms,"
                          "caption_above,has_image,image_path,image_w AS image_width,image_h AS "
                          "image_height,image_bytes AS image_size,"
                          "has_voice,voice_path,voice_seconds AS "
                          "voice_duration,has_file,file_path,file_name,file_mime,file_bytes AS file_size,"
                          "has_video,video_path,video_name,video_cover AS video_cover_path,video_seconds AS "
                          "video_duration,video_bytes AS video_size,"
                          "0 AS video_width,0 AS video_height,"
                        : "id,remote_id,from_id,to_id,body,direction,status,created_at AS "
                          "created_ms,caption_above,image_path,image_width,image_height,"
                          "image_size,voice_path,voice_duration,file_path,file_name,file_mime,file_size,video_"
                          "path,'' AS video_name,video_cover_path,"
                          "video_duration,video_width,video_height,video_size,";
    return s + "quote_msg_id,quote_sender,quote_digest,quote_kind";
}
}  // namespace

struct MaiIMHistorySnapshot::Impl {
    MaiIMHistorySnapshotOptions options;
    MaiIMHistorySource descriptor;
    MaiFilePath original, canonical;
    sqlite3* database = nullptr;
    std::unique_ptr<MaiFileReadHandle> held;
    Clock::time_point expires, pageDeadline;
    MaiError failure;
    bool hasCursor = false, finished = false;
    std::int64_t lastTime = 0;
    std::string lastId;
    std::uint64_t sequence = 0;
    ~Impl() {
        if (database) {
            sqlite3_progress_handler(database, 0, nullptr, nullptr);
            sqlite3_busy_handler(database, nullptr, nullptr);
            sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr);
            sqlite3_close_v2(database);
        }
    }
    MaiError stopped() const {
        if (options.canceled->load()) return {MaiErrorCode::Canceled, "History snapshot was canceled"};
        auto now = Clock::now();
        if (now >= expires) return {MaiErrorCode::Busy, "History snapshot lifetime expired"};
        if (now >= pageDeadline) return {MaiErrorCode::Busy, "History page deadline expired"};
        return {};
    }
    MaiError guard() const {
        if (auto e = stopped()) return e;
        if (!MaiFileSystem::exists(original) || MaiFileSystem::resolve(original) != canonical)
            return {MaiErrorCode::InvalidInput, "History source path was replaced"};
        int moved = 0;
        int code = sqlite3_file_control(database, "main", SQLITE_FCNTL_HAS_MOVED, &moved);
        if (code == SQLITE_OK && moved) return {MaiErrorCode::InvalidInput, "History source file was replaced"};
#ifdef _WIN32
        // Core holds a no-share-delete handle; normal WAL/checkpoint writes remain allowed.
        if (code == SQLITE_NOTFOUND && held) return {};
#endif
        if (code != SQLITE_OK) return {MaiErrorCode::NotSupported, "History source VFS cannot verify file identity"};
        return {};
    }
    MaiError sqlError(int code) const {
        if (auto e = stopped()) return e;
        if ((code & 255) == SQLITE_BUSY || (code & 255) == SQLITE_LOCKED)
            return {MaiErrorCode::Busy, "History source database is busy"};
        return {MaiErrorCode::Internal, "History source SQL or storage operation failed"};
    }
    static int progress(void* p) {
        return bool(static_cast<Impl*>(p)->stopped()) ? 1 : 0;
    }
    static int busy(void* p, int) {
        if (static_cast<Impl*>(p)->stopped()) return 0;
        sqlite3_sleep(1);
        return 1;
    }
    std::string time() const {
        return options.profile == MaiIMHistoryProfile::IOS ? "CAST(ROUND(created_at*1000.0) AS INTEGER)" : "created_at";
    }
    std::string query() const {
        std::string s = "SELECT " + columns(options.profile) + " FROM messages WHERE ";
        if (options.profile == MaiIMHistoryProfile::Desktop)
            s += "peer=?";
        else if (options.profile == MaiIMHistoryProfile::Android)
            s += "owner_id=? AND peer_id=?";
        else
            // 和普通 iOS 查询及其表达式索引保持同一 peer 定义，保留尚未回填的旧行。
            s += "sdk_app_id=? AND owner_user_id=? AND (CASE WHEN peer_user_id IS NULL OR "
                 "peer_user_id='' THEN CASE WHEN from_user=owner_user_id THEN to_user ELSE "
                 "from_user END ELSE peer_user_id END)=? AND group_id IS NULL";
        // OR 负责同毫秒的稳定分页；显式下界让 SQLite 从复合索引游标起点查找，
        // 避免每一页重新扫描该好友全部更早的历史。下界不改变原严格 keyset 语义。
        if (hasCursor)
            s += " AND " + time() + ">=? AND (" + time() + ">? OR (" + time() + "=? AND id COLLATE BINARY>?))";
        return s + " ORDER BY " + time() + " ASC,id COLLATE BINARY ASC LIMIT ?";
    }
    bool bind(sqlite3_stmt* s) const {
        int i = 0;
        auto text = [&](const std::string& v) {
            return sqlite3_bind_text(s, ++i, v.data(), int(v.size()), SQLITE_TRANSIENT) == SQLITE_OK;
        };
        if (options.profile == MaiIMHistoryProfile::IOS && !text(std::to_string(options.sdkAppId))) return false;
        if (options.profile != MaiIMHistoryProfile::Desktop && !text(options.ownerUserId)) return false;
        if (!text(options.peerUserId)) return false;
        if (hasCursor &&
            (sqlite3_bind_int64(s, ++i, lastTime) != SQLITE_OK || sqlite3_bind_int64(s, ++i, lastTime) != SQLITE_OK ||
             sqlite3_bind_int64(s, ++i, lastTime) != SQLITE_OK || !text(lastId)))
            return false;
        return sqlite3_bind_int64(s, ++i, sqlite3_int64(options.maxRecords + 1)) == SQLITE_OK;
    }
};
MaiIMHistorySnapshot::MaiIMHistorySnapshot(std::unique_ptr<Impl> impl) : mImpl(std::move(impl)) {}
MaiIMHistorySnapshot::~MaiIMHistorySnapshot() = default;
MaiResult<std::unique_ptr<MaiIMHistorySnapshot>> MaiIMHistorySnapshot::open(const MaiIMHistorySnapshotOptions& o) {
    if ((o.profile != MaiIMHistoryProfile::Desktop && o.profile != MaiIMHistoryProfile::Android &&
         o.profile != MaiIMHistoryProfile::IOS) ||
        !identifier(o.ownerUserId) || !identifier(o.peerUserId) || o.ownerUserId == o.peerUserId ||
        o.databasePath.find('\0') != std::string::npos || (o.profile == MaiIMHistoryProfile::IOS && !o.sdkAppId) ||
        !o.maxRecords || o.maxRecords > 200 || !o.maxPageBytes || o.maxPageBytes > 384 * 1024 || !o.maxTextBytes ||
        o.maxTextBytes > 256 * 1024 || !o.pageTimeoutMillis || o.pageTimeoutMillis > 5000 || !o.lifetimeTimeoutMillis ||
        o.lifetimeTimeoutMillis > 300000)
        return {MaiErrorCode::InvalidInput, "Invalid history snapshot options"};
    auto p = std::make_unique<Impl>();
    p->options = o;
    if (!p->options.canceled) p->options.canceled = std::make_shared<std::atomic<bool>>(false);
    auto start = Clock::now();
    p->expires = start + std::chrono::milliseconds(o.lifetimeTimeoutMillis);
    p->pageDeadline = start + std::chrono::milliseconds(o.pageTimeoutMillis);
    if (auto e = p->stopped()) return e;
    p->original = MaiFilePath::fromUtf8(o.databasePath);
    if (!p->original.isAbsolute()) return {MaiErrorCode::InvalidInput, "History database path must be absolute"};
    if (!MaiFileSystem::exists(p->original)) return {MaiErrorCode::NotFound, "History source database does not exist"};
    if (MaiFileSystem::isDirectory(p->original))
        return {MaiErrorCode::InvalidInput, "History source is not a database file"};
    p->canonical = MaiFileSystem::resolve(p->original);
    if (p->canonical.isEmpty()) return {MaiErrorCode::InvalidInput, "History source cannot be resolved"};
#ifdef _WIN32
    auto held = MaiFileReadHandle::open(p->canonical);
    if (!held) return held.error();
    p->held = std::move(held.value());
#endif
    auto path = p->canonical.toUtf8();
    int code = sqlite3_open_v2(path.c_str(), &p->database,
                               SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_PRIVATECACHE, nullptr);
    if (code != SQLITE_OK) return p->sqlError(code);
    sqlite3_extended_result_codes(p->database, 1);
    sqlite3_progress_handler(p->database, 200, &Impl::progress, p.get());
    sqlite3_busy_handler(p->database, &Impl::busy, p.get());
    code = sqlite3_exec(p->database, "PRAGMA query_only=ON;PRAGMA trusted_schema=OFF;BEGIN", nullptr, nullptr, nullptr);
    if (code != SQLITE_OK) return p->sqlError(code);
    sqlite3_stmt* pin = nullptr;
    code = sqlite3_prepare_v2(p->database, "SELECT 1 FROM messages LIMIT 1", -1, &pin, nullptr);
    if (code != SQLITE_OK) return p->sqlError(code);
    code = sqlite3_step(pin);
    sqlite3_finalize(pin);
    if (code != SQLITE_ROW && code != SQLITE_DONE) return p->sqlError(code);
    // Validate all fixed profile columns inside the same actual pinned snapshot.
    sqlite3_stmt* schema = nullptr;
    code = sqlite3_prepare_v2(p->database, p->query().c_str(), -1, &schema, nullptr);
    if (schema) sqlite3_finalize(schema);
    if (code != SQLITE_OK) return p->sqlError(code);
    if (auto e = p->guard()) return e;
    p->descriptor = {o.profile,
                     path,
                     o.ownerUserId,
                     o.peerUserId,
                     o.profile == MaiIMHistoryProfile::IOS ? std::optional<std::uint64_t>(o.sdkAppId) : std::nullopt,
                     o.profile == MaiIMHistoryProfile::IOS ? "owner_sdk" : "owner_only_legacy",
                     "local_persisted"};
    return std::unique_ptr<MaiIMHistorySnapshot>(new MaiIMHistorySnapshot(std::move(p)));
}
MaiResult<MaiIMHistoryPage> MaiIMHistorySnapshot::read() {
    auto& p = *mImpl;
    if (p.failure) return p.failure;
    p.pageDeadline = Clock::now() + std::chrono::milliseconds(p.options.pageTimeoutMillis);
    auto fail = [&](MaiError e) -> MaiResult<MaiIMHistoryPage> {
        p.failure = e;
        return e;
    };
    if (auto e = p.guard()) return fail(e);
    MaiIMHistoryPage page;
    page.sequence = p.sequence;
    if (p.finished) {
        ++p.sequence;
        return page;
    }
    sqlite3_stmt* statement = nullptr;
    int code = sqlite3_prepare_v2(p.database, p.query().c_str(), -1, &statement, nullptr);
    auto finalize = [](sqlite3_stmt* s) {
        if (s) sqlite3_finalize(s);
    };
    std::unique_ptr<sqlite3_stmt, decltype(finalize)> owned(statement, finalize);
    if (code != SQLITE_OK || !p.bind(statement)) return fail(p.sqlError(code));
    while ((code = sqlite3_step(statement)) == SQLITE_ROW) {
        if (auto e = p.stopped()) return fail(e);
        if (page.records.size() == p.options.maxRecords) {
            page.hasMore = true;
            break;
        }
        auto row = decode(statement, p.options);
        if (!row) return fail(row.error());
        std::size_t bytes = 0;
        try {
            bytes = recordBytes(row.value());
        } catch (const std::exception&) {
            return fail({MaiErrorCode::InvalidInput, "History row has invalid text encoding"});
        }
        if (bytes > p.options.maxPageBytes)
            return fail({MaiErrorCode::InvalidInput, "History row exceeds page byte budget"});
        if (page.serializedBytes + bytes > p.options.maxPageBytes) {
            page.hasMore = true;
            break;
        }
        page.serializedBytes += bytes;
        page.records.push_back(std::move(row.value()));
    }
    if (code != SQLITE_ROW && code != SQLITE_DONE) return fail(p.sqlError(code));
    if (auto e = p.guard()) return fail(e);
    if (!page.records.empty()) {
        p.hasCursor = true;
        p.lastTime = page.records.back().createdAtMillis;
        p.lastId = page.records.back().rowId;
    }
    p.finished = !page.hasMore;
    ++p.sequence;
    return page;
}
void MaiIMHistorySnapshot::cancel() {
    mImpl->options.canceled->store(true);
}
const MaiIMHistorySource& MaiIMHistorySnapshot::source() const {
    return mImpl->descriptor;
}
