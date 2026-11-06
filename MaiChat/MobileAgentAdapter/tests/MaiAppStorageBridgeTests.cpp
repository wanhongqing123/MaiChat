// 文件作用：只在自建目录验证普通存储 C ABI，不启动 Agent 或接触宿主真实存储。
#include "MaiAppStorageBridge.h"

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

#include <json.hpp>

#if defined(_WIN32)
#include <windows.h>
#else
#include <utime.h>
#endif

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"

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

struct Fixture {
    MaiFilePath root, temporary, cache, workspace;
    void* handle = nullptr;
    Fixture() {
        root = MaiFileSystem::temporaryDirectory().append(
            MaiFilePath::fromUtf8(MaiIdGenerator::generate("storage_bridge_test_")));
        temporary = root.append(MaiFilePath::fromUtf8("temporary"));
        cache = root.append(MaiFilePath::fromUtf8("cache"));
        workspace = root.append(MaiFilePath::fromUtf8("workspace"));
        CHECK(!MaiFileSystem::createDirectories(temporary));
        CHECK(!MaiFileSystem::createDirectories(cache));
        CHECK(!MaiFileSystem::createDirectories(workspace));
        handle = create();
        CHECK(handle);
    }
    ~Fixture() {
        maiMobileAppStorageDestroy(handle);
        MaiFileSystem::removeRecursively(root);
    }
    void* create() const {
        return maiMobileAppStorageCreate(temporary.toUtf8().c_str(), cache.toUtf8().c_str(),
                                         workspace.toUtf8().c_str());
    }
    MaiFilePath put(const MaiFilePath& base, const std::string& relative, const std::string& bytes = "data",
                    bool old = true) const {
        const auto path = base.append(MaiFilePath::fromUtf8(relative));
        CHECK(!MaiFileSystem::createDirectories(path.dirName()));
        CHECK(!MaiFileSystem::writeFile(path, bytes));
        if (old) {
            const auto seconds = std::time(nullptr) - 26 * 3600;
#if defined(_WIN32)
            const auto file = ::CreateFileW(path.value().c_str(), FILE_WRITE_ATTRIBUTES,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            CHECK(file != INVALID_HANDLE_VALUE);
            ULARGE_INTEGER value{};
            value.QuadPart = static_cast<std::uint64_t>(seconds) * 10000000ull + 116444736000000000ull;
            FILETIME time{value.LowPart, value.HighPart};
            CHECK(::SetFileTime(file, nullptr, &time, &time));
            ::CloseHandle(file);
#else
            utimbuf times{seconds, seconds};
            CHECK(::utime(path.value().c_str(), &times) == 0);
#endif
        }
        return path;
    }
};

Json request(void* handle, const std::string& request) {
    char* response = maiMobileAppStorageRequest(handle, request.c_str());
    CHECK(response);
    const auto parsed = response ? Json::parse(response) : Json::object();
    maiMobileAppStorageFree(response);
    return parsed;
}
Json preview(void* handle, const std::string& target) {
    const auto result = request(handle, Json{{"action", "preview"}, {"target", target}}.dump());
    CHECK(result.value("ok", false));
    return result.value("output", Json::object());
}
Json cleanup(void* handle, const std::string& id) {
    return request(handle, Json{{"action", "cleanup"}, {"preview_id", id}}.dump());
}
void rejected(void* handle, const std::string& text) {
    const auto result = request(handle, text);
    CHECK(!result.value("ok", true));
    CHECK(!result.value("errorCode", "").empty());
    CHECK(!result.value("error", "").empty());
}

void previewAndOneCategoryCleanup() {
    Fixture files;
    const auto cached = files.put(files.cache, "old.tmp", "cached");
    const auto temporary = files.put(files.temporary, "old.tmp", "temp");
    const auto fresh = files.put(files.cache, "fresh.tmp", "recent", false);
    const auto model = files.put(files.cache, "model.onnx", "model");
    const auto gallery = files.put(files.cache, "gallery-delivered.png", "delivery");
    const auto original = files.put(files.workspace, "original.jpg", "original");
    const auto intermediate = files.put(files.workspace, "frames_1/frame.png", "frame");
    const auto summary = request(files.handle, R"({"action":"preview"})").at("output");
    CHECK(summary.at("summaries").size() == 3);
    CHECK(summary.at("candidate_files") == 2);
    CHECK(summary.at("candidate_bytes") == 10);
    CHECK(summary.at("min_age_hours") == 24);
    CHECK(!summary.at("cleanup_allowed").get<bool>());
    CHECK(!summary.at("workspace_cleanup_allowed").get<bool>());
    CHECK(!summary.contains("candidates"));
    CHECK(!cleanup(files.handle, summary.at("preview_id")).value("ok", true));
    CHECK(MaiFileSystem::exists(cached));
    CHECK(MaiFileSystem::exists(temporary));

    const auto cache = preview(files.handle, "cache");
    CHECK(cache.at("candidate_files") == 1);
    CHECK(cache.at("candidate_bytes") == 6);
    CHECK(cache.at("cleanup_allowed").get<bool>());
    const auto cleaned = cleanup(files.handle, cache.at("preview_id"));
    CHECK(cleaned.value("ok", false));
    CHECK(cleaned.at("output").at("removed_files") == 1);
    CHECK(cleaned.at("output").at("reclaimed_bytes") == 6);
    CHECK(cleaned.at("output").at("skipped_files") == 0);
    CHECK(!cleanup(files.handle, cache.at("preview_id")).value("ok", true));
    CHECK(!MaiFileSystem::exists(cached));
    for (const auto& kept : {temporary, fresh, model, gallery, original, intermediate})
        CHECK(MaiFileSystem::exists(kept));

    const auto temp = preview(files.handle, "temporary");
    CHECK(cleanup(files.handle, temp.at("preview_id")).value("ok", false));
    CHECK(!MaiFileSystem::exists(temporary));
    CHECK(MaiFileSystem::exists(original));
    CHECK(MaiFileSystem::exists(intermediate));
}

void exactCandidateAndTokenBinding() {
    Fixture files;
    const auto changed = files.put(files.cache, "changed.tmp");
    const auto removable = files.put(files.cache, "remove.tmp");
    const auto first = preview(files.handle, "cache");
    files.put(files.cache, "changed.tmp", "changed-size");
    const auto newcomer = files.put(files.cache, "new.tmp");
    const auto cleaned = cleanup(files.handle, first.at("preview_id"));
    CHECK(cleaned.value("ok", false));
    CHECK(cleaned.at("output").at("removed_files") == 1);
    CHECK(cleaned.at("output").at("skipped_files") == 1);
    CHECK(MaiFileSystem::exists(changed));
    CHECK(MaiFileSystem::exists(newcomer));
    CHECK(!MaiFileSystem::exists(removable));

    const auto second = preview(files.handle, "cache");
    void* other = files.create();
    CHECK(other && other != files.handle);
    const auto otherPreview = preview(other, "cache");
    CHECK(!cleanup(other, second.at("preview_id")).value("ok", true));
    CHECK(cleanup(other, otherPreview.at("preview_id")).value("ok", false));
    maiMobileAppStorageDestroy(other);
    const auto replacement = preview(files.handle, "cache");
    CHECK(!cleanup(files.handle, second.at("preview_id")).value("ok", true));
    CHECK(cleanup(files.handle, replacement.at("preview_id")).value("ok", false));
}

void strictProtocolAndClosedHandle() {
    Fixture files;
    const auto kept = files.put(files.workspace, "final.png");
    for (const auto* invalid :
         {"", "[]", "{}", R"({"action":1})", R"({"action":"scan"})", R"({"action":"preview","target":"workspace"})",
          R"({"action":"preview","target":null})", R"({"action":"preview","min_age_hours":0})",
          R"({"action":"preview","scope":"frames_1"})", R"({"action":"preview","patterns":["*.tmp"]})",
          R"({"action":"preview","action":"cleanup","preview_id":"x"})", R"({"action":"preview"} {})",
          R"({"action":"cleanup","preview_id":"x","target":"workspace"})",
          R"({"action":"cleanup","preview_id":"x","dry_run":false})", R"({"action":"cleanup","preview_id":1})"})
        rejected(files.handle, invalid);
    CHECK(MaiFileSystem::exists(kept));
    CHECK(!maiMobileAppStorageCreate(nullptr, files.cache.toUtf8().c_str(), files.workspace.toUtf8().c_str()));
    CHECK(!maiMobileAppStorageCreate("relative", files.cache.toUtf8().c_str(), files.workspace.toUtf8().c_str()));
    void* overlap = maiMobileAppStorageCreate(files.cache.toUtf8().c_str(), files.cache.toUtf8().c_str(),
                                              files.workspace.toUtf8().c_str());
    CHECK(overlap);
    rejected(overlap, R"({"action":"preview"})");
    maiMobileAppStorageDestroy(overlap);
    const auto nonexistent = files.root.append(MaiFilePath::fromUtf8("not-created"));
    void* missing = maiMobileAppStorageCreate(nonexistent.toUtf8().c_str(), files.cache.toUtf8().c_str(),
                                              files.workspace.toUtf8().c_str());
    CHECK(missing);
    rejected(missing, R"({"action":"preview"})");
    CHECK(!MaiFileSystem::exists(nonexistent));
    maiMobileAppStorageDestroy(missing);
    const auto closed = files.handle;
    maiMobileAppStorageDestroy(closed);
    maiMobileAppStorageDestroy(closed);
    rejected(closed, R"({"action":"preview"})");
    rejected(nullptr, R"({"action":"preview"})");
    files.handle = files.create();
    CHECK(files.handle && files.handle != closed);
    rejected(closed, R"({"action":"cleanup","preview_id":"x"})");
    maiMobileAppStorageFree(nullptr);
}

void fixedCandidateLimit() {
    Fixture files;
    for (int i = 0; i < 1002; ++i) files.put(files.cache, std::to_string(i) + ".tmp");
    const auto cache = preview(files.handle, "cache");
    CHECK(cache.at("candidate_files") == 1000);
    CHECK(cache.at("candidate_bytes") == 4000);
    CHECK(cache.at("limited").get<bool>());
    CHECK(cache.at("summaries").at(0).at("eligible_files") == 1002);
    const auto cleaned = cleanup(files.handle, cache.at("preview_id"));
    CHECK(cleaned.value("ok", false));
    CHECK(cleaned.at("output").at("removed_files") == 1000);
    CHECK(preview(files.handle, "cache").at("candidate_files") == 2);
}
}  // namespace

int main() {
    previewAndOneCategoryCleanup();
    exactCandidateAndTokenBinding();
    strictProtocolAndClosedHandle();
    fixedCandidateLimit();
    std::printf("MaiAppStorageBridgeTests: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
