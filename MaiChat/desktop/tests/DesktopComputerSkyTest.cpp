#include <QBuffer>
#include <QCoreApplication>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <json.hpp>

#include "MaiChatTools/DesktopComputerBackend.h"
#include "MaiChatTools/DesktopComputerTools.h"
#include "MaiTool.h"

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

bool close(double actual, double expected) {
    return std::abs(actual - expected) < 0.01;
}

void printError(const std::string& operation, const MaiError& error) {
    std::fprintf(stderr, "Sky %.100s failed [%s]: %.512s\n", operation.c_str(), maiErrorCodeToString(error.code()),
                 error.message().c_str());
}

// This fixture replaces only the OS seam: the real controller and registered
// model adapter still validate identity, snapshot ownership and coordinates.
class RecordingSkyBackend final : public DesktopComputerBackend {
public:
    std::vector<DesktopNativeWindow> visible{{101, 5001, "Isolated editor", "Fixture", 100, 200, 400, 300}};
    std::vector<DesktopNativeApplication> catalog;
    std::string currentApplicationId = "fixture.editor";
    DesktopNativeAccessibilitySnapshot accessibility;
    std::vector<DesktopNativeInput> inputs;
    std::vector<std::vector<DesktopInputEvent>> batches;
    std::vector<std::string> launched;
    std::vector<std::string> assignedValues;
    std::vector<std::string> secondaryActions;
    int captures = 0;
    int inspections = 0;
    int activations = 0;
    int resolvedElements = 0;
    int applicationIdentityReads = 0;
    int releasedSessions = 0;
    std::function<void()> onResolve;
    std::function<void()> onSetValue;
    std::function<void()> onCapture;
    std::function<void(int)> onApplicationId;
    MaiError valueError;
    MaiError secondaryError;
    MaiError captureError;
    MaiError inputError;
    MaiError catalogError;
    std::function<void()> onActivate;
    std::function<void()> onSuccessfulSetValue;

    RecordingSkyBackend() {
        visible.front().coordinateScale = 2;
        DesktopNativeApplication app;
        app.id = "fixture.editor";
        app.displayName = "Fixture editor";
        app.isRunning = true;
        app.windows = visible;
        catalog.push_back(app);
        DesktopNativeApplication dormant;
        dormant.id = "fixture.viewer";
        catalog.push_back(dormant);  // Unknown usage facts deliberately stay absent.
        DesktopNativeElement edit;
        edit.role = "edit";
        edit.label = "Fixture input";
        edit.x = 140;
        edit.y = 240;
        edit.width = 120;
        edit.height = 40;
        edit.nativeId = "fixture-edit-identity";
        edit.automationId = "fixture-edit";
        edit.secondaryActions = {"Expand", "Collapse"};
        accessibility.elements.push_back(edit);
        accessibility.provider = "recording-sky";
        accessibility.visitedNodes = 1;
        accessibility.countsKnown = true;
    }

    MaiResult<std::vector<DesktopNativeWindow>> windows() override {
        return visible;
    }
    MaiResult<std::vector<DesktopNativeDisplay>> displays() override {
        // Typed window input derives its native environment from real monitor
        // bounds. This owned fixture monitor contains both isolated windows.
        return std::vector<DesktopNativeDisplay>{{11, 0, 0, 1600, 900, true}};
    }
    MaiResult<std::vector<DesktopNativeApplication>> applications(const std::function<bool()>& canceled) override {
        if (canceled && canceled()) return {MaiErrorCode::Canceled, "fixture canceled"};
        if (catalogError) return catalogError;
        return catalog;
    }
    MaiResult<std::string> applicationId(const DesktopNativeWindow& window) override {
        ++applicationIdentityReads;
        const auto callback = onApplicationId;
        if (callback) callback(applicationIdentityReads);
        if (window.processId != 5001) return {MaiErrorCode::NotFound, "fixture process is gone"};
        return currentApplicationId;
    }
    MaiError launchApplication(const std::string& app, const std::function<bool()>& canceled) override {
        if (canceled && canceled()) return {MaiErrorCode::Canceled, "fixture canceled"};
        launched.push_back(app);
        return {};
    }
    MaiResult<std::vector<DesktopNativeElement>> elements(const DesktopNativeWindow&) override {
        return accessibility.elements;
    }
    MaiResult<DesktopNativeAccessibilitySnapshot> inspectElements(const DesktopNativeWindow&,
                                                                  const std::function<bool()>& canceled) override {
        if (canceled && canceled()) return {MaiErrorCode::Canceled, "fixture canceled"};
        ++inspections;
        return accessibility;
    }
    MaiResult<DesktopNativeElement> resolveElement(const DesktopNativeWindow&, const DesktopNativeElement& observed,
                                                   const std::function<bool()>& canceled) override {
        if (canceled && canceled()) return {MaiErrorCode::Canceled, "fixture canceled"};
        ++resolvedElements;
        if (onResolve) onResolve();
        if (canceled && canceled()) return {MaiErrorCode::Canceled, "fixture canceled during resolution"};
        for (const auto& fresh : accessibility.elements)
            if (fresh.nativeId == observed.nativeId) return fresh;
        return {MaiErrorCode::NotFound, "fixture observed element was replaced"};
    }
    MaiError setElementValue(const DesktopNativeWindow&, const DesktopNativeElement& observed, const std::string& value,
                             const std::function<bool()>& canceled) override {
        if (canceled && canceled()) return {MaiErrorCode::Canceled, "fixture canceled"};
        if (onSetValue) onSetValue();
        if (canceled && canceled()) return {MaiErrorCode::Canceled, "fixture canceled during value update"};
        if (valueError) return valueError;
        if (observed.nativeId != accessibility.elements.front().nativeId)
            return {MaiErrorCode::NotFound, "fixture element identity changed"};
        assignedValues.push_back(value);
        if (onSuccessfulSetValue) onSuccessfulSetValue();
        return {};
    }
    MaiError performSecondaryAction(const DesktopNativeWindow&, const DesktopNativeElement& observed,
                                    const std::string& action, const std::function<bool()>& canceled) override {
        if (canceled && canceled()) return {MaiErrorCode::Canceled, "fixture canceled"};
        if (secondaryError) return secondaryError;
        if (observed.nativeId != accessibility.elements.front().nativeId)
            return {MaiErrorCode::NotFound, "fixture element identity changed"};
        secondaryActions.push_back(action);
        return {};
    }
    MaiError capture(const DesktopNativeWindow&, MaiScreenshot& output) override {
        ++captures;
        const auto callback = onCapture;
        if (callback) callback();
        if (captureError) return captureError;
        // The encoded dimensions intentionally differ from native and logical
        // bounds: neither image pixels nor normalized ratios are Sky input.
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
        ++activations;
        const auto callback = onActivate;
        if (callback) callback();
        return {};
    }
    MaiResult<bool> isFocused(const DesktopNativeWindow&) override {
        return true;
    }
    MaiResult<bool> secureInput() override {
        return false;
    }
    MaiError input(const DesktopNativeInput& input) override {
        if (input.isCanceled && input.isCanceled()) return {MaiErrorCode::Canceled, "fixture canceled"};
        if (inputError) return inputError;
        inputs.push_back(input);
        inputs.back().isCanceled = {};  // Never retain caller callbacks.
        return {};
    }
    MaiResult<std::string> focusedText(const DesktopNativeWindow&) override {
        return std::string();
    }
    DesktopInputCapabilities inputCapabilities() const override {
        return {};
    }
    std::shared_ptr<DesktopNativeInputSession> createInputSession() override;
};

