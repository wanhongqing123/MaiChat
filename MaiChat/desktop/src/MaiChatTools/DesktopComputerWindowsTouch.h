#pragma once

#include <memory>
#include <functional>
#include <utility>

#include "MaiChatTools/DesktopComputerPlatform.h"

using DesktopWindowsTouchReceiverValidator =
    std::function<MaiError(const DesktopNativeInput&, std::int32_t, std::int32_t)>;

// Ordinary source-report touchscreen adapter. One source owns one contact
// ledger and a resident writer. Initial interactive births expand after a
// normal source report, not a target-window acknowledgment. Caller cancellation
// is consumed only by synchronous dispatch and is cleared before its response.
// Guarded sources retain a value copy of receiver/display identity without
// caller closures; every ordinary report rechecks that scope before injection.
// Reports and deferred cleanup share admission with public operations; a
// reentrant/overlapping call returns Busy instead of waiting on the writer.
// Immutable state/fault/physical positions remain readable during native calls;
// positions never reinterpret cached points through a later viewport.
// Opaque caller IDs never become Windows pointer IDs or device handles.
// Direction starts at screen +Y and increases toward -X, as for pen tilt.
// It is independent of the contact ellipse: normalized ellipse diameters are
// projected per viewport axis while center ratios use size-1 pixel endpoints.
class DesktopComputerWindowsTouch {
public:
    virtual ~DesktopComputerWindowsTouch() = default;
    virtual MaiError dispatch(const std::vector<DesktopInputTouch>& contacts, const DesktopInputPort& port,
                              std::function<bool()> isCanceled = {}) = 0;
    virtual MaiError dispatchGuarded(const std::vector<DesktopInputTouch>& contacts, const DesktopInputPort& port,
                                    const DesktopNativeInput& receiverScope,
                                    std::function<bool()> isCanceled = {}) {
        DesktopInputEvent event;
        event.kind = DesktopInputEventKind::Touch;
        event.contacts = contacts;
        if (receiverScope.executionWindow && !maiDesktopInputEventsOnlyRelease({event}))
            return {MaiErrorCode::NotSupported, "this touch adapter cannot guard a receiver"};
        return dispatch(contacts, port, std::move(isCanceled));
    }
    virtual DesktopNativeInputState state() const = 0;
    virtual std::vector<std::pair<std::int32_t, std::int32_t>> positions() const = 0;
    virtual MaiError validate() const = 0;
    // Revokes in-flight work before admission. Busy means source-thread cleanup
    // is pending; native faults/resources remain published for retry.
    // Cancels/destroys only this source. Completion ends owned native contacts;
    // it does not acknowledge application rollback or an observable cancel bit.
    virtual MaiError releaseAll() = 0;
};

std::shared_ptr<DesktopComputerWindowsTouch> makeDesktopComputerWindowsTouch();
std::shared_ptr<DesktopComputerWindowsTouch> makeDesktopComputerWindowsTouch(DesktopWindowsTouchReceiverValidator validator);
