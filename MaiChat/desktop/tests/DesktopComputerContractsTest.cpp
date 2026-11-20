#include <QCoreApplication>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <json.hpp>

#include "MaiChatTools/DesktopComputerBackend.h"
#include <thread>
#include "MaiChatTools/DesktopComputerKeys.h"
#include "MaiChatTools/DesktopComputerInputProtocol.h"
#include "MaiChatTools/DesktopComputerInputSession.h"
#include "MaiChatTools/DesktopComputerTools.h"
#include "MaiTool.h"
#include "MaiAgentSendMediaTool.h"

namespace {

using Json = nlohmann::json;

int failures = 0;

#define CHECK(value)                                                                       \
    do {                                                                                   \
        if (!(value)) {                                                                    \
            std::fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, #value); \
            ++failures;                                                                    \
        }                                                                                  \
    } while (false)

bool contains(const Json& values, const char* expected) {
    if (!values.is_array()) return false;
    for (const auto& value : values) {
        if (value.is_string() && value.get<std::string>() == expected) return true;
    }
    return false;
}

std::int64_t fixtureClockMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// The controller owns this recording session through the ordinary native seam.
// Keep the watchdog's reads synchronized and never retain a per-call callback.
class RecordingInputSession final : public DesktopNativeInputSession {
public:
    using OneShot = std::function<MaiError(const DesktopNativeInput&)>;
    explicit RecordingInputSession(bool stateful = true, OneShot oneShot = {})
        : stateful_(stateful), oneShot_(std::move(oneShot)) {}
    std::function<void()> beforeInput;
    std::function<void()> beforeBatch;
    bool failBatchIfReset = false;

    MaiError input(const DesktopNativeInput& request) override {
        if (beforeInput) beforeInput();
        if (request.isCanceled && request.isCanceled()) return {MaiErrorCode::Canceled, "fixture canceled"};
        const bool independent = request.kind == DesktopNativeInputKind::KeyDown ||
            request.kind == DesktopNativeInputKind::KeyUp || request.kind == DesktopNativeInputKind::MouseDown ||
            request.kind == DesktopNativeInputKind::MouseUp;
        if (!stateful_ && independent)
            return {MaiErrorCode::NotSupported, "one-shot fixture does not support independent holds"};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++inputCalls_;
            scope_ = request;
            scope_.isCanceled = {};
            if (request.kind == DesktopNativeInputKind::KeyDown) add(held_.keysDown, request.key);
            if (request.kind == DesktopNativeInputKind::KeyUp) erase(held_.keysDown, request.key);
            if (request.kind == DesktopNativeInputKind::MouseDown) add(held_.buttonsDown, buttonName(request.button));
            if (request.kind == DesktopNativeInputKind::MouseUp) erase(held_.buttonsDown, buttonName(request.button));
        }
        // Only the test adapter delegates one-shot effects to the old backend spy.
        // Production still has one native executor, never a backend.input fallback.
        return oneShot_ ? oneShot_(request) : MaiError{};
    }
    DesktopNativeInputState state() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto projection = held_;
        for (const auto& key : held_.rawKeys)
            projection.keysDown.push_back("raw:" + std::to_string(key.code) + ":" + std::to_string(key.flags));
        return projection;
    }
    MaiError validate() const override { return {}; }
    DesktopInputCapabilities capabilities() const override {
        DesktopInputCapabilities value;
        value.baseline = "recording-native-contract";
        value.maxControllers = 0;
        value.maxTouchContacts = 0;
        for (const char* name : {"absolute_move", "relative_move", "mouse_button", "wheel", "raw_key", "unicode_text"})
            value.features.push_back({name, true, true, "recording-native", "record-only", "owned contract fixture"});
        return value;
    }
    MaiError resetControls() override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++resetCalls_;
        held_ = {};
        return {};
    }
    MaiError releaseAll() override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++releaseCalls_;
        held_ = {};
        return {};
    }
    MaiError inputEvents(const std::vector<DesktopInputEvent>& events, const DesktopNativeInput& scope,
                         const DesktopInputPort& port, const DesktopInputOptions& options) override {
        if (!stateful_) return {MaiErrorCode::NotSupported, "one-shot fixture has no typed hold ledger"};
        for (const auto& event : events)
            if (event.kind > DesktopInputEventKind::UnicodeText)
                return {MaiErrorCode::NotSupported, "fixture does not implement deferred pointer/device simulation"};
        const int resetBefore = resetCalls();
        if (beforeBatch) beforeBatch();
        if (scope.isCanceled && scope.isCanceled()) return {MaiErrorCode::Canceled, "fixture batch canceled"};
        std::lock_guard<std::mutex> lock(mutex_);
        if (failBatchIfReset && resetBefore != resetCalls_)
            return {MaiErrorCode::Canceled, "fixture operation revoked by reset"};
        ++batchCalls_;
        batch_ = events;
        port_ = port;
        options_ = options;
        scope_ = scope;
        scope_.isCanceled = {};  // Per-call callbacks never survive this synchronous seam.
        for (const auto& event : events) {
            if (event.kind == DesktopInputEventKind::MouseButton) {
                const char* names[] = {"left", "middle", "right", "back", "forward"};
                if (event.button >= 1 && event.button <= 5) {
                    if (event.release) erase(held_.buttonsDown, names[event.button - 1]);
                    else add(held_.buttonsDown, names[event.button - 1]);
                }
            }
            if (event.kind != DesktopInputEventKind::RawKey) continue;
            const auto same = [&](const DesktopInputRawKey& key) {
                return key.code == event.key.code && key.flags == event.key.flags &&
                    ((key.modifiers ^ event.key.modifiers) & 0x10) == 0;
            };
            const auto found = std::find_if(held_.rawKeys.begin(), held_.rawKeys.end(), same);
            if (event.key.release) {
                if (found != held_.rawKeys.end()) held_.rawKeys.erase(found);
            } else if (!event.key.repeat && found == held_.rawKeys.end()) {
                auto source = event.key;
                source.release = false;
                source.repeat = false;
                held_.rawKeys.push_back(source);  // A projection of this one native ledger, not borrowed state.
            }
        }
        return {};
    }
    int batchCalls() const { std::lock_guard<std::mutex> lock(mutex_); return batchCalls_; }
    std::vector<DesktopInputEvent> batch() const { std::lock_guard<std::mutex> lock(mutex_); return batch_; }
    DesktopInputPort port() const { std::lock_guard<std::mutex> lock(mutex_); return port_; }
    DesktopNativeInput scope() const { std::lock_guard<std::mutex> lock(mutex_); return scope_; }
    DesktopInputOptions options() const { std::lock_guard<std::mutex> lock(mutex_); return options_; }
    int inputCalls() const { std::lock_guard<std::mutex> lock(mutex_); return inputCalls_; }
    int releaseCalls() const { std::lock_guard<std::mutex> lock(mutex_); return releaseCalls_; }
    int resetCalls() const { std::lock_guard<std::mutex> lock(mutex_); return resetCalls_; }

private:
    static void add(std::vector<std::string>& values, const std::string& value) {
        if (std::find(values.begin(), values.end(), value) == values.end()) values.push_back(value);
    }
    static void erase(std::vector<std::string>& values, const std::string& value) {
        values.erase(std::remove(values.begin(), values.end(), value), values.end());
    }
    static const char* buttonName(DesktopMouseButton button) {
        switch (button) {
        case DesktopMouseButton::Left: return "left";
        case DesktopMouseButton::Right: return "right";
        case DesktopMouseButton::Middle: return "middle";
        case DesktopMouseButton::Back: return "back";
        case DesktopMouseButton::Forward: return "forward";
        }
        return "unknown";
    }
    bool stateful_;
    OneShot oneShot_;
    mutable std::mutex mutex_;
    DesktopNativeInputState held_;
    int inputCalls_ = 0, releaseCalls_ = 0, resetCalls_ = 0, batchCalls_ = 0;
    std::vector<DesktopInputEvent> batch_;
    DesktopInputPort port_;
    DesktopInputOptions options_;
    DesktopNativeInput scope_;
};

// This backend supplies actual PNG data and records the controller's native
// boundary calls. It never enumerates, captures or injects into the user's OS.
class RecordingBackend : public DesktopComputerBackend {
public:
    bool statefulInput = false;
    bool composeInputSession = false;
    int inputSessionCreations = 0;
    std::shared_ptr<RecordingInputSession> lastInputSession;
    std::vector<DesktopNativeWindow> visible{{101, 5001, "Isolated editor", "Fixture", 100, 200, 400, 300}};
    std::vector<DesktopNativeDisplay> availableDisplays{{11, 0, 0, 1600, 900, true},
                                                        {22, -1280, -120, 1280, 1024, false}};
    std::vector<DesktopNativeInput> inputs;
    std::vector<std::string> events;
    std::string textValue;
    bool focused = true;
    bool secure = false;
    bool ownedPopup = false;
    bool requirePopupForKey = false;
    int activationCount = 0;
    int windowsCount = 0;
    int captureCount = 0;
    int displayCaptureCount = 0;
    int captureOptionsCount = 0;
    int readCount = 0;
    std::uint64_t capturedWindowId = 0;
    std::uint64_t capturedDisplayId = 0;
    std::uint64_t lastAfterFrameSequence = 0;
    std::int64_t lastAfterFrameReceivedAtMs = 0;
    std::uint64_t lastProducedFrameSequence = 0;
    std::int64_t lastProducedFrameReceivedAtMs = 0;
    bool lastCaptureHadCancel = false;
    bool returnStaleDisplayFrame = false;
    MaiError activationError;
    MaiError inputError;
    MaiError readError;
    MaiError displayCaptureError;
    MaiError secureInputError;
    MaiError stateError;
    std::optional<std::vector<DesktopNativeElement>> nativeElements;
    int elementReads = 0;
    int stateCalls = 0;
    std::function<void()> onElements;
    std::function<void()> onState;
    std::function<void(int)> onWindows;
    std::function<void()> onActivate;
    std::function<void()> onCapture;
    std::function<void()> onDisplayCapture;
    std::function<void()> onInput;
    std::function<void()> onNativeInput;
    std::function<MaiError(const CaptureOptions&)> onDisplayCaptureOptions;

    std::shared_ptr<DesktopNativeInputSession> createInputSession() override {
        ++inputSessionCreations;
        RecordingInputSession::OneShot oneShot;
        if (!statefulInput) oneShot = [this](const DesktopNativeInput& input) { return this->input(input); };
        lastInputSession = std::make_shared<RecordingInputSession>(statefulInput, std::move(oneShot));
        lastInputSession->beforeInput = [this] { if (onNativeInput) onNativeInput(); };
        if (composeInputSession)
            return makeDesktopComputerInputSession(lastInputSession,
                [](const DesktopNativeInput&) { return MaiError{}; });
        return lastInputSession;
    }

    DesktopInputCapabilities inputCapabilities() const override {
        DesktopInputCapabilities value;
        value.baseline = "recording-backend-contract";
        value.maxControllers = 0;
        value.maxTouchContacts = 0;
        return value;  // Capability discovery must never reach the real OS from this fixture.
    }

    MaiResult<std::vector<DesktopNativeWindow>> windows() override {
        ++windowsCount;
        events.push_back("windows");
        if (onWindows) onWindows(windowsCount);
        return visible;
    }
    MaiResult<std::vector<DesktopNativeDisplay>> displays() override {
        events.push_back("displays");
        return availableDisplays;
    }
    MaiError captureDisplay(const DesktopNativeDisplay& display, MaiScreenshot& output) override {
        events.push_back("captureDisplay");
        ++displayCaptureCount;
        capturedDisplayId = display.id;
        output = {};
        if (displayCaptureError) return displayCaptureError;
        // Encoded pixels deliberately differ from native input dimensions. A
        // display mapping based on PNG width instead of native bounds is wrong.
        QImage image(320, 256, QImage::Format_RGB32);
        image.fill(display.id == 22 ? Qt::blue : Qt::green);
        QByteArray png;
        QBuffer buffer(&png);
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
            return {MaiErrorCode::Internal, "fixture display PNG encoding failed"};
        output.width = image.width();
        output.height = image.height();
        output.pngBytes.assign(png.begin(), png.end());
        output.frameSequence = 40 + displayCaptureCount;
        output.frameReceivedAtMs = 1700000000000LL + displayCaptureCount;
        lastProducedFrameSequence = output.frameSequence;
        lastProducedFrameReceivedAtMs = output.frameReceivedAtMs;
        if (onDisplayCapture) onDisplayCapture();
        return {};
    }
    MaiError captureDisplay(const DesktopNativeDisplay& display, MaiScreenshot& output,
                            const CaptureOptions& options) override {
        ++captureOptionsCount;
        lastAfterFrameSequence = options.afterFrameSequence;
        lastAfterFrameReceivedAtMs = options.afterFrameReceivedAtMs;
        lastCaptureHadCancel = bool(options.isCanceled);
        // Record only values: the callback is used synchronously and never
        // retained after this backend call returns.
        if (onDisplayCaptureOptions) {
            const MaiError stopped = onDisplayCaptureOptions(options);
            if (stopped) {
                output = {};
                return stopped;
            }
        }
        if (options.isCanceled && options.isCanceled()) {
            output = {};
            return {MaiErrorCode::Canceled, "fixture display capture canceled"};
        }
        const MaiError captured = captureDisplay(display, output);
        if (captured) return captured;
        if (options.isCanceled && options.isCanceled()) {
            output = {};
            return {MaiErrorCode::Canceled, "fixture display capture canceled"};
        }
        if (returnStaleDisplayFrame) {
            output.frameSequence = 41;
            output.frameReceivedAtMs = 1700000000001LL;
        } else if (options.afterFrameSequence || options.afterFrameReceivedAtMs) {
            // The fixture producer samples its own clock. Echoing a requested
            // freshness threshold would conceal incorrect controller checks.
            output.frameSequence = 1000 + displayCaptureCount;
            output.frameReceivedAtMs = fixtureClockMs();
        }
        lastProducedFrameSequence = output.frameSequence;
        lastProducedFrameReceivedAtMs = output.frameReceivedAtMs;
        return {};
    }
    MaiResult<std::vector<DesktopNativeElement>> elements(const DesktopNativeWindow& window) override {
        ++elementReads;
        if (onElements) onElements();
        if (nativeElements) return *nativeElements;
        return std::vector<DesktopNativeElement>{
            {"text", "Isolated editor input", window.x + 20, window.y + 20, 100, 30}};
    }
    MaiError setWindowState(const DesktopNativeWindow& window, DesktopWindowStateAction action,
                              const std::function<bool()>& isCanceled) override {
        ++stateCalls;
        if (onState) onState();
        if (isCanceled && isCanceled()) return {MaiErrorCode::Canceled, "fixture window state canceled"};
        if (stateError) return stateError;
        for (auto& current : visible) {
            if (current.id != window.id) continue;
            current.minimized = action == DesktopWindowStateAction::Minimize;
            current.boundsAvailable = !current.minimized;
            if (!current.minimized) {
                current.x = 100;
                current.y = 200;
                current.width = 400;
                current.height = 300;
            }
        }
        return {};
    }
    MaiError capture(const DesktopNativeWindow& window, MaiScreenshot& output) override {
        ++captureCount;
        capturedWindowId = window.id;
        if (onCapture) onCapture();
        QImage image(20, 20, QImage::Format_RGB32);
        image.fill(Qt::white);
        QByteArray png;
        QBuffer buffer(&png);
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
            return {MaiErrorCode::Internal, "fixture PNG encoding failed"};
        output.width = image.width();
        output.height = image.height();
        output.pngBytes.assign(png.begin(), png.end());
        return {};
    }
    MaiError activate(const DesktopNativeWindow&) override {
        if (ownedPopup) focused = true;
        ownedPopup = false;
        events.push_back("activate");
        ++activationCount;
        if (onActivate) onActivate();
        return activationError;
    }
    MaiResult<bool> isFocused(const DesktopNativeWindow&) override {
        events.push_back("isFocused");
        return focused;
    }
    MaiError activateForInput(const DesktopNativeWindow& window) override {
        return ownedPopup ? MaiError{} : activate(window);
    }
    MaiResult<bool> inputFocused(const DesktopNativeWindow& window) override {
        return ownedPopup ? MaiResult<bool>(true) : isFocused(window);
    }
    MaiResult<bool> secureInput() override {
        events.push_back("secureInput");
        if (secureInputError) return secureInputError;
        return secure;
    }
    MaiError input(const DesktopNativeInput& value) override {
        events.push_back("input");
        inputs.push_back(value);
        inputs.back().isCanceled = {};  // Record effects, never retain this call's callback.
        if (onInput) onInput();
        if (inputError) return inputError;
        if (value.kind == DesktopNativeInputKind::RightClick) {
            ownedPopup = true;
            focused = false;
        }
        if (value.kind == DesktopNativeInputKind::Key && requirePopupForKey && !ownedPopup)
            return {MaiErrorCode::Busy, "fixture popup was dismissed before its key input"};
        if (value.kind == DesktopNativeInputKind::ReplaceFocusedText) textValue = value.text;
        if (value.kind == DesktopNativeInputKind::Type) textValue += value.text;
        return {};
    }
    MaiResult<std::string> focusedText(const DesktopNativeWindow&) override {
        events.push_back("focusedText");
        ++readCount;
        if (readError) return readError;
        return textValue;
    }
};

struct Fixture {
    std::shared_ptr<RecordingBackend> backend = std::make_shared<RecordingBackend>();
    DesktopComputerController controller{backend};
    QTemporaryDir workspace;

    DesktopComputerAction observeDisplay(const std::string& kind = "click", const std::string& session = "owner") {
        DesktopComputerAction action;
        action.action = kind;
        action.desktopScope = true;
        action.displayId = 22;
        action.workspace = workspace.path().toUtf8().toStdString();
        action.xRatio = 0.25;
        action.yRatio = 0.5;
        action.toXRatio = 0.75;
        action.toYRatio = 0.75;
        const auto observed = controller.observeDisplay(session, workspace.path().toUtf8().toStdString(), 22);
        CHECK(observed);
        if (!observed) return action;
        const Json output = Json::parse(observed.value().json, nullptr, false);
        CHECK(output.is_object());
        if (output.is_object()) action.snapshotId = output.value("snapshot_id", std::string());
        CHECK(!action.snapshotId.empty());
        return action;
    }

    DesktopComputerAction observe(const std::string& kind = "click") {
        DesktopComputerAction action;
        action.action = kind;
        action.windowId = backend->visible.front().id;
        action.xRatio = 0.25;
        action.yRatio = 0.5;
        action.toXRatio = 0.75;
        action.toYRatio = 0.75;
        CHECK(workspace.isValid());
        const auto observed =
            controller.observe("owner", "", workspace.path().toUtf8().toStdString(), true, action.windowId);
        CHECK(observed);
        if (observed) {
            const Json output = Json::parse(observed.value().json, nullptr, false);
            CHECK(output.is_object());
            if (output.is_object()) action.snapshotId = output.value("snapshot_id", std::string());
            CHECK(!action.snapshotId.empty());
            CHECK(QFileInfo(QString::fromStdString(observed.value().imagePath)).size() > 0);
        }
        return action;
    }
};

std::shared_ptr<RecordingInputSession> holdControl(Fixture& fixture, bool display,
                                                   const std::string& turn = "turn-current") {
    fixture.backend->statefulInput = true;
    auto action = display ? fixture.observeDisplay("key_down") : fixture.observe("key_down");
    action.key = "Ctrl";
    action.turnId = turn;
    CHECK(fixture.controller.perform("owner", action));
    const auto native = fixture.backend->lastInputSession;
    CHECK(native);
    if (native) CHECK(native->state().keysDown == std::vector<std::string>{"Ctrl"});
    return native;
}

void verifyCanceledPerformReleasesHeldInput() {
    for (const bool display : {false, true}) {
        Fixture fixture;
        const auto native = holdControl(fixture, display);
        if (!native) continue;
        auto action = display ? fixture.observeDisplay("move") : fixture.observe("move");
        action.turnId = "turn-current";
        action.isCanceled = [] { return true; };
        const int callsBefore = native->inputCalls();
        const auto canceled = fixture.controller.perform("owner", action);
        CHECK(!canceled);
        if (!canceled) CHECK(canceled.error().code() == MaiErrorCode::Canceled);
        CHECK(native->inputCalls() == callsBefore);
        CHECK(native->state().empty());
        CHECK(fixture.controller.inputState("owner").empty());
        CHECK(native->releaseCalls() > 0);
    }
}

void verifyRejectedPerformReleasesHeldInput() {
    for (const bool display : {false, true}) {
        for (const bool secureQueryFails : {false, true}) {
            Fixture fixture;
            const auto native = holdControl(fixture, display);
            if (!native) continue;
            auto action = display ? fixture.observeDisplay("key") : fixture.observe("key");
            action.turnId = "turn-current";
            action.key = "A";
            fixture.backend->secure = !secureQueryFails;
            if (secureQueryFails)
                fixture.backend->secureInputError = {MaiErrorCode::Internal, "fixture secure-input query failed"};
            const int callsBefore = native->inputCalls();
            const auto rejected = fixture.controller.perform("owner", action);
            CHECK(!rejected);
            if (!rejected)
                CHECK(rejected.error().code() ==
                      (secureQueryFails ? MaiErrorCode::Internal : MaiErrorCode::NotSupported));
            CHECK(native->inputCalls() == callsBefore);
            CHECK(native->state().empty());
            CHECK(fixture.controller.inputState("owner").empty());
            CHECK(native->releaseCalls() > 0);
        }
    }
}

void verifyCanceledObservationReleasesOnlyItsSession() {
    for (const bool display : {false, true}) {
        for (const bool observingOwner : {false, true}) {
            Fixture fixture;
            const auto native = holdControl(fixture, display);
            if (!native) continue;
            const std::string session = observingOwner ? "owner" : "different-session";
            const std::string workspace = fixture.workspace.path().toUtf8().toStdString();
            const auto canceled =
                display ? fixture.controller.observeDisplay(
                              session, workspace, 22, [] { return true; }, 0, 0, 0, "turn-current")
                        : fixture.controller.observe(
                              session, "", workspace, false, 101, false, [] { return true; }, "turn-current");
            CHECK(!canceled);
            if (!canceled) CHECK(canceled.error().code() == MaiErrorCode::Canceled);
            CHECK(native->state().empty() == observingOwner);
            CHECK(fixture.controller.inputState("owner").empty() == observingOwner);
            CHECK((native->releaseCalls() > 0) == observingOwner);
        }
    }
}

void verifyOldTurnCanceledObservationPreservesReplacementHold() {
    for (const bool display : {false, true}) {
        Fixture fixture;
        const auto native = holdControl(fixture, display, "turn-new");
        if (!native) continue;
        const std::string workspace = fixture.workspace.path().toUtf8().toStdString();
        const auto canceled = display ? fixture.controller.observeDisplay(
                                            "owner", workspace, 22, [] { return true; }, 0, 0, 0, "turn-old")
                                      : fixture.controller.observe(
                                            "owner", "", workspace, false, 101, false, [] { return true; }, "turn-old");
        CHECK(!canceled);
        if (!canceled) CHECK(canceled.error().code() == MaiErrorCode::Canceled);
        CHECK(native->state().keysDown == std::vector<std::string>{"Ctrl"});
        CHECK(native->releaseCalls() == 0);
    }
}

void verifyOldTurnCleanupPreservesReplacementHold() {
    for (const bool display : {false, true}) {
        Fixture fixture;
        const auto native = holdControl(fixture, display, "turn-new");
        if (!native) continue;
        auto action = display ? fixture.observeDisplay("move") : fixture.observe("move");
        action.turnId = "turn-old";
        action.isCanceled = [] { return true; };
        const auto canceled = fixture.controller.perform("owner", action);
        CHECK(!canceled);
        if (!canceled) CHECK(canceled.error().code() == MaiErrorCode::Canceled);
        CHECK(!fixture.controller.releaseInput("owner", "turn-old"));
        CHECK(native->state().keysDown == std::vector<std::string>{"Ctrl"});
        CHECK(native->releaseCalls() == 0);

        // A queued old task's release cannot invalidate the replacement's token
        // either. The same fresh snapshot must still reach its native session.
        action.turnId = "turn-new";
        action.isCanceled = {};
        CHECK(fixture.controller.perform("owner", action));
        CHECK(native->state().keysDown == std::vector<std::string>{"Ctrl"});
        CHECK(!fixture.controller.releaseInput("owner", "turn-new"));
        CHECK(native->state().empty());
    }
}