// The fixture session records the controller's one-shot native requests and
// owns no controls. In particular it never calls the platform session factory.
class RecordingSkyInputSession final : public DesktopNativeInputSession {
public:
    explicit RecordingSkyInputSession(RecordingSkyBackend& backend) : backend_(backend) {}
    MaiError input(const DesktopNativeInput& input) override {
        const auto error = backend_.input(input);
        if (error) return error;
        std::lock_guard<std::mutex> lock(mutex_);
        if (input.kind == DesktopNativeInputKind::MouseDown) held_.buttonsDown = {"left"};
        if (input.kind == DesktopNativeInputKind::MouseUp) held_.buttonsDown.clear();
        return {};
    }
    DesktopNativeInputState state() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return held_;
    }
    MaiError validate() const override {
        return {};
    }
    MaiError releaseAll() override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++backend_.releasedSessions;
        held_ = {};
        return {};
    }
    MaiError inputEvents(const std::vector<DesktopInputEvent>& events, const DesktopNativeInput& scope,
                         const DesktopInputPort&, const DesktopInputOptions&) override {
        if (scope.isCanceled && scope.isCanceled()) return {MaiErrorCode::Canceled, "fixture canceled"};
        backend_.batches.push_back(events);
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& event : events) {
            if (event.kind != DesktopInputEventKind::MouseButton) continue;
            if (event.release)
                held_.buttonsDown.clear();
            else
                held_.buttonsDown = {"left"};
        }
        return {};
    }
    DesktopInputCapabilities capabilities() const override {
        DesktopInputCapabilities result;
        for (const char* name : {"absolute_move", "mouse_button", "wheel", "raw_key", "unicode_text"})
            result.features.push_back({name, true, true, "recording-sky", "record-only", "fixture"});
        return result;
    }

private:
    RecordingSkyBackend& backend_;
    mutable std::mutex mutex_;
    DesktopNativeInputState held_;
};

std::shared_ptr<DesktopNativeInputSession> RecordingSkyBackend::createInputSession() {
    return std::make_shared<RecordingSkyInputSession>(*this);
}

struct Fixture {
    std::shared_ptr<RecordingSkyBackend> backend = std::make_shared<RecordingSkyBackend>();
    std::shared_ptr<DesktopComputerController> controller = std::make_shared<DesktopComputerController>(backend);
    QTemporaryDir workspace;
    DesktopComputerWindow window{"fixture.editor", 101, std::string("Isolated editor")};

