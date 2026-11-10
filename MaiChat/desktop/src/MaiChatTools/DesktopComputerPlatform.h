#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "MaiError.h"

struct DesktopNativeWindow {
    std::uint64_t id = 0;
    std::uint64_t processId = 0;
    std::string title;
    std::string application;
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
};

struct DesktopNativeElement {
    std::string role;
    std::string label;
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
};

// Bounds use native pointer coordinates; the observation reports PNG pixels
// separately and supplies the normalized mapping between these spaces.
struct DesktopNativeDisplay {
    std::uint64_t id = 0;
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
    bool primary = false;
};

enum class DesktopNativeInputKind { Click, DoubleClick, RightClick, Drag, Type, Key, ReplaceFocusedText, Scroll, Move };

// The controller resolves observed window coordinates before native dispatch.
// Cancellation is checked throughout gestures; native code always releases its
// own pressed buttons and keys when dispatch fails or the foreground changes.
struct DesktopNativeInput {
    DesktopNativeInputKind kind = DesktopNativeInputKind::Click;
    double x = 0;
    double y = 0;
    double toX = 0;
    double toY = 0;
    std::string text;
    std::string key;
    int lines = 0;
    int durationMs = 200;
    // New controller calls bind dispatch to the observed native identity. Zero
    // keeps the older free function wrappers scoped to their initial foreground.
    std::uint64_t expectedWindowId = 0;
    std::uint64_t expectedProcessId = 0;
    // Explicit display scope permits intentional foreground changes; the legacy
    // window scope continues checking the selected window identity.
    bool desktopScope = false;
    DesktopNativeDisplay display;
    std::function<bool()> isCanceled;
};

MaiResult<std::vector<DesktopNativeWindow>> maiDesktopWindows();
MaiResult<std::vector<DesktopNativeDisplay>> maiDesktopDisplays();
MaiResult<std::vector<DesktopNativeElement>> maiDesktopElements(
    const DesktopNativeWindow& window);
MaiError maiDesktopActivateWindow(const DesktopNativeWindow& window);
MaiError maiDesktopClick(double x, double y);
MaiError maiDesktopType(const std::string& text);
MaiError maiDesktopKey(const std::string& key);
MaiError maiDesktopScroll(double x, double y, int lines);
MaiResult<bool> maiDesktopFocusedInputIsSecure();
MaiError maiDesktopInput(const DesktopNativeInput& input);
MaiResult<std::string> maiDesktopFocusedText(const DesktopNativeWindow& window);
MaiResult<bool> maiDesktopWindowIsFocused(const DesktopNativeWindow& window);
// Input may continue in a menu/HUD owned by the selected window. Text readback
// and explicit focus verification still require the selected window itself.
MaiResult<bool> maiDesktopWindowAcceptsInput(const DesktopNativeWindow& window);

#if defined(_WIN32)
#include "MaiChatTools/DesktopComputerKeys.h"

// Read-only native key resolution shared by dispatch and host diagnostics. The
// supplied layout is the target thread's HKL; resolving never activates a window
// or injects input. A valid VK and its scan/extended identity are returned, or a
// typed error for unsupported hardware/system keys. impliedModifiers only apply
// to literal characters; named punctuation keys retain their physical identity.
struct DesktopWindowsKeyBinding {
    std::uint16_t virtualKey = 0;
    std::uint16_t scanCode = 0;
    bool extended = false;
    std::vector<DesktopKeyModifier> impliedModifiers;
};

MaiResult<DesktopWindowsKeyBinding> maiDesktopWindowsResolveKey(
    const DesktopKeyChord& chord, std::uintptr_t keyboardLayout);
#endif
