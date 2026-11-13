// Native Agent entry for the same explicit friend/Session bindings used by IM.
#pragma once

#include <functional>

class DesktopSessionImBridge;
class MaiToolRegistry;
class QObject;

// Register on the application thread. The provider is invoked only there and
// must validate the immutable account/controller identity captured at register
// time. The host budget is bounded to 1..5000 ms and is not a model parameter.
void registerDesktopSessionImBindingTools(
    MaiToolRegistry& registry, QObject* dispatcher,
    std::function<DesktopSessionImBridge*()> bridgeProvider, int timeoutMs = 5000);