    std::string root() const {
        return workspace.path().toUtf8().toStdString();
    }
    MaiResult<DesktopComputerWindowState> state(bool screenshot = true, bool text = true,
                                                const std::string& session = "owner") {
        return controller->getWindowState(session, window, root(), screenshot, text);
    }
    DesktopComputerSkyAction action(const std::string& method) const {
        DesktopComputerSkyAction value;
        value.method = method;
        value.window = window;
        return value;
    }
    bool holdButton(const std::string& session = "owner", const std::string& turn = "") {
        // Legacy typed holds share the Sky owner's native ledger. Only this
        // compatibility setup extracts its pre-existing observation token.
        const auto observed = controller->observe(session, {}, root(), true, window.id, true, {}, turn);
        CHECK(observed);
        if (!observed) return false;
        const auto metadata = Json::parse(observed.value().json, nullptr, false);
        CHECK(metadata.is_object());
        if (!metadata.is_object()) return false;
        DesktopComputerAction down;
        down.action = "mouse_down";
        down.snapshotId = metadata.value("snapshot_id", std::string());
        down.windowId = window.id;
        down.xRatio = 0.25;
        down.yRatio = 0.25;
        down.turnId = turn;
        const auto held = controller->perform(session, down);
        CHECK(held);
        CHECK(!controller->inputState(session).empty());
        return bool(held) && !controller->inputState(session).empty();
    }
};

void verifyDiscoveryAndGuardedLaunch() {
    Fixture f;
    const auto windows = f.controller->listWindows();
    CHECK(windows && windows.value().size() == 1);
    if (windows && !windows.value().empty()) {
        CHECK(windows.value()[0].id == 101);
        CHECK(windows.value()[0].app == "fixture.editor");
        CHECK(windows.value()[0].title == std::optional<std::string>("Isolated editor"));
    }
    const auto rehydrated = f.controller->getWindow({"", 101, {}});
    CHECK(rehydrated && rehydrated.value().app == "fixture.editor");
    CHECK(!f.controller->getWindow({"fixture.viewer", 101, {}}));
    CHECK(!f.controller->getWindow({"fixture.editor", 999, {}}));
    const auto apps = f.controller->applications();
    CHECK(apps && apps.value().size() == 2);
    if (apps && apps.value().size() == 2) {
        CHECK(!apps.value()[1].isRunning.has_value());
        CHECK(!apps.value()[1].lastUsedDate.has_value());
        CHECK(!apps.value()[1].useCount.has_value());
    }
    CHECK(!f.controller->launchApplication("fixture.viewer"));
    CHECK(f.backend->launched == std::vector<std::string>{"fixture.viewer"});
    CHECK(f.controller->launchApplication("fixture.viewer --unsafe-argument").hasError());
    CHECK(f.controller->launchApplication("C:/unknown.exe").hasError());
    CHECK(f.backend->launched.size() == 1);
    CHECK(!f.controller->activateWindow(f.window));
    CHECK(f.backend->activations == 1);
    CHECK(f.controller->activateWindow({"foreign.app", 101, {}}).hasError());
    CHECK(f.backend->activations == 1);
}

void verifyStateDefaultsAndOptionalFacts() {
    Fixture f;
    CHECK(f.workspace.isValid());
    const auto defaults = f.controller->getWindowState("owner", f.window, f.root());
    CHECK(defaults && defaults.value().screenshots.size() == 1);
    CHECK(f.backend->captures == 1);
    CHECK(f.backend->inspections == 0);
    if (defaults) {
        CHECK(!defaults.value().accessibility.has_value());
        CHECK(defaults.value().indexedElements.empty());
        if (!defaults.value().screenshots.empty()) {
            const auto& screenshot = defaults.value().screenshots.front();
            CHECK(!screenshot.id.empty());
            CHECK(screenshot.url.rfind("data:image/png;base64,", 0) == 0);
            CHECK(QFileInfo(QString::fromUtf8(screenshot.imagePath.c_str())).isFile());
            CHECK(close(screenshot.width, 200));
            CHECK(close(screenshot.height, 150));
        }
    }
    const auto text = f.state(false, true);
    CHECK(text && text.value().screenshots.empty());
    CHECK(f.backend->captures == 1);
    CHECK(f.backend->inspections == 1);
    if (text) {
        CHECK(text.value().indexedElements.size() == 1);
        CHECK(text.value().accessibility.has_value());
        if (text.value().accessibility) {
            CHECK(!text.value().accessibility->focusedElement.has_value());
            CHECK(!text.value().accessibility->selectedText.has_value());
            CHECK(!text.value().accessibility->selectedElements.has_value());
            CHECK(!text.value().accessibility->documentText.has_value());
        }
    }
    const auto neither = f.state(false, false);
    CHECK(!neither && neither.error().code() == MaiErrorCode::InvalidInput);
    CHECK(f.backend->captures == 1 && f.backend->inspections == 1);
    f.backend->accessibility.selectedText = "";
    f.backend->accessibility.selectedElements = std::vector<std::string>{};
    f.backend->accessibility.documentText = "Fixture document";
    const auto known = f.state(false, true);
    CHECK(known && known.value().accessibility.has_value());
    if (known && known.value().accessibility) {
        CHECK(known.value().accessibility->selectedText == std::optional<std::string>(""));
        CHECK(known.value().accessibility->selectedElements.has_value());
        CHECK(known.value().accessibility->documentText == std::optional<std::string>("Fixture document"));
    }
}

