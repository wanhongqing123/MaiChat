#include "MaiChatTools/DesktopComputerInputProtocol.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <utility>

#include <json.hpp>

namespace {
using Json = nlohmann::json;

MaiError invalid(const std::string& message) {
    return {MaiErrorCode::InvalidInput, message};
}

bool fields(const Json& object, std::initializer_list<const char*> allowed) {
    if (!object.is_object()) return false;
    for (auto field = object.begin(); field != object.end(); ++field)
        if (std::none_of(allowed.begin(), allowed.end(), [&](const char* name) { return field.key() == name; }))
            return false;
    return true;
}

bool unsignedValue(const Json& value, std::uint64_t maximum, std::uint64_t& output) {
    if (!value.is_number_integer()) return false;
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > maximum) return false;
        output = number;
        return true;
    }
    const auto number = value.get<std::int64_t>();
    if (number < 0 || std::uint64_t(number) > maximum) return false;
    output = std::uint64_t(number);
    return true;
}

template <typename T>
bool unsignedField(const Json& object, const char* name, T& output, bool required = false) {
    if (!object.contains(name)) return !required;
    std::uint64_t number = 0;
    if (!unsignedValue(object[name], std::numeric_limits<T>::max(), number)) return false;
    output = T(number);
    return true;
}

template <typename T>
bool signedField(const Json& object, const char* name, T& output, bool required = false) {
    if (!object.contains(name)) return !required;
    const auto& value = object[name];
    if (!value.is_number_integer()) return false;
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > std::uint64_t(std::numeric_limits<T>::max())) return false;
        output = T(number);
        return true;
    }
    const auto number = value.get<std::int64_t>();
    if (number < std::numeric_limits<T>::min() || number > std::numeric_limits<T>::max()) return false;
    output = T(number);
    return true;
}

template <typename T>
bool numberField(const Json& object, const char* name, T& output, bool required = false) {
    if (!object.contains(name)) return !required;
    if (!object[name].is_number()) return false;
    const double number = object[name].get<double>();
    if (!std::isfinite(number) || number < -double(std::numeric_limits<T>::max()) ||
        number > double(std::numeric_limits<T>::max()))
        return false;
    output = T(number);
    return true;
}

bool boolField(const Json& object, const char* name, bool& output, bool required = false) {
    if (!object.contains(name)) return !required;
    if (!object[name].is_boolean()) return false;
    output = object[name].get<bool>();
    return true;
}

bool phase(const Json& object, DesktopInputPhase& output) {
    if (!object.value("phase", Json{}).is_string()) return false;
    const auto name = object["phase"].get<std::string>();
    const std::pair<const char*, DesktopInputPhase> phases[] = {{"hover", DesktopInputPhase::Hover},
                                                                {"down", DesktopInputPhase::Down},
                                                                {"up", DesktopInputPhase::Up},
                                                                {"move", DesktopInputPhase::Move},
                                                                {"cancel", DesktopInputPhase::Cancel},
                                                                {"button_only", DesktopInputPhase::ButtonOnly},
                                                                {"hover_leave", DesktopInputPhase::HoverLeave},
                                                                {"cancel_all", DesktopInputPhase::CancelAll}};
    for (const auto& item : phases)
        if (name == item.first) {
            output = item.second;
            return true;
        }
    return false;
}

template <typename T>
bool unknownField(const Json& object, const char* name, T& output) {
    if (object.contains(name) && object[name].is_null()) {
        output = std::numeric_limits<T>::max();
        return true;
    }
    return unsignedField(object, name, output);
}

bool contact(const Json& value, DesktopInputTouch& output) {
    if (!fields(value, {"phase", "pointer_id", "x_ratio", "y_ratio", "pressure_or_distance", "contact_area_major",
                        "contact_area_minor", "rotation"}) ||
        !phase(value, output.phase) ||
        !unsignedField(value, "pointer_id", output.pointerId, output.phase != DesktopInputPhase::CancelAll) ||
        !unknownField(value, "rotation", output.rotation))
        return false;
    const bool positioned = output.phase == DesktopInputPhase::Down || output.phase == DesktopInputPhase::Move ||
                            output.phase == DesktopInputPhase::Hover;
    return numberField(value, "x_ratio", output.x, positioned) && numberField(value, "y_ratio", output.y, positioned) &&
           numberField(value, "pressure_or_distance", output.pressureOrDistance) &&
           numberField(value, "contact_area_major", output.contactAreaMajor) &&
           numberField(value, "contact_area_minor", output.contactAreaMinor);
}

