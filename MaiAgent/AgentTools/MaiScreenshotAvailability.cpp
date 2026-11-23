// 文件作用：实现可跨平台验证的截图可用性闸门和错误说明，系统读取留在平台后端。
#include "MaiScreenshotAvailability.h"

#include <string>

namespace {
const char* observationText(MaiScreenshotObservation value) {
    switch (value) {
        case MaiScreenshotObservation::False: return "false";
        case MaiScreenshotObservation::True: return "true";
        default: return "unknown";
    }
}

const char* phaseText(MaiScreenshotCheckPhase phase) {
    switch (phase) {
        case MaiScreenshotCheckPhase::Entry: return "entry";
        case MaiScreenshotCheckPhase::Worker: return "worker";
        case MaiScreenshotCheckPhase::Frame: return "frame";
        case MaiScreenshotCheckPhase::Delivery: return "delivery";
        default: return "unknown";
    }
}

std::string details(const MaiScreenshotAvailabilitySnapshot& snapshot) {
    const auto& value = snapshot.observed;
    return std::string("phase=") + phaseText(snapshot.phase) +
           "; guiSession=" + observationText(value.guiSession) +
           "; onConsole=" + observationText(value.onConsole) +
           "; loginDone=" + observationText(value.loginDone) +
           "; frontmostKnown=" + observationText(value.frontmostKnown) +
           "; loginWindow=" + observationText(value.loginWindow) +
           "; screenRecordingPermission=" + observationText(value.screenRecordingPermission) +
           "; failed=" + (snapshot.failed ? "true" : "false") +
           "; nativeStreamPresent=" + (snapshot.nativeStreamPresent ? "true" : "false") +
           "; capturing=" + (snapshot.capturing ? "true" : "false");
}
}  // namespace

bool maiScreenshotDesktopCanCapture(const MaiScreenshotAvailability& observed) {
    return observed.guiSession == MaiScreenshotObservation::True &&
           observed.onConsole == MaiScreenshotObservation::True &&
           observed.loginDone == MaiScreenshotObservation::True &&
           observed.frontmostKnown == MaiScreenshotObservation::True &&
           observed.loginWindow == MaiScreenshotObservation::False;
}

MaiError maiScreenshotAvailabilityError(const MaiScreenshotAvailabilitySnapshot& snapshot) {
    if (!maiScreenshotDesktopCanCapture(snapshot.observed)) {
        return MaiError::make(MaiErrorCode::Busy,
                              "screen capture desktop is temporarily unavailable; " +
                                  details(snapshot) +
                                  ". Use an active, unlocked desktop and retry. If the resident "
                                  "capture remains unavailable, use screenshot(action=stop), "
                                  "then capture again; stopping does not unlock or activate the "
                                  "desktop.");
    }
    if (snapshot.observed.screenRecordingPermission == MaiScreenshotObservation::False) {
        return MaiError::make(
            MaiErrorCode::NotConfigured,
            "Allow Screen & System Audio Recording for MaiChat, restart it, and retry; " +
                details(snapshot));
    }
    return {};
}

MaiError maiRecordScreenshotAvailability(MaiScreenshotAvailabilityHistory& history,
                                         const MaiScreenshotAvailabilitySnapshot& snapshot) {
    const auto error = maiScreenshotAvailabilityError(snapshot);
    history.current = snapshot;
    if (error) {
        history.hasRejection = true;
        history.lastRejection = snapshot;
    }
    return error;
}