void verifyLogicalCoordinatesAndInputMethods() {
    for (const char* method : {"click", "drag", "scroll", "press_key", "type_text"}) {
        Fixture f;
        const auto observed = f.state();
        CHECK(observed);
        if (!observed || observed.value().screenshots.empty()) continue;
        auto action = f.action(method);
        action.x = 25;
        action.y = 30;
        action.toX = 125;
        action.toY = 80;
        action.scrollY = 120;
        action.key = "Ctrl+A";
        action.text = "Fixture typed text";
        action.screenshotId = observed.value().screenshots.front().id;
        const auto result = f.controller->performSky("owner", action);
        if (!result) printError(action.method, result.error());
        CHECK(result && result.value().dispatched);
        if (action.method == "scroll") {
            CHECK(f.backend->batches.size() == 1);
            if (f.backend->batches.size() == 1) {
                const auto& events = f.backend->batches.front();
                const auto wheel = std::find_if(events.begin(), events.end(), [](const DesktopInputEvent& event) {
                    return event.kind == DesktopInputEventKind::Wheel;
                });
                CHECK(wheel != events.end());
                if (wheel != events.end()) CHECK(!wheel->horizontal && wheel->wheelDelta120 == -120);
            }
            continue;  // Wheel transport is raw native delta, not a physical-distance claim.
        }
        CHECK(!f.backend->inputs.empty());
        if (f.backend->inputs.empty()) continue;
        const auto& input = f.backend->inputs.front();
        CHECK(input.expectedWindowId == 101 && input.expectedProcessId == 5001);
        if (action.method == "click" || action.method == "drag") {
            CHECK(close(input.x, 150));
            CHECK(close(input.y, 260));
        }
        if (action.method == "click") CHECK(input.kind == DesktopNativeInputKind::Click);
        if (action.method == "drag") {
            CHECK(input.kind == DesktopNativeInputKind::Drag);
            CHECK(close(input.toX, 350));
            CHECK(close(input.toY, 360));
        }
        if (action.method == "press_key") {
            CHECK(input.kind == DesktopNativeInputKind::Key);
            CHECK(input.key == "Ctrl+A");
        }
        if (action.method == "type_text") {
            CHECK(input.kind == DesktopNativeInputKind::Type);
            CHECK(input.text == action.text);
        }
    }
    Fixture f;
    const auto observed = f.state();
    CHECK(observed);
    if (!observed) return;
    auto click = f.action("click");
    click.elementIndex = 0;
    click.clickCount = 2;
    const auto result = f.controller->performSky("owner", click);
    CHECK(result && result.value().dispatched);
    CHECK(f.backend->resolvedElements > 0);
    if (!f.backend->inputs.empty()) {
        CHECK(f.backend->inputs.front().kind == DesktopNativeInputKind::DoubleClick);
        CHECK(close(f.backend->inputs.front().x, 200));
        CHECK(close(f.backend->inputs.front().y, 260));
    }
}

void verifyElementOperationsAndNativeErrors() {
    for (const char* method : {"set_value", "perform_secondary_action"}) {
        Fixture f;
        CHECK(f.state(false, true));
        auto action = f.action(method);
        action.elementIndex = 0;
        action.text = "Replacement value";
        action.secondaryAction = "Expand";
        const auto result = f.controller->performSky("owner", action);
        CHECK(result && result.value().dispatched);
        CHECK(f.backend->inputs.empty());  // ValuePattern is not focus/select-all typing.
        if (action.method == "set_value")
            CHECK(f.backend->assignedValues == std::vector<std::string>{"Replacement value"});
        else
            CHECK(f.backend->secondaryActions == std::vector<std::string>{"Expand"});
    }
    for (const char* method : {"set_value", "perform_secondary_action"}) {
        Fixture f;
        CHECK(f.state(false, true));
        f.backend->valueError = {MaiErrorCode::NotSupported, "fixture native ValuePattern unavailable"};
        f.backend->secondaryError = {MaiErrorCode::NotSupported, "fixture native action unavailable"};
        auto action = f.action(method);
        action.elementIndex = 0;
        action.text = "Replacement";
        action.secondaryAction = "Expand";
        const auto result = f.controller->performSky("owner", action);
        CHECK(!result && result.error().code() == MaiErrorCode::NotSupported);
        CHECK(f.backend->inputs.empty() && f.backend->assignedValues.empty() && f.backend->secondaryActions.empty());
    }
}

void verifySnapshotAndIdentityGuards() {
    {
        Fixture f;
        const auto state = f.state();
        CHECK(state);
        if (!state || state.value().screenshots.empty()) return;
        auto click = f.action("click");
        click.x = 25;
        click.y = 30;
        click.screenshotId = state.value().screenshots.front().id;
        CHECK(!f.controller->performSky("other-session", click));
        click.window.app = "foreign.app";
        CHECK(!f.controller->performSky("owner", click));
        CHECK(f.backend->inputs.empty());
    }
    {
        Fixture f;
        const auto old = f.state();
        CHECK(old);
        if (!old || old.value().screenshots.empty()) return;
        CHECK(f.state());
        auto click = f.action("click");
        click.x = 25;
        click.y = 30;
        click.screenshotId = old.value().screenshots.front().id;
        CHECK(!f.controller->performSky("owner", click));
        CHECK(f.backend->inputs.empty());
    }
    {
        Fixture f;
        CHECK(f.state(false, true));
        CHECK(f.state(true, false));
        auto action = f.action("set_value");
        action.elementIndex = 0;
        action.text = "Old index";
        CHECK(!f.controller->performSky("owner", action));
        CHECK(f.backend->assignedValues.empty());
    }
    {
        Fixture f;
        CHECK(f.state(false, true));
        f.backend->accessibility.elements.front().nativeId = "replacement-element";
        auto click = f.action("click");
        click.elementIndex = 0;
        CHECK(!f.controller->performSky("owner", click));
        CHECK(f.backend->inputs.empty());
    }
    for (int invalid = 0; invalid < 4; ++invalid) {
        Fixture f;
        CHECK(f.state());
        auto click = f.action("click");
        click.x = invalid == 1 ? -1 : 25;
        click.y = 30;
        if (invalid == 0) click.elementIndex = 0;  // Locator alternatives are exclusive.
        if (invalid == 2) click.x = std::numeric_limits<double>::infinity();
        if (invalid == 3) click.clickCount = 0;
        CHECK(!f.controller->performSky("owner", click));
        CHECK(f.backend->inputs.empty());
    }
}

