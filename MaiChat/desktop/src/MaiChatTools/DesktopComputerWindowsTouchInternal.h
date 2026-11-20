#pragma once

#include "MaiChatTools/DesktopComputerWindowsTouch.h"

#include <chrono>
#include <mutex>

// Exact Win32 pointer action bits, kept independent of Windows headers so the
// composer and its transport contracts compile as ordinary C++17 everywhere.
constexpr std::uint32_t kDesktopTouchInRange = 0x00000002;
constexpr std::uint32_t kDesktopTouchInContact = 0x00000004;
constexpr std::uint32_t kDesktopTouchCanceled = 0x00008000;
constexpr std::uint32_t kDesktopTouchDown = 0x00010000;
constexpr std::uint32_t kDesktopTouchUpdate = 0x00020000;
constexpr std::uint32_t kDesktopTouchUp = 0x00040000;

struct DesktopWindowsTouchPoint {
    std::uint32_t pointerId = 0; // Owned native slot 0..9.
    std::uint32_t flags = 0;
    std::int32_t x = 0; // API coordinates relative to the virtual-screen origin.
    std::int32_t y = 0;
    std::int32_t screenX = 0; // Actual physical desktop coordinates for receipts/tests.
    std::int32_t screenY = 0;
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
    std::uint32_t pressure = 0;
    std::uint32_t orientation = 0;  // Native +X origin, clockwise; valid only when hasOrientation.
    bool hasPressure = false;
    bool hasOrientation = false;
    bool hasArea = false;
};

struct DesktopWindowsTouchFrame {
    std::vector<DesktopWindowsTouchPoint> points;
    std::int32_t virtualX = 0;
    std::int32_t virtualY = 0;
    std::int32_t virtualWidth = 0;
    std::int32_t virtualHeight = 0;
};

enum class DesktopWindowsTouchSubmitKind { Accepted, Retry, Failed };
struct DesktopWindowsTouchSubmission {
    DesktopWindowsTouchSubmitKind kind = DesktopWindowsTouchSubmitKind::Accepted;
    MaiError error;
};

class DesktopWindowsTouchTransport {
public:
    virtual ~DesktopWindowsTouchTransport() = default;
    virtual MaiError create(std::uint32_t maxContacts) = 0;
    virtual DesktopWindowsTouchSubmission inject(const DesktopWindowsTouchFrame& frame) = 0;
    virtual MaiError destroy() = 0;
};

// Explicit test-only capture. Production factories pass no recorder, so normal
// input is never added to a diagnostic history and no caller callback is kept.
struct DesktopWindowsTouchNativePacket {
    DesktopWindowsTouchFrame frame;
    std::vector<std::uint8_t> bytes;
    std::uint32_t stride = 0;
    std::uint64_t sourceHandle = 0;
    bool accepted = false;
    std::uint32_t error = 0;
    std::uint32_t creatorThread = 0, injectorThread = 0, maxContacts = 0;
    bool visualFeedback = true;
    std::int64_t creatorDpi = 0, beforeDpi = 0, effectiveDpi = 0, restoredDpi = 0;
    int creatorAwareness = -1, beforeAwareness = -1, effectiveAwareness = -1;
    bool creatorV2 = false, beforeV2 = false, effectiveV2 = false;
    std::uint64_t callStartedMs = 0, callFinishedMs = 0;
};
class DesktopWindowsTouchNativeTrace {
public:
    void append(DesktopWindowsTouchNativePacket packet) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (packets_.size() < 128) packets_.push_back(std::move(packet));
    }
    std::vector<DesktopWindowsTouchNativePacket> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return packets_;
    }
private:
    mutable std::mutex mutex_;
    std::vector<DesktopWindowsTouchNativePacket> packets_;
};
std::shared_ptr<DesktopWindowsTouchTransport> makeDesktopWindowsTouchNativeTransportForTesting(
    std::shared_ptr<DesktopWindowsTouchNativeTrace> trace, bool visualFeedback = true, bool explicitQpc = false);

// A zero interval is an explicit recording mode: the seed report advances
// without wall time and periodic updates are disabled. Production uses its
// ordinary 50ms source report cycle, not a startup sleep or delivery fence.
// All transport calls run on the resident writer; no caller closure is kept
// for periodic reports.
std::shared_ptr<DesktopComputerWindowsTouch> makeDesktopComputerWindowsTouchWithTransport(
    std::shared_ptr<DesktopWindowsTouchTransport> transport,
    std::chrono::milliseconds refreshInterval = std::chrono::milliseconds(50),
    DesktopWindowsTouchReceiverValidator validator = {});
