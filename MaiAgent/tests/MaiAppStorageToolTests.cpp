// 文件作用：只在自建临时目录验证存储预览、年龄、保护及单次执行边界。
#include "MaiAppStorageTool.h"

#include <json.hpp>

#include <atomic>
#include <cstdio>
#include <ctime>
#include <string>
#include <unordered_set>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#include <utime.h>
#endif

#include "MaiAgent.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiMemoryStore.h"

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
    Fixture() {
        root = MaiFileSystem::temporaryDirectory().append(
            MaiFilePath::fromUtf8(MaiIdGenerator::generate("storage_test_")));
        temporary = root.append(MaiFilePath::fromUtf8("temporary"));
        cache = root.append(MaiFilePath::fromUtf8("cache"));
        workspace = root.append(MaiFilePath::fromUtf8("workspace"));
        CHECK(!MaiFileSystem::createDirectories(temporary));
        CHECK(!MaiFileSystem::createDirectories(cache));
        CHECK(!MaiFileSystem::createDirectories(workspace));
    }
    ~Fixture() {
        MaiFileSystem::removeRecursively(root);
    }
    MaiAppStoragePaths paths() const {
        return {temporary.toUtf8(), cache.toUtf8(), workspace.toUtf8()};
    }
    MaiAppStorageContext context() const {
        return {"session", workspace.toUtf8(), {}, nullptr, true};
    }
    MaiToolContext toolContext() const {
        MaiToolContext context;
        context.sessionId = "session";
        context.root = workspace.toUtf8();
        return context;
    }
    MaiFilePath put(const MaiFilePath& base, const std::string& relative,
                    const std::string& bytes = "data") {
        const auto path = base.append(MaiFilePath::fromUtf8(relative));
        CHECK(!MaiFileSystem::createDirectories(path.dirName()));
        CHECK(!MaiFileSystem::writeFile(path, bytes));
        return path;
    }
};
void modifiedTime(const MaiFilePath& path, std::int64_t old) {
#if defined(_WIN32)
    const auto handle =
        ::CreateFileW(path.value().c_str(), FILE_WRITE_ATTRIBUTES,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(handle != INVALID_HANDLE_VALUE);
    ULARGE_INTEGER value{};
    value.QuadPart = static_cast<std::uint64_t>(old) * 10000000ull + 116444736000000000ull;
    FILETIME time{value.LowPart, value.HighPart};
    CHECK(::SetFileTime(handle, nullptr, &time, &time));
    ::CloseHandle(handle);
#else
    utimbuf times{old, old};
    CHECK(::utime(path.value().c_str(), &times) == 0);
#endif
}
void age(const MaiFilePath& path) {
    modifiedTime(path, std::time(nullptr) - 26 * 3600);
}
MaiAppStorageSelection workspaceScope(const std::string& scope) {
    MaiAppStorageSelection result;
    result.target = MaiAppStorageTarget::Workspace;
    result.scope = scope;
    result.minimumAgeHours = 0;
    return result;
}
Json output(const MaiToolResult& result) {
    CHECK(!result.hasError());
    return result.hasError() ? Json::object() : Json::parse(result.output());
}
std::string executeRequest(const std::string& id) {
    return Json{{"action", "cleanup"}, {"dry_run", false}, {"preview_id", id}}.dump();
}

void ageAndDefaultPreview() {
    Fixture files;
    const auto model = files.put(files.temporary, "qa-model/public-fixture.gguf", "GGUFtest");
    files.put(files.workspace, "final.jpg", "final");
    for (int i = 0; i < 25; ++i) age(files.put(files.cache, "old-" + std::to_string(i) + ".tmp"));
    auto tool = makeMaiAppStorageTool(files.paths());
    auto context = files.toolContext();
    const auto legacy = output(tool->execute(R"({"action":"scan"})", context));
    CHECK(legacy.value("cleanup_candidate_count", 0) == 25);
    CHECK(!legacy.contains("cleanup_candidates"));
    CHECK(legacy.value("cleanup_candidate_bytes", 0) == 100);
    CHECK(!legacy.contains("cleanup_max_age_hours"));
    CHECK(legacy.value("cleanup_min_age_hours", 0) == 24);
    CHECK(!legacy.value("workspace_cleanup_allowed", true));
    CHECK(MaiFileSystem::exists(model));
    MaiAppStorage service(files.paths());
    MaiAppStorageSelection selection;
    selection.target = MaiAppStorageTarget::Temporary;
    selection.scope = "qa-model";
    const auto fresh = service.preview(selection, files.context());
    CHECK(fresh && fresh.value().candidates.empty());
    CHECK(fresh && fresh.value().summaries[0].excludedRecentFiles == 1);
    age(model);
    const auto aged = service.preview(selection, files.context());
    CHECK(aged && aged.value().candidates.size() == 1);
    CHECK(aged && aged.value().candidateBytes == 8);
    selection.minimumAgeHours = 0;
    const auto explicitFresh = service.preview(selection, files.context());
    CHECK(explicitFresh && explicitFresh.value().candidates.size() == 1);
    const auto defaultAgain = output(tool->execute(R"({"action":"scan"})", context));
    CHECK(defaultAgain.value("cleanup_candidate_count", 0) == 25);
    CHECK(defaultAgain.value("temporary", Json::object()).value("excluded_protected_files", 0) ==
          1);
    CHECK(
        !tool->requiresApproval(R"({"action":"cleanup","target":"temporary","scope":"qa-model"})"));
    CHECK(!tool->requiresPerCallApproval(R"({"action":"scan"})"));
    CHECK(tool->requiresPerCallApproval(executeRequest("x")));
    CHECK(tool->requiresApproval(R"({"action":"cleanup","dry_run":"false"})"));
    const auto dry = output(tool->execute(
        R"({"action":"cleanup","target":"temporary","scope":"qa-model","min_age_hours":0})",
        context));
    CHECK(dry.value("dry_run", false));
    CHECK(MaiFileSystem::exists(model));
    CHECK(tool->execute(
                  R"({"action":"cleanup","dry_run":false,"preview_id":"x","target":"temporary"})",
                  context)
              .hasError());
    CHECK(tool->execute(R"({"action":"cleanup","target":"temporary","min_age_hours":0})", context)
              .hasError());
    const auto cleaned =
        output(tool->execute(executeRequest(dry.value("preview_id", "")), context));
    CHECK(cleaned.value("removed_files", 0) == 1);
    CHECK(!MaiFileSystem::exists(model));
}

void exactPreviewAndChangedFiles() {
    Fixture files;
    const auto kept = files.put(files.workspace, "screenshots/gallery-original.png");
    const auto delivered = files.put(files.workspace, "screenshots/delivered.png");
    const auto changed = files.put(files.workspace, "screenshots/changed.png");
    const auto replaced = files.put(files.workspace, "screenshots/replaced.png");
    const auto removable = files.put(files.workspace, "screenshots/removable.png");
    const auto laterDelivered = files.put(files.workspace, "screenshots/later-delivered.png");
    const auto final = files.put(files.workspace, "final-output.mp4");
    MaiAppStorage service(files.paths());
    auto context = files.context();
    context.protectedPaths = {delivered.toUtf8()};
    const auto preview = service.preview(workspaceScope("screenshots"), context);
    CHECK(preview && preview.value().candidates.size() == 4);
    CHECK(preview && preview.value().candidateBytes == 16);
    CHECK(MaiFileSystem::exists(removable));
    const auto newcomer = files.put(files.workspace, "screenshots/new-after-preview.png");
    const auto saved = service.readPreview(preview.value().id, context);
    CHECK(saved && saved.value().id == preview.value().id);
    CHECK(saved && saved.value().candidates.size() == 4);
    CHECK(saved && saved.value().candidateBytes == 16);
    std::int64_t previousModified = 0;
    CHECK(MaiFileSystem::modifiedTime(changed, previousModified));
    CHECK(!MaiFileSystem::writeFile(changed, "edit"));
    modifiedTime(changed, previousModified);
    std::int64_t restored = 0;
    CHECK(MaiFileSystem::modifiedTime(changed, restored) && restored == previousModified);
    CHECK(!MaiFileSystem::removeFile(replaced));
    CHECK(!MaiFileSystem::writeFile(replaced, "data"));
    context.protectedPaths.push_back(laterDelivered.toUtf8());
    auto other = context;
    other.sessionId = "other";
    CHECK(!service.readPreview(preview.value().id, other));
    CHECK(!service.cleanup(preview.value().id, other));
    other = context;
    other.workspace = files.cache.toUtf8();
    CHECK(!service.readPreview(preview.value().id, other));
    CHECK(!service.cleanup(preview.value().id, other));
    CHECK(service.readPreview(preview.value().id, context));
    const auto cleanup = service.cleanup(preview.value().id, context);
    CHECK(cleanup && cleanup.value().removedFiles == 1);
    CHECK(cleanup && cleanup.value().skippedFiles == 3);
    CHECK(!MaiFileSystem::exists(removable));
    for (const auto& path : {kept, delivered, changed, replaced, laterDelivered, newcomer, final})
        CHECK(MaiFileSystem::exists(path));
    CHECK(!service.cleanup(preview.value().id, context));
    CHECK(!service.readPreview(preview.value().id, context));
    MaiRegularFileInfo info;
    CHECK(!MaiFileSystem::regularFileInfo(files.workspace, info));
    CHECK(MaiFileSystem::regularFileInfo(changed, info));
}

void scopeBoundaries() {
    Fixture files;
    MaiAppStorage service(files.paths());
    auto context = files.context();
    for (const auto& scope :
         {"", ".", "..", "/", "*", "final", "../screenshots", "screenshots/../final",
          "screenshots\\x", ".maiagent", "gallery-x", "screenshots//x"})
        CHECK(!service.preview(workspaceScope(scope), context));
    for (const auto& scope : {"frames_run", "frames_*", "wmframes", "screenshots", "search",
                              "browser", ".maiagent/model-previews"}) {
        if (std::string(scope) != "frames_*")
            files.put(files.workspace, std::string(scope) + "/one.bin");
        const auto preview = service.preview(workspaceScope(scope), context);
        if (!preview)
            std::printf("scope-preview-error scope=%s error=%s\n", scope,
                        preview.error().message().c_str());
        CHECK(preview);
    }
    auto selection = workspaceScope("");
    selection.patterns = {"screenshots/*.bin"};
    CHECK(service.preview(selection, context).value().candidates.size() == 1);
    selection.patterns = {"*"};
    CHECK(!service.preview(selection, context));
    selection.patterns = {"screenshots/**"};
    CHECK(!service.preview(selection, context));
    selection.patterns = {"final/*.jpg"};
    CHECK(!service.preview(selection, context));
    selection = {};
    selection.target = MaiAppStorageTarget::Temporary;
    selection.minimumAgeHours = 0;
    CHECK(!service.preview(selection, context));
    auto empty = context;
    empty.sessionId.clear();
    CHECK(!service.preview({}, empty));
    std::atomic<bool> cancel{true};
    context.cancel = &cancel;
    CHECK(!service.preview(workspaceScope("screenshots"), context));
#ifndef _WIN32
    const auto outside = files.put(files.root, "outside/external.png");
    const auto link = files.workspace.append(MaiFilePath::fromUtf8("screenshots/link.png"));
    CHECK(::symlink(outside.value().c_str(), link.value().c_str()) == 0);
    context.cancel = nullptr;
    const auto preview = service.preview(workspaceScope("screenshots"), context);
    CHECK(preview && preview.value().candidates.size() == 1);
    MaiRegularFileInfo info;
    CHECK(!MaiFileSystem::regularFileInfo(link, info));
    const auto linkedDirectory = files.workspace.append(MaiFilePath::fromUtf8("frames_escape"));
    CHECK(::symlink(outside.dirName().value().c_str(), linkedDirectory.value().c_str()) == 0);
    CHECK(!service.preview(workspaceScope("frames_escape"), context));
    CHECK(MaiFileSystem::exists(outside));
#endif
}

void recordedDeliveryAndToolValidation() {
    Fixture files;
    files.put(files.workspace, "screenshots/keep.png");
    files.put(files.workspace, "screenshots/attachment.png");
    const auto candidate = files.put(files.workspace, "screenshots/temporary.png");
    const auto lateDelivery = files.put(files.workspace, "screenshots/new-delivery.png");
    auto store = makeMaiMemoryStore();
    auto* memory = store.get();
    MaiSession session;
    session.id = "session";
    session.directory = files.workspace.toUtf8();
    memory->putSession(session);
    MaiSession other = session;
    other.id = "other";
    other.parentId = session.id;
    memory->putSession(other);
    MaiMessage message;
    message.id = "message";
    MaiToolPart delivery;
    delivery.tool = "agent_send_media";
    delivery.state = MaiToolState::Completed;
    delivery.output =
        R"({"delivery":"current_ai_session","path":"screenshots/keep.png","type":"image"})";
    message.parts.push_back({"delivery", delivery, 0});
    message.parts.push_back(
        {"attachment", MaiImagePart{"screenshots/attachment.png", "image/png"}, 0});
    memory->putMessage(other.id, message);
    MaiAgent agent(std::move(store), nullptr);
    auto context = files.toolContext();
    context.sessionAgent = &agent;
    auto tool = makeMaiAppStorageTool(files.paths());
    const auto preview = output(tool->execute(
        R"({"action":"cleanup","target":"workspace","scope":"screenshots","min_age_hours":0,"dry_run":true})",
        context));
    CHECK(preview.value("cleanup_candidate_count", 0) == 2);
    CHECK(!preview.contains("cleanup_candidates"));
    CHECK(preview.value("workspace_cleanup_allowed", false));
    CHECK(preview.value("target", "") == "workspace");
    CHECK(preview.value("scope", "") == "screenshots");
    const auto page = output(tool->execute(Json{{"action", "scan"},
                                                {"preview_id", preview.value("preview_id", "")},
                                                {"detail", true},
                                                {"limit", 1}}
                                               .dump(),
                                           context));
    CHECK(page.value("preview_id", "") == preview.value("preview_id", ""));
    CHECK(page.value("detail_total_count", 0) == 2);
    context.sessionAgent = nullptr;
    CHECK(tool->execute(executeRequest(preview.value("preview_id", "")), context).hasError());
    context.sessionAgent = &agent;
    message.id = "new-delivery";
    delivery.output =
        R"({"delivery":"current_ai_session","path":"screenshots/new-delivery.png","type":"image"})";
    message.parts = {{"new-delivery", delivery, 0}};
    memory->putMessage(other.id, message);
    const auto result =
        output(tool->execute(executeRequest(preview.value("preview_id", "")), context));
    CHECK(result.value("removed_files", 0) == 1);
    CHECK(result.value("skipped_files", 0) == 1);
    CHECK(!MaiFileSystem::exists(candidate));
    CHECK(MaiFileSystem::exists(lateDelivery));
    CHECK(MaiFileSystem::exists(
        files.workspace.append(MaiFilePath::fromUtf8("screenshots/keep.png"))));
    const auto schema = Json::parse(tool->parametersSchema());
    CHECK(schema.value("properties", Json::object())
              .value("dry_run", Json::object())
              .value("default", false));
    for (const auto& raw :
         {R"({"action":"cleanup","target":"workspace"})", R"({"action":"scan","oops":1})",
          R"({"action":"scan","patterns":[1]})", R"({"action":"scan","min_age_hours":0.5})",
          R"({"action":"scan","target":"workspace","scope":"*"})"})
        CHECK(tool->execute(raw, context).hasError());
    context.sessionAgent = nullptr;
    CHECK(tool->execute(R"({"action":"scan","target":"workspace","scope":"screenshots"})", context)
              .hasError());
}

void boundedList() {
    Fixture files;
    for (int i = 0; i < 1002; ++i)
        files.put(files.workspace, "frames_limit/" + std::to_string(i) + ".png", "x");
    MaiAppStorage service(files.paths());
    const auto result = service.preview(workspaceScope("frames_limit"), files.context());
    CHECK(result && result.value().limited && result.value().candidates.size() == 1000);
    CHECK(result && result.value().candidateBytes == 1000);
    CHECK(result && result.value().summaries[0].eligibleFiles == 1002);
    auto tool = makeMaiAppStorageTool(
        {files.temporary.toUtf8(), files.workspace.toUtf8(), files.cache.toUtf8()});
    auto context = files.toolContext();
    context.root = files.cache.toUtf8();
    const auto summary = output(tool->execute(
        R"({"action":"scan","target":"cache","scope":"frames_limit","min_age_hours":0})", context));
    CHECK(summary.value("cleanup_candidate_count", 0) == 1000);
    CHECK(summary.value("cleanup_candidate_bytes", 0) == 1000);
    CHECK(summary.value("cleanup_limited", false));
    CHECK(summary.value("scan_complete", false));
    CHECK(summary.value("eligible_totals_exact", false));
    CHECK(!summary.value("cleanup_candidates_complete", true));
    CHECK(summary.value("cache", Json::object()).value("eligible_files", 0) == 1002);
}

void defaultSummaryDoesNotInlineCandidates() {
    Fixture files;
    // 复现现场候选数量，但只写入本测试自建目录；本用例只请求扫描和 dry-run。
    for (int index = 0; index < 971; ++index)
        age(files.put(files.cache, "owned-cache/entry-" + std::to_string(index) + ".tmp"));
    auto tool = makeMaiAppStorageTool(files.paths());
    const auto context = files.toolContext();
    for (const auto* request :
         {R"({"action":"scan"})", R"({"action":"cleanup","target":"cache","dry_run":true})"}) {
        const auto result = tool->execute(request, context);
        const auto summary = output(result);
        std::printf("storage-summary-repro action=%s output_bytes=%zu candidates=%d inline=%zu\n",
                    Json::parse(request)["action"].get<std::string>().c_str(),
                    result.output().size(), summary.value("cleanup_candidate_count", 0),
                    summary.value("cleanup_candidates", Json::array()).size());
        CHECK(summary.value("cleanup_candidate_count", 0) == 971);
        CHECK(summary.value("cleanup_candidate_bytes", 0) == 3884);
        CHECK(!summary.value("preview_id", "").empty());
        CHECK(summary.value("dry_run", false));
        CHECK(!summary.value("cleanup_limited", true));
        CHECK(summary.value("cleanup_candidates", Json::array()).empty());
        CHECK(result.output().size() < 4096);
        const auto cache = summary.value("cache", Json::object());
        CHECK(cache.value("files", 0) == 971);
        CHECK(cache.value("bytes", 0) == 3884);
        CHECK(cache.value("eligible_files", 0) == 971);
        CHECK(cache.value("eligible_bytes", 0) == 3884);
        CHECK(!cache.value("scan_truncated", true));
    }
    CHECK(MaiFileSystem::exists(
        files.cache.append(MaiFilePath::fromUtf8("owned-cache/entry-970.tmp"))));
}

void detailPagesKeepTheSamePreview() {
    Fixture files;
    for (int index = 0; index < 971; ++index)
        age(files.put(files.cache, "owned-cache/entry-" + std::to_string(index) + ".tmp"));
    auto tool = makeMaiAppStorageTool(files.paths());
    const auto context = files.toolContext();
    const auto summary = output(tool->execute(R"({"action":"scan","target":"cache"})", context));
    const auto id = summary.value("preview_id", "");
    CHECK(summary.value("scan_complete", false));
    CHECK(summary.value("eligible_totals_exact", false));
    CHECK(summary.value("cleanup_candidates_complete", false));
    // 后续页读取已保存集合，不能重新扫描把后来创建的文件补进审批预览。
    age(files.put(files.cache, "owned-cache/later.tmp"));
    std::unordered_set<std::string> paths;
    std::size_t offset = 0;
    std::uint64_t totalBytes = 0;
    for (int pageIndex = 0; pageIndex < 20; ++pageIndex) {
        const auto request = Json{{"action", "scan"},
                                  {"preview_id", id},
                                  {"detail", true},
                                  {"offset", offset},
                                  {"limit", 100}};
        const auto result = tool->execute(request.dump(), context);
        CHECK(!result.hasError());
        if (result.hasError()) return;
        const auto page = output(result);
        CHECK(page.value("preview_id", "") == id);
        CHECK(page.value("detail_total_count", 0) == 971);
        CHECK(page.value("detail_total_bytes", 0) == 3884);
        const auto entries = page.value("cleanup_candidates", Json::array());
        CHECK(!entries.empty());
        CHECK(entries.size() <= 100);
        CHECK(page.value("detail_returned_count", 0u) == entries.size());
        CHECK(page.value("detail_returned_bytes", 0u) == entries.size() * 4);
        CHECK(result.output().size() <= 65536);
        for (const auto& entry : entries) {
            CHECK(entry.value("relative_path", "") != "owned-cache/later.tmp");
            CHECK(paths.insert(entry.value("path", "")).second);
            totalBytes += entry.value("bytes", 0u);
        }
        if (!page.value("has_more", false)) {
            CHECK(page.value("next_offset", Json{}).is_null());
            break;
        }
        const auto next = page.value("next_offset", std::size_t{});
        CHECK(next == offset + entries.size());
        CHECK(next > offset);
        offset = next;
    }
    CHECK(paths.size() == 971);
    CHECK(totalBytes == 3884);
    auto other = context;
    other.sessionId = "other";
    const auto read = Json{{"action", "scan"}, {"preview_id", id}, {"detail", true}}.dump();
    CHECK(tool->execute(read, other).hasError());
    CHECK(!tool->execute(read, context).hasError());
    CHECK(tool->requiresPerCallApproval(executeRequest(id)));
    CHECK(!tool->requiresPerCallApproval(read));
    CHECK(
        tool->execute(
                Json{
                    {"action", "cleanup"}, {"preview_id", id}, {"dry_run", false}, {"detail", true}}
                    .dump(),
                context)
            .hasError());
    CHECK(!tool->execute(read, context).hasError());
    // 重新预览替换唯一快照，旧分页 ID 与旧执行 ID 同时失效。
    output(tool->execute(R"({"action":"scan","target":"cache"})", context));
    CHECK(tool->execute(read, context).hasError());
}

void detailPageByteBudget() {
    Fixture files;
    std::string longName;
    for (int index = 0; index < 60; ++index) longName += "\U0001f600";
    for (int index = 0; index < 110; ++index)
        age(files.put(files.cache, longName + "-" + std::to_string(index) + ".tmp"));
    auto tool = makeMaiAppStorageTool(files.paths());
    const auto context = files.toolContext();
    const auto result =
        tool->execute(R"({"action":"scan","target":"cache","detail":true,"limit":100})", context);
    CHECK(!result.hasError());
    if (result.hasError()) return;
    const auto page = output(result);
    std::printf("storage-detail-budget output_bytes=%zu returned=%d total=%d\n",
                result.output().size(), page.value("detail_returned_count", 0),
                page.value("detail_total_count", 0));
    CHECK(result.output().size() <= 65536);
    CHECK(page.value("detail_returned_count", 0) > 0);
    CHECK(page.value("detail_returned_count", 100) < 100);
    CHECK(page.value("has_more", false));
    CHECK(page.value("next_offset", 0) == page.value("detail_returned_count", -1));
}

void emptySummaryIsAnExactZero() {
    Fixture files;
    auto tool = makeMaiAppStorageTool(files.paths());
    const auto summary = output(tool->execute(R"({"action":"scan"})", files.toolContext()));
    CHECK(summary.value("cleanup_candidate_count", -1) == 0);
    CHECK(summary.value("cleanup_candidate_bytes", -1) == 0);
    CHECK(summary.value("scan_complete", false));
    CHECK(summary.value("eligible_totals_exact", false));
    for (const auto* target : {"temporary", "cache", "workspace"}) {
        const auto category = summary.value(target, Json::object());
        CHECK(category.value("files", -1) == 0);
        CHECK(category.value("totals_exact", false));
        CHECK(category.value("scan_error_count", -1) == 0);
    }
}

void incompleteZeroIsNotAnExactZero() {
    Fixture files;
    // 目录也计入遍历预算；超过 20000 个条目后，观察到零文件不代表完整扫描确认零文件。
    for (int index = 0; index < 20001; ++index)
        CHECK(!MaiFileSystem::createDirectories(
            files.cache.append(MaiFilePath::fromUtf8("owned-dir-" + std::to_string(index)))));
    auto tool = makeMaiAppStorageTool(files.paths());
    const auto summary =
        output(tool->execute(R"({"action":"scan","target":"cache"})", files.toolContext()));
    CHECK(summary.value("cleanup_candidate_count", -1) == 0);
    CHECK(!summary.value("scan_complete", true));
    CHECK(!summary.value("eligible_totals_exact", true));
    CHECK(!summary.value("cleanup_candidates_complete", true));
    const auto cache = summary.value("cache", Json::object());
    CHECK(cache.value("files", -1) == 0);
    CHECK(cache.value("scan_truncated", false));
    CHECK(!cache.value("totals_exact", true));
    CHECK(cache.value("scan_error_count", -1) == 0);
}

void detailValidationAndLimitClamping() {
    Fixture files;
    for (int index = 0; index < 3; ++index)
        age(files.put(files.cache, "owned-" + std::to_string(index)));
    auto tool = makeMaiAppStorageTool(files.paths());
    const auto context = files.toolContext();
    const auto first = output(
        tool->execute(R"({"action":"scan","target":"cache","detail":true,"limit":0})", context));
    CHECK(first.value("detail_limit", 0) == 1);
    CHECK(first.value("detail_returned_count", 0) == 1);
    const auto id = first.value("preview_id", "");
    const auto request = Json{{"action", "cleanup"},
                              {"detail", true},
                              {"preview_id", id},
                              {"offset", 3},
                              {"limit", 1000}};
    const auto last = output(tool->execute(request.dump(), context));
    CHECK(last.value("detail_limit", 0) == 100);
    CHECK(last.value("detail_returned_count", -1) == 0);
    CHECK(!last.value("has_more", true));
    CHECK(last.value("next_offset", Json{}).is_null());
    for (const auto* raw : {R"({"action":"scan","detail":1})", R"({"action":"scan","offset":0})",
                            R"({"action":"scan","detail":true,"offset":-1})",
                            R"({"action":"scan","detail":true,"offset":0.5})",
                            R"({"action":"scan","detail":true,"limit":0.5})"})
        CHECK(tool->execute(raw, context).hasError());
    auto invalid = request;
    invalid["offset"] = 4;
    CHECK(tool->execute(invalid.dump(), context).hasError());
    invalid = request;
    invalid["target"] = "cache";
    CHECK(tool->execute(invalid.dump(), context).hasError());
    invalid = request;
    invalid["detail"] = false;
    CHECK(tool->execute(invalid.dump(), context).hasError());
    CHECK(!tool->execute(request.dump(), context).hasError());
}

void excludedClassificationsIncludeBytes() {
    Fixture files;
    age(files.put(files.cache, "eligible.tmp", std::string(5, 'e')));
    files.put(files.cache, "recent.tmp", std::string(6, 'r'));
    age(files.put(files.cache, "gallery-original.tmp", std::string(7, 'g')));
    age(files.put(files.temporary, "active-model.gguf", std::string(8, 'm')));
    age(files.put(files.workspace, "final.mp4", std::string(9, 'f')));
    age(files.put(files.workspace, "screenshots/intermediate.png", std::string(10, 'i')));
    const auto delivered = files.put(files.cache, "delivered.tmp", std::string(11, 'd'));
    age(delivered);

    MaiAppStorage service(files.paths());
    auto ordinaryContext = files.context();
    ordinaryContext.protectedPaths = {delivered.toUtf8()};
    const auto ordinary = service.preview({}, ordinaryContext);
    CHECK(ordinary && ordinary.value().candidates.size() == 1);
    CHECK(ordinary && ordinary.value().candidateBytes == 5);
    std::uint64_t ordinaryProtectedBytes = 0, ordinaryPolicyBytes = 0;
    if (ordinary) {
        for (const auto& category : ordinary.value().summaries) {
            CHECK(category.bytes == category.eligibleBytes + category.excludedRecentBytes +
                                        category.excludedProtectedBytes +
                                        category.excludedPolicyBytes);
            ordinaryProtectedBytes += category.excludedProtectedBytes;
            ordinaryPolicyBytes += category.excludedPolicyBytes;
        }
        const auto saved = service.readPreview(ordinary.value().id, ordinaryContext);
        CHECK(saved && saved.value().summaries[0].excludedProtectedBytes == 8);
        CHECK(saved && saved.value().summaries[2].excludedPolicyBytes == 19);
    }
    CHECK(ordinaryProtectedBytes == 26);
    CHECK(ordinaryPolicyBytes == 19);

    // 普通扫描与工具入口共享分类；实际交付收据、gallery 和默认模型均进入保护分支。
    auto store = makeMaiMemoryStore();
    MaiSession session;
    session.id = "session";
    session.directory = files.workspace.toUtf8();
    store->putSession(session);
    MaiMessage message;
    message.id = "delivery";
    message.parts.push_back({"attachment", MaiImagePart{delivered.toUtf8(), "image/png"}, 0});
    store->putMessage(session.id, message);
    MaiAgent agent(std::move(store), nullptr);
    auto context = files.toolContext();
    context.sessionAgent = &agent;
    auto tool = makeMaiAppStorageTool(files.paths());
    const auto summary = output(tool->execute(R"({"action":"scan"})", context));
    CHECK(summary.dump().size() < 4096);
    CHECK(!summary.contains("cleanup_candidates"));
    CHECK(summary.value("cleanup_candidate_count", 0) == 1);
    CHECK(summary.value("cleanup_candidate_bytes", 0) == 5);
    CHECK(summary.value("eligible_totals_exact", false));
    std::uint64_t protectedBytes = 0, policyBytes = 0, classifiedBytes = 0;
    for (const auto* target : {"temporary", "cache", "workspace"}) {
        const auto category = summary.value(target, Json::object());
        const auto protectedCount = category.value("excluded_protected_files", -1);
        const auto protectedSize = category.value("excluded_protected_bytes", -1);
        const auto policyCount = category.value("excluded_outside_policy_files", -1);
        const auto policySize = category.value("excluded_outside_policy_bytes", -1);
        if (std::string(target) == "temporary") {
            CHECK(protectedCount == 1 && protectedSize == 8);
            CHECK(policyCount == 0 && policySize == 0);
        } else if (std::string(target) == "cache") {
            CHECK(protectedCount == 2 && protectedSize == 18);
            CHECK(policyCount == 0 && policySize == 0);
            CHECK(category.value("excluded_recent_files", -1) == 1);
            CHECK(category.value("excluded_recent_bytes", -1) == 6);
        } else {
            CHECK(protectedCount == 0 && protectedSize == 0);
            CHECK(policyCount == 2 && policySize == 19);
        }
        CHECK(category.value("bytes", -1) == category.value("eligible_bytes", -1) +
                                                 category.value("excluded_recent_bytes", -1) +
                                                 protectedSize + policySize);
        CHECK(category.value("files", -1) == category.value("eligible_files", -1) +
                                                 category.value("excluded_recent_files", -1) +
                                                 protectedCount + policyCount);
        if (protectedSize >= 0) protectedBytes += protectedSize;
        if (policySize >= 0) policyBytes += policySize;
        classifiedBytes += category.value("bytes", 0u);
    }
    CHECK(protectedBytes == 26);
    CHECK(policyBytes == 19);
    CHECK(classifiedBytes == 56);
    const auto page = output(tool->execute(
        Json{{"action", "scan"}, {"preview_id", summary.value("preview_id", "")}, {"detail", true}}
            .dump(),
        context));
    CHECK(page.value("cache", Json::object()).value("excluded_protected_bytes", -1) == 18);
    CHECK(page.value("workspace", Json::object()).value("excluded_outside_policy_bytes", -1) == 19);
    CHECK(MaiFileSystem::exists(delivered));
}
}  // namespace

