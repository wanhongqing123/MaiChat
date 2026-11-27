#include "MaiChatTools/DesktopComputerTools.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>

#include <json.hpp>

#include "MaiCloudModelCatalog.h"
#include "MaiTool.h"

namespace {
using Json = nlohmann::json;
constexpr const char* methods[] = {
    "list_windows", "get_window", "list_apps", "launch_app", "get_window_state",         "click",          "press_key",
    "type_text",    "scroll",     "set_value", "drag",       "perform_secondary_action", "activate_window"};

Json windowJson(const DesktopComputerWindow& window) {
    Json result{{"app", window.app}, {"id", window.id}};
    if (window.title) result["title"] = *window.title;
    return result;
}

bool only(const Json& value, std::initializer_list<const char*> names) {
    if (!value.is_object()) return false;
    for (auto it = value.begin(); it != value.end(); ++it)
        if (std::none_of(names.begin(), names.end(), [&](const auto* name) { return it.key() == name; })) return false;
    return true;
}

bool text(const Json& value, const char* name, bool required = true, std::size_t maximum = 32768) {
    if (!value.contains(name)) return !required;
    if (!value[name].is_string()) return false;
    const auto& string = value[name].get_ref<const std::string&>();
    return string.size() <= maximum && string.find('\0') == std::string::npos;
}

MaiResult<std::uint64_t> positiveId(const Json& value) {
    if (value.is_number_unsigned()) {
        const auto id = value.get<std::uint64_t>();
        if (id) return id;
    } else if (value.is_number_integer() && value.get<std::int64_t>() > 0) {
        return static_cast<std::uint64_t>(value.get<std::int64_t>());
    }
    return {MaiErrorCode::InvalidInput, "id must be a positive integer returned by discovery"};
}

MaiResult<DesktopComputerWindow> parseWindow(const Json& value, bool optionalApp = false) {
    if (!only(value, {"app", "id", "title"}) || !value.contains("id") || !text(value, "app", !optionalApp) ||
        !text(value, "title", false))
        return {MaiErrorCode::InvalidInput, "window must contain its returned app and id"};
    const auto id = positiveId(value["id"]);
    if (!id) return id.error();
    DesktopComputerWindow window;
    window.id = id.value();
    window.app = value.value("app", std::string());
    if (!optionalApp && window.app.empty()) return {MaiErrorCode::InvalidInput, "window app is required"};
    if (value.contains("title")) window.title = value["title"].get<std::string>();
    return window;
}

bool number(const Json& value, const char* name, double& result, bool required = true) {
    if (!value.contains(name)) return !required;
    if (!value[name].is_number()) return false;
    result = value[name].get<double>();
    return std::isfinite(result);
}

Json stateJson(const DesktopComputerWindowState& state) {
    Json result{{"window", windowJson(state.window)},
                {"accessibility", nullptr},
                {"screenshots", Json::array()},
                {"image_coordinates_usable_for_input", state.imageCoordinatesUsable}};
    result["native_input_binding"] = {{"snapshot_id", state.nativeSnapshotId},
                                      {"window_id", std::to_string(state.window.id)}};
    if (state.accessibility) {
        const auto& source = *state.accessibility;
        Json accessibility{{"tree", ""},
                           {"provider", source.provider},
                           {"status", source.status},
                           {"truncated", source.truncated},
                           {"limit_reason", source.limitReason}};
        std::string tree;
        for (std::size_t index = 0; index < state.indexedElements.size(); ++index) {
            const auto& element = state.indexedElements[index];
            tree += std::string((std::min)(element.depth, std::size_t(64)), '\t') + std::to_string(index) + " " +
                    element.role + " " + element.label;
            if (!element.enabled) tree += " [disabled]";
            if (element.offscreen) tree += " [offscreen]";
            if (index < state.elementBounds.size()) {
                const auto& bounds = state.elementBounds[index];
                tree += " [window-logical-bounds:" + Json(bounds).dump() + "]";
            }
            if (!element.secondaryActions.empty()) {
                tree += " [actions:";
                for (const auto& action : element.secondaryActions) tree += " " + action;
                tree += "]";
            }
            tree += '\n';
        }
        accessibility["tree"] = std::move(tree);
        if (source.focusedElement) accessibility["focused_element"] = *source.focusedElement;
        if (source.selectedText) accessibility["selected_text"] = *source.selectedText;
        if (source.selectedElements) accessibility["selected_elements"] = *source.selectedElements;
        if (source.documentText) accessibility["document_text"] = *source.documentText;
        result["accessibility"] = std::move(accessibility);
    }
    for (const auto& image : state.screenshots)
        result["screenshots"].push_back({{"id", image.id},
                                         {"image_attachment_index", result["screenshots"].size()},
                                         {"width", image.width},
                                         {"height", image.height},
                                         {"image_pixel_width", image.imagePixelWidth},
                                         {"image_pixel_height", image.imagePixelHeight},
                                         {"originX", image.originX},
                                         {"originY", image.originY},
                                         {"zIndex", image.zIndex}});
    return result;
}

class DesktopComputerSkyTool final : public MaiTool {
public:
    explicit DesktopComputerSkyTool(std::shared_ptr<DesktopComputerController> controller)
        : controller_(std::move(controller)) {}
    std::string name() const override {
        return "computer";
    }
    std::string description() const override {
        return "Operate native desktop apps using the Sky window2 methods. Call with {method,parameters}; "
               "parameters preserves that method's arguments. Discover list_windows/list_apps, recover get_window "
               "by id (app optional), or launch_app by returned app identity or literal executable path. "
               "Use the returned {app,id,title?} window object, not a guessed handle. get_window_state takes "
               "window, include_screenshot(default true), include_text(default false); at least one is true. "
               "It returns window, accessibility(tree and available focus/selection/document facts), and "
               "screenshots(id,image_attachment_index,width,height,originX,originY,zIndex); images are attached "
               "automatically. The ordinary library keeps each data URL; tool text omits it to avoid duplicate "
               "Base64 and does not expose an absolute local path as url. "
               "No separate screenshot call or base64 printing is needed. Coordinates x/y and drag from_x/from_y/"
               "to_x/to_y are window-relative logical pixels, not ratios or whole-screen pixels. Screenshot "
               "metadata gives logical bounds and image_pixel_width/height. If a point (px,py) was measured in "
               "the original attachment pixels, convert x=px*width/image_pixel_width and "
               "y=py*height/image_pixel_height before input. Respect image_coordinates_usable_for_input; "
               "unverified image mappings require native elements. Use screenshotId "
               "from the current state for coordinate actions, or click/set_value/perform_secondary_action with "
               "element_index from its current tree. Click accepts mouse_button(left/right/middle or l/r/m) and "
               "click_count(default1, rounded positive, max127). press_key takes key (keysym aliases and chords); "
               "type_text takes literal text into the focused editor. scroll takes x,y,scrollX,scrollY: negative "
               "X left, positive X right, negative Y up, positive Y down; native wheel delta units, actual "
               "distance depends on platform/settings. set_value takes element_index,value, not blind select-all. "
               "perform_secondary_action takes element_index,action from the observed element's actions. "
               "activate_window explicitly focuses a returned window. Inputs activate their target; verify "
               "editor focus before typing. State/indices/screenshots expire on changes, mutations or interruption: "
               "observe and inspect before choosing state-derived input, then act and refresh when needed. "
               "Never repeat an uncertain input without observing the result; dispatch is not task completion. "
               "Password/secure inputs require manual entry. Stop ends owned input. This client-executed function "
               "does not enable Responses built-in computer_call or change native capture lifetime.";
    }
    std::string parametersSchema() const override {
        const Json window{{"type", "object"},
                          {"properties",
                           {{"app", {{"type", "string"}}},
                            {"id", {{"type", "integer"}, {"minimum", 1}}},
                            {"title", {{"type", "string"}}}}},
                          {"required", {"app", "id"}},
                          {"additionalProperties", false}};
        Json properties{{"window", window},
                        {"app", {{"type", "string"}}},
                        {"id", {{"type", "integer"}, {"minimum", 1}}},
                        {"include_screenshot", {{"type", "boolean"}}},
                        {"include_text", {{"type", "boolean"}}},
                        {"element_index", {{"type", "integer"}, {"minimum", 0}}},
                        {"screenshotId", {{"type", "string"}}},
                        {"mouse_button", {{"type", "string"}, {"enum", {"left", "right", "middle", "l", "r", "m"}}}},
                        {"click_count", {{"type", "number"}, {"minimum", 1}, {"maximum", 127}}},
                        {"key", {{"type", "string"}}},
                        {"text", {{"type", "string"}}},
                        {"value", {{"type", "string"}}},
                        {"action", {{"type", "string"}}}};
        for (const auto* name : {"x", "y", "from_x", "from_y", "to_x", "to_y", "scrollX", "scrollY"})
            properties[name] = {{"type", "number"}};
        return Json{
            {"type", "object"},
            {"properties",
             {{"method", {{"type", "string"}, {"enum", methods}}},
              {"parameters", {{"type", "object"}, {"properties", properties}, {"additionalProperties", false}}}}},
            {"required", {"method", "parameters"}},
            {"additionalProperties", false}}
            .dump();
    }
    bool requiresApproval(const std::string&) const override {
        return true;
    }
    bool requiresPerCallApproval(const std::string&) const override {
        return true;
    }
    MaiError onTurnFinished(const std::string& session, const std::string& turn) override {
        return controller_->releaseInput(session, turn);
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const auto fail = [](const MaiError& error) { return MaiToolResult::failure(error.code(), error.message()); };
        const auto invalid = [](const char* message) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, message);
        };
        if (context.isCanceled()) {
            controller_->releaseInput(context.sessionId, context.turnId);
            return MaiToolResult::failure(MaiErrorCode::Canceled, "computer operation canceled");
        }
        if (context.sessionId.empty()) return invalid("a Desktop session is required");
        const auto input = Json::parse(raw, nullptr, false);
        if (!only(input, {"method", "parameters"}) || !text(input, "method") || !input.contains("parameters") ||
            !input["parameters"].is_object())
            return invalid("computer expects {method,parameters}");
        const auto method = input["method"].get<std::string>();
        if (std::none_of(std::begin(methods), std::end(methods), [&](const auto* name) { return method == name; }))
            return invalid("unknown computer method");
        const auto& args = input["parameters"];
        const auto canceled = [&] { return context.isCanceled(); };
        if (method == "list_windows" || method == "list_apps") {
            if (!args.empty()) return invalid("discovery method takes no parameters");
            Json output = Json::array();
            if (method == "list_windows") {
                const auto windows = controller_->listWindows(canceled);
                if (!windows) return fail(windows.error());
                for (const auto& window : windows.value()) output.push_back(windowJson(window));
            } else {
                const auto apps = controller_->applications(canceled);
                if (!apps) return fail(apps.error());
                for (const auto& app : apps.value()) {
                    Json value{{"id", app.id}, {"windows", Json::array()}};
                    if (app.displayName) value["displayName"] = *app.displayName;
                    if (app.isRunning) value["isRunning"] = *app.isRunning;
                    if (app.lastUsedDate) value["lastUsedDate"] = *app.lastUsedDate;
                    if (app.useCount) value["useCount"] = *app.useCount;
                    for (const auto& window : app.windows)
                        value["windows"].push_back(windowJson({app.id, window.id, window.title}));
                    output.push_back(std::move(value));
                }
            }
            return MaiToolResult::success(output.dump());
        }
        if (method == "get_window") {
            const auto window = parseWindow(args, true);
            if (!window) return fail(window.error());
            const auto restored = controller_->getWindow(window.value(), canceled);
            return restored ? MaiToolResult::success(windowJson(restored.value()).dump()) : fail(restored.error());
        }
        if (method == "launch_app") {
            if (!only(args, {"app"}) || !text(args, "app") || args["app"].get_ref<const std::string&>().empty())
                return invalid("launch_app requires only a literal app identity");
            const auto error = controller_->launchApplication(args["app"].get<std::string>(), canceled);
            return error ? fail(error)
                         : MaiToolResult::success(R"({"state":"launch_requested","application_ready":false})");
        }
        if (!args.contains("window")) return invalid("method requires a returned window object");
        const auto window = parseWindow(args["window"]);
        if (!window) return fail(window.error());
        if (method == "activate_window") {
            if (!only(args, {"window"})) return invalid("activate_window takes only window");
            const auto error = controller_->activateWindow(window.value(), canceled);
            return error ? fail(error) : MaiToolResult::success(R"({"state":"activation_requested"})");
        }
        if (method == "get_window_state") {
            if (!only(args, {"window", "include_screenshot", "include_text"}) ||
                (args.contains("include_screenshot") && !args["include_screenshot"].is_boolean()) ||
                (args.contains("include_text") && !args["include_text"].is_boolean()))
                return invalid("get_window_state accepts window and boolean observation flags");
            const bool screenshot = args.value("include_screenshot", true);
            const auto model = maiCloudModelInfo(context.model);
            if (screenshot && ((model && !model->supportsImages) || context.model.compare(0, 6, "local:") == 0))
                return MaiToolResult::failure(MaiErrorCode::NotSupported,
                                              "primary model cannot receive window images; request text only");
            const auto state = controller_->getWindowState(context.sessionId, window.value(), context.root, screenshot,
                                                           args.value("include_text", false), canceled, context.turnId);
            if (!state) return fail(state.error());
            std::vector<MaiToolImage> images;
            for (const auto& image : state.value().screenshots) images.push_back({image.imagePath, "image/png"});
            return MaiToolResult::successWithImages(stateJson(state.value()).dump(), std::move(images));
        }
        DesktopComputerSkyAction action;
        action.method = method;
        action.window = window.value();
        action.turnId = context.turnId;
        action.isCanceled = canceled;
        if (args.contains("element_index")) {
            if (!args["element_index"].is_number_integer() || args["element_index"].get<std::int64_t>() < 0)
                return invalid("element_index must be a nonnegative integer from the current tree");
            action.elementIndex = args["element_index"].get<std::size_t>();
        }
        if (args.contains("screenshotId")) {
            if (!text(args, "screenshotId") || args["screenshotId"].get_ref<const std::string&>().empty())
                return invalid("screenshotId must be from the current window state");
            action.screenshotId = args["screenshotId"].get<std::string>();
        }
        if (method == "click") {
            if (!only(args, {"window", "x", "y", "element_index", "screenshotId", "mouse_button", "click_count"}))
                return invalid("unexpected click parameter");
            if (args.contains("mouse_button")) {
                if (!text(args, "mouse_button")) return invalid("mouse_button must be a string");
                const auto button = args["mouse_button"].get<std::string>();
                if (button == "left" || button == "l")
                    action.button = DesktopMouseButton::Left;
                else if (button == "middle" || button == "m")
                    action.button = DesktopMouseButton::Middle;
                else if (button == "right" || button == "r")
                    action.button = DesktopMouseButton::Right;
                else
                    return invalid("mouse_button must be left, middle or right");
            }
            double count = 1;
            if (!number(args, "click_count", count, false) || std::round(count) < 1 || std::round(count) > 127)
                return invalid("click_count must round to 1..127");
            action.clickCount = int(std::round(count));
        } else if (method == "press_key" || method == "type_text") {
            const char* field = method == "press_key" ? "key" : "text";
            if (!(method == "press_key" ? only(args, {"window", "key"}) : only(args, {"window", "text"})) ||
                !text(args, field, true, method == "press_key" ? 128 : 4000))
                return invalid("invalid text/key parameters");
            if (method == "press_key")
                action.key = args[field].get<std::string>();
            else
                action.text = args[field].get<std::string>();
        } else if (method == "set_value" || method == "perform_secondary_action") {
            const auto* field = method == "set_value" ? "value" : "action";
            if (!(method == "set_value" ? only(args, {"window", "element_index", "value"})
                                        : only(args, {"window", "element_index", "action"})) ||
                !action.elementIndex || !text(args, field, true, 4000))
                return invalid("element index and value/action are required");
            if (method == "set_value")
                action.text = args[field].get<std::string>();
            else
                action.secondaryAction = args[field].get<std::string>();
        } else if (method == "drag") {
            if (!only(args, {"window", "from_x", "from_y", "to_x", "to_y", "screenshotId"}))
                return invalid("unexpected drag parameter");
            double x = 0, y = 0;
            if (!number(args, "from_x", x) || !number(args, "from_y", y) || !number(args, "to_x", action.toX) ||
                !number(args, "to_y", action.toY))
                return invalid("drag requires finite from/to coordinates");
            action.x = x;
            action.y = y;
        } else if (method == "scroll") {
            if (!only(args, {"window", "x", "y", "scrollX", "scrollY", "screenshotId"}) ||
                !number(args, "scrollX", action.scrollX) || !number(args, "scrollY", action.scrollY))
                return invalid("scroll requires finite scrollX and scrollY deltas");
        }
        if (method == "click" || method == "scroll") {
            double x = 0, y = 0;
            if (args.contains("x")) {
                if (!number(args, "x", x)) return invalid("x must be finite");
                action.x = x;
            }
            if (args.contains("y")) {
                if (!number(args, "y", y)) return invalid("y must be finite");
                action.y = y;
            }
        }
        // 互斥定位属于入参校验；不能进入执行核心后才发现并撤销原本仍有效的观察。
        if (method == "click" &&
            ((action.elementIndex && (action.x || action.y)) || (!action.elementIndex && (!action.x || !action.y))))
            return invalid("click requires element_index or both x/y, never both");
        DesktopComputerActionProgress progress;
        const auto result = controller_->performSky(context.sessionId, action, &progress);
        if (!result) {
            Json facts{{"message", result.error().message()},
                       {"input_dispatch_attempted", progress.inputDispatchAttempted},
                       {"window_operation_attempted", progress.windowOperationAttempted},
                       {"needs_observation", progress.needsObservation}};
            return MaiToolResult::failure(result.error().code(), facts.dump());
        }
        return MaiToolResult::success(
            Json{{"state", result.value().state},
                 {"dispatched", result.value().dispatched},
                 {"verified", result.value().verified},
                 {"next_step", "Observe the actual window before further state-derived input."}}
                .dump());
    }

private:
    std::shared_ptr<DesktopComputerController> controller_;
};
}  // namespace

void registerDesktopComputerTools(MaiToolRegistry& registry, std::shared_ptr<DesktopComputerController> controller) {
    registry.add(std::make_unique<DesktopComputerSkyTool>(controller));
    registerDesktopComputerInputTools(registry, std::move(controller));
}