void verifyCanceledToolObservationReleasesOnlyItsTurn() {
    for (const bool display : {false, true}) {
        for (const bool observingCurrentTurn : {false, true}) {
            Fixture fixture;
            const auto native = holdControl(fixture, display, "turn-new");
            if (!native) continue;
            MaiToolRegistry registry;
            auto controller =
                std::shared_ptr<DesktopComputerController>(&fixture.controller, [](DesktopComputerController*) {});
            registerDesktopComputerTools(registry, controller);
            MaiTool* tool = registry.find("desktop_computer");
            CHECK(tool);
            if (!tool) continue;
            std::atomic<bool> cancel{true};
            MaiToolContext context;
            context.sessionId = "owner";
            context.turnId = observingCurrentTurn ? "turn-new" : "turn-old";
            context.root = fixture.workspace.path().toUtf8().toStdString();
            context.cancel = &cancel;
            const auto canceled = tool->execute(display ? R"({"action":"observe","target":"display","display_id":"22"})"
                                                        : R"({"action":"observe","target":"window","window_id":"101"})",
                                                context);
            CHECK(canceled.hasError());
            if (canceled.hasError()) CHECK(canceled.error().code() == MaiErrorCode::Canceled);
            CHECK(native->state().empty() == observingCurrentTurn);
            CHECK((native->releaseCalls() > 0) == observingCurrentTurn);
        }
    }
}

void verifyIndependentUpDryRunKeepsOwnedInput() {
    for (const bool display : {false, true}) {
        Fixture fixture;
        const auto native = holdControl(fixture, display);
        if (!native) continue;
        DesktopComputerAction up;
        up.action = "key_up";
        up.key = "Ctrl";
        up.turnId = "turn-current";
        up.dryRun = true;
        const int callsBefore = native->inputCalls();
        const auto planned = fixture.controller.perform("owner", up);
        CHECK(planned && planned.value().dryRun && !planned.value().dispatched);
        CHECK(native->inputCalls() == callsBefore);
        CHECK(native->state().keysDown == std::vector<std::string>{"Ctrl"});
        up.dryRun = false;
        CHECK(fixture.controller.perform("owner", up));
        CHECK(native->state().empty());
    }
}

void verifyInvalidIndependentButtonCannotValidateWithoutAnOwner() {
    Fixture fixture;
    DesktopComputerAction up;
    up.action = "mouse_up";
    up.button = static_cast<DesktopMouseButton>(99);
    for (const bool dryRun : {false, true}) {
        up.dryRun = dryRun;
        const auto invalid = fixture.controller.perform("no-owner", up);
        CHECK(!invalid);
        if (!invalid) CHECK(invalid.error().code() == MaiErrorCode::InvalidInput);
    }
    CHECK(fixture.backend->inputs.empty());
}

void verifyIndependentUpConsumesOnlyRealOwnedWindowToken() {
    for (const bool display : {false, true}) {
        for (int mode = 0; mode < 4; ++mode) {
            Fixture fixture;
            const auto native = holdControl(fixture, display);
            if (!native) continue;
            auto shift = display ? fixture.observeDisplay("key_down") : fixture.observe("key_down");
            shift.key = "Shift";
            shift.turnId = "turn-current";
            CHECK(fixture.controller.perform("owner", shift));
            auto after = display ? fixture.observeDisplay("move") : fixture.observe("move");
            after.turnId = "turn-current";
            DesktopComputerAction up;
            up.action = "key_up";
            up.key = mode == 3 ? "A" : "Ctrl";
            up.turnId = mode == 2 ? "turn-old" : "turn-current";
            up.dryRun = mode == 1;
            const auto result = fixture.controller.perform("owner", up);
            CHECK(result);
            if (mode == 0) CHECK(native->state().keysDown == std::vector<std::string>{"Shift"});
            const auto next = fixture.controller.perform("owner", after);
            if (mode == 0) {
                CHECK(!next);
            } else {
                CHECK(next);
                CHECK(native->state().keysDown == (std::vector<std::string>{"Ctrl", "Shift"}));
            }
        }
    }
}

void verifyHeldLauncherModifiersDoNotRequireEditableFocus() {
    Fixture fixture;
    fixture.backend->statefulInput = true;
    auto down = fixture.observeDisplay("key_down");
    down.key = "Meta";
    down.turnId = "turn-launcher";
    CHECK(fixture.controller.perform("owner", down));
    const auto native = fixture.backend->lastInputSession;
    CHECK(native);
    if (!native) return;
    fixture.backend->secureInputError = {MaiErrorCode::NotConfigured, "there is no focused editable control"};
    auto shortcut = fixture.observeDisplay("key");
    shortcut.turnId = "turn-launcher";
#if defined(Q_OS_MAC)
    shortcut.key = "Space";
#else
    shortcut.key = "E";
#endif
    CHECK(fixture.controller.perform("owner", shortcut));
    CHECK(native->inputCalls() == 2);
    CHECK(native->state().keysDown == std::vector<std::string>{"Meta"});
}

void verifyReleaseDoesNotWaitForObservedStateDuringPlatformWork() {
    for (const bool display : {false, true}) {
        Fixture fixture;
        const auto native = holdControl(fixture, display);
        if (!native) continue;
        auto request = display ? fixture.observeDisplay("move") : fixture.observe("move");
        request.turnId = "turn-current";
        std::atomic<bool> canceled{false};
        request.isCanceled = [&] { return canceled.load(); };
        std::mutex gateMutex;
        std::condition_variable gate;
        bool waitingForGui = false;
        bool guiMayReturn = false;
        bool releaseDone = false;
        native->beforeInput = [&] {
            // A platform may await main-thread layout work without retaining
            // its native ledger mutex. GUI Stop cannot await controller state.
            std::unique_lock<std::mutex> lock(gateMutex);
            waitingForGui = true;
            gate.notify_all();
            gate.wait(lock, [&] { return guiMayReturn; });
        };
        std::thread worker([&] { fixture.controller.perform("owner", request); });
        {
            std::unique_lock<std::mutex> lock(gateMutex);
            CHECK(gate.wait_for(lock, std::chrono::seconds(2), [&] { return waitingForGui; }));
        }
        std::thread guiStop([&] {
            canceled = true;
            fixture.controller.releaseInput("owner", "turn-current");
            std::lock_guard<std::mutex> lock(gateMutex);
            releaseDone = true;
            gate.notify_all();
        });
        bool completedWithoutGuiDispatch = false;
        {
            std::unique_lock<std::mutex> lock(gateMutex);
            completedWithoutGuiDispatch =
                gate.wait_for(lock, std::chrono::milliseconds(500), [&] { return releaseDone; });
            guiMayReturn = true;  // Always unwind both threads on a failed baseline.
        }
        gate.notify_all();
        worker.join();
        guiStop.join();
        CHECK(completedWithoutGuiDispatch);
        CHECK(native->state().empty());
    }
}

void verifyDisplayObservation() {
    Fixture fixture;
    fixture.backend->visible.clear();
    const auto displays = fixture.controller.displays();
    CHECK(displays);
    if (displays) {
        CHECK(displays.value().size() == 2);
        if (displays.value().size() == 2) {
            CHECK(displays.value()[0].id == 11);
            CHECK(displays.value()[0].primary);
            CHECK(displays.value()[1].id == 22);
            CHECK(displays.value()[1].x == -1280);
            CHECK(displays.value()[1].y == -120);
            CHECK(displays.value()[1].width == 1280);
            CHECK(displays.value()[1].height == 1024);
        }
    }
    const auto observed =
        fixture.controller.observeDisplay("owner", fixture.workspace.path().toUtf8().toStdString(), 22);
    CHECK(observed);
    if (!observed) return;
    const Json output = Json::parse(observed.value().json, nullptr, false);
    CHECK(output.is_object());
    if (!output.is_object()) return;
    CHECK(output.value("target", std::string()) == "display");
    CHECK(output.value("display_id", std::string()) == "22");
    CHECK(!output.value("snapshot_id", std::string()).empty());
    CHECK(output["bounds"]["x"] == -1280);
    CHECK(output["bounds"]["y"] == -120);
    CHECK(output["bounds"]["width"] == 1280);
    CHECK(output["bounds"]["height"] == 1024);
    CHECK(output["image_width"] == 320);
    CHECK(output["image_height"] == 256);
    CHECK(output["frame_sequence"] == 41);
    CHECK(output["frame_received_at_ms"] == 1700000000001LL);
    CHECK(output.value("has_image", false));
    CHECK(output.value("mime_type", std::string()) == "image/png");
    CHECK(!output.contains("window_id"));
    CHECK(QImage(QString::fromStdString(observed.value().imagePath)).size() == QSize(320, 256));
    const QString relative = QString::fromStdString(output.value("image_path", std::string()));
    CHECK(!relative.isEmpty());
    CHECK(QDir::isRelativePath(relative));
    CHECK(QFileInfo(fixture.workspace.filePath(relative)).canonicalFilePath() ==
          QFileInfo(QString::fromStdString(observed.value().imagePath)).canonicalFilePath());
    CHECK(fixture.backend->capturedDisplayId == 22);
    CHECK(fixture.backend->displayCaptureCount == 1);
    CHECK(fixture.backend->captureCount == 0);
    CHECK(fixture.backend->activationCount == 0);
    CHECK(fixture.backend->inputs.empty());
}

void verifyDisplayDispatchAndNextObservation() {
    const std::pair<const char*, DesktopNativeInputKind> gestures[] = {{"move", DesktopNativeInputKind::Move},
                                                                       {"click", DesktopNativeInputKind::Click},
                                                                       {"key", DesktopNativeInputKind::Key}};
    for (const auto& gesture : gestures) {
        Fixture fixture;
        fixture.backend->visible.clear();
        fixture.backend->focused = false;
        auto action = fixture.observeDisplay(gesture.first);
        if (action.snapshotId.empty()) continue;
        action.key = "Meta+A";
        // Foreground changes are an expected consequence of desktop input.
        // No selected application identity may be imposed on this path.
        fixture.backend->onInput = [&fixture] { fixture.backend->focused = false; };
        fixture.backend->events.clear();
        const auto result = fixture.controller.perform("owner", action);
        CHECK(result);
        if (!result) continue;
        CHECK(result.value().dispatched);
        CHECK(!result.value().dryRun);
        CHECK(!result.value().verified);
        CHECK(result.value().state == "dispatched");
        CHECK(result.value().observationError.empty());
        CHECK(!result.value().observation.json.empty());
        CHECK(!result.value().observation.imagePath.empty());
        const Json next = Json::parse(result.value().observation.json, nullptr, false);
        CHECK(next.is_object());
        if (!next.is_object()) continue;
        CHECK(next.value("target", std::string()) == "display");
        CHECK(next.value("display_id", std::string()) == "22");
        CHECK(next["frame_sequence"] == fixture.backend->lastProducedFrameSequence);
        CHECK(next["frame_received_at_ms"] == fixture.backend->lastProducedFrameReceivedAtMs);
        const std::string nextToken = next.value("snapshot_id", std::string());
        CHECK(!nextToken.empty());
        CHECK(nextToken != action.snapshotId);
        CHECK(QImage(QString::fromStdString(result.value().observation.imagePath)).size() == QSize(320, 256));
        CHECK(fixture.backend->displayCaptureCount == 2);
        CHECK(fixture.backend->inputs.size() == 1);
        if (!fixture.backend->inputs.empty()) {
            const auto& input = fixture.backend->inputs.front();
            CHECK(input.kind == gesture.second);
            CHECK(input.desktopScope);
            CHECK(input.display.id == 22);
            CHECK(input.display.x == -1280);
            CHECK(input.expectedWindowId == 0);
            CHECK(input.expectedProcessId == 0);
            if (gesture.second != DesktopNativeInputKind::Key) {
                CHECK(std::abs(input.x - (-960.25)) < 0.01);
                CHECK(std::abs(input.y - 391.5) < 0.01);
            } else {
                CHECK(input.key == "Meta+A");
            }
        }
        CHECK(fixture.backend->activationCount == 0);
        CHECK(fixture.backend->readCount == 0);
        CHECK(std::find(fixture.backend->events.begin(), fixture.backend->events.end(), "windows") ==
              fixture.backend->events.end());
        CHECK(std::find(fixture.backend->events.begin(), fixture.backend->events.end(), "isFocused") ==
              fixture.backend->events.end());
        const auto replay = fixture.controller.perform("owner", action);
        CHECK(!replay);
        CHECK(fixture.backend->inputs.size() == 1);
        // The observation returned with the receipt is immediately actionable,
        // without a second independent screenshot/tool round trip.
        action.snapshotId = nextToken;
        CHECK(fixture.controller.perform("owner", action));
        CHECK(fixture.backend->inputs.size() == 2);
    }
}

void verifyDisplayIdentityAndGeometry() {
    for (int changed = 0; changed < 5; ++changed) {
        Fixture fixture;
        auto action = fixture.observeDisplay();
        if (action.snapshotId.empty()) continue;
        auto& display = fixture.backend->availableDisplays[1];
        if (changed == 0)
            ++display.id;
        else if (changed == 1)
            display.x += 20;
        else if (changed == 2)
            display.y += 20;
        else if (changed == 3)
            display.width += 20;
        else
            display.height += 20;
        CHECK(!fixture.controller.perform("owner", action));
        CHECK(fixture.backend->inputs.empty());
        CHECK(fixture.backend->activationCount == 0);
    }
    Fixture fixture;
    auto action = fixture.observeDisplay();
    if (action.snapshotId.empty()) return;
    CHECK(!fixture.controller.perform("foreign-session", action));
    auto wrongDisplay = action;
    wrongDisplay.displayId = 11;
    CHECK(!fixture.controller.perform("owner", wrongDisplay));
    auto wrongScope = action;
    wrongScope.desktopScope = false;
    CHECK(!fixture.controller.perform("owner", wrongScope));
    auto invalidPosition = action;
    invalidPosition.xRatio = 1.5;
    CHECK(!fixture.controller.perform("owner", invalidPosition));
    CHECK(fixture.backend->inputs.empty());
    // Rejected validation must not spend a valid observation token.
    CHECK(fixture.controller.perform("owner", action));
    CHECK(fixture.backend->inputs.size() == 1);
}

void verifyDisplayCaptureFailureRetainsDispatchReceipt() {
    Fixture fixture;
    auto action = fixture.observeDisplay();
    if (action.snapshotId.empty()) return;
    fixture.backend->displayCaptureError = {MaiErrorCode::NotSupported, "fixture screen capture unavailable"};
    const auto result = fixture.controller.perform("owner", action);
    // A screen refresh failure cannot turn an already injected click into a
    // retryable action failure and thereby invite a second click.
    CHECK(result);
    if (result) {
        CHECK(result.value().dispatched);
        CHECK(result.value().state == "dispatched_observation_failed");
        CHECK(result.value().observation.json.empty());
        CHECK(result.value().observation.imagePath.empty());
        CHECK(result.value().observationError.find("fixture screen capture unavailable") != std::string::npos);
    }
    CHECK(fixture.backend->inputs.size() == 1);
    CHECK(fixture.backend->displayCaptureCount == 2);
    CHECK(!fixture.controller.perform("owner", action));
    CHECK(fixture.backend->inputs.size() == 1);
    fixture.backend->displayCaptureError = {};
    CHECK(fixture.controller.observeDisplay("owner", fixture.workspace.path().toUtf8().toStdString(), 22));
}

void verifyDisplayCancellationAndSecureInput() {
    {
        Fixture fixture;
        const auto canceled = fixture.controller.observeDisplay(
            "owner", fixture.workspace.path().toUtf8().toStdString(), 22, [] { return true; });
        CHECK(!canceled);
        if (!canceled) CHECK(canceled.error().code() == MaiErrorCode::Canceled);
        CHECK(fixture.backend->displayCaptureCount == 0);
    }
    {
        Fixture fixture;
        std::atomic<bool> canceled{false};
        fixture.backend->onDisplayCapture = [&canceled] { canceled = true; };
        const auto observed = fixture.controller.observeDisplay(
            "owner", fixture.workspace.path().toUtf8().toStdString(), 22, [&canceled] { return canceled.load(); });
        CHECK(!observed);
        if (!observed) CHECK(observed.error().code() == MaiErrorCode::Canceled);
        CHECK(fixture.backend->displayCaptureCount == 1);
        CHECK(QDir(fixture.workspace.filePath(QStringLiteral("computer"))).entryList(QDir::Files).isEmpty());
    }
    {
        Fixture fixture;
        auto action = fixture.observeDisplay();
        if (!action.snapshotId.empty()) {
            action.isCanceled = [] { return true; };
            const auto result = fixture.controller.perform("owner", action);
            CHECK(!result);
            if (!result) CHECK(result.error().code() == MaiErrorCode::Canceled);
            CHECK(fixture.backend->inputs.empty());
            CHECK(fixture.backend->activationCount == 0);
        }
    }
    for (const char* kind : {"type", "replace_text", "key"}) {
        Fixture fixture;
        auto action = fixture.observeDisplay(kind);
        if (action.snapshotId.empty()) continue;
        action.text = "owned fixture text";
        action.key = "Ctrl+V";
        fixture.backend->secure = true;
        const auto result = fixture.controller.perform("owner", action);
        CHECK(!result);
        if (!result) CHECK(result.error().code() == MaiErrorCode::NotSupported);
        CHECK(fixture.backend->inputs.empty());
        CHECK(fixture.backend->activationCount == 0);
    }
}

void verifyDisplayDryRunAndClear() {
    Fixture fixture;
    auto action = fixture.observeDisplay();
    if (action.snapshotId.empty()) return;
    action.dryRun = true;
    const auto planned = fixture.controller.perform("owner", action);
    CHECK(planned);
    if (planned) {
        CHECK(planned.value().dryRun);
        CHECK(!planned.value().dispatched);
        CHECK(planned.value().observation.imagePath.empty());
    }
    CHECK(fixture.backend->inputs.empty());
    CHECK(fixture.backend->displayCaptureCount == 1);
    action.dryRun = false;
    CHECK(fixture.controller.perform("owner", action));
    auto cleared = fixture.observeDisplay();
    CHECK(!fixture.controller.clear("owner"));
    CHECK(!fixture.controller.perform("owner", cleared));
}

void verifyDesktopLauncherWithoutFocusedTextControl() {
#if defined(Q_OS_MAC)
    const char* launchKeys[] = {"Cmd+Space"};
#else
    const char* launchKeys[] = {"Meta+R", "Meta+E", "Meta+D", "Meta+S"};
#endif
    // A launcher/shell may not expose any focused editable AX/UIA element.
    // Opening it must not be confused with typing into an unverified control.
    for (const char* key : launchKeys) {
        Fixture fixture;
        auto action = fixture.observeDisplay("key");
        action.key = key;
        fixture.backend->secureInputError = {MaiErrorCode::NotConfigured, "no focused text control"};
        const auto launched = fixture.controller.perform("owner", action);
        CHECK(launched);
        CHECK(fixture.backend->inputs.size() == 1);
        CHECK(std::find(fixture.backend->events.begin(), fixture.backend->events.end(), "secureInput") ==
              fixture.backend->events.end());
    }
    // Arbitrary characters, editing and paste remain subject to the original
    // focused-input check; the launcher exception must not become a bypass.
    for (const char* key : {"A", "Space", "Ctrl+V", "Delete", "Meta+V"}) {
        Fixture fixture;
        auto action = fixture.observeDisplay("key");
        action.key = key;
        fixture.backend->secureInputError = {MaiErrorCode::NotConfigured, "no focused text control"};
        const auto typed = fixture.controller.perform("owner", action);
        CHECK(!typed);
        CHECK(fixture.backend->inputs.empty());
    }
}

void verifyDisplayPostActionRequiresFreshNativeFrame() {
    Fixture fixture;
    auto action = fixture.observeDisplay();
    if (action.snapshotId.empty()) return;
    std::int64_t inputStartedAtMs = 0;
    fixture.backend->onInput = [&inputStartedAtMs] { inputStartedAtMs = fixtureClockMs(); };
    action.isCanceled = [] { return false; };
    const auto result = fixture.controller.perform("owner", action);
    CHECK(result);
    if (!result) return;
    CHECK(result.value().dispatched);
    CHECK(result.value().state == "dispatched");
    CHECK(fixture.backend->captureOptionsCount == 2);
    CHECK(fixture.backend->lastAfterFrameSequence >= 41);
    CHECK(fixture.backend->lastAfterFrameReceivedAtMs >= inputStartedAtMs);
    CHECK(fixture.backend->lastAfterFrameReceivedAtMs > 0);
    CHECK(fixture.backend->lastCaptureHadCancel);
    const Json next = Json::parse(result.value().observation.json, nullptr, false);
    CHECK(next.is_object());
    if (!next.is_object()) return;
    CHECK(next["frame_sequence"] == fixture.backend->lastProducedFrameSequence);
    CHECK(next["frame_received_at_ms"] == fixture.backend->lastProducedFrameReceivedAtMs);
    CHECK(fixture.backend->lastProducedFrameSequence > fixture.backend->lastAfterFrameSequence);
    CHECK(fixture.backend->lastProducedFrameReceivedAtMs >= fixture.backend->lastAfterFrameReceivedAtMs);
    CHECK(fixture.backend->inputs.size() == 1);
}

void verifyDisplayStaleNativeFrameCannotIssueActionableToken() {
    Fixture fixture;
    auto action = fixture.observeDisplay();
    if (action.snapshotId.empty()) return;
    const QString directory = fixture.workspace.filePath(QStringLiteral("computer"));
    const auto originalFiles = QDir(directory).entryList(QDir::Files);
    fixture.backend->returnStaleDisplayFrame = true;
    const auto result = fixture.controller.perform("owner", action);
    CHECK(result);
    if (!result) return;
    CHECK(result.value().dispatched);
    CHECK(result.value().state == "dispatched_observation_failed");
    CHECK(!result.value().observationError.empty());
    CHECK(result.value().observation.json.empty());
    CHECK(result.value().observation.imagePath.empty());
    CHECK(QDir(directory).entryList(QDir::Files) == originalFiles);
    CHECK(!fixture.controller.perform("owner", action));
    CHECK(fixture.backend->inputs.size() == 1);
    // A faulty implementation may return a fresh UUID for the old frame.
    // Such a UUID must not authorize a second action either.
    if (!result.value().observation.json.empty()) {
        const Json stale = Json::parse(result.value().observation.json, nullptr, false);
        if (stale.is_object()) {
            auto replay = action;
            replay.snapshotId = stale.value("snapshot_id", std::string());
            if (!replay.snapshotId.empty()) CHECK(!fixture.controller.perform("owner", replay));
        }
    }
    CHECK(fixture.backend->inputs.size() == 1);
}

void verifyDisplayCaptureCancellationReachesSynchronousBackend() {
    Fixture fixture;
    std::atomic<bool> canceled{false};
    bool captureEntered = false;
    bool backendObservedCancellation = false;
    fixture.backend->onDisplayCaptureOptions = [&](const DesktopComputerBackend::CaptureOptions& options) {
        captureEntered = true;
        CHECK(bool(options.isCanceled));
        canceled = true;
        backendObservedCancellation = options.isCanceled && options.isCanceled();
        return backendObservedCancellation
                   ? MaiError{MaiErrorCode::Canceled, "fixture canceled inside synchronous capture"}
                   : MaiError{MaiErrorCode::Internal, "capture did not receive the caller cancellation"};
    };
    const auto observed = fixture.controller.observeDisplay("owner", fixture.workspace.path().toUtf8().toStdString(),
                                                            22, [&canceled] { return canceled.load(); });
    CHECK(!observed);
    if (!observed) CHECK(observed.error().code() == MaiErrorCode::Canceled);
    CHECK(captureEntered);
    CHECK(backendObservedCancellation);
    CHECK(fixture.backend->captureOptionsCount == 1);
    CHECK(fixture.backend->displayCaptureCount == 0);
    CHECK(fixture.backend->inputs.empty());
    CHECK(QDir(fixture.workspace.filePath(QStringLiteral("computer"))).entryList(QDir::Files).isEmpty());
}

void verifyDisplayObservationFreshnessThresholdsAreIndependent() {
    // Both sequence and receive time matter. A newer sequence alone cannot
    // justify a pre-action image, and an encoding time cannot replace either.
    for (int dimension = 0; dimension < 2; ++dimension) {
        Fixture fixture;
        const auto observed =
            fixture.controller.observeDisplay("owner", fixture.workspace.path().toUtf8().toStdString(), 22, {},
                                              dimension == 0 ? 5000 : 0, dimension == 1 ? fixtureClockMs() + 60000 : 0);
        CHECK(!observed);
        CHECK(fixture.backend->captureOptionsCount == 1);
        CHECK(fixture.backend->inputs.empty());
        CHECK(QDir(fixture.workspace.filePath(QStringLiteral("computer"))).entryList(QDir::Files).isEmpty());
    }
}

void verifyDisplayOrdinaryWorkspaceCannotChangeAfterObservation() {
    Fixture fixture;
    QTemporaryDir differentWorkspace;
    CHECK(differentWorkspace.isValid());
    auto action = fixture.observeDisplay();
    if (action.snapshotId.empty()) return;
    const auto beforeFiles = QDir(fixture.workspace.filePath(QStringLiteral("computer"))).entryList(QDir::Files);
    action.workspace = differentWorkspace.path().toUtf8().toStdString();
    const auto result = fixture.controller.perform("owner", action);
    CHECK(!result);
    if (!result) CHECK(result.error().code() == MaiErrorCode::InvalidInput);
    CHECK(fixture.backend->inputs.empty());
    CHECK(fixture.backend->displayCaptureCount == 1);
    CHECK(QDir(fixture.workspace.filePath(QStringLiteral("computer"))).entryList(QDir::Files) == beforeFiles);
    CHECK(!QFileInfo::exists(differentWorkspace.filePath(QStringLiteral("computer"))));
}

