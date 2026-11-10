#include "MaiChatTools/DesktopComputerPlatform.h"
#include "MaiChatTools/DesktopComputerKeys.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>
#include <IOKit/hidsystem/IOLLEvent.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <QString>

namespace {

std::string utf8(CFStringRef value) {
    if (!value || CFGetTypeID(value) != CFStringGetTypeID()) return {};
    const CFIndex length = CFStringGetLength(value);
    if (length > 4096) return {};
    std::vector<char> bytes(std::size_t(length * 4 + 1), 0);
    if (!CFStringGetCString(value, bytes.data(), CFIndex(bytes.size()),
                            kCFStringEncodingUTF8)) return {};
    return bytes.data();
}

std::string attribute(AXUIElementRef element, CFStringRef name) {
    CFTypeRef value = nullptr;
    if (AXUIElementCopyAttributeValue(element, name, &value) != kAXErrorSuccess ||
        !value) return {};
    const std::string result = utf8(static_cast<CFStringRef>(value));
    CFRelease(value);
    return result;
}

bool bounds(AXUIElementRef element, DesktopNativeElement& result) {
    CFTypeRef position = nullptr;
    CFTypeRef size = nullptr;
    const bool found = AXUIElementCopyAttributeValue(element, kAXPositionAttribute,
        &position) == kAXErrorSuccess && position &&
        AXUIElementCopyAttributeValue(element, kAXSizeAttribute, &size) == kAXErrorSuccess && size;
    CGPoint point{};
    CGSize dimensions{};
    const bool valid = found && CFGetTypeID(position) == AXValueGetTypeID() &&
        CFGetTypeID(size) == AXValueGetTypeID() &&
        AXValueGetValue(static_cast<AXValueRef>(position),
                        static_cast<AXValueType>(kAXValueCGPointType), &point) &&
        AXValueGetValue(static_cast<AXValueRef>(size),
                        static_cast<AXValueType>(kAXValueCGSizeType), &dimensions);
    if (position) CFRelease(position);
    if (size) CFRelease(size);
    if (!valid) return false;
    result.x = point.x;
    result.y = point.y;
    result.width = dimensions.width;
    result.height = dimensions.height;
    return true;
}

bool currentWindowFrame(const DesktopNativeWindow& window, CGRect& frame) {
    if (!window.id || window.id > (std::numeric_limits<CGWindowID>::max)()) return false;
    CFArrayRef details = CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow,
                                                   CGWindowID(window.id));
    if (!details) return false;
    bool found = false;
    for (CFIndex index = 0; index < CFArrayGetCount(details); ++index) {
        const auto entry = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(details, index));
        if (!entry || CFGetTypeID(entry) != CFDictionaryGetTypeID()) continue;
        int pid = 0;
        std::uint32_t id = 0;
        const auto pidValue = static_cast<CFNumberRef>(
            CFDictionaryGetValue(entry, kCGWindowOwnerPID));
        const auto idValue = static_cast<CFNumberRef>(
            CFDictionaryGetValue(entry, kCGWindowNumber));
        const auto frameValue = static_cast<CFDictionaryRef>(
            CFDictionaryGetValue(entry, kCGWindowBounds));
        if (pidValue && idValue && frameValue &&
            CFNumberGetValue(pidValue, kCFNumberIntType, &pid) &&
            CFNumberGetValue(idValue, kCFNumberSInt32Type, &id) &&
            id == window.id && std::uint64_t(pid) == window.processId &&
            CGRectMakeWithDictionaryRepresentation(frameValue, &frame)) {
            found = true;
            break;
        }
    }
    CFRelease(details);
    return found;
}

AXUIElementRef accessibilityWindow(const DesktopNativeWindow& window) {
    CGRect frame{};
    if (!currentWindowFrame(window, frame)) return nullptr;
    AXUIElementRef application = AXUIElementCreateApplication(pid_t(window.processId));
    if (!application) return nullptr;
    CFTypeRef value = nullptr;
    AXUIElementRef found = nullptr;
    if (AXUIElementCopyAttributeValue(application, kAXWindowsAttribute, &value) ==
            kAXErrorSuccess && value && CFGetTypeID(value) == CFArrayGetTypeID()) {
        const CFArrayRef windows = static_cast<CFArrayRef>(value);
        for (CFIndex index = 0; index < CFArrayGetCount(windows); ++index) {
            AXUIElementRef candidate = static_cast<AXUIElementRef>(
                const_cast<void*>(CFArrayGetValueAtIndex(windows, index)));
            DesktopNativeElement candidateFrame;
            if (CFGetTypeID(candidate) == AXUIElementGetTypeID() &&
                bounds(candidate, candidateFrame) &&
                std::abs(candidateFrame.x - frame.origin.x) < 3 &&
                std::abs(candidateFrame.y - frame.origin.y) < 3 &&
                std::abs(candidateFrame.width - frame.size.width) < 3 &&
                std::abs(candidateFrame.height - frame.size.height) < 3) {
                // Public AX APIs expose no CGWindowID. Resolve that ID to its
                // current PID/frame and require one unique AX geometry match;
                // identical titles never select an arbitrary sibling window.
                if (found) {
                    CFRelease(found);
                    found = nullptr;
                    break;
                }
                found = candidate;
                CFRetain(found);
            }
        }
    }
    if (value) CFRelease(value);
    CFRelease(application);
    return found;
}

MaiError accessPermission() {
    if (!AXIsProcessTrusted())
        return {MaiErrorCode::NotConfigured,
                "grant MaiChat Accessibility permission in macOS Settings before computer actions"};
    return {};
}

bool interactiveDesktopIsAvailable() {
    @autoreleasepool {
        CFDictionaryRef session = CGSessionCopyCurrentDictionary();
        if (!session) return false;
        const bool available = CFDictionaryGetValue(session, kCGSessionOnConsoleKey) == kCFBooleanTrue &&
                               CFDictionaryGetValue(session, kCGSessionLoginDoneKey) == kCFBooleanTrue;
        CFRelease(session);
        NSRunningApplication* frontmost = NSWorkspace.sharedWorkspace.frontmostApplication;
        // Match the capture service's public rejection signals. macOS has no
        // complete public lock-state API; an absent/locked GUI is never an
        // opportunity to post input to a different login session.
        return available && frontmost && ![frontmost.bundleIdentifier isEqualToString:@"com.apple.loginwindow"];
    }
}

bool validDisplayBounds(CGRect frame) {
    return std::isfinite(frame.origin.x) && std::isfinite(frame.origin.y) && std::isfinite(frame.size.width) &&
           std::isfinite(frame.size.height) && frame.size.width > 0 && frame.size.height > 0;
}

MaiError validateDisplay(const DesktopNativeDisplay& display) {
    if (!display.id || display.id > (std::numeric_limits<CGDirectDisplayID>::max)() || !std::isfinite(display.x) ||
        !std::isfinite(display.y) || !std::isfinite(display.width) || !std::isfinite(display.height) ||
        display.width <= 0 || display.height <= 0)
        return {MaiErrorCode::InvalidInput, "a valid observed display is required"};
    const CGDirectDisplayID id = CGDirectDisplayID(display.id);
    if (!CGDisplayIsActive(id)) return {MaiErrorCode::NotFound, "the observed display is no longer active"};
    const CGRect frame = CGDisplayBounds(id);
    if (!validDisplayBounds(frame) || frame.origin.x != display.x || frame.origin.y != display.y ||
        frame.size.width != display.width || frame.size.height != display.height)
        return {MaiErrorCode::Busy, "display geometry changed; observe the display again"};
    return {};
}

bool displayContainsPoint(const DesktopNativeDisplay& display, CGPoint point) {
    return point.x >= display.x && point.y >= display.y && point.x < display.x + display.width &&
           point.y < display.y + display.height;
}

struct ReleaseCFValue {
    void operator()(CFTypeRef value) const { CFRelease(value); }
};

