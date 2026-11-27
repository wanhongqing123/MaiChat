// Advanced model entry reuses the ordinary controller and never touches the OS.
#include <QCoreApplication>
#include <QTemporaryDir>
#include <json.hpp>
#include <algorithm>
#include <cstdio>
#include <mutex>

#include "MaiChatTools/DesktopComputerBackend.h"
#include "MaiChatTools/DesktopComputerTools.h"
#include "MaiTool.h"

void registerDesktopComputerInputTools(MaiToolRegistry&, std::shared_ptr<DesktopComputerController>);

namespace {
using Json = nlohmann::json;
int failures = 0;
#define CHECK(value)                                                        \
    do {                                                                    \
        if (!(value)) {                                                     \
            ++failures;                                                     \
            std::fprintf(stderr, "failed line %d: %s\n", __LINE__, #value); \
        }                                                                   \
    } while (false)

class HeldSession final : public DesktopNativeInputSession {
public:
    DesktopNativeInputState held;
    mutable std::mutex mutex;
    int releases = 0;
    MaiError input(const DesktopNativeInput& input) override {
        if (input.isCanceled && input.isCanceled()) return {MaiErrorCode::Canceled, "fixture canceled"};
        std::lock_guard<std::mutex> lock(mutex);
        if (input.kind == DesktopNativeInputKind::KeyDown) held.keysDown = {input.key};
        if (input.kind == DesktopNativeInputKind::KeyUp) held.keysDown.clear();
        return {};
    }
    DesktopNativeInputState state() const override {
        std::lock_guard<std::mutex> lock(mutex);
        return held;
    }
    MaiError validate() const override {
        return {};
    }
    MaiError releaseAll() override {
        std::lock_guard<std::mutex> lock(mutex);
        ++releases;
        held = {};
        return {};
    }
};

class Backend final : public DesktopComputerBackend {
public:
    std::shared_ptr<HeldSession> session = std::make_shared<HeldSession>();
    int boundaries = 0;
    MaiResult<std::vector<DesktopNativeWindow>> windows() override {
        ++boundaries;
        return std::vector<DesktopNativeWindow>{{101, 5001, "Owned fixture", "Fixture", 100, 200, 400, 300}};
    }
    MaiResult<std::vector<DesktopNativeDisplay>> displays() override {
        ++boundaries;
        return std::vector<DesktopNativeDisplay>{{11, 0, 0, 1920, 1080, true}};
    }
    MaiResult<std::vector<DesktopNativeElement>> elements(const DesktopNativeWindow&) override {
        return std::vector<DesktopNativeElement>{};
    }
    MaiError capture(const DesktopNativeWindow&, MaiScreenshot&) override {
        ++boundaries;
        return {MaiErrorCode::NotSupported, "fixture has no capture"};
    }
    MaiError activate(const DesktopNativeWindow&) override {
        ++boundaries;
        return {};
    }
    MaiResult<bool> isFocused(const DesktopNativeWindow&) override {
        return true;
    }
    MaiResult<bool> secureInput() override {
        return false;
    }
    MaiError input(const DesktopNativeInput&) override {
        ++boundaries;
        return {MaiErrorCode::Internal, "must use shared native session"};
    }
    MaiResult<std::string> focusedText(const DesktopNativeWindow&) override {
        return std::string("Owned value");
    }
    std::shared_ptr<DesktopNativeInputSession> createInputSession() override {
        return session;
    }
};
}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    auto backend = std::make_shared<Backend>();
    auto controller = std::make_shared<DesktopComputerController>(backend);
    MaiToolRegistry registry;
    registerDesktopComputerInputTools(registry, controller);
    auto* tool = registry.find("computer_input");
    CHECK(tool && !registry.find("computer") && !registry.find("desktop_computer"));
    if (!tool) return 1;
    const auto schema = Json::parse(tool->parametersSchema());
    const auto actions = schema["properties"]["action"]["enum"];
    CHECK(actions.is_array() && actions.size() == 18);
    for (const char* expected : {"list_displays", "observe", "list_keys", "input_events", "input_capabilities",
                                 "feedback", "mouse_down", "mouse_up", "key_down", "key_up", "move", "input_state",
                                 "reset_inputs", "release_all", "minimize", "restore", "verify", "clear"}) {
        CHECK(std::find(actions.begin(), actions.end(), Json(expected)) != actions.end());
    }
    MaiToolContext context;
    context.sessionId = "owner";
    context.turnId = "turn";
    QTemporaryDir workspace;
    context.root = workspace.path().toUtf8().toStdString();
    for (const char* forbidden : {"list", "focus", "click", "double_click", "right_click", "type", "key", "scroll",
                                  "drag", "replace_text", "list_windows", "get_window_state", "launch_app"}) {
        const auto result = tool->execute(Json{{"action", forbidden}}.dump(), context);
        CHECK(result.hasError() && result.error().code() == MaiErrorCode::InvalidInput);
    }
    CHECK(tool->execute(R"({"action":"observe","target":"window","window_id":"101"})", context).error().code() ==
          MaiErrorCode::InvalidInput);
    CHECK(tool->execute(R"({"method":"click","parameters":{}})", context).error().code() == MaiErrorCode::InvalidInput);
    CHECK(backend->boundaries == 0);
    CHECK(!tool->execute(R"({"action":"list_displays"})", context).hasError());
    CHECK(!tool->execute(R"({"action":"list_keys"})", context).hasError());
    const auto observed =
        controller->observe(context.sessionId, "", context.root, false, 101, false, {}, context.turnId);
    CHECK(observed);
    if (observed) {
        const auto token = Json::parse(observed.value().json)["snapshot_id"];
        const auto held = tool->execute(
            Json{{"action", "key_down"}, {"window_id", "101"}, {"snapshot_id", token}, {"key", "Ctrl"}}.dump(),
            context);
        CHECK(!held.hasError());
        CHECK(backend->session->state().keysDown == std::vector<std::string>{"Ctrl"});
        auto stale = context;
        stale.turnId = "old-turn";
        CHECK(!tool->execute(R"({"action":"release_all"})", stale).hasError());
        CHECK(!backend->session->state().empty());
        CHECK(!tool->execute(R"({"action":"key_up","key":"Ctrl"})", context).hasError());
        CHECK(backend->session->state().empty());
    }
    CHECK(!tool->requiresPerCallApproval(R"({"action":"key_up","key":"Ctrl"})"));
    CHECK(!tool->requiresPerCallApproval(R"({"action":"reset_inputs"})"));
    CHECK(tool->requiresPerCallApproval(R"({"action":"key_down","key":"Ctrl"})"));
    CHECK(!controller->releaseInput(context.sessionId, context.turnId));
    return failures ? 1 : 0;
}