void verifyCancellationHasNoSideEffects() {
    Fixture f;
    const auto canceled = [] { return true; };
    const auto observed = f.controller->getWindowState("owner", f.window, f.root(), true, true, canceled);
    CHECK(!observed && observed.error().code() == MaiErrorCode::Canceled);
    CHECK(f.backend->captures == 0 && f.backend->inspections == 0);
    CHECK(f.controller->launchApplication("fixture.viewer", canceled).code() == MaiErrorCode::Canceled);
    CHECK(f.controller->activateWindow(f.window, canceled).code() == MaiErrorCode::Canceled);
    CHECK(f.backend->launched.empty() && f.backend->activations == 0);
    CHECK(f.state());
    auto click = f.action("click");
    click.elementIndex = 0;
    click.isCanceled = canceled;
    const auto result = f.controller->performSky("owner", click);
    CHECK(!result && result.error().code() == MaiErrorCode::Canceled);
    CHECK(f.backend->inputs.empty() && f.backend->assignedValues.empty());
}

void verifyDpiAndRepeatedElementClickGuards() {
    {
        Fixture f;
        const auto observed = f.state();
        CHECK(observed);
        if (!observed || observed.value().screenshots.empty()) return;
        auto click = f.action("click");
        click.x = 25;
        click.y = 30;
        click.screenshotId = observed.value().screenshots.front().id;
        // DPI can change without a title/PID/physical rectangle change. Input
        // from the old logical coordinate space must still be rejected.
        f.backend->visible.front().coordinateScale = 1;
        const auto result = f.controller->performSky("owner", click);
        CHECK(!result);
        CHECK(f.backend->inputs.empty() && f.backend->batches.empty());
    }
    {
        Fixture f;
        CHECK(f.state(false, true));
        f.backend->accessibility.elements.front().x += 30;
        auto click = f.action("click");
        click.elementIndex = 0;
        click.clickCount = 3;
        const auto result = f.controller->performSky("owner", click);
        CHECK(!result);
        CHECK(f.backend->resolvedElements > 0);
        CHECK(f.backend->inputs.empty() && f.backend->batches.empty());
    }
}

void verifyChangedApplicationCannotAdoptOldScreenshot() {
    Fixture f;
    const auto observed = f.state();
    CHECK(observed);
    if (!observed || observed.value().screenshots.empty()) return;
    f.backend->currentApplicationId = "fixture.reassigned";
    auto click = f.action("click");
    click.window.app = "fixture.reassigned";
    click.screenshotId = observed.value().screenshots.front().id;
    click.x = 25;
    click.y = 30;
    // An application identity change must not let B adopt an observation of A,
    // even when the OS window handle, PID and physical bounds stay unchanged.
    const auto result = f.controller->performSky("owner", click);
    CHECK(!result);
    CHECK(f.backend->inputs.empty() && f.backend->batches.empty());
}

void verifyActivationCannotDispatchReplacedObservation() {
    Fixture f;
    const auto observed = f.state();
    CHECK(observed);
    if (!observed || observed.value().screenshots.empty()) return;
    auto nativeSecond = f.backend->visible.front();
    nativeSecond.id = 102;
    nativeSecond.x = 700;
    nativeSecond.title = "Replacement activation target";
    f.backend->visible.push_back(nativeSecond);
    const DesktopComputerWindow second{"fixture.editor", 102, nativeSecond.title};
    std::optional<MaiResult<DesktopComputerWindowState>> replacement;
    f.backend->onActivate = [&] {
        f.backend->onActivate = {};
        replacement.emplace(f.controller->getWindowState("owner", second, f.root(), true, false));
    };
    auto click = f.action("click");
    click.x = 25;
    click.y = 30;
    click.screenshotId = observed.value().screenshots.front().id;
    DesktopComputerActionProgress progress;
    const auto result = f.controller->performSky("owner", click, &progress);
    CHECK(replacement.has_value() && bool(*replacement));
    if (replacement && *replacement) CHECK(replacement->value().window.id == 102);
    CHECK(!result);
    CHECK(!progress.inputDispatchAttempted);
    CHECK(f.backend->inputs.empty() && f.backend->batches.empty());
}