using ScopedCFValue = std::unique_ptr<const void, ReleaseCFValue>;

ScopedCFValue copiedAttribute(AXUIElementRef element, CFStringRef name) {
    CFTypeRef value = nullptr;
    const AXError result = AXUIElementCopyAttributeValue(element, name, &value);
    ScopedCFValue owned(value);
    if (result != kAXErrorSuccess) return {};
    return owned;
}

bool sameProcessElement(CFTypeRef value, pid_t process) {
    if (!value || CFGetTypeID(value) != AXUIElementGetTypeID()) return false;
    pid_t actual = 0;
    return AXUIElementGetPid(static_cast<AXUIElementRef>(value), &actual) ==
        kAXErrorSuccess && actual == process;
}

// Only public AX ownership proves that a focused sheet/panel belongs to this
// target. Same-process sibling windows are separate targets. Both searches are
// bounded, and every copied/retained CF reference is released on early return.
bool acceptsInputFromWindow(AXUIElementRef selected, AXUIElementRef focused,
                            pid_t process) {
    if (!sameProcessElement(selected, process) || !sameProcessElement(focused, process))
        return false;
    if (CFEqual(selected, focused)) return true;

    std::vector<ScopedCFValue> ancestors;
    ancestors.emplace_back(CFRetain(focused));
    for (int depth = 0; depth < 16; ++depth) {
        ScopedCFValue parent = copiedAttribute(
            static_cast<AXUIElementRef>(ancestors.back().get()), kAXParentAttribute);
        if (!sameProcessElement(parent.get(), process)) break;
        if (CFEqual(parent.get(), selected)) return true;
        const bool seen = std::any_of(ancestors.begin(), ancestors.end(),
            [&parent](const ScopedCFValue& ancestor) {
                return CFEqual(ancestor.get(), parent.get());
            });
        if (seen) break;
        ancestors.push_back(std::move(parent));
    }

    struct SheetNode {
        ScopedCFValue value;
        int depth = 0;
    };
    std::vector<SheetNode> pending;
    pending.push_back({ScopedCFValue(CFRetain(selected)), 0});
    std::size_t inspected = 1;
    for (std::size_t index = 0; index < pending.size(); ++index) {
        const int depth = pending[index].depth;
        if (depth >= 4) continue;
        // The public AX API exposes sheets through the children hierarchy,
        // without a kAXSheetsAttribute constant. Follow only sheet children;
        // an unrelated same-process window must not become an input target.
        ScopedCFValue children = copiedAttribute(
            static_cast<AXUIElementRef>(pending[index].value.get()), kAXChildrenAttribute);
        if (!children || CFGetTypeID(children.get()) != CFArrayGetTypeID()) continue;
        const auto array = static_cast<CFArrayRef>(children.get());
        for (CFIndex child = 0; child < CFArrayGetCount(array); ++child) {
            // Count invalid and duplicate provider entries too: traversal work
            // stays bounded even when an AX array contains no useful children.
            if (inspected++ >= 64) return false;
            const CFTypeRef value = CFArrayGetValueAtIndex(array, child);
            if (!sameProcessElement(value, process)) continue;
            ScopedCFValue role = copiedAttribute(
                static_cast<AXUIElementRef>(value), kAXRoleAttribute);
            if (!role || CFGetTypeID(role.get()) != CFStringGetTypeID() ||
                !CFEqual(role.get(), kAXSheetRole)) continue;
            const bool seen = std::any_of(pending.begin(), pending.end(),
                [value](const SheetNode& node) { return CFEqual(node.value.get(), value); });
            if (seen) continue;
            if (CFEqual(value, focused)) return true;
            pending.push_back({ScopedCFValue(CFRetain(value)), depth + 1});
        }
    }
    return false;
}

class InputScope {
public:
    explicit InputScope(const DesktopNativeInput& input) : input_(input) {
        // A remote-control source owns only its synthetic state table. Cleanup
        // must never manufacture key-up events for the user's hardware state.
        source_ = CGEventSourceCreate(kCGEventSourceStatePrivate);
        if (input.desktopScope) return;
        if (input.expectedWindowId && input.expectedProcessId) {
            DesktopNativeWindow selected;
            selected.id = input.expectedWindowId;
            selected.processId = input.expectedProcessId;
            window_ = accessibilityWindow(selected);
            process_ = pid_t(input.expectedProcessId);
            application_ = AXUIElementCreateApplication(process_);
        } else {
            AXUIElementRef system = AXUIElementCreateSystemWide();
            CFTypeRef application = nullptr;
            if (system && AXUIElementCopyAttributeValue(system,
                    kAXFocusedApplicationAttribute, &application) == kAXErrorSuccess &&
                application && CFGetTypeID(application) == AXUIElementGetTypeID()) {
                application_ = static_cast<AXUIElementRef>(application);
                AXUIElementGetPid(application_, &process_);
                CFTypeRef focused = nullptr;
                if (AXUIElementCopyAttributeValue(application_, kAXFocusedWindowAttribute,
                        &focused) == kAXErrorSuccess && focused &&
                    CFGetTypeID(focused) == AXUIElementGetTypeID())
                    window_ = static_cast<AXUIElementRef>(focused);
                else if (focused) CFRelease(focused);
            } else if (application) CFRelease(application);
            if (system) CFRelease(system);
        }
    }
    ~InputScope() {
        // CGEventPost has no delivery acknowledgement. Cleanup pairs our downs
        // with ups after cancellation/focus changes, preserving hardware holds.
        while (!releases_.empty()) release();
        if (window_) CFRelease(window_);
        if (application_) CFRelease(application_);
        if (source_) CFRelease(source_);
    }
    MaiError validate() const {
        if (input_.isCanceled && input_.isCanceled())
            return {MaiErrorCode::Canceled, "computer input canceled"};
        const MaiError permission = accessPermission();
        if (permission) return permission;
        if (!source_) return {MaiErrorCode::Internal, "could not create a private computer input source"};
        if (input_.desktopScope) {
            if (!interactiveDesktopIsAvailable())
                return {MaiErrorCode::NotConfigured,
                        "an unlocked interactive macOS desktop is required for computer input"};
            return validateDisplay(input_.display);
        }
        if (!window_ || !application_)
            return {MaiErrorCode::NotFound, "selected window is unavailable to macOS Accessibility"};
        bool front = false;
        @autoreleasepool {
            NSRunningApplication* app = [[NSWorkspace sharedWorkspace] frontmostApplication];
            front = app && app.processIdentifier == process_;
        }
        if (!front)
            return {MaiErrorCode::Busy, "selected application lost foreground focus during computer input"};
        const ScopedCFValue focused = copiedAttribute(application_, kAXFocusedWindowAttribute);
        if (!focused || !acceptsInputFromWindow(window_,
                static_cast<AXUIElementRef>(focused.get()), process_))
            return {MaiErrorCode::Busy,
                    "focused macOS window ownership is unproven; explicitly observe or focus that window"};
        return {};
    }
    MaiError hold(CGEventRef down, CGEventRef up, CGEventFlags modifier = 0,
                  CGEventFlags extra = 0) {
        const MaiError ready = validate();
        if (ready) return ready;
        const CGEventType type = CGEventGetType(down);
        const bool keyboard = type == kCGEventKeyDown || type == kCGEventFlagsChanged;
        const int code = keyboard ? int(CGEventGetIntegerValueField(down, kCGKeyboardEventKeycode))
                                  : int(CGEventGetIntegerValueField(down, kCGMouseEventButtonNumber));
        if ((keyboard && CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, CGKeyCode(code))) ||
            (!keyboard && CGEventSourceButtonState(kCGEventSourceStateHIDSystemState, CGMouseButton(code))))
            return {MaiErrorCode::Busy, "the requested input is already held by the user"};
        CGEventSetSource(down, source_);
        CGEventSetFlags(down, effectiveFlags() | modifier | extra);
        CGEventPost(kCGHIDEventTap, down);
        CFRetain(up);
        releases_.push_back({up, modifier, extra, keyboard, code});
        return {};
    }
    void release() {
        const Pending pending = releases_.back();
        releases_.pop_back();
        // A physical key/button may have gone down after our synthetic down.
        // Leave that hardware input held; releasing the private source still
        // discards our private state when the action ends.
        const bool userHeld =
            pending.keyboard ? CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, CGKeyCode(pending.code))
                             : CGEventSourceButtonState(kCGEventSourceStateHIDSystemState, CGMouseButton(pending.code));
        if (!userHeld) {
            CGEventSetSource(pending.event, source_);
            CGEventSetFlags(pending.event, effectiveFlags() | pending.extra);
            CGEventPost(kCGHIDEventTap, pending.event);
        }
        CFRelease(pending.event);
    }
    MaiError tap(CGEventRef down, CGEventRef up, CGEventFlags extra = 0) {
        const MaiError sent = hold(down, up, 0, extra);
        if (sent) return sent;
        release();
        return validate();
    }
    MaiError key(CGKeyCode code, CGEventFlags extra = 0, bool holdModifier = false) {
        CGEventRef down = CGEventCreateKeyboardEvent(source_, code, true);
        CGEventRef up = CGEventCreateKeyboardEvent(source_, code, false);
        if (!down || !up) {
            if (down) CFRelease(down);
            if (up) CFRelease(up);
            return {MaiErrorCode::Internal, "could not create keyboard events"};
        }
        if (holdModifier) {
            CGEventSetType(down, kCGEventFlagsChanged);
            CGEventSetType(up, kCGEventFlagsChanged);
        }
        const MaiError sent = holdModifier ? hold(down, up, extra) : tap(down, up, extra);
        CFRelease(down);
        CFRelease(up);
        return sent;
    }
    MaiError move(CGPoint point, bool dragging = true) {
        const MaiError ready = validate();
        if (ready) return ready;
        if (input_.desktopScope && !displayContainsPoint(input_.display, point))
            return {MaiErrorCode::InvalidInput, "pointer coordinates are outside the observed display"};
        CGEventRef event = CGEventCreateMouseEvent(source_, dragging ? kCGEventLeftMouseDragged : kCGEventMouseMoved,
                                                   point, kCGMouseButtonLeft);
        if (!event) return {MaiErrorCode::Internal, "could not create pointer motion event"};
        CGEventSetFlags(event, effectiveFlags());
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
        if (dragging && !releases_.empty()) CGEventSetLocation(releases_.back().event, point);
        return validate();
    }
    void post(CGEventRef event) const {
        CGEventSetSource(event, source_);
        CGEventSetFlags(event, effectiveFlags());
        CGEventPost(kCGHIDEventTap, event);
    }