void verifyDisplayInputInvalidatesOtherSessionObservation() {
    // Distinct controllers still operate one physical desktop. Serializing
    // individual input calls must not authorize B against pixels from before A.
    Fixture first;
    Fixture second;
    auto firstAction = first.observeDisplay("click", "first-session");
    auto secondAction = second.observeDisplay("click", "second-session");
    if (firstAction.snapshotId.empty() || secondAction.snapshotId.empty()) return;
    const auto firstResult = first.controller.perform("first-session", firstAction);
    CHECK(firstResult);
    if (!firstResult) return;
    CHECK(firstResult.value().dispatched);
    CHECK(first.backend->inputs.size() == 1);

    const int secondCaptures = second.backend->displayCaptureCount;
    const int secondCaptureOptions = second.backend->captureOptionsCount;
    const QString secondDirectory = second.workspace.filePath(QStringLiteral("computer"));
    const auto secondFiles = QDir(secondDirectory).entryList(QDir::Files);
    const auto stale = second.controller.perform("second-session", secondAction);
    CHECK(!stale);
    CHECK(second.backend->inputs.empty());
    CHECK(second.backend->displayCaptureCount == secondCaptures);
    CHECK(second.backend->captureOptionsCount == secondCaptureOptions);
    CHECK(QDir(secondDirectory).entryList(QDir::Files) == secondFiles);

    // A new observation after A's input restores B's ability to decide and act.
    secondAction = second.observeDisplay("click", "second-session");
    if (secondAction.snapshotId.empty()) return;
    const auto refreshed = second.controller.perform("second-session", secondAction);
    CHECK(refreshed);
    if (refreshed) CHECK(refreshed.value().dispatched);
    CHECK(second.backend->inputs.size() == 1);
}

void verifyDisplayInputDuringInitialCaptureCannotPublishObservation() {
    Fixture actor;
    const auto action = actor.observeDisplay("click", "actor-session");
    if (action.snapshotId.empty()) return;
    Fixture observer;
    bool inputDuringCapture = false;
    // This is an initial observation, outside perform's desktop operation lock.
    // The synchronous fake callback models another Session changing the screen
    // while capture is in flight; no native input or recursive post-input lock
    // acquisition is involved.
    observer.backend->onDisplayCapture = [&] {
        inputDuringCapture = true;
        const auto changed = actor.controller.perform("actor-session", action);
        CHECK(changed);
        if (changed) CHECK(changed.value().dispatched);
    };
    const auto observed =
        observer.controller.observeDisplay("observer-session", observer.workspace.path().toUtf8().toStdString(), 22);
    CHECK(inputDuringCapture);
    CHECK(actor.backend->inputs.size() == 1);
    CHECK(observer.backend->displayCaptureCount == 1);
    CHECK(observer.backend->inputs.empty());
    CHECK(!observed);
    CHECK(QDir(observer.workspace.filePath(QStringLiteral("computer"))).entryList(QDir::Files).isEmpty());
}

void verifyWindowActionInvalidatesDisplayObservation() {
    // Window focus alone can change the displayed UI, so both activation-only
    // and window input attempts invalidate another Session's screen snapshot.
    for (const char* kind : {"focus", "click"}) {
        Fixture observer;
        const auto displayAction = observer.observeDisplay("click", "display-observer");
        if (displayAction.snapshotId.empty()) continue;
        Fixture actor;
        const auto windowAction = actor.observe(kind);
        if (windowAction.snapshotId.empty()) continue;
        const auto changed = actor.controller.perform("owner", windowAction);
        CHECK(changed);
        if (!changed) continue;
        CHECK(changed.value().dispatched);
        CHECK(actor.backend->activationCount == 1);
        CHECK(actor.backend->inputs.size() == (std::string(kind) == "focus" ? 0u : 1u));

        const int captures = observer.backend->displayCaptureCount;
        const int captureOptions = observer.backend->captureOptionsCount;
        const QString directory = observer.workspace.filePath(QStringLiteral("computer"));
        const auto files = QDir(directory).entryList(QDir::Files);
        const auto stale = observer.controller.perform("display-observer", displayAction);
        CHECK(!stale);
        CHECK(observer.backend->inputs.empty());
        CHECK(observer.backend->displayCaptureCount == captures);
        CHECK(observer.backend->captureOptionsCount == captureOptions);
        CHECK(QDir(directory).entryList(QDir::Files) == files);
    }
}

void verifyDisplayObservationRejectsAlreadyInFlightInput() {
    Fixture actor;
    const auto action = actor.observeDisplay("click", "in-flight-actor");
    if (action.snapshotId.empty()) return;
    Fixture observer;
    bool observeDuringInput = false;
    // Only initial observation is called from this synchronous fake input hook.
    // It must reject the in-flight generation before capture; the callback never
    // performs a second input or recursively enters perform's desktop lock.
    actor.backend->onInput = [&] {
        observeDuringInput = true;
        const auto observed = observer.controller.observeDisplay("in-flight-observer",
                                                                 observer.workspace.path().toUtf8().toStdString(), 22);
        CHECK(!observed);
        CHECK(observer.backend->captureOptionsCount == 0);
        CHECK(observer.backend->displayCaptureCount == 0);
        CHECK(observer.backend->inputs.empty());
        CHECK(QDir(observer.workspace.filePath(QStringLiteral("computer"))).entryList(QDir::Files).isEmpty());
    };
    const auto changed = actor.controller.perform("in-flight-actor", action);
    CHECK(observeDuringInput);
    CHECK(changed);
    if (changed) {
        CHECK(changed.value().dispatched);
        // The actor's own post-input observation occurs after the scope ends.
        CHECK(changed.value().state == "dispatched");
        CHECK(!changed.value().observation.json.empty());
    }
    CHECK(actor.backend->inputs.size() == 1);
}

struct DisplayToolFixture {
    std::shared_ptr<RecordingBackend> backend = std::make_shared<RecordingBackend>();
    std::shared_ptr<DesktopComputerController> controller = std::make_shared<DesktopComputerController>(backend);
    MaiToolRegistry registry;
    QTemporaryDir workspace;
    MaiToolContext context;
    MaiTool* tool = nullptr;

    DisplayToolFixture() {
        CHECK(workspace.isValid());
        context.sessionId = "display-tool-owner";
        context.root = workspace.path().toUtf8().toStdString();
        context.model = "deepseek-flash";
        registerDesktopComputerTools(registry, controller);
        tool = registry.find("desktop_computer");
        CHECK(tool);
    }
};

// Receiver selection is independent of the display coordinate port. These are
// fixture-owned windows, including a negative-origin display, never OS targets.
struct ExecutionWindowScene {
    DesktopNativeDisplay display;
    DesktopNativeWindow window;
    double outsideX;
    double outsideY;
};

std::vector<ExecutionWindowScene> executionWindowScenes() {
    return {
        {{11, 0, 0, 1920, 1080, true},
         {101, 5001, "Owned document", "Contract fixture", 320, 115, 1454, 800},
         .568,
         .8712},
        {{22, -1280, -120, 1280, 1024, false},
         {202, 5002, "Owned palette", "Contract fixture", -1000, 80, 600, 500},
         .1,
         .9},
        {{33, 0, 0, 1600, 900, false}, {303, 5003, "Owned console", "Contract fixture", 900, 350, 600, 500}, .1, .1}};
}

void useExecutionWindowScene(RecordingBackend& backend, const ExecutionWindowScene& scene) {
    backend.visible = {scene.window};
    backend.availableDisplays = {scene.display};
}

DesktopComputerAction observeExecutionWindow(Fixture& fixture, const std::string& kind, std::uint64_t receiver) {
    DesktopComputerAction action;
    action.action = kind;
    action.desktopScope = true;
    action.displayId = fixture.backend->availableDisplays.front().id;
    action.workspace = fixture.workspace.path().toUtf8().toStdString();
    action.turnId = "execution-window-turn";
    const auto& display = fixture.backend->availableDisplays.front();
    const auto& window = fixture.backend->visible.front();
    action.xRatio = (window.x + window.width / 2 - display.x) / (display.width - 1);
    action.yRatio = (window.y + window.height / 2 - display.y) / (display.height - 1);
    action.toXRatio = action.xRatio;
    action.toYRatio = action.yRatio;
    const auto observed = fixture.controller.observeDisplay("owner", action.workspace, action.displayId, {}, 0, 0, 0,
                                                            action.turnId, 0, receiver);
    CHECK(observed);
    if (observed) action.snapshotId = Json::parse(observed.value().json).value("snapshot_id", std::string());
    CHECK(!action.snapshotId.empty());
    // The ordinary request deliberately leaves executionWindowId empty. The
    // controller must enforce the receiver bound to its observation, not trust it.
    return action;
}

void checkPointOutsideExecutionWindow(const MaiError& error) {
    CHECK(error.code() == MaiErrorCode::InvalidInput);
    CHECK(error.message().find("point_outside_target") != std::string::npos);
    CHECK(error.message().find("bounds") != std::string::npos);
    CHECK(error.message().find("requested") != std::string::npos);
}

void verifyDisplayExecutionWindowBoundsAndCompatibility() {
    for (const auto& scene : executionWindowScenes()) {
        for (const char* kind : {"click", "move", "scroll", "drag"}) {
            for (const bool constrained : {false, true}) {
                Fixture fixture;
                useExecutionWindowScene(*fixture.backend, scene);
                auto action = observeExecutionWindow(fixture, kind, constrained ? scene.window.id : 0);
                if (action.snapshotId.empty()) continue;
                action.scrollLines = 1;
                if (std::string(kind) == "drag") {
                    action.toXRatio = scene.outsideX;
                    action.toYRatio = scene.outsideY;
                } else {
                    action.xRatio = scene.outsideX;
                    action.yRatio = scene.outsideY;
                }
                const auto result = fixture.controller.perform("owner", action);
                if (constrained) {
                    CHECK(!result);
                    if (!result) checkPointOutsideExecutionWindow(result.error());
                    CHECK(fixture.backend->inputs.empty());
                    if (fixture.backend->lastInputSession) {
                        CHECK(fixture.backend->lastInputSession->inputCalls() == 0);
                        CHECK(fixture.backend->lastInputSession->batchCalls() == 0);
                    }
                } else {
                    CHECK(result && result.value().dispatched);
                    CHECK(fixture.backend->inputs.size() == 1);
                    if (!fixture.backend->inputs.empty()) {
                        const auto& input = fixture.backend->inputs.back();
                        CHECK(input.desktopScope && !input.executionWindow);
                        const double x = std::string(kind) == "drag" ? input.toX : input.x;
                        const double y = std::string(kind) == "drag" ? input.toY : input.y;
                        CHECK(std::abs(x - (scene.display.x + scene.outsideX * (scene.display.width - 1))) < .001);
                        CHECK(std::abs(y - (scene.display.y + scene.outsideY * (scene.display.height - 1))) < .001);
                    }
                }
                CHECK(fixture.backend->activationCount == 0);
            }
        }
        Fixture fixture;
        useExecutionWindowScene(*fixture.backend, scene);
        fixture.backend->statefulInput = true;
        auto batch = observeExecutionWindow(fixture, "input_events", scene.window.id);
        if (batch.snapshotId.empty()) continue;
        DesktopInputEvent move;
        move.kind = DesktopInputEventKind::AbsoluteMove;
        move.x = scene.outsideX;
        move.y = scene.outsideY;
        batch.events = {move};
        const auto outside = fixture.controller.perform("owner", batch);
        CHECK(!outside);
        if (!outside) checkPointOutsideExecutionWindow(outside.error());
        if (fixture.backend->lastInputSession) CHECK(fixture.backend->lastInputSession->batchCalls() == 0);
        CHECK(fixture.backend->activationCount == 0);

        auto inside = observeExecutionWindow(fixture, "input_events", scene.window.id);
        move.x = inside.xRatio;
        move.y = inside.yRatio;
        inside.events = {move};
        const auto accepted = fixture.controller.perform("owner", inside);
        CHECK(accepted && accepted.value().dispatched);
        const auto native = fixture.backend->lastInputSession;
        CHECK(native && native->scope().executionWindow);
        if (native) {
            if (native->scope().executionWindow) CHECK(native->scope().executionWindow->id == scene.window.id);
            CHECK(native->port().x == scene.display.x && native->port().y == scene.display.y);
            CHECK(native->port().width == scene.display.width && native->port().height == scene.display.height);
        }
    }
}

void verifyDisplayExecutionWindowBindingAndSuccessor() {
    for (const char* kind : {"click", "key", "type", "replace_text"}) {
        Fixture fixture;
        const auto scene = executionWindowScenes().front();
        useExecutionWindowScene(*fixture.backend, scene);
        auto action = observeExecutionWindow(fixture, kind, scene.window.id);
        if (action.snapshotId.empty()) continue;
        action.key = "Ctrl+V";
        action.text = "Owned receiver text";
        action.executionWindowId = 999;  // Cannot override the snapshot-owned receiver.
        action.executionWindowProcessId = 999;
        const auto acted = fixture.controller.perform("owner", action);
        CHECK(acted && acted.value().dispatched);
        CHECK(fixture.backend->activationCount == 0);
        CHECK(fixture.backend->inputs.size() == 1);
        if (!fixture.backend->inputs.empty()) {
            const auto& input = fixture.backend->inputs.back();
            CHECK(input.desktopScope && input.executionWindow);
            if (input.executionWindow) {
                CHECK(input.executionWindow->id == scene.window.id);
                CHECK(input.executionWindow->processId == scene.window.processId);
                CHECK(input.executionWindow->x == scene.window.x && input.executionWindow->y == scene.window.y);
            }
        }
        if (!acted || acted.value().observation.json.empty()) continue;
        const auto next = Json::parse(acted.value().observation.json);
        auto successor = action;
        successor.action = "click";
        successor.snapshotId = next.value("snapshot_id", std::string());
        successor.xRatio = scene.outsideX;
        successor.yRatio = scene.outsideY;
        CHECK(!successor.snapshotId.empty() && successor.snapshotId != action.snapshotId);
        const auto rejected = fixture.controller.perform("owner", successor);
        CHECK(!rejected);
        if (!rejected) checkPointOutsideExecutionWindow(rejected.error());
        CHECK(fixture.backend->inputs.size() == 1);
    }
    for (int change = 0; change < 9; ++change) {
        Fixture fixture;
        const auto scene = executionWindowScenes().front();
        useExecutionWindowScene(*fixture.backend, scene);
        auto action = observeExecutionWindow(fixture, "click", scene.window.id);
        if (action.snapshotId.empty()) continue;
        auto& window = fixture.backend->visible.front();
        if (change == 0) ++window.processId;
        if (change == 1) ++window.id;
        if (change == 2) window.x += 10;
        if (change == 3) window.height += 10;
        if (change == 4) window.title += " changed";
        if (change == 5) window.minimized = true;
        if (change == 6) window.cloaked = true;
        if (change == 7) window.boundsAvailable = false;
        if (change == 8) fixture.backend->visible.clear();
        CHECK(!fixture.controller.perform("owner", action));
        CHECK(fixture.backend->inputs.empty() && fixture.backend->activationCount == 0);
    }
    for (const char* kind : {"key", "key_down", "type", "replace_text", "input_events"}) {
        Fixture fixture;
        const auto scene = executionWindowScenes().front();
        useExecutionWindowScene(*fixture.backend, scene);
        fixture.backend->statefulInput = true;
        fixture.backend->focused = false;
        auto action = observeExecutionWindow(fixture, kind, scene.window.id);
        action.key = std::string(kind) == "key_down" ? "Ctrl" : "Ctrl+V";
        action.text = "Do not inject into another receiver";
        if (std::string(kind) == "input_events") {
            DesktopInputEvent text;
            text.kind = DesktopInputEventKind::UnicodeText;
            text.text = "Owned text";
            action.events = {text};
        }
        const auto rejected = fixture.controller.perform("owner", action);
        CHECK(!rejected);
        if (!rejected) CHECK(rejected.error().code() == MaiErrorCode::Busy);
        CHECK(fixture.backend->activationCount == 0);
        if (fixture.backend->lastInputSession) {
            CHECK(fixture.backend->lastInputSession->inputCalls() == 0);
            CHECK(fixture.backend->lastInputSession->batchCalls() == 0);
        }
    }
}

void verifyDisplayExecutionWindowHeldTargetAndRelease() {
    for (const std::uint64_t nextReceiver : {std::uint64_t(0), std::uint64_t(202), std::uint64_t(101)}) {
        Fixture fixture;
        const auto scene = executionWindowScenes().front();
        useExecutionWindowScene(*fixture.backend, scene);
        fixture.backend->visible.push_back({202, 6002, "Owned alternate", "Contract fixture", 600, 180, 900, 700});
        fixture.backend->statefulInput = true;
        auto down = observeExecutionWindow(fixture, "key_down", scene.window.id);
        down.key = "Ctrl";
        CHECK(fixture.controller.perform("owner", down));
        const auto native = fixture.backend->lastInputSession;
        CHECK(native && native->state().keysDown == std::vector<std::string>{"Ctrl"});
        if (!native) continue;
        if (nextReceiver == 101) ++fixture.backend->visible.front().processId;
        auto changed = observeExecutionWindow(fixture, "key", nextReceiver);
        changed.key = "Ctrl+V";
        const auto before = native->inputCalls();
        const auto rejected = fixture.controller.perform("owner", changed);
        CHECK(!rejected);
        if (!rejected) CHECK(rejected.error().code() == MaiErrorCode::Busy);
        CHECK(native->inputCalls() == before);
        DesktopComputerAction up;
        up.action = "key_up";
        up.key = "Ctrl";
        up.turnId = down.turnId;
        CHECK(fixture.controller.perform("owner", up));
        CHECK(fixture.controller.inputState("owner").empty());
    }
    for (const bool pointer : {false, true}) {
        Fixture fixture;
        const auto scene = executionWindowScenes().front();
        useExecutionWindowScene(*fixture.backend, scene);
        fixture.backend->statefulInput = true;
        auto down = observeExecutionWindow(fixture, pointer ? "mouse_down" : "key_down", scene.window.id);
        down.key = "Ctrl";
        CHECK(fixture.controller.perform("owner", down));
        const auto native = fixture.backend->lastInputSession;
        CHECK(native && !native->state().empty());
        fixture.backend->visible.clear();
        fixture.backend->focused = false;
        DesktopComputerAction up;
        up.action = pointer ? "mouse_up" : "key_up";
        up.key = "Ctrl";
        up.turnId = down.turnId;
        CHECK(fixture.controller.perform("owner", up));
        CHECK(fixture.controller.inputState("owner").empty());
        CHECK(fixture.backend->activationCount == 0);
    }
}

void verifySuccessorCannotAdoptRecycledExecutionWindow() {
    for (const char* kind : {"click", "key_down"}) {
        for (int change = 0; change < 4; ++change) {
            Fixture fixture;
            const auto scene = executionWindowScenes().front();
            useExecutionWindowScene(*fixture.backend, scene);
            fixture.backend->statefulInput = true;
            auto action = observeExecutionWindow(fixture, kind, scene.window.id);
            action.key = "Ctrl";
            fixture.backend->onNativeInput = [&] {
                auto& receiver = fixture.backend->visible.front();
                if (change == 0) ++receiver.processId;
                if (change == 1) receiver.ownerId = 808;
                if (change == 2) receiver.className += " replacement";
                if (change == 3) receiver.application += " replacement";
            };
            const auto result = fixture.controller.perform("owner", action);
            CHECK(result && result.value().dispatched);
            if (result) {
                CHECK(result.value().state == "dispatched_observation_failed");
                CHECK(result.value().observation.json.empty() && result.value().observation.imagePath.empty());
                CHECK(result.value().observationError.find("execution window") != std::string::npos);
                CHECK(result.value().inputState.empty());
            }
            CHECK(fixture.controller.inputState("owner").empty());
            CHECK(fixture.backend->lastInputSession && fixture.backend->lastInputSession->inputCalls() == 1);
        }
    }
}

void verifyRevokedObservationCannotBeResurrectedBySuccessor() {
    Fixture fixture;
    const auto scene = executionWindowScenes().front();
    useExecutionWindowScene(*fixture.backend, scene);
    auto action = observeExecutionWindow(fixture, "click", scene.window.id);
    // The ordinary caller can revoke a Session without owning a cancel flag.
    // No key/button is held, so revocation need not change input generation.
    CHECK(!action.isCanceled);
    fixture.backend->onDisplayCapture = [&] {
        CHECK(!fixture.controller.releaseInput("owner", action.turnId));
    };
    const auto result = fixture.controller.perform("owner", action);
    CHECK(result && result.value().dispatched);
    if (result) {
        CHECK(result.value().state == "dispatched_observation_failed");
        CHECK(result.value().observation.json.empty() && result.value().observation.imagePath.empty());
        CHECK(!result.value().observationError.empty());
    }
    CHECK(fixture.controller.inputState("owner").empty());
    CHECK(fixture.backend->inputs.size() == 1);
}

void verifyDisplayExecutionWindowToolContract() {
    DisplayToolFixture fixture;
    if (!fixture.tool) return;
    const auto scene = executionWindowScenes().front();
    useExecutionWindowScene(*fixture.backend, scene);
    const Json observe{
        {"action", "observe"}, {"target", "display"}, {"display_id", "11"}, {"execution_window_id", "101"}};
    const auto observed = fixture.tool->execute(observe.dump(), fixture.context);
    CHECK(!observed.hasError());
    if (observed.hasError()) return;
    const auto output = Json::parse(observed.output());
    Json click{{"action", "click"},         {"target", "display"},
               {"display_id", "11"},        {"snapshot_id", output["snapshot_id"]},
               {"x_ratio", scene.outsideX}, {"y_ratio", scene.outsideY}};
    auto overrideReceiver = click;
    overrideReceiver["execution_window_id"] = "202";
    CHECK(fixture.tool->execute(overrideReceiver.dump(), fixture.context).error().code() == MaiErrorCode::InvalidInput);
    overrideReceiver = click;
    overrideReceiver["execution_window_process_id"] = "5001";
    CHECK(fixture.tool->execute(overrideReceiver.dump(), fixture.context).error().code() == MaiErrorCode::InvalidInput);
    const auto rejected = fixture.tool->execute(click.dump(), fixture.context);
    CHECK(rejected.hasError());
    if (rejected.hasError()) {
        CHECK(rejected.error().code() == MaiErrorCode::InvalidInput);
        const auto detail = Json::parse(rejected.error().message(), nullptr, false);
        CHECK(detail.is_object());
        if (detail.is_object()) {
            CHECK(detail.value("reason", std::string()) == "point_outside_target");
            CHECK(detail.contains("target_bounds") && detail.contains("requested_px"));
            CHECK(detail.contains("execution_window_id"));
        }
        CHECK(rejected.error().message().find("point_outside_target") != std::string::npos);
        CHECK(rejected.error().message().find("bounds") != std::string::npos);
        CHECK(rejected.error().message().find("requested") != std::string::npos);
    }
    CHECK(fixture.backend->inputs.empty() && fixture.backend->activationCount == 0);
    auto mixed = observe;
    mixed["target"] = "window";
    mixed["window_id"] = "101";
    mixed.erase("display_id");
    CHECK(fixture.tool->execute(mixed.dump(), fixture.context).error().code() == MaiErrorCode::InvalidInput);
}

void verifyWindowObservationImageOption() {
    DisplayToolFixture fixture;
    if (!fixture.tool) return;
    const Json metadata{{"action", "observe"}, {"target", "window"}, {"window_id", "101"}};
    for (const bool explicitFalse : {false, true}) {
        auto request = metadata;
        if (explicitFalse) request["include_image"] = false;
        const auto observed = fixture.tool->execute(request.dump(), fixture.context);
        CHECK(!observed.hasError());
        CHECK(observed.images().empty());
        if (!observed.hasError()) {
            const auto output = Json::parse(observed.output());
            CHECK(output["has_image"] == false && output["image_width"] == 0 && output["image_height"] == 0);
        }
    }
    CHECK(fixture.backend->captureCount == 0);
    auto image = metadata;
    image["include_image"] = true;
    const auto captured = fixture.tool->execute(image.dump(), fixture.context);
    CHECK(!captured.hasError());
    CHECK(captured.images().size() == 1);
        if (!captured.hasError()) {
            const auto picture = Json::parse(captured.output());
#if defined(Q_OS_WIN)
            CHECK(picture.value("image_coordinates_usable_for_input", false));
#else
            CHECK(!picture.value("image_coordinates_usable_for_input", true));
#endif
        }
    if (!captured.hasError()) {
        const auto output = Json::parse(captured.output());
        CHECK(output["has_image"] == true && output["image_width"] == 20 && output["image_height"] == 20);
        CHECK(output.value("snapshot_id", std::string()).size() > 0);
        CHECK(!output.value("image_path", std::string()).empty());
    }
    CHECK(fixture.backend->captureCount == 1 && fixture.backend->activationCount == 0);
    CHECK(fixture.backend->inputs.empty());
    for (const char* model : {"glm-5.3", "deepseek-v4-pro", "local:owned-text-model"}) {
        fixture.context.model = model;
        const auto textOnly = fixture.tool->execute(image.dump(), fixture.context);
        CHECK(textOnly.hasError() && textOnly.error().code() == MaiErrorCode::NotSupported);
        CHECK(textOnly.images().empty());
        CHECK(fixture.backend->captureCount == 1);
    }
}

