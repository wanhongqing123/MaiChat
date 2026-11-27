// 文件作用：核对权威日志的实际 UTF-8 字节、原文前缀、游标、并发序号与损坏边界。
#include <atomic>
#include <cstdio>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include <json.hpp>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "MaiAppendFile.h"
#include "MaiBlockingCheck.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiRolloutLog.h"

namespace {
using Json = nlohmann::json;
int failures = 0;
#define CHECK(value)                                                    \
    do {                                                                \
        if (!(value)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #value); \
            ++failures;                                                 \
        }                                                               \
    } while (false)

struct OwnedDirectory {
    MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("rollout-" + MaiIdGenerator::newSessionId()));
    OwnedDirectory() {
        CHECK(!MaiFileSystem::createDirectories(root));
    }
    ~OwnedDirectory() {
        MaiDirectoryDeleteOptions options;
        options.recursive = true;
        options.pathAllowed = [this](const MaiFilePath& path) {
            return path == root || root.isParentOf(path);
        };
        CHECK(!MaiFileSystem::deleteDirectory(root, options).error);
    }
};

constexpr MaiMillis kTimestamp = 1795185882657;
const std::string kMetadata = R"({"id":"owned-rollout","session_id":"ses_owned","cwd":"owned"})";

void test_actual_wrapper_offsets_replay_and_immutable_prefix() {
    OwnedDirectory owned;
    const auto path = owned.root.append(MaiFilePath::fromUtf8("sessions/owned.jsonl"));
    auto created = MaiRolloutLog::create(path, kMetadata, kTimestamp);
    CHECK(created);
    if (!created) return;
    auto log = std::move(created.value());
    std::string original;
    CHECK(!MaiFileSystem::readFile(path, original));
    CHECK(original.find("{\"timestamp\":\"2026-11-20T14:44:42.657Z\",\"ordinal\":0,"
                        "\"type\":\"session_meta\",\"payload\":") == 0);
    const Json payload{
        {"type", "message"},
        {"id", "msg_owned"},
        {"content", Json::array({{{"type", "input_text"}, {"text", "\u4f60\u597d\nsecond line"}}})},
        {"unknown_vendor_field", {{"signature", "opaque"}}}};
    const auto item = log->append("response_item", payload.dump(), kTimestamp + 1);
    CHECK(item);
    if (!item) return;
    CHECK(item.value().ordinal == 1);
    CHECK(item.value().byteOffset == original.size());
    const auto read = MaiRolloutLog::read(path, item.value());
    CHECK(read);
    if (read) CHECK(Json::parse(read.value().payload) == payload);
    std::string final;
    CHECK(!MaiFileSystem::readFile(path, final));
    CHECK(final.substr(0, original.size()) == original);
    CHECK(final.size() == item.value().byteOffset + item.value().byteLength);
    unsigned applied = 0;
    const auto cursor = log->replay({}, [&](const MaiRolloutRecord& record) {
        CHECK(record.location.ordinal == applied++);
        return MaiError{};
    });
    CHECK(cursor && applied == 2);
    if (cursor) {
        CHECK(cursor.value().byteOffset == final.size());
        CHECK(cursor.value().nextOrdinal == 2);
        CHECK(log->replay(cursor.value(), [&](const MaiRolloutRecord&) {
            CHECK(false);
            return MaiError{};
        }));
    }
    CHECK(!log->append("session_meta", kMetadata, kTimestamp));
    CHECK(!log->append("event_msg", "[]", kTimestamp));
    CHECK(!log->append("event_msg", "{broken", kTimestamp));
    log.reset();
    auto reopened = MaiRolloutLog::open(path);
    CHECK(reopened);
    if (!reopened) return;
    const auto next =
        reopened.value()->append("event_msg", R"({"type":"task_complete"})", kTimestamp + 2);
    CHECK(next && next.value().ordinal == 2);
    auto invalidLocation = item.value();
    ++invalidLocation.ordinal;
    CHECK(!MaiRolloutLog::read(path, invalidLocation));
}