private:
    CGEventFlags effectiveFlags() const {
        // Both explicitly requested sides may belong to one modifier family.
        // Recompute from the still-owned downs so releasing RightShift cannot
        // clear Shift while our LeftShift remains down (or vice versa).
        CGEventFlags flags = CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState);
        for (const auto& pending : releases_) flags |= pending.modifier;
        return flags;
    }
    struct Pending {
        CGEventRef event;
        CGEventFlags modifier;
        CGEventFlags extra;
        bool keyboard;
        int code;
    };
    const DesktopNativeInput& input_;
    CGEventSourceRef source_ = nullptr;
    AXUIElementRef application_ = nullptr;
    AXUIElementRef window_ = nullptr;
    pid_t process_ = 0;
    std::vector<Pending> releases_;
};

struct NativeModifierFamily {
    DesktopKeyModifier generic;
    DesktopKeyModifier left;
    DesktopKeyModifier right;
    CGKeyCode leftCode;
    CGKeyCode rightCode;
    CGEventFlags familyFlag;
    CGEventFlags leftFlag;
    CGEventFlags rightFlag;
};

const NativeModifierFamily modifierFamilies[] = {
    {DesktopKeyModifier::Control, DesktopKeyModifier::ControlLeft, DesktopKeyModifier::ControlRight, kVK_Control,
     kVK_RightControl, kCGEventFlagMaskControl, NX_DEVICELCTLKEYMASK, NX_DEVICERCTLKEYMASK},
    {DesktopKeyModifier::Shift, DesktopKeyModifier::ShiftLeft, DesktopKeyModifier::ShiftRight, kVK_Shift,
     kVK_RightShift, kCGEventFlagMaskShift, NX_DEVICELSHIFTKEYMASK, NX_DEVICERSHIFTKEYMASK},
    {DesktopKeyModifier::Alt, DesktopKeyModifier::AltLeft, DesktopKeyModifier::AltRight, kVK_Option, kVK_RightOption,
     kCGEventFlagMaskAlternate, NX_DEVICELALTKEYMASK, NX_DEVICERALTKEYMASK},
    {DesktopKeyModifier::Meta, DesktopKeyModifier::MetaLeft, DesktopKeyModifier::MetaRight, kVK_Command,
     kVK_RightCommand, kCGEventFlagMaskCommand, NX_DEVICELCMDKEYMASK, NX_DEVICERCMDKEYMASK}};

const NativeModifierFamily* nativeModifierFamily(DesktopKeyModifier modifier) {
    const DesktopKeyModifier family = maiDesktopModifierFamily(modifier);
    for (const auto& item : modifierFamilies) {
        if (item.generic == family) return &item;
    }
    return nullptr;
}

struct NativeKey {
    CGKeyCode code = 0;
    CGEventFlags extra = 0;
    CGEventFlags modifier = 0;
    std::vector<DesktopKeyModifier> implicitModifiers;
};