void verifyExecutionWindowContracts() {
    verifyRevokedObservationCannotBeResurrectedBySuccessor();
    verifySuccessorCannotAdoptRecycledExecutionWindow();
    verifyDisplayExecutionWindowBoundsAndCompatibility();
    verifyDisplayExecutionWindowBindingAndSuccessor();
    verifyDisplayExecutionWindowHeldTargetAndRelease();
    verifyDisplayExecutionWindowToolContract();
    verifyWindowObservationImageOption();
}

void verifyDisplayToolWaitAndCancel() {
    {
        DisplayToolFixture fixture;
        const auto start = std::chrono::steady_clock::now();
        fixture.backend->onDisplayCapture = [&] {
            CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(60));
        };
        const auto image = fixture.tool->execute(
            R"({"action":"observe","target":"display","display_id":"22","wait_ms":60})", fixture.context);
        CHECK(!image.hasError());
        CHECK(fixture.backend->displayCaptureCount == 1);
        CHECK(fixture.backend->inputs.empty());
    }
    {
        DisplayToolFixture fixture;
        std::atomic<bool> canceled{false};
        fixture.context.cancel = &canceled;
        std::thread canceler([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            canceled = true;
        });
        const auto start = std::chrono::steady_clock::now();
        const auto image = fixture.tool->execute(
            R"({"action":"observe","target":"display","display_id":"22","wait_ms":4000})", fixture.context);
        canceler.join();
        CHECK(image.hasError());
        if (image.hasError()) CHECK(image.error().code() == MaiErrorCode::Canceled);
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
        CHECK(fixture.backend->displayCaptureCount == 0);
    }
    for (const Json& value : {Json(-1), Json(5001), Json(1.5), Json("10")}) {
        DisplayToolFixture fixture;
        const auto invalid = fixture.tool->execute(
            Json{{"action", "observe"}, {"target", "display"}, {"display_id", "22"}, {"wait_ms", value}}.dump(),
            fixture.context);
        CHECK(invalid.hasError());
        CHECK(fixture.backend->displayCaptureCount == 0);
    }
}

void verifyDisplayToolSchema() {
    DisplayToolFixture fixture;
    if (!fixture.tool) return;
    const Json schema = Json::parse(fixture.tool->parametersSchema(), nullptr, false);
    CHECK(schema.is_object());
    if (!schema.is_object()) return;
    const Json properties = schema.value("properties", Json::object());
    const Json actions = properties.value("action", Json::object()).value("enum", Json{});
    CHECK(contains(actions, "list_displays"));
    CHECK(contains(actions, "move"));
    CHECK(properties.value("display_id", Json::object()).value("type", std::string()) == "string");
    const Json targets = properties.value("target", Json::object()).value("enum", Json{});
    CHECK(contains(targets, "display"));
    CHECK(contains(targets, "window"));
}

void verifyDisplayToolObservationAndActionChain() {
    DisplayToolFixture fixture;
    if (!fixture.tool) return;
    fixture.backend->visible.clear();
    fixture.backend->focused = false;
    const auto listed = fixture.tool->execute(R"({"action":"list_displays"})", fixture.context);
    CHECK(!listed.hasError());
    if (!listed.hasError()) {
        const Json output = Json::parse(listed.output(), nullptr, false);
        CHECK(output.is_object());
        if (output.is_object()) {
            const Json displays = output.value("displays", Json::array());
            CHECK(displays.is_array());
            CHECK(displays.size() == 2);
            if (displays.is_array() && displays.size() == 2) {
                CHECK(displays[0].value("display_id", std::string()) == "11");
                CHECK(displays[0].value("primary", false));
                CHECK(displays[1].value("display_id", std::string()) == "22");
                CHECK(!displays[1].value("primary", true));
                CHECK(displays[1]["bounds"]["x"] == -1280);
                CHECK(displays[1]["bounds"]["height"] == 1024);
            }
        }
        CHECK(listed.images().empty());
    }
    CHECK(fixture.backend->displayCaptureCount == 0);
    const auto observed =
        fixture.tool->execute(R"({"action":"observe","target":"display","display_id":"22"})", fixture.context);
    CHECK(!observed.hasError());
    if (observed.hasError()) return;
    const Json first = Json::parse(observed.output(), nullptr, false);
    CHECK(first.is_object());
    if (!first.is_object()) return;
    CHECK(first.value("target", std::string()) == "display");
    CHECK(first.value("display_id", std::string()) == "22");
    CHECK(first["image_width"] == 320);
    CHECK(first["image_height"] == 256);
    CHECK(first["frame_sequence"] == 41);
    CHECK(first["frame_received_at_ms"] == 1700000000001LL);
    const std::string firstToken = first.value("snapshot_id", std::string());
    CHECK(!firstToken.empty());
    CHECK(observed.images().size() == 1);
    if (observed.images().size() == 1) {
        CHECK(observed.images()[0].mimeType == "image/png");
        CHECK(QImage(QString::fromStdString(observed.images()[0].path)).size() == QSize(320, 256));
        CHECK(QFileInfo(QString::fromStdString(observed.images()[0].path)).canonicalFilePath() ==
              QFileInfo(fixture.workspace.filePath(QString::fromStdString(first.value("image_path", std::string()))))
                  .canonicalFilePath());
    }
    const Json key{
        {"action", "key"}, {"target", "display"}, {"display_id", "22"}, {"snapshot_id", firstToken}, {"key", "Meta"}};
    const auto opened = fixture.tool->execute(key.dump(), fixture.context);
    CHECK(!opened.hasError());
    if (opened.hasError()) return;
    const Json keyReceipt = Json::parse(opened.output(), nullptr, false);
    CHECK(keyReceipt.is_object());
    if (!keyReceipt.is_object()) return;
    CHECK(keyReceipt.value("dispatched", false));
    CHECK(!keyReceipt.value("verified", true));
    CHECK(keyReceipt.value("state", std::string()) == "dispatched");
    const Json second = keyReceipt.value("observation", Json::object());
    CHECK(second.is_object());
    CHECK(second.value("display_id", std::string()) == "22");
    CHECK(second["frame_sequence"] == fixture.backend->lastProducedFrameSequence);
    CHECK(second["frame_received_at_ms"] == fixture.backend->lastProducedFrameReceivedAtMs);
    const std::string secondToken = second.value("snapshot_id", std::string());
    CHECK(!secondToken.empty());
    CHECK(secondToken != firstToken);
    CHECK(opened.images().size() == 1);
    CHECK(fixture.backend->inputs.size() == 1);
    if (!fixture.backend->inputs.empty()) {
        CHECK(fixture.backend->inputs[0].desktopScope);
        CHECK(fixture.backend->inputs[0].expectedWindowId == 0);
        CHECK(fixture.backend->inputs[0].kind == DesktopNativeInputKind::Key);
        CHECK(fixture.backend->inputs[0].key == "Meta");
    }
    const auto replayed = fixture.tool->execute(key.dump(), fixture.context);
    CHECK(replayed.hasError());
    CHECK(fixture.backend->inputs.size() == 1);
    const auto moved = fixture.tool->execute(Json{{"action", "move"},
                                                  {"target", "display"},
                                                  {"display_id", "22"},
                                                  {"snapshot_id", secondToken},
                                                  {"x_ratio", 0.25},
                                                  {"y_ratio", 0.5}}
                                                 .dump(),
                                             fixture.context);
    CHECK(!moved.hasError());
    if (!moved.hasError()) {
        const Json moveReceipt = Json::parse(moved.output(), nullptr, false);
        CHECK(moveReceipt.is_object());
        if (moveReceipt.is_object()) {
            CHECK(moveReceipt.value("dispatched", false));
            CHECK(moveReceipt["observation"]["frame_sequence"] == fixture.backend->lastProducedFrameSequence);
            CHECK(moveReceipt["observation"]["frame_received_at_ms"] == fixture.backend->lastProducedFrameReceivedAtMs);
            CHECK(moveReceipt["observation"].value("snapshot_id", std::string()) != secondToken);
        }
        CHECK(moved.images().size() == 1);
    }
    CHECK(fixture.backend->inputs.size() == 2);
    if (fixture.backend->inputs.size() == 2) {
        CHECK(fixture.backend->inputs[1].kind == DesktopNativeInputKind::Move);
        CHECK(std::abs(fixture.backend->inputs[1].x - (-960.25)) < 0.01);
        CHECK(std::abs(fixture.backend->inputs[1].y - 391.5) < 0.01);
    }
    CHECK(fixture.backend->displayCaptureCount == 3);
    CHECK(fixture.backend->activationCount == 0);
    CHECK(fixture.backend->captureCount == 0);
}

void verifyDisplayToolCaptureFailureRetainsReceipt() {
    DisplayToolFixture fixture;
    if (!fixture.tool) return;
    const auto observed =
        fixture.tool->execute(R"({"action":"observe","target":"display","display_id":"22"})", fixture.context);
    CHECK(!observed.hasError());
    if (observed.hasError()) return;
    const Json first = Json::parse(observed.output(), nullptr, false);
    CHECK(first.is_object());
    if (!first.is_object()) return;
    fixture.backend->displayCaptureError = {MaiErrorCode::NotSupported, "fixture next frame unavailable"};
    const Json click{{"action", "click"},  {"target", "display"},
                     {"display_id", "22"}, {"snapshot_id", first.value("snapshot_id", std::string())},
                     {"x_ratio", 0.5},     {"y_ratio", 0.5}};
    const auto clicked = fixture.tool->execute(click.dump(), fixture.context);
    CHECK(!clicked.hasError());
    if (!clicked.hasError()) {
        const Json receipt = Json::parse(clicked.output(), nullptr, false);
        CHECK(receipt.is_object());
        if (receipt.is_object()) {
            CHECK(receipt.value("dispatched", false));
            CHECK(receipt.value("state", std::string()) == "dispatched_observation_failed");
            CHECK(receipt.value("observation_error", std::string()).find("fixture next frame unavailable") !=
                  std::string::npos);
            CHECK(!receipt.contains("observation") || receipt["observation"].empty());
        }
        CHECK(clicked.images().empty());
    }
    CHECK(fixture.backend->inputs.size() == 1);
    CHECK(fixture.tool->execute(click.dump(), fixture.context).hasError());
    CHECK(fixture.backend->inputs.size() == 1);
}

void verifyDisplayToolRejectsMixedTargetsAndInvalidIds() {
    DisplayToolFixture fixture;
    if (!fixture.tool) return;
    const Json mixed[] = {
        {{"action", "observe"}, {"target", "display"}, {"display_id", "22"}, {"window_id", "101"}},
        {{"action", "observe"}, {"target", "display"}, {"display_id", "22"}, {"window_title", "Isolated editor"}},
        {{"action", "click"},
         {"target", "display"},
         {"display_id", "22"},
         {"window_id", "101"},
         {"snapshot_id", "no-token"},
         {"x_ratio", 0.5},
         {"y_ratio", 0.5}},
        {{"action", "observe"}, {"target", "window"}, {"display_id", "22"}, {"window_id", "101"}}};
    for (const auto& parameters : mixed) {
        const auto result = fixture.tool->execute(parameters.dump(), fixture.context);
        CHECK(result.hasError());
        if (result.hasError()) CHECK(result.error().code() == MaiErrorCode::InvalidInput);
    }
    for (const Json& invalidId :
         {Json(""), Json("0"), Json("-1"), Json("22x"), Json("18446744073709551616"), Json(22), Json(nullptr)}) {
        const auto result = fixture.tool->execute(
            Json{{"action", "observe"}, {"target", "display"}, {"display_id", invalidId}}.dump(), fixture.context);
        CHECK(result.hasError());
        if (result.hasError()) CHECK(result.error().code() == MaiErrorCode::InvalidInput);
    }
    const auto absent =
        fixture.tool->execute(R"({"action":"observe","target":"display","display_id":"99"})", fixture.context);
    CHECK(absent.hasError());
    if (absent.hasError()) CHECK(absent.error().code() == MaiErrorCode::NotFound);
    CHECK(fixture.backend->displayCaptureCount == 0);
    CHECK(fixture.backend->captureCount == 0);
    CHECK(fixture.backend->inputs.empty());
    CHECK(fixture.backend->activationCount == 0);
}

void verifyDisplayToolRejectsTextModelsBeforeNativeAccess() {
    for (const std::string model : {"glm-5.3", "deepseek-v4-pro", "local:qa"}) {
        DisplayToolFixture fixture;
        if (!fixture.tool) continue;
        // Seed a valid ordinary observation: failure must be the model gate,
        // rather than a missing snapshot or target validation failure.
        const auto observed = fixture.controller->observeDisplay(fixture.context.sessionId, fixture.context.root, 22);
        CHECK(observed);
        if (!observed) continue;
        const Json first = Json::parse(observed.value().json, nullptr, false);
        CHECK(first.is_object());
        if (!first.is_object()) continue;
        fixture.context.model = model;
        const int captures = fixture.backend->displayCaptureCount;
        const auto events = fixture.backend->events.size();
        const int windows = fixture.backend->windowsCount;
        const int captureOptions = fixture.backend->captureOptionsCount;
        const auto deniedObserve =
            fixture.tool->execute(R"({"action":"observe","target":"display","display_id":"22"})", fixture.context);
        CHECK(deniedObserve.hasError());
        if (deniedObserve.hasError()) CHECK(deniedObserve.error().code() == MaiErrorCode::NotSupported);
        const auto deniedInput = fixture.tool->execute(Json{{"action", "key"},
                                                            {"target", "display"},
                                                            {"display_id", "22"},
                                                            {"snapshot_id", first.value("snapshot_id", std::string())},
                                                            {"key", "Meta"}}
                                                           .dump(),
                                                       fixture.context);
        CHECK(deniedInput.hasError());
        if (deniedInput.hasError()) CHECK(deniedInput.error().code() == MaiErrorCode::NotSupported);
        CHECK(fixture.backend->displayCaptureCount == captures);
        CHECK(fixture.backend->captureOptionsCount == captureOptions);
        CHECK(fixture.backend->windowsCount == windows);
        CHECK(fixture.backend->events.size() == events);
        CHECK(fixture.backend->inputs.empty());
        CHECK(fixture.backend->activationCount == 0);
        // Metadata-only window observation remains usable by these models.
        const auto window =
            fixture.tool->execute(R"({"action":"observe","window_id":"101","include_elements":true})", fixture.context);
        CHECK(!window.hasError());
        CHECK(window.images().empty());
        CHECK(fixture.backend->captureCount == 0);
    }
}

void verifyDisplayToolWorkspaceChangeRejectsOldToken() {
    for (const bool clearWorkspace : {false, true}) {
        DisplayToolFixture fixture;
        QTemporaryDir differentWorkspace;
        CHECK(differentWorkspace.isValid());
        if (!fixture.tool) continue;
        const auto observed =
            fixture.tool->execute(R"({"action":"observe","target":"display","display_id":"22"})", fixture.context);
        CHECK(!observed.hasError());
        if (observed.hasError()) continue;
        const Json first = Json::parse(observed.output(), nullptr, false);
        CHECK(first.is_object());
        if (!first.is_object()) continue;
        const auto originalFiles = QDir(fixture.workspace.filePath(QStringLiteral("computer"))).entryList(QDir::Files);
        fixture.context.root = clearWorkspace ? std::string() : differentWorkspace.path().toUtf8().toStdString();
        const auto result = fixture.tool->execute(Json{{"action", "click"},
                                                       {"target", "display"},
                                                       {"display_id", "22"},
                                                       {"snapshot_id", first.value("snapshot_id", std::string())},
                                                       {"x_ratio", 0.5},
                                                       {"y_ratio", 0.5}}
                                                      .dump(),
                                                  fixture.context);
        CHECK(result.hasError());
        if (result.hasError()) CHECK(result.error().code() == MaiErrorCode::InvalidInput);
        CHECK(result.images().empty());
        CHECK(fixture.backend->inputs.empty());
        CHECK(fixture.backend->activationCount == 0);
        CHECK(fixture.backend->displayCaptureCount == 1);
        CHECK(QDir(fixture.workspace.filePath(QStringLiteral("computer"))).entryList(QDir::Files) == originalFiles);
        CHECK(!QFileInfo::exists(differentWorkspace.filePath(QStringLiteral("computer"))));
    }
}

void verifyOwnedPopupInput() {
    Fixture fixture;
    auto menu = fixture.observe("right_click");
    CHECK(fixture.controller.perform("owner", menu));
    CHECK(fixture.backend->ownedPopup);
    CHECK(!fixture.backend->focused);
    const int activated = fixture.backend->activationCount;
    fixture.backend->requirePopupForKey = true;
    auto key = fixture.observe("key");
    key.key = "ArrowDown";
    const auto sent = fixture.controller.perform("owner", key);
    CHECK(sent);
    if (sent) CHECK(sent.value().dispatched);
    CHECK(fixture.backend->ownedPopup);
    CHECK(fixture.backend->activationCount == activated);
    // A menu owner is valid for keyboard navigation, not for focused-value verification.
    auto verify = fixture.observe("verify");
    const auto checked = fixture.controller.perform("owner", verify);
    CHECK(!checked);
    if (!checked) CHECK(checked.error().code() == MaiErrorCode::Busy);
    CHECK(fixture.backend->readCount == 0);
}

void verifySecureKeyboardInput() {
    for (const char* key : {"A", "Space", "Backspace", "Delete", "Ctrl+V", "Shift+Insert"}) {
        Fixture fixture;
        auto action = fixture.observe("key");
        action.key = key;
        fixture.backend->secure = true;
        const auto result = fixture.controller.perform("owner", action);
        CHECK(!result);
        if (!result) CHECK(result.error().code() == MaiErrorCode::NotSupported);
        CHECK(fixture.backend->inputs.empty());
    }
    // Navigating away from a protected field remains available without writing it.
    for (const char* key : {"Tab", "Escape", "ControlRight", "ShiftLeft", "AltRight", "MetaRight", "Ctrl+Shift",
                            "ControlLeft+ControlRight"}) {
        Fixture fixture;
        auto action = fixture.observe("key");
        action.key = key;
        fixture.backend->secure = true;
        CHECK(fixture.controller.perform("owner", action));
        CHECK(fixture.backend->inputs.size() == 1);
    }
}

void verifyNativeInputFailureRequiresObservation() {
    for (const bool display : {false, true}) {
        Fixture fixture;
        auto action = display ? fixture.observeDisplay("key") : fixture.observe("key");
        action.key = "Enter";
        const auto code = display ? MaiErrorCode::Internal : MaiErrorCode::Busy;
        const std::string detail =
            display ? "owned display native error after input began" : "owned window native error after input began";
        // The native seam may fail after part of a key batch reached the OS.
        // Preserve that uncertainty so the next decision observes the current
        // application, instead of treating a tool error as proof of no input.
        fixture.backend->inputError = {code, detail};
        const auto failed = fixture.controller.perform("owner", action);
        CHECK(!failed);
        if (!failed) {
            CHECK(failed.error().code() == code);
            CHECK(failed.error().message().find("Input may have been partially delivered. Observe the current "
                                                "application state before deciding whether to retry. Native error: ") ==
                  0);
            CHECK(failed.error().message().find(detail) != std::string::npos);
        }
        CHECK(fixture.backend->inputs.size() == 1);
        fixture.backend->inputError = {};
        const auto replay = fixture.controller.perform("owner", action);
        CHECK(!replay);
        if (!replay) CHECK(replay.error().code() == MaiErrorCode::InvalidInput);
        CHECK(fixture.backend->inputs.size() == 1);
    }
    // A failure before native input remains precise; it must not imply that a
    // keyboard event might have been delivered when activation itself failed.
    Fixture fixture;
    auto action = fixture.observe("key");
    action.key = "Enter";
    const std::string detail = "owned activation denied before input";
    fixture.backend->activationError = {MaiErrorCode::Busy, detail};
    const auto denied = fixture.controller.perform("owner", action);
    CHECK(!denied);
    if (!denied) {
        CHECK(denied.error().code() == MaiErrorCode::Busy);
        CHECK(denied.error().message() == detail);
    }
    CHECK(fixture.backend->inputs.empty());
}

void verifyKeyGrammar() {
    const auto selectAll = maiDesktopParseKey("Ctrl+A");
    CHECK(selectAll);
    if (selectAll) {
        CHECK(selectAll.value().key == DesktopKeyCode::Character);
        CHECK(selectAll.value().character == 'A');
        CHECK(selectAll.value().modifiers == std::vector<DesktopKeyModifier>{DesktopKeyModifier::Control});
    }
    const auto selection = maiDesktopParseKey("shift+home");
    CHECK(selection);
    if (selection) {
        CHECK(selection.value().key == DesktopKeyCode::Home);
        CHECK(selection.value().modifiers == std::vector<DesktopKeyModifier>{DesktopKeyModifier::Shift});
    }
    for (const char* shortcut : {"Control+Shift+End",
                                 "Alt+F4",
                                 "Cmd+A",
                                 "Meta+A",
                                 "Super+A",
                                 "Enter",
                                 "Tab",
                                 "Escape",
                                 "Delete",
                                 "Backspace",
                                 "ArrowLeft",
                                 "ArrowRight",
                                 "ArrowUp",
                                 "ArrowDown",
                                 "PageUp",
                                 "PageDown",
                                 "Space",
                                 "Insert",
                                 "End",
                                 "F1",
                                 "F24",
                                 "9",
                                 "Ctrl+Shift",
                                 "Alt+Shift"}) {
        const auto parsed = maiDesktopParseKey(shortcut);
        CHECK(parsed);
    }
    // Standalone Meta opens an OS launcher; standalone modifiers also need a
    // balanced native down/up dispatch rather than an invented character key.
    const std::pair<const char*, DesktopKeyCode> modifiers[] = {{"Ctrl", DesktopKeyCode::Control},
                                                                {"Shift", DesktopKeyCode::Shift},
                                                                {"Alt", DesktopKeyCode::Alt},
                                                                {"Meta", DesktopKeyCode::Meta}};
    for (const auto& modifier : modifiers) {
        const auto parsed = maiDesktopParseKey(modifier.first);
        CHECK(parsed);
        if (parsed) CHECK(parsed.value().key == modifier.second);
    }
    for (const std::string& invalid :
         {std::string(), std::string("Ctrl+"), std::string("+A"), std::string("Ctrl++A"), std::string("Ctrl+Control+A"),
          std::string("Cmd+Meta+A"), std::string("Ctrl+A+B"), std::string("F0"), std::string("F25"),
          std::string("Hyper+A"), std::string("\xe4\xb8\xad")}) {
        const auto parsed = maiDesktopParseKey(invalid);
        CHECK(!parsed);
        if (!parsed) CHECK(parsed.error().code() == MaiErrorCode::InvalidInput);
    }
}