void verifySuccessfulNativeValueStillHonorsLateCancel() {
    Fixture f;
    if (!f.holdButton("owner", "late-value-turn")) return;
    CHECK(f.controller->getWindowState("owner", f.window, f.root(), false, true, {}, "late-value-turn"));
    bool canceled = false;
    f.backend->onSuccessfulSetValue = [&] { canceled = true; };
    auto action = f.action("set_value");
    action.elementIndex = 0;
    action.text = "Applied before cancellation";
    action.turnId = "late-value-turn";
    action.isCanceled = [&] { return canceled; };
    DesktopComputerActionProgress progress;
    const auto result = f.controller->performSky("owner", action, &progress);
    // The provider can return success as cancellation races with its last
    // write. Report the attempted effect and release holds without success.
    CHECK(canceled);
    CHECK(f.backend->assignedValues == std::vector<std::string>{"Applied before cancellation"});
    CHECK(!result && result.error().code() == MaiErrorCode::Canceled);
    CHECK(progress.inputDispatchAttempted && progress.windowOperationAttempted);
    CHECK(f.controller->inputState("owner").empty());
    CHECK(f.backend->releasedSessions > 0);
}

void verifyCanceledCatalogAndUnsupportedButtonsHaveNoEffects() {
    {
        Fixture f;
        f.backend->catalogError = {MaiErrorCode::Canceled, "fixture native catalog canceled"};
        const auto error = f.controller->launchApplication("fixture.viewer");
        CHECK(error.code() == MaiErrorCode::Canceled);
        CHECK(f.backend->launched.empty());
    }
    for (const auto button : {DesktopMouseButton::Back, DesktopMouseButton::Forward}) {
        Fixture f;
        CHECK(f.state(false, true));
        auto click = f.action("click");
        click.elementIndex = 0;
        click.clickCount = 3;
        click.button = button;
        const auto result = f.controller->performSky("owner", click);
        CHECK(!result && result.error().code() == MaiErrorCode::InvalidInput);
        CHECK(f.backend->inputs.empty() && f.backend->batches.empty());
    }
    {
        Fixture f;
        CHECK(f.state(false, true));
        auto empty = f.action("type_text");
        const auto result = f.controller->performSky("owner", empty);
        CHECK(result);
        CHECK(f.backend->inputs.empty() && f.backend->batches.empty());
    }
}

void verifyWindowOperationsRespectHeldOwner() {
    for (const bool launch : {false, true}) {
        Fixture f;
        if (!f.holdButton("owner-a")) continue;
        const int activationsBefore = f.backend->activations;
        const std::size_t inputCount = f.backend->inputs.size();
        const auto error =
            launch ? f.controller->launchApplication("fixture.viewer") : f.controller->activateWindow(f.window);
        CHECK(error.code() == MaiErrorCode::Busy);
        CHECK(f.backend->activations == activationsBefore);
        CHECK(f.backend->launched.empty());
        CHECK(f.backend->inputs.size() == inputCount);
        CHECK(!f.controller->inputState("owner-a").empty());
    }
}

void verifyStateDeliveryRejectsReentrantReplacement() {
    Fixture f;
    auto nativeSecond = f.backend->visible.front();
    nativeSecond.id = 102;
    nativeSecond.x = 700;
    nativeSecond.title = "Second isolated editor";
    f.backend->visible.push_back(nativeSecond);
    const DesktopComputerWindow second{"fixture.editor", 102, nativeSecond.title};
    bool captured = false;
    bool interleaved = false;
    std::optional<MaiResult<DesktopComputerWindowState>> replacement;
    f.backend->onCapture = [&] { captured = true; };
    f.backend->onApplicationId = [&](int) {
        if (!captured) return;
        f.backend->onApplicationId = {};
        f.backend->onCapture = {};
        interleaved = true;
        // Reenter at the native identity seam after outer observe publishes A,
        // replacing its session state with B before the outer call delivers A.
        replacement.emplace(f.controller->getWindowState("owner", second, f.root(), true, false));
    };
    const auto original = f.state(true, false);
    CHECK(interleaved);
    CHECK(replacement.has_value() && bool(*replacement));
    if (replacement && *replacement) CHECK(replacement->value().window.id == 102);
    CHECK(!original);
    CHECK(f.backend->inputs.empty());
}

void verifyLateCancellationReleasesHeldOwner() {
    for (const bool cancelDuringValue : {false, true}) {
        Fixture f;
        if (!f.holdButton("owner", "cancel-turn")) continue;
        CHECK(f.controller->getWindowState("owner", f.window, f.root(), false, true, {}, "cancel-turn"));
        bool canceled = false;
        if (cancelDuringValue)
            f.backend->onSetValue = [&] { canceled = true; };
        else
            f.backend->onResolve = [&] { canceled = true; };
        auto action = f.action("set_value");
        action.elementIndex = 0;
        action.text = "Must not replace";
        action.turnId = "cancel-turn";
        action.isCanceled = [&] { return canceled; };
        const auto result = f.controller->performSky("owner", action);
        CHECK(canceled);
        CHECK(!result && result.error().code() == MaiErrorCode::Canceled);
        CHECK(f.backend->assignedValues.empty());
        CHECK(f.controller->inputState("owner").empty());
        CHECK(f.backend->releasedSessions > 0);
    }
}

