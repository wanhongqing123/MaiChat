// 文件作用：用普通值夹具验证运行期拒绝、权限证据和原生流对象的观察边界，不读取桌面。
#include "MaiScreenshotAvailability.h"

#include <cstdio>
#include <string>

namespace {
int failures = 0;
#define CHECK(value)                                                      \
    do {                                                                  \
        if (!(value)) {                                                   \
            ++failures;                                                   \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); \
        }                                                                 \
    } while (false)

MaiScreenshotAvailability available() {
    return {MaiScreenshotObservation::True,  MaiScreenshotObservation::True,
            MaiScreenshotObservation::True,  MaiScreenshotObservation::True,
            MaiScreenshotObservation::False, MaiScreenshotObservation::Unknown};
}

void runtimeFields() {
    const char* expected[] = {"guiSession=false", "onConsole=false", "loginDone=false",
                              "frontmostKnown=false", "loginWindow=true"};
    for (int field = 0; field < 5; ++field) {
        MaiScreenshotAvailabilitySnapshot snapshot;
        snapshot.phase = MaiScreenshotCheckPhase::Frame;
        snapshot.observed = available();
        auto& value = snapshot.observed;
        if (field == 0) value.guiSession = MaiScreenshotObservation::False;
        if (field == 1) value.onConsole = MaiScreenshotObservation::False;
        if (field == 2) value.loginDone = MaiScreenshotObservation::False;
        if (field == 3) value.frontmostKnown = MaiScreenshotObservation::False;
        if (field == 4) value.loginWindow = MaiScreenshotObservation::True;
        const auto error = maiScreenshotAvailabilityError(snapshot);
        CHECK(error.code() == MaiErrorCode::Busy);
        CHECK(error.message().find(expected[field]) != std::string::npos);
        CHECK(error.message().find("phase=frame") != std::string::npos);
        CHECK(error.message().find("screenRecordingPermission=unknown") != std::string::npos);
        CHECK(error.message().find("screenshot(action=stop)") != std::string::npos);
        CHECK(error.message().find("active, unlocked desktop") != std::string::npos);
        CHECK(error.message().find("Allow Screen") == std::string::npos);
    }
}

void unknownAndPermission() {
    MaiScreenshotAvailabilitySnapshot snapshot;
    snapshot.phase = MaiScreenshotCheckPhase::Delivery;
    snapshot.observed = available();
    CHECK(!maiScreenshotAvailabilityError(snapshot));
    snapshot.observed.screenRecordingPermission = MaiScreenshotObservation::True;
    CHECK(!maiScreenshotAvailabilityError(snapshot));
    snapshot.observed.screenRecordingPermission = MaiScreenshotObservation::False;
    auto error = maiScreenshotAvailabilityError(snapshot);
    CHECK(error.code() == MaiErrorCode::NotConfigured);
    CHECK(error.message().find("phase=delivery") != std::string::npos);
    CHECK(error.message().find("Allow Screen & System Audio Recording") != std::string::npos);
    snapshot.observed.onConsole = MaiScreenshotObservation::Unknown;
    error = maiScreenshotAvailabilityError(snapshot);
    CHECK(error.code() == MaiErrorCode::Busy);
    CHECK(error.message().find("onConsole=unknown") != std::string::npos);
    CHECK(error.message().find("Allow Screen") == std::string::npos);
    snapshot.observed = available();
    snapshot.observed.frontmostKnown = MaiScreenshotObservation::False;
    snapshot.observed.loginWindow = MaiScreenshotObservation::Unknown;
    error = maiScreenshotAvailabilityError(snapshot);
    CHECK(error.message().find("frontmostKnown=false; loginWindow=unknown") != std::string::npos);
}

void unknownFieldsAndPhases() {
    const char* names[] = {"guiSession=unknown", "onConsole=unknown", "loginDone=unknown",
                           "frontmostKnown=unknown", "loginWindow=unknown"};
    const MaiScreenshotCheckPhase phases[] = {
        MaiScreenshotCheckPhase::Entry, MaiScreenshotCheckPhase::Worker,
        MaiScreenshotCheckPhase::Frame, MaiScreenshotCheckPhase::Delivery};
    const char* phaseNames[] = {"phase=entry", "phase=worker", "phase=frame", "phase=delivery"};
    for (int field = 0; field < 5; ++field) {
        MaiScreenshotAvailabilitySnapshot snapshot;
        snapshot.observed = available();
        MaiScreenshotObservation* fields[] = {
            &snapshot.observed.guiSession, &snapshot.observed.onConsole,
            &snapshot.observed.loginDone, &snapshot.observed.frontmostKnown,
            &snapshot.observed.loginWindow};
        *fields[field] = MaiScreenshotObservation::Unknown;
        CHECK(!maiScreenshotDesktopCanCapture(snapshot.observed));
        for (int phase = 0; phase < 4; ++phase) {
            snapshot.phase = phases[phase];
            const auto error = maiScreenshotAvailabilityError(snapshot);
            CHECK(error.code() == MaiErrorCode::Busy);
            CHECK(error.message().find(names[field]) != std::string::npos);
            CHECK(error.message().find(phaseNames[phase]) != std::string::npos);
        }
    }
}

void residentOwnership() {
    MaiScreenshotAvailabilitySnapshot rejection;
    rejection.phase = MaiScreenshotCheckPhase::Worker;
    rejection.observed = available();
    rejection.observed.loginDone = MaiScreenshotObservation::False;
    rejection.failed = true;
    rejection.nativeStreamPresent = true;
    rejection.capturing = false;
    MaiScreenshotAvailabilityHistory history;
    CHECK(!history.hasRejection);
    const auto error = maiRecordScreenshotAvailability(history, rejection);
    CHECK(error.message().find("failed=true; nativeStreamPresent=true; capturing=false") !=
          std::string::npos);
    auto current = rejection;
    current.phase = MaiScreenshotCheckPhase::Entry;
    current.observed = available();
    CHECK(!maiRecordScreenshotAvailability(history, current));
    CHECK(history.current.phase == MaiScreenshotCheckPhase::Entry);
    CHECK(history.hasRejection);
    CHECK(history.lastRejection.phase == MaiScreenshotCheckPhase::Worker);
    CHECK(history.lastRejection.observed.loginDone == MaiScreenshotObservation::False);
    CHECK(error.message().find("streamActive") == std::string::npos);
}
}  // namespace

int main() {
    runtimeFields();
    unknownAndPermission();
    unknownFieldsAndPhases();
    residentOwnership();
    std::printf("screenshot availability failures=%d (pure fixtures; no capture/UI)\n", failures);
    return failures ? 1 : 0;
}