void verifyExpandedKeyGrammar() {
    // Key chords describe hardware operations; Unicode text (including Chinese)
    // remains the separate type action. Punctuation glyphs and physical names
    // must both reach native mapping without conflating Equal with NumpadAdd.
    for (char character = '!'; character <= '~'; ++character) {
        const auto parsed = maiDesktopParseKey(std::string(1, character));
        if (!parsed) std::fprintf(stderr, "printable key rejected: %c\n", character);
        CHECK(parsed);
        if (parsed) {
            const char expected = character >= 'a' && character <= 'z' ? char(character - 'a' + 'A') : character;
            CHECK(parsed.value().key == DesktopKeyCode::Character);
            CHECK(parsed.value().character == expected);
        }
    }
    const auto plus = maiDesktopParseKey("Ctrl++");
    CHECK(plus);
    if (plus) {
        CHECK(plus.value().key == DesktopKeyCode::Character);
        CHECK(plus.value().character == '+');
        CHECK(plus.value().modifiers == std::vector<DesktopKeyModifier>{DesktopKeyModifier::Control});
    }
    for (const char* key : {"Plus",
                            "Option+Return",
                            "Win+R",
                            "Windows+R",
                            "Esc",
                            "Del",
                            "PgUp",
                            "PgDn",
                            "Backquote",
                            "Minus",
                            "Equal",
                            "BracketLeft",
                            "BracketRight",
                            "Backslash",
                            "IntlBackslash",
                            "Semicolon",
                            "Quote",
                            "Comma",
                            "Period",
                            "Slash",
                            "CapsLock",
                            "NumLock",
                            "ScrollLock",
                            "PrintScreen",
                            "Pause",
                            "ContextMenu",
                            "Numpad0",
                            "Numpad1",
                            "Numpad2",
                            "Numpad3",
                            "Numpad4",
                            "Numpad5",
                            "Numpad6",
                            "Numpad7",
                            "Numpad8",
                            "Numpad9",
                            "NumpadAdd",
                            "NumpadSubtract",
                            "NumpadMultiply",
                            "NumpadDivide",
                            "NumpadDecimal",
                            "NumpadEnter",
                            "NumpadEqual",
                            "NumpadComma",
                            "Clear",
                            "Help",
                            "Convert",
                            "NonConvert",
                            "KanaMode",
                            "Eisu",
                            "IntlRo",
                            "IntlYen",
                            "ControlLeft",
                            "ControlRight",
                            "ShiftLeft",
                            "ShiftRight",
                            "AltLeft",
                            "AltRight",
                            "MetaLeft",
                            "MetaRight",
                            "Fn",
                            "VolumeMute",
                            "VolumeDown",
                            "VolumeUp",
                            "MediaPlayPause",
                            "MediaStop",
                            "MediaNext",
                            "MediaPrevious",
                            "BrowserBack",
                            "BrowserForward",
                            "BrowserRefresh",
                            "BrowserStop",
                            "BrowserSearch",
                            "BrowserFavorites",
                            "BrowserHome",
                            "LaunchMail",
                            "LaunchMedia",
                            "LaunchApp1",
                            "LaunchApp2",
                            "LeftCtrl+RightCtrl",
                            "ControlLeft+ControlRight+A",
                            "LeftShift+RightAlt+Enter"}) {
        const auto parsed = maiDesktopParseKey(key);
        if (!parsed) std::fprintf(stderr, "named key rejected: %s\n", key);
        CHECK(parsed);
    }
    for (const char* invalid :
         {"Ctrl+++", "Ctrl++A", "Ctrl+ControlLeft+A", "ShiftLeft+ShiftLeft+A", "MetaRight+RightCmd+A",
          "AltLeft+Option+A", "ControlLeft+Ctrl", "ControlRight+RightCtrl"}) {
        const auto parsed = maiDesktopParseKey(invalid);
        CHECK(!parsed);
        if (!parsed) CHECK(parsed.error().code() == MaiErrorCode::InvalidInput);
    }
}

void verifyKeyIdentityAndCatalog() {
    // Distinguish logical printable input from its physical main/numpad key.
    const auto literal = maiDesktopParseKey("=");
    const auto physical = maiDesktopParseKey("Equal");
    const auto numpad = maiDesktopParseKey("NumpadEqual");
    CHECK(literal && physical && numpad);
    if (literal && physical && numpad) {
        CHECK(literal.value().key == DesktopKeyCode::Character);
        CHECK(literal.value().character == '=');
        CHECK(physical.value().key == DesktopKeyCode::Equal);
        CHECK(numpad.value().key == DesktopKeyCode::NumpadEqual);
    }
    const auto plusAlias = maiDesktopParseKey("Ctrl+Plus");
    CHECK(plusAlias);
    if (plusAlias) {
        CHECK(plusAlias.value().key == DesktopKeyCode::Character);
        CHECK(plusAlias.value().character == '+');
        CHECK(plusAlias.value().modifiers == std::vector<DesktopKeyModifier>{DesktopKeyModifier::Control});
    }
    const std::pair<const char*, DesktopKeyCode> identities[] = {
        {"NumpadEnter", DesktopKeyCode::NumpadEnter}, {"Enter", DesktopKeyCode::Enter},
        {"NumpadAdd", DesktopKeyCode::NumpadAdd},     {"Numpad0", DesktopKeyCode::Numpad0},
        {"Numpad9", DesktopKeyCode::Numpad9},         {"CapsLock", DesktopKeyCode::CapsLock},
        {"PrintScreen", DesktopKeyCode::PrintScreen}, {"Pause", DesktopKeyCode::Pause},
        {"IntlRo", DesktopKeyCode::IntlRo},           {"IntlYen", DesktopKeyCode::IntlYen},
        {"VolumeUp", DesktopKeyCode::VolumeUp},       {"BrowserBack", DesktopKeyCode::BrowserBack},
        {"LaunchApp2", DesktopKeyCode::LaunchApp2}};
    for (const auto& identity : identities) {
        const auto parsed = maiDesktopParseKey(identity.first);
        CHECK(parsed);
        if (parsed) CHECK(parsed.value().key == identity.second);
    }

    struct ModifierCase {
        const char* name;
        DesktopKeyCode key;
        DesktopKeyModifier modifier;
        DesktopKeyModifier family;
    };
    const ModifierCase modifiers[] = {
        {"Control", DesktopKeyCode::Control, DesktopKeyModifier::Control, DesktopKeyModifier::Control},
        {"Shift", DesktopKeyCode::Shift, DesktopKeyModifier::Shift, DesktopKeyModifier::Shift},
        {"Alt", DesktopKeyCode::Alt, DesktopKeyModifier::Alt, DesktopKeyModifier::Alt},
        {"Meta", DesktopKeyCode::Meta, DesktopKeyModifier::Meta, DesktopKeyModifier::Meta},
        {"ControlLeft", DesktopKeyCode::ControlLeft, DesktopKeyModifier::ControlLeft, DesktopKeyModifier::Control},
        {"ControlRight", DesktopKeyCode::ControlRight, DesktopKeyModifier::ControlRight, DesktopKeyModifier::Control},
        {"ShiftLeft", DesktopKeyCode::ShiftLeft, DesktopKeyModifier::ShiftLeft, DesktopKeyModifier::Shift},
        {"ShiftRight", DesktopKeyCode::ShiftRight, DesktopKeyModifier::ShiftRight, DesktopKeyModifier::Shift},
        {"AltLeft", DesktopKeyCode::AltLeft, DesktopKeyModifier::AltLeft, DesktopKeyModifier::Alt},
        {"AltRight", DesktopKeyCode::AltRight, DesktopKeyModifier::AltRight, DesktopKeyModifier::Alt},
        {"MetaLeft", DesktopKeyCode::MetaLeft, DesktopKeyModifier::MetaLeft, DesktopKeyModifier::Meta},
        {"MetaRight", DesktopKeyCode::MetaRight, DesktopKeyModifier::MetaRight, DesktopKeyModifier::Meta}};
    for (const auto& item : modifiers) {
        const auto parsed = maiDesktopParseKey(item.name);
        CHECK(parsed);
        if (parsed) CHECK(parsed.value().key == item.key);
        DesktopKeyModifier found = DesktopKeyModifier::Control;
        CHECK(maiDesktopKeyModifier(item.key, found));
        CHECK(found == item.modifier);
        CHECK(maiDesktopModifierFamily(found) == item.family);
    }
    DesktopKeyModifier unchanged = DesktopKeyModifier::Alt;
    CHECK(!maiDesktopKeyModifier(DesktopKeyCode::Enter, unchanged));
    CHECK(unchanged == DesktopKeyModifier::Alt);
    const auto opposite = maiDesktopParseKey("LeftCtrl+RightCtrl");
    CHECK(opposite);
    if (opposite) {
        CHECK(opposite.value().modifiers == std::vector<DesktopKeyModifier>{DesktopKeyModifier::ControlLeft});
        CHECK(opposite.value().key == DesktopKeyCode::ControlRight);
    }
    const auto chord = maiDesktopParseKey(" rightctrl + LEFTSHIFT + ? ");
    CHECK(chord);
    if (chord) {
        const std::vector<DesktopKeyModifier> expected{DesktopKeyModifier::ControlRight, DesktopKeyModifier::ShiftLeft};
        CHECK(chord.value().modifiers == expected);
        CHECK(chord.value().character == '?');
    }
    for (const char* invalid : {" ", "\t", "Ctrl+ ", "Ctrl+Control", "Shift+Shift", "Ctrl+ControlRight+Enter"}) {
        CHECK(!maiDesktopParseKey(invalid));
    }

    const auto& names = maiDesktopKeyNames();
    CHECK(!names.empty());
    for (auto iterator = names.begin(); iterator != names.end(); ++iterator) {
        CHECK(maiDesktopParseKey(*iterator));
        CHECK(std::find(iterator + 1, names.end(), *iterator) == names.end());
    }
    for (const char* name : {"+", "=", "A", "0", "F24", "Space", "Equal", "NumpadEnter", "ControlRight", "LaunchApp2"})
        CHECK(std::find(names.begin(), names.end(), name) != names.end());
}

void verifyKeyCatalogTool() {
    auto backend = std::make_shared<RecordingBackend>();
    auto controller = std::make_shared<DesktopComputerController>(backend);
    MaiToolRegistry registry;
    registerDesktopComputerTools(registry, controller);
    MaiTool* tool = registry.find("desktop_computer");
    CHECK(tool);
    if (!tool) return;
    const auto schema = Json::parse(tool->parametersSchema(), nullptr, false);
    CHECK(schema.is_object());
    if (schema.is_object()) {
        CHECK(contains(schema["properties"]["action"]["enum"], "list_keys"));
        CHECK(schema["properties"]["key"].value("maxLength", 0) == 128);
    }
    MaiToolContext context;
    context.sessionId = "catalog-no-observation";
    // Capabilities are ordinary metadata: no observation token, workspace,
    // display, window enumeration, capture, activation or native input is needed.
    const auto catalog = tool->execute(R"({"action":"list_keys"})", context);
    CHECK(!catalog.hasError());
    CHECK(catalog.images().empty());
    CHECK(!catalog.isTruncated());
    if (!catalog.hasError()) {
        const auto output = Json::parse(catalog.output(), nullptr, false);
        CHECK(output.is_object());
        if (output.is_object()) {
            for (const char* name : {"ControlRight", "NumpadEnter", "Equal", "Plus", "VolumeMute"})
                CHECK(contains(output.value("key_names", Json::array()), name));
            CHECK(output.value("text_action", std::string()) == "type");
            CHECK(output.value("native_support_checked_at_dispatch", false));
        }
    }
    for (const auto& extra : {Json{{"text", "do not type"}}, Json{{"key", "Enter"}}, Json{{"target", "display"}},
                              Json{{"window_id", "1"}}, Json{{"snapshot_id", "unused"}}}) {
        Json arguments{{"action", "list_keys"}};
        arguments.update(extra);
        const auto rejected = tool->execute(arguments.dump(), context);
        CHECK(rejected.hasError());
        if (rejected.hasError()) CHECK(rejected.error().code() == MaiErrorCode::InvalidInput);
    }
    std::atomic<bool> canceled{true};
    context.cancel = &canceled;
    const auto stopped = tool->execute(R"({"action":"list_keys"})", context);
    CHECK(stopped.hasError());
    if (stopped.hasError()) CHECK(stopped.error().code() == MaiErrorCode::Canceled);
    context.cancel = nullptr;
    context.sessionId.clear();
    const auto ownerless = tool->execute(R"({"action":"list_keys"})", context);
    CHECK(ownerless.hasError());
    if (ownerless.hasError()) CHECK(ownerless.error().code() == MaiErrorCode::NotConfigured);
    CHECK(backend->windowsCount == 0);
    CHECK(backend->activationCount == 0);
    CHECK(backend->captureCount == 0);
    CHECK(backend->displayCaptureCount == 0);
    CHECK(backend->inputs.empty());
    CHECK(backend->events.empty());
}

void makeWindowUnavailable(DesktopNativeWindow& window, int state) {
    if (state == 0) window.minimized = true;
    else if (state == 1) window.cloaked = true;
    else window.boundsAvailable = false;
}

void verifyWindowDiscoveryMetadataAndUntitledSelection() {
    auto backend = std::make_shared<RecordingBackend>();
    backend->visible.clear();
    for (std::uint64_t id = 1; id <= 205; ++id) {
        DesktopNativeWindow window{id, 5001, "", "Fixture", 10, 20, 20, 12};
        window.className = "OwnedUntitledProbe";
        window.toolWindow = true;
        window.ownerId = 9007199254740993ULL;
        if (id == 2) window.minimized = true;
        if (id == 3) window.cloaked = true;
        if (id == 4) window.boundsAvailable = false;
        backend->visible.push_back(window);
    }
    auto controller = std::make_shared<DesktopComputerController>(backend);
    MaiToolRegistry registry;
    registerDesktopComputerTools(registry, controller);
    const auto tool = registry.find("desktop_computer");
    CHECK(tool);
    if (!tool) return;
    MaiToolContext context;
    context.sessionId = "window-discovery";
    const auto result = tool->execute(R"({"action":"list"})", context);
    CHECK(!result.hasError());
    if (!result.hasError()) {
        const auto output = Json::parse(result.output(), nullptr, false);
        CHECK(output.is_object());
        CHECK(output.value("count", 0) == 205);
        CHECK(!output.contains("complete") && !output.contains("truncated"));
        const auto windows = output.value("windows", Json::array());
        CHECK(windows.size() == 205);
        if (windows.size() == 205) {
            const auto first = windows[0];
            CHECK(first.value("title", std::string()) == "");
            CHECK(first.value("class_name", std::string()) == "OwnedUntitledProbe");
            CHECK(first.value("owner_window_id", std::string()) == "9007199254740993");
            CHECK(first.value("tool_window", false));
            CHECK(first.contains("minimized") && !first.value("minimized", true));
            CHECK(first.contains("cloaked") && !first.value("cloaked", true));
            CHECK(first.contains("bounds_available") && first.value("bounds_available", false));
            CHECK(windows[1].value("minimized", false));
            CHECK(windows[2].value("cloaked", false));
            CHECK(windows[3].contains("bounds_available") && !windows[3].value("bounds_available", true));
            CHECK(windows.back().value("window_id", std::string()) == "205");
        }
    }
    CHECK(backend->captureCount == 0 && backend->activationCount == 0 && backend->inputSessionCreations == 0);
    CHECK(backend->inputs.empty());
    const auto extra = tool->execute(R"({"action":"list","limit":10})", context);
    CHECK(extra.hasError());
    if (extra.hasError()) CHECK(extra.error().code() == MaiErrorCode::InvalidInput);
    QTemporaryDir workspace;
    const auto ordinary = controller->observe("untitled", "", workspace.path().toUtf8().toStdString(), false, 1, false);
    CHECK(ordinary);
    if (ordinary) CHECK(!Json::parse(ordinary.value().json).value("snapshot_id", std::string()).empty());
    context.root = workspace.path().toUtf8().toStdString();
    const auto byId = tool->execute(R"({"action":"observe","window_id":"1","include_elements":false})", context);
    CHECK(!byId.hasError());
    if (!byId.hasError()) {
        const auto output = Json::parse(byId.output());
        CHECK(output.value("title", std::string()) == "");
        CHECK(!output.value("snapshot_id", std::string()).empty());
    }
    CHECK(backend->captureCount == 0 && backend->activationCount == 0 && backend->inputSessionCreations == 0);
    CHECK(backend->inputs.empty());
}

void verifyUnobservableDiscoveryCannotIssueWindowToken() {
    for (int state = 0; state < 3; ++state) {
        for (const bool image : {false, true}) {
            Fixture fixture;
            makeWindowUnavailable(fixture.backend->visible.front(), state);
            const auto observed = fixture.controller.observe("owner", "", fixture.workspace.path().toUtf8().toStdString(),
                                                               false, 101, image);
            CHECK(bool(observed) == (!image && state < 2));
            if (!observed) CHECK(observed.error().code() == MaiErrorCode::Busy ||
                                 observed.error().code() == MaiErrorCode::InvalidInput);
            CHECK(fixture.backend->captureCount == 0 && fixture.backend->activationCount == 0);
            CHECK(fixture.backend->inputSessionCreations == 0 && fixture.backend->inputs.empty());
        }
        Fixture fixture;
        makeWindowUnavailable(fixture.backend->visible.front(), state);
        auto controller = std::make_shared<DesktopComputerController>(fixture.backend);
        MaiToolRegistry registry;
        registerDesktopComputerTools(registry, controller);
        const auto tool = registry.find("desktop_computer");
        CHECK(tool);
        if (!tool) continue;
        MaiToolContext context;
        context.sessionId = "unobservable-window";
        context.root = fixture.workspace.path().toUtf8().toStdString();
        const auto result = tool->execute(R"({"action":"observe","window_id":"101","include_elements":false})", context);
        CHECK(result.hasError() == (state == 2));
        if (result.hasError()) CHECK(result.error().code() == MaiErrorCode::Busy ||
                                     result.error().code() == MaiErrorCode::InvalidInput);
        CHECK(fixture.backend->captureCount == 0 && fixture.backend->activationCount == 0);
        CHECK(fixture.backend->inputSessionCreations == 0 && fixture.backend->inputs.empty());
    }
}

void verifyMinimizedMetadataObservationAndNoHoldsRelease() {
    {
        Fixture fixture;
        fixture.backend->visible.front().minimized = true;
        fixture.backend->visible.front().boundsAvailable = false;
        const auto observed = fixture.controller.observe("owner", "", fixture.workspace.path().toUtf8().toStdString(),
                                                           false, 101, false, {}, "state-turn");
        CHECK(observed);
        if (observed) {
            const auto output = Json::parse(observed.value().json);
            CHECK(output["minimized"] == true);
            CHECK(!output.value("snapshot_id", std::string()).empty());
        }
        CHECK(fixture.backend->captureCount == 0 && fixture.backend->activationCount == 0);
        CHECK(fixture.backend->inputSessionCreations == 0);
    }
    for (const bool display : {false, true}) {
        Fixture fixture;
        auto action = display ? fixture.observeDisplay() : fixture.observe();
        CHECK(!fixture.controller.releaseInput("owner"));
        CHECK(!fixture.controller.perform("owner", action));
        CHECK(fixture.backend->inputs.empty() && fixture.backend->inputSessionCreations == 0);
    }
}

void verifyWindowStateOperations() {
    Fixture fixture;
    auto minimize = fixture.observe("minimize");
    minimize.dryRun = true;
    DesktopComputerActionProgress progress;
    const auto dry = fixture.controller.perform("owner", minimize, &progress);
    CHECK(dry && dry.value().dryRun && !dry.value().dispatched);
    CHECK(fixture.backend->stateCalls == 0 && fixture.backend->activationCount == 0);
    CHECK(!progress.windowOperationAttempted && !progress.inputDispatchAttempted);
    minimize.dryRun = false;
    const auto minimized = fixture.controller.perform("owner", minimize, &progress);
    CHECK(minimized);
    if (minimized) {
        CHECK(minimized.value().beforeWindow && !minimized.value().beforeWindow->minimized);
        CHECK(minimized.value().afterWindow && minimized.value().afterWindow->minimized);
        CHECK(minimized.value().focused && *minimized.value().focused);
    }
    CHECK(progress.windowOperationAttempted && !progress.inputDispatchAttempted && progress.needsObservation);
    CHECK(fixture.backend->stateCalls == 1 && fixture.backend->activationCount == 0);
    CHECK(!fixture.controller.perform("owner", minimize));
    const auto observed = fixture.controller.observe("owner", "", fixture.workspace.path().toUtf8().toStdString(),
                                                       true, 101, false);
    CHECK(observed);
    if (!observed) return;
    DesktopComputerAction restore;
    restore.action = "restore";
    restore.windowId = 101;
    restore.snapshotId = Json::parse(observed.value().json).value("snapshot_id", std::string());
    const auto restored = fixture.controller.perform("owner", restore);
    CHECK(restored && restored.value().afterWindow && !restored.value().afterWindow->minimized);
    CHECK(fixture.backend->stateCalls == 2 && fixture.backend->inputs.empty());
    for (const int changed : {0, 1}) {
        Fixture invalid;
        auto action = invalid.observe("minimize");
        if (changed == 0) ++invalid.backend->visible[0].processId;
        else invalid.backend->visible[0].cloaked = true;
        CHECK(!invalid.controller.perform("owner", action));
        CHECK(invalid.backend->stateCalls == 0);
    }
    Fixture held;
    const auto native = holdControl(held, false, "state-turn");
    auto state = held.observe("minimize");
    state.turnId = "state-turn";
    CHECK(held.controller.perform("owner", state));
    CHECK(native && native->state().empty());
    CHECK(held.backend->stateCalls == 1);
}

DesktopComputerAction observeNativeElement(Fixture& fixture, bool display = false,
                                            const std::string& session = "owner", const std::string& turn = {}) {
    fixture.backend->nativeElements = std::vector<DesktopNativeElement>{
        {"button", "Observed button", 120, 220, 100, 30, "runtime-1", "fixture-button"}};
    DesktopComputerAction action;
    action.action = "click";
    action.windowId = display ? 0 : 101;
    action.displayId = display ? 11 : 0;
    action.desktopScope = display;
    action.workspace = fixture.workspace.path().toUtf8().toStdString();
    action.turnId = turn;
    const auto observed = display ? fixture.controller.observeDisplay(session, action.workspace, 11, {}, 0, 0, 0, turn, 101)
                                  : fixture.controller.observe(session, "", action.workspace, true, 101, false, {}, turn);
    CHECK(observed);
    if (!observed) return action;
    const auto output = Json::parse(observed.value().json);
    action.snapshotId = output.value("snapshot_id", std::string());
    CHECK(output.contains("accessibility"));
    CHECK(output.value("elements", Json::array()).size() == 1);
    if (output.contains("elements") && !output["elements"].empty()) {
        const auto& element = output["elements"][0];
        action.elementId = element.value("element_id", std::string());
        CHECK(!element.contains("native_id") && !element.contains("reference"));
        CHECK(element.value("enabled", false));
        CHECK(!element.value("offscreen", true));
    }
    CHECK(!action.elementId.empty());
    return action;
}

void verifySnapshotBoundElementActions() {
    for (const bool display : {false, true}) {
        Fixture fixture;
        fixture.backend->nativeElements = std::vector<DesktopNativeElement>{
            {"button", "Inside", 120, 220, 100, 30, "inside-runtime", "inside"},
            {"button", "Outside", 100000, 100000, 20, 20, "outside-runtime", "outside"}};
        const auto workspace = fixture.workspace.path().toUtf8().toStdString();
        const auto observed = display ? fixture.controller.observeDisplay("owner", workspace, 11, {}, 0, 0, 0, {}, 101)
                                      : fixture.controller.observe("owner", "", workspace, true, 101, false);
        CHECK(observed);
        if (observed) {
            const auto output = Json::parse(observed.value().json);
            CHECK(output["elements"].size() == 1);
            CHECK(output["accessibility"]["returned_elements"] == output["elements"].size());
        }
    }
    for (const bool display : {false, true}) {
        for (const char* kind : {"click", "double_click", "right_click"}) {
            Fixture fixture;
            auto action = observeNativeElement(fixture, display);
            action.action = kind;
            const auto acted = fixture.controller.perform("owner", action);
            CHECK(acted);
            CHECK(fixture.backend->inputs.size() == 1);
            if (!fixture.backend->inputs.empty()) {
                CHECK(fixture.backend->inputs[0].x == 170 && fixture.backend->inputs[0].y == 235);
                CHECK(fixture.backend->inputs[0].desktopScope == display);
            }
            CHECK(fixture.backend->activationCount == (display ? 0 : 1));
            CHECK(!fixture.controller.perform("owner", action));
        }
        for (int change = 0; change < 9; ++change) {
            Fixture fixture;
            const auto action = observeNativeElement(fixture, display);
            auto& elements = *fixture.backend->nativeElements;
            if (change == 0) elements[0].label += " changed";
            if (change == 1) elements[0].x += 1;
            if (change == 2) elements[0].enabled = false;
            if (change == 3) elements[0].offscreen = true;
            if (change == 4) elements[0].automationId += " changed";
            if (change == 5) elements.clear();
            if (change == 6) elements.push_back(elements[0]);
            if (change == 7) ++fixture.backend->visible[0].processId;
            if (change == 8) elements[0].role = "text";
            CHECK(!fixture.controller.perform("owner", action));
            CHECK(fixture.backend->inputs.empty() && fixture.backend->activationCount == 0);
        }
        Fixture fixture;
        const auto old = observeNativeElement(fixture, display, "owner", "old-turn");
        const auto fresh = observeNativeElement(fixture, display, "owner", "new-turn");
        CHECK(old.elementId != fresh.elementId);
        CHECK(!fixture.controller.releaseInput("owner", "old-turn"));
        auto wrong = fresh;
        wrong.elementId = old.elementId;
        CHECK(!fixture.controller.perform("owner", wrong));
        CHECK(!fixture.controller.perform("other-session", fresh));
        CHECK(fixture.controller.perform("owner", fresh));
    }
}