MaiError event(const Json& value, DesktopInputEvent& output) {
    if (!value.is_object() || !value.value("type", Json{}).is_string() ||
        !unsignedField(value, "delay_ms", output.delayMs))
        return invalid("each input event needs a type and a bounded integer delay_ms");
    const auto type = value["type"].get<std::string>();
    if (type == "absolute_move") {
        output.kind = DesktopInputEventKind::AbsoluteMove;
        if (fields(value, {"type", "delay_ms", "x_ratio", "y_ratio"}) &&
            numberField(value, "x_ratio", output.x, true) && numberField(value, "y_ratio", output.y, true))
            return {};
    } else if (type == "relative_move") {
        output.kind = DesktopInputEventKind::RelativeMove;
        if (fields(value, {"type", "delay_ms", "delta_x", "delta_y"}) &&
            signedField(value, "delta_x", output.deltaX, true) && signedField(value, "delta_y", output.deltaY, true))
            return {};
    } else if (type == "mouse_button") {
        output.kind = DesktopInputEventKind::MouseButton;
        if (fields(value, {"type", "delay_ms", "button", "release"}) &&
            boolField(value, "release", output.release, true) && value.value("button", Json{}).is_string()) {
            const auto button = value["button"].get<std::string>();
            const char* names[] = {"left", "middle", "right", "back", "forward"};
            for (std::uint8_t index = 0; index < 5; ++index)
                if (button == names[index]) {
                    output.button = std::uint8_t(index + 1);
                    return {};
                }
        }
    } else if (type == "wheel") {
        output.kind = DesktopInputEventKind::Wheel;
        if (fields(value, {"type", "delay_ms", "axis", "delta_120"}) &&
            signedField(value, "delta_120", output.wheelDelta120, true) && value.value("axis", Json{}).is_string()) {
            const auto axis = value["axis"].get<std::string>();
            if (axis == "vertical" || axis == "horizontal") {
                output.horizontal = axis == "horizontal";
                return {};
            }
        }
    } else if (type == "raw_key") {
        output.kind = DesktopInputEventKind::RawKey;
        if (fields(value, {"type", "delay_ms", "code", "flags", "modifiers", "release"}) &&
            unsignedField(value, "code", output.key.code, true) && unsignedField(value, "flags", output.key.flags) &&
            unsignedField(value, "modifiers", output.key.modifiers) &&
            boolField(value, "release", output.key.release, true))
            return {};
    } else if (type == "unicode_text") {
        output.kind = DesktopInputEventKind::UnicodeText;
        if (fields(value, {"type", "delay_ms", "text"}) && value.value("text", Json{}).is_string()) {
            output.text = value["text"].get<std::string>();
            return {};
        }
    } else if (type == "touch") {
        output.kind = DesktopInputEventKind::Touch;
        if (fields(value, {"type", "delay_ms", "contacts"}) && value.value("contacts", Json{}).is_array() &&
            value["contacts"].size() <= 256) {
            for (const auto& item : value["contacts"]) {
                DesktopInputTouch touch;
                if (!contact(item, touch)) return invalid("invalid touch contact fields");
                output.contacts.push_back(touch);
            }
            return {};
        }
    } else if (type == "pen") {
        output.kind = DesktopInputEventKind::Pen;
        auto& pen = output.pen;
        if (fields(value, {"type", "delay_ms", "phase", "tool_type", "buttons", "tilt", "rotation", "x_ratio",
                           "y_ratio", "pressure_or_distance", "contact_area_major", "contact_area_minor"}) &&
            phase(value, pen.phase) && unsignedField(value, "tool_type", pen.toolType) &&
            unsignedField(value, "buttons", pen.buttons) && unknownField(value, "tilt", pen.tilt) &&
            unknownField(value, "rotation", pen.rotation)) {
            const bool positioned = pen.phase == DesktopInputPhase::Down || pen.phase == DesktopInputPhase::Move ||
                                    pen.phase == DesktopInputPhase::Hover;
            if (numberField(value, "x_ratio", pen.x, positioned) && numberField(value, "y_ratio", pen.y, positioned) &&
                numberField(value, "pressure_or_distance", pen.pressureOrDistance) &&
                numberField(value, "contact_area_major", pen.contactAreaMajor) &&
                numberField(value, "contact_area_minor", pen.contactAreaMinor))
                return {};
        }
    } else if (type == "gamepad_attach" || type == "gamepad_state" || type == "gamepad_touch" ||
               type == "gamepad_motion" || type == "gamepad_battery" || type == "gamepad_detach" ||
               type == "gamepad_rebind") {
        // Historical DTOs remain serializable, but virtual devices are outside
        // the current public input contract and must never reach native dispatch.
        return {MaiErrorCode::NotSupported, "virtual gamepad input is deferred"};
    }
    return invalid("unknown input event type or invalid fields for " + type);
}

