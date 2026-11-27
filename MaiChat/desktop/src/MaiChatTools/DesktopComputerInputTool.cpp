
// Advanced-only model adapter. All operations reuse the existing ordinary
// controller, observation state and native input ownership.
#include "MaiChatTools/DesktopComputerTools.h"
#include "MaiChatTools/DesktopComputerKeys.h"
#include "MaiChatTools/DesktopComputerInputProtocol.h"
#include "MaiCloudModelCatalog.h"
#include "MaiTool.h"

#include <QString>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <json.hpp>

namespace {
using Json = nlohmann::json;

std::vector<std::string> advancedActions() {
    return {"list_displays", "observe",     "list_keys",    "input_events", "input_capabilities",
            "feedback",      "mouse_down",  "mouse_up",     "key_down",     "key_up",
            "move",          "input_state", "reset_inputs", "release_all",  "minimize",
            "restore",       "verify",      "clear"};
}

const char* nativeScreenUnit() {
#if defined(Q_OS_WIN)
    return "screen_physical_pixels";
#else
    return "screen_points";
#endif
}

Json displayJson(const DesktopNativeDisplay& display) {
    return {{"display_id", std::to_string(display.id)},
            {"primary", display.primary},
            {"bounds", {{"x", display.x}, {"y", display.y}, {"width", display.width}, {"height", display.height}}}};
}

MaiToolResult failure(const MaiError& error) {
    return MaiToolResult::failure(error.code(), error.message());
}

MaiResult<std::uint64_t> windowIdFrom(const Json& value, const std::string& field = "window_id") {
    if (!value.is_string()) return {MaiErrorCode::InvalidInput, field + " must be a decimal string"};
    const std::string id = value.get<std::string>();
    if (id.empty() || id.size() > 20 || !std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return {MaiErrorCode::InvalidInput, field + " must be a nonzero decimal ID string"};
    errno = 0;
    char* end = nullptr;
    const auto parsed = std::strtoull(id.c_str(), &end, 10);
    if (errno == ERANGE || !end || *end || !parsed)
        return {MaiErrorCode::InvalidInput, field + " must be a nonzero decimal ID string within uint64 range"};
    return std::uint64_t(parsed);
}

Json windowJson(const DesktopNativeWindow& window) {
    return {{"window_id", std::to_string(window.id)},
            {"process_id", window.processId},
            {"title", window.title},
            {"application", window.application},
            {"minimized", window.minimized},
            {"cloaked", window.cloaked},
            {"bounds_available", window.boundsAvailable},
            {"tool_window", window.toolWindow},
            {"owner_window_id", std::to_string(window.ownerId)},
            {"class_name", window.className},
            {"native_screen_bounds_unit", nativeScreenUnit()},
            {"bounds", {{"x", window.x}, {"y", window.y}, {"width", window.width}, {"height", window.height}}}};
}

Json inputStateJson(const DesktopNativeInputState& state) {
    Json rawKeys = Json::array();
    for (const auto& key : state.rawKeys)
        rawKeys.push_back({{"code", key.code}, {"flags", key.flags}, {"modifiers", key.modifiers}});
    Json controllers = Json::array();
    for (const auto& controller : state.controllers)
        controllers.push_back({{"controller_id", controller.controllerId},
                               {"generation", std::to_string(controller.generation)},
                               {"profile", maiDesktopInputProfileName(controller.profile)}});
    return {{"buttons_down", state.buttonsDown}, {"keys_down", state.keysDown},
            {"raw_keys_down", rawKeys},          {"touch_pointers", state.touchPointers},
            {"pen_active", state.penActive},     {"devices", state.devices},
            {"controllers", controllers},        {"scope", "owned_synthetic_inputs"},
            {"idle_release_ms", 120000},         {"idle_remaining_ms", state.idleRemainingMs}};
}

MaiResult<DesktopMouseButton> mouseButtonFrom(const Json& value) {
    if (!value.is_string()) return {MaiErrorCode::InvalidInput, "button must be a mouse button name"};
    const std::string name = value.get<std::string>();
    if (name == "left") return DesktopMouseButton::Left;
    if (name == "right") return DesktopMouseButton::Right;
    if (name == "middle") return DesktopMouseButton::Middle;
    if (name == "back") return DesktopMouseButton::Back;
    if (name == "forward") return DesktopMouseButton::Forward;
    return {MaiErrorCode::InvalidInput, "button must be left, right, middle, back or forward"};
}

class DesktopComputerInputTool final : public MaiTool {
public:
    explicit DesktopComputerInputTool(std::shared_ptr<DesktopComputerController> controller)
        : controller_(std::move(controller)) {}
    std::string name() const override {
        return "computer_input";
    }
    MaiError onTurnFinished(const std::string& session, const std::string& turn) override {
        return controller_->releaseInput(session, turn);
    }
    std::string description() const override {
        return "Advanced native input complement to computer's 13 Sky methods. This separate tool accepts "
               "{action,...}. It reuses the SAME ordinary controller, observation and owned native session; "
               "never invent a second observation or pass a Sky screenshot ID as snapshot_id. Use the exact "
               "native_input_binding returned by computer get_window_state for advanced window input. "
               "list_displays discovers native display IDs; observe requires target=display, display_id and "
               "host workspace, returns actual PNG plus the shared snapshot_id. Optional element_window_id "
               "chooses an accessibility source; execution_window_id restricts the receiver. Display input "
               "requires an image-capable model and x_ratio/y_ratio in the SAME native display bounds. "
               "move, mouse_down/up and key_down/up preserve native held controls; key_down/up take one "
               "physical key, while ordinary chords and literal text use computer press_key/type_text. "
               "input_events uses the existing typed event/options grammar and checks all native scopes. "
               "input_capabilities reports actual backend support; feedback reports supported native feedback. "
               "list_keys lists accepted shared names, with native support checked at dispatch. "
               "input_state reports this caller's synthetic controls; reset_inputs neutralizes owned controls "
               "and retains configuration, release_all tears down this caller's session. Release/reset/up "
               "operations remain available without a current snapshot/focus and cannot release another turn's "
               "controls. "
               "One session/task/target owns held input at a time. Release before changing target. "
               "minimize/restore consume a current WINDOW binding and report verified native state; "
               "verify compares expected_text with actual focused nonsecure text. clear retires observation "
               "and owned input without changing editor text. dry_run validates without input. Refresh the "
               "shared observation after mutation, partial dispatch, target changes or interruption. "
               "Basic discovery, activation, click, type, key, drag, scroll and element actions belong only "
               "to computer; this extension provides no aliases for those Sky methods.";
    }
    std::string parametersSchema() const override {
        Json base{{"type", "object"},
                  {"properties",
                   {{"action", {{"type", "string"}, {"enum", advancedActions()}}},
                    {"target", {{"type", "string"}, {"enum", {"window", "display"}}}},
                    {"window_id", {{"type", "string"}}},
                    {"display_id", {{"type", "string"}}},
                    {"snapshot_id", {{"type", "string"}}},
                    {"element_window_id", {{"type", "string"}}},
                    {"execution_window_id", {{"type", "string"}}},
                    {"wait_ms", {{"type", "integer"}, {"minimum", 0}, {"maximum", 5000}}},
                    {"x_ratio", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
                    {"y_ratio", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
                    {"button", {{"type", "string"}, {"enum", {"left", "middle", "right", "back", "forward"}}}},
                    {"key", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                    {"expected_text", {{"type", "string"}, {"maxLength", 4000}}},
                    {"dry_run", {{"type", "boolean"}}},
                    {"after_sequence", {{"type", "string"}}},
                    {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 256}}}}},
                  {"required", {"action"}},
                  {"additionalProperties", false}};
        auto schema = Json::parse(maiDesktopComputerInputSchema(base.dump()));
        // The event codec supplies typed event/options schema; the extension's
        // action directory is exact and cannot acquire any basic Sky aliases.
        schema["properties"]["action"]["enum"] = advancedActions();
        return schema.dump();
    }
    bool requiresApproval(const std::string& raw) const override {
        return requiresPerCallApproval(raw);
    }
    bool requiresPerCallApproval(const std::string& raw) const override {
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object() || !args.value("action", Json{}).is_string()) return true;
        const auto action = args["action"].get<std::string>();
        if (action == "input_events") {
            const auto batch = maiDesktopParseInputBatch(
                args.value("events", Json{}).dump(),
                args.contains("input_options") ? args["input_options"].dump() : std::string());
            return !batch || !maiDesktopInputEventsOnlyRelease(batch.value().events);
        }
        // Cleanup cannot depend on a new approval dialog or a still-valid
        // observation; these operations only release this caller's own ledger.
        return action != "input_capabilities" && action != "feedback" && action != "clear" && action != "release_all" &&
               action != "reset_inputs" && action != "mouse_up" && action != "key_up" && action != "input_state";
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        Json args = Json::parse(raw, nullptr, false);
        if (context.isCanceled()) controller_->releaseInput(context.sessionId, context.turnId);
        if (!args.is_object() || !args.value("action", Json{}).is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "computer_input requires an advanced action");
        const auto actionName = args["action"].get<std::string>();
        const auto allowedActions = advancedActions();
        if (std::find(allowedActions.begin(), allowedActions.end(), actionName) == allowedActions.end())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "unsupported computer_input action; use computer Sky methods");
        if (actionName == "observe" && args.value("target", Json{}) != "display")
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "computer_input observe supports target=display only");
        bool releaseEvents = false;
        if (args.is_object() && args.value("action", Json{}) == "input_events") {
            const auto parsed = maiDesktopParseInputBatch(
                args.value("events", Json{}).dump(),
                args.contains("input_options") ? args["input_options"].dump() : std::string());
            releaseEvents = parsed && maiDesktopInputEventsOnlyRelease(parsed.value().events);
        }
        const bool cleanup =
            args.is_object() && args.value("action", Json{}).is_string() &&
            (args["action"] == "release_all" || args["action"] == "reset_inputs" || args["action"] == "mouse_up" ||
             args["action"] == "key_up" || args["action"] == "input_state" || releaseEvents);
        if (context.isCanceled()) {
            // Host Stop retires the turn independently of action dry-run. A
            // validation-only request cannot keep canceled synthetic holds.
            controller_->releaseInput(context.sessionId, context.turnId);
            if (!cleanup) return MaiToolResult::failure(MaiErrorCode::Canceled, "computer operation canceled");
        }
        if (context.sessionId.empty())
            return MaiToolResult::failure(MaiErrorCode::NotConfigured, "a local Desktop Session is required");
        if (!args.is_object() || !args.value("action", Json{}).is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "computer action is required");
        // Validate types before .value<T>() and before any backend access.
        // Workspace belongs to the host context; it is never a tool argument.
        for (auto field = args.begin(); field != args.end(); ++field) {
            const auto& name = field.key();
            const bool known = name == "action" || name == "target" || name == "window_id" || name == "display_id" ||
                               name == "element_window_id" || name == "execution_window_id" || name == "snapshot_id" ||
                               name == "wait_ms" || name == "x_ratio" || name == "y_ratio" || name == "key" ||
                               name == "expected_text" || name == "dry_run" || name == "button" || name == "events" ||
                               name == "input_options" || name == "after_sequence" || name == "limit";
            if (!known)
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "unexpected computer parameter: " + name);
            if ((name == "window_id" || name == "display_id" || name == "element_window_id" ||
                 name == "execution_window_id") &&
                !field.value().is_string())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, name + " must be a decimal string");
            if ((name == "target" || name == "snapshot_id" || name == "key" || name == "expected_text" ||
                 name == "button") &&
                !field.value().is_string())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, name + " must be a string");
            if (name == "dry_run" && !field.value().is_boolean())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, name + " must be boolean");
            if ((name == "x_ratio" || name == "y_ratio") && !field.value().is_number())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, name + " must be a number");
            if (name == "wait_ms" && !field.value().is_number_integer())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, name + " must be an integer");
        }
        if (args["action"] == "input_capabilities") {
            if (args.size() != 1)
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "input_capabilities has no other parameters");
            return MaiToolResult::success(maiDesktopInputCapabilitiesJson(controller_->capabilities()));
        }
        if (args["action"] == "feedback") {
            for (auto field = args.begin(); field != args.end(); ++field)
                if (field.key() != "action" && field.key() != "after_sequence" && field.key() != "limit")
                    return MaiToolResult::failure(MaiErrorCode::InvalidInput, "unexpected feedback parameter");
            if (args.contains("after_sequence") && !args["after_sequence"].is_string())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "after_sequence must be a decimal string");
            const auto sequence = maiDesktopInputSequence(args.value("after_sequence", std::string("0")));
            if (!sequence) return failure(sequence.error());
            if (args.contains("limit") && (!args["limit"].is_number_integer() || args["limit"].get<double>() < 1 ||
                                           args["limit"].get<double>() > 256))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "feedback limit must be 1..256");
            const auto batch = controller_->feedback(context.sessionId, sequence.value(), args.value("limit", 64));
            return batch ? MaiToolResult::success(maiDesktopInputFeedbackJson(batch.value())) : failure(batch.error());
        }
        if (args["action"] == "input_events") return executeInputEvents(args, context);
        if (args["action"] == "input_state" || args["action"] == "release_all" || args["action"] == "reset_inputs") {
            if (args.size() != 1)
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "owned input state actions have no other parameters");
            if (args["action"] == "release_all") {
                const auto released = controller_->releaseInput(context.sessionId, context.turnId);
                if (released) return failure(released);
            }
            if (args["action"] == "reset_inputs") {
                const auto reset = controller_->resetInput(context.sessionId, context.turnId);
                if (reset) return failure(reset);
            }
            auto result = inputStateJson(controller_->inputState(context.sessionId));
            if (args["action"] == "reset_inputs") result["state"] = "inputs_reset";
            return MaiToolResult::success(result.dump());
        }
        if (args["action"] == "mouse_up" || args["action"] == "key_up") {
            const bool mouse = args["action"] == "mouse_up";
            for (auto field = args.begin(); field != args.end(); ++field) {
                const auto& key = field.key();
                if (key != "action" && key != "target" && key != "window_id" && key != "display_id" &&
                    key != "snapshot_id" && key != "dry_run" && key != (mouse ? "button" : "key"))
                    return MaiToolResult::failure(MaiErrorCode::InvalidInput, "unexpected input-release parameter");
            }
            if (args.contains("target") &&
                (!args["target"].is_string() || (args["target"] != "display" && args["target"] != "window")))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "target must be display or window");
            if (args.contains("display_id") && args.contains("window_id"))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "release refers to at most one input target");
            DesktopComputerAction request;
            request.action = args["action"].get<std::string>();
            request.turnId = context.turnId;
            if (args.contains("dry_run") && !args["dry_run"].is_boolean())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "dry_run must be boolean");
            request.dryRun = args.value("dry_run", false);
            request.isCanceled = [&context] { return context.isCanceled(); };
            if (mouse) {
                const auto button = mouseButtonFrom(args.value("button", Json("left")));
                if (!button) return failure(button.error());
                request.button = button.value();
            } else {
                if (!args.value("key", Json{}).is_string())
                    return MaiToolResult::failure(MaiErrorCode::InvalidInput, "key_up needs one named key");
                request.key = args["key"].get<std::string>();
            }
            request.desktopScope = args.value("target", std::string("window")) == "display";
            for (const char* field : {"display_id", "window_id"})
                if (args.contains(field)) {
                    const auto id = windowIdFrom(args[field], field);
                    if (!id) return failure(id.error());
                    if (std::string(field) == "display_id")
                        request.displayId = id.value();
                    else
                        request.windowId = id.value();
                }
            const auto released = controller_->perform(context.sessionId, request);
            if (!released) return failure(released.error());
            Json output = inputStateJson(released.value().inputState);
            output["state"] = released.value().state;
            output["dry_run"] = released.value().dryRun;
            output["dispatched"] = released.value().dispatched;
            return MaiToolResult::success(output.dump());
        }
        if (args["action"] == "list_keys") {
            if (args.size() != 1)
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "list_keys has no other parameters");
            // This ordinary immutable directory has no dependency on a selected
            // session window, permissions, capture, focus or native input state.
            return MaiToolResult::success(Json{{"key_names", maiDesktopKeyNames()},
                                               {"key_tool", "computer"},
                                               {"key_method", "press_key"},
                                               {"text_tool", "computer"},
                                               {"text_method", "type_text"},
                                               {"native_support_checked_at_dispatch", true},
                                               {"note",
                                                "Names describe the shared grammar. Unsupported native mappings fail "
                                                "explicitly; use computer type_text for Unicode text."}}
                                              .dump());
        }
        if (args.contains("target")) {
            if (!args["target"].is_string() || (args["target"] != "display" && args["target"] != "window"))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "target must be display or window");
            if (args["target"] == "display") return executeDisplay(args, context);
            args.erase("target");
        }
        if (args["action"] == "list_displays") return executeDisplay(args, context);
        auto exact = [&args](std::initializer_list<const char*> allowed) {
            for (auto item = args.begin(); item != args.end(); ++item) {
                bool found = false;
                for (const char* field : allowed) found |= item.key() == field;
                if (!found) return false;
            }
            return true;
        };
        const std::string action = args["action"].get<std::string>();
        if (action == "clear") {
            if (!exact({"action"}))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "clear has no other parameters");
            const MaiError cleared = controller_->clear(context.sessionId, context.turnId);
            if (cleared) return failure(cleared);
            // Preserve the state field while making the cleared scope explicit to the Agent.
            return MaiToolResult::success(
                R"({"state":"cleared","scope":"window_observation",)"
                R"("text_changed":false,"next_step":"Use computer get_window_state before further window input."})");
        }
        if (!args.contains("snapshot_id"))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "snapshot_id is required for window actions");
        if (!args.value("snapshot_id", Json{}).is_string() || !args.value("window_id", Json{}).is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "action needs an observed snapshot and window ID");
        const auto id = windowIdFrom(args["window_id"]);
        if (!id) return failure(id.error());
        const bool positioned = action == "move" || action == "mouse_down";
        const bool buttonAction = action == "mouse_down";
        for (auto field = args.begin(); field != args.end(); ++field) {
            const std::string& name = field.key();
            const bool allowed = name == "action" || name == "snapshot_id" || name == "window_id" ||
                                 name == "dry_run" || (positioned && (name == "x_ratio" || name == "y_ratio")) ||
                                 (action == "key_down" && name == "key") || (buttonAction && name == "button") ||
                                 (action == "verify" && name == "expected_text");
            if (!allowed)
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "unexpected computer action parameter: " + name);
        }
        const auto locator = validateLocator(args, positioned);
        if (locator) return failure(locator);
        if ((positioned &&
             (!args.value("x_ratio", Json{}).is_number() || !args.value("y_ratio", Json{}).is_number())) ||
            (action == "key_down" && !args.value("key", Json{}).is_string()) ||
            (action == "verify" && !args.value("expected_text", Json{}).is_string()) ||
            (args.contains("dry_run") && !args["dry_run"].is_boolean()))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "computer action parameters are incomplete");
        DesktopComputerAction request;
        request.action = action;
        request.snapshotId = args["snapshot_id"].get<std::string>();
        request.windowId = id.value();
        request.xRatio = args.value("x_ratio", 0.0);
        request.yRatio = args.value("y_ratio", 0.0);
        request.key = args.value("key", std::string());
        request.expectedText = args.value("expected_text", std::string());
        request.turnId = context.turnId;
        if (buttonAction) {
            const auto button = mouseButtonFrom(args.value("button", Json("left")));
            if (!button) return failure(button.error());
            request.button = button.value();
        }
        request.dryRun = args.value("dry_run", false);
        request.isCanceled = [&context] { return context.isCanceled(); };
        DesktopComputerActionProgress progress;
        const auto acted = controller_->perform(context.sessionId, request, &progress);
        if (!acted) return actionFailure(acted.error(), context.sessionId, progress);
        Json output{{"state", acted.value().state},
                    {"action", action},
                    {"window_id", std::to_string(request.windowId)},
                    {"dispatched", acted.value().dispatched},
                    {"verified", acted.value().verified},
                    {"dry_run", acted.value().dryRun},
                    {"next_step", request.dryRun ? "No input was sent; the snapshot remains available."
                                                 : "Use computer get_window_state before the next window action. "
                                                   "Verify business persistence separately."}};
        output["input_state"] = inputStateJson(controller_->inputState(context.sessionId));
        if (acted.value().beforeWindow) output["before_window"] = windowJson(*acted.value().beforeWindow);
        if (acted.value().afterWindow) output["after_window"] = windowJson(*acted.value().afterWindow);
        if (acted.value().focused) output["focused"] = *acted.value().focused;
        if (action == "verify") {
            const QString actual =
                QString::fromUtf8(acted.value().actualText.data(), int(acted.value().actualText.size()));
            output["actual_text"] = actual.left(4000).toUtf8().toStdString();
            output["actual_text_truncated"] = actual.size() > 4000;
        }
        return MaiToolResult::success(output.dump());
    }