void verifyCompletedTurnReleasesSnapshotReferences() {
    class ElementReference final : public DesktopNativeElementReference {};
    for (const bool display : {false, true}) {
        for (const bool clear : {false, true}) {
            Fixture fixture;
            auto reference = std::make_shared<ElementReference>();
            const std::weak_ptr<const DesktopNativeElementReference> weak = reference;
            fixture.backend->nativeElements = std::vector<DesktopNativeElement>{
                {"button", "Retained control", 120, 220, 100, 30, "reference-runtime", "reference-button",
                 true, false, reference}};
            const auto workspace = fixture.workspace.path().toUtf8().toStdString();
            const auto observed = display ? fixture.controller.observeDisplay(
                "owner", workspace, 11, {}, 0, 0, 0, "reference-turn", 101)
                : fixture.controller.observe("owner", "", workspace, true, 101, false, {}, "reference-turn");
            CHECK(observed);
            reference.reset();
            fixture.backend->nativeElements.reset();
            CHECK(!weak.expired());  // The observation is now the sole native-reference owner.
            if (clear)
                CHECK(!fixture.controller.clear("owner"));
            else
                CHECK(!fixture.controller.releaseInput("owner", "reference-turn"));
            CHECK(weak.expired());
            CHECK(fixture.backend->inputSessionCreations == 0);
        }
        Fixture fixture;
        const auto workspace = fixture.workspace.path().toUtf8().toStdString();
        auto reference = std::make_shared<ElementReference>();
        const std::weak_ptr<const DesktopNativeElementReference> old = reference;
        const auto observeReference = [&](const std::string& turn) {
            fixture.backend->nativeElements = std::vector<DesktopNativeElement>{
                {"button", "Retained control", 120, 220, 100, 30, "reference-runtime", "reference-button",
                 true, false, reference}};
            const auto observed = display ? fixture.controller.observeDisplay("owner", workspace, 11, {}, 0, 0, 0, turn, 101)
                                          : fixture.controller.observe("owner", "", workspace, true, 101, false, {}, turn);
            CHECK(observed);
            reference.reset();
            fixture.backend->nativeElements.reset();
        };
        observeReference("old-reference-turn");
        CHECK(!old.expired());
        reference = std::make_shared<ElementReference>();
        const std::weak_ptr<const DesktopNativeElementReference> current = reference;
        observeReference("new-reference-turn");
        CHECK(old.expired() && !current.expired());
        CHECK(!fixture.controller.releaseInput("owner", "old-reference-turn"));
        CHECK(!current.expired());
        CHECK(!fixture.controller.releaseInput("owner", "new-reference-turn"));
        CHECK(current.expired());
    }
}

void verifyDefaultElementResolverRejectsIncompleteSnapshots() {
    class SnapshotBackend final : public RecordingBackend {
    public:
        std::string status = "available";
        bool truncated = false;
        MaiResult<DesktopNativeAccessibilitySnapshot> inspectElements(
            const DesktopNativeWindow& window, const std::function<bool()>& isCanceled) override {
            auto result = DesktopComputerBackend::inspectElements(window, isCanceled);
            if (result) {
                result.value().status = status;
                result.value().truncated = truncated;
            }
            return result;
        }
    };
    for (int incomplete = 0; incomplete < 3; ++incomplete) {
        auto backend = std::make_shared<SnapshotBackend>();
        backend->nativeElements = std::vector<DesktopNativeElement>{
            {"button", "Unique partial match", 120, 220, 100, 30, "partial-runtime", "partial-button"}};
        DesktopComputerController controller(backend);
        QTemporaryDir workspace;
        const auto observed = controller.observe("owner", "", workspace.path().toUtf8().toStdString(), true, 101, false);
        CHECK(observed);
        if (!observed) continue;
        const auto output = Json::parse(observed.value().json);
        DesktopComputerAction action;
        action.action = "click";
        action.windowId = 101;
        action.snapshotId = output.value("snapshot_id", std::string());
        action.elementId = output["elements"][0].value("element_id", std::string());
        backend->status = incomplete == 0 ? "partial" : incomplete == 1 ? "unavailable" : "available";
        backend->truncated = incomplete == 2;
        const auto result = controller.perform("owner", action);
        CHECK(!result);
        CHECK(backend->inputs.empty() && backend->activationCount == 0 && backend->inputSessionCreations == 0);
    }
}

void verifyElementToolValidationAndFailureFacts() {
    Fixture fixture;
    fixture.backend->nativeElements = std::vector<DesktopNativeElement>{
        {"button", "Observed button", 120, 220, 100, 30, "runtime-1", "fixture-button"}};
    auto controller = std::shared_ptr<DesktopComputerController>(&fixture.controller, [](DesktopComputerController*) {});
    MaiToolRegistry registry;
    registerDesktopComputerTools(registry, controller);
    const auto tool = registry.find("desktop_computer");
    CHECK(tool);
    if (!tool) return;
    MaiToolContext context;
    context.sessionId = "owner";
    context.root = fixture.workspace.path().toUtf8().toStdString();
    const auto observed = tool->execute(R"({"action":"observe","window_id":"101"})", context);
    CHECK(!observed.hasError());
    if (observed.hasError()) return;
    const auto output = Json::parse(observed.output());
    Json args{{"action", "click"}, {"window_id", "101"}, {"snapshot_id", output["snapshot_id"]},
              {"element_id", output["elements"][0].value("element_id", std::string())}};
    for (int bad = 0; bad < 4; ++bad) {
        auto invalid = args;
        if (bad == 0) invalid["x_ratio"] = 0.5;
        if (bad == 1) { invalid["x_ratio"] = 0.5; invalid["y_ratio"] = 0.5; }
        if (bad == 2) invalid["element_id"] = 2;
        if (bad == 3) invalid["element_id"] = "";
        const auto result = tool->execute(invalid.dump(), context);
        CHECK(result.hasError() && result.error().code() == MaiErrorCode::InvalidInput);
    }
    CHECK(fixture.backend->inputs.empty() && fixture.backend->activationCount == 0);
    fixture.backend->inputError = {MaiErrorCode::Busy, "fixture partial input"};
    const auto failed = tool->execute(args.dump(), context);
    CHECK(failed.hasError() && failed.error().code() == MaiErrorCode::Busy);
    const auto facts = Json::parse(failed.error().message(), nullptr, false);
    CHECK(facts.is_object());
    if (facts.is_object()) {
        CHECK(facts["input_dispatch_attempted"] == true);
        CHECK(facts["window_operation_attempted"] == true);
        CHECK(facts["needs_observation"] == true);
        CHECK(facts.contains("input_state") && !facts.contains("accepted_events"));
    }
    for (const auto& invalid : {Json{{"action", "observe"}, {"target", "display"}, {"display_id", "bad"}},
                                Json{{"action", "observe"}, {"target", "display"}, {"display_id", "11"},
                                     {"element_window_id", "bad"}}}) {
        const auto result = tool->execute(invalid.dump(), context);
        CHECK(result.hasError());
        if (result.hasError()) CHECK(result.error().message().find(invalid.contains("element_window_id") ?
                                     "element_window_id" : "display_id") != std::string::npos);
    }
}

void verifyElementStateChangesAtDispatchAndMissingDisplaySource() {
    {
        Fixture fixture;
        const auto action = observeNativeElement(fixture);
        fixture.backend->onActivate = [&] { (*fixture.backend->nativeElements)[0].x += 30; };
        const auto result = fixture.controller.perform("owner", action);
        CHECK(!result);
        CHECK(fixture.backend->inputs.empty());
    }
    for (const bool display : {false, true}) {
        Fixture fixture;
        const auto action = observeNativeElement(fixture, display);
        auto& element = (*fixture.backend->nativeElements)[0];
        element.label = "Replaced transiently";
        CHECK(!fixture.controller.perform("owner", action));
        element.label = "Observed button";
        CHECK(!fixture.controller.perform("owner", action));
        CHECK(fixture.backend->inputs.empty());
    }
    {
        Fixture fixture;
        const auto action = observeNativeElement(fixture, true);
        fixture.backend->onInput = [&] { fixture.backend->visible.clear(); };
        const auto result = fixture.controller.perform("owner", action);
        CHECK(result && result.value().dispatched);
        if (result) {
            const auto next = Json::parse(result.value().observation.json);
            CHECK(next["element_window_id"] == "101");
            CHECK(next["accessibility"]["status"] == "unavailable");
            CHECK(next["accessibility"]["limit_reason"] == "source_missing");
            auto coordinate = action;
            coordinate.elementId.clear();
            coordinate.snapshotId = next.value("snapshot_id", std::string());
            coordinate.xRatio = 0.2;
            coordinate.yRatio = 0.3;
            const auto again = fixture.controller.perform("owner", coordinate);
            CHECK(again && again.value().dispatched);
            if (again) {
                const auto last = Json::parse(again.value().observation.json);
                CHECK(last.value("element_window_id", std::string()) == "101");
            }
        }
        CHECK(fixture.backend->inputs.size() == 2 && fixture.backend->activationCount == 0);
    }
}

void verifyCanceledStateDryRunStillReleasesStoppedTurn() {
    Fixture fixture;
    const auto native = holdControl(fixture, false, "dry-state-turn");
    auto action = fixture.observe("restore");
    action.turnId = "dry-state-turn";
    auto controller = std::shared_ptr<DesktopComputerController>(&fixture.controller, [](DesktopComputerController*) {});
    MaiToolRegistry registry;
    registerDesktopComputerTools(registry, controller);
    const auto tool = registry.find("desktop_computer");
    CHECK(tool && native);
    if (!tool || !native) return;
    std::atomic<bool> canceled{true};
    MaiToolContext context;
    context.sessionId = "owner";
    context.turnId = action.turnId;
    context.cancel = &canceled;
    const auto result = tool->execute(Json{{"action", "restore"}, {"window_id", "101"},
        {"snapshot_id", action.snapshotId}, {"dry_run", true}}.dump(), context);
    CHECK(result.hasError() && result.error().code() == MaiErrorCode::Canceled);
    CHECK(native->releaseCalls() > 0 && native->state().empty());
    CHECK(fixture.backend->stateCalls == 0);
}

void verifyInvalidatedEvidenceProgressAndObserveDiagnostics() {
    for (const bool display : {false, true}) {
        Fixture fixture;
        const auto action = display ? fixture.observeDisplay() : fixture.observe();
        if (display) fixture.backend->availableDisplays[1].width += 20;
        else ++fixture.backend->visible[0].processId;
        DesktopComputerActionProgress progress;
        CHECK(!fixture.controller.perform("owner", action, &progress));
        CHECK(progress.needsObservation && !progress.inputDispatchAttempted && !progress.windowOperationAttempted);
    }
    Fixture fixture;
    auto controller = std::shared_ptr<DesktopComputerController>(&fixture.controller, [](DesktopComputerController*) {});
    MaiToolRegistry registry;
    registerDesktopComputerTools(registry, controller);
    const auto tool = registry.find("desktop_computer");
    CHECK(tool);
    if (!tool) return;
    MaiToolContext context;
    context.sessionId = "owner";
    context.root = fixture.workspace.path().toUtf8().toStdString();
    const std::vector<std::pair<Json, std::string>> invalid{
        {Json{{"action", "observe"}, {"window_id", "101"}, {"bogus", 1}}, "bogus"},
        {Json{{"action", "observe"}, {"window_id", "101"}, {"include_elements", "true"}}, "include_elements"},
        {Json{{"action", "observe"}, {"window_id", "101"}, {"window_title", "Isolated editor"}}, "exactly one"},
        {Json{{"action", "observe"}, {"window_id", "101"}, {"workspace", context.root}}, "workspace"},
        {Json{{"action", "observe"}, {"target", "display"}, {"display_id", 22}}, "display_id"},
        {Json{{"action", "observe"}, {"target", "display"}, {"display_id", "22"}, {"wait_ms", "0"}}, "wait_ms"}};
    for (const auto& entry : invalid) {
        const auto result = tool->execute(entry.first.dump(), context);
        CHECK(result.hasError() && result.error().message().find(entry.second) != std::string::npos);
    }
    context.root.clear();
    const auto host = tool->execute(R"({"action":"observe","window_id":"101"})", context);
    CHECK(host.hasError() && host.error().message().find("host workspace") != std::string::npos);
    CHECK(fixture.backend->events.empty() && fixture.backend->windowsCount == 0);
}

void verifyMissingSnapshotFieldIsDiagnosedBeforeBackendAccess() {
    for (const bool display : {false, true}) {
        Fixture fixture;
        const auto action = display ? observeNativeElement(fixture, true) : fixture.observe("restore");
        auto controller = std::shared_ptr<DesktopComputerController>(&fixture.controller,
                                                                     [](DesktopComputerController*) {});
        MaiToolRegistry registry;
        registerDesktopComputerTools(registry, controller);
        const auto tool = registry.find("desktop_computer");
        CHECK(tool);
        if (!tool) continue;
        MaiToolContext context;
        context.sessionId = "owner";
        context.root = fixture.workspace.path().toUtf8().toStdString();
        const Json args = display ? Json{{"action", "click"}, {"target", "display"},
                                         {"display_id", "11"}, {"element_id", action.elementId}}
                                  : Json{{"action", "restore"}, {"window_id", "101"}};
        const auto windows = fixture.backend->windowsCount;
        const auto captures = fixture.backend->captureCount;
        const auto displayCaptures = fixture.backend->displayCaptureCount;
        const auto elementReads = fixture.backend->elementReads;
        fixture.backend->events.clear();
        const auto result = tool->execute(args.dump(), context);
        CHECK(result.hasError() && result.error().code() == MaiErrorCode::InvalidInput);
        CHECK(result.error().message().find("snapshot_id is required") != std::string::npos);
        CHECK(fixture.backend->events.empty() && fixture.backend->inputs.empty());
        CHECK(fixture.backend->windowsCount == windows && fixture.backend->captureCount == captures &&
              fixture.backend->displayCaptureCount == displayCaptures && fixture.backend->elementReads == elementReads);
        CHECK(fixture.backend->activationCount == 0 && fixture.backend->stateCalls == 0 &&
              fixture.backend->inputSessionCreations == 0);
    }
}

void verifyWindowDiscoveryStateChangeDuringObservation() {
    for (int state = 0; state < 3; ++state) {
        for (const bool image : {false, true}) {
            Fixture fixture;
            if (image) fixture.backend->onCapture = [&] { makeWindowUnavailable(fixture.backend->visible.front(), state); };
            else fixture.backend->onWindows = [&](int count) {
                if (count == 2) makeWindowUnavailable(fixture.backend->visible.front(), state);
            };
            const auto observed = fixture.controller.observe("owner", "", fixture.workspace.path().toUtf8().toStdString(),
                                                               false, 101, image);
            CHECK(!observed);
            if (!observed) CHECK(observed.error().code() == MaiErrorCode::Busy ||
                                 observed.error().code() == MaiErrorCode::InvalidInput);
            CHECK(fixture.backend->captureCount == (image ? 1 : 0));
            CHECK(fixture.backend->activationCount == 0 && fixture.backend->inputSessionCreations == 0);
            CHECK(fixture.backend->inputs.empty());
        }
    }
}

void verifyWindowDiscoveryStateRevokesInputAndHeldControls() {
    for (int state = 0; state < 3; ++state) {
        for (const bool held : {false, true}) {
            Fixture fixture;
            const auto native = held ? holdControl(fixture, false) : nullptr;
            auto action = fixture.observe("click");
            action.turnId = "turn-current";
            const int captures = fixture.backend->captureCount;
            const int activations = fixture.backend->activationCount;
            const int sessions = fixture.backend->inputSessionCreations;
            const int calls = native ? native->inputCalls() : 0;
            makeWindowUnavailable(fixture.backend->visible.front(), state);
            const auto result = fixture.controller.perform("owner", action);
            CHECK(!result);
            if (!result) CHECK(result.error().code() == MaiErrorCode::Busy || result.error().code() == MaiErrorCode::InvalidInput);
            CHECK(fixture.backend->captureCount == captures && fixture.backend->activationCount == activations);
            CHECK(fixture.backend->inputSessionCreations == sessions && fixture.backend->inputs.empty());
            if (native) {
                CHECK(native->inputCalls() == calls && native->releaseCalls() > 0);
                CHECK(native->state().empty() && fixture.controller.inputState("owner").empty());
                auto& window = fixture.backend->visible.front();
                window.minimized = window.cloaked = false;
                window.boundsAvailable = true;
                const auto oldToken = fixture.controller.perform("owner", action);
                CHECK(!oldToken);
                CHECK(native->inputCalls() == calls && fixture.backend->activationCount == activations);
            }
        }
    }
}

void verifyWindowSelection() {
    Fixture fixture;
    auto duplicate = fixture.backend->visible.front();
    duplicate.id = 202;
    duplicate.processId = 5002;
    fixture.backend->visible.push_back(duplicate);
    const auto listed = fixture.controller.windows();
    CHECK(listed);
    if (listed) {
        CHECK(listed.value().size() == 2);
        CHECK(listed.value()[0].id == 101);
        CHECK(listed.value()[1].id == 202);
        CHECK(listed.value()[1].processId == 5002);
        CHECK(listed.value()[1].title == "Isolated editor");
        CHECK(listed.value()[1].width == 400);
    }
    const auto ambiguous =
        fixture.controller.observe("owner", duplicate.title, fixture.workspace.path().toUtf8().toStdString(), true);
    CHECK(!ambiguous);
    if (!ambiguous) CHECK(ambiguous.error().code() == MaiErrorCode::NotFound);
    CHECK(fixture.backend->captureCount == 0);
    const auto selected =
        fixture.controller.observe("owner", "", fixture.workspace.path().toUtf8().toStdString(), true, duplicate.id);
    CHECK(selected);
    if (selected) {
        const Json output = Json::parse(selected.value().json);
        CHECK(output["window_id"] == "202");
        CHECK(output["process_id"] == 5002);
        CHECK(output["bounds"]["width"] == 400);
        CHECK(output["elements"].size() == 1);
        CHECK(QImage(QString::fromStdString(selected.value().imagePath)).size() == QSize(20, 20));
    }
    CHECK(fixture.backend->capturedWindowId == 202);
    CHECK(fixture.backend->activationCount == 0);
    CHECK(fixture.backend->inputs.empty());
}

void verifyScreenshotHandoff() {
    Fixture fixture;
    // The ordinary API retains a controlled native-fixture capture for host
    // integration; model-facing observe is metadata only.
    MaiToolContext context;
    context.sessionId = "screenshot-delivery";
    context.root = fixture.workspace.path().toUtf8().toStdString();
    context.model = "deepseek-flash";
    const auto captured = fixture.controller.observe(context.sessionId, "", context.root, true, 101);
    CHECK(captured);
    if (!captured) return;
    const Json output = Json::parse(captured.value().json);
    const std::string relative = output.value("image_path", std::string());
    CHECK(!relative.empty());
    CHECK(QDir::isRelativePath(QString::fromStdString(relative)));
    if (relative.empty()) return;
    const QString saved = fixture.workspace.filePath(QString::fromStdString(relative));
    CHECK(QFileInfo(saved).canonicalFilePath() ==
          QFileInfo(QString::fromStdString(captured.value().imagePath)).canonicalFilePath());
    const auto delivered =
        makeMaiAgentSendMediaTool()->execute(Json{{"file_path", relative}, {"type", "image"}}.dump(), context);
    CHECK(!delivered.hasError());
    if (!delivered.hasError()) {
        const Json receipt = Json::parse(delivered.output());
        CHECK(receipt["path"] == relative);
        CHECK(receipt["type"] == "image");
    }
    CHECK(fixture.backend->captureCount == 1);
    CHECK(fixture.backend->inputs.empty());
}

void verifyScreenshotDirectoryBoundary() {
#if defined(Q_OS_WIN)
    Fixture fixture;
    QTemporaryDir outside;
    CHECK(outside.isValid());
    const QString link = fixture.workspace.filePath(QStringLiteral("computer"));
    if (!CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(link.utf16()), reinterpret_cast<LPCWSTR>(outside.path().utf16()),
                             0x3)) {
        std::fprintf(stderr, "computer directory link boundary not exercised: Win32 %lu\n", GetLastError());
        return;
    }
    const auto observed =
        fixture.controller.observe("owner", "", fixture.workspace.path().toUtf8().toStdString(), false, 101);
    CHECK(!observed);
    if (!observed) CHECK(observed.error().code() == MaiErrorCode::InvalidInput);
    CHECK(fixture.backend->captureCount == 0);
    CHECK(QDir(outside.path()).entryList(QDir::Files).isEmpty());
    CHECK(RemoveDirectoryW(reinterpret_cast<LPCWSTR>(link.utf16())) != 0);
#endif
}

void verifyScreenshotDirectoryIsValidatedBeforeCapture() {
    Fixture fixture;
    // A file occupying the output directory must fail before requesting pixels.
    QFile blocker(fixture.workspace.filePath(QStringLiteral("computer")));
    const bool opened = blocker.open(QIODevice::WriteOnly);
    CHECK(opened);
    if (!opened) return;
    blocker.close();

    const auto captured = fixture.controller.observe("screenshot-invalid-directory", "",
                                                     fixture.workspace.path().toUtf8().toStdString(), true, 101, true);
    CHECK(!captured);
    CHECK(fixture.backend->captureCount == 0);
    CHECK(QFileInfo(blocker).isFile());
}

void verifyScreenshotCancellationDoesNotPublishPixels() {
    Fixture fixture;
    std::atomic<bool> canceled{false};
    // Cancellation during a successful capture must be observed before saving
    // or publishing its pixels; an empty output directory may already exist.
    fixture.backend->onCapture = [&canceled] { canceled.store(true, std::memory_order_relaxed); };
    const auto captured = fixture.controller.observe("screenshot-canceled-during-capture", "",
                                                     fixture.workspace.path().toUtf8().toStdString(), true, 101, true,
                                                     [&canceled] { return canceled.load(std::memory_order_relaxed); });
    CHECK(canceled.load(std::memory_order_relaxed));
    CHECK(fixture.backend->captureCount == 1);
    CHECK(!captured);
    if (!captured) CHECK(captured.error().code() == MaiErrorCode::Canceled);
    CHECK(QDir(fixture.workspace.filePath(QStringLiteral("computer"))).entryList(QDir::Files).isEmpty());
}

void verifyObservationWithoutImage() {
    Fixture fixture;
    // Element-only observation still issues a fresh usable observation without
    // depending on capture support or creating a screenshot on disk.
    const auto observed =
        fixture.controller.observe("owner", "", fixture.workspace.path().toUtf8().toStdString(), true, 101, false);
    CHECK(observed);
    std::string firstSnapshot;
    if (observed) {
        CHECK(observed.value().imagePath.empty());
        const Json output = Json::parse(observed.value().json);
        CHECK(output["has_image"] == false);
        CHECK(output.value("image_path", std::string()).empty());
        CHECK(output["window_id"] == "101");
        CHECK(output["elements"].size() == 1);
        firstSnapshot = output.value("snapshot_id", std::string());
        CHECK(!firstSnapshot.empty());
    }
    CHECK(fixture.backend->captureCount == 0);
    CHECK(!QFileInfo::exists(fixture.workspace.filePath(QStringLiteral("computer"))));

    MaiToolRegistry registry;
    auto controller = std::make_shared<DesktopComputerController>(fixture.backend);
    registerDesktopComputerTools(registry, controller);
    MaiTool* tool = registry.find("desktop_computer");
    CHECK(tool);
    if (!tool) return;
    MaiToolContext context;
    context.sessionId = "element-only-tool-session";
    context.root = fixture.workspace.path().toUtf8().toStdString();
    context.model = "deepseek-flash";
    const auto result = tool->execute(R"({"action":"observe","window_id":"101","include_elements":true})", context);
    CHECK(!result.hasError());
    CHECK(result.images().empty());
    if (!result.hasError()) {
        const Json output = Json::parse(result.output());
        CHECK(output["has_image"] == false);
        CHECK(!output.contains("image_path"));
        CHECK(output["elements"].size() == 1);
        CHECK(!output.value("snapshot_id", std::string()).empty());
        CHECK(output.value("snapshot_id", std::string()) != firstSnapshot);
    }
    // Text-only and image-capable models receive the same metadata contract.
    for (const std::string model : {"glm-5.3", "local:qa"}) {
        context.model = model;
        const auto defaultResult =
            tool->execute(R"({"action":"observe","window_id":"101","include_elements":true})", context);
        CHECK(!defaultResult.hasError());
        CHECK(defaultResult.images().empty());
        if (!defaultResult.hasError()) {
            const Json output = Json::parse(defaultResult.output());
            CHECK(output["has_image"] == false);
            CHECK(!output.contains("image_path"));
            CHECK(output["elements"].size() == 1);
        }
    }
    for (const std::string model : {"deepseek-flash", "glm-5.3", "local:qa"}) {
        context.model = model;
        const auto explicitMetadata =
            tool->execute(Json{{"action", "observe"}, {"window_id", "101"}, {"include_image", false}}.dump(), context);
        CHECK(!explicitMetadata.hasError() && explicitMetadata.images().empty());
        for (const Json includeImage : {Json(nullptr), Json("false")}) {
            const auto unsupportedImage = tool->execute(
                Json{{"action", "observe"}, {"window_id", "101"}, {"include_image", includeImage}}.dump(), context);
            CHECK(unsupportedImage.hasError());
            CHECK(unsupportedImage.error().code() == MaiErrorCode::InvalidInput);
            CHECK(unsupportedImage.images().empty());
        }
    }
    CHECK(fixture.backend->captureCount == 0);
    CHECK(fixture.backend->activationCount == 0);
    CHECK(fixture.backend->inputs.empty());
    CHECK(!QFileInfo::exists(fixture.workspace.filePath(QStringLiteral("computer"))));
}