void test_two_writers_allocate_only_file_ordinals() {
    OwnedDirectory owned;
    const auto path = owned.root.append(MaiFilePath::fromUtf8("concurrent.jsonl"));
    auto first = MaiRolloutLog::create(path, kMetadata, kTimestamp);
    CHECK(first);
    if (!first) return;
    auto second = MaiRolloutLog::open(path);
    CHECK(second);
    if (!second) return;
    std::atomic<unsigned> errors{0};
    const auto append = [&](MaiRolloutLog& log, unsigned producer) {
        for (unsigned i = 0; i < 20; ++i)
            if (!log.append(
                    "event_msg",
                    Json{{"type", "item_completed"}, {"producer", producer}, {"sequence", i}}
                        .dump(),
                    kTimestamp + i))
                ++errors;
    };
    std::thread a([&] { append(*first.value(), 1); });
    std::thread b([&] { append(*second.value(), 2); });
    a.join();
    b.join();
    CHECK(errors == 0);
    unsigned count = 0;
    CHECK(first.value()->replay({}, [&](const MaiRolloutRecord& record) {
        CHECK(record.location.ordinal == count++);
        return MaiError{};
    }));
    CHECK(count == 41);
}

void test_incomplete_or_corrupt_source_never_gets_silently_rewritten() {
    OwnedDirectory owned;
    const auto path = owned.root.append(MaiFilePath::fromUtf8("bad.jsonl"));
    auto created = MaiRolloutLog::create(path, kMetadata, kTimestamp);
    CHECK(created);
    if (!created) return;
    created.value().reset();
    std::string original;
    CHECK(!MaiFileSystem::readFile(path, original));
    const auto broken = original + "{\"timestamp\":\"partial";
    CHECK(!MaiFileSystem::writeFile(path, broken));
    CHECK(!MaiRolloutLog::open(path));
    std::string unchanged;
    CHECK(!MaiFileSystem::readFile(path, unchanged));
    CHECK(unchanged == broken);
    const auto missing = owned.root.append(MaiFilePath::fromUtf8("missing.jsonl"));
    CHECK(!MaiRolloutLog::open(missing));
    CHECK(!MaiFileSystem::exists(missing));
}

std::string rawRecord(std::uint64_t ordinal, const std::string& type,
                      const std::string& payload = "{}") {
    return "{\"timestamp\":\"2026-11-20T14:44:42.657Z\",\"ordinal\":" + std::to_string(ordinal) +
           ",\"type\":\"" + type + "\",\"payload\":" + payload + "}\n";
}

void appendRaw(const MaiFilePath& path, const std::string& bytes) {
    auto file = MaiAppendFile::open(path);
    CHECK(file);
    if (!file) return;
    CHECK(!file.value()->lockWriter());
    CHECK(file.value()->append(bytes));
    CHECK(!file.value()->flush());
}

