#pragma once

#include <chrono>

#include "MaiChatTools/DesktopComputerWindowsPen.h"

// Exact system bits without exposing Windows headers in the ordinary API.
constexpr std::uint32_t kDesktopPenInRange = 0x00000002;
constexpr std::uint32_t kDesktopPenInContact = 0x00000004;
constexpr std::uint32_t kDesktopPenCanceled = 0x00008000;
constexpr std::uint32_t kDesktopPenDown = 0x00010000;
constexpr std::uint32_t kDesktopPenUpdate = 0x00020000;
constexpr std::uint32_t kDesktopPenUp = 0x00040000;
constexpr std::uint32_t kDesktopPenBarrel = 0x00000001;
constexpr std::uint32_t kDesktopPenInverted = 0x00000002;
constexpr std::uint32_t kDesktopPenEraser = 0x00000004;

struct DesktopWindowsPenDesktop {
    std::int32_t x = 0, y = 0, width = 0, height = 0;
};
struct DesktopWindowsPenApiPosition {
    std::int32_t x = 0, y = 0;
};
struct DesktopWindowsPenPoint {
    std::uint32_t pointerId = 1;
    std::uint32_t flags = 0;
    std::int32_t x = 0, y = 0;         // Physical screen pixels, including negative origins.
    DesktopWindowsPenDesktop desktop;  // Cached coordinate plane, also used by cleanup.
    std::uint32_t penFlags = 0;
    std::uint32_t pressure = 0;
    bool hasPressure = false;
    std::int32_t tiltX = 0, tiltY = 0;
    // No shaft-twist, distance or ellipse field: those are not native mappings
    // of this DTO's tilt azimuth / hover distance / contact ellipse.
};

// User32 takes offsets from the virtual-screen origin. A stale layout must
// fail before an OS injection; ordinary points/receipts remain physical pixels.
MaiResult<DesktopWindowsPenApiPosition> maiDesktopWindowsPenApiPosition(const DesktopWindowsPenPoint& point,
                                                                        const DesktopWindowsPenDesktop& currentDesktop);

enum class DesktopWindowsPenSubmitKind { Accepted, Retry, Failed };
struct DesktopWindowsPenSubmission {
    DesktopWindowsPenSubmitKind kind = DesktopWindowsPenSubmitKind::Accepted;
    MaiError error;
};

class DesktopWindowsPenTransport {
public:
    virtual ~DesktopWindowsPenTransport() = default;
    virtual MaiError create(std::uint32_t maxPointers) = 0;
    virtual DesktopWindowsPenSubmission inject(const DesktopWindowsPenPoint& point) = 0;
    virtual MaiError destroy() = 0;
};

// Recording mode disables periodic refresh explicitly. Production refresh is
// 16ms; create/inject/destroy always run on the same resident source thread.
std::shared_ptr<DesktopComputerWindowsPen> makeDesktopComputerWindowsPenWithTransport(
    std::shared_ptr<DesktopWindowsPenTransport> transport,
    std::chrono::milliseconds refreshInterval = std::chrono::milliseconds(16));