MaiResult<NativeKey> nativeKey(const DesktopKeyChord& key) {
    if (key.key == DesktopKeyCode::Character) {
        // Resolve logical characters through the active layout, including the
        // Command layer used by Dvorak-QWERTY shortcuts. Explicit modifiers keep
        // their shortcut meaning (Shift+1 is not a request to type a literal 1).
        // Only add Shift/Option when the character itself needs them. Never use
        // a keypad substitute or a dead-key sequence as a printable key match.
        static const CGKeyCode positions[] = {
            kVK_ANSI_A,         kVK_ANSI_S,     kVK_ANSI_D,           kVK_ANSI_F,     kVK_ANSI_H,
            kVK_ANSI_G,         kVK_ANSI_Z,     kVK_ANSI_X,           kVK_ANSI_C,     kVK_ANSI_V,
            kVK_ANSI_B,         kVK_ANSI_Q,     kVK_ANSI_W,           kVK_ANSI_E,     kVK_ANSI_R,
            kVK_ANSI_Y,         kVK_ANSI_T,     kVK_ANSI_1,           kVK_ANSI_2,     kVK_ANSI_3,
            kVK_ANSI_4,         kVK_ANSI_6,     kVK_ANSI_5,           kVK_ANSI_Equal, kVK_ANSI_9,
            kVK_ANSI_7,         kVK_ANSI_Minus, kVK_ANSI_8,           kVK_ANSI_0,     kVK_ANSI_RightBracket,
            kVK_ANSI_O,         kVK_ANSI_U,     kVK_ANSI_LeftBracket, kVK_ANSI_I,     kVK_ANSI_P,
            kVK_ANSI_L,         kVK_ANSI_J,     kVK_ANSI_Quote,       kVK_ANSI_K,     kVK_ANSI_Semicolon,
            kVK_ANSI_Backslash, kVK_ANSI_Comma, kVK_ANSI_Slash,       kVK_ANSI_N,     kVK_ANSI_M,
            kVK_ANSI_Period,    kVK_ANSI_Grave, kVK_ISO_Section,      kVK_JIS_Yen,    kVK_JIS_Underscore};
        bool command = false;
        for (const auto modifier : key.modifiers) {
            command = command || maiDesktopModifierFamily(modifier) == DesktopKeyModifier::Meta;
        }
        __block int matched = -1;
        __block UInt32 required = 0;
        const auto resolve = ^{
          TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource();
          if (!source) return;
          const auto data = static_cast<CFDataRef>(TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData));
          if (data && CFGetTypeID(data) == CFDataGetTypeID()) {
              const auto* layout = reinterpret_cast<const UCKeyboardLayout*>(CFDataGetBytePtr(data));
              const UInt32 combinations[] = {0, shiftKey, optionKey, shiftKey | optionKey};
              for (const UInt32 combination : combinations) {
                  for (const CGKeyCode code : positions) {
                      UInt32 dead = 0;
                      UniCharCount count = 0;
                      UniChar units[4]{};
                      const UInt32 modifiers = (combination | (command ? cmdKey : 0)) >> 8;
                      if (UCKeyTranslate(layout, code, kUCKeyActionDown, modifiers, UInt32(LMGetKbdType()), 0, &dead, 4,
                                         &count, units) != noErr ||
                          dead != 0 || count != 1)
                          continue;
                      UniChar character = units[0];
                      if (character >= 'a' && character <= 'z') character -= 'a' - 'A';
                      if (character == static_cast<unsigned char>(key.character)) {
                          matched = code;
                          required = combination;
                          break;
                      }
                  }
                  if (matched >= 0) break;
              }
              if (matched < 0 && command) {
                  // Some Command tables suppress Shift in the translated
                  // text even though the shortcut still has a Shift flag.
                  // Resolve its glyph separately only when this exact key's
                  // unshifted identity is unchanged by Command. A layout
                  // that remaps the key must not borrow an ANSI substitute.
                  const auto glyph = [layout](CGKeyCode code, UInt32 modifiers, UniChar& value) {
                      UInt32 dead = 0;
                      UniCharCount count = 0;
                      UniChar units[4]{};
                      const auto status = UCKeyTranslate(layout, code, kUCKeyActionDown, modifiers >> 8,
                                                         UInt32(LMGetKbdType()), 0, &dead, 4, &count, units);
                      if (status != noErr || dead || count != 1) return false;
                      value = units[0];
                      return true;
                  };
                  for (const UInt32 combination : combinations) {
                      if (!(combination & shiftKey)) continue;
                      for (const CGKeyCode code : positions) {
                          UniChar character = 0, base = 0, commandBase = 0, commandShifted = 0;
                          if (!glyph(code, combination, character)) continue;
                          if (character >= 'a' && character <= 'z') character -= 'a' - 'A';
                          if (character != static_cast<unsigned char>(key.character)) continue;
                          const UInt32 unshifted = combination & ~UInt32(shiftKey);
                          if (!glyph(code, unshifted, base) || !glyph(code, unshifted | cmdKey, commandBase) ||
                              !glyph(code, combination | cmdKey, commandShifted) || base != commandBase ||
                              commandShifted != commandBase)
                              continue;
                          matched = code;
                          required = combination;
                          break;
                      }
                      if (matched >= 0) break;
                  }
              }
          }
          CFRelease(source);
        };
        if ([NSThread isMainThread])
            resolve();
        else
            dispatch_sync(dispatch_get_main_queue(), resolve);
        if (matched >= 0) {
            NativeKey result{CGKeyCode(matched), 0};
            if (required & shiftKey) result.implicitModifiers.push_back(DesktopKeyModifier::Shift);
            if (required & optionKey) result.implicitModifiers.push_back(DesktopKeyModifier::Alt);
            return result;
        }
        return {MaiErrorCode::NotSupported,
                "the current macOS keyboard layout cannot map this character to a non-dead physical key"};
    }
    DesktopKeyModifier modifier;
    if (maiDesktopKeyModifier(key.key, modifier)) {
        const auto* family = nativeModifierFamily(modifier);
        if (!family) return {MaiErrorCode::InvalidInput, "unknown native modifier family"};
        const bool right = modifier == family->right;
        return NativeKey{right ? family->rightCode : family->leftCode, 0,
                         family->familyFlag | (right ? family->rightFlag : family->leftFlag)};
    }
    switch (key.key) {
    case DesktopKeyCode::Enter: return NativeKey{kVK_Return, 0};
    case DesktopKeyCode::Tab: return NativeKey{kVK_Tab, 0};
    case DesktopKeyCode::Escape: return NativeKey{kVK_Escape, 0};
    case DesktopKeyCode::Backspace: return NativeKey{kVK_Delete, 0};
    case DesktopKeyCode::Delete: return NativeKey{kVK_ForwardDelete, kCGEventFlagMaskSecondaryFn};
    case DesktopKeyCode::ArrowLeft: return NativeKey{kVK_LeftArrow, kCGEventFlagMaskSecondaryFn};
    case DesktopKeyCode::ArrowRight: return NativeKey{kVK_RightArrow, kCGEventFlagMaskSecondaryFn};
    case DesktopKeyCode::ArrowUp: return NativeKey{kVK_UpArrow, kCGEventFlagMaskSecondaryFn};
    case DesktopKeyCode::ArrowDown: return NativeKey{kVK_DownArrow, kCGEventFlagMaskSecondaryFn};
    case DesktopKeyCode::Home: return NativeKey{kVK_Home, kCGEventFlagMaskSecondaryFn};
    case DesktopKeyCode::End: return NativeKey{kVK_End, kCGEventFlagMaskSecondaryFn};
    case DesktopKeyCode::PageUp: return NativeKey{kVK_PageUp, kCGEventFlagMaskSecondaryFn};
    case DesktopKeyCode::PageDown: return NativeKey{kVK_PageDown, kCGEventFlagMaskSecondaryFn};
    case DesktopKeyCode::Space: return NativeKey{kVK_Space, 0};
    // Named keys identify physical ANSI/ISO/JIS positions. They do not promise
    // the glyph printed by a different active keyboard layout.
    case DesktopKeyCode::Backquote: return NativeKey{kVK_ANSI_Grave, 0};
    case DesktopKeyCode::Minus: return NativeKey{kVK_ANSI_Minus, 0};
    case DesktopKeyCode::Equal: return NativeKey{kVK_ANSI_Equal, 0};
    case DesktopKeyCode::BracketLeft: return NativeKey{kVK_ANSI_LeftBracket, 0};
    case DesktopKeyCode::BracketRight: return NativeKey{kVK_ANSI_RightBracket, 0};
    case DesktopKeyCode::Backslash: return NativeKey{kVK_ANSI_Backslash, 0};
    case DesktopKeyCode::IntlBackslash: return NativeKey{kVK_ISO_Section, 0};
    case DesktopKeyCode::Semicolon: return NativeKey{kVK_ANSI_Semicolon, 0};
    case DesktopKeyCode::Quote: return NativeKey{kVK_ANSI_Quote, 0};
    case DesktopKeyCode::Comma: return NativeKey{kVK_ANSI_Comma, 0};
    case DesktopKeyCode::Period: return NativeKey{kVK_ANSI_Period, 0};
    case DesktopKeyCode::Slash: return NativeKey{kVK_ANSI_Slash, 0};
    case DesktopKeyCode::Numpad0: return NativeKey{kVK_ANSI_Keypad0, kCGEventFlagMaskNumericPad};
    case DesktopKeyCode::Numpad1: return NativeKey{kVK_ANSI_Keypad1, kCGEventFlagMaskNumericPad};
    case DesktopKeyCode::Numpad2: return NativeKey{kVK_ANSI_Keypad2, kCGEventFlagMaskNumericPad};
    case DesktopKeyCode::Numpad3: return NativeKey{kVK_ANSI_Keypad3, kCGEventFlagMaskNumericPad};
    case DesktopKeyCode::Numpad4: return NativeKey{kVK_ANSI_Keypad4, kCGEventFlagMaskNumericPad};
    case DesktopKeyCode::Numpad5: return NativeKey{kVK_ANSI_Keypad5, kCGEventFlagMaskNumericPad};
    case DesktopKeyCode::Numpad6: return NativeKey{kVK_ANSI_Keypad6, kCGEventFlagMaskNumericPad};
    case DesktopKeyCode::Numpad7: return NativeKey{kVK_ANSI_Keypad7, kCGEventFlagMaskNumericPad};
    case DesktopKeyCode::Numpad8: return NativeKey{kVK_ANSI_Keypad8, kCGEventFlagMaskNumericPad};
    case DesktopKeyCode::Numpad9: return NativeKey{kVK_ANSI_Keypad9, kCGEventFlagMaskNumericPad};
    case DesktopKeyCode::NumpadAdd: return NativeKey{kVK_ANSI_KeypadPlus, kCGEventFlagMaskNumericPad};
    case DesktopKeyCode::NumpadSubtract: return NativeKey{kVK_ANSI_KeypadMinus, kCGEventFlagMaskNumericPad};
    case DesktopKeyCode::NumpadMultiply: return NativeKey{kVK_ANSI_KeypadMultiply, kCGEventFlagMaskNumericPad};
    case DesktopKeyCode::NumpadDivide: return NativeKey{kVK_ANSI_KeypadDivide, kCGEventFlagMaskNumericPad};
    case DesktopKeyCode::NumpadDecimal: return NativeKey{kVK_ANSI_KeypadDecimal, kCGEventFlagMaskNumericPad};
    case DesktopKeyCode::NumpadEnter: return NativeKey{kVK_ANSI_KeypadEnter, kCGEventFlagMaskNumericPad};
    case DesktopKeyCode::NumpadEqual: return NativeKey{kVK_ANSI_KeypadEquals, kCGEventFlagMaskNumericPad};
    case DesktopKeyCode::NumpadComma: return NativeKey{kVK_JIS_KeypadComma, kCGEventFlagMaskNumericPad};
    case DesktopKeyCode::Clear: return NativeKey{kVK_ANSI_KeypadClear, kCGEventFlagMaskNumericPad};
    case DesktopKeyCode::Help: return NativeKey{kVK_Help, kCGEventFlagMaskHelp};
    case DesktopKeyCode::ContextMenu: return NativeKey{kVK_ContextualMenu, 0};
    case DesktopKeyCode::KanaMode: return NativeKey{kVK_JIS_Kana, 0};
    case DesktopKeyCode::Eisu: return NativeKey{kVK_JIS_Eisu, 0};
    case DesktopKeyCode::IntlRo: return NativeKey{kVK_JIS_Underscore, 0};
    case DesktopKeyCode::IntlYen: return NativeKey{kVK_JIS_Yen, 0};
    case DesktopKeyCode::Fn: return NativeKey{kVK_Function, 0, kCGEventFlagMaskSecondaryFn};
    case DesktopKeyCode::Function: {
        const CGKeyCode functions[] = {kVK_F1, kVK_F2, kVK_F3, kVK_F4, kVK_F5,
            kVK_F6, kVK_F7, kVK_F8, kVK_F9, kVK_F10, kVK_F11, kVK_F12,
            kVK_F13, kVK_F14, kVK_F15, kVK_F16, kVK_F17, kVK_F18, kVK_F19, kVK_F20};
        if (key.function >= 1 && key.function <= 20)
            return NativeKey{functions[key.function - 1], kCGEventFlagMaskSecondaryFn};
        // AppKit's F21+ Unicode constants are not physical Quartz keycodes.
        // Posting Unicode as keycode zero can become an unrelated shortcut.
        return {MaiErrorCode::NotSupported, "macOS native key dispatch supports F1 through F20"};
    }
    case DesktopKeyCode::CapsLock:
        // A normal key down/up pair does not prove a persistent CapsLock
        // toggle. Do not report a momentary modifier as a successful lock.
        return {MaiErrorCode::NotSupported, "macOS CapsLock toggle is not supported by this input backend"};
    default: break;
    }
    // Insert is not Help, Clear is not NumLock, and NX media identifiers are
    // not CGKeyCodes. System/media/browser keys without a verified ordinary
    // event path fail explicitly instead of changing another key or claiming
    // that the OS consumed an event it might ignore.
    return {MaiErrorCode::NotSupported, "this key has no supported native macOS mapping"};
}