void test_types_metadata_and_large_record_boundaries() {
    OwnedDirectory owned;
    const auto path = owned.root.append(MaiFilePath::fromUtf8("all-types.jsonl"));
    CHECK(!MaiRolloutLog::create(path, "{}", kTimestamp));
    CHECK(!MaiRolloutLog::create(path, R"({"id":"","session_id":"s"})", kTimestamp));
    CHECK(!MaiRolloutLog::create(path, R"({"id":"i","session_id":3})", kTimestamp));
    CHECK(!MaiRolloutLog::create(path, R"({"id":"i","id":"j","session_id":"s"})", kTimestamp));
    CHECK(!MaiRolloutLog::create(path, kMetadata, -1));
    CHECK(!MaiFileSystem::exists(path));
    auto log = MaiRolloutLog::create(path, kMetadata, kTimestamp);
    CHECK(log);
    if (!log) return;
    std::uint64_t ordinal = 1;
    for (const char* type :
         {"turn_context", "world_state", "response_item", "event_msg", "token_usage_record",
          "compacted", "inter_agent_communication_metadata"}) {
        auto item = log.value()->append(type, R"({"opaque":{"signature":"exact"}})", kTimestamp);
        CHECK(item && item.value().ordinal == ordinal++);
    }
    CHECK(!log.value()->append("unknown_record_type", "{}", kTimestamp));
    CHECK(!log.value()->append("event_msg", R"({"unknown":1,"unknown":2})", kTimestamp));
    CHECK(!log.value()->append("event_msg", R"({"unknown":{"x":1,"x":2}})", kTimestamp));
    const Json payload{{"unknown_large_field", std::string(1024 * 1024 + 7, 'z')},
                       {"nested", Json::array({Json{{"one", 1}}, Json{{"one", 2}}})}};
    auto large = log.value()->append("response_item", payload.dump(), kTimestamp);
    CHECK(large && large.value().ordinal == ordinal++ && large.value().byteLength > 1024 * 1024);
    if (large) {
        const auto record = MaiRolloutLog::read(path, large.value());
        CHECK(record && Json::parse(record.value().payload) == payload);
        auto broken = large.value();
        --broken.byteLength;
        CHECK(!MaiRolloutLog::read(path, broken));
        broken = large.value();
        ++broken.byteLength;
        CHECK(!MaiRolloutLog::read(path, broken));
        broken = large.value();
        ++broken.byteOffset;
        CHECK(!MaiRolloutLog::read(path, broken));
        broken.byteLength = std::numeric_limits<std::uint64_t>::max();
        CHECK(!MaiRolloutLog::read(path, broken));
        broken.byteLength = 2;
        broken.byteOffset = std::numeric_limits<std::uint64_t>::max();
        CHECK(!MaiRolloutLog::read(path, broken));
    }
    CHECK(!log.value()->append("event_msg", std::string(64 * 1024 * 1024 + 1, 'x'), kTimestamp));
    std::string original;
    CHECK(!MaiFileSystem::readFile(path, original));
    CHECK(!MaiRolloutLog::create(path, kMetadata, kTimestamp));
    std::string after;
    CHECK(!MaiFileSystem::readFile(path, after));
    CHECK(original == after);
    unsigned count = 0;
    const auto cursor = log.value()->replay({}, [&](const MaiRolloutRecord&) {
        ++count;
        return MaiError{};
    });
    CHECK(cursor && cursor.value().nextOrdinal == ordinal && count == ordinal);
}

void test_cursor_validation_callback_error_and_snapshot_append() {
    OwnedDirectory owned;
    const auto path = owned.root.append(MaiFilePath::fromUtf8("cursor.jsonl"));
    auto log = MaiRolloutLog::create(path, kMetadata, kTimestamp);
    CHECK(log);
    if (!log) return;
    const auto first = log.value()->append("event_msg", "{}", kTimestamp);
    CHECK(first);
    if (!first) return;
    unsigned calls = 0;
    const auto apply = [&](const MaiRolloutRecord&) {
        ++calls;
        return MaiError{};
    };
    CHECK(!log.value()->replay({1, 0}, apply));
    CHECK(!log.value()->replay({0, 1}, apply));
    CHECK(!log.value()->replay({first.value().byteOffset, 2}, apply));
    CHECK(!log.value()->replay({std::numeric_limits<std::uint64_t>::max(), 0}, apply));
    CHECK(calls == 0);
    const auto failed = log.value()->replay({}, [&](const MaiRolloutRecord&) {
        ++calls;
        return MaiError{MaiErrorCode::Canceled, "projection transaction canceled"};
    });
    CHECK(!failed && failed.error().code() == MaiErrorCode::Canceled && calls == 1);
    calls = 0;
    CHECK(log.value()->replay({}, apply));
    CHECK(calls == 2);
    CHECK(!log.value()->replay({}, {}));
    auto other = MaiRolloutLog::open(path);
    CHECK(other);
    if (!other) return;
    calls = 0;
    const auto snapshot = log.value()->replay({}, [&](const MaiRolloutRecord&) {
        if (++calls == 1) CHECK(other.value()->append("event_msg", "{}", kTimestamp));
        return MaiError{};
    });
    CHECK(snapshot && calls == 2 && snapshot.value().nextOrdinal == 2);
    calls = 0;
    if (snapshot) {
        const auto next = log.value()->replay(snapshot.value(), apply);
        CHECK(next && calls == 1 && next.value().nextOrdinal == 3);
    }
}

