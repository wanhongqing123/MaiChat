#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "MaiError.h"

// Independent of JSON, Qt and the HID SDK. Values preserve Sunshine's input
// widths and units; absolute positions are normalized within the observed port.
enum class DesktopInputEventKind {
    AbsoluteMove,
    RelativeMove,
    MouseButton,
    Wheel,
    RawKey,
    UnicodeText,
    Touch,
    Pen,
    GamepadAttach,
    GamepadState,
    GamepadTouch,
    GamepadMotion,
    GamepadBattery,
    GamepadDetach,
    GamepadRebind
};

enum class DesktopInputPhase : std::uint8_t {
    Hover = 0,
    Down = 1,
    Up = 2,
    Move = 3,
    Cancel = 4,
    ButtonOnly = 5,
    HoverLeave = 6,
    CancelAll = 7
};

enum class DesktopInputProfile { Auto, Generic, Xbox360, XboxOne, XboxSeries, DualShock4, DualSense, SwitchPro };
enum class DesktopInputDriver { Auto, VirtualHid, ViGEm, Disabled };

struct DesktopInputPort {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
    double environmentX = 0;
    double environmentY = 0;
    double environmentWidth = 0;
    double environmentHeight = 0;
    std::string outputName;
};

struct DesktopInputRawKey {
    // Sunshine normalized 16-bit key space. flags bit 0 means non-normalized;
    // modifiers bit 4 means extended. These are not UTF-16 characters.
    std::uint16_t code = 0;
    std::uint8_t flags = 0;
    std::uint8_t modifiers = 0;
    bool release = false;
    // Set only by the ordinary repeat scheduler, never by the model codec.
    // A repeat reuses the original binding without another owned down.
    bool repeat = false;
};

struct DesktopInputTouch {
    DesktopInputPhase phase = DesktopInputPhase::Move;
    std::uint32_t pointerId = 0;
    float x = 0;
    float y = 0;
    float pressureOrDistance = 0;
    float contactAreaMajor = 0;
    float contactAreaMinor = 0;
    std::uint16_t rotation = 0xffff;
};

struct DesktopInputPen {
    DesktopInputPhase phase = DesktopInputPhase::Move;
    std::uint8_t toolType = 1;
    std::uint8_t buttons = 0;
    std::uint8_t tilt = 0xff;
    std::uint16_t rotation = 0xffff;
    float x = 0;
    float y = 0;
    float pressureOrDistance = 0;
    float contactAreaMajor = 0;
    float contactAreaMinor = 0;
};

struct DesktopInputGamepad {
    // Controller number is local to the owning session. generation=0 binds a
    // newly attached device in this batch; a nonzero value rejects stale leases.
    std::uint8_t controllerId = 0;
    std::uint64_t generation = 0;
    DesktopInputProfile profile = DesktopInputProfile::Auto;
    std::uint8_t type = 0;
    std::uint16_t capabilities = 0;
    std::uint32_t supportedButtons = 0;
    std::uint32_t buttonFlags = 0;
    std::uint8_t leftTrigger = 0;
    std::uint8_t rightTrigger = 0;
    std::int16_t leftX = 0;
    std::int16_t leftY = 0;
    std::int16_t rightX = 0;
    std::int16_t rightY = 0;
    DesktopInputTouch touch;
    // 1 accelerometer (m/s^2), 2 gyroscope (degrees/s).
    std::uint8_t motionType = 0;
    float motionX = 0;
    float motionY = 0;
    float motionZ = 0;
    std::uint8_t batteryState = 0;
    std::uint8_t batteryPercentage = 0xff;
};

struct DesktopInputEvent {
    DesktopInputEventKind kind = DesktopInputEventKind::RelativeMove;
    // Delay before this event; batch delay total is bounded. This expresses
    // timing without prescribing a click/type business workflow.
    std::uint32_t delayMs = 0;
    double x = 0;
    double y = 0;
    std::int32_t deltaX = 0;
    std::int32_t deltaY = 0;
    // Native button numbers 1..5: left, middle, right, back, forward.
    std::uint8_t button = 1;
    bool release = false;
    bool horizontal = false;
    std::int32_t wheelDelta120 = 0;
    DesktopInputRawKey key;
    std::string text;
    // One frame may contain multiple contacts. IDs remain owner-local.
    std::vector<DesktopInputTouch> contacts;
    DesktopInputPen pen;
    DesktopInputGamepad gamepad;
};