MaiError options(const Json& value, DesktopInputOptions& output) {
    for (const char* name : {"controller_enabled", "ds4_back_as_touchpad", "motion_as_playstation",
                             "touchpad_as_playstation", "randomize_controller_mac", "driver", "default_profile",
                             "back_button_timeout_ms"})
        if (value.contains(name))
            return {MaiErrorCode::NotSupported, "virtual controller input_options are deferred"};
    if (!fields(value, {"keyboard_enabled", "mouse_enabled", "native_pen_touch_enabled", "always_send_scan_codes",
                        "right_alt_to_meta", "high_resolution_scroll", "keyboard_repeat", "repeat_delay_ms",
                        "repeat_period_ms", "key_bindings"}) ||
        !boolField(value, "keyboard_enabled", output.keyboardEnabled) ||
        !boolField(value, "mouse_enabled", output.mouseEnabled) ||
        !boolField(value, "native_pen_touch_enabled", output.nativePenTouchEnabled) ||
        !boolField(value, "always_send_scan_codes", output.alwaysSendScanCodes) ||
        !boolField(value, "right_alt_to_meta", output.rightAltToMeta) ||
        !boolField(value, "high_resolution_scroll", output.highResolutionScroll) ||
        !boolField(value, "keyboard_repeat", output.keyboardRepeat) ||
        !unsignedField(value, "repeat_delay_ms", output.repeatDelayMs) ||
        !numberField(value, "repeat_period_ms", output.repeatPeriodMs))
        return invalid("invalid input_options fields");
    if (value.contains("key_bindings")) {
        const auto& bindings = value["key_bindings"];
        if (!bindings.is_array() || bindings.size() > 65535) return invalid("key_bindings needs bounded code pairs");
        for (const auto& binding : bindings) {
            std::uint64_t from = 0, to = 0;
            if (!binding.is_array() || binding.size() != 2 || !unsignedValue(binding[0], 65535, from) ||
                !unsignedValue(binding[1], 65535, to))
                return invalid("key_bindings needs two unsigned 16-bit integer codes per pair");
            output.keyBindings.push_back({std::uint16_t(from), std::uint16_t(to)});
        }
    }
    return maiDesktopValidateInputOptions(output);
}

Json integerSchema(std::int64_t minimum, std::uint64_t maximum) {
    return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
}