std::vector<DesktopKeyModifier> chordModifiers(const DesktopKeyChord& chord, const NativeKey& key) {
    std::vector<DesktopKeyModifier> modifiers = chord.modifiers;
    for (const auto implicit : key.implicitModifiers) {
        const bool hasFamily = std::any_of(modifiers.begin(), modifiers.end(), [implicit](DesktopKeyModifier value) {
            return maiDesktopModifierFamily(value) == implicit;
        });
        // An explicit right/left side must not be widened to either side by a
        // generic modifier inferred from character translation.
        if (!hasFamily) modifiers.push_back(implicit);
    }
    return modifiers;
}

bool hasUnexpectedModifiers(const DesktopKeyChord* chord, const NativeKey* key = nullptr);

MaiError sendChord(InputScope& scope, const DesktopKeyChord& chord) {
    const auto key = nativeKey(chord);
    if (!key) return key.error();
    if (hasUnexpectedModifiers(&chord, &key.value()))
        return {MaiErrorCode::Busy, "release modifiers that are not part of the requested action"};
    int held = 0;
    for (const auto modifier : chordModifiers(chord, key.value())) {
        const auto* family = nativeModifierFamily(modifier);
        if (!family) return {MaiErrorCode::InvalidInput, "unknown native modifier family"};
        const bool right = modifier == family->right;
        const CGKeyCode code = right ? family->rightCode : family->leftCode;
        const bool heldByUser = modifier == family->generic
                                    ? CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, family->leftCode) ||
                                          CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, family->rightCode)
                                    : CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, code);
        if (heldByUser) continue;
        const CGEventFlags flags = family->familyFlag | (right ? family->rightFlag : family->leftFlag);
        const MaiError sent = scope.key(code, flags, true);
        if (sent) return sent;
        ++held;
    }
    if (CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, key.value().code))
        return {MaiErrorCode::Busy, "the requested key is already held by the user"};
    const NativeKey mapped = key.value();
    const MaiError sent =
        mapped.modifier ? scope.key(mapped.code, mapped.modifier, true) : scope.key(mapped.code, mapped.extra);
    if (sent) return sent;
    if (mapped.modifier) ++held;
    while (held-- > 0) scope.release();
    return scope.validate();
}

MaiError sendText(InputScope& scope, const std::string& text) {
    const QString decoded = QString::fromUtf8(text.c_str(), int(text.size()));
    // Keep surrogate pairs together and bound each Unicode event. Long input
    // remains cancellable and never uses or overwrites the system clipboard.
    for (qsizetype index = 0; index < decoded.size();) {
        qsizetype count = (std::min)(qsizetype(20), decoded.size() - index);
        if (index + count < decoded.size() &&
            decoded.at(int(index + count - 1)).isHighSurrogate())
            --count;
        CGEventRef down = CGEventCreateKeyboardEvent(nullptr, 0, true);
        CGEventRef up = CGEventCreateKeyboardEvent(nullptr, 0, false);
        if (!down || !up) {
            if (down) CFRelease(down);
            if (up) CFRelease(up);
            return {MaiErrorCode::Internal, "could not create Unicode text events"};
        }
        const auto* units = reinterpret_cast<const UniChar*>(decoded.utf16() + index);
        CGEventKeyboardSetUnicodeString(down, UniCharCount(count), units);
        CGEventKeyboardSetUnicodeString(up, UniCharCount(count), units);
        const MaiError sent = scope.tap(down, up);
        CFRelease(down);
        CFRelease(up);
        if (sent) return sent;
        index += count;
    }
    return scope.validate();
}

bool validPoint(double x, double y) {
    constexpr double limit = double((std::numeric_limits<int>::max)()) - 1;
    return std::isfinite(x) && std::isfinite(y) && std::abs(x) <= limit && std::abs(y) <= limit;
}