void test_gaps_overflow_and_external_tail_validation() {
    OwnedDirectory owned;
    const auto gapPath = owned.root.append(MaiFilePath::fromUtf8("gap.jsonl"));
    auto gap = MaiRolloutLog::create(gapPath, kMetadata, kTimestamp);
    CHECK(gap);
    if (!gap) return;
    appendRaw(gapPath, rawRecord(8, "event_msg"));
    const auto next = gap.value()->append("event_msg", "{}", kTimestamp);
    CHECK(next && next.value().ordinal == 9);
    appendRaw(gapPath, rawRecord(std::numeric_limits<std::uint64_t>::max() - 1, "event_msg"));
    std::string before;
    CHECK(!MaiFileSystem::readFile(gapPath, before));
    CHECK(!gap.value()->append("event_msg", "{}", kTimestamp));
    std::string after;
    CHECK(!MaiFileSystem::readFile(gapPath, after));
    CHECK(after == before);
    unsigned calls = 0;
    const auto cursor = gap.value()->replay({}, [&](const MaiRolloutRecord&) {
        ++calls;
        return MaiError{};
    });
    CHECK(cursor && calls == 4 &&
          cursor.value().nextOrdinal == std::numeric_limits<std::uint64_t>::max());

    unsigned caseIndex = 0;
    for (const std::string& tail :
         {std::string("{\"partial\":"), rawRecord(0, "event_msg"),
          rawRecord(1, "session_meta", kMetadata), rawRecord(1, "unrecognized"),
          rawRecord(1, "event_msg", "[]"),
          rawRecord(std::numeric_limits<std::uint64_t>::max(), "event_msg"),
          rawRecord(1, "event_msg") + rawRecord(1, "event_msg")}) {
        const auto path = owned.root.append(
            MaiFilePath::fromUtf8("corrupt-" + std::to_string(caseIndex++) + ".jsonl"));
        auto log = MaiRolloutLog::create(path, kMetadata, kTimestamp);
        CHECK(log);
        if (!log) continue;
        appendRaw(path, tail);
        CHECK(!MaiFileSystem::readFile(path, before));
        CHECK(!log.value()->append("event_msg", "{}", kTimestamp));
        calls = 0;
        CHECK(!log.value()->replay({}, [&](const MaiRolloutRecord&) {
            ++calls;
            return MaiError{};
        }));
        CHECK(calls == 0);
        CHECK(!MaiRolloutLog::open(path));
        CHECK(!MaiFileSystem::readFile(path, after));
        CHECK(after == before);
    }
    const auto truncated = owned.root.append(MaiFilePath::fromUtf8("truncate.jsonl"));
    auto log = MaiRolloutLog::create(truncated, kMetadata, kTimestamp);
    CHECK(log);
    if (!log) return;
#if defined(_WIN32)
    HANDLE handle =
        ::CreateFileW(truncated.value().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(handle != INVALID_HANDLE_VALUE);
    if (handle != INVALID_HANDLE_VALUE) {
        CHECK(::SetEndOfFile(handle));
        ::CloseHandle(handle);
    }
#else
    CHECK(::truncate(truncated.value().c_str(), 0) == 0);
#endif
    CHECK(!log.value()->append("event_msg", "{}", kTimestamp));
    CHECK(log.value()->confirmDurable());
    std::uint64_t size = 1;
    CHECK(MaiFileSystem::fileSize(truncated, size) && size == 0);
}
void test_read_at_returns_source_ordinal_without_an_index_ordinal() {
    OwnedDirectory owned;
    const auto path = owned.root.append(MaiFilePath::fromUtf8("source-ordinal.jsonl"));
    auto log = MaiRolloutLog::create(path, kMetadata, kTimestamp);
    CHECK(log);
    if (!log) return;
    std::uint64_t metadataLength = 0;
    CHECK(MaiFileSystem::fileSize(path, metadataLength));
    const auto line = rawRecord(42, "response_item", R"({"id":"message-owned","opaque":"kept"})");
    appendRaw(path, line);
    const auto metadata = MaiRolloutLog::readAt(path, 0, metadataLength);
    CHECK(metadata && metadata.value().location.ordinal == 0 &&
          metadata.value().type == "session_meta");
    const auto observed = MaiRolloutLog::readAt(path, metadataLength, line.size());
    CHECK(observed);
    if (observed) {
        CHECK(observed.value().location.ordinal == 42);
        CHECK(observed.value().location.byteOffset == metadataLength);
        CHECK(observed.value().location.byteLength == line.size());
        CHECK(Json::parse(observed.value().payload)["opaque"] == "kept");
    }
    CHECK(MaiRolloutLog::read(path, {42, metadataLength, line.size()}));
    CHECK(!MaiRolloutLog::read(path, {41, metadataLength, line.size()}));
    CHECK(!MaiRolloutLog::readAt(path, metadataLength + 1, line.size() - 1));
    CHECK(!MaiRolloutLog::readAt(path, metadataLength, line.size() - 1));
    CHECK(!MaiRolloutLog::readAt(path, metadataLength, 0));
    CHECK(!MaiRolloutLog::readAt(path, metadataLength, 64 * 1024 * 1024 + 1));
    CHECK(!MaiRolloutLog::readAt(path, std::numeric_limits<std::uint64_t>::max(), 1));
    const std::string partial = "{\"partial\":";
    appendRaw(path, partial);
    CHECK(!MaiRolloutLog::readAt(path, metadataLength + line.size(), partial.size()));
    // 未完成尾部不能污染已存在的不可变完整记录，也不能被静默修剪。
    CHECK(MaiRolloutLog::readAt(path, metadataLength, line.size()));
    std::uint64_t length = 0;
    CHECK(MaiFileSystem::fileSize(path, length));
    CHECK(length == metadataLength + line.size() + partial.size());
}
void test_confirm_durable_preserves_bytes_and_next_ordinal() {
    OwnedDirectory owned;
    const auto path = owned.root.append(MaiFilePath::fromUtf8("confirm-durable.jsonl"));
    auto log = MaiRolloutLog::create(path, kMetadata, kTimestamp);
    CHECK(log);
    if (!log) return;
    const auto first = log.value()->append("event_msg", "{}", kTimestamp);
    CHECK(first && first.value().ordinal == 1);
    // 独立写者的完整尾部也必须在重新验证后纳入本次 flush。
    appendRaw(path, rawRecord(2, "response_item"));
    std::string before;
    CHECK(!MaiFileSystem::readFile(path, before));
    CHECK(!log.value()->confirmDurable());
    CHECK(!log.value()->confirmDurable());
    std::string after;
    CHECK(!MaiFileSystem::readFile(path, after));
    CHECK(before == after);
    const auto next = log.value()->append("event_msg", "{}", kTimestamp);
    CHECK(next && next.value().ordinal == 3);
    appendRaw(path, "{\"incomplete\":");
    CHECK(!MaiFileSystem::readFile(path, before));
    CHECK(log.value()->confirmDurable());
    CHECK(!MaiFileSystem::readFile(path, after));
    CHECK(before == after);
    const auto malformedPath = owned.root.append(MaiFilePath::fromUtf8("durable-malformed.jsonl"));
    auto malformed = MaiRolloutLog::create(malformedPath, kMetadata, kTimestamp);
    CHECK(malformed);
    if (!malformed) return;
    appendRaw(malformedPath, "{invalid-json}\n");
    CHECK(!MaiFileSystem::readFile(malformedPath, before));
    CHECK(malformed.value()->confirmDurable());
    CHECK(!MaiFileSystem::readFile(malformedPath, after));
    CHECK(before == after);
}
void test_source_path_is_absolute_immutable_and_requires_no_io() {
    OwnedDirectory owned;
    const auto absolute = owned.root.append(MaiFilePath::fromUtf8("source-bound.jsonl"));
    auto created = MaiRolloutLog::create(absolute, kMetadata, kTimestamp);
    CHECK(created);
    if (!created) return;
    CHECK(created.value()->sourcePath() == absolute);
    CHECK(created.value()->sourcePath().isAbsolute());
    auto reopened = MaiRolloutLog::open(absolute);
    CHECK(reopened);
    if (reopened) {
        const MaiScopedDisallowBlocking noIo;
        CHECK(reopened.value()->sourcePath() == absolute);
        CHECK(&reopened.value()->sourcePath() == &reopened.value()->sourcePath());
    }
    // 规范化不能先跟随最终文件链接；否则 create 会错误地向被链接的空文件写入元数据。
    const auto empty = owned.root.append(MaiFilePath::fromUtf8("empty-source.jsonl"));
    const auto link = owned.root.append(MaiFilePath::fromUtf8("source-link.jsonl"));
    CHECK(!MaiFileSystem::writeFile(empty, ""));
#if defined(_WIN32)
    const bool linked =
        ::CreateSymbolicLinkW(link.value().c_str(), empty.value().c_str(), 0x2) != FALSE;
#else
    const bool linked = ::symlink(empty.value().c_str(), link.value().c_str()) == 0;
#endif
    if (linked) {
        CHECK(!MaiRolloutLog::create(link, kMetadata, kTimestamp));
        CHECK(!MaiRolloutLog::open(link));
        std::uint64_t bytes = 1;
        CHECK(MaiFileSystem::fileSize(empty, bytes) && bytes == 0);
#if defined(_WIN32)
        CHECK(::DeleteFileW(link.value().c_str()));
#else
        CHECK(::unlink(link.value().c_str()) == 0);
#endif
    } else {
        std::printf("SKIP source binding symlink creation unavailable\n");
    }
    // 相对路径位于当前测试构建的独占工作目录，避免更改全局 cwd 或干扰并行测试。
    const auto relative =
        MaiFilePath::fromUtf8("source-relative-" + MaiIdGenerator::newSessionId() + ".jsonl");
    const auto expected = MaiFileSystem::resolve(MaiFilePath::fromUtf8(".")).append(relative);
    {
        auto relativeLog = MaiRolloutLog::create(relative, kMetadata, kTimestamp);
        CHECK(relativeLog);
        if (relativeLog) {
            CHECK(relativeLog.value()->sourcePath().isAbsolute());
            CHECK(relativeLog.value()->sourcePath() == expected);
            auto relativeOpen = MaiRolloutLog::open(relative);
            CHECK(relativeOpen);
            if (relativeOpen) CHECK(relativeOpen.value()->sourcePath() == expected);
            auto absoluteOpen = MaiRolloutLog::open(expected);
            CHECK(absoluteOpen);
            if (absoluteOpen) CHECK(absoluteOpen.value()->sourcePath() == expected);
        }
    }
    CHECK(!MaiFileSystem::removeFile(relative));
}
}  // namespace

int main() {
    test_actual_wrapper_offsets_replay_and_immutable_prefix();
    test_two_writers_allocate_only_file_ordinals();
    test_incomplete_or_corrupt_source_never_gets_silently_rewritten();
    test_types_metadata_and_large_record_boundaries();
    test_cursor_validation_callback_error_and_snapshot_append();
    test_gaps_overflow_and_external_tail_validation();
    test_read_at_returns_source_ordinal_without_an_index_ordinal();
    test_confirm_durable_preserves_bytes_and_next_ordinal();
    test_source_path_is_absolute_immutable_and_requires_no_io();
    std::printf("Rollout log failures: %d\n", failures);
    return failures ? 1 : 0;
}