void verifyDispatchAndSnapshots() {
    Fixture fixture;
    auto action = fixture.observe("click");
    action.dryRun = true;
    const auto planned = fixture.controller.perform("owner", action);
    CHECK(planned);
    if (planned) {
        CHECK(planned.value().dryRun);
        CHECK(!planned.value().dispatched);
        CHECK(!planned.value().verified);
    }
    CHECK(fixture.backend->activationCount == 0);
    CHECK(fixture.backend->inputs.empty());
    action.dryRun = false;
    const auto clicked = fixture.controller.perform("owner", action);
    CHECK(clicked);
    if (clicked) {
        CHECK(clicked.value().state == "dispatched");
        CHECK(clicked.value().dispatched);
        CHECK(!clicked.value().verified);
        CHECK(!clicked.value().dryRun);
    }
    CHECK(fixture.backend->activationCount == 1);
    CHECK(fixture.backend->inputs.size() == 1);
    if (!fixture.backend->inputs.empty()) {
        CHECK(fixture.backend->inputs.back().kind == DesktopNativeInputKind::Click);
        CHECK(fixture.backend->inputs.back().expectedWindowId == 101);
        CHECK(fixture.backend->inputs.back().expectedProcessId == 5001);
        CHECK(std::abs(fixture.backend->inputs.back().x - 199.75) < 0.01);
        CHECK(std::abs(fixture.backend->inputs.back().y - 349.5) < 0.01);
    }
    const auto reused = fixture.controller.perform("owner", action);
    CHECK(!reused);
    if (!reused) CHECK(reused.error().code() == MaiErrorCode::InvalidInput);
    CHECK(fixture.backend->inputs.size() == 1);

    auto focus = fixture.observe("focus");
    const auto focused = fixture.controller.perform("owner", focus);
    CHECK(focused);
    if (focused) CHECK(!focused.value().verified);
    CHECK(fixture.backend->activationCount == 2);
    CHECK(fixture.backend->inputs.size() == 1);
    CHECK(!fixture.controller.perform("owner", focus));

    const std::pair<const char*, DesktopNativeInputKind> gestures[] = {
        {"double_click", DesktopNativeInputKind::DoubleClick},
        {"right_click", DesktopNativeInputKind::RightClick},
        {"drag", DesktopNativeInputKind::Drag}};
    for (const auto& gesture : gestures) {
        auto next = fixture.observe(gesture.first);
        next.durationMs = 100;
        const auto sent = fixture.controller.perform("owner", next);
        CHECK(sent);
        CHECK(fixture.backend->inputs.back().kind == gesture.second);
        if (gesture.second == DesktopNativeInputKind::Drag) {
            CHECK(std::abs(fixture.backend->inputs.back().toX - 399.25) < 0.01);
            CHECK(std::abs(fixture.backend->inputs.back().toY - 424.25) < 0.01);
            CHECK(fixture.backend->inputs.back().durationMs == 100);
        }
        CHECK(!fixture.controller.perform("owner", next));
    }
}

void verifyTextAndReadback() {
    Fixture fixture;
    const std::string chinese = "\xe7\xac\xac\xe4\xb8\x80\xe8\xa1\x8c\xe4\xb8\xad\xe6\x96\x87";
    auto replacement = fixture.observe("replace_text");
    replacement.text = chinese;
    const auto replaced = fixture.controller.perform("owner", replacement);
    CHECK(replaced);
    CHECK(fixture.backend->textValue == chinese);
    CHECK(fixture.backend->inputs.size() == 1);
    if (!fixture.backend->inputs.empty()) {
        CHECK(fixture.backend->inputs.back().kind == DesktopNativeInputKind::ReplaceFocusedText);
        CHECK(fixture.backend->inputs.back().text == chinese);
    }
    auto verify = fixture.observe("verify");
    verify.expectedText = chinese;
    const int activations = fixture.backend->activationCount;
    fixture.backend->events.clear();
    const auto matched = fixture.controller.perform("owner", verify);
    CHECK(matched);
    if (matched) {
        CHECK(matched.value().verified);
        CHECK(!matched.value().dispatched);
        CHECK(matched.value().actualText == chinese);
    }
    CHECK(fixture.backend->activationCount == activations);
    CHECK(fixture.backend->readCount == 1);
    const auto focusedEvent = std::find(fixture.backend->events.begin(), fixture.backend->events.end(), "isFocused");
    const auto textEvent = std::find(fixture.backend->events.begin(), fixture.backend->events.end(), "focusedText");
    CHECK(focusedEvent != fixture.backend->events.end());
    CHECK(textEvent != fixture.backend->events.end());
    CHECK(focusedEvent < textEvent);
    CHECK(!fixture.controller.perform("owner", verify));
    auto mismatch = fixture.observe("verify");
    mismatch.expectedText = "different expected text";
    const auto mismatched = fixture.controller.perform("owner", mismatch);
    CHECK(mismatched);
    if (mismatched) {
        CHECK(!mismatched.value().verified);
        CHECK(mismatched.value().actualText == chinese);
    }
    CHECK(fixture.backend->activationCount == activations);

    auto shortcut = fixture.observe("key");
    shortcut.key = "Shift+Home";
    CHECK(fixture.controller.perform("owner", shortcut));
    CHECK(fixture.backend->inputs.back().kind == DesktopNativeInputKind::Key);
    CHECK(fixture.backend->inputs.back().key == "Shift+Home");
    auto empty = fixture.observe("replace_text");
    empty.text.clear();
    CHECK(fixture.controller.perform("owner", empty));
    CHECK(fixture.backend->inputs.back().kind == DesktopNativeInputKind::ReplaceFocusedText);
    CHECK(fixture.backend->inputs.back().text.empty());
    CHECK(fixture.backend->textValue.empty());
    auto append = fixture.observe("type");
    append.text = chinese;
    CHECK(fixture.controller.perform("owner", append));
    CHECK(fixture.backend->inputs.back().kind == DesktopNativeInputKind::Type);
    CHECK(fixture.backend->textValue == chinese);
    auto forgotten = fixture.observe();
    CHECK(!fixture.controller.clear("owner"));
    CHECK(fixture.backend->textValue == chinese);
    CHECK(!fixture.controller.perform("owner", forgotten));
}

void verifyIdentityAndSyntaxFailures() {
    for (int changed = 0; changed < 4; ++changed) {
        Fixture fixture;
        auto action = fixture.observe();
        auto& window = fixture.backend->visible.front();
        if (changed == 0)
            ++window.id;
        else if (changed == 1)
            ++window.processId;
        else if (changed == 2)
            window.x += 20;
        else
            window.title = "A different window title";
        CHECK(!fixture.controller.perform("owner", action));
        CHECK(fixture.backend->activationCount == 0);
        CHECK(fixture.backend->inputs.empty());
    }
    Fixture fixture;
    auto original = fixture.observe();
    CHECK(!fixture.controller.perform("other-session", original));
    auto invalid = original;
    ++invalid.windowId;
    CHECK(!fixture.controller.perform("owner", invalid));
    invalid = original;
    invalid.xRatio = 1.5;
    CHECK(!fixture.controller.perform("owner", invalid));
    invalid = original;
    invalid.action = "key";
    invalid.key = "Ctrl++A";
    CHECK(!fixture.controller.perform("owner", invalid));
    invalid = original;
    invalid.action = "drag";
    invalid.durationMs = 0;
    CHECK(!fixture.controller.perform("owner", invalid));
    invalid.durationMs = 201;
    CHECK(!fixture.controller.perform("owner", invalid));
    invalid = original;
    invalid.action = "type";
    invalid.text = std::string(4001, 'x');
    CHECK(!fixture.controller.perform("owner", invalid));
    CHECK(fixture.backend->activationCount == 0);
    CHECK(fixture.backend->inputs.empty());
    // Correcting syntax reuses the same observation because nothing crossed
    // the OS boundary in the rejected requests.
    CHECK(fixture.controller.perform("owner", original));
    CHECK(fixture.backend->inputs.size() == 1);
}

void verifyFailureAndCancellationBoundaries() {
    {
        Fixture fixture;
        auto action = fixture.observe();
        std::atomic<bool> canceled{true};
        action.isCanceled = [&canceled] { return canceled.load(); };
        const auto stopped = fixture.controller.perform("owner", action);
        CHECK(!stopped);
        if (!stopped) CHECK(stopped.error().code() == MaiErrorCode::Canceled);
        CHECK(fixture.backend->activationCount == 0);
        CHECK(fixture.backend->inputs.empty());
        canceled = false;
        CHECK(!fixture.controller.perform("owner", action));
        action = fixture.observe();
        CHECK(fixture.controller.perform("owner", action));
    }
    {
        Fixture fixture;
        auto action = fixture.observe();
        std::atomic<bool> canceled{false};
        action.isCanceled = [&canceled] { return canceled.load(); };
        fixture.backend->onActivate = [&canceled] { canceled = true; };
        const auto stopped = fixture.controller.perform("owner", action);
        CHECK(!stopped);
        if (!stopped) CHECK(stopped.error().code() == MaiErrorCode::Canceled);
        CHECK(fixture.backend->activationCount == 1);
        CHECK(fixture.backend->inputs.empty());
        canceled = false;
        fixture.backend->onActivate = {};
        CHECK(!fixture.controller.perform("owner", action));
        CHECK(fixture.backend->activationCount == 1);
    }
    {
        Fixture fixture;
        auto action = fixture.observe();
        fixture.backend->activationError = {MaiErrorCode::Busy, "fixture activation denied"};
        const auto denied = fixture.controller.perform("owner", action);
        CHECK(!denied);
        if (!denied) CHECK(denied.error().code() == MaiErrorCode::Busy);
        CHECK(fixture.backend->inputs.empty());
        fixture.backend->activationError = {};
        CHECK(!fixture.controller.perform("owner", action));
        CHECK(fixture.backend->activationCount == 1);
    }
    {
        Fixture fixture;
        auto action = fixture.observe();
        fixture.backend->inputError = {MaiErrorCode::Internal, "fixture input failed after dispatch began"};
        const auto failed = fixture.controller.perform("owner", action);
        CHECK(!failed);
        if (!failed) CHECK(failed.error().code() == MaiErrorCode::Internal);
        fixture.backend->inputError = {};
        CHECK(!fixture.controller.perform("owner", action));
        CHECK(fixture.backend->inputs.size() == 1);
    }
    for (const char* kind : {"type", "replace_text", "verify"}) {
        Fixture fixture;
        auto action = fixture.observe(kind);
        action.text = "blocked secret";
        action.expectedText = "blocked secret";
        fixture.backend->secure = true;
        const auto secure = fixture.controller.perform("owner", action);
        CHECK(!secure);
        if (!secure) CHECK(secure.error().code() == MaiErrorCode::NotSupported);
        CHECK(fixture.backend->inputs.empty());
        CHECK(fixture.backend->readCount == 0);
    }
    {
        Fixture fixture;
        auto action = fixture.observe("verify");
        fixture.backend->focused = false;
        CHECK(!fixture.controller.perform("owner", action));
        CHECK(fixture.backend->readCount == 0);
        CHECK(fixture.backend->activationCount == 0);
    }
    {
        Fixture fixture;
        auto action = fixture.observe("verify");
        fixture.backend->readError = {MaiErrorCode::NotSupported, "fixture cannot safely read the focused control"};
        const auto unreadable = fixture.controller.perform("owner", action);
        CHECK(!unreadable);
        if (!unreadable) CHECK(unreadable.error().code() == MaiErrorCode::NotSupported);
        CHECK(fixture.backend->activationCount == 0);
    }
}

void verifyReadOnlyAndDryRunNeverCreateNativeSession() {
    for (const bool display : {false, true}) {
        Fixture fixture;
        auto action = display ? fixture.observeDisplay("click") : fixture.observe("click");
        CHECK(fixture.backend->inputSessionCreations == 0 && !fixture.backend->lastInputSession);
        action.dryRun = true;
        CHECK(fixture.controller.perform("owner", action));
        CHECK(fixture.backend->inputSessionCreations == 0 && fixture.backend->inputs.empty());
        MaiToolRegistry registry;
        auto controller = std::shared_ptr<DesktopComputerController>(&fixture.controller, [](DesktopComputerController*) {});
        registerDesktopComputerTools(registry, controller);
        auto* tool = registry.find("desktop_computer");
        CHECK(tool);
        if (!tool) continue;
        MaiToolContext context;
        context.sessionId = "owner";
        for (const char* query : {R"({"action":"input_capabilities"})", R"({"action":"list_keys"})",
                                 R"({"action":"input_state"})"})
            CHECK(!tool->execute(query, context).hasError());
        CHECK(fixture.backend->inputSessionCreations == 0 && !fixture.backend->lastInputSession);
    }
}

void verifyOneShotNativeAdapterPreservesSpyAndRejectsIndependentHolds() {
    {
        Fixture fixture;
        auto action = fixture.observe("click");
        CHECK(fixture.controller.perform("owner", action));
        CHECK(fixture.backend->inputSessionCreations == 1 && fixture.backend->inputs.size() == 1);
        CHECK(fixture.backend->lastInputSession && fixture.backend->lastInputSession->inputCalls() == 1);
    }
    for (const char* kind : {"key_down", "mouse_down"}) {
        Fixture fixture;
        auto action = fixture.observe(kind);
        action.key = "Ctrl";
        const auto rejected = fixture.controller.perform("owner", action);
        CHECK(!rejected);
        if (!rejected) CHECK(rejected.error().code() == MaiErrorCode::NotSupported);
        CHECK(fixture.backend->inputs.empty() && fixture.controller.inputState("owner").empty());
    }
    RecordingInputSession oneShot(false);
    DesktopNativeInput up;
    up.kind = DesktopNativeInputKind::KeyUp;
    up.key = "Ctrl";
    CHECK(oneShot.input(up).code() == MaiErrorCode::NotSupported);
    up.kind = DesktopNativeInputKind::MouseUp;
    CHECK(oneShot.input(up).code() == MaiErrorCode::NotSupported);
}

void verifyInputEventsLosslessParsing() {
    const auto omitted = maiDesktopParseInputBatch(R"([{"type":"relative_move","delta_x":1,"delta_y":0}])");
    CHECK(omitted && omitted.value().options.inheritExisting);
    const auto explicitDefault =
        maiDesktopParseInputBatch(R"([{"type":"relative_move","delta_x":1,"delta_y":0}])", "{}");
    CHECK(explicitDefault && !explicitDefault.value().options.inheritExisting);
    CHECK(!maiDesktopParseInputBatch(R"([{"type":"relative_move","delta_x":1,"delta_y":0}])",
                                     R"({"inherit_existing":true})"));
    const auto parsed = maiDesktopParseInputBatch(
        R"([
        {"type":"absolute_move","x_ratio":0,"y_ratio":1},
        {"type":"relative_move","delta_x":-32768,"delta_y":32767},
        {"type":"mouse_button","button":"forward","release":false},
        {"type":"wheel","axis":"horizontal","delta_120":30},
        {"type":"raw_key","code":259,"flags":1,"modifiers":31,"release":false},
        {"type":"unicode_text","text":"\u4e2d\u6587"},
        {"type":"touch","contacts":[{"phase":"down","pointer_id":4294967295,"x_ratio":0.25,"y_ratio":0.75,
          "pressure_or_distance":0.8,"contact_area_major":0.2,"contact_area_minor":0.1,"rotation":null}]},
        {"type":"pen","phase":"hover","tool_type":2,"buttons":7,"tilt":90,"rotation":360,
          "x_ratio":0.1,"y_ratio":0.9,"pressure_or_distance":0.2}
    ])",
        R"({"keyboard_enabled":false,"mouse_enabled":false,"native_pen_touch_enabled":false,
              "always_send_scan_codes":false,"right_alt_to_meta":true,"high_resolution_scroll":false,
              "keyboard_repeat":false,"repeat_delay_ms":0,"repeat_period_ms":1,"key_bindings":[[65535,1]]})");
    CHECK(parsed);
    if (parsed) {
        const auto& batch = parsed.value();
        CHECK(batch.events.size() == 8);
        if (batch.events.size() == 8) {
            CHECK(batch.events[1].deltaX == -32768 && batch.events[1].deltaY == 32767);
            CHECK(batch.events[2].button == 5);
            CHECK(batch.events[3].horizontal && batch.events[3].wheelDelta120 == 30);
            CHECK(batch.events[4].key.code == 259 && batch.events[4].key.modifiers == 31);
            CHECK(batch.events[6].contacts[0].pointerId == 0xffffffffu);
            CHECK(batch.events[6].contacts[0].rotation == 0xffff);
            CHECK(batch.events[7].pen.toolType == 2 && batch.events[7].pen.buttons == 7);
        }
        CHECK(!batch.options.keyboardEnabled && !batch.options.mouseEnabled);
        CHECK(!batch.options.nativePenTouchEnabled && !batch.options.alwaysSendScanCodes);
        CHECK(batch.options.rightAltToMeta && !batch.options.highResolutionScroll);
        CHECK(!batch.options.keyboardRepeat && batch.options.repeatDelayMs == 0 && batch.options.repeatPeriodMs == 1);
        CHECK(batch.options.keyBindings.size() == 1 && batch.options.keyBindings[0][0] == 65535);
    }

    // These values are legal JSON numbers but cannot be narrowed to the public
    // integer widths. They must fail before a platform session is even created.
    for (const char* bad :
         {R"([{"type":"raw_key","code":65536,"release":false}])", R"([{"type":"raw_key","code":-1,"release":false}])",
          R"([{"type":"raw_key","code":65.5,"release":false}])",
          R"([{"type":"raw_key","code":65,"flags":256,"release":false}])",
          R"([{"type":"raw_key","code":65,"modifiers":256,"release":false}])",
          R"([{"type":"raw_key","code":65,"repeat":true,"release":false}])",
          R"([{"type":"relative_move","delta_x":2147483648,"delta_y":0}])",
          R"([{"type":"wheel","axis":"vertical","delta_120":1.5}])",
          R"([{"type":"touch","contacts":[{"phase":"up","pointer_id":4294967296}]}])",
          R"([{"type":"pen","phase":"hover","x_ratio":0,"y_ratio":0,"tilt":256}])",
          R"([{"type":"relative_move","delta_x":1,"delta_y":1,"text":"unused"}])"}) {
        const auto rejected = maiDesktopParseInputBatch(bad);
        CHECK(!rejected);
        if (!rejected) CHECK(rejected.error().code() == MaiErrorCode::InvalidInput);
    }
    CHECK(!maiDesktopParseInputBatch(R"([{"type":"relative_move","delta_x":1,"delta_y":1}])",
                                     R"({"keyboard_enabled":1})"));
    CHECK(!maiDesktopInputSequence("18446744073709551616"));
    CHECK(maiDesktopInputSequence("18446744073709551615"));
}

// Virtual profiles/driver selection, retained-pad reset and SDK feedback payloads
// are explicitly deferred. Their exact former source is archived with the pre-migration
// file in .cache/desktop-contracts-system-native-20261116; they are not claimed to pass.
void verifyModelOptionsOmissionInheritsButOrdinaryDefaultsReset() {
    for (const bool model : {true, false}) {
        auto backend = std::make_shared<RecordingBackend>();
        backend->statefulInput = true;
        backend->composeInputSession = true;
        auto controller = std::make_shared<DesktopComputerController>(backend);
        QTemporaryDir workspace;
        MaiToolRegistry registry;
        registerDesktopComputerTools(registry, controller);
        auto* tool = registry.find("desktop_computer");
        CHECK(tool && workspace.isValid());
        if (!tool || !workspace.isValid()) continue;
        MaiToolContext context;
        context.sessionId = model ? "inherited-model-options" : "explicit-ordinary-options";
        context.turnId = "configuration-turn";
        context.root = workspace.path().toUtf8().toStdString();
        const auto observe = [&] {
            const auto observed = controller->observe(context.sessionId, "", context.root, false, 101, false, {}, context.turnId);
            CHECK(observed);
            return observed ? Json::parse(observed.value().json)["snapshot_id"].get<std::string>() : std::string();
        };
        auto snapshot = observe();
        DesktopComputerAction first;
        first.action = "input_events"; first.windowId = 101; first.snapshotId = snapshot; first.turnId = context.turnId;
        first.inputOptions.keyboardRepeat = false;
        first.inputOptions.highResolutionScroll = false;
        first.inputOptions.repeatDelayMs = 73;
        first.inputOptions.repeatPeriodMs = 17;
        first.inputOptions.keyBindings = {{65, 66}};
        DesktopInputEvent held;
        held.kind = DesktopInputEventKind::MouseButton;
        held.button = 1;
        first.events = {held};
        if (model) {
            const Json args{{"action", "input_events"}, {"window_id", "101"}, {"snapshot_id", snapshot},
                {"events", Json::array({Json{{"type", "mouse_button"}, {"button", "left"}, {"release", false}}})},
                {"input_options", {{"keyboard_repeat", false}, {"high_resolution_scroll", false},
                    {"repeat_delay_ms", 73}, {"repeat_period_ms", 17}, {"key_bindings", {{65, 66}}}}}};
            CHECK(!tool->execute(args.dump(), context).hasError());
        } else {
            CHECK(!first.inputOptions.inheritExisting);
            CHECK(controller->perform(context.sessionId, first));
        }
        const auto native = backend->lastInputSession;
        CHECK(native && native->state().buttonsDown == std::vector<std::string>{"left"});
        if (!native) continue;
        CHECK(!native->options().keyboardRepeat && !native->options().highResolutionScroll);
        CHECK(native->options().repeatDelayMs == 73 && native->options().repeatPeriodMs == 17);
        CHECK(native->options().keyBindings == (std::vector<std::array<std::uint16_t, 2>>{{65, 66}}));
        snapshot = observe();
        const Json move{{"action", "input_events"}, {"window_id", "101"}, {"snapshot_id", snapshot},
            {"events", Json::array({Json{{"type", "relative_move"}, {"delta_x", 1}, {"delta_y", 0}}})}};
        if (model) {
            CHECK(!tool->execute(move.dump(), context).hasError());
            CHECK(!native->options().keyboardRepeat && !native->options().highResolutionScroll);
            CHECK(native->options().keyBindings == (std::vector<std::array<std::uint16_t, 2>>{{65, 66}}));
            auto reset = move;
            reset["snapshot_id"] = observe();
            reset["input_options"] = Json::object();
            CHECK(!tool->execute(reset.dump(), context).hasError());
        } else {
            DesktopComputerAction reset;
            reset.action = "input_events"; reset.windowId = 101; reset.snapshotId = snapshot; reset.turnId = context.turnId;
            DesktopInputEvent relative; relative.kind = DesktopInputEventKind::RelativeMove; relative.deltaX = 1;
            reset.events = {relative};
            CHECK(!reset.inputOptions.inheritExisting);
            CHECK(controller->perform(context.sessionId, reset));
        }
        CHECK(native->options().keyboardRepeat && native->options().highResolutionScroll);
        CHECK(native->options().repeatDelayMs == 500 && native->options().repeatPeriodMs == 1000.0 / 24.9);
        CHECK(native->options().keyBindings.empty() && !native->options().inheritExisting);
        CHECK(backend->lastInputSession == native && backend->inputSessionCreations == 1);
        CHECK(native->state().buttonsDown == std::vector<std::string>{"left"});
        CHECK(native->releaseCalls() == 0 && native->resetCalls() == 0);
        CHECK(!controller->releaseInput(context.sessionId, context.turnId));
        CHECK(controller->inputState(context.sessionId).empty());
    }
}