Json contactSchema() {
    return {{"type", "object"},
            {"properties",
             {{"phase",
               {{"type", "string"}, {"enum", {"hover", "down", "up", "move", "cancel", "hover_leave", "cancel_all"}}}},
              {"pointer_id", integerSchema(0, 0xffffffff)},
              {"x_ratio", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
              {"y_ratio", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
              {"pressure_or_distance", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
              {"contact_area_major", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
              {"contact_area_minor", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
              {"rotation",
               {{"type", {"integer", "null"}},
                {"minimum", 0},
                {"maximum", 65535},
                {"description", "Degrees 0..360; null or 65535 means unknown."}}}}},
            {"required", {"phase"}},
            {"additionalProperties", false}};
}

Json eventSchema(const char* type, Json properties, Json required = Json::array()) {
    properties["type"] = {{"type", "string"}, {"enum", {type}}};
    properties["delay_ms"] = integerSchema(0, 5000);
    required.push_back("type");
    return {{"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(required)},
            {"additionalProperties", false}};
}

}  // namespace

MaiResult<std::uint64_t> maiDesktopInputSequence(const std::string& decimal) {
    if (decimal.empty() || decimal.size() > 20) return invalid("sequence must be an unsigned decimal string");
    std::uint64_t value = 0;
    for (char digit : decimal) {
        if (digit < '0' || digit > '9' ||
            value > (std::numeric_limits<std::uint64_t>::max() - std::uint64_t(digit - '0')) / 10)
            return invalid("sequence exceeds the unsigned 64-bit decimal range");
        value = value * 10 + std::uint64_t(digit - '0');
    }
    return value;
}

MaiResult<DesktopComputerInputBatchArguments> maiDesktopParseInputBatch(const std::string& eventsJson,
                                                                        const std::string& optionsJson) {
    const auto values = Json::parse(eventsJson, nullptr, false);
    const auto config = Json::parse(optionsJson.empty() ? "{}" : optionsJson, nullptr, false);
    if (!values.is_array() || values.empty() || values.size() > 256)
        return invalid("events needs 1..256 typed input events");
    DesktopComputerInputBatchArguments output;
    // 省略表示沿用该 native session 已选的配置；显式 {} 才表示恢复默认。
    // 这是工具适配元数据，不是可由模型注入的额外 options 字段。
    output.options.inheritExisting = optionsJson.empty();
    const auto configured = options(config, output.options);
    if (configured) return configured;
    output.events.reserve(values.size());
    for (const auto& value : values) {
        DesktopInputEvent parsed;
        const auto error = event(value, parsed);
        if (error) return error;
        output.events.push_back(std::move(parsed));
    }
    const auto validated = maiDesktopValidateInputEvents(output.events);
    if (validated) return validated;
    return output;
}

std::string maiDesktopComputerInputSchema(const std::string& legacySchema) {
    auto schema = Json::parse(legacySchema);
    auto& properties = schema["properties"];
    for (const char* action : {"input_events", "input_capabilities"})
        properties["action"]["enum"].push_back(action);
    Json variants = Json::array();
    const Json ratio = {{"type", "number"}, {"minimum", 0}, {"maximum", 1}};
    variants.push_back(eventSchema("absolute_move", {{"x_ratio", ratio}, {"y_ratio", ratio}}, {"x_ratio", "y_ratio"}));
    variants.push_back(eventSchema(
        "relative_move",
        {{"delta_x", integerSchema(-2147483648ll, 2147483647)}, {"delta_y", integerSchema(-2147483648ll, 2147483647)}},
        {"delta_x", "delta_y"}));
    variants.push_back(
        eventSchema("mouse_button",
                    {{"button", {{"type", "string"}, {"enum", {"left", "middle", "right", "back", "forward"}}}},
                     {"release", {{"type", "boolean"}}}},
                    {"button", "release"}));
    variants.push_back(eventSchema("wheel",
                                   {{"axis", {{"type", "string"}, {"enum", {"vertical", "horizontal"}}}},
                                    {"delta_120", integerSchema(-2147483648ll, 2147483647)}},
                                   {"axis", "delta_120"}));
    variants.back()["properties"]["delta_120"]["description"] =
        "Signed native wheel units: 120 per tick; positive is up vertically or right horizontally. No pointer warp.";
    variants.push_back(eventSchema("raw_key",
                                   {{"code", integerSchema(1, 65535)},
                                    {"flags", integerSchema(0, 1)},
                                    {"modifiers", integerSchema(0, 31)},
                                    {"release", {{"type", "boolean"}}}},
                                   {"code", "release"}));
    // 保留协议的编码空间和每一位含义，避免模型把 raw code 当成 Unicode 字符或扫描码。
    variants.back()["properties"]["code"]["description"] =
        "Sunshine 16-bit physical key code, including its legacy 0x8000 prefix; not a Unicode scalar.";
    variants.back()["properties"]["flags"]["description"] = "Bit 0 means non-normalized key code.";
    variants.back()["properties"]["modifiers"]["description"] =
        "Modifier-state bits: Shift=1, Control=2, Alt=4, Meta=8, extended key=16.";
    variants.push_back(
        eventSchema("unicode_text",
                    {{"text",
                      {{"type", "string"},
                       {"maxLength", 4000},
                       {"description",
                        "Literal valid UTF-8, at most 4000 bytes. Empty is a no-op; release owned modifiers first."}}}},
                    {"text"}));
    variants.push_back(eventSchema(
        "touch", {{"contacts", {{"type", "array"}, {"minItems", 1}, {"maxItems", 10}, {"items", contactSchema()}}}},
        {"contacts"}));
    variants.back()["description"] =
        "System touch input, up to ten owned contacts. Check input_capabilities before use. Contacts are admitted "
        "through full peer frames and the system source report schedule; this batch is not an atomic simultaneous "
        "gesture or acknowledgement from the target application. Observation does not release held contacts.";
    auto penProperties = contactSchema()["properties"];
    penProperties.erase("pointer_id");
    penProperties["phase"]["enum"].push_back("button_only");
    penProperties["tool_type"] = integerSchema(0, 2);
    penProperties["buttons"] = integerSchema(0, 7);
    penProperties["tilt"] = {{"type", {"integer", "null"}},
                             {"minimum", 0},
                             {"maximum", 255},
                             {"description", "Degrees 0..90; null or 255 means unknown."}};
    variants.push_back(eventSchema("pen", penProperties, {"phase"}));
    variants.back()["description"] =
        "System pen input. Check input_capabilities before use. Logical side buttons share one native barrel bit; "
        "tilt/azimuth map to native tilt, not shaft twist. Hover distance and contact ellipse have no Windows mapping. "
        "Dispatch is not acknowledgement from the target application.";
    properties["events"] = {{"type", "array"},
                            {"minItems", 1},
                            {"maxItems", 256},
                            {"items", {{"oneOf", variants}}},
                            {"description",
                             "Ordered primitive batch. One fresh target token; only owned release batches need no "
                             "token. Native errors can follow partial delivery."}};
    properties["input_options"] = {
        {"type", "object"},
        {"properties",
         {{"keyboard_enabled", {{"type", "boolean"}}},
          {"mouse_enabled", {{"type", "boolean"}}},
          {"native_pen_touch_enabled", {{"type", "boolean"}}},
          {"always_send_scan_codes", {{"type", "boolean"}}},
          {"right_alt_to_meta", {{"type", "boolean"}}},
          {"high_resolution_scroll", {{"type", "boolean"}}},
          {"keyboard_repeat", {{"type", "boolean"}}},
          {"repeat_delay_ms", integerSchema(0, std::numeric_limits<std::int32_t>::max())},
          {"repeat_period_ms",
           {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", std::numeric_limits<std::int32_t>::max()}}},
          {"key_bindings",
           {{"type", "array"},
            {"maxItems", 65535},
            {"items", {{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", integerSchema(1, 65535)}}}}}}},
        {"additionalProperties", false}};
    properties["input_options"]["description"] =
        "Omitted keeps this input session's current settings; explicit {} resets defaults.";
    return schema.dump();
}

std::string maiDesktopInputCapabilitiesJson(const DesktopInputCapabilities& capabilities) {
    Json features = Json::array();
    for (const auto& feature : capabilities.features)
        features.push_back({{"name", feature.name},
                            {"implemented", feature.implemented},
                            {"available", feature.available},
                            {"backend", feature.backend},
                            {"delivery", feature.delivery},
                            {"requirement", feature.requirement}});
    Json profiles = Json::array();
    for (const auto profile : capabilities.profiles) profiles.push_back(maiDesktopInputProfileName(profile));
    return Json{
        {"baseline", capabilities.baseline},
        {"features", features},
        {"profiles", profiles},
        {"max_controllers", capabilities.maxControllers},
        {"max_touch_contacts", capabilities.maxTouchContacts},
        {"native_verification", "Capability discovery is not an input delivery or application acknowledgement."}}
        .dump();
}

std::string maiDesktopInputFeedbackJson(const DesktopInputFeedbackBatch& batch) {
    Json events = Json::array();
    for (const auto& value : batch.events) {
        Json event{{"controller_id", value.controllerId},
                   {"generation", std::to_string(value.generation)},
                   {"sequence", std::to_string(value.sequence)},
                   {"received_at_ms", value.receivedAtMs}};
        switch (value.kind) {
            case DesktopInputFeedbackKind::Rumble:
                event.update({{"type", "rumble"},
                              {"low_frequency", value.lowFrequency},
                              {"high_frequency", value.highFrequency}});
                break;
            case DesktopInputFeedbackKind::TriggerRumble:
                event.update({{"type", "trigger_rumble"},
                              {"left", value.leftTriggerRumble},
                              {"right", value.rightTriggerRumble}});
                break;
            case DesktopInputFeedbackKind::MotionEventState:
                event.update({{"type", "motion_event_state"},
                              {"motion_type", value.motionType},
                              {"report_rate", value.reportRate}});
                break;
            case DesktopInputFeedbackKind::RgbLed:
                event.update({{"type", "rgb_led"}, {"red", value.red}, {"green", value.green}, {"blue", value.blue}});
                break;
            case DesktopInputFeedbackKind::PlayerLeds:
                event.update({{"type", "player_leds"},
                              {"solid", value.solidPlayerLeds},
                              {"flashing", value.flashingPlayerLeds}});
                break;
            case DesktopInputFeedbackKind::AdaptiveTriggers:
                event.update({{"type", "adaptive_triggers"},
                              {"event_flags", value.adaptiveEventFlags},
                              {"left_type", value.adaptiveLeftType},
                              {"right_type", value.adaptiveRightType},
                              {"left", value.adaptiveLeft},
                              {"right", value.adaptiveRight}});
                break;
        }
        events.push_back(std::move(event));
    }
    return Json{{"events", events},
                {"next_sequence", std::to_string(batch.nextSequence)},
                {"dropped_events", batch.droppedEvents},
                {"scope", "owned_input_feedback"}}
        .dump();
}
