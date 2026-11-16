#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include "MaiChatTools/DesktopComputerPlatform.h"

// One system pen source, independent of the tool protocol. Calls are synchronous
// and serialize against refresh; overlapping work returns Busy without a new
// dispatch. state() remains readable during native calls. The cancellation
// callback is checked around side effects and discarded before dispatch returns;
// it is never retained for periodic updates. Cleanup uses only cached owned
// geometry and does not require a current port or an uncanceled caller.
// Physical pixels retain their cached virtual-desktop plane. Layout changes
// retire the source instead of reinterpreting an old cleanup position.
// Three logical side-button bits share one system barrel bit (OR), not three
// independent hardware buttons. Tilt/azimuth become native tiltX/Y; distance,
// contact ellipse and shaft twist have no mapping in this adapter. Cancellation
// closes this source; it is not acknowledgement of application rollback.
// Eraser contact selects its legal hover intent immediately before DOWN when
// needed. Changing tool in range explicitly ends the old contact/leaves range
// before selecting the new role; it does not silently continue the old stroke.
class DesktopComputerWindowsPen {
public:
    virtual ~DesktopComputerWindowsPen() = default;
    virtual MaiError dispatch(const DesktopInputPen& pen, const DesktopInputPort& port,
                              std::function<bool()> isCanceled = {}) = 0;
    virtual DesktopNativeInputState state() const = 0;
    // Read the last asynchronous/cleanup fault without waiting for transport or
    // invoking an earlier caller. Empty native state does not imply no fault.
    virtual MaiError validate() const = 0;
    // Cached physical point of an active/uncertain owned source. Neutral or
    // absent sources have no point; never reconstruct it from a new viewport.
    virtual std::optional<std::pair<std::int32_t, std::int32_t>> position() const = 0;
    // Revokes in-flight work immediately. Busy means cleanup is pending on the
    // source thread; failed native destruction remains visible for retry.
    virtual MaiError releaseAll() = 0;
};

std::shared_ptr<DesktopComputerWindowsPen> makeDesktopComputerWindowsPen();
