// 文件作用：只在自建临时目录验证存储预览、年龄、保护及单次执行边界。
#include "MaiAppStorageTool.h"

#include <json.hpp>

#include <atomic>
#include <cstdio>
#include <ctime>
#include <string>

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
    CHECK(legacy.value("cleanup_candidates", Json::array()).size() == 25);
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
    CHECK(!service.cleanup(preview.value().id, other));
    other = context;
    other.workspace = files.cache.toUtf8();
    CHECK(!service.cleanup(preview.value().id, other));
    const auto cleanup = service.cleanup(preview.value().id, context);
    CHECK(cleanup && cleanup.value().removedFiles == 1);
    CHECK(cleanup && cleanup.value().skippedFiles == 3);
    CHECK(!MaiFileSystem::exists(removable));
    for (const auto& path : {kept, delivered, changed, replaced, laterDelivered, newcomer, final})
        CHECK(MaiFileSystem::exists(path));
    CHECK(!service.cleanup(preview.value().id, context));
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
        CHECK(service.preview(workspaceScope(scope), context));
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
    CHECK(preview.value("cleanup_candidates", Json::array()).size() == 2);
    CHECK(preview.value("workspace_cleanup_allowed", false));
    CHECK(preview.value("target", "") == "workspace");
    CHECK(preview.value("scope", "") == "screenshots");
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
}
}  // namespace

int main() {
    ageAndDefaultPreview();
    exactPreviewAndChangedFiles();
    scopeBoundaries();
    recordedDeliveryAndToolValidation();
    boundedList();
    std::printf("MaiAppStorageToolTests: %d failures\n", failures);
    return failures ? 1 : 0;
}