void verifyResetIsBusyDuringRawKeyDispatch() {
    auto backend = std::make_shared<RecordingBackend>();
    backend->statefulInput = true;
    DesktopComputerController controller(backend);
    QTemporaryDir workspace;
    const std::string id = "owned-raw-reset-race", turn = "same-task";
    const auto observe = [&] {
        const auto observation = controller.observe(id, "", workspace.path().toStdString(), false, 101, false, {}, turn);
        CHECK(observation);
        return observation ? Json::parse(observation.value().json)["snapshot_id"].get<std::string>() : std::string();
    };
    DesktopComputerAction action;
    action.action = "input_events"; action.windowId = 101; action.turnId = turn; action.snapshotId = observe();
    DesktopInputEvent event; event.kind = DesktopInputEventKind::RawKey; event.key.code = 65;
    action.events = {event};
    CHECK(controller.perform(id, action));
    const auto native = backend->lastInputSession;
    CHECK(native && native->state().rawKeys.size() == 1);
    if (!native) return;
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, proceed = false;
    native->failBatchIfReset = true;
    native->beforeBatch = [&] {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true; changed.notify_all();
        changed.wait(lock, [&] { return proceed; });
    };
    action.snapshotId = observe(); action.events[0].key.code = 66;
    MaiError failure;
    std::thread pending([&] {
        const auto result = controller.perform(id, action);
        failure = result ? MaiError{} : result.error();
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        CHECK(changed.wait_for(lock, std::chrono::seconds(2), [&] { return entered; }));
    }
    const auto reset = controller.resetInput(id, turn);
    const auto keysAfterReset = native->state().rawKeys.size();
    const int resetsAfterBusy = native->resetCalls(), releasesAfterBusy = native->releaseCalls();
    { std::lock_guard<std::mutex> lock(mutex); proceed = true; }
    changed.notify_all(); pending.join();
    native->beforeBatch = {};
    CHECK(reset.code() == MaiErrorCode::Busy && keysAfterReset == 1);
    CHECK(resetsAfterBusy == 0 && releasesAfterBusy == 0);
    CHECK(failure.code() == MaiErrorCode::Ok);
    CHECK(native->state().rawKeys.size() == 2 && controller.inputState(id).rawKeys.size() == 2);
    CHECK(backend->lastInputSession == native && backend->inputSessionCreations == 1);
    CHECK(native->resetCalls() == 0 && native->releaseCalls() == 0);
    CHECK(!controller.resetInput(id, turn));
    CHECK(native->resetCalls() == 1 && native->state().empty());
}

void verifyResetInputsRequiresNoObservationAndClearsKeysAndMouse() {
    auto backend = std::make_shared<RecordingBackend>();
    backend->statefulInput = true;
    auto controller = std::make_shared<DesktopComputerController>(backend);
    QTemporaryDir workspace;
    MaiToolRegistry registry;
    registerDesktopComputerTools(registry, controller);
    auto* tool = registry.find("desktop_computer");
    CHECK(tool && workspace.isValid());
    if (!tool || !workspace.isValid()) return;
    MaiToolContext context;
    context.sessionId = "reset-native-controls-model"; context.turnId = "reset-turn";
    context.root = workspace.path().toUtf8().toStdString();
    const auto observed = controller->observe(context.sessionId, "", context.root, false, 101, false, {}, context.turnId);
    CHECK(observed);
    if (!observed) return;
    DesktopComputerAction action;
    action.action = "input_events"; action.windowId = 101; action.turnId = context.turnId;
    action.snapshotId = Json::parse(observed.value().json)["snapshot_id"].get<std::string>();
    DesktopInputEvent key; key.kind = DesktopInputEventKind::RawKey; key.key.code = 65;
    DesktopInputEvent mouse; mouse.kind = DesktopInputEventKind::MouseButton; mouse.button = 1;
    action.events = {key, mouse};
    CHECK(controller->perform(context.sessionId, action));
    const auto native = backend->lastInputSession;
    CHECK(native && native->state().rawKeys.size() == 1 && native->state().buttonsDown.size() == 1);
    if (!native) return;
    const int activations = backend->activationCount, captures = backend->captureCount;
    backend->focused = false; backend->secure = true;
    context.root.clear(); // Reset has no observation/workspace requirement.
    const std::string reset = R"({"action":"reset_inputs"})";
    CHECK(!tool->requiresPerCallApproval(reset));
    CHECK(contains(Json::parse(tool->parametersSchema())["properties"]["action"]["enum"], "reset_inputs"));
    auto oldTurn = context; oldTurn.turnId = "old-turn";
    CHECK(!tool->execute(reset, oldTurn).hasError());
    CHECK(native->resetCalls() == 0 && !native->state().empty());
    const auto actual = tool->execute(reset, context);
    CHECK(!actual.hasError());
    if (!actual.hasError()) CHECK(Json::parse(actual.output())["state"] == "inputs_reset");
    CHECK(native->resetCalls() == 1 && native->state().empty() && controller->inputState(context.sessionId).empty());
    CHECK(backend->lastInputSession == native && backend->inputSessionCreations == 1);
    CHECK(backend->activationCount == activations && backend->captureCount == captures);
    const auto bad = tool->execute(R"({"action":"reset_inputs","snapshot_id":"wrong"})", context);
    CHECK(bad.hasError() && native->resetCalls() == 1);
    CHECK(!tool->execute(R"({"action":"release_all"})", context).hasError());
    CHECK(controller->inputState(context.sessionId).empty());
}

void verifyInputEventsControllerAndTool() {
    {
        Fixture fixture;
        fixture.backend->statefulInput = true;
        // A partially off-screen window must not enlarge the OS input space.
        fixture.backend->visible.front().x = -1600;
        auto action = fixture.observe("input_events");
        DesktopInputEvent move;
        move.kind = DesktopInputEventKind::RelativeMove;
        move.deltaX = 3;
        move.deltaY = -2;
        DesktopInputEvent wheel;
        wheel.kind = DesktopInputEventKind::Wheel;
        wheel.wheelDelta120 = 30;
        action.events = {move, wheel};
        const auto result = fixture.controller.perform("owner", action);
        CHECK(result && result.value().acceptedEvents == 2);
        const auto native = fixture.backend->lastInputSession;
        CHECK(native && native->batchCalls() == 1);
        if (native) {
            const auto batch = native->batch();
            CHECK(batch.size() == 2 && batch[0].deltaX == 3 && batch[1].wheelDelta120 == 30);
            CHECK(native->scope().expectedWindowId == 101 && native->scope().expectedProcessId == 5001);
            CHECK(native->port().x == -1600 && native->port().y == 200);
            CHECK(native->port().environmentX == -1280 && native->port().environmentY == -120);
        }
        CHECK(fixture.backend->inputs.empty());  // Typed primitives never become legacy absolute moves.
        CHECK(!fixture.controller.perform("owner", action));
    }
    {
        DisplayToolFixture fixture;
        fixture.backend->statefulInput = true;
        if (!fixture.tool) return;
        const auto observed =
            fixture.tool->execute(R"({"action":"observe","target":"display","display_id":"22"})", fixture.context);
        CHECK(!observed.hasError());
        if (observed.hasError()) return;
        const auto snapshot = Json::parse(observed.output())["snapshot_id"];
        Json args{{"action", "input_events"},
                  {"target", "display"},
                  {"display_id", "22"},
                  {"snapshot_id", snapshot},
                  {"events", Json::array({Json{{"type", "relative_move"}, {"delta_x", 3}, {"delta_y", -2}},
                                          Json{{"type", "wheel"}, {"axis", "horizontal"}, {"delta_120", 30}}})}};
        const auto result = fixture.tool->execute(args.dump(), fixture.context);
        CHECK(!result.hasError());
        if (!result.hasError()) {
            const auto output = Json::parse(result.output());
            CHECK(output["accepted_events"] == 2);
            CHECK(output["observation"]["snapshot_id"] != snapshot);
            CHECK(result.images().size() == 1);
        }
        CHECK(fixture.backend->displayCaptureCount == 2);
        CHECK(fixture.backend->lastInputSession && fixture.backend->lastInputSession->batchCalls() == 1);
        CHECK(fixture.backend->inputs.empty());
    }
    {
        Fixture fixture;
        fixture.backend->statefulInput = true;
        auto action = fixture.observe("input_events");
        action.turnId = "current";
        DesktopInputEvent key;
        key.kind = DesktopInputEventKind::RawKey;
        key.key.code = 65;
        action.events = {key};
        CHECK(fixture.controller.perform("owner", action));
        const auto native = fixture.backend->lastInputSession;
        CHECK(native && !native->state().empty());
        DesktopComputerAction release;
        release.action = "input_events";
        key.key.release = true;
        release.events = {key};
        release.turnId = "old";
        CHECK(fixture.controller.perform("owner", release));
        CHECK(native && native->batchCalls() == 1 && !native->state().empty());
        release.turnId = "current";
        release.dryRun = true;
        CHECK(fixture.controller.perform("owner", release));
        CHECK(native && native->batchCalls() == 1 && !native->state().empty());
        release.dryRun = false;
        CHECK(fixture.controller.perform("owner", release));
        CHECK(native && native->batchCalls() == 2 && native->state().empty());
    }
    {
        Fixture fixture;
        fixture.backend->statefulInput = true;
        fixture.backend->secure = true;
        auto action = fixture.observe("input_events");
        DesktopInputEvent key;
        key.kind = DesktopInputEventKind::RawKey;
        key.key.code = 65;
        action.events = {key};
        const auto result = fixture.controller.perform("owner", action);
        CHECK(!result);
        if (!result) CHECK(result.error().code() == MaiErrorCode::NotSupported);
        CHECK(!fixture.backend->lastInputSession);
        action = fixture.observe("input_events");
        key.key.code = 0x11;
        action.events = {key};
        action.inputOptions.keyBindings = {{0x11, 65}};
        const auto remapped = fixture.controller.perform("owner", action);
        CHECK(!remapped);
        if (!remapped) CHECK(remapped.error().code() == MaiErrorCode::NotSupported);
        CHECK(!fixture.backend->lastInputSession);
    }
}

void verifyFractionalRepeatConfigurationCodec() {
    const std::string events = R"([{"type":"relative_move","delta_x":1,"delta_y":0}])";
    const auto schema = Json::parse(maiDesktopComputerInputSchema(R"({"properties":{"action":{"enum":["move"]}}})"));
    const auto configuration = schema["properties"]["input_options"]["properties"];
    CHECK(configuration["repeat_delay_ms"]["maximum"] == 2147483647);
    CHECK(configuration["repeat_period_ms"]["type"] == "number");
    CHECK(configuration["repeat_period_ms"].value("exclusiveMinimum", -1) == 0);
    CHECK(configuration["repeat_period_ms"]["maximum"] == 2147483647);
    for (const double period : {1.5, 1000.0 / 24.9, 1500.25, 2147483647.0}) {
        const auto parsed = maiDesktopParseInputBatch(events, Json{{"repeat_period_ms", period}}.dump());
        CHECK(parsed);
        if (parsed) CHECK(parsed.value().options.repeatPeriodMs == period);
    }
    const auto delayed = maiDesktopParseInputBatch(events, R"({"repeat_delay_ms":2147483647})");
    CHECK(delayed && delayed.value().options.repeatDelayMs == 2147483647);
    const auto reset = maiDesktopParseInputBatch(events, "{}");
    CHECK(reset && !reset.value().options.inheritExisting && reset.value().options.repeatPeriodMs == 1000.0 / 24.9);
    for (const char* bad :
         {R"({"repeat_period_ms":0})", R"({"repeat_period_ms":-0.25})", R"({"repeat_period_ms":2147483648})",
          R"({"repeat_period_ms":1e309})", R"({"repeat_period_ms":"Infinity"})", R"({"repeat_delay_ms":2147483648})",
          R"({"repeat_delay_ms":0.5})"}) {
        const auto parsed = maiDesktopParseInputBatch(events, bad);
        CHECK(!parsed);
        if (!parsed) CHECK(parsed.error().code() == MaiErrorCode::InvalidInput);
    }
}

void verifyDeferredVirtualInputContract() {
    const auto schema = Json::parse(maiDesktopComputerInputSchema(
        R"({"type":"object","properties":{"action":{"type":"string","enum":["move","input_state","reset_inputs","release_all"]}}})"));
    const auto& properties = schema["properties"];
    const auto& actions = properties["action"]["enum"];
    for (const char* action : {"move", "input_state", "reset_inputs", "release_all", "input_events", "input_capabilities"})
        CHECK(contains(actions, action));
    CHECK(!contains(actions, "feedback"));
    CHECK(!properties.contains("after_sequence"));
    CHECK(!properties.contains("limit"));

    Json names = Json::array();
    for (const auto& variant : properties["events"]["items"]["oneOf"])
        names.push_back(variant["properties"]["type"]["enum"][0]);
    CHECK(names.size() == 8);
    for (const char* type : {"absolute_move", "relative_move", "mouse_button", "wheel", "raw_key", "unicode_text", "touch", "pen"})
        CHECK(contains(names, type));
    const auto& configuration = properties["input_options"]["properties"];
    CHECK(configuration.size() == 10);
    for (const char* name : {"keyboard_enabled", "mouse_enabled", "native_pen_touch_enabled", "always_send_scan_codes",
                             "right_alt_to_meta", "high_resolution_scroll", "keyboard_repeat", "repeat_delay_ms",
                             "repeat_period_ms", "key_bindings"})
        CHECK(configuration.contains(name));
    for (const char* name : {"controller_enabled", "ds4_back_as_touchpad", "motion_as_playstation", "touchpad_as_playstation",
                             "randomize_controller_mac", "driver", "default_profile", "back_button_timeout_ms"})
        CHECK(!configuration.contains(name));

    // Deferred fields must be refused even with a historical default value.
    // Parsing the complete batch first also prevents a preceding ordinary event
    // from reaching native input before a deferred event is discovered.
    for (const char* gamepad : {
             R"({"type":"gamepad_attach","controller_id":0})",
             R"({"type":"gamepad_state","controller_id":0})",
             R"({"type":"gamepad_touch","controller_id":0,"contact":{"phase":"down","pointer_id":1,"x_ratio":0.5,"y_ratio":0.5}})",
             R"({"type":"gamepad_motion","controller_id":0,"motion_type":1,"x":0,"y":0,"z":0})",
             R"({"type":"gamepad_battery","controller_id":0,"state":1})",
             R"({"type":"gamepad_detach","controller_id":0})",
             R"({"type":"gamepad_rebind","controller_id":0})"}) {
        const auto deferred = maiDesktopParseInputBatch(
            std::string(R"([{"type":"relative_move","delta_x":1,"delta_y":0},)") + gamepad + "]");
        CHECK(!deferred);
        if (!deferred) CHECK(deferred.error().code() == MaiErrorCode::NotSupported);
    }
    for (const char* config : {R"({"controller_enabled":true})", R"({"ds4_back_as_touchpad":false})",
                               R"({"motion_as_playstation":true})", R"({"touchpad_as_playstation":true})",
                               R"({"randomize_controller_mac":true})", R"({"driver":"auto"})",
                               R"({"default_profile":"x360"})", R"({"back_button_timeout_ms":-1})"}) {
        const auto deferred = maiDesktopParseInputBatch(
            R"([{"type":"relative_move","delta_x":1,"delta_y":0}])", config);
        CHECK(!deferred);
        if (!deferred) CHECK(deferred.error().code() == MaiErrorCode::NotSupported);
    }
}

void verifyInputEventsProtocolDiscovery() {
    auto backend = std::make_shared<RecordingBackend>();
    auto controller = std::make_shared<DesktopComputerController>(backend);
    MaiToolRegistry registry;
    registerDesktopComputerTools(registry, controller);
    MaiTool* tool = registry.find("desktop_computer");
    CHECK(tool);
    if (!tool) return;

    const auto schema = Json::parse(tool->parametersSchema(), nullptr, false);
    CHECK(schema.is_object());
    if (schema.is_object()) {
        const auto properties = schema.value("properties", Json::object());
        const auto actions = properties.value("action", Json::object()).value("enum", Json::array());
        for (const char* action : {"input_events", "input_capabilities"}) CHECK(contains(actions, action));
        CHECK(properties.value("events", Json::object()).value("type", std::string()) == "array");
        Json eventNames = Json::array();
        for (const auto& variant :
             properties.value("events", Json::object()).value("items", Json::object()).value("oneOf", Json::array())) {
            const auto names =
                variant.value("properties", Json::object()).value("type", Json::object()).value("enum", Json::array());
            if (names.size() == 1) eventNames.push_back(names[0]);
        }
        // Schema syntax includes the six ordinary primitives and two system
        // candidates. It does not assert that touch/pen delivery is accepted.
        CHECK(eventNames.size() == 8);
        for (const char* name : {"absolute_move", "relative_move", "mouse_button", "wheel", "raw_key", "unicode_text", "touch", "pen"})
            CHECK(contains(eventNames, name));
        CHECK(!contains(actions, "feedback"));
    }

    MaiToolContext context;
    context.sessionId = "input-protocol-without-observation";
    // Capability discovery must report the connected backend's actual support
    // without capturing, activating, or posting even a probe input event.
    const auto capability = tool->execute(R"({"action":"input_capabilities"})", context);
    CHECK(!capability.hasError());
    CHECK(capability.images().empty());
    if (!capability.hasError()) CHECK(Json::parse(capability.output(), nullptr, false).is_object());
    CHECK(backend->events.empty());
    CHECK(backend->inputs.empty());
    CHECK(backend->activationCount == 0);
    CHECK(backend->captureCount == 0);
    CHECK(backend->displayCaptureCount == 0);

    const auto extra = tool->execute(R"({"action":"input_capabilities","text":"unused"})", context);
    CHECK(extra.hasError());
    if (extra.hasError()) CHECK(extra.error().code() == MaiErrorCode::InvalidInput);
    for (const char* deferred : {
             R"({"action":"input_events","events":[{"type":"relative_move","delta_x":1,"delta_y":0},{"type":"gamepad_attach","controller_id":0}]})",
             R"({"action":"input_events","events":[{"type":"relative_move","delta_x":1,"delta_y":0}],"input_options":{"driver":"auto"}})",
             R"({"action":"feedback"})"}) {
        const auto result = tool->execute(deferred, context);
        CHECK(result.hasError());
        if (result.hasError()) CHECK(result.error().code() == MaiErrorCode::NotSupported);
    }
    CHECK(backend->inputSessionCreations == 0);
    CHECK(backend->events.empty() && backend->inputs.empty());
    CHECK(backend->activationCount == 0 && backend->captureCount == 0 && backend->displayCaptureCount == 0);
}

void verifyPublicContract() {
    MaiToolRegistry registry;
    auto controller = std::make_shared<DesktopComputerController>();
    registerDesktopComputerTools(registry, controller);
    MaiTool* tool = registry.find("desktop_computer");
    CHECK(tool);
    if (!tool) return;

    const Json schema = Json::parse(tool->parametersSchema(), nullptr, false);
    CHECK(schema.is_object());
    if (!schema.is_object()) return;
    const Json properties = schema.value("properties", Json::object());
    CHECK(properties.value("include_image", Json::object()).value("type", std::string()) == "boolean");
    CHECK(properties.value("execution_window_id", Json::object()).value("type", std::string()) == "string");
    const Json actions = properties.value("action", Json::object()).value("enum", Json{});
    // The public contract exposes routine desktop operations without needing a
    // native session. Native dispatch belongs to the separate fixture test.
    for (const char* action : {"list", "observe", "focus", "click", "double_click", "right_click", "drag", "type",
                               "key", "scroll", "replace_text", "verify", "clear"}) {
        if (!contains(actions, action)) {
            std::fprintf(stderr, "desktop_computer schema is missing action: %s\n", action);
            ++failures;
        }
    }

    const Json key = properties.value("key", Json::object());
    CHECK(key.value("type", std::string()) == "string");
    // A fixed list of navigation keys prevents ordinary editing shortcuts such
    // as Ctrl+A and Shift+Home from reaching the native key parser.
    CHECK(!key.contains("enum"));

    const Json text = properties.value("text", Json::object());
    CHECK(text.value("type", std::string()) == "string");
    CHECK(text.value("minLength", 0) == 0);
    const std::string description = tool->description();
    CHECK(description.find("whole-display screenshot ratios") != std::string::npos);
    CHECK(description.find("screenshot does not refresh") != std::string::npos);
    CHECK(description.find("clear only forgets") != std::string::npos);
    CHECK(description.find("does not clear input text") != std::string::npos);

    MaiToolContext context;
    context.sessionId = "contract-without-observation";
    // These deliberately unknown snapshots must be rejected before any native
    // operation. NotFound proves valid empty replacement and shortcut arguments
    // reached the shared controller rather than being rejected by tool parsing.
    const auto emptyReplacement =
        tool->execute(R"({"action":"replace_text","snapshot_id":"missing","window_id":"1","text":""})", context);
    CHECK(emptyReplacement.error().code() == MaiErrorCode::NotFound);
    const auto shortcut =
        tool->execute(R"({"action":"key","snapshot_id":"missing","window_id":"1","key":"Ctrl+A"})", context);
    CHECK(shortcut.error().code() == MaiErrorCode::NotFound);
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc == 2 && std::string(argv[1]) == "--execution-window") {
        verifyExecutionWindowContracts();
        std::fprintf(stderr, "execution-window contract failures: %d\n", failures);
        return failures == 0 ? 0 : 1;
    }
    verifyExecutionWindowContracts();
    verifyDisplayObservation();
    verifyDisplayDispatchAndNextObservation();
    verifyDisplayIdentityAndGeometry();
    verifyDisplayCaptureFailureRetainsDispatchReceipt();
    verifyDisplayCancellationAndSecureInput();
    verifyDisplayDryRunAndClear();
    verifyDesktopLauncherWithoutFocusedTextControl();
    verifyDisplayPostActionRequiresFreshNativeFrame();
    verifyDisplayStaleNativeFrameCannotIssueActionableToken();
    verifyDisplayCaptureCancellationReachesSynchronousBackend();
    verifyDisplayObservationFreshnessThresholdsAreIndependent();
    verifyDisplayOrdinaryWorkspaceCannotChangeAfterObservation();
    verifyDisplayInputInvalidatesOtherSessionObservation();
    verifyDisplayInputDuringInitialCaptureCannotPublishObservation();
    verifyWindowActionInvalidatesDisplayObservation();
    verifyDisplayObservationRejectsAlreadyInFlightInput();
    verifyDisplayToolSchema();
    verifyDisplayToolWaitAndCancel();
    verifyDisplayToolObservationAndActionChain();
    verifyDisplayToolCaptureFailureRetainsReceipt();
    verifyDisplayToolRejectsMixedTargetsAndInvalidIds();
    verifyDisplayToolRejectsTextModelsBeforeNativeAccess();
    verifyDisplayToolWorkspaceChangeRejectsOldToken();
    verifyReadOnlyAndDryRunNeverCreateNativeSession();
    verifyOneShotNativeAdapterPreservesSpyAndRejectsIndependentHolds();
    verifyInputEventsLosslessParsing();
    verifyModelOptionsOmissionInheritsButOrdinaryDefaultsReset();
    verifyResetIsBusyDuringRawKeyDispatch();
    verifyResetInputsRequiresNoObservationAndClearsKeysAndMouse();
    verifyInputEventsControllerAndTool();
    verifyDeferredVirtualInputContract();
    verifyFractionalRepeatConfigurationCodec();
    verifyInputEventsProtocolDiscovery();
    verifyPublicContract();
    verifyKeyGrammar();
    verifyExpandedKeyGrammar();
    verifyKeyIdentityAndCatalog();
    verifyKeyCatalogTool();
    verifySecureKeyboardInput();
    verifyWindowDiscoveryMetadataAndUntitledSelection();
    verifyUnobservableDiscoveryCannotIssueWindowToken();
    verifyMinimizedMetadataObservationAndNoHoldsRelease();
    verifyWindowStateOperations();
    verifySnapshotBoundElementActions();
    verifyCompletedTurnReleasesSnapshotReferences();
    verifyDefaultElementResolverRejectsIncompleteSnapshots();
    verifyElementToolValidationAndFailureFacts();
    verifyElementStateChangesAtDispatchAndMissingDisplaySource();
    verifyCanceledStateDryRunStillReleasesStoppedTurn();
    verifyInvalidatedEvidenceProgressAndObserveDiagnostics();
    verifyMissingSnapshotFieldIsDiagnosedBeforeBackendAccess();
    verifyWindowDiscoveryStateChangeDuringObservation();
    verifyWindowDiscoveryStateRevokesInputAndHeldControls();
    verifyWindowSelection();
    verifyScreenshotHandoff();
    verifyScreenshotDirectoryBoundary();
    verifyScreenshotDirectoryIsValidatedBeforeCapture();
    verifyScreenshotCancellationDoesNotPublishPixels();
    verifyObservationWithoutImage();
    verifyOwnedPopupInput();
    verifyDispatchAndSnapshots();
    verifyTextAndReadback();
    verifyIdentityAndSyntaxFailures();
    verifyFailureAndCancellationBoundaries();
    verifyNativeInputFailureRequiresObservation();
    verifyCanceledPerformReleasesHeldInput();
    verifyRejectedPerformReleasesHeldInput();
    verifyCanceledObservationReleasesOnlyItsSession();
    verifyOldTurnCanceledObservationPreservesReplacementHold();
    verifyOldTurnCleanupPreservesReplacementHold();
    verifyCanceledToolObservationReleasesOnlyItsTurn();
    verifyIndependentUpDryRunKeepsOwnedInput();
    verifyInvalidIndependentButtonCannotValidateWithoutAnOwner();
    verifyIndependentUpConsumesOnlyRealOwnedWindowToken();
    verifyHeldLauncherModifiersDoNotRequireEditableFocus();
    verifyReleaseDoesNotWaitForObservedStateDuringPlatformWork();

    if (failures != 0) {
        std::fprintf(stderr, "desktop computer contract failures: %d\n", failures);
        return 1;
    }
    std::fprintf(stderr, "desktop computer public contracts verified without native input\n");
    return 0;
}
