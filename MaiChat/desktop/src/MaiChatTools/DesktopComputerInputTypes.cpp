#include "MaiChatTools/DesktopComputerInputTypes.h"

#include <cmath>
#include <unordered_set>

namespace {
using Kind = DesktopInputEventKind;
using Phase = DesktopInputPhase;

MaiError invalid(const std::string& message) {
    return {MaiErrorCode::InvalidInput, message};
}

bool unit(double value) {
    return std::isfinite(value) && value >= 0 && value <= 1;
}

bool releasePhase(Phase phase) {
    return phase == Phase::Up || phase == Phase::Cancel ||
           phase == Phase::HoverLeave || phase == Phase::CancelAll;
}

bool knownPhase(Phase phase) {
    switch (phase) {
    case Phase::Hover:
    case Phase::Down:
    case Phase::Up:
    case Phase::Move:
    case Phase::Cancel:
    case Phase::ButtonOnly:
    case Phase::HoverLeave:
    case Phase::CancelAll: return true;
    }
    return false;
}

bool knownDriver(DesktopInputDriver driver) {
    switch (driver) {
    case DesktopInputDriver::Auto:
    case DesktopInputDriver::VirtualHid:
    case DesktopInputDriver::ViGEm:
    case DesktopInputDriver::Disabled: return true;
    }
    return false;
}

bool rotation(std::uint16_t value) {
    return value <= 360 || value == 0xffff;
}

// Strict Unicode scalar decoding rejects overlong forms, surrogate encodings,
// truncation and values above U+10FFFF. The byte bound is checked by the caller;
// embedded NUL is a legal scalar because the ordinary API carries a length.
bool validUtf8(const std::string& text) {
    std::size_t position = 0;
    while (position < text.size()) {
        const auto lead = static_cast<unsigned char>(text[position]);
        if (lead < 0x80) {
            ++position;
            continue;
        }
        std::size_t width = 0;
        std::uint32_t scalar = 0;
        std::uint32_t minimum = 0;
        if (lead >= 0xc2 && lead <= 0xdf) {
            width = 2;
            scalar = lead & 0x1f;
            minimum = 0x80;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            width = 3;
            scalar = lead & 0x0f;
            minimum = 0x800;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            width = 4;
            scalar = lead & 0x07;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (text.size() - position < width) return false;
        for (std::size_t offset = 1; offset < width; ++offset) {
            const auto next = static_cast<unsigned char>(text[position + offset]);
            if ((next & 0xc0) != 0x80) return false;
            scalar = (scalar << 6) | (next & 0x3f);
        }
        if (scalar < minimum || scalar > 0x10ffff ||
            (scalar >= 0xd800 && scalar <= 0xdfff)) return false;
        position += width;
    }
    return true;
}

std::string touchError(const DesktopInputTouch& touch, bool controller) {
    if (!knownPhase(touch.phase)) return "unknown touch phase";
    if (touch.phase == Phase::ButtonOnly) return "button_only is a pen-only phase";
    if (controller && (touch.phase == Phase::Hover || touch.phase == Phase::HoverLeave))
        return "controller touch supports down, move, up, cancel and cancel_all";
    if (!std::isfinite(touch.x) || !std::isfinite(touch.y) ||
        !std::isfinite(touch.pressureOrDistance) || !std::isfinite(touch.contactAreaMajor) ||
        !std::isfinite(touch.contactAreaMinor)) return "touch geometry must be finite";
    // The native release paths consume only the ID/phase. Stale but finite
    // geometry must not prevent cleanup after a display or viewport change.
    if (releasePhase(touch.phase)) return {};
    if (!unit(touch.x) || !unit(touch.y) || !unit(touch.pressureOrDistance) ||
        !unit(touch.contactAreaMajor) || !unit(touch.contactAreaMinor))
        return "touch position, pressure/distance and contact axes must be in [0, 1]";
    if (!rotation(touch.rotation)) return "touch rotation must be 0..360 or 65535 (unknown)";
    return {};
}

std::string penError(const DesktopInputPen& pen) {
    if (!knownPhase(pen.phase)) return "unknown pen phase";
    if (pen.toolType > 2) return "pen tool must be unknown, pen or eraser (0..2)";
    if ((pen.buttons & 0xf8U) != 0) return "pen buttons contain undefined bits";
    if (!std::isfinite(pen.x) || !std::isfinite(pen.y) ||
        !std::isfinite(pen.pressureOrDistance) || !std::isfinite(pen.contactAreaMajor) ||
        !std::isfinite(pen.contactAreaMinor)) return "pen geometry must be finite";
    // Button-only events also carry irrelevant geometry in Sunshine's wire
    // structure; its unknown angle sentinels are not substitutes for zero.
    if (releasePhase(pen.phase) || pen.phase == Phase::ButtonOnly) return {};
    if (!unit(pen.x) || !unit(pen.y) || !unit(pen.pressureOrDistance) ||
        !unit(pen.contactAreaMajor) || !unit(pen.contactAreaMinor))
        return "pen position, pressure/distance and contact axes must be in [0, 1]";
    if (!rotation(pen.rotation)) return "pen rotation must be 0..360 or 65535 (unknown)";
    if (pen.tilt > 90 && pen.tilt != 0xff) return "pen tilt must be 0..90 or 255 (unknown)";
    return {};
}

std::string eventError(const DesktopInputEvent& event) {
    switch (event.kind) {
    case Kind::AbsoluteMove:
        return unit(event.x) && unit(event.y) ? "" : "absolute position must be finite and in [0, 1]";
    case Kind::RelativeMove:
    case Kind::Wheel:
        // Signed deltas retain every typed endpoint; the platform splits a
        // larger delta if its native report width is narrower.
        return {};
    case Kind::MouseButton:
        return event.button >= 1 && event.button <= 5 ? "" : "mouse button must be 1..5";
    case Kind::RawKey:
        if (event.key.code == 0) return "raw key code must be 1..65535";
        if ((event.key.flags & 0xfeU) != 0) return "raw key flags support only non-normalized bit 0";
        if ((event.key.modifiers & 0xe0U) != 0) return "raw key modifiers support bits 0..4";
        return {};
    case Kind::UnicodeText:
        if (event.text.size() > 4000) return "Unicode text must be at most 4000 UTF-8 bytes";
        return validUtf8(event.text) ? "" : "Unicode text must contain valid UTF-8 scalars";
    case Kind::Touch: {
        if (event.contacts.empty() || event.contacts.size() > 10)
            return "touch frame must contain 1..10 contacts";
        std::unordered_set<std::uint32_t> ids;
        for (const auto& contact : event.contacts) {
            if (!ids.insert(contact.pointerId).second) return "touch frame contains a duplicate pointer ID";
            if (const auto error = touchError(contact, false); !error.empty()) return error;
        }
        return {};
    }
    case Kind::Pen: return penError(event.pen);
    case Kind::GamepadAttach:
    case Kind::GamepadState:
    case Kind::GamepadTouch:
    case Kind::GamepadMotion:
    case Kind::GamepadBattery:
    case Kind::GamepadDetach:
    case Kind::GamepadRebind:
        if (event.gamepad.controllerId >= 16) return "controller ID must be 0..15";
        break;
    default: return "unknown input event kind";
    }
    const auto& gamepad = event.gamepad;
    switch (event.kind) {
    case Kind::GamepadAttach:
    case Kind::GamepadRebind:
        if (maiDesktopInputProfileName(gamepad.profile).empty()) return "unknown controller profile";
        if (gamepad.type > 4) return "controller type must be 0..4";
        // Capability and button fields are protocol bitsets, not enums. Keep
        // unknown/reserved bits for the backend mapping instead of narrowing.
        return {};
    case Kind::GamepadState:
    case Kind::GamepadDetach: return {};
    case Kind::GamepadTouch: return touchError(gamepad.touch, true);
    case Kind::GamepadMotion:
        if (gamepad.motionType != 1 && gamepad.motionType != 2)
            return "controller motion type must be accelerometer (1) or gyroscope (2)";
        if (!std::isfinite(gamepad.motionX) || !std::isfinite(gamepad.motionY) ||
            !std::isfinite(gamepad.motionZ)) return "controller motion axes must be finite";
        return {};
    case Kind::GamepadBattery:
        if (gamepad.batteryState > 5) return "controller battery state must be 0..5";
        if (gamepad.batteryPercentage > 100 && gamepad.batteryPercentage != 0xff)
            return "controller battery percentage must be 0..100 or 255 (unknown)";
        return {};
    default: return "unknown controller event kind";
    }
}

bool onlyRelease(const DesktopInputEvent& event) {
    switch (event.kind) {
    case Kind::MouseButton: return event.release;
    case Kind::RawKey: return event.key.release;
    case Kind::Touch:
        for (const auto& contact : event.contacts) {
            if (!releasePhase(contact.phase)) return false;
        }
        return true;
    case Kind::Pen:
        return event.pen.buttons == 0 &&
               (releasePhase(event.pen.phase) || event.pen.phase == Phase::ButtonOnly);
    case Kind::GamepadState: {
        const auto& state = event.gamepad;
        return state.buttonFlags == 0 && state.leftTrigger == 0 && state.rightTrigger == 0 &&
               state.leftX == 0 && state.leftY == 0 && state.rightX == 0 && state.rightY == 0;
    }
    case Kind::GamepadTouch: return releasePhase(event.gamepad.touch.phase);
    case Kind::GamepadDetach: return true;
    default: return false;
    }
}
} // namespace

MaiError maiDesktopValidateInputEvents(const std::vector<DesktopInputEvent>& events) {
    if (events.empty() || events.size() > 256) return invalid("input batch must contain 1..256 events");
    std::uint64_t totalDelay = 0;
    for (std::size_t index = 0; index < events.size(); ++index) {
        totalDelay += events[index].delayMs;
        if (totalDelay > 5000) return invalid("input batch delay must total at most 5000 ms");
        if (const auto error = eventError(events[index]); !error.empty())
            return invalid("events[" + std::to_string(index) + "]: " + error);
    }
    return {};
}

MaiError maiDesktopValidateInputOptions(const DesktopInputOptions& options) {
    if (!knownDriver(options.driver)) return invalid("unknown input driver");
    if (maiDesktopInputProfileName(options.defaultProfile).empty()) return invalid("unknown default controller profile");
    if (options.repeatDelayMs > 5000) return invalid("repeat delay must be 0..5000 ms");
    if (options.repeatPeriodMs == 0 || options.repeatPeriodMs > 1000)
        return invalid("repeat period must be 1..1000 ms");
    if (options.backButtonTimeoutMs < -1 || options.backButtonTimeoutMs > 5000)
        return invalid("Back button timeout must be -1..5000 ms");
    std::unordered_set<std::uint16_t> originals;
    for (const auto& binding : options.keyBindings) {
        if (binding[0] == 0 || binding[1] == 0) return invalid("key binding codes must be 1..65535");
        if (!originals.insert(binding[0]).second) return invalid("key bindings must have unique original codes");
    }
    // The ordinary DTO keeps its historical device fields for data compatibility.
    // Their defaults select no provider; a changed field must not silently
    // configure virtual input now that those capabilities are explicitly deferred.
    const DesktopInputOptions defaults;
    if (options.controllerEnabled != defaults.controllerEnabled ||
        options.ds4BackAsTouchpad != defaults.ds4BackAsTouchpad ||
        options.motionAsPlayStation != defaults.motionAsPlayStation ||
        options.touchpadAsPlayStation != defaults.touchpadAsPlayStation ||
        options.randomizeControllerMac != defaults.randomizeControllerMac || options.driver != defaults.driver ||
        options.defaultProfile != defaults.defaultProfile || options.backButtonTimeoutMs != defaults.backButtonTimeoutMs)
        return {MaiErrorCode::NotSupported, "virtual controller input_options are deferred"};

    return {};
}

bool maiDesktopInputEventsOnlyRelease(const std::vector<DesktopInputEvent>& events) {
    // This predicate is used after focus/token loss, so malformed events must
    // never gain the cleanup exception merely by carrying a release flag.
    if (maiDesktopValidateInputEvents(events)) return false;
    for (const auto& event : events) {
        if (!onlyRelease(event)) return false;
    }
    return true;
}

std::uint32_t maiDesktopInputKeyIdentity(const DesktopInputRawKey& key) {
    return (std::uint32_t(key.code) << 8) | key.flags;
}

DesktopInputRawKey maiDesktopRemapInputKey(const DesktopInputRawKey& source, const DesktopInputOptions& options) {
    auto result = source;
    const bool legacyNormalized = (source.code & 0x8000) != 0;
    auto code = std::uint16_t(legacyNormalized ? source.code & 0xff : source.code);
    bool mapped = false;
    for (const auto& binding : options.keyBindings) {
        if (binding[0] != code) continue;
        code = binding[1];
        mapped = true;
        break;
    }
    if (!mapped) {
        if (code == 0x10)
            code = 0xa0;
        else if (code == 0x11)
            code = 0xa2;
        else if (code == 0x12)
            code = 0xa4;
        else if (code == 0xa5 && options.rightAltToMeta)
            code = 0x5b;
    }
    // Only the native ledger calls this on the first down. Preserve the legacy
    // normalization marker; later up/repeat use its frozen binding instead.
    result.code = std::uint16_t(code | (legacyNormalized ? 0x8000 : 0));
    return result;
}

std::string maiDesktopInputProfileName(DesktopInputProfile profile) {
    switch (profile) {
    case DesktopInputProfile::Auto: return "auto";
    case DesktopInputProfile::Generic: return "generic";
    case DesktopInputProfile::Xbox360: return "x360";
    case DesktopInputProfile::XboxOne: return "xone";
    case DesktopInputProfile::XboxSeries: return "xseries";
    case DesktopInputProfile::DualShock4: return "ds4";
    case DesktopInputProfile::DualSense: return "ds5";
    case DesktopInputProfile::SwitchPro: return "switch";
    }
    return {};
}
