#pragma once

#include <functional>
#include <memory>

#include "MaiChatTools/DesktopComputerPlatform.h"

// Coordinate policy around one native session. The native owns the only input
// ledger and supplies both composite and typed primitives, without a sibling
// provider or borrowed-state bridge.
// The optional validator lets an embedding host narrow native access further;
// the default checks the actual observed window/display and current permission.
// It must not retain the per-call cancellation callback or inject input.
// Native still classifies secure input using the actual command/frozen binding;
// a target validator cannot replace that check or prohibit safe navigation.
using DesktopInputScopeValidator = std::function<MaiError(const DesktopNativeInput&)>;

std::shared_ptr<DesktopNativeInputSession> makeDesktopComputerInputSession(
    std::shared_ptr<DesktopNativeInputSession> native, DesktopInputScopeValidator validator = {});

DesktopInputCapabilities maiDesktopInputCapabilities();