int main(int count, char** arguments) {
    if (count == 2 && std::string(arguments[1]) == "--classification-only") {
        excludedClassificationsIncludeBytes();
        std::printf("MaiAppStorageToolTests classification-only: %d failures\n", failures);
        return failures ? 1 : 0;
    }
    if (count == 2 && std::string(arguments[1]) == "--summary-only") {
        defaultSummaryDoesNotInlineCandidates();
        detailPagesKeepTheSamePreview();
        detailPageByteBudget();
        emptySummaryIsAnExactZero();
        incompleteZeroIsNotAnExactZero();
        detailValidationAndLimitClamping();
        excludedClassificationsIncludeBytes();
        std::printf("MaiAppStorageToolTests summary-only: %d failures\n", failures);
        return failures ? 1 : 0;
    }
    ageAndDefaultPreview();
    exactPreviewAndChangedFiles();
    scopeBoundaries();
    recordedDeliveryAndToolValidation();
    boundedList();
    defaultSummaryDoesNotInlineCandidates();
    detailPagesKeepTheSamePreview();
    detailPageByteBudget();
    emptySummaryIsAnExactZero();
    incompleteZeroIsNotAnExactZero();
    detailValidationAndLimitClamping();
    excludedClassificationsIncludeBytes();
    std::printf("MaiAppStorageToolTests: %d failures\n", failures);
    return failures ? 1 : 0;
}
