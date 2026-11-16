#pragma once

// Private Windows-only contract. Production factories use the real Win32
// functions; deterministic tests replace only native effects/observations.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <functional>

#include "MaiChatTools/DesktopComputerPlatform.h"

class DesktopComputerWindowsPen;
class DesktopComputerWindowsTouch;

struct DesktopWindowsInputHooks {
    std::function<UINT(const INPUT&)> emitInput;
    std::function<MaiError(POINT)> move;
    std::function<MaiResult<POINT>()> cursor;
    std::function<MaiError(const DesktopNativeInput&)> validate;
    std::function<MaiError(const DesktopNativeInput&, POINT)> hitTest;
    std::function<SHORT(int)> keyState;
    std::function<std::uintptr_t()> layout;
    std::function<MaiResult<bool>()> secureInput;
    // Explicit recording adapter for the same native session. A keyboard hook
    // alone must never enable real pointer injection in a deterministic test.
    std::shared_ptr<DesktopComputerWindowsPen> pen;
    std::shared_ptr<DesktopComputerWindowsTouch> touch;
};

struct DesktopWindowsRawBinding {
    INPUT down{};
    INPUT up{};
    std::uint16_t virtualKey = 0;
    std::uint16_t physicalScan = 0;
    bool extended = false;
};

// Effective key is already remapped. The source identity is deliberately kept
// by InputScope, not collapsed into this layout-dependent native binding.
MaiResult<DesktopWindowsRawBinding> maiDesktopWindowsResolveRawKey(
    const DesktopInputRawKey& effective, const DesktopInputOptions& options, std::uintptr_t layout);

std::shared_ptr<DesktopNativeInputSession> makeDesktopWindowsInputSessionForTesting(DesktopWindowsInputHooks hooks);