bool hasUnexpectedModifiers(const DesktopKeyChord* chord, const NativeKey* key) {
    const CGEventFlags flags = CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState);
    std::vector<DesktopKeyModifier> requested;
    if (chord) {
        requested = key ? chordModifiers(*chord, *key) : chord->modifiers;
        DesktopKeyModifier terminal;
        if (maiDesktopKeyModifier(chord->key, terminal)) requested.push_back(terminal);
    }
    const auto allows = [&requested](DesktopKeyModifier modifier) {
        return std::find(requested.begin(), requested.end(), modifier) != requested.end();
    };
    for (const auto& family : modifierFamilies) {
        const bool generic = allows(family.generic);
        const bool left =
            (flags & family.leftFlag) || CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, family.leftCode);
        const bool right =
            (flags & family.rightFlag) || CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, family.rightCode);
        if ((left && !generic && !allows(family.left)) || (right && !generic && !allows(family.right))) return true;
        // A generic family flag without a proven side cannot satisfy a
        // side-specific request. Keep that uncertainty fail-closed.
        if ((flags & family.familyFlag) && !left && !right && !generic) return true;
    }
    if ((flags & kCGEventFlagMaskSecondaryFn) && (!chord || chord->key != DesktopKeyCode::Fn)) return true;
    return false;
}

} // namespace

MaiResult<std::vector<DesktopNativeDisplay>> maiDesktopDisplays() {
    std::uint32_t count = 0;
    if (CGGetActiveDisplayList(0, nullptr, &count) != kCGErrorSuccess || !count)
        return {MaiErrorCode::NotFound, "no active display is available"};
    // Keep the enumeration bounded even if a virtual-display provider misbehaves.
    if (count > 64) return {MaiErrorCode::NotSupported, "the active display count exceeds the supported limit"};
    std::vector<CGDirectDisplayID> displays(count);
    if (CGGetActiveDisplayList(count, displays.data(), &count) != kCGErrorSuccess)
        return {MaiErrorCode::Internal, "could not enumerate active macOS displays"};
    const CGDirectDisplayID primary = CGMainDisplayID();
    std::vector<DesktopNativeDisplay> output;
    for (std::uint32_t index = 0; index < count; ++index) {
        const CGDirectDisplayID id = displays[index];
        const CGRect frame = CGDisplayBounds(id);
        if (!CGDisplayIsActive(id) || !validDisplayBounds(frame)) continue;
        // Quartz bounds use top-left global pointer coordinates, including
        // negative origins. Do not substitute AppKit's bottom-left screen frame
        // or Retina backing pixels for these input coordinates.
        output.push_back({id, frame.origin.x, frame.origin.y, frame.size.width, frame.size.height, id == primary});
    }
    if (output.empty()) return {MaiErrorCode::NotFound, "no active display has valid pointer bounds"};
    std::sort(output.begin(), output.end(), [](const DesktopNativeDisplay& first, const DesktopNativeDisplay& second) {
        return first.id < second.id;
    });
    return output;
}

MaiResult<std::vector<DesktopNativeWindow>> maiDesktopWindows() {
    if (!CGPreflightScreenCaptureAccess())
        return {MaiErrorCode::NotConfigured,
                "grant MaiChat Screen & System Audio Recording permission and restart it"};
    std::vector<DesktopNativeWindow> output;
    CFArrayRef windows = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
        kCGNullWindowID);
    if (!windows) return {MaiErrorCode::NotSupported, "could not list visible macOS windows"};
    for (CFIndex index = 0; index < CFArrayGetCount(windows) && output.size() < 200; ++index) {
        CFDictionaryRef details = static_cast<CFDictionaryRef>(
            CFArrayGetValueAtIndex(windows, index));
        if (!details || CFGetTypeID(details) != CFDictionaryGetTypeID()) continue;
        const auto name = static_cast<CFStringRef>(
            CFDictionaryGetValue(details, kCGWindowName));
        const std::string title = utf8(name);
        if (title.empty()) continue;
        int layer = -1;
        const auto layerValue = static_cast<CFNumberRef>(
            CFDictionaryGetValue(details, kCGWindowLayer));
        if (!layerValue || !CFNumberGetValue(layerValue, kCFNumberIntType, &layer) ||
            layer != 0) continue;
        CGRect frame{};
        const auto frameValue = static_cast<CFDictionaryRef>(
            CFDictionaryGetValue(details, kCGWindowBounds));
        if (!frameValue || !CGRectMakeWithDictionaryRepresentation(frameValue, &frame) ||
            frame.size.width < 80 || frame.size.height < 50) continue;
        std::uint32_t id = 0;
        int pid = 0;
        const auto idValue = static_cast<CFNumberRef>(
            CFDictionaryGetValue(details, kCGWindowNumber));
        const auto pidValue = static_cast<CFNumberRef>(
            CFDictionaryGetValue(details, kCGWindowOwnerPID));
        if (!idValue || !pidValue ||
            !CFNumberGetValue(idValue, kCFNumberSInt32Type, &id) ||
            !CFNumberGetValue(pidValue, kCFNumberIntType, &pid)) continue;
        DesktopNativeWindow window;
        window.id = id;
        window.processId = std::uint64_t(pid);
        window.title = title;
        window.application = utf8(static_cast<CFStringRef>(
            CFDictionaryGetValue(details, kCGWindowOwnerName)));
        window.x = frame.origin.x;
        window.y = frame.origin.y;
        window.width = frame.size.width;
        window.height = frame.size.height;
        output.push_back(std::move(window));
    }
    CFRelease(windows);
    return output;
}

MaiResult<std::vector<DesktopNativeElement>> maiDesktopElements(
    const DesktopNativeWindow& window) {
    const MaiError permission = accessPermission();
    if (permission) return permission;
    AXUIElementRef root = accessibilityWindow(window);
    if (!root) return {MaiErrorCode::NotFound, "window is unavailable to macOS Accessibility"};
    std::vector<DesktopNativeElement> output;
    std::vector<AXUIElementRef> pending{root};
    for (std::size_t index = 0; index < pending.size() && index < 250; ++index) {
        AXUIElementRef current = pending[index];
        DesktopNativeElement element;
        element.role = attribute(current, kAXRoleAttribute);
        element.label = attribute(current, kAXTitleAttribute);
        if (element.label.empty())
            element.label = attribute(current, kAXDescriptionAttribute);
        if (element.label.empty())
            element.label = attribute(current, kAXPlaceholderValueAttribute);
        if (!element.role.empty() && !element.label.empty() && bounds(current, element) &&
            element.width > 0 && element.height > 0 && output.size() < 80)
            output.push_back(std::move(element));
        CFTypeRef children = nullptr;
        if (AXUIElementCopyAttributeValue(current, kAXChildrenAttribute, &children) ==
                kAXErrorSuccess && children && CFGetTypeID(children) == CFArrayGetTypeID()) {
            const CFArrayRef array = static_cast<CFArrayRef>(children);
            for (CFIndex child = 0; child < CFArrayGetCount(array) && pending.size() < 250;
                 ++child) {
                AXUIElementRef value = static_cast<AXUIElementRef>(
                    const_cast<void*>(CFArrayGetValueAtIndex(array, child)));
                if (CFGetTypeID(value) == AXUIElementGetTypeID()) {
                    CFRetain(value);
                    pending.push_back(value);
                }
            }
        }
        if (children) CFRelease(children);
        CFRelease(current);
    }
    // Any retained children after the traversal cap still belong to this call.
    for (std::size_t index = std::min<std::size_t>(pending.size(), 250);
         index < pending.size(); ++index) CFRelease(pending[index]);
    return output;
}