struct DesktopInputOptions {
    // Host metadata, not a model parameter. An omitted input_options object
    // inherits this session's configuration; an explicit object resets omitted
    // fields to documented defaults before applying its supplied fields.
    bool inheritExisting = false;
    bool keyboardEnabled = true;
    bool mouseEnabled = true;
    bool controllerEnabled = true;
    bool nativePenTouchEnabled = true;
    bool alwaysSendScanCodes = true;
    bool rightAltToMeta = false;
    bool ds4BackAsTouchpad = true;
    bool motionAsPlayStation = true;
    bool touchpadAsPlayStation = true;
    bool randomizeControllerMac = true;
    DesktopInputDriver driver = DesktopInputDriver::Auto;
    DesktopInputProfile defaultProfile = DesktopInputProfile::Auto;
    bool highResolutionScroll = true;
    bool keyboardRepeat = true;
    std::uint32_t repeatDelayMs = 500;
    std::uint32_t repeatPeriodMs = 40;
    // -1 disables controller Back-to-Home emulation.
    std::int32_t backButtonTimeoutMs = -1;
    // Original and replacement 16-bit Sunshine key codes, applied before down.
    std::vector<std::array<std::uint16_t, 2>> keyBindings;
};

struct DesktopInputDevice {
    std::uint8_t controllerId = 0;
    std::uint64_t generation = 0;
    DesktopInputProfile profile = DesktopInputProfile::Auto;
};

struct DesktopInputFeature {
    std::string name;
    bool implemented = false;
    bool available = false;
    std::string backend;
    std::string delivery;
    std::string requirement;
};

struct DesktopInputCapabilities {
    std::string baseline;
    std::vector<DesktopInputFeature> features;
    std::vector<DesktopInputProfile> profiles;
    // A default/recording backend advertises no device capability. Concrete
    // native implementations set only the limits they actually provide.
    std::uint32_t maxControllers = 0;
    std::uint32_t maxTouchContacts = 0;
};

enum class DesktopInputFeedbackKind { Rumble, TriggerRumble, MotionEventState, RgbLed, PlayerLeds, AdaptiveTriggers };

struct DesktopInputFeedback {
    DesktopInputFeedbackKind kind = DesktopInputFeedbackKind::Rumble;
    std::uint8_t controllerId = 0;
    std::uint64_t generation = 0;
    std::uint64_t sequence = 0;
    std::int64_t receivedAtMs = 0;
    std::uint16_t lowFrequency = 0;
    std::uint16_t highFrequency = 0;
    std::uint16_t leftTriggerRumble = 0;
    std::uint16_t rightTriggerRumble = 0;
    std::uint8_t motionType = 0;
    std::uint16_t reportRate = 0;
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    std::uint8_t solidPlayerLeds = 0;
    std::uint8_t flashingPlayerLeds = 0;
    std::uint8_t adaptiveEventFlags = 0;
    std::uint8_t adaptiveLeftType = 0;
    std::uint8_t adaptiveRightType = 0;
    std::array<std::uint8_t, 10> adaptiveLeft{};
    std::array<std::uint8_t, 10> adaptiveRight{};
};

struct DesktopInputFeedbackBatch {
    std::vector<DesktopInputFeedback> events;
    std::uint64_t nextSequence = 0;
    std::uint64_t droppedEvents = 0;
};

// Whole-batch preflight never injects input. Native state transitions and exact
// effective profile capability are checked again by the owning device session.
MaiError maiDesktopValidateInputEvents(const std::vector<DesktopInputEvent>& events);
MaiError maiDesktopValidateInputOptions(const DesktopInputOptions& options);
bool maiDesktopInputEventsOnlyRelease(const std::vector<DesktopInputEvent>& events);
// Pure source identity/remapping shared by platform implementations. Identity
// retains the complete 16-bit source code and flags. Modifier snapshots do not
// change the identity or reinterpret an already held native binding.
std::uint32_t maiDesktopInputKeyIdentity(const DesktopInputRawKey& key);
DesktopInputRawKey maiDesktopRemapInputKey(const DesktopInputRawKey& key, const DesktopInputOptions& options);
std::string maiDesktopInputProfileName(DesktopInputProfile profile);
