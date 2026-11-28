// 文件作用：验证分组帮助仅查询当前模型可见的元数据，不执行工具。
#include <atomic>
#include <cstdio>
#include <memory>
#include <string>

#include <json.hpp>

#include "MaiArkMediaTools.h"
#include "MaiMemoryStore.h"
#include "MaiModelToolCatalog.h"
#include "MaiSessionStore.h"
#include "MaiTool.h"
#include "MaiToolHelp.h"

static int failures = 0;
#define CHECK(value)                                       \
    do {                                                   \
        if (!(value)) {                                    \
            std::printf("FAIL %d %s\n", __LINE__, #value); \
            ++failures;                                    \
        }                                                  \
    } while (0)

namespace {
// Schema、执行和凭据快照都设探针，避免帮助绕到会读取配置的完整工具路径。
class MaiHelpProbeTool final : public MaiTool {
public:
    explicit MaiHelpProbeTool(std::string toolName, std::string purpose = "Short purpose")
        : mName(std::move(toolName)), mPurpose(std::move(purpose)) {}
    int* forbiddenCalls = nullptr;
    std::atomic<bool>* cancelOnDescription = nullptr;
    bool available = true;
    std::string name() const override {
        return mName;
    }
    std::string description() const override {
        if (cancelOnDescription) cancelOnDescription->store(true);
        return mPurpose;
    }
    std::string parametersSchema() const override {
        if (forbiddenCalls) ++*forbiddenCalls;
        return "SCHEMA_BOMB";
    }
    MaiToolSpec specForModel(const std::string&) const override {
        if (forbiddenCalls) ++*forbiddenCalls;
        return {};
    }
    std::optional<MaiSpecialistInfo> specialistInfo() const override {
        if (forbiddenCalls) ++*forbiddenCalls;
        return std::nullopt;
    }
    bool isAvailableForModel(const std::string& model) const override {
        return available && model != "blocked";
    }
    MaiToolResult execute(const std::string&, const MaiToolContext&) override {
        if (forbiddenCalls) ++*forbiddenCalls;
        return MaiToolResult::failure(MaiErrorCode::Internal, "EXECUTION_BOMB");
    }

private:
    std::string mName;
    std::string mPurpose;
};

void testRegistrationAndParity() {
    MaiToolRegistry registry;
    registerMaiBuiltinTools(registry);
    auto* tool = registry.findForModel("tool_help");
    CHECK(tool);
    if (!tool) return;
    CHECK(!tool->requiresWorkingDirectory("{}"));
    CHECK(!tool->requiresApproval("{}"));
    CHECK(tool->executionMode() == MaiToolExecutionMode::ParallelReadOnly);
    CHECK(nlohmann::json::parse(tool->parametersSchema()).at("additionalProperties") == false);
    MaiToolContext context;
    context.model = "glm-5.4";
    for (const std::vector<std::string> arguments :
         {std::vector<std::string>{}, {"-h"}, {"agent", "-h"}, {"files", "-h"}}) {
        const auto ordinary = maiBuiltinToolHelp(registry, context.model, {arguments});
        const auto adapted =
            tool->execute(nlohmann::json{{"arguments", arguments}}.dump(), context);
        CHECK(ordinary && !adapted.hasError());
        if (ordinary) {
            CHECK(adapted.output() == ordinary.value().text);
            CHECK(adapted.isTruncated() == ordinary.value().truncated);
        }
    }
    const auto empty = maiBuiltinToolHelp(registry, context.model, {});
    const auto help = maiBuiltinToolHelp(registry, context.model, {{"-h"}});
    CHECK(empty && help && empty.value().text == help.value().text);
    // 真实注册结果是分类完整性的依据；类外或动态 name 定义也不能被漏成未知扩展。
    for (const auto& model : {std::string{}, context.model}) {
        const auto general = maiBuiltinToolHelp(registry, model, {{"general", "-h"}});
        CHECK(!general && general.error().code() == MaiErrorCode::InvalidInput);
        if (general)
            std::printf("Unclassified registered builtin tools:\n%s", general.value().text.c_str());
        const auto groups = maiBuiltinToolHelp(registry, model, {{"-h"}});
        CHECK(groups && groups.value().text.find("- general (") == std::string::npos);
        const auto files = maiBuiltinToolHelp(registry, model, {{"files", "-h"}});
        CHECK(files && files.value().text.find("- file_copy:") != std::string::npos);
        CHECK(files && files.value().text.find("- file_move:") != std::string::npos);
    }
}

void testMetadataGatesAndFallback() {
    MaiToolRegistry registry;
    int forbiddenCalls = 0;
    for (const auto* name :
         {"ffmpeg", "minimax_image", "minimax_video", "cv_detect_edges", "cv_motion_detect",
          "cv_scene_detect", "curl", "web_fetch", "web_search", "file_read", "tool_search",
          "skill_read", "vendor_extension", "ffmpeg_extension"}) {
        auto tool = std::make_unique<MaiHelpProbeTool>(name);
        tool->forbiddenCalls = &forbiddenCalls;
        registry.add(std::move(tool));
    }
    auto incompatible = std::make_unique<MaiHelpProbeTool>("ffprobe", "INCOMPATIBLE_SECRET");
    incompatible->available = false;
    registry.add(std::move(incompatible));
    registry.addInternal(std::make_unique<MaiHelpProbeTool>("wan_video", "HIDDEN_SECRET"));
    int credentials = 0;
    registry.add(makeMaiSeedanceVideoTool([&] {
        ++credentials;
        return std::string{};
    }));
    const auto groups = maiBuiltinToolHelp(registry, "glm-5.4", {{"-h"}});
    CHECK(groups);
    if (groups) {
        for (const auto* name :
             {"media", "vision", "network-security", "files", "agent", "general"})
            CHECK(groups.value().text.find(name) != std::string::npos);
    }
    const auto media = maiBuiltinToolHelp(registry, "glm-5.4", {{"media", "-h"}});
    CHECK(media);
    if (media) {
        CHECK(media.value().text.find("ffmpeg:") != std::string::npos);
        CHECK(media.value().text.find("seedance_video:") != std::string::npos);
        CHECK(media.value().text.find("minimax_image:") != std::string::npos);
        CHECK(media.value().text.find("minimax_video:") != std::string::npos);
        CHECK(media.value().text.find("ffmpeg_extension:") == std::string::npos);
        CHECK(media.value().text.find("ffprobe") == std::string::npos);
        CHECK(media.value().text.find("wan_video") == std::string::npos);
        CHECK(media.value().text.find("tool_search") != std::string::npos);
        CHECK(media.value().text.find("mode=discover") != std::string::npos);
    }
    // 已知内置名称必须进入真实职责分组，不能与未知扩展一起落入 general。
    const auto vision = maiBuiltinToolHelp(registry, "glm-5.4", {{"vision", "-h"}});
    CHECK(vision && vision.value().text.find("cv_motion_detect:") != std::string::npos);
    CHECK(vision && vision.value().text.find("cv_scene_detect:") != std::string::npos);
    const auto network = maiBuiltinToolHelp(registry, "glm-5.4", {{"network-security", "-h"}});
    CHECK(network && network.value().text.find("web_fetch:") != std::string::npos);
    CHECK(network && network.value().text.find("web_search:") != std::string::npos);
    const auto general = maiBuiltinToolHelp(registry, "glm-5.4", {{"general", "-h"}});
    CHECK(general && general.value().text.find("vendor_extension:") != std::string::npos);
    CHECK(general && general.value().text.find("ffmpeg_extension:") != std::string::npos);
    for (const auto* name : {"cv_motion_detect", "cv_scene_detect", "minimax_image",
                             "minimax_video", "web_fetch", "web_search"})
        CHECK(general && general.value().text.find(std::string(name) + ":") == std::string::npos);
    CHECK(!maiBuiltinToolHelp(registry, "blocked", {{"vision", "-h"}}));
    CHECK(forbiddenCalls == 0 && credentials == 0);
}

void testBoundsUtf8AndUnusualNames() {
    MaiToolRegistry registry;
    const std::string unusual = "odd\n\"\\ tool\t\u6587";
    registry.add(std::make_unique<MaiHelpProbeTool>(unusual, "Line\n\twith controls"));
    const auto quoted = maiBuiltinToolHelp(registry, "model", {{"general", "-h"}});
    CHECK(quoted);
    if (quoted) {
        CHECK(quoted.value().text.find(nlohmann::json(unusual).dump()) != std::string::npos);
        CHECK(quoted.value().text.find("Line with controls") != std::string::npos);
        CHECK(!maiValidateSessionContextContent(quoted.value().text));
    }
    std::string purpose;
    for (int i = 0; i < 90; ++i) purpose += "\u6587";
    for (int i = 0; i < 100; ++i)
        registry.add(std::make_unique<MaiHelpProbeTool>("extension_" + std::to_string(i), purpose));
    const auto bounded = maiBuiltinToolHelp(registry, "model", {{"general", "-h"}, 512});
    CHECK(bounded && bounded.value().truncated);
    if (bounded) {
        CHECK(bounded.value().text.size() <= 512);
        CHECK(!maiValidateSessionContextContent(bounded.value().text));
        CHECK(bounded.value().text.find("omitted") != std::string::npos);
    }
    CHECK(!maiBuiltinToolHelp(registry, "model", {{"-h"}, 511}));
    CHECK(!maiBuiltinToolHelp(registry, "model", {{"-h"}, 8193}));
    MaiToolRegistry invalid;
    invalid.add(std::make_unique<MaiHelpProbeTool>(std::string(257, 'x')));
    CHECK(!maiBuiltinToolHelp(invalid, "model", {}));
    MaiToolRegistry badUtf8;
    badUtf8.add(std::make_unique<MaiHelpProbeTool>(std::string("bad\xff", 4)));
    CHECK(!maiBuiltinToolHelp(badUtf8, "model", {}));
    MaiToolRegistry badPurpose;
    badPurpose.add(std::make_unique<MaiHelpProbeTool>("bad", std::string("bad\xff", 4)));
    CHECK(!maiBuiltinToolHelp(badPurpose, "model", {{"general", "-h"}}));
}

void testStrictParsingAndCancellation() {
    MaiToolRegistry registry;
    registry.add(std::make_unique<MaiHelpProbeTool>("ffmpeg"));
    auto tool = makeMaiToolHelpTool(registry);
    for (const std::vector<std::string> arguments : {std::vector<std::string>{"media"},
                                                     {"-h", "media"},
                                                     {"media", "--help"},
                                                     {"media", "-h", "extra"},
                                                     {"unknown", "-h"},
                                                     {"", "-h"},
                                                     {std::string(257, 'x'), "-h"}}) {
        const auto ordinary = maiBuiltinToolHelp(registry, "model", {arguments});
        const auto adapted = tool->execute(nlohmann::json{{"arguments", arguments}}.dump(), {});
        CHECK(!ordinary && ordinary.error().code() == MaiErrorCode::InvalidInput);
        CHECK(adapted.error().code() == MaiErrorCode::InvalidInput);
    }
    for (const auto* invalid : {"{}", "[]", "null", "{", "{\"arguments\":null}",
                                "{\"arguments\":[2]}", "{\"arguments\":[\"-h\"],\"extra\":true}"})
        CHECK(tool->execute(invalid, {}).error().code() == MaiErrorCode::InvalidInput);
    std::atomic<bool> canceled{true};
    CHECK(maiBuiltinToolHelp(registry, "model", {}, &canceled).error().code() ==
          MaiErrorCode::Canceled);
    MaiToolContext context;
    context.cancel = &canceled;
    CHECK(tool->execute("{\"arguments\":[\"-h\"]}", context).error().code() ==
          MaiErrorCode::Canceled);
    canceled.store(false);
    auto probe = std::make_unique<MaiHelpProbeTool>("cancel_in_metadata");
    probe->cancelOnDescription = &canceled;
    registry.add(std::move(probe));
    CHECK(maiBuiltinToolHelp(registry, "model", {}, &canceled).error().code() ==
          MaiErrorCode::Canceled);
}

void testSummaryRouteAndFrozenSnapshot() {
    auto store = makeMaiMemoryStore();
    MaiSession session;
    session.id = "help-summary";
    store->putSession(session);
    MaiToolRegistry registry;
    registry.add(std::make_unique<MaiHelpProbeTool>("tool_search"));
    registry.add(makeMaiToolHelpTool(registry));
    registry.add(std::make_unique<MaiHelpProbeTool>("skill_read"));
    MaiModelToolCatalog catalog(*store, registry);
    MaiTurnContext context{"help-summary", {}, "model", {}, {}};
    const auto summary = catalog.capabilitySummary(context, 512);
    CHECK(summary && summary.value().size() <= 512);
    // 只检查首个条目之前的说明，避免工具自身描述中的同名示例掩盖缺失的入口指引。
    const auto introduction =
        summary ? summary.value().substr(0, summary.value().find("\n- ")) : std::string{};
    CHECK(introduction.find("arguments:[\"-h\"]") != std::string::npos);
    CHECK(introduction.find("mode=discover") != std::string::npos);
    registry.addInternal(makeMaiToolHelpTool(registry));
    MaiModelToolCatalog reopened(*store, registry);
    CHECK(summary && reopened.capabilitySummary(context).value() == summary.value());
    session.id = "help-summary-no-route";
    store->putSession(session);
    registry.add(std::make_unique<MaiHelpProbeTool>("other"));
    context.sessionId = session.id;
    const auto withoutHelp = reopened.capabilitySummary(context);
    CHECK(withoutHelp && withoutHelp.value().find("arguments:[\"-h\"]") == std::string::npos);
}
}  // namespace

int main() {
    testRegistrationAndParity();
    testMetadataGatesAndFallback();
    testBoundsUtf8AndUnusualNames();
    testStrictParsingAndCancellation();
    testSummaryRouteAndFrozenSnapshot();
    std::printf("MaiToolHelpTests: %d failures\n", failures);
    return failures ? 1 : 0;
}