MaiError maiDesktopActivateWindow(const DesktopNativeWindow& window) {
    const MaiError permission = accessPermission();
    if (permission) return permission;
    AXUIElementRef selected = accessibilityWindow(window);
    if (!selected)
        return {MaiErrorCode::NotFound, "selected window is no longer available"};
    __block bool activated = false;
    const auto work = ^{
        @autoreleasepool {
            NSRunningApplication* app = [NSRunningApplication
                runningApplicationWithProcessIdentifier:pid_t(window.processId)];
            activated = app && [app activateWithOptions:0];
        }
    };
    if ([NSThread isMainThread]) work();
    else dispatch_sync(dispatch_get_main_queue(), work);
    if (!activated) {
        CFRelease(selected);
        return {MaiErrorCode::Busy, "macOS did not activate the selected application"};
    }
    AXUIElementSetAttributeValue(selected, kAXMainAttribute, kCFBooleanTrue);
    AXUIElementSetAttributeValue(selected, kAXFocusedAttribute, kCFBooleanTrue);
    AXUIElementPerformAction(selected, kAXRaiseAction);
    CFRelease(selected);
    for (int attempt = 0; attempt < 20; ++attempt) {
        const auto focused = maiDesktopWindowIsFocused(window);
        if (!focused) return focused.error();
        if (focused.value()) return {};
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return {MaiErrorCode::Busy, "selected window did not become focused"};
}

MaiError maiDesktopClick(double x, double y) {
    DesktopNativeInput input;
    input.x = x;
    input.y = y;
    return maiDesktopInput(input);
}

MaiError maiDesktopType(const std::string& text) {
    DesktopNativeInput input;
    input.kind = DesktopNativeInputKind::Type;
    input.text = text;
    return maiDesktopInput(input);
}

MaiError maiDesktopKey(const std::string& key) {
    DesktopNativeInput input;
    input.kind = DesktopNativeInputKind::Key;
    input.key = key;
    return maiDesktopInput(input);
}

MaiError maiDesktopScroll(double x, double y, int lines) {
    DesktopNativeInput input;
    input.kind = DesktopNativeInputKind::Scroll;
    input.x = x;
    input.y = y;
    input.lines = lines;
    return maiDesktopInput(input);
}

MaiResult<bool> maiDesktopWindowIsFocused(const DesktopNativeWindow& window) {
    const MaiError permission = accessPermission();
    if (permission) return permission;
    AXUIElementRef selected = accessibilityWindow(window);
    if (!selected)
        return {MaiErrorCode::NotFound, "selected CGWindowID has no unique Accessibility window"};
    bool front = false;
    @autoreleasepool {
        NSRunningApplication* app = [[NSWorkspace sharedWorkspace] frontmostApplication];
        front = app && app.processIdentifier == pid_t(window.processId);
    }
    AXUIElementRef application = AXUIElementCreateApplication(pid_t(window.processId));
    CFTypeRef focused = nullptr;
    const bool matches = front && application &&
        AXUIElementCopyAttributeValue(application, kAXFocusedWindowAttribute,
            &focused) == kAXErrorSuccess && focused &&
        CFGetTypeID(focused) == AXUIElementGetTypeID() && CFEqual(selected, focused);
    if (focused) CFRelease(focused);
    if (application) CFRelease(application);
    CFRelease(selected);
    return matches;
}

MaiResult<bool> maiDesktopWindowAcceptsInput(const DesktopNativeWindow& window) {
    const MaiError permission = accessPermission();
    if (permission) return permission;
    const ScopedCFValue selected(accessibilityWindow(window));
    if (!selected)
        return {MaiErrorCode::NotFound, "selected CGWindowID has no unique Accessibility window"};
    bool front = false;
    @autoreleasepool {
        NSRunningApplication* app = [[NSWorkspace sharedWorkspace] frontmostApplication];
        front = app && app.processIdentifier == pid_t(window.processId);
    }
    if (!front) return false;
    const ScopedCFValue application(AXUIElementCreateApplication(pid_t(window.processId)));
    const ScopedCFValue focused = application ? copiedAttribute(
        static_cast<AXUIElementRef>(application.get()), kAXFocusedWindowAttribute) :
        ScopedCFValue();
    if (!focused || !acceptsInputFromWindow(static_cast<AXUIElementRef>(selected.get()),
            static_cast<AXUIElementRef>(focused.get()), pid_t(window.processId)))
        // Re-activating the owner here can close an independent panel. Require
        // an explicit target/focus choice when public AX cannot prove ownership.
        return {MaiErrorCode::Busy,
                "focused macOS window ownership is unproven; explicitly observe or focus that window"};
    return true;
}

MaiError maiDesktopInput(const DesktopNativeInput& input) {
    const MaiError permission = accessPermission();
    if (permission) return permission;
    if (input.kind == DesktopNativeInputKind::Type ||
        input.kind == DesktopNativeInputKind::ReplaceFocusedText) {
        if (input.text.size() > 4000 ||
            (input.kind == DesktopNativeInputKind::Type && input.text.empty()))
            return {MaiErrorCode::InvalidInput, "text must contain at most 4000 UTF-8 bytes"};
    }
    const bool positioned =
        input.kind == DesktopNativeInputKind::Click || input.kind == DesktopNativeInputKind::DoubleClick ||
        input.kind == DesktopNativeInputKind::RightClick || input.kind == DesktopNativeInputKind::Drag ||
        input.kind == DesktopNativeInputKind::Scroll || input.kind == DesktopNativeInputKind::Move;
    if (positioned && !validPoint(input.x, input.y))
        return {MaiErrorCode::InvalidInput, "pointer coordinates are invalid"};
    const int maximumDragMs = input.desktopScope ? 5000 : 200;
    if (input.kind == DesktopNativeInputKind::Drag &&
        (!validPoint(input.toX, input.toY) || input.durationMs < 1 || input.durationMs > maximumDragMs))
        return {MaiErrorCode::InvalidInput, input.desktopScope
                                                ? "drag needs a valid destination and 1 to 5000 ms duration"
                                                : "drag needs a valid destination and 1 to 200 ms duration"};
    if (input.kind == DesktopNativeInputKind::Scroll &&
        (input.lines == 0 || input.lines < -10 || input.lines > 10))
        return {MaiErrorCode::InvalidInput, "scroll needs -10 to 10 lines, excluding zero"};
    if ((input.expectedWindowId == 0) != (input.expectedProcessId == 0))
        return {MaiErrorCode::InvalidInput, "expected window and process identities must be provided together"};
    if (input.desktopScope) {
        const MaiError display = validateDisplay(input.display);
        if (display) return display;
        if ((positioned && !displayContainsPoint(input.display, CGPointMake(input.x, input.y))) ||
            (input.kind == DesktopNativeInputKind::Drag &&
             !displayContainsPoint(input.display, CGPointMake(input.toX, input.toY))))
            return {MaiErrorCode::InvalidInput, "pointer coordinates are outside the observed display"};
    }
    DesktopKeyChord chord;
    if (input.kind == DesktopNativeInputKind::Key) {
        const auto parsed = maiDesktopParseKey(input.key);
        if (!parsed) return parsed.error();
        chord = parsed.value();
    }
    // Chords check explicit sides and layout-inferred modifiers together in
    // sendChord. Other actions must never become modified gestures, and never
    // clear hardware modifiers by manufacturing key-up on the user's keys.
    if (input.kind != DesktopNativeInputKind::Key && hasUnexpectedModifiers(nullptr))
        return {MaiErrorCode::Busy, "release modifiers that are not part of the requested action"};
    InputScope scope(input);
    const MaiError ready = scope.validate();
    if (ready) return ready;
    if (input.kind == DesktopNativeInputKind::Key) return sendChord(scope, chord);
    if (input.kind == DesktopNativeInputKind::Type) return sendText(scope, input.text);
    if (input.kind == DesktopNativeInputKind::ReplaceFocusedText) {
        const auto select = maiDesktopParseKey("Cmd+A");
        const MaiError selected = sendChord(scope, select.value());
        if (selected) return selected;
        if (input.text.empty()) return scope.key(kVK_Delete);
        return sendText(scope, input.text);
    }
    if (CGEventSourceButtonState(kCGEventSourceStateHIDSystemState, kCGMouseButtonLeft) ||
        CGEventSourceButtonState(kCGEventSourceStateHIDSystemState, kCGMouseButtonRight) ||
        CGEventSourceButtonState(kCGEventSourceStateHIDSystemState, kCGMouseButtonCenter))
        return {MaiErrorCode::Busy, "a mouse button is already held by the user"};
    if (input.kind == DesktopNativeInputKind::Move) return scope.move(CGPointMake(input.x, input.y), false);
    if (input.kind == DesktopNativeInputKind::Scroll) {
        CGEventRef event = CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitLine,
                                                         1, input.lines);
        if (!event) return {MaiErrorCode::Internal, "could not create scroll event"};
        CGEventSetLocation(event, CGPointMake(input.x, input.y));
        const MaiError before = scope.validate();
        if (!before) scope.post(event);
        CFRelease(event);
        return before ? before : scope.validate();
    }
    const bool right = input.kind == DesktopNativeInputKind::RightClick;
    const CGMouseButton button = right ? kCGMouseButtonRight : kCGMouseButtonLeft;
    const CGPoint point = CGPointMake(input.x, input.y);
    CGEventRef down = CGEventCreateMouseEvent(nullptr,
        right ? kCGEventRightMouseDown : kCGEventLeftMouseDown, point, button);
    CGEventRef up = CGEventCreateMouseEvent(nullptr,
        right ? kCGEventRightMouseUp : kCGEventLeftMouseUp, point, button);
    if (!down || !up) {
        if (down) CFRelease(down);
        if (up) CFRelease(up);
        return {MaiErrorCode::Internal, "could not create mouse events"};
    }
    CGEventSetIntegerValueField(down, kCGMouseEventClickState, 1);
    CGEventSetIntegerValueField(up, kCGMouseEventClickState, 1);
    MaiError result;
    if (input.kind == DesktopNativeInputKind::Drag) {
        result = scope.hold(down, up);
        if (!result) {
            const int steps = (std::max)(1, input.durationMs / 10);
            const auto started = std::chrono::steady_clock::now();
            for (int step = 1; step <= steps; ++step) {
                std::this_thread::sleep_until(started +
                    std::chrono::milliseconds(input.durationMs * step / steps));
                const double fraction = double(step) / steps;
                result = scope.move(CGPointMake(input.x + (input.toX - input.x) * fraction,
                    input.y + (input.toY - input.y) * fraction));
                if (result) break;
            }
            scope.release();
            if (!result) result = scope.validate();
        }
    } else {
        result = scope.tap(down, up);
        if (!result && input.kind == DesktopNativeInputKind::DoubleClick) {
            // AppKit derives double-click behavior from the Quartz click count.
            // Keep the two clicks within the configured system interval.
            std::this_thread::sleep_for(std::chrono::duration<double>(
                (std::min)(0.04, [NSEvent doubleClickInterval] / 4)));
            CGEventSetIntegerValueField(down, kCGMouseEventClickState, 2);
            CGEventSetIntegerValueField(up, kCGMouseEventClickState, 2);
            result = scope.tap(down, up);
        }
    }
    CFRelease(down);
    CFRelease(up);
    return result;
}

