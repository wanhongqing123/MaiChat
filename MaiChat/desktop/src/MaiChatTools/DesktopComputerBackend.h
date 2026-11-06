// Host seam for the ordinary controller. Production uses native capture/input;
// deterministic tests use the same controller without touching global input.
#pragma once

#include "MaiChatTools/DesktopComputerPlatform.h"
#include "MaiScreenshot.h"

#include <memory>

class DesktopComputerBackend {
public:
    virtual ~DesktopComputerBackend() = default;
    virtual MaiResult<std::vector<DesktopNativeWindow>> windows() = 0;
    virtual MaiResult<std::vector<DesktopNativeElement>> elements(const DesktopNativeWindow& window) = 0;
    virtual MaiError capture(const DesktopNativeWindow& window, MaiScreenshot& output) = 0;
    virtual MaiError activate(const DesktopNativeWindow& window) = 0;
    virtual MaiResult<bool> isFocused(const DesktopNativeWindow& window) = 0;
    // A native owned menu may be the input target. Explicit focus and text
    // verification still require the selected window itself.
    virtual MaiError activateForInput(const DesktopNativeWindow& window);
    virtual MaiResult<bool> inputFocused(const DesktopNativeWindow& window);
    virtual MaiResult<bool> secureInput() = 0;
    virtual MaiError input(const DesktopNativeInput& input) = 0;
    virtual MaiResult<std::string> focusedText(const DesktopNativeWindow& window) = 0;
};

// The product and separately hosted receivers share this native implementation.
// Callers may wrap it to narrow the set of windows available to a session.
std::shared_ptr<DesktopComputerBackend> makeDesktopComputerBackend();
