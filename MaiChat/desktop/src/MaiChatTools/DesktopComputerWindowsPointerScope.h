#pragma once

#include <optional>

#include "MaiChatTools/DesktopComputerPlatform.h"

// Compare only native authorization identity/geometry, never per-call payload
// or cancellation. A live source keeps its existing scope as its single ledger;
// changing that binding requires retiring the source first.
inline bool maiDesktopWindowsPointerScopeMatches(const std::optional<DesktopNativeInput>& bound,
                                                 const std::optional<DesktopNativeInput>& incoming) {
    if (bound.has_value() != incoming.has_value()) return false;
    if (!bound) return true;
    const auto& a = *bound;
    const auto& b = *incoming;
    if (a.desktopScope != b.desktopScope || a.expectedWindowId != b.expectedWindowId ||
        a.expectedProcessId != b.expectedProcessId)
        return false;
    if (a.desktopScope && (a.display.id != b.display.id || a.display.x != b.display.x || a.display.y != b.display.y ||
                           a.display.width != b.display.width || a.display.height != b.display.height))
        return false;
    if (a.executionWindow.has_value() != b.executionWindow.has_value()) return false;
    if (!a.executionWindow) return true;
    const auto& receiver = *a.executionWindow;
    const auto& next = *b.executionWindow;
    return receiver.id == next.id && receiver.processId == next.processId && receiver.x == next.x &&
           receiver.y == next.y && receiver.width == next.width && receiver.height == next.height &&
           receiver.boundsAvailable == next.boundsAvailable && receiver.minimized == next.minimized &&
           receiver.cloaked == next.cloaked;
}