void verifySkyKeyNamesAndRawScrollDeltas() {
    for (const char* key : {"Control_L+a", "Control_L+Shift_L+period", "KP_0", "greater"}) {
        Fixture f;
        CHECK(f.state(false, true));
        auto action = f.action("press_key");
        action.key = key;
        const auto result = f.controller->performSky("owner", action);
        if (!result) printError(std::string("press_key ") + key, result.error());
        CHECK(result && result.value().dispatched);
        CHECK(f.backend->inputs.size() == 1);
        if (!f.backend->inputs.empty()) CHECK(f.backend->inputs.front().kind == DesktopNativeInputKind::Key);
    }
    Fixture f;
    CHECK(f.state());
    auto scroll = f.action("scroll");
    scroll.x = 25;
    scroll.y = 30;
    scroll.scrollX = -45;
    scroll.scrollY = -90;
    const auto result = f.controller->performSky("owner", scroll);
    if (!result) printError("mixed-axis scroll", result.error());
    CHECK(result && result.value().dispatched);
    CHECK(f.backend->batches.size() == 1);
    if (f.backend->batches.empty()) return;
    std::vector<DesktopInputEvent> wheels;
    for (const auto& event : f.backend->batches.front())
        if (event.kind == DesktopInputEventKind::Wheel) wheels.push_back(event);
    CHECK(wheels.size() == 2);
    if (wheels.size() == 2) {
        CHECK(wheels[0].horizontal && wheels[0].wheelDelta120 == -45);
        CHECK(!wheels[1].horizontal && wheels[1].wheelDelta120 == 90);
    }
}

Json windowJson() {
    return {{"app", "fixture.editor"}, {"id", 101}};
}

struct ModelFixture : Fixture {
    MaiToolRegistry registry;
    MaiTool* tool = nullptr;
    MaiToolContext context;
    ModelFixture() {
        registerDesktopComputerTools(registry, controller);
        tool = registry.find("computer");
        context.sessionId = "owner";
        context.root = root();
        context.turnId = "fixture-turn";
        CHECK(tool);
    }
    MaiToolResult execute(Json args) {
        const auto method = args["action"];
        args.erase("action");
        // Sky's method selector lives outside its exact argument object, so
        // the native secondary action retains its ordinary `action` field.
        if (args.contains("action_name")) {
            args["action"] = args["action_name"];
            args.erase("action_name");
        }
        return tool->execute(Json{{"method", method}, {"parameters", args}}.dump(), context);
    }
};

void verifyCanonicalModelSchema() {
    ModelFixture f;
    if (!f.tool) return;
    const auto schema = Json::parse(f.tool->parametersSchema(), nullptr, false);
    CHECK(schema.is_object());
    if (!schema.is_object()) return;
    const auto properties = schema.value("properties", Json::object());
    const auto actions = properties.value("method", Json::object()).value("enum", Json::array());
    const std::vector<std::string> canonical{"list_windows",     "get_window", "list_apps", "launch_app",
                                             "get_window_state", "click",      "press_key", "type_text",
                                             "scroll",           "set_value",  "drag",      "perform_secondary_action",
                                             "activate_window"};
    CHECK(actions.is_array() && actions.size() == canonical.size());
    for (const auto& method : canonical)
        CHECK(std::find(actions.begin(), actions.end(), Json(method)) != actions.end());
    CHECK(properties.contains("parameters"));
    CHECK(!properties.contains("action"));
    CHECK(!properties.contains("snapshot_id"));
    CHECK(!properties.contains("x_ratio"));
    CHECK(!f.registry.find("desktop_computer"));
}

void verifyModelDiscoveryAndObservation() {
    ModelFixture f;
    if (!f.tool) return;
    const auto listed = f.execute({{"action", "list_windows"}});
    CHECK(!listed.hasError());
    const auto windows = Json::parse(listed.output(), nullptr, false);
    CHECK(windows.is_array() && windows.size() == 1);
    if (windows.is_array() && !windows.empty()) CHECK(windows[0]["app"] == "fixture.editor");
    const auto hydrated = f.execute({{"action", "get_window"}, {"id", 101}});
    CHECK(!hydrated.hasError());
    const auto window = Json::parse(hydrated.output(), nullptr, false);
    CHECK(window.is_object() && window.value("app", std::string()) == "fixture.editor");
    const auto listedApps = f.execute({{"action", "list_apps"}});
    CHECK(!listedApps.hasError());
    const auto apps = Json::parse(listedApps.output(), nullptr, false);
    CHECK(apps.is_array() && apps.size() == 2);
    if (apps.is_array() && apps.size() == 2) {
        CHECK(!apps[1].contains("lastUsedDate"));
        CHECK(!apps[1].contains("useCount"));
        CHECK(!apps[1].contains("isRunning"));
    }
    const auto observed = f.execute({{"action", "get_window_state"}, {"window", windowJson()}});
    CHECK(!observed.hasError() && observed.images().size() == 1);
    const auto state = Json::parse(observed.output(), nullptr, false);
    CHECK(state.is_object());
    if (state.is_object()) {
        CHECK(state.contains("window"));
        CHECK(state.contains("accessibility") && state["accessibility"].is_null());
        CHECK(state.contains("screenshots") && state["screenshots"].size() == 1);
        if (state.contains("screenshots") && state["screenshots"].size() == 1) {
            CHECK(state["screenshots"][0].contains("image_attachment_index"));
            CHECK(state["screenshots"][0]["image_attachment_index"] == 0);
            CHECK(!state["screenshots"][0].contains("url"));
        }
    }
    const auto text = f.execute({{"action", "get_window_state"},
                                 {"window", windowJson()},
                                 {"include_screenshot", false},
                                 {"include_text", true}});
    CHECK(!text.hasError() && text.images().empty());
    CHECK(f.backend->captures == 1);
    const auto textState = Json::parse(text.output(), nullptr, false);
    CHECK(textState.is_object() && textState.contains("accessibility"));
    if (textState.is_object() && textState.contains("accessibility")) {
        const auto& accessibility = textState["accessibility"];
        CHECK(accessibility.is_object() && accessibility.contains("tree"));
        CHECK(!accessibility.contains("selected_text"));
        CHECK(!accessibility.contains("focused_element"));
        CHECK(!accessibility.contains("selected_elements"));
        CHECK(!accessibility.contains("document_text"));
    }
    const auto neither = f.execute({{"action", "get_window_state"},
                                    {"window", windowJson()},
                                    {"include_screenshot", false},
                                    {"include_text", false}});
    CHECK(neither.hasError() && neither.error().code() == MaiErrorCode::InvalidInput);
}

