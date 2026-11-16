#pragma once

#include <memory>

#include "MaiChatTools/DesktopComputerPlatform.h"

// Ordinary frame-based touchscreen adapter. One instance owns one synthetic
// source; opaque caller IDs never become Windows pointer IDs or device handles.
class DesktopComputerWindowsTouch {
public:
    virtual ~DesktopComputerWindowsTouch() = default;
    virtual MaiError dispatch(const std::vector<DesktopInputTouch>& contacts, const DesktopInputPort& port) = 0;
    virtual DesktopNativeInputState state() const = 0;
    // Cancels/destroys only this source. Completion ends owned native contacts;
    // it does not acknowledge application rollback or an observable cancel bit.
    virtual MaiError releaseAll() = 0;
};

std::shared_ptr<DesktopComputerWindowsTouch> makeDesktopComputerWindowsTouch();
