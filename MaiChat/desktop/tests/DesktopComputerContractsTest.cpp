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
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
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
#include "MaiChatTools/DesktopComputerTools.h"
#include "MaiTool.h"
#include "MaiAgentSendMediaTool.h"

namespace {

using Json = nlohmann::json;

int failures = 0;

#define CHECK(value) do { if (!(value)) { \
    std::fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, #value); \
    ++failures; } } while (false)

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

// This backend supplies actual PNG data and records the controller's native
// boundary calls. It never enumerates, captures or injects into the user's OS.
class RecordingBackend final : public DesktopComputerBackend {
public:
    std::vector<DesktopNativeWindow> visible{
        {101, 5001, "Isolated editor", "Fixture", 100, 200, 400, 300}};
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
    std::function<void()> onActivate;
    std::function<void()> onCapture;
    std::function<void()> onDisplayCapture;
    std::function<void()> onInput;
    std::function<MaiError(const CaptureOptions&)> onDisplayCaptureOptions;

    MaiResult<std::vector<DesktopNativeWindow>> windows() override {
        events.push_back("windows");
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
        return std::vector<DesktopNativeElement>{
            {"text", "Isolated editor input", window.x + 20, window.y + 20, 100, 30}};
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
        if (onInput) onInput();
        if (inputError) return inputError;
        if (value.kind == DesktopNativeInputKind::RightClick) { ownedPopup = true; focused = false; }
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
        const auto observed = controller.observe("owner", "",
            workspace.path().toUtf8().toStdString(), true, action.windowId);
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
    for (const std::string model : {"glm-5.3", "local:qa"}) {
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
    for (const char* key : {"Tab", "Escape"}) {
        Fixture fixture;
        auto action = fixture.observe("key");
        action.key = key;
        fixture.backend->secure = true;
        CHECK(fixture.controller.perform("owner", action));
        CHECK(fixture.backend->inputs.size() == 1);
    }
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
    for (const char* shortcut : {"Control+Shift+End", "Alt+F4", "Cmd+A", "Meta+A",
                                  "Super+A", "Enter", "Tab", "Escape", "Delete",
                                  "Backspace", "ArrowLeft", "ArrowRight", "ArrowUp",
                                  "ArrowDown", "PageUp", "PageDown", "Space", "Insert",
                                  "End", "F1", "F24", "9"}) {
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
          std::string("Cmd+Meta+A"), std::string("Ctrl+A+B"), std::string("Shift+Ctrl"), std::string("F0"),
          std::string("F25"), std::string("Hyper+A"), std::string("\xe4\xb8\xad")}) {
        const auto parsed = maiDesktopParseKey(invalid);
        CHECK(!parsed);
        if (!parsed) CHECK(parsed.error().code() == MaiErrorCode::InvalidInput);
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
    const auto ambiguous = fixture.controller.observe("owner", duplicate.title,
        fixture.workspace.path().toUtf8().toStdString(), true);
    CHECK(!ambiguous);
    if (!ambiguous) CHECK(ambiguous.error().code() == MaiErrorCode::NotFound);
    CHECK(fixture.backend->captureCount == 0);
    const auto selected = fixture.controller.observe("owner", "",
        fixture.workspace.path().toUtf8().toStdString(), true, duplicate.id);
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
        for (const Json includeImage : {Json(true), Json(false), Json(nullptr), Json("false")}) {
            const auto unsupportedImage = tool->execute(
                Json{{"action", "observe"}, {"window_id", "101"}, {"include_image", includeImage}}.dump(), context);
            CHECK(unsupportedImage.hasError());
            CHECK(unsupportedImage.error().code() == MaiErrorCode::InvalidInput);
            CHECK(unsupportedImage.error().message().find("screenshot") != std::string::npos);
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
        if (changed == 0) ++window.id;
        else if (changed == 1) ++window.processId;
        else if (changed == 2) window.x += 20;
        else window.title = "A different window title";
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
    CHECK(!properties.contains("include_image"));
    const Json actions = properties.value("action", Json::object()).value("enum", Json{});
    // The public contract exposes routine desktop operations without needing a
    // native session. Native dispatch belongs to the separate fixture test.
    for (const char* action : {"list", "observe", "focus", "click", "double_click",
                               "right_click", "drag", "type", "key", "scroll",
                               "replace_text", "verify", "clear"}) {
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
    CHECK(description.find("clear only forgets") != std::string::npos);
    CHECK(description.find("does not clear input text") != std::string::npos);

    MaiToolContext context;
    context.sessionId = "contract-without-observation";
    // These deliberately unknown snapshots must be rejected before any native
    // operation. NotFound proves valid empty replacement and shortcut arguments
    // reached the shared controller rather than being rejected by tool parsing.
    const auto emptyReplacement = tool->execute(
        R"({"action":"replace_text","snapshot_id":"missing","window_id":"1","text":""})",
        context);
    CHECK(emptyReplacement.error().code() == MaiErrorCode::NotFound);
    const auto shortcut = tool->execute(
        R"({"action":"key","snapshot_id":"missing","window_id":"1","key":"Ctrl+A"})",
        context);
    CHECK(shortcut.error().code() == MaiErrorCode::NotFound);
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
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
    verifyPublicContract();
    verifyKeyGrammar();
    verifySecureKeyboardInput();
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

    if (failures != 0) {
        std::fprintf(stderr, "desktop computer contract failures: %d\n", failures);
        return 1;
    }
    std::fprintf(stderr, "desktop computer public contracts verified without native input\n");
    return 0;
}