void verifyModelInputBoundary() {
    for (const char* method : {"click", "press_key", "type_text", "scroll", "set_value", "drag",
                               "perform_secondary_action", "activate_window"}) {
        ModelFixture f;
        if (!f.tool) return;
        CHECK(!f.execute({{"action", "get_window_state"},
                          {"window", windowJson()},
                          {"include_screenshot", false},
                          {"include_text", true}})
                   .hasError());
        Json args{{"action", method}, {"window", windowJson()}};
        const std::string action(method);
        if (action == "click" || action == "set_value" || action == "perform_secondary_action")
            args["element_index"] = 0;
        if (action == "set_value") args["value"] = "Model replacement";
        if (action == "perform_secondary_action") args["action_name"] = "Expand";
        if (action == "press_key") args["key"] = "Return";
        if (action == "type_text") args["text"] = "Model typed text";
        if (action == "scroll") {
            args["x"] = 25;
            args["y"] = 30;
            args["scrollX"] = 0;
            args["scrollY"] = 120;
        }
        if (action == "drag") {
            args["from_x"] = 25;
            args["from_y"] = 30;
            args["to_x"] = 80;
            args["to_y"] = 60;
        }
        const auto result = f.execute(args);
        if (result.hasError()) printError(std::string("model ") + action, result.error());
        CHECK(!result.hasError());
        if (action == "set_value")
            CHECK(f.backend->assignedValues.size() == 1);
        else if (action == "perform_secondary_action")
            CHECK(f.backend->secondaryActions.size() == 1);
        else if (action == "activate_window")
            CHECK(f.backend->activations > 0);
        else if (action == "scroll")
            CHECK(!f.backend->batches.empty());
        else
            CHECK(!f.backend->inputs.empty());
    }
    ModelFixture f;
    if (!f.tool) return;
    CHECK(!f.execute({{"action", "launch_app"}, {"app", "fixture.viewer"}}).hasError());
    CHECK(f.backend->launched.size() == 1);
    CHECK(f.execute({{"action", "launch_app"}, {"app", "fixture.viewer --unsafe"}}).hasError());
    CHECK(f.backend->launched.size() == 1);
    for (const char* invalid : {R"({"action":"observe"})", R"({"method":"click","parameters":{"x":2,"y":3}})",
                                R"({"method":"get_window","parameters":{"id":true}})",
                                R"({"method":"list_windows","parameters":{"unexpected":true}})"})
        CHECK(f.tool->execute(invalid, f.context).hasError());
    std::atomic<bool> canceled{true};
    f.context.cancel = &canceled;
    const auto stopped = f.execute({{"action", "launch_app"}, {"app", "fixture.viewer"}});
    CHECK(stopped.hasError() && stopped.error().code() == MaiErrorCode::Canceled);
    CHECK(f.backend->launched.size() == 1);
    f.context.cancel = nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    verifyDiscoveryAndGuardedLaunch();
    verifyStateDefaultsAndOptionalFacts();
    verifyLogicalCoordinatesAndInputMethods();
    verifyElementOperationsAndNativeErrors();
    verifySnapshotAndIdentityGuards();
    verifyCancellationHasNoSideEffects();
    verifyDpiAndRepeatedElementClickGuards();
    verifyChangedApplicationCannotAdoptOldScreenshot();
    verifyActivationCannotDispatchReplacedObservation();
    verifySuccessfulNativeValueStillHonorsLateCancel();
    verifyCanceledCatalogAndUnsupportedButtonsHaveNoEffects();
    verifyWindowOperationsRespectHeldOwner();
    verifyStateDeliveryRejectsReentrantReplacement();
    verifyLateCancellationReleasesHeldOwner();
    verifySkyKeyNamesAndRawScrollDeltas();
    verifyCanonicalModelSchema();
    verifyModelDiscoveryAndObservation();
    verifyModelInputBoundary();
    if (failures) std::fprintf(stderr, "Sky contract checks failed: %d\n", failures);
    return failures ? 1 : 0;
}
