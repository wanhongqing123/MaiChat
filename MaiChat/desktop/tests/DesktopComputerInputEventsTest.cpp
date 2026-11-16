// Pure C++ contract checks. No Qt, devices, timers or OS input are involved.
#include "MaiChatTools/DesktopComputerInputTypes.h"

#include <cstdio>
#include <limits>
#include <type_traits>

namespace {
int failures = 0;
int checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) { \
    std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #value); ++failures; } } while (false)

using Kind = DesktopInputEventKind;
using Phase = DesktopInputPhase;

DesktopInputEvent event(Kind kind) {
    DesktopInputEvent result;
    result.kind = kind;
    result.key.code = 0x41;
    result.text = "text";
    result.contacts.push_back({});
    result.gamepad.motionType = 1;
    return result;
}

void valid(const DesktopInputEvent& value) {
    CHECK(!maiDesktopValidateInputEvents({value}));
}

void invalid(const DesktopInputEvent& value) {
    const auto error = maiDesktopValidateInputEvents({value});
    CHECK(error.code() == MaiErrorCode::InvalidInput);
    CHECK(!error.message().empty());
}

void batchAndKinds() {
    CHECK(maiDesktopValidateInputEvents({}).code() == MaiErrorCode::InvalidInput);
    for (int kind = 0; kind <= static_cast<int>(Kind::GamepadRebind); ++kind) {
        valid(event(static_cast<Kind>(kind)));
    }
    invalid(event(static_cast<Kind>(-1)));
    invalid(event(static_cast<Kind>(255)));
    std::vector<DesktopInputEvent> values(256, event(Kind::RelativeMove));
    values.front().delayMs = 5000;
    CHECK(!maiDesktopValidateInputEvents(values));
    values.back().delayMs = 1;
    CHECK(maiDesktopValidateInputEvents(values).code() == MaiErrorCode::InvalidInput);
    values.back().delayMs = 0;
    values.push_back(event(Kind::RelativeMove));
    CHECK(maiDesktopValidateInputEvents(values).code() == MaiErrorCode::InvalidInput);
    auto value = event(Kind::Wheel);
    value.delayMs = std::numeric_limits<std::uint32_t>::max();
    invalid(value);
}

void basicPrimitives() {
    auto value = event(Kind::AbsoluteMove);
    value.x = 0;
    value.y = 1;
    valid(value);
    value.x = -0.001;
    invalid(value);
    value.x = 1.001;
    invalid(value);
    value.x = std::numeric_limits<double>::quiet_NaN();
    invalid(value);
    value.x = 0;
    value.y = std::numeric_limits<double>::infinity();
    invalid(value);
    value = event(Kind::RelativeMove);
    value.deltaX = std::numeric_limits<std::int32_t>::min();
    value.deltaY = std::numeric_limits<std::int32_t>::max();
    valid(value);
    value = event(Kind::Wheel);
    for (auto delta : {std::numeric_limits<std::int32_t>::min(), -1, 0, 1,
                       std::numeric_limits<std::int32_t>::max()}) {
        value.wheelDelta120 = delta;
        value.horizontal = false;
        valid(value);
        value.horizontal = true;
        valid(value);
    }
    value = event(Kind::MouseButton);
    for (std::uint8_t button = 1; button <= 5; ++button) {
        value.button = button;
        value.release = false;
        valid(value);
        value.release = true;
        valid(value);
    }
    value.button = 0;
    invalid(value);
    value.button = 255;
    invalid(value);
}

void rawKeysAndText() {
    auto value = event(Kind::RawKey);
    for (std::uint16_t code : {std::uint16_t{1}, std::uint16_t{0x100}, std::uint16_t{0x800d},
                              std::uint16_t{0xffff}}) {
        value.key.code = code;
        value.key.flags = 1;
        value.key.modifiers = 0x1f;
        valid(value);
        value.key.release = true;
        valid(value);
    }
    value.key.code = 0;
    invalid(value);
    value.key.code = 1;
    value.key.flags = 2;
    invalid(value);
    value.key.flags = 0xff;
    invalid(value);
    value.key.flags = 0;
    value.key.modifiers = 0x20;
    invalid(value);
    value.key.modifiers = 0xff;
    invalid(value);

    value = event(Kind::UnicodeText);
    for (const auto& text : {std::string(), std::string(4000, 'a'),
            std::string("\x00\x7f", 2), std::string("\xc2\x80\xdf\xbf"),
            std::string("\xe0\xa0\x80\xed\x9f\xbf\xee\x80\x80"),
            std::string("\xf0\x90\x80\x80\xf4\x8f\xbf\xbf")}) {
        value.text = text;
        valid(value);
    }
    for (const auto& text : {std::string(4001, 'a'), std::string("\x80"), std::string("\xc0\x80"),
            std::string("\xc1\xbf"), std::string("\xc2"), std::string("\xc2\x20"),
            std::string("\xe0\x9f\xbf"), std::string("\xed\xa0\x80"),
            std::string("\xf0\x8f\xbf\xbf"), std::string("\xf4\x90\x80\x80"),
            std::string("\xf5\x80\x80\x80"), std::string("\xff"), std::string("\xe1\x80"),
            std::string("\xf1\x80\x80")}) {
        value.text = text;
        invalid(value);
    }
    value.text.clear();
    for (int index = 0; index < 1000; ++index) value.text += "\xf0\x9f\x98\x80";
    valid(value);
    value.text += "a";
    invalid(value);
}

void editingKeysCanonicalizeBeforeBindings() {
    for (std::uint16_t offset = 0; offset < 4; ++offset) {
        DesktopInputRawKey source;
        source.code = std::uint16_t(0x7a + offset);
        source.flags = 1;
        source.modifiers = 0x12;
        const auto identity = maiDesktopInputKeyIdentity(source);
        const auto canonical = std::uint16_t(0x100 + offset);
        const auto prepared = maiDesktopRemapInputKey(source, {});
        CHECK(prepared.code == canonical);
        CHECK(prepared.flags == source.flags && prepared.modifiers == source.modifiers);
        CHECK(source.code == std::uint16_t(0x7a + offset) && maiDesktopInputKeyIdentity(source) == identity);

        DesktopInputOptions selected;
        selected.keyBindings = {{{source.code, 'B'}}, {{canonical, 'A'}}};
        CHECK(maiDesktopRemapInputKey(source, selected).code == 'A');
        auto dedicated = source;
        dedicated.code = canonical;
        CHECK(maiDesktopRemapInputKey(dedicated, {}).code == canonical);
        CHECK(maiDesktopRemapInputKey(dedicated, selected).code == 'A');

        // Canonical editing bindings do not redefine ordinary F11-F14 forms.
        selected.keyBindings = {{{canonical, 'A'}}};
        auto function = source;
        function.code |= 0x8000;
        CHECK(maiDesktopRemapInputKey(function, {}).code == function.code);
        CHECK(maiDesktopRemapInputKey(function, selected).code == function.code);
        function = source;
        function.flags = 0;
        CHECK(maiDesktopRemapInputKey(function, {}).code == function.code);
        CHECK(maiDesktopRemapInputKey(function, selected).code == function.code);
    }
    for (const std::uint16_t boundary : {std::uint16_t{0x79}, std::uint16_t{0x7e}}) {
        DesktopInputRawKey source;
        source.code = boundary;
        source.flags = 1;
        CHECK(maiDesktopRemapInputKey(source, {}).code == boundary);
    }
}

void touchAndPen() {
    auto value = event(Kind::Touch);
    for (auto phase : {Phase::Hover, Phase::Down, Phase::Up, Phase::Move, Phase::Cancel,
                       Phase::HoverLeave, Phase::CancelAll}) {
        value.contacts.front().phase = phase;
        valid(value);
    }
    value.contacts.front().phase = Phase::ButtonOnly;
    invalid(value);
    value.contacts.front().phase = static_cast<Phase>(255);
    invalid(value);
    value.contacts.front().phase = Phase::Move;
    for (std::uint16_t angle : {std::uint16_t{0}, std::uint16_t{360}, std::uint16_t{65535}}) {
        value.contacts.front().rotation = angle;
        valid(value);
    }
    value.contacts.front().rotation = 361;
    invalid(value);
    value.contacts.front().rotation = 65534;
    invalid(value);
    value.contacts.front().rotation = 0;
    for (auto member : {&DesktopInputTouch::x, &DesktopInputTouch::y,
            &DesktopInputTouch::pressureOrDistance, &DesktopInputTouch::contactAreaMajor,
            &DesktopInputTouch::contactAreaMinor}) {
        auto candidate = value;
        candidate.contacts.front().*member = 1;
        valid(candidate);
        candidate.contacts.front().*member = -0.001F;
        invalid(candidate);
        candidate.contacts.front().*member = 1.001F;
        invalid(candidate);
        candidate.contacts.front().*member = std::numeric_limits<float>::quiet_NaN();
        invalid(candidate);
        candidate.contacts.front().*member = std::numeric_limits<float>::infinity();
        invalid(candidate);
    }
    value.contacts.clear();
    invalid(value);
    for (std::uint32_t id = 0; id < 10; ++id) {
        DesktopInputTouch contact;
        contact.pointerId = id;
        value.contacts.push_back(contact);
    }
    valid(value);
    value.contacts.back().pointerId = 0xffffffff;
    valid(value);
    value.contacts.back().pointerId = value.contacts.front().pointerId;
    invalid(value);
    value.contacts.back().pointerId = 9;
    value.contacts.push_back({});
    value.contacts.back().pointerId = 10;
    invalid(value);

    value = event(Kind::Pen);
    for (int phase = 0; phase <= 7; ++phase) {
        value.pen.phase = static_cast<Phase>(phase);
        valid(value);
    }
    value.pen.phase = static_cast<Phase>(255);
    invalid(value);
    value.pen.phase = Phase::Move;
    for (std::uint8_t tool = 0; tool <= 2; ++tool) {
        value.pen.toolType = tool;
        value.pen.buttons = 7;
        valid(value);
    }
    value.pen.toolType = 3;
    invalid(value);
    value.pen.toolType = 1;
    value.pen.buttons = 8;
    invalid(value);
    value.pen.buttons = 0;
    for (std::uint8_t tilt : {std::uint8_t{0}, std::uint8_t{90}, std::uint8_t{255}}) {
        value.pen.tilt = tilt;
        valid(value);
    }
    for (std::uint8_t tilt : {std::uint8_t{91}, std::uint8_t{254}}) {
        value.pen.tilt = tilt;
        invalid(value);
    }
    value.pen.tilt = 0;
    value.pen.rotation = 360;
    valid(value);
    value.pen.rotation = 65535;
    valid(value);
    value.pen.rotation = 361;
    invalid(value);
    value.pen.rotation = 0;
    for (auto member : {&DesktopInputPen::x, &DesktopInputPen::y,
            &DesktopInputPen::pressureOrDistance, &DesktopInputPen::contactAreaMajor,
            &DesktopInputPen::contactAreaMinor}) {
        auto candidate = value;
        candidate.pen.*member = 1;
        valid(candidate);
        candidate.pen.*member = -0.001F;
        invalid(candidate);
        candidate.pen.*member = 1.001F;
        invalid(candidate);
        candidate.pen.*member = std::numeric_limits<float>::quiet_NaN();
        invalid(candidate);
    }

    // Native cleanup ignores geometry, while non-finite values never enter a
    // session. Out-of-range stale coordinates must not prevent a release.
    for (auto phase : {Phase::Up, Phase::Cancel, Phase::CancelAll, Phase::HoverLeave}) {
        value = event(Kind::Touch);
        value.contacts.front().phase = phase;
        value.contacts.front().x = -100;
        value.contacts.front().pressureOrDistance = 100;
        value.contacts.front().rotation = 65534;
        valid(value);
        value.contacts.front().y = std::numeric_limits<float>::quiet_NaN();
        invalid(value);
    }
    for (auto phase : {Phase::Up, Phase::Cancel, Phase::CancelAll, Phase::HoverLeave, Phase::ButtonOnly}) {
        value = event(Kind::Pen);
        value.pen.phase = phase;
        value.pen.x = -100;
        value.pen.contactAreaMajor = 100;
        value.pen.rotation = 65534;
        value.pen.tilt = 254;
        valid(value);
        value.pen.y = std::numeric_limits<float>::infinity();
        invalid(value);
    }
}

void controllers() {
    for (auto kind : {Kind::GamepadAttach, Kind::GamepadState, Kind::GamepadTouch,
            Kind::GamepadMotion, Kind::GamepadBattery, Kind::GamepadDetach, Kind::GamepadRebind}) {
        auto value = event(kind);
        for (std::uint8_t id : {std::uint8_t{0}, std::uint8_t{15}}) {
            value.gamepad.controllerId = id;
            valid(value);
        }
        value.gamepad.controllerId = 16;
        invalid(value);
        value.gamepad.controllerId = 255;
        invalid(value);
    }
    auto value = event(Kind::GamepadAttach);
    for (int profile = 0; profile <= static_cast<int>(DesktopInputProfile::SwitchPro); ++profile) {
        value.gamepad.profile = static_cast<DesktopInputProfile>(profile);
        valid(value);
        CHECK(!maiDesktopInputProfileName(value.gamepad.profile).empty());
    }
    value.gamepad.profile = static_cast<DesktopInputProfile>(255);
    invalid(value);
    value.kind = Kind::GamepadRebind;
    invalid(value);
    CHECK(maiDesktopInputProfileName(static_cast<DesktopInputProfile>(255)).empty());
    value = event(Kind::GamepadAttach);
    for (std::uint8_t type = 0; type <= 4; ++type) {
        value.gamepad.type = type;
        value.gamepad.capabilities = 0xffff;
        value.gamepad.supportedButtons = 0xffffffff;
        valid(value);
    }
    value.gamepad.type = 5;
    invalid(value);
    value.kind = Kind::GamepadRebind;
    invalid(value);
    value = event(Kind::GamepadState);
    value.gamepad.buttonFlags = 0xffffffff;
    value.gamepad.leftTrigger = 255;
    value.gamepad.rightTrigger = 255;
    value.gamepad.leftX = std::numeric_limits<std::int16_t>::min();
    value.gamepad.leftY = std::numeric_limits<std::int16_t>::max();
    value.gamepad.rightX = std::numeric_limits<std::int16_t>::min();
    value.gamepad.rightY = std::numeric_limits<std::int16_t>::max();
    valid(value);
    value = event(Kind::GamepadMotion);
    value.gamepad.motionType = 2;
    value.gamepad.motionX = std::numeric_limits<float>::max();
    value.gamepad.motionY = -std::numeric_limits<float>::max();
    valid(value);
    value.gamepad.motionType = 0;
    invalid(value);
    value.gamepad.motionType = 3;
    invalid(value);
    value.gamepad.motionType = 1;
    for (auto member : {&DesktopInputGamepad::motionX, &DesktopInputGamepad::motionY,
                       &DesktopInputGamepad::motionZ}) {
        auto candidate = value;
        candidate.gamepad.*member = std::numeric_limits<float>::quiet_NaN();
        invalid(candidate);
        candidate.gamepad.*member = std::numeric_limits<float>::infinity();
        invalid(candidate);
    }
    value = event(Kind::GamepadBattery);
    for (std::uint8_t state = 0; state <= 5; ++state) {
        value.gamepad.batteryState = state;
        for (std::uint8_t percentage : {std::uint8_t{0}, std::uint8_t{100}, std::uint8_t{255}}) {
            value.gamepad.batteryPercentage = percentage;
            valid(value);
        }
    }
    value.gamepad.batteryState = 6;
    invalid(value);
    value.gamepad.batteryState = 0;
    value.gamepad.batteryPercentage = 101;
    invalid(value);
    value.gamepad.batteryPercentage = 254;
    invalid(value);
    value = event(Kind::GamepadTouch);
    for (auto phase : {Phase::Down, Phase::Move, Phase::Up, Phase::Cancel, Phase::CancelAll}) {
        value.gamepad.touch.phase = phase;
        valid(value);
    }
    for (auto phase : {Phase::Hover, Phase::HoverLeave, Phase::ButtonOnly, static_cast<Phase>(255)}) {
        value.gamepad.touch.phase = phase;
        invalid(value);
    }
    value.gamepad.touch.phase = Phase::Move;
    value.gamepad.touch.x = 1.1F;
    invalid(value);
    value.gamepad.touch.phase = Phase::Cancel;
    valid(value);
    value.gamepad.touch.x = std::numeric_limits<float>::infinity();
    invalid(value);
}

void optionsAndNames() {
    DesktopInputOptions options;
    CHECK(options.keyboardEnabled && options.mouseEnabled && options.controllerEnabled &&
          options.nativePenTouchEnabled && options.alwaysSendScanCodes && !options.rightAltToMeta &&
          options.ds4BackAsTouchpad && options.motionAsPlayStation && options.touchpadAsPlayStation &&
          options.randomizeControllerMac && options.highResolutionScroll && options.keyboardRepeat);
    CHECK(options.driver == DesktopInputDriver::Auto && options.defaultProfile == DesktopInputProfile::Auto);
    CHECK(options.repeatDelayMs == 500 && options.repeatPeriodMs == 1000.0 / 24.9 && options.backButtonTimeoutMs == -1);
    CHECK(!maiDesktopValidateInputOptions(options));
    for (auto driver : {DesktopInputDriver::Auto, DesktopInputDriver::VirtualHid,
                        DesktopInputDriver::ViGEm, DesktopInputDriver::Disabled}) {
        options.driver = driver;
        CHECK(maiDesktopValidateInputOptions(options).code() ==
              (driver == DesktopInputDriver::Auto ? MaiErrorCode::Ok : MaiErrorCode::NotSupported));
    }
    options.driver = static_cast<DesktopInputDriver>(-1);
    CHECK(maiDesktopValidateInputOptions(options).code() == MaiErrorCode::InvalidInput);
    options.driver = static_cast<DesktopInputDriver>(255);
    CHECK(maiDesktopValidateInputOptions(options).code() == MaiErrorCode::InvalidInput);
    options.driver = DesktopInputDriver::Auto;
    options.defaultProfile = static_cast<DesktopInputProfile>(-1);
    CHECK(maiDesktopValidateInputOptions(options).code() == MaiErrorCode::InvalidInput);
    options.defaultProfile = static_cast<DesktopInputProfile>(255);
    CHECK(maiDesktopValidateInputOptions(options).code() == MaiErrorCode::InvalidInput);
    options.defaultProfile = DesktopInputProfile::Auto;
    options.repeatDelayMs = 0;
    options.repeatPeriodMs = 1;
    CHECK(!maiDesktopValidateInputOptions(options));
    options.repeatDelayMs = 5000;
    options.repeatPeriodMs = 1000;
    options.keyBindings = {{{1, 0xffff}}, {{0xffff, 1}}};
    CHECK(!maiDesktopValidateInputOptions(options));
    auto candidate = options;
    candidate.repeatDelayMs = std::uint32_t(std::numeric_limits<std::int32_t>::max()) + 1;
    CHECK(maiDesktopValidateInputOptions(candidate).code() == MaiErrorCode::InvalidInput);
    candidate = options;
    candidate.repeatPeriodMs = 0;
    CHECK(maiDesktopValidateInputOptions(candidate).code() == MaiErrorCode::InvalidInput);
    candidate.repeatPeriodMs = std::uint32_t(std::numeric_limits<std::int32_t>::max()) + 1;
    CHECK(maiDesktopValidateInputOptions(candidate).code() == MaiErrorCode::InvalidInput);
    candidate = options;
    candidate.keyboardRepeat = false;
    candidate.repeatPeriodMs = 0;
    CHECK(maiDesktopValidateInputOptions(candidate).code() == MaiErrorCode::InvalidInput);
    candidate = options;
    candidate.backButtonTimeoutMs = -2;
    CHECK(maiDesktopValidateInputOptions(candidate).code() == MaiErrorCode::InvalidInput);
    candidate.backButtonTimeoutMs = 5001;
    CHECK(maiDesktopValidateInputOptions(candidate).code() == MaiErrorCode::InvalidInput);
    candidate = options;
    candidate.keyBindings.push_back({{1, 2}});
    CHECK(maiDesktopValidateInputOptions(candidate).code() == MaiErrorCode::InvalidInput);
    candidate = options;
    candidate.keyBindings.front()[0] = 0;
    CHECK(maiDesktopValidateInputOptions(candidate).code() == MaiErrorCode::InvalidInput);
    candidate = options;
    candidate.keyBindings.front()[1] = 0;
    CHECK(maiDesktopValidateInputOptions(candidate).code() == MaiErrorCode::InvalidInput);
    const char* names[] = {"auto", "generic", "x360", "xone", "xseries", "ds4", "ds5", "switch"};
    for (int index = 0; index < 8; ++index)
        CHECK(maiDesktopInputProfileName(static_cast<DesktopInputProfile>(index)) == names[index]);
}

void deferredOptionsCannotSilentlyConfigureOrdinaryInput() {
    const DesktopInputOptions defaults;
    for (auto member : {&DesktopInputOptions::controllerEnabled, &DesktopInputOptions::ds4BackAsTouchpad,
                         &DesktopInputOptions::motionAsPlayStation, &DesktopInputOptions::touchpadAsPlayStation,
                         &DesktopInputOptions::randomizeControllerMac}) {
        auto candidate = defaults;
        candidate.*member = !(candidate.*member);
        CHECK(maiDesktopValidateInputOptions(candidate).code() == MaiErrorCode::NotSupported);
    }
    for (auto profile : {DesktopInputProfile::Generic, DesktopInputProfile::Xbox360, DesktopInputProfile::XboxOne,
                         DesktopInputProfile::XboxSeries, DesktopInputProfile::DualShock4,
                         DesktopInputProfile::DualSense, DesktopInputProfile::SwitchPro}) {
        auto candidate = defaults;
        candidate.defaultProfile = profile;
        CHECK(maiDesktopValidateInputOptions(candidate).code() == MaiErrorCode::NotSupported);
    }
    for (const auto timeout : {0, 1, 5000}) {
        auto candidate = defaults;
        candidate.backButtonTimeoutMs = timeout;
        CHECK(maiDesktopValidateInputOptions(candidate).code() == MaiErrorCode::NotSupported);
    }
    // Invalid data remains invalid even if another field requests a deferred
    // feature. Deferral is not a shortcut around the ordinary DTO preflight.
    auto candidate = defaults;
    candidate.controllerEnabled = false;
    candidate.driver = static_cast<DesktopInputDriver>(255);
    CHECK(maiDesktopValidateInputOptions(candidate).code() == MaiErrorCode::InvalidInput);
    candidate.driver = DesktopInputDriver::VirtualHid;
    candidate.defaultProfile = static_cast<DesktopInputProfile>(255);
    CHECK(maiDesktopValidateInputOptions(candidate).code() == MaiErrorCode::InvalidInput);
    // Every current ordinary switch can still be configured independently.
    for (auto member : {&DesktopInputOptions::keyboardEnabled, &DesktopInputOptions::mouseEnabled,
                         &DesktopInputOptions::nativePenTouchEnabled, &DesktopInputOptions::alwaysSendScanCodes,
                         &DesktopInputOptions::rightAltToMeta, &DesktopInputOptions::highResolutionScroll,
                         &DesktopInputOptions::keyboardRepeat, &DesktopInputOptions::inheritExisting}) {
        candidate = defaults;
        candidate.*member = !(candidate.*member);
        CHECK(!maiDesktopValidateInputOptions(candidate));
    }
}

template <class Period>
void invalidFractionalRepeatPeriods(Period DesktopInputOptions::* member) {
    CHECK((std::is_same<Period, double>::value));
    if constexpr (std::is_floating_point<Period>::value) {
        for (const double bad :
             {0.0, -0.25, -1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
              -std::numeric_limits<double>::infinity(), double(std::numeric_limits<std::int32_t>::max()) + 1}) {
            DesktopInputOptions candidate;
            candidate.*member = bad;
            CHECK(maiDesktopValidateInputOptions(candidate).code() == MaiErrorCode::InvalidInput);
        }
    }
}

void fractionalAndSlowRepeatDomain() {
    DesktopInputOptions options;
    options.repeatDelayMs = std::uint32_t(std::numeric_limits<std::int32_t>::max());
    CHECK(!maiDesktopValidateInputOptions(options));
    options.repeatDelayMs = 500;
    for (const double period :
         {0.25, 1.5, 1000.0 / 24.9, 1001.0, 60000.5, double(std::numeric_limits<std::int32_t>::max()),
          std::numeric_limits<double>::denorm_min()}) {
        options.repeatPeriodMs = static_cast<decltype(options.repeatPeriodMs)>(period);
        CHECK(options.repeatPeriodMs == period);
        CHECK(!maiDesktopValidateInputOptions(options));
    }
    invalidFractionalRepeatPeriods(&DesktopInputOptions::repeatPeriodMs);
}

void releases() {
    CHECK(!maiDesktopInputEventsOnlyRelease({}));
    for (int kind = 0; kind <= static_cast<int>(Kind::GamepadRebind); ++kind) {
        const auto value = event(static_cast<Kind>(kind));
        const bool expected = value.kind == Kind::GamepadState || value.kind == Kind::GamepadDetach;
        CHECK(maiDesktopInputEventsOnlyRelease({value}) == expected);
    }
    auto value = event(Kind::MouseButton);
    value.release = true;
    CHECK(maiDesktopInputEventsOnlyRelease({value}));
    value.button = 0;
    CHECK(!maiDesktopInputEventsOnlyRelease({value}));
    value = event(Kind::RawKey);
    value.key.release = true;
    CHECK(maiDesktopInputEventsOnlyRelease({value}));
    for (auto phase : {Phase::Up, Phase::Cancel, Phase::HoverLeave, Phase::CancelAll}) {
        value = event(Kind::Touch);
        value.contacts.front().phase = phase;
        CHECK(maiDesktopInputEventsOnlyRelease({value}));
        value = event(Kind::Pen);
        value.pen.phase = phase;
        CHECK(maiDesktopInputEventsOnlyRelease({value}));
        value.pen.buttons = 1;
        CHECK(!maiDesktopInputEventsOnlyRelease({value}));
    }
    value = event(Kind::Pen);
    value.pen.phase = Phase::ButtonOnly;
    CHECK(maiDesktopInputEventsOnlyRelease({value}));
    value.pen.buttons = 4;
    CHECK(!maiDesktopInputEventsOnlyRelease({value}));
    value = event(Kind::Touch);
    value.contacts.front().phase = Phase::Up;
    value.contacts.push_back({});
    value.contacts.back().pointerId = 1;
    CHECK(!maiDesktopInputEventsOnlyRelease({value}));
    value.contacts.back().phase = Phase::Cancel;
    CHECK(maiDesktopInputEventsOnlyRelease({value}));
    value = event(Kind::GamepadState);
    value.gamepad.leftTrigger = 1;
    CHECK(!maiDesktopInputEventsOnlyRelease({value}));
    value.gamepad.leftTrigger = 0;
    value.gamepad.rightTrigger = 1;
    CHECK(!maiDesktopInputEventsOnlyRelease({value}));
    value.gamepad.rightTrigger = 0;
    value.gamepad.buttonFlags = 0x80000000;
    CHECK(!maiDesktopInputEventsOnlyRelease({value}));
    value.gamepad.buttonFlags = 0;
    for (auto member : {&DesktopInputGamepad::leftX, &DesktopInputGamepad::leftY,
                       &DesktopInputGamepad::rightX, &DesktopInputGamepad::rightY}) {
        auto candidate = value;
        candidate.gamepad.*member = 1;
        CHECK(!maiDesktopInputEventsOnlyRelease({candidate}));
        candidate.gamepad.*member = -1;
        CHECK(!maiDesktopInputEventsOnlyRelease({candidate}));
    }
    value = event(Kind::GamepadTouch);
    value.gamepad.touch.phase = Phase::Cancel;
    CHECK(maiDesktopInputEventsOnlyRelease({value}));
    value.kind = static_cast<Kind>(255);
    CHECK(!maiDesktopInputEventsOnlyRelease({value}));
    value = event(Kind::GamepadDetach);
    CHECK(!maiDesktopInputEventsOnlyRelease({value, event(Kind::RelativeMove)}));
}
} // namespace

int main() {
    batchAndKinds();
    basicPrimitives();
    rawKeysAndText();
    editingKeysCanonicalizeBeforeBindings();
    touchAndPen();
    controllers();
    optionsAndNames();
    deferredOptionsCannotSilentlyConfigureOrdinaryInput();
    fractionalAndSlowRepeatDomain();
    releases();
    std::printf("DesktopComputerInputEventsTest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