private:
    MaiError validateLocator(const Json& args, bool positioned) const {
        if (positioned && (!args.value("x_ratio", Json{}).is_number() || !args.value("y_ratio", Json{}).is_number()))
            return {MaiErrorCode::InvalidInput, "positioned input needs both x_ratio and y_ratio"};
        return {};
    }

    MaiToolResult actionFailure(const MaiError& error, const std::string& session,
                                const DesktopComputerActionProgress& progress) const {
        Json output{{"error", error.message()},
                    {"error_code", maiErrorCodeToString(error.code())},
                    {"input_dispatch_attempted", progress.inputDispatchAttempted},
                    {"window_operation_attempted", progress.windowOperationAttempted},
                    {"needs_observation", progress.needsObservation},
                    {"input_state", inputStateJson(controller_->inputState(session))}};
        const auto details = Json::parse(error.message(), nullptr, false);
        if (details.is_object() && details.value("reason", Json{}) == "point_outside_target") {
            for (const char* field : {"reason", "target_bounds", "requested_px", "requested_point",
                                      "execution_window_id", "native_screen_bounds_unit"})
                if (details.contains(field)) output[field] = details[field];
            output["status"] = "rejected";
        }
        return MaiToolResult::failure(error.code(), output.dump());
    }
    MaiToolResult executeInputEvents(const Json& args, const MaiToolContext& context) {
        for (auto field = args.begin(); field != args.end(); ++field)
            if (field.key() != "action" && field.key() != "target" && field.key() != "window_id" &&
                field.key() != "display_id" && field.key() != "snapshot_id" && field.key() != "events" &&
                field.key() != "input_options" && field.key() != "dry_run")
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "unexpected input_events parameter");
        if ((args.contains("target") &&
             (!args["target"].is_string() || (args["target"] != "display" && args["target"] != "window"))) ||
            (args.contains("dry_run") && !args["dry_run"].is_boolean()))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "input_events target/dry_run is invalid");
        const auto parsed =
            maiDesktopParseInputBatch(args.value("events", Json{}).dump(),
                                      args.contains("input_options") ? args["input_options"].dump() : std::string());
        if (!parsed) {
            if (!args.value("dry_run", false)) controller_->releaseInput(context.sessionId, context.turnId);
            return failure(parsed.error());
        }
        const bool releasing = maiDesktopInputEventsOnlyRelease(parsed.value().events);
        const bool display = args.value("target", std::string("window")) == "display";
        if (args.contains(display ? "window_id" : "display_id"))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "input_events cannot mix input targets");
        DesktopComputerAction request;
        request.action = "input_events";
        request.events = parsed.value().events;
        request.inputOptions = parsed.value().options;
        request.desktopScope = display;
        request.workspace = context.root;
        request.turnId = context.turnId;
        request.isCanceled = [&context] { return context.isCanceled(); };
        request.dryRun = args.value("dry_run", false);
        const char* targetField = display ? "display_id" : "window_id";
        if (!releasing || args.contains(targetField)) {
            const auto target = windowIdFrom(args.value(targetField, Json{}), targetField);
            if (!target) return failure(target.error());
            if (display)
                request.displayId = target.value();
            else
                request.windowId = target.value();
        }
        if (!releasing || args.contains("snapshot_id")) {
            if (!args.value("snapshot_id", Json{}).is_string())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "input_events needs a fresh snapshot_id");
            request.snapshotId = args["snapshot_id"].get<std::string>();
        }
        if (display && !releasing) {
            const auto modelInfo = maiCloudModelInfo(context.model);
            if ((modelInfo && !modelInfo->supportsImages) || context.model.compare(0, 6, "local:") == 0)
                return MaiToolResult::failure(MaiErrorCode::NotSupported,
                                              "Desktop screen operation requires an image-capable primary model");
        }
        DesktopComputerActionProgress progress;
        const auto result = controller_->perform(context.sessionId, request, &progress);
        if (!result) return actionFailure(result.error(), context.sessionId, progress);
        Json output{{"action", "input_events"},
                    {"state", result.value().state},
                    {"dispatched", result.value().dispatched},
                    {"dry_run", result.value().dryRun},
                    {"accepted_events", result.value().acceptedEvents},
                    {"input_state", inputStateJson(result.value().inputState)}};
        if (!result.value().observationError.empty()) {
            output["observation_error"] = result.value().observationError;
            output["next_step"] = "Input was dispatched. Observe again; do not repeat the batch blindly.";
        }
        if (!result.value().observation.json.empty()) {
            output["observation"] = Json::parse(result.value().observation.json);
            output["next_step"] = "Inspect the screen; use observation.snapshot_id for the next input.";
            return MaiToolResult::successWithImages(output.dump(),
                                                    {MaiToolImage{result.value().observation.imagePath, "image/png"}});
        }
        if (!releasing)
            output["next_step"] = request.dryRun ? "No input was sent; the snapshot remains available."
                                                 : "Observe again before the next input; verify task results.";
        return MaiToolResult::success(output.dump());
    }

    MaiToolResult executeDisplay(const Json& args, const MaiToolContext& context) {
        const std::string action = args["action"].get<std::string>();
        if (action == "list_displays") {
            if (args.size() != 1)
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "list_displays has no other parameters");
            const auto listed = controller_->displays();
            if (!listed) return failure(listed.error());
            Json items = Json::array();
            for (const auto& display : listed.value()) items.push_back(displayJson(display));
            return MaiToolResult::success(Json{
                {"displays", items}, {"next_step", "Observe target=display with one display_id before desktop input."}}
                                              .dump());
        }
        // Use the same current image boundary as the resident screenshot tool.
        // A text-only model must not inject against pixels it cannot observe.
        const auto modelInfo = maiCloudModelInfo(context.model);
        if ((modelInfo && !modelInfo->supportsImages) || context.model.compare(0, 6, "local:") == 0)
            return MaiToolResult::failure(
                MaiErrorCode::NotSupported,
                "Desktop screen operation requires an image-capable primary model; window metadata remains available.");
        const auto parsedId = windowIdFrom(args.value("display_id", Json{}), "display_id");
        if (!parsedId) return failure(parsedId.error());
        const bool observing = action == "observe";
        const bool positioned = action == "move" || action == "mouse_down";
        const bool buttonAction = action == "mouse_down";
        if (!observing && !positioned && action != "key_down")
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "unsupported desktop action");
        for (auto field = args.begin(); field != args.end(); ++field) {
            const std::string& name = field.key();
            const bool allowed =
                name == "action" || name == "target" || name == "display_id" ||
                (observing && (name == "wait_ms" || name == "element_window_id" || name == "execution_window_id")) ||
                (!observing && (name == "snapshot_id" || name == "dry_run")) ||
                (positioned && (name == "x_ratio" || name == "y_ratio")) || (action == "key_down" && name == "key") ||
                (buttonAction && name == "button");
            if (!allowed)
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "unexpected desktop action parameter: " + name);
        }
        if (observing) {
            if (context.root.empty())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "host workspace is missing; display observe requires host context.root");
            std::uint64_t sourceId = 0;
            std::uint64_t executionId = 0;
            if (args.contains("element_window_id")) {
                const auto source = windowIdFrom(args["element_window_id"], "element_window_id");
                if (!source) return failure(source.error());
                sourceId = source.value();
            }
            if (args.contains("execution_window_id")) {
                const auto receiver = windowIdFrom(args["execution_window_id"], "execution_window_id");
                if (!receiver) return failure(receiver.error());
                executionId = receiver.value();
            }
            if (args.contains("wait_ms") && (!args["wait_ms"].is_number_integer() ||
                                             args["wait_ms"].get<double>() < 0 || args["wait_ms"].get<double>() > 5000))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "display observation wait must be 0..5000ms");
            const auto observed = controller_->observeDisplay(
                context.sessionId, context.root, parsedId.value(), [&context] { return context.isCanceled(); }, 0, 0,
                args.value("wait_ms", 0), context.turnId, sourceId, executionId);
            if (!observed) return failure(observed.error());
            return MaiToolResult::successWithImages(observed.value().json,
                                                    {MaiToolImage{observed.value().imagePath, "image/png"}});
        }
        const auto locator = validateLocator(args, positioned);
        if (locator) return failure(locator);
        if (!args.contains("snapshot_id"))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "snapshot_id is required for display actions");
        if (!args.value("snapshot_id", Json{}).is_string() ||
            (positioned &&
             (!args.value("x_ratio", Json{}).is_number() || !args.value("y_ratio", Json{}).is_number())) ||
            (action == "key_down" && !args.value("key", Json{}).is_string()) ||
            (args.contains("dry_run") && !args["dry_run"].is_boolean()))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "desktop action parameters are incomplete");
        DesktopComputerAction request;
        request.desktopScope = true;
        request.workspace = context.root;
        request.displayId = parsedId.value();
        request.action = action;
        request.snapshotId = args["snapshot_id"].get<std::string>();
        request.xRatio = args.value("x_ratio", 0.0);
        request.yRatio = args.value("y_ratio", 0.0);
        request.key = args.value("key", std::string());
        request.turnId = context.turnId;
        if (buttonAction) {
            const auto button = mouseButtonFrom(args.value("button", Json("left")));
            if (!button) return failure(button.error());
            request.button = button.value();
        }
        request.dryRun = args.value("dry_run", false);
        request.isCanceled = [&context] { return context.isCanceled(); };
        DesktopComputerActionProgress progress;
        const auto result = controller_->perform(context.sessionId, request, &progress);
        if (!result) return actionFailure(result.error(), context.sessionId, progress);
        Json output{{"target", "display"},
                    {"display_id", std::to_string(request.displayId)},
                    {"action", action},
                    {"state", result.value().state},
                    {"dispatched", result.value().dispatched},
                    {"verified", result.value().verified},
                    {"dry_run", result.value().dryRun}};
        output["input_state"] = inputStateJson(controller_->inputState(context.sessionId));
        if (!result.value().observationError.empty()) {
            output["observation_error"] = result.value().observationError;
            output["next_step"] = "Input was dispatched. Observe again; do not blindly repeat the input.";
        }
        if (!result.value().observation.json.empty()) {
            output["observation"] = Json::parse(result.value().observation.json);
            output["next_step"] =
                "Inspect the returned screen; use observation.snapshot_id for the next action. Verify task results.";
            return MaiToolResult::successWithImages(output.dump(),
                                                    {MaiToolImage{result.value().observation.imagePath, "image/png"}});
        }
        return MaiToolResult::success(output.dump());
    }

    std::shared_ptr<DesktopComputerController> controller_;
};

}  // namespace

void registerDesktopComputerInputTools(MaiToolRegistry& registry,
                                       std::shared_ptr<DesktopComputerController> controller) {
    registry.add(std::make_unique<DesktopComputerInputTool>(std::move(controller)));
}