MaiResult<std::string> maiDesktopFocusedText(const DesktopNativeWindow& window) {
    const auto active = maiDesktopWindowIsFocused(window);
    if (!active) return active.error();
    if (!active.value())
        return {MaiErrorCode::Busy, "selected window does not have foreground focus"};
    AXUIElementRef selected = accessibilityWindow(window);
    if (!selected) return {MaiErrorCode::NotFound, "selected window is unavailable"};
    AXUIElementRef system = AXUIElementCreateSystemWide();
    CFTypeRef focusedValue = nullptr;
    const AXError found = system ? AXUIElementCopyAttributeValue(system,
        kAXFocusedUIElementAttribute, &focusedValue) : kAXErrorFailure;
    if (system) CFRelease(system);
    if (found != kAXErrorSuccess || !focusedValue ||
        CFGetTypeID(focusedValue) != AXUIElementGetTypeID()) {
        if (focusedValue) CFRelease(focusedValue);
        CFRelease(selected);
        return {MaiErrorCode::NotConfigured, "focused input is unavailable to Accessibility"};
    }
    AXUIElementRef focused = static_cast<AXUIElementRef>(focusedValue);
    pid_t pid = 0;
    CFTypeRef owner = nullptr;
    const bool belongs = AXUIElementGetPid(focused, &pid) == kAXErrorSuccess &&
        std::uint64_t(pid) == window.processId &&
        AXUIElementCopyAttributeValue(focused, kAXWindowAttribute, &owner) == kAXErrorSuccess &&
        owner && CFGetTypeID(owner) == AXUIElementGetTypeID() && CFEqual(owner, selected);
    if (owner) CFRelease(owner);
    CFRelease(selected);
    if (!belongs) {
        CFRelease(focused);
        return {MaiErrorCode::Busy, "focused input is outside the selected window"};
    }
    const std::string role = attribute(focused, kAXRoleAttribute);
    const std::string subrole = attribute(focused, kAXSubroleAttribute);
    if (role.empty() || role == "AXSecureTextField" || subrole == "AXSecureTextField" ||
        subrole == "AXPasswordField") {
        CFRelease(focused);
        return {MaiErrorCode::NotSupported, "secure or unverified focused input cannot be read"};
    }
    CFTypeRef value = nullptr;
    const AXError read = AXUIElementCopyAttributeValue(focused, kAXValueAttribute, &value);
    CFRelease(focused);
    if (read != kAXErrorSuccess || !value || CFGetTypeID(value) != CFStringGetTypeID()) {
        if (value) CFRelease(value);
        return {MaiErrorCode::NotSupported, "focused input does not expose a readable text value"};
    }
    const CFStringRef text = static_cast<CFStringRef>(value);
    const CFIndex length = CFStringGetLength(text);
    if (length > 65536) {
        CFRelease(value);
        return {MaiErrorCode::NotSupported, "focused input text exceeds the readback limit"};
    }
    std::vector<UInt8> bytes(std::size_t(length * 4 + 1), 0);
    CFIndex byteCount = 0;
    const bool converted = CFStringGetBytes(text, CFRangeMake(0, length),
        kCFStringEncodingUTF8, 0, false, bytes.data(), CFIndex(bytes.size()),
        &byteCount) == length;
    CFRelease(value);
    if (!converted) return {MaiErrorCode::Protocol, "focused input text is not valid Unicode"};
    const auto stillFocused = maiDesktopWindowIsFocused(window);
    if (!stillFocused) return stillFocused.error();
    if (!stillFocused.value())
        return {MaiErrorCode::Busy, "selected window lost focus while reading its text"};
    return std::string(reinterpret_cast<const char*>(bytes.data()), std::size_t(byteCount));
}

MaiResult<bool> maiDesktopFocusedInputIsSecure() {
    const MaiError permission = accessPermission();
    if (permission) return permission;
    AXUIElementRef system = AXUIElementCreateSystemWide();
    if (!system) return {MaiErrorCode::Internal, "could not inspect focused input"};
    CFTypeRef focused = nullptr;
    const AXError result = AXUIElementCopyAttributeValue(system,
        kAXFocusedUIElementAttribute, &focused);
    CFRelease(system);
    if (result != kAXErrorSuccess || !focused ||
        CFGetTypeID(focused) != AXUIElementGetTypeID()) {
        if (focused) CFRelease(focused);
        return {MaiErrorCode::NotConfigured, "focused input is not available to Accessibility"};
    }
    const std::string role = attribute(static_cast<AXUIElementRef>(focused), kAXRoleAttribute);
    const std::string subrole = attribute(static_cast<AXUIElementRef>(focused),
                                           kAXSubroleAttribute);
    CFRelease(focused);
    if (role.empty())
        return {MaiErrorCode::NotConfigured, "could not verify whether focused input is secure"};
    return role == "AXSecureTextField" || subrole == "AXSecureTextField" ||
           subrole == "AXPasswordField";
}
