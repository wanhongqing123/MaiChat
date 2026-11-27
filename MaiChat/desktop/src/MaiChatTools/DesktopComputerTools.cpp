#include "MaiChatTools/DesktopComputerTools.h"
#include "MaiChatTools/DesktopComputerBackend.h"
#include "MaiChatTools/DesktopComputerKeys.h"
#include "MaiChatTools/DesktopComputerInputProtocol.h"
#include "MaiChatTools/DesktopComputerInputSession.h"

#include <QDir>
#include <QByteArray>
#include <QSaveFile>
#include <QString>
#include <QUuid>

#include <chrono>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cerrno>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <mutex>
#include <thread>

#include <json.hpp>

#include "MaiChatTools/DesktopComputerPlatform.h"
#include "MaiChatTools/DesktopComputerInputOwnership.h"
#include "MaiCloudModelCatalog.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiPathGuard.h"
#include "MaiScreenshot.h"
#include "MaiTime.h"
#include <QScopeGuard>
#include "MaiTool.h"

namespace {

using Json = nlohmann::json;
const char* nativeScreenUnit();

struct ObservedWindow {
    DesktopNativeWindow window;
    DesktopNativeDisplay display;
    bool desktopScope = false;
    std::string workspace;
    std::uint64_t frameSequence = 0;
    std::uint64_t inputGeneration = 0;
    std::string token;
    std::string turnId;
    std::optional<DesktopNativeWindow> elementWindow;
    std::optional<DesktopNativeWindow> executionWindow;
    std::uint64_t elementWindowId = 0;
    std::map<std::string, DesktopNativeElement> elements;
    // Sky 索引与图片标识附着在唯一观察状态上，不另建一套窗口/授权缓存。
    std::vector<std::string> indexedElementIds;
    std::optional<DesktopNativeAccessibilitySnapshot> accessibility;
    std::string screenshotId;
    std::string imagePath;
    std::string screenshotUrl;
    std::string canonicalApp;
    bool screenshotCoordinatesUsable = false;
    std::atomic<bool> invalidated{false};
    std::chrono::steady_clock::time_point observedAt;
    std::mutex mutex;
};

bool windowIdentityMatches(const DesktopNativeWindow& before, const DesktopNativeWindow& now) {
    return before.id == now.id && before.processId == now.processId && before.title == now.title &&
           before.application == now.application && before.ownerId == now.ownerId && before.className == now.className;
}

bool currentWindowMatches(const DesktopNativeWindow& before, const DesktopNativeWindow& now) {
    // Discovery includes windows that cannot issue or retain an actionable
    // observation. Recheck this state after capture and before using old tokens.
    return !before.minimized && !before.cloaked && before.boundsAvailable && !now.minimized && !now.cloaked &&
           now.boundsAvailable && windowIdentityMatches(before, now) && std::isfinite(before.coordinateScale) &&
           before.coordinateScale > 0 && std::isfinite(now.coordinateScale) && now.coordinateScale > 0 &&
           std::abs(before.coordinateScale - now.coordinateScale) < 0.000001 && std::abs(before.x - now.x) < 3 &&
           std::abs(before.y - now.y) < 3 && std::abs(before.width - now.width) < 3 &&
           std::abs(before.height - now.height) < 3;
}

bool observationWindowMatches(const DesktopNativeWindow& before, const DesktopNativeWindow& now) {
    if (before.minimized != now.minimized || before.cloaked != now.cloaked ||
        before.boundsAvailable != now.boundsAvailable || !windowIdentityMatches(before, now))
        return false;
    return (before.minimized || before.cloaked) ? true : currentWindowMatches(before, now);
}

bool executionWindowMatches(const DesktopNativeWindow& before, const DesktopNativeWindow& now) {
    // A screenshot's receiver is an exact spatial restriction. Even a small
    // move requires fresh evidence; the legacy window tolerance is unchanged.
    return currentWindowMatches(before, now) && before.x == now.x && before.y == now.y && before.width == now.width &&
           before.height == now.height;
}

bool executionIdentityMatches(const DesktopNativeWindow& before, const DesktopNativeWindow& now) {
    // Post-input observation may refresh a document title or geometry, but an
    // automatic handoff must never adopt a recycled handle or another owner.
    return before.id == now.id && before.processId == now.processId && before.ownerId == now.ownerId &&
           before.className == now.className && before.application == now.application;
}

bool validWindowBounds(const DesktopNativeWindow& window) {
    return window.id && window.processId && !window.minimized && !window.cloaked && window.boundsAvailable &&
           std::isfinite(window.x) && std::isfinite(window.y) && std::isfinite(window.width) &&
           std::isfinite(window.height) && window.width > 0 && window.height > 0 &&
           std::isfinite(window.x + window.width) && std::isfinite(window.y + window.height);
}

MaiError executionPointError(const DesktopNativeWindow& window, double x, double y) {
    // Match the native integer pointer location on Windows. macOS dispatch
    // retains screen points; PNG pixels are never substituted for native units.
#if defined(Q_OS_WIN)
    x = std::round(x);
    y = std::round(y);
#endif
    if (x >= window.x && y >= window.y && x < window.x + window.width && y < window.y + window.height) return {};
    Json details{
        {"reason", "point_outside_target"},
        {"execution_window_id", std::to_string(window.id)},
        {"target_bounds", {{"x", window.x}, {"y", window.y}, {"width", window.width}, {"height", window.height}}},
        {"requested_point", {{"x", x}, {"y", y}, {"unit", nativeScreenUnit()}}},
        {"native_screen_bounds_unit", nativeScreenUnit()}};
#if defined(Q_OS_WIN)
    details["requested_px"] = {{"x", x}, {"y", y}};
#endif
    return {MaiErrorCode::InvalidInput, details.dump()};
}

bool ratio(double value) {
    return std::isfinite(value) && value >= 0 && value <= 1;
}

bool validDisplay(const DesktopNativeDisplay& display) {
    return display.id != 0 && std::isfinite(display.x) && std::isfinite(display.y) && std::isfinite(display.width) &&
           std::isfinite(display.height) && display.width > 0 && display.height > 0 &&
           std::isfinite(display.x + display.width) && std::isfinite(display.y + display.height);
}

bool displayMatches(const DesktopNativeDisplay& first, const DesktopNativeDisplay& second) {
    return first.id == second.id && first.x == second.x && first.y == second.y && first.width == second.width &&
           first.height == second.height;
}

Json displayJson(const DesktopNativeDisplay& display) {
    return {{"display_id", std::to_string(display.id)},
            {"primary", display.primary},
            {"bounds", {{"x", display.x}, {"y", display.y}, {"width", display.width}, {"height", display.height}}}};
}

const char* nativeScreenUnit() {
#if defined(Q_OS_WIN)
    return "screen_physical_pixels";
#else
    return "screen_points";
#endif
}

MaiToolResult failure(const MaiError& error) {
    return MaiToolResult::failure(error.code(), error.message());
}

std::mutex desktopOperationMutex;
// All controllers share one physical desktop. An even generation identifies a
// completed input batch; odd means input/activation is still in flight. This
// also invalidates display observations held by other Sessions/controllers.
std::atomic<std::uint64_t> desktopInputGeneration{0};

class DesktopInputBatch final {
public:
    DesktopInputBatch() {
        desktopInputGeneration.fetch_add(1);
    }
    ~DesktopInputBatch() {
        finish();
    }
    void finish() {
        if (active_) {
            desktopInputGeneration.fetch_add(1);
            active_ = false;
        }
    }

private:
    bool active_ = true;
};

class NativeComputerBackend final : public DesktopComputerBackend {
public:
    MaiResult<std::vector<DesktopNativeApplication>> applications(const std::function<bool()>& canceled) override {
        return maiDesktopApplications(canceled);
    }
    MaiResult<std::string> applicationId(const DesktopNativeWindow& window) override {
        return maiDesktopApplicationId(window);
    }
    MaiError launchApplication(const std::string& app, const std::function<bool()>& canceled) override {
        return maiDesktopLaunchApplication(app, canceled);
    }
    MaiError setElementValue(const DesktopNativeWindow& window, const DesktopNativeElement& element,
                             const std::string& value, const std::function<bool()>& canceled) override {
        return maiDesktopSetElementValue(window, element, value, canceled);
    }
    MaiError performSecondaryAction(const DesktopNativeWindow& window, const DesktopNativeElement& element,
                                    const std::string& action, const std::function<bool()>& canceled) override {
        return maiDesktopPerformSecondaryAction(window, element, action, canceled);
    }
    MaiResult<std::vector<DesktopNativeDisplay>> displays() override {
        return maiDesktopDisplays();
    }
    MaiError captureDisplay(const DesktopNativeDisplay& display, MaiScreenshot& output) override {
        return captureDisplay(display, output, {});
    }
    MaiError captureDisplay(const DesktopNativeDisplay& display, MaiScreenshot& output,
                            const CaptureOptions& options) override {
        MaiScreenshotRequest request;
        request.target = MaiScreenshotTarget::DisplayById;
        request.displayId = display.id;
        request.afterFrameSequence = options.afterFrameSequence;
        request.afterFrameReceivedAtMs = options.afterFrameReceivedAtMs;
        request.isCanceled = options.isCanceled;
        return maiCaptureScreenshot(request, output);
    }
    MaiResult<std::vector<DesktopNativeWindow>> windows() override {
        return maiDesktopWindows();
    }
    MaiResult<std::vector<DesktopNativeElement>> elements(const DesktopNativeWindow& window) override {
        return maiDesktopElements(window);
    }
    MaiResult<DesktopNativeAccessibilitySnapshot> inspectElements(const DesktopNativeWindow& window,
                                                                  const std::function<bool()>& isCanceled) override {
        return maiDesktopInspectElements(window, isCanceled);
    }
    MaiResult<DesktopNativeElement> resolveElement(const DesktopNativeWindow& window,
                                                   const DesktopNativeElement& observedElement,
                                                   const std::function<bool()>& isCanceled) override {
        return maiDesktopResolveElement(window, observedElement, isCanceled);
    }
    MaiError setWindowState(const DesktopNativeWindow& window, DesktopWindowStateAction action,
                            const std::function<bool()>& isCanceled) override {
        return maiDesktopSetWindowState(window, action, isCanceled);
    }
    MaiError capture(const DesktopNativeWindow& window, MaiScreenshot& output) override {
        MaiScreenshotRequest request;
        request.target = MaiScreenshotTarget::WindowById;
        request.windowId = window.id;
        return maiCaptureScreenshot(request, output);
    }
    MaiError activate(const DesktopNativeWindow& window) override {
        return maiDesktopActivateWindow(window);
    }
    MaiError activate(const DesktopNativeWindow& window, const std::function<bool()>& canceled) override {
        return maiDesktopActivateWindow(window, canceled);
    }
    MaiResult<bool> isFocused(const DesktopNativeWindow& window) override {
        return maiDesktopWindowIsFocused(window);
    }
    MaiError activateForInput(const DesktopNativeWindow& window) override {
        const auto ready = maiDesktopWindowAcceptsInput(window);
        if (!ready) return ready.error();
        return ready.value() ? MaiError{} : activate(window);
    }
    MaiError activateForInput(const DesktopNativeWindow& window, const std::function<bool()>& canceled) override {
        if (canceled && canceled()) return {MaiErrorCode::Canceled, "window activation canceled"};
        const auto ready = maiDesktopWindowAcceptsInput(window);
        if (!ready) return ready.error();
        return ready.value() ? MaiError{} : activate(window, canceled);
    }
    MaiResult<bool> inputFocused(const DesktopNativeWindow& window) override {
        return maiDesktopWindowAcceptsInput(window);
    }
    MaiResult<bool> secureInput() override {
        return maiDesktopFocusedInputIsSecure();
    }
    MaiError input(const DesktopNativeInput& input) override {
        return maiDesktopInput(input);
    }
    std::shared_ptr<DesktopNativeInputSession> createInputSession() override {
        return makeDesktopComputerInputSession(maiDesktopCreateInputSession());
    }
    DesktopInputCapabilities inputCapabilities() const override {
        return maiDesktopInputCapabilities();
    }
    MaiResult<std::string> focusedText(const DesktopNativeWindow& window) override {
        return maiDesktopFocusedText(window);
    }
};

bool validText(const std::string& text, bool allowEmpty) {
    if ((!allowEmpty && text.empty()) || text.size() > 4000 || text.find('\0') != std::string::npos) return false;
    const QString decoded = QString::fromUtf8(text.data(), int(text.size()));
    return decoded.toUtf8().toStdString() == text;
}

MaiError inputDispatchFailure(const MaiError& error) {
    // A native error can follow a successful down/character and a failed up,
    // focus check or cancellation. Preserve the code/detail, but do not tell
    // callers that nothing happened or encourage repeating a state-changing key.
    return {error.code(),
            "Input may have been partially delivered. Observe the current application state "
            "before deciding whether to retry. Native error: " +
                error.message()};
}

bool desktopLauncherKey(const DesktopKeyChord& chord) {
    if (chord.modifiers.size() != 1 || maiDesktopModifierFamily(chord.modifiers.front()) != DesktopKeyModifier::Meta)
        return false;
    // Shell launchers are valid even when there is no focused AX/UIA control.
    // Keep the exception exact: arbitrary Meta chords may still edit or paste.
#if defined(Q_OS_MAC)
    return chord.key == DesktopKeyCode::Space;
#elif defined(Q_OS_WIN)
    return chord.key == DesktopKeyCode::Character &&
           (chord.character == 'R' || chord.character == 'E' || chord.character == 'D' || chord.character == 'S');
#else
    return false;
#endif
}

MaiResult<std::uint64_t> windowIdFrom(const Json& value, const std::string& field = "window_id") {
    if (!value.is_string()) return {MaiErrorCode::InvalidInput, field + " must be a decimal string"};
    const std::string id = value.get<std::string>();
    if (id.empty() || id.size() > 20 || !std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return {MaiErrorCode::InvalidInput, field + " must be a nonzero decimal ID string"};
    errno = 0;
    char* end = nullptr;
    const auto parsed = std::strtoull(id.c_str(), &end, 10);
    if (errno == ERANGE || !end || *end || !parsed)
        return {MaiErrorCode::InvalidInput, field + " must be a nonzero decimal ID string within uint64 range"};
    return std::uint64_t(parsed);
}

Json windowJson(const DesktopNativeWindow& window) {
    return {{"window_id", std::to_string(window.id)},
            {"process_id", window.processId},
            {"title", window.title},
            {"application", window.application},
            {"minimized", window.minimized},
            {"cloaked", window.cloaked},
            {"bounds_available", window.boundsAvailable},
            {"tool_window", window.toolWindow},
            {"owner_window_id", std::to_string(window.ownerId)},
            {"class_name", window.className},
            {"native_screen_bounds_unit", nativeScreenUnit()},
            {"bounds", {{"x", window.x}, {"y", window.y}, {"width", window.width}, {"height", window.height}}}};
}

bool validElementBounds(const DesktopNativeElement& element) {
    return std::isfinite(element.x) && std::isfinite(element.y) && std::isfinite(element.width) &&
           std::isfinite(element.height) && element.width > 0 && element.height > 0 &&
           std::isfinite(element.x + element.width) && std::isfinite(element.y + element.height);
}

bool elementMatches(const DesktopNativeElement& before, const DesktopNativeElement& now) {
    return before.nativeId == now.nativeId && before.role == now.role && before.label == now.label &&
           before.automationId == now.automationId && before.enabled == now.enabled &&
           before.offscreen == now.offscreen && now.enabled && !now.offscreen && validElementBounds(now) &&
           before.x == now.x && before.y == now.y && before.width == now.width && before.height == now.height;
}

void unavailableAccessibility(Json& output, const std::string& reason, const std::string& detail = {}) {
    output["elements"] = Json::array();
    output["accessibility"] = {{"provider", "backend"},  {"status", "unavailable"}, {"visited_nodes", 0},
                               {"returned_elements", 0}, {"counts_known", false},   {"truncated", false},
                               {"limit_reason", reason}};
    if (!detail.empty()) output["element_error"] = detail;
}

void publishElements(Json& output, ObservedWindow& state, const DesktopNativeAccessibilitySnapshot& snapshot, double x,
                     double y, double width, double height) {
    Json items = Json::array();
    state.accessibility = snapshot;
    for (const auto& item : snapshot.elements) {
        if (!validElementBounds(item)) continue;
        const double centerX = (item.x + item.width / 2 - x) / width;
        const double centerY = (item.y + item.height / 2 - y) / height;
        if (!ratio(centerX) || !ratio(centerY)) continue;
        const QString label = QString::fromUtf8(item.label.c_str(), int(item.label.size())).left(160);
        Json element{{"role", item.role},
                     {"label", label.toUtf8().toStdString()},
                     {"enabled", item.enabled},
                     {"offscreen", item.offscreen},
                     {"center_x_ratio", centerX},
                     {"center_y_ratio", centerY},
                     {"bounds",
                      {{"x_ratio", (item.x - x) / width},
                       {"y_ratio", (item.y - y) / height},
                       {"width_ratio", item.width / width},
                       {"height_ratio", item.height / height}}}};
        if (!item.automationId.empty()) element["automation_id"] = item.automationId;
        if (!item.nativeId.empty() || item.reference) {
            const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
            state.elements.emplace(id, item);
            state.indexedElementIds.push_back(id);
            element["element_id"] = id;
        }
        items.push_back(std::move(element));
    }
    output["elements"] = std::move(items);
    output["accessibility"] = {{"provider", snapshot.provider},
                               {"status", snapshot.status},
                               {"visited_nodes", snapshot.visitedNodes},
                               {"returned_elements", output["elements"].size()},
                               {"provider_elements", snapshot.elements.size()},
                               {"counts_known", snapshot.countsKnown},
                               {"truncated", snapshot.truncated},
                               {"limit_reason", snapshot.limitReason}};
}

Json inputStateJson(const DesktopNativeInputState& state) {
    Json rawKeys = Json::array();
    for (const auto& key : state.rawKeys)
        rawKeys.push_back({{"code", key.code}, {"flags", key.flags}, {"modifiers", key.modifiers}});
    Json controllers = Json::array();
    for (const auto& controller : state.controllers)
        controllers.push_back({{"controller_id", controller.controllerId},
                               {"generation", std::to_string(controller.generation)},
                               {"profile", maiDesktopInputProfileName(controller.profile)}});
    return {{"buttons_down", state.buttonsDown}, {"keys_down", state.keysDown},
            {"raw_keys_down", rawKeys},          {"touch_pointers", state.touchPointers},
            {"pen_active", state.penActive},     {"devices", state.devices},
            {"controllers", controllers},        {"scope", "owned_synthetic_inputs"},
            {"idle_release_ms", 120000},         {"idle_remaining_ms", state.idleRemainingMs}};
}

MaiResult<DesktopMouseButton> mouseButtonFrom(const Json& value) {
    if (!value.is_string()) return {MaiErrorCode::InvalidInput, "button must be a mouse button name"};
    const std::string name = value.get<std::string>();
    if (name == "left") return DesktopMouseButton::Left;
    if (name == "right") return DesktopMouseButton::Right;
    if (name == "middle") return DesktopMouseButton::Middle;
    if (name == "back") return DesktopMouseButton::Back;
    if (name == "forward") return DesktopMouseButton::Forward;
    return {MaiErrorCode::InvalidInput, "button must be left, right, middle, back or forward"};
}

MaiResult<DesktopScrollAxis> scrollAxisFrom(const Json& value) {
    if (!value.is_string()) return {MaiErrorCode::InvalidInput, "axis must be vertical or horizontal"};
    if (value == "vertical") return DesktopScrollAxis::Vertical;
    if (value == "horizontal") return DesktopScrollAxis::Horizontal;
    return {MaiErrorCode::InvalidInput, "axis must be vertical or horizontal"};
}

bool ownsRequestedUp(const DesktopNativeInputState& state, const DesktopComputerAction& request) {
    if (request.action == "mouse_up") {
        return std::any_of(state.buttonsDown.begin(), state.buttonsDown.end(), [&](const auto& name) {
            const auto button = mouseButtonFrom(Json(name));
            return button && button.value() == request.button;
        });
    }
    const auto requested = maiDesktopParseKey(request.key);
    if (!requested) return false;
    return std::any_of(state.keysDown.begin(), state.keysDown.end(), [&](const auto& name) {
        const auto held = maiDesktopParseKey(name);
        return held && held.value().modifiers.empty() && held.value().key == requested.value().key &&
               held.value().character == requested.value().character &&
               held.value().function == requested.value().function;
    });
}

DesktopInputPort inputPort(double x, double y, double width, double height,
                           const std::vector<DesktopNativeDisplay>& displays) {
    DesktopInputPort port;
    port.x = x;
    port.y = y;
    port.width = width;
    port.height = height;
    port.environmentX = x;
    port.environmentY = y;
    double right = x + width;
    double bottom = y + height;
    bool hasEnvironment = false;
    // Native display bounds, including negative origins, define the input
    // environment. Model-supplied pixels or cropped image geometry never do.
    for (const auto& display : displays) {
        if (!validDisplay(display)) continue;
        if (!hasEnvironment) {
            port.environmentX = display.x;
            port.environmentY = display.y;
            right = display.x + display.width;
            bottom = display.y + display.height;
            hasEnvironment = true;
            continue;
        }
        port.environmentX = (std::min)(port.environmentX, display.x);
        port.environmentY = (std::min)(port.environmentY, display.y);
        right = (std::max)(right, display.x + display.width);
        bottom = (std::max)(bottom, display.y + display.height);
    }
    port.environmentWidth = right - port.environmentX;
    port.environmentHeight = bottom - port.environmentY;
    return port;
}

}  // namespace

MaiError maiDesktopValidateExecutionPoint(const DesktopNativeWindow& window, double x, double y) {
    if (!validWindowBounds(window) || !std::isfinite(x) || !std::isfinite(y))
        return {MaiErrorCode::InvalidInput, "execution window and pointer need finite visible native bounds"};
    return executionPointError(window, x, y);
}

MaiError DesktopComputerBackend::activateForInput(const DesktopNativeWindow& window) {
    return activate(window);
}

MaiError DesktopComputerBackend::activate(const DesktopNativeWindow& window, const std::function<bool()>& canceled) {
    if (canceled && canceled()) return {MaiErrorCode::Canceled, "window activation canceled"};
    const auto result = activate(window);
    if (!result && canceled && canceled()) return {MaiErrorCode::Canceled, "window activation canceled"};
    return result;
}

MaiError DesktopComputerBackend::activateForInput(const DesktopNativeWindow& window,
                                                  const std::function<bool()>& canceled) {
    if (canceled && canceled()) return {MaiErrorCode::Canceled, "window activation canceled"};
    const auto result = activateForInput(window);
    if (!result && canceled && canceled()) return {MaiErrorCode::Canceled, "window activation canceled"};
    return result;
}

MaiResult<bool> DesktopComputerBackend::inputFocused(const DesktopNativeWindow& window) {
    return isFocused(window);
}

std::shared_ptr<DesktopComputerBackend> makeDesktopComputerBackend() {
    return std::make_shared<NativeComputerBackend>();
}

struct DesktopComputerController::Impl {
    std::shared_ptr<DesktopComputerBackend> backend;
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<ObservedWindow>> sessions;
    std::unique_ptr<DesktopComputerInputOwnership> inputOwnership;

    std::shared_ptr<ObservedWindow> get(const std::string& sessionId) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = sessions.find(sessionId);
        return found == sessions.end() ? nullptr : found->second;
    }
    void invalidate(const std::string& session, const std::string& turn, bool allowLegacy = false,
                    const std::shared_ptr<ObservedWindow>& expected = {}) {
        std::shared_ptr<ObservedWindow> revoked;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto found = sessions.find(session);
            if (found == sessions.end()) return;
            const auto& selected = found->second;
            if (expected && selected != expected) return;
            if (!turn.empty() && selected->turnId != turn && !(allowLegacy && selected->turnId.empty())) return;
            // Never wait for a worker's state mutex: platform work may await
            // this GUI thread. Its local shared_ptr keeps state alive while
            // atomic revocation cancels it; a completed turn leaves no cache.
            selected->invalidated.store(true);
            revoked = std::move(found->second);
            sessions.erase(found);
        }
        // Native AX/provider reference destruction may perform platform work.
        // Drop the cache's ownership only after releasing the sessions mutex.
    }
};

DesktopComputerController::DesktopComputerController() : DesktopComputerController(nullptr) {}
DesktopComputerController::DesktopComputerController(std::shared_ptr<DesktopComputerBackend> backend)
    : impl_(std::make_unique<Impl>()) {
    impl_->backend = backend ? std::move(backend) : makeDesktopComputerBackend();
    impl_->inputOwnership =
        std::make_unique<DesktopComputerInputOwnership>(impl_->backend, [] { desktopInputGeneration.fetch_add(2); });
}
DesktopComputerController::~DesktopComputerController() = default;

MaiResult<std::vector<DesktopNativeApplication>> DesktopComputerBackend::applications(const std::function<bool()>&) {
    return {MaiErrorCode::NotSupported, "application discovery is unavailable"};
}
MaiResult<std::string> DesktopComputerBackend::applicationId(const DesktopNativeWindow& window) {
    if (window.application.empty()) return {MaiErrorCode::NotFound, "window application is unknown"};
    return window.application;
}
MaiError DesktopComputerBackend::launchApplication(const std::string&, const std::function<bool()>&) {
    return {MaiErrorCode::NotSupported, "application launch is unavailable"};
}
MaiError DesktopComputerBackend::setElementValue(const DesktopNativeWindow&, const DesktopNativeElement&,
                                                 const std::string&, const std::function<bool()>&) {
    return {MaiErrorCode::NotSupported, "element value pattern is unavailable"};
}
MaiError DesktopComputerBackend::performSecondaryAction(const DesktopNativeWindow&, const DesktopNativeElement&,
                                                        const std::string&, const std::function<bool()>&) {
    return {MaiErrorCode::NotSupported, "element secondary action is unavailable"};
}

std::shared_ptr<DesktopNativeInputSession> DesktopComputerBackend::createInputSession() {
    return {};
}

DesktopNativeInputState DesktopComputerController::inputState(const std::string& sessionId) const {
    return impl_->inputOwnership->state(sessionId);
}

DesktopInputCapabilities DesktopComputerController::capabilities() const {
    return impl_->backend->inputCapabilities();
}

MaiResult<DesktopInputFeedbackBatch> DesktopComputerController::feedback(const std::string& sessionId,
                                                                         std::uint64_t afterSequence,
                                                                         std::uint32_t limit) const {
    if (limit == 0 || limit > 256) return {MaiErrorCode::InvalidInput, "feedback limit must be 1..256"};
    return impl_->inputOwnership->feedback(sessionId, afterSequence, limit);
}

MaiError DesktopComputerController::releaseInput(const std::string& sessionId, const std::string& expectedTurn) {
    // Revocation is independent of a native input session or held controls and
    // precedes native cleanup so an in-flight operation sees Stop immediately.
    // A late completion from an older turn cannot revoke its replacement.
    impl_->invalidate(sessionId, expectedTurn);
    bool released = false;
    const auto result = impl_->inputOwnership->release(sessionId, expectedTurn, &released);
    if (released) impl_->invalidate(sessionId, expectedTurn, true);
    return result;
}

MaiError DesktopComputerController::resetInput(const std::string& sessionId, const std::string& expectedTurn) {
    std::unique_lock<std::mutex> desktopLock(desktopOperationMutex, std::try_to_lock);
    if (!desktopLock.owns_lock()) return {MaiErrorCode::Busy, "another desktop operation is in flight"};
    bool reset = false;
    const auto result = impl_->inputOwnership->reset(sessionId, expectedTurn, &reset);
    if (reset) impl_->invalidate(sessionId, expectedTurn);
    return result;
}

MaiResult<std::vector<DesktopNativeDisplay>> DesktopComputerBackend::displays() {
    return {MaiErrorCode::NotSupported, "display observation is unavailable in this backend"};
}

MaiResult<DesktopNativeAccessibilitySnapshot> DesktopComputerBackend::inspectElements(
    const DesktopNativeWindow& window, const std::function<bool()>& isCanceled) {
    if (isCanceled && isCanceled()) return {MaiErrorCode::Canceled, "element inspection canceled"};
    const auto found = elements(window);
    if (!found) return found.error();
    if (isCanceled && isCanceled()) return {MaiErrorCode::Canceled, "element inspection canceled"};
    DesktopNativeAccessibilitySnapshot result;
    result.elements = found.value();
    return result;  // Legacy providers do not know traversal coverage counts.
}

MaiResult<DesktopNativeElement> DesktopComputerBackend::resolveElement(const DesktopNativeWindow& window,
                                                                       const DesktopNativeElement& observedElement,
                                                                       const std::function<bool()>& isCanceled) {
    if (observedElement.nativeId.empty())
        return {MaiErrorCode::NotSupported, "element has no re-resolvable native identity; observe again"};
    const auto fresh = inspectElements(window, isCanceled);
    if (!fresh) return fresh.error();
    // A single match in a partial traversal does not prove uniqueness. Native
    // providers and the legacy fallback must agree before deriving a click.
    if (fresh.value().status != "available" || fresh.value().truncated)
        return {MaiErrorCode::Busy, "element inspection is incomplete; observe again before selecting a control"};
    DesktopNativeElement selected;
    std::size_t count = 0;
    for (const auto& item : fresh.value().elements) {
        if (item.nativeId != observedElement.nativeId) continue;
        selected = item;
        ++count;
    }
    if (count != 1) return {MaiErrorCode::InvalidInput, "observed element is missing or ambiguous; observe again"};
    return selected;
}

MaiError DesktopComputerBackend::setWindowState(const DesktopNativeWindow&, DesktopWindowStateAction,
                                                const std::function<bool()>& isCanceled) {
    if (isCanceled && isCanceled()) return {MaiErrorCode::Canceled, "window state operation canceled"};
    return {MaiErrorCode::NotSupported, "window state operations are unavailable in this backend"};
}

MaiError DesktopComputerBackend::captureDisplay(const DesktopNativeDisplay&, MaiScreenshot& output) {
    output = {};
    return {MaiErrorCode::NotSupported, "display capture is unavailable in this backend"};
}

MaiError DesktopComputerBackend::captureDisplay(const DesktopNativeDisplay& display, MaiScreenshot& output,
                                                const CaptureOptions& options) {
    if (options.isCanceled && options.isCanceled()) return {MaiErrorCode::Canceled, "display capture canceled"};
    const MaiError captured = captureDisplay(display, output);
    if (options.isCanceled && options.isCanceled()) {
        output = {};
        return {MaiErrorCode::Canceled, "display capture canceled"};
    }
    return captured;
}

MaiResult<std::vector<DesktopNativeDisplay>> DesktopComputerController::displays() {
    return impl_->backend->displays();
}

MaiResult<DesktopComputerObservation> DesktopComputerController::observeDisplay(
    const std::string& sessionId, const std::string& workspace, std::uint64_t displayId,
    std::function<bool()> isCanceled, std::uint64_t afterFrameSequence, std::int64_t afterFrameReceivedAtMs, int waitMs,
    const std::string& turnId, std::uint64_t elementWindowId, std::uint64_t executionWindowId) {
    impl_->inputOwnership->touch(sessionId, turnId);
    const auto canceled = [&] { return isCanceled && isCanceled(); };
    const auto cleanupOnCancel = qScopeGuard([&] {
        if (canceled()) releaseInput(sessionId, turnId);
    });
    if (canceled()) return {MaiErrorCode::Canceled, "desktop observation canceled"};
    if (sessionId.empty() || workspace.empty() || displayId == 0)
        return {MaiErrorCode::InvalidInput, "Session, workspace and display ID are required"};
    if (waitMs < 0 || waitMs > 5000) return {MaiErrorCode::InvalidInput, "display observation wait must be 0..5000ms"};
    // A fresh compositor frame may precede the application's response to input.
    // Let callers wait for paint/load before observing, without repeating input
    // or holding the global desktop mutex. Cancellation is polled throughout.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (canceled()) return {MaiErrorCode::Canceled, "desktop observation canceled"};
        std::this_thread::sleep_until(
            (std::min)(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(20)));
    }
    if (canceled()) return {MaiErrorCode::Canceled, "desktop observation canceled"};
    const MaiFilePath resolvedWorkspace = MaiFileSystem::resolve(MaiFilePath::fromUtf8(workspace));
    if (resolvedWorkspace.isEmpty() || !MaiFileSystem::isDirectory(resolvedWorkspace))
        return {MaiErrorCode::InvalidInput, "display observation needs an accessible workspace"};
    const auto listed = impl_->backend->displays();
    if (!listed) return listed.error();
    DesktopNativeDisplay selected;
    int count = 0;
    for (const auto& display : listed.value()) {
        if (display.id == displayId) {
            selected = display;
            ++count;
        }
    }
    if (count == 0) return {MaiErrorCode::NotFound, "selected display is not active"};
    if (count != 1 || !validDisplay(selected))
        return {MaiErrorCode::InvalidInput, "selected display is ambiguous or invalid"};

    std::optional<DesktopNativeWindow> executionWindow;
    if (executionWindowId) {
        const auto windows = impl_->backend->windows();
        if (!windows) return windows.error();
        for (const auto& window : windows.value()) {
            if (window.id != executionWindowId) continue;
            if (executionWindow) return {MaiErrorCode::InvalidInput, "execution window is ambiguous; observe again"};
            executionWindow = window;
        }
        if (!executionWindow) return {MaiErrorCode::NotFound, "execution window is not available"};
        const auto& window = *executionWindow;
        if (!validWindowBounds(window) || window.x >= selected.x + selected.width ||
            window.y >= selected.y + selected.height || window.x + window.width <= selected.x ||
            window.y + window.height <= selected.y)
            return {MaiErrorCode::InvalidInput, "execution window needs visible bounds on the selected display"};
    }

    const std::string directory = maiResolvePathWithinRoot(workspace, "computer");
    if (directory.empty()) return {MaiErrorCode::InvalidInput, "display image directory leaves the workspace"};
    const std::string relative = "computer/" + MaiIdGenerator::generate("display_") + ".png";
    const std::string destination = maiResolvePathWithinRoot(workspace, relative);
    if (destination.empty()) return {MaiErrorCode::InvalidInput, "display image path leaves the workspace"};
    const auto insideWorkspace = [&] {
        return maiResolvePathWithinRoot(workspace, "computer") == directory &&
               maiResolvePathWithinRoot(workspace, destination) == destination;
    };
    const MaiError created = MaiFileSystem::createDirectories(MaiFilePath::fromUtf8(directory));
    if (created) return created;
    if (!insideWorkspace()) return {MaiErrorCode::InvalidInput, "display image directory changed"};
    if (canceled()) return {MaiErrorCode::Canceled, "desktop observation canceled"};
    const auto inputGeneration = desktopInputGeneration.load();
    if (inputGeneration & 1)
        return {MaiErrorCode::Busy, "desktop input is in progress; observe again after it finishes"};
    MaiScreenshot screenshot;
    DesktopComputerBackend::CaptureOptions options;
    options.afterFrameSequence = afterFrameSequence;
    options.afterFrameReceivedAtMs = afterFrameReceivedAtMs;
    options.isCanceled = isCanceled;
    const MaiError captured = impl_->backend->captureDisplay(selected, screenshot, options);
    if (captured) return captured;
    if (canceled()) return {MaiErrorCode::Canceled, "desktop observation canceled"};
    if (desktopInputGeneration.load() != inputGeneration)
        return {MaiErrorCode::Busy, "desktop input changed while capturing; observe again"};
    if ((afterFrameSequence && screenshot.frameSequence <= afterFrameSequence) ||
        (afterFrameReceivedAtMs && screenshot.frameReceivedAtMs <= afterFrameReceivedAtMs))
        return {MaiErrorCode::Busy, "no newer display frame is available after the input"};
    const auto afterCapture = impl_->backend->displays();
    if (!afterCapture) return afterCapture.error();
    if (std::count_if(afterCapture.value().begin(), afterCapture.value().end(),
                      [&](const auto& display) { return displayMatches(selected, display); }) != 1)
        return {MaiErrorCode::InvalidInput, "display geometry changed while observing; observe again"};
    if (executionWindow) {
        const auto windows = impl_->backend->windows();
        if (!windows) return windows.error();
        if (std::count_if(windows.value().begin(), windows.value().end(),
                          [&](const auto& window) { return executionWindowMatches(*executionWindow, window); }) != 1)
            return {MaiErrorCode::InvalidInput, "execution window changed while observing; observe again"};
    }
    if (screenshot.pngBytes.empty() || screenshot.pngBytes.size() > 20 * 1024 * 1024 || screenshot.width <= 0 ||
        screenshot.height <= 0)
        return {MaiErrorCode::Protocol, "display capture returned an empty or oversized image"};
    if (!insideWorkspace()) return {MaiErrorCode::InvalidInput, "display image path changed"};
    QSaveFile file(QString::fromUtf8(destination.data(), int(destination.size())));
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(reinterpret_cast<const char*>(screenshot.pngBytes.data()), qint64(screenshot.pngBytes.size())) !=
            qint64(screenshot.pngBytes.size()) ||
        !file.commit())
        return {MaiErrorCode::Internal, "could not save display observation"};
    if (canceled() || !insideWorkspace() || desktopInputGeneration.load() != inputGeneration) {
        MaiFileSystem::removeFile(MaiFilePath::fromUtf8(destination));
        if (canceled()) return {MaiErrorCode::Canceled, "desktop observation canceled"};
        if (!insideWorkspace()) return {MaiErrorCode::InvalidInput, "display image path changed"};
        return {MaiErrorCode::Busy, "desktop input changed while observing; observe again"};
    }
    auto state = std::make_shared<ObservedWindow>();
    state->desktopScope = true;
    state->display = selected;
    state->workspace = resolvedWorkspace.toUtf8();
    state->frameSequence = screenshot.frameSequence;
    state->inputGeneration = inputGeneration;
    state->token = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    state->turnId = turnId;
    state->elementWindowId = elementWindowId;
    state->executionWindow = executionWindow;
    state->observedAt = std::chrono::steady_clock::now();
    Json output = displayJson(selected);
    output["target"] = "display";
    output["snapshot_id"] = state->token;
    output["image_path"] = relative;
    output["mime_type"] = "image/png";
    output["has_image"] = true;
    output["image_width"] = screenshot.width;
    output["image_height"] = screenshot.height;
    output["frame_sequence"] = screenshot.frameSequence;
    output["frame_received_at_ms"] = screenshot.frameReceivedAtMs;
    output["coordinate_space"] = "relative fractions 0..1 within the captured display";
    output["native_screen_bounds_unit"] = nativeScreenUnit();
    output["input_coordinate_space"] = {
        {"space", "display"}, {"space_id", std::to_string(selected.id)}, {"unit", "ratio"}};
    output["image_coordinate_space"] = {{"space", "image"}, {"space_id", relative}, {"unit", "pixel"}};
    if (executionWindow) {
        output["execution_window_id"] = std::to_string(executionWindow->id);
        output["execution_window"] = windowJson(*executionWindow);
    }
    if (elementWindowId) {
        output["element_window_id"] = std::to_string(elementWindowId);
        const auto sources = impl_->backend->windows();
        std::size_t matches = 0;
        DesktopNativeWindow source;
        if (sources) {
            for (const auto& window : sources.value()) {
                if (window.id != elementWindowId) continue;
                source = window;
                ++matches;
            }
        }
        if (!sources || matches != 1) {
            unavailableAccessibility(
                output, "source_missing",
                sources ? "selected element source is missing or ambiguous" : sources.error().message());
        } else {
            state->elementWindow = source;
            output["element_window"] = windowJson(source);
            const bool intersects = source.x < selected.x + selected.width && source.x + source.width > selected.x &&
                                    source.y < selected.y + selected.height && source.y + source.height > selected.y;
            if (!currentWindowMatches(source, source) || !intersects) {
                unavailableAccessibility(output, "source_outside_input_bounds");
            } else {
                const auto elements = impl_->backend->inspectElements(source, isCanceled);
                if (!elements)
                    unavailableAccessibility(output, "provider_error", elements.error().message());
                else {
                    const auto currentSources = impl_->backend->windows();
                    if (!currentSources ||
                        std::count_if(currentSources.value().begin(), currentSources.value().end(),
                                      [&](const auto& now) { return currentWindowMatches(source, now); }) != 1)
                        unavailableAccessibility(output, "source_changed");
                    else
                        publishElements(output, *state, elements.value(), selected.x, selected.y, selected.width,
                                        selected.height);
                }
            }
        }
    }
    bool receiverChanged = false;
    if (executionWindow) {
        const auto windows = impl_->backend->windows();
        receiverChanged =
            !windows || std::count_if(windows.value().begin(), windows.value().end(), [&](const auto& now) {
                            return executionWindowMatches(*executionWindow, now);
                        }) != 1;
    }
    if (canceled() || desktopInputGeneration.load() != inputGeneration || receiverChanged) {
        MaiFileSystem::removeFile(MaiFilePath::fromUtf8(destination));
        return {canceled() ? MaiErrorCode::Canceled : MaiErrorCode::Busy,
                "desktop observation or execution window changed during inspection; observe again"};
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->sessions[sessionId] = state;
    }
    return DesktopComputerObservation{output.dump(), destination};
}

MaiResult<std::vector<DesktopNativeWindow>> DesktopComputerController::windows() {
    return impl_->backend->windows();
}

MaiResult<DesktopComputerObservation> DesktopComputerController::observe(
    const std::string& sessionId, const std::string& windowTitle, const std::string& workspace, bool includeElements,
    std::uint64_t windowId, bool includeImage, std::function<bool()> isCanceled, const std::string& turnId) {
    impl_->inputOwnership->touch(sessionId, turnId);
    // Cancellation is checked synchronously and never retained with session state.
    const auto canceled = [&] { return isCanceled && isCanceled(); };
    const auto cleanupOnCancel = qScopeGuard([&] {
        if (canceled()) releaseInput(sessionId, turnId);
    });
    if (canceled()) return {MaiErrorCode::Canceled, "computer observation canceled"};
    if (sessionId.empty() || (windowTitle.empty() == (windowId == 0)) || windowTitle.size() > 500 || workspace.empty())
        return {MaiErrorCode::InvalidInput, "Session, exact window title and workspace are required"};
    const auto listed = impl_->backend->windows();
    if (!listed) return listed.error();
    DesktopNativeWindow selected;
    int count = 0;
    for (const auto& window : listed.value()) {
        if (windowId ? window.id == windowId : window.title == windowTitle) {
            selected = window;
            ++count;
        }
    }
    if (count != 1)
        return {MaiErrorCode::NotFound, count == 0 ? "exact visible window title was not found"
                                                   : "window title is ambiguous; select a unique title"};
    const bool metadataOnly = selected.minimized || selected.cloaked;
    if (metadataOnly && includeImage)
        return {MaiErrorCode::Busy,
                "minimized or cloaked windows support metadata observation only; no screenshot is available"};
    if (!metadataOnly && (!selected.boundsAvailable || !std::isfinite(selected.width) ||
                          !std::isfinite(selected.height) || selected.width <= 0 || selected.height <= 0))
        return {MaiErrorCode::InvalidInput, "selected window has invalid bounds"};

    MaiScreenshot capture;
    std::string relativeImagePath;
    std::string captureDirectory;
    MaiFilePath imageFile;
    if (includeImage) {
        // The model needs a workspace-relative file reference as well as the
        // pixels, so the same capture can be delivered without a shell recapture.
        captureDirectory = maiResolvePathWithinRoot(workspace, "computer");
        if (captureDirectory.empty())
            return {MaiErrorCode::InvalidInput, "screenshot directory leaves the Session workspace"};
        const std::string name = MaiIdGenerator::generate("computer_") + ".png";
        relativeImagePath = "computer/" + name;
        const std::string resolved = maiResolvePathWithinRoot(
            workspace, MaiFilePath::fromUtf8(captureDirectory).append(MaiFilePath::fromUtf8(name)).toUtf8());
        if (resolved.empty()) return {MaiErrorCode::InvalidInput, "screenshot path leaves the Session workspace"};
        imageFile = MaiFilePath::fromUtf8(resolved);
        const MaiError created = MaiFileSystem::createDirectories(imageFile.dirName());
        if (created) return created;
        if (maiResolvePathWithinRoot(workspace, "computer") != captureDirectory ||
            maiResolvePathWithinRoot(workspace, imageFile.toUtf8()) != imageFile.toUtf8())
            return {MaiErrorCode::InvalidInput, "screenshot path changed outside the Session workspace"};
        if (canceled()) return {MaiErrorCode::Canceled, "computer observation canceled"};
        const MaiError captured = impl_->backend->capture(selected, capture);
        if (captured) return captured;
        if (canceled()) return {MaiErrorCode::Canceled, "computer observation canceled"};
    }
    // Capture and accessibility reads can take time. Re-check the same ID/PID
    // and bounds before issuing a token; a recycled handle is a different target.
    const auto afterCapture = impl_->backend->windows();
    if (!afterCapture) return afterCapture.error();
    if (std::none_of(afterCapture.value().begin(), afterCapture.value().end(),
                     [&](const DesktopNativeWindow& now) { return observationWindowMatches(selected, now); }))
        return {MaiErrorCode::InvalidInput, "selected window changed while observing; observe again"};
    std::string imagePath;
    if (includeImage) {
        if (capture.pngBytes.empty() || capture.width <= 0 || capture.height <= 0 ||
            capture.pngBytes.size() > 20 * 1024 * 1024)
            return {MaiErrorCode::Protocol, "selected window screenshot was empty or oversized"};
        const std::string nativePath = imageFile.toUtf8();
        if (maiResolvePathWithinRoot(workspace, "computer") != captureDirectory ||
            maiResolvePathWithinRoot(workspace, nativePath) != nativePath)
            return {MaiErrorCode::InvalidInput, "screenshot path changed outside the Session workspace"};
        if (canceled()) return {MaiErrorCode::Canceled, "computer observation canceled"};
        QSaveFile file(QString::fromUtf8(nativePath.c_str(), int(nativePath.size())));
        if (!file.open(QIODevice::WriteOnly) ||
            file.write(reinterpret_cast<const char*>(capture.pngBytes.data()), qint64(capture.pngBytes.size())) !=
                qint64(capture.pngBytes.size()) ||
            !file.commit())
            return {MaiErrorCode::Internal, "could not save selected window screenshot"};
        if (maiResolvePathWithinRoot(workspace, "computer") != captureDirectory ||
            maiResolvePathWithinRoot(workspace, nativePath) != nativePath)
            return {MaiErrorCode::InvalidInput, "screenshot path changed outside the Session workspace"};
        imagePath = nativePath;
    }

    Json output{
        {"window_id", std::to_string(selected.id)},
        {"title", selected.title},
        {"application", selected.application},
        {"process_id", selected.processId},
        {"minimized", selected.minimized},
        {"cloaked", selected.cloaked},
        {"bounds_available", selected.boundsAvailable},
        {"bounds", {{"x", selected.x}, {"y", selected.y}, {"width", selected.width}, {"height", selected.height}}},
        {"image_width", capture.width},
        {"image_height", capture.height},
        {"has_image", includeImage},
        {"coordinate_space", "relative fractions 0..1 within the captured window"}};
    if (includeImage) {
        output["image_path"] = relativeImagePath;
        output["mime_type"] = "image/png";
        // A picture is not proof of a native input transform. Windows WGC's
        // DWM-frame correspondence is independently measured; the current Mac
        // capture API does not publish its surface/content origin transform.
#if defined(Q_OS_WIN)
        output["image_coordinates_usable_for_input"] = true;
#else
        output["image_coordinates_usable_for_input"] = false;
        output["image_coordinate_mapping_note"] =
            "Window-image/native-frame alignment is unverified. Use display observe with execution_window_id "
            "for image-derived positions, or use native accessibility element geometry.";
#endif
    }
    auto state = std::make_shared<ObservedWindow>();
    state->window = selected;
    if (includeImage) {
        state->screenshotId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        state->imagePath = imagePath;
        state->screenshotUrl =
            "data:image/png;base64," +
            QByteArray(reinterpret_cast<const char*>(capture.pngBytes.data()), int(capture.pngBytes.size()))
                .toBase64()
                .toStdString();
#if defined(Q_OS_WIN)
        state->screenshotCoordinatesUsable = true;
#endif
    }
    output["native_screen_bounds_unit"] = nativeScreenUnit();
    if (includeElements) {
        if (metadataOnly)
            unavailableAccessibility(output, "window_has_no_input_geometry");
        else {
            state->elementWindow = selected;
            const auto elements = impl_->backend->inspectElements(selected, isCanceled);
            if (elements)
                publishElements(output, *state, elements.value(), selected.x, selected.y, selected.width,
                                selected.height);
            else
                unavailableAccessibility(output, "provider_error", elements.error().message());
            const auto current = impl_->backend->windows();
            if (!current || std::count_if(current.value().begin(), current.value().end(),
                                          [&](const auto& now) { return currentWindowMatches(selected, now); }) != 1)
                return {MaiErrorCode::InvalidInput, "selected window changed during element inspection; observe again"};
        }
    }
    if (canceled()) return {MaiErrorCode::Canceled, "computer observation canceled"};
    state->token = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    state->turnId = turnId;
    state->observedAt = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->sessions[sessionId] = state;
    }
    output["snapshot_id"] = state->token;
    output["input_coordinate_space"] = {
        {"space", "window"}, {"space_id", std::to_string(selected.id)}, {"unit", "ratio"}};
    if (includeImage)
        output["image_coordinate_space"] = {{"space", "image"}, {"space_id", relativeImagePath}, {"unit", "pixel"}};
    return DesktopComputerObservation{output.dump(), imagePath};
}

MaiError DesktopComputerController::act(const std::string& sessionId, const std::string& action,
                                        const std::string& snapshotId, std::uint64_t windowId, double xRatio,
                                        double yRatio, const std::string& text, const std::string& key,
                                        int scrollLines) {
    DesktopComputerAction request;
    request.action = action;
    request.snapshotId = snapshotId;
    request.windowId = windowId;
    request.xRatio = xRatio;
    request.yRatio = yRatio;
    request.text = text;
    request.key = key;
    request.scrollLines = scrollLines;
    const auto result = perform(sessionId, request);
    return result ? MaiError{} : result.error();
}

MaiResult<DesktopComputerActionResult> DesktopComputerController::perform(const std::string& sessionId,
                                                                          const DesktopComputerAction& originalRequest,
                                                                          DesktopComputerActionProgress* progress) {
    if (progress) *progress = {};
    // Receiver identity comes from the snapshot, not from an action that could
    // accidentally omit or override it. Keep the caller's ordinary DTO intact.
    DesktopComputerAction request = originalRequest;
    // Keep validation and failure cleanup in the same serialized operation.
    // A reset cannot slip between unlocking input and an older failure guard.
    std::unique_lock<std::mutex> desktopLock(desktopOperationMutex, std::try_to_lock);
    if (!desktopLock.owns_lock()) return {MaiErrorCode::Busy, "another desktop operation is in flight"};
    const std::string& action = request.action;
    const bool eventBatch = action == "input_events";
    if (eventBatch) {
        auto error = maiDesktopValidateInputEvents(request.events);
        if (!error) error = maiDesktopValidateInputOptions(request.inputOptions);
        if (error) {
            if (!request.dryRun) {
                releaseInput(sessionId, request.turnId);
                if (progress) progress->needsObservation = true;
            }
            return error;
        }
        if (maiDesktopInputEventsOnlyRelease(request.events)) {
            // Owned cleanup is independent of screenshots, focus and canceled
            // tasks. A previous turn cannot release a replacement turn's lease.
            if (!request.dryRun && request.isCanceled && request.isCanceled()) releaseInput(sessionId, request.turnId);

            const auto native = impl_->inputOwnership->prepare(sessionId, request, false, true);
            if (!native) return native.error();
            DesktopComputerActionResult receipt;
            receipt.state = request.dryRun ? "validated" : "released";
            receipt.dryRun = request.dryRun;
            if (!request.dryRun && native.value()) {
                DesktopInputBatch batch;
                if (progress) {
                    progress->inputDispatchAttempted = true;
                    progress->needsObservation = true;
                }
                const auto sent = native.value()->inputEvents(request.events, {}, {}, request.inputOptions);
                impl_->invalidate(sessionId, request.turnId, true);
                if (sent) return inputDispatchFailure(sent);
                receipt.dispatched = true;
                receipt.acceptedEvents = request.events.size();
            }
            receipt.inputState = inputState(sessionId);
            return receipt;
        }
    }
    if (action == "mouse_up" || action == "key_up") {
        if (action == "mouse_up" && (int(request.button) < int(DesktopMouseButton::Left) ||
                                     int(request.button) > int(DesktopMouseButton::Forward)))
            return {MaiErrorCode::InvalidInput, "invalid mouse button"};
        if (!request.dryRun && request.isCanceled && request.isCanceled()) releaseInput(sessionId, request.turnId);
        if (action == "key_up") {
            const auto parsed = maiDesktopParseKey(request.key);
            if (!parsed) return parsed.error();
            if (!parsed.value().modifiers.empty())
                return {MaiErrorCode::InvalidInput, "key_up needs one physical key, not a chord"};
        }

        const auto native = impl_->inputOwnership->prepare(sessionId, request, false, true);
        if (!native) return native.error();
        DesktopComputerActionResult receipt;
        receipt.state = "released";
        if (request.dryRun) {
            receipt.state = "validated";
            receipt.dryRun = true;
        } else if (native.value() && ownsRequestedUp(native.value()->state(), request)) {
            DesktopNativeInput input;
            input.kind = action == "mouse_up" ? DesktopNativeInputKind::MouseUp : DesktopNativeInputKind::KeyUp;
            input.button = request.button;
            input.key = request.key;
            DesktopInputBatch batch;
            if (progress) {
                progress->inputDispatchAttempted = true;
                progress->needsObservation = true;
            }
            const auto result = native.value()->input(input);
            impl_->invalidate(sessionId, request.turnId, true);
            if (result) return inputDispatchFailure(result);
            receipt.dispatched = true;
        }
        receipt.inputState = inputState(sessionId);
        return receipt;
    }
    bool completed = false;
    const auto cleanupOnFailure = qScopeGuard([&] {
        if (completed || request.dryRun) return;
        if (request.isCanceled && request.isCanceled())
            releaseInput(sessionId, request.turnId);
        else {
            // Ordinary syntax/stale-call rejection only retires an owned native
            // lease. It must not revoke a newer observation after a replay.
            bool released = false;
            impl_->inputOwnership->release(sessionId, request.turnId, &released);
            if (released) impl_->invalidate(sessionId, request.turnId, true);
        }
        const auto selected = impl_->get(sessionId);
        if (progress && (!selected || selected->invalidated.load() || selected->token.empty()))
            progress->needsObservation = true;
    });
    const auto success = [&](DesktopComputerActionResult receipt) -> MaiResult<DesktopComputerActionResult> {
        receipt.inputState = inputState(sessionId);
        completed = true;
        return receipt;
    };
    const bool positioned = action == "click" || action == "double_click" || action == "right_click" ||
                            action == "drag" || action == "scroll" || action == "move" || action == "mouse_down";
    const bool windowState = action == "minimize" || action == "restore";
    const bool elementClick = action == "click" || action == "double_click" || action == "right_click" ||
                              (eventBatch && !request.elementId.empty());
    if (!request.elementId.empty() && !elementClick)
        return {MaiErrorCode::InvalidInput, "element_id is supported only for click, double_click and right_click"};
    if (windowState && request.desktopScope)
        return {MaiErrorCode::InvalidInput, "window state operations require target=window"};
    const bool writesText = action == "type" || action == "replace_text";
    const bool batchText =
        eventBatch && std::any_of(request.events.begin(), request.events.end(),
                                  [](const auto& item) { return item.kind == DesktopInputEventKind::UnicodeText; });
    if (positioned &&
        (int(request.button) < int(DesktopMouseButton::Left) || int(request.button) > int(DesktopMouseButton::Forward)))
        return {MaiErrorCode::InvalidInput, "invalid mouse button"};
    if (action == "scroll" && request.scrollAxis != DesktopScrollAxis::Vertical &&
        request.scrollAxis != DesktopScrollAxis::Horizontal)
        return {MaiErrorCode::InvalidInput, "invalid scroll axis"};
    if (action != "focus" && !windowState && !eventBatch && !positioned && !writesText && action != "key" &&
        action != "key_down" && action != "verify")
        return {MaiErrorCode::InvalidInput, "unknown computer action"};
    if (positioned && request.elementId.empty() && (!ratio(request.xRatio) || !ratio(request.yRatio)))
        return {MaiErrorCode::InvalidInput, "window coordinates must be fractions from 0 to 1"};
    if (action == "drag" && (!ratio(request.toXRatio) || !ratio(request.toYRatio) || request.durationMs < 1 ||
                             request.durationMs > (request.desktopScope ? 5000 : 200)))
        return {MaiErrorCode::InvalidInput, "drag needs bounded end coordinates and a valid duration"};
    if (writesText && !validText(request.text, action == "replace_text"))
        return {MaiErrorCode::InvalidInput,
                "text must be valid UTF-8 within 4000 bytes; only replacement may be empty"};
    if (action == "verify" && !validText(request.expectedText, true))
        return {MaiErrorCode::InvalidInput, "expected text must be valid UTF-8 within 4000 bytes"};
    bool keyNeedsInputCheck = false;
    if (eventBatch) {
        keyNeedsInputCheck = std::any_of(request.events.begin(), request.events.end(), [](const auto& item) {
            // Raw bindings can remap even a modifier/navigation code into an
            // editing key. The native effective binding is authoritative, so
            // the controller conservatively checks every raw down here.
            return item.kind == DesktopInputEventKind::RawKey && !item.key.release;
        });
    }
    if (action == "key" || action == "key_down") {
        const auto parsed = maiDesktopParseKey(request.key);
        if (!parsed) return parsed.error();
        if (action == "key_down" && !parsed.value().modifiers.empty())
            return {MaiErrorCode::InvalidInput, "key_down needs one physical key, not a chord"};
        // A shortcut can write through the clipboard or delete a protected
        // value just like type(). Only navigation away is exempt from the
        // same secure-input check; raw character keys cannot bypass it.
        const auto key = parsed.value().key;
        DesktopKeyModifier modifier;
        keyNeedsInputCheck =
            key != DesktopKeyCode::Tab && key != DesktopKeyCode::Escape && !maiDesktopKeyModifier(key, modifier);
        if (request.desktopScope) {
            auto effective = parsed.value();
            for (const auto& held : inputState(sessionId).keysDown) {
                const auto physical = maiDesktopParseKey(held);
                DesktopKeyModifier modifier;
                if (!physical || !maiDesktopKeyModifier(physical.value().key, modifier)) continue;
                const auto family = maiDesktopModifierFamily(modifier);
                if (std::none_of(effective.modifiers.begin(), effective.modifiers.end(),
                                 [&](const auto existing) { return maiDesktopModifierFamily(existing) == family; }))
                    effective.modifiers.push_back(modifier);
            }
            if (desktopLauncherKey(effective)) keyNeedsInputCheck = false;
        }
    }
    if (action == "scroll" && (request.scrollLines == 0 || request.scrollLines < -10 || request.scrollLines > 10))
        return {MaiErrorCode::InvalidInput, "scroll lines must be -10..10, excluding zero"};
    const auto canceled = [&] { return request.isCanceled && request.isCanceled(); };
    if (canceled()) return {MaiErrorCode::Canceled, "computer operation canceled"};
    const auto state = impl_->get(sessionId);
    if (!state) return {MaiErrorCode::NotFound, "observe a window in this Session first"};
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->invalidated.load() || state->token.empty() || state->token != request.snapshotId ||
        state->desktopScope != request.desktopScope ||
        (state->desktopScope ? state->display.id != request.displayId : state->window.id != request.windowId) ||
        std::chrono::steady_clock::now() - state->observedAt > std::chrono::minutes(5))
        return {MaiErrorCode::InvalidInput, "window observation expired; observe again"};
    // Only one controller may manipulate the physical desktop at a time. The
    // native layer also checks target identity between multi-event inputs.

    if (state->invalidated.load()) return {MaiErrorCode::Canceled, "input observation was revoked"};
    request.executionWindowId = state->executionWindow ? state->executionWindow->id : 0;
    request.executionWindowProcessId = state->executionWindow ? state->executionWindow->processId : 0;
    const auto inputCanceled = [&request, state] {
        return state->invalidated.load() || (request.isCanceled && request.isCanceled());
    };
    const auto evidenceFailure = [&](const std::string& message) -> MaiError {
        if (!request.dryRun) {
            state->invalidated.store(true);
            if (progress) progress->needsObservation = true;
        }
        return {MaiErrorCode::InvalidInput, message};
    };
    if (!request.dryRun) {
        const auto allowed = impl_->inputOwnership->prepare(sessionId, request, false);
        if (!allowed) return allowed.error();
    }
    std::optional<std::pair<double, double>> elementPoint;
    const auto resolveElementPoint = [&]() -> MaiError {
        if (request.elementId.empty()) return {};
        const auto found = state->elements.find(request.elementId);
        if (found == state->elements.end() || !state->elementWindow)
            return {MaiErrorCode::InvalidInput, "element_id does not belong to this Session snapshot; observe again"};
        bool resolved = false;
        const auto revokeChangedEvidence = qScopeGuard([&] {
            if (!resolved && !request.dryRun) {
                state->invalidated.store(true);
                if (progress) progress->needsObservation = true;
            }
        });
        const auto& observed = found->second;
        if (!observed.enabled || observed.offscreen)
            return {MaiErrorCode::InvalidInput, "observed element is disabled or offscreen; observe again"};
        const auto sources = impl_->backend->windows();
        if (!sources) return sources.error();
        if (std::count_if(sources.value().begin(), sources.value().end(),
                          [&](const auto& now) { return currentWindowMatches(*state->elementWindow, now); }) != 1)
            return {MaiErrorCode::InvalidInput, "element source window changed; observe again"};
        const auto fresh = impl_->backend->resolveElement(*state->elementWindow, observed, inputCanceled);
        if (!fresh) return fresh.error();
        if (!elementMatches(observed, fresh.value()))
            return {MaiErrorCode::InvalidInput, "observed element changed, is disabled or is offscreen; observe again"};
        const auto after = impl_->backend->windows();
        if (!after || std::count_if(after.value().begin(), after.value().end(), [&](const auto& now) {
                          return currentWindowMatches(*state->elementWindow, now);
                      }) != 1)
            return {MaiErrorCode::InvalidInput, "element source window changed while resolving; observe again"};
        const auto& target = state->desktopScope
                                 ? state->display
                                 : DesktopNativeDisplay{state->window.id, state->window.x, state->window.y,
                                                        state->window.width, state->window.height};
        const double x = fresh.value().x + fresh.value().width / 2;
        const double y = fresh.value().y + fresh.value().height / 2;
        if (x < target.x || y < target.y || x >= target.x + target.width || y >= target.y + target.height)
            return {MaiErrorCode::InvalidInput, "element center is outside the selected input bounds; observe again"};
        elementPoint = {x, y};
        resolved = true;
        return {};
    };
    if (writesText || batchText) {
        for (const auto& key : inputState(sessionId).keysDown) {
            const auto parsed = maiDesktopParseKey(key);
            DesktopKeyModifier modifier;
            if (parsed && maiDesktopKeyModifier(parsed.value().key, modifier))
                return {MaiErrorCode::Busy,
                        "literal text needs released modifiers; use key for composed physical input"};
        }
    }
    if (state->desktopScope) {
        if (state->inputGeneration != desktopInputGeneration.load())
            return evidenceFailure("desktop input changed since this snapshot; observe again");
        if (request.workspace.empty() ||
            MaiFileSystem::resolve(MaiFilePath::fromUtf8(request.workspace)).toUtf8() != state->workspace)
            return evidenceFailure("workspace changed; observe this display again");
        if (request.windowId != 0 || action == "focus" || action == "verify")
            return {MaiErrorCode::InvalidInput, "desktop actions use a display and visual observation"};
        const auto current = impl_->backend->displays();
        if (!current) return current.error();
        if (std::count_if(current.value().begin(), current.value().end(),
                          [&](const auto& display) { return displayMatches(state->display, display); }) != 1)
            return evidenceFailure("display geometry changed; observe again");
        if (state->executionWindow) {
            const auto windows = impl_->backend->windows();
            if (!windows) return windows.error();
            if (std::count_if(windows.value().begin(), windows.value().end(), [&](const auto& now) {
                    return executionWindowMatches(*state->executionWindow, now);
                }) != 1)
                return evidenceFailure("execution window changed; observe again");
        }
        const auto resolved = resolveElementPoint();
        if (resolved) return resolved;
        if (state->executionWindow) {
            const auto& receiver = *state->executionWindow;
            const auto point = [&](double x, double y) {
                return executionPointError(receiver, state->display.x + x * (std::max)(1.0, state->display.width - 1),
                                           state->display.y + y * (std::max)(1.0, state->display.height - 1));
            };
            const bool positioned = action == "move" || action == "click" || action == "double_click" ||
                                    action == "right_click" || action == "drag" || action == "scroll" ||
                                    action == "mouse_down";
            if (positioned) {
                const auto error = elementPoint
                                       ? executionPointError(receiver, elementPoint->first, elementPoint->second)
                                       : point(request.xRatio, request.yRatio);
                if (error) return error;
                if (action == "drag") {
                    const auto error = point(request.toXRatio, request.toYRatio);
                    if (error) return error;
                }
            }
            for (const auto& event : request.events) {
                if (event.kind == DesktopInputEventKind::AbsoluteMove) {
                    if (const auto error = point(event.x, event.y)) return error;
                } else if (event.kind == DesktopInputEventKind::RelativeMove && (event.deltaX || event.deltaY)) {
                    // Native relative deltas are subject to OS acceleration;
                    // predicting their destination would manufacture evidence.
                    return {MaiErrorCode::NotSupported,
                            "window-restricted input requires absolute motion; native relative motion is not bounded"};
                } else if (event.kind == DesktopInputEventKind::Touch) {
                    for (const auto& contact : event.contacts) {
                        if (contact.phase == DesktopInputPhase::Up || contact.phase == DesktopInputPhase::Cancel ||
                            contact.phase == DesktopInputPhase::CancelAll)
                            continue;
                        if (const auto error = point(contact.x, contact.y)) return error;
                    }
                } else if (event.kind == DesktopInputEventKind::Pen && event.pen.phase != DesktopInputPhase::Up &&
                           event.pen.phase != DesktopInputPhase::Cancel &&
                           event.pen.phase != DesktopInputPhase::CancelAll &&
                           event.pen.phase != DesktopInputPhase::HoverLeave &&
                           event.pen.phase != DesktopInputPhase::ButtonOnly) {
                    if (const auto error = point(event.pen.x, event.pen.y)) return error;
                }
            }
            const auto focused = impl_->backend->inputFocused(receiver);
            if (!focused) return focused.error();
            if (!focused.value())
                return {MaiErrorCode::Busy, "execution window is not focused; observe and focus it explicitly"};
        }
        DesktopComputerActionResult receipt;
        if (request.dryRun) {
            receipt.state = "validated";
            receipt.dryRun = true;
            return success(std::move(receipt));
        }
        if (writesText || batchText || keyNeedsInputCheck) {
            const auto secure = impl_->backend->secureInput();
            if (!secure) return secure.error();
            if (secure.value()) return {MaiErrorCode::NotSupported, "secure inputs need manual user entry"};
        }
        if (canceled()) return {MaiErrorCode::Canceled, "desktop operation canceled"};
        DesktopNativeInput input;
        input.desktopScope = true;
        input.display = state->display;
        input.executionWindow = state->executionWindow;
        input.text = request.text;
        input.key = request.key;
        input.lines = request.scrollLines;
        input.durationMs = request.durationMs;
        input.button = request.button;
        input.scrollAxis = request.scrollAxis;
        input.isCanceled = inputCanceled;
        input.x = state->display.x + request.xRatio * (std::max)(1.0, state->display.width - 1);
        input.y = state->display.y + request.yRatio * (std::max)(1.0, state->display.height - 1);
        if (elementPoint) {
            input.x = elementPoint->first;
            input.y = elementPoint->second;
        }
        input.toX = state->display.x + request.toXRatio * (std::max)(1.0, state->display.width - 1);
        input.toY = state->display.y + request.toYRatio * (std::max)(1.0, state->display.height - 1);
        input.kind = action == "move"           ? DesktopNativeInputKind::Move
                     : action == "click"        ? DesktopNativeInputKind::Click
                     : action == "double_click" ? DesktopNativeInputKind::DoubleClick
                     : action == "right_click"  ? DesktopNativeInputKind::RightClick
                     : action == "drag"         ? DesktopNativeInputKind::Drag
                     : action == "type"         ? DesktopNativeInputKind::Type
                     : action == "replace_text" ? DesktopNativeInputKind::ReplaceFocusedText
                     : action == "key"          ? DesktopNativeInputKind::Key
                     : action == "mouse_down"   ? DesktopNativeInputKind::MouseDown
                     : action == "key_down"     ? DesktopNativeInputKind::KeyDown
                                                : DesktopNativeInputKind::Scroll;
        // Composite and primitive operations enter the same owned native
        // executor, even when a one-shot operation begins without held keys.
        const auto created = impl_->inputOwnership->prepare(sessionId, request, true);
        if (!created) return created.error();
        const auto native = created.value();
        state->token.clear();
        if (progress) {
            progress->inputDispatchAttempted = true;
            progress->needsObservation = true;
        }
        DesktopInputBatch inputBatch;
        const MaiError sent =
            eventBatch ? native->inputEvents(request.events, input,
                                             inputPort(state->display.x, state->display.y, state->display.width,
                                                       state->display.height, current.value()),
                                             request.inputOptions)
                       : native->input(input);
        if (sent) return inputDispatchFailure(sent);
        inputBatch.finish();
        receipt.state = "dispatched";
        receipt.dispatched = true;
        if (eventBatch) receipt.acceptedEvents = request.events.size();
        receipt.inputState = inputState(sessionId);
        const auto next = [&]() -> MaiResult<DesktopComputerObservation> {
            if (inputCanceled()) return {MaiErrorCode::Canceled, "execution observation was revoked after input"};
            if (state->executionWindow) {
                const auto windows = impl_->backend->windows();
                if (!windows) return windows.error();
                if (std::count_if(windows.value().begin(), windows.value().end(), [&](const auto& now) {
                        return executionIdentityMatches(*state->executionWindow, now);
                    }) != 1)
                    return {MaiErrorCode::InvalidInput,
                            "execution window identity changed after input; observe explicitly before further input"};
            }
            auto observed =
                observeDisplay(sessionId, state->workspace, state->display.id, inputCanceled, state->frameSequence,
                               MaiTime::getCurrentTime(), 0, request.turnId, state->elementWindowId,
                               state->executionWindow ? state->executionWindow->id : 0);
            if (!observed || !state->executionWindow) return observed;
            const auto successor = impl_->get(sessionId);
            const auto metadata = Json::parse(observed.value().json, nullptr, false);
            const bool sameSnapshot = successor && metadata.is_object() && metadata.contains("snapshot_id") &&
                                      metadata["snapshot_id"] == successor->token;
            if (!sameSnapshot || inputCanceled() || successor->invalidated.load() || !successor->executionWindow ||
                !executionIdentityMatches(*state->executionWindow, *successor->executionWindow)) {
                // Revoke only this handoff, never a concurrently selected new
                // receiver. The newly captured frame is not published to tools.
                if (sameSnapshot) impl_->invalidate(sessionId, request.turnId, false, successor);
                if (!observed.value().imagePath.empty())
                    MaiFileSystem::removeFile(MaiFilePath::fromUtf8(observed.value().imagePath));
                return {MaiErrorCode::InvalidInput, "execution window identity changed during successor observation"};
            }
            return observed;
        }();
        if (next)
            receipt.observation = next.value();
        else {
            receipt.state = "dispatched_observation_failed";
            receipt.observationError = next.error().message();
            if (state->executionWindow) {
                // Holds belong to the original receiver even when the input
                // itself succeeded. A failed handoff cannot transfer them.
                const auto released = impl_->inputOwnership->release(sessionId, request.turnId);
                if (released) receipt.observationError += "; owned cleanup: " + released.message();
            }
        }
        if (canceled()) impl_->inputOwnership->release(sessionId, request.turnId);
        return success(std::move(receipt));
    }
    const auto listed = impl_->backend->windows();
    if (!listed) return listed.error();
    bool stillVisible = false;
    DesktopNativeWindow currentWindow;
    for (const auto& window : listed.value()) {
        if (window.id == request.windowId) {
            stillVisible = windowState ? observationWindowMatches(state->window, window)
                                       : currentWindowMatches(state->window, window);
            currentWindow = window;
            break;
        }
    }
    if (!stillVisible) return evidenceFailure("selected window changed; observe again");
    if (!state->canonicalApp.empty()) {
        const auto app = impl_->backend->applicationId(currentWindow);
        if (!app || app.value() != state->canonicalApp)
            return evidenceFailure("observed application identity changed; observe again");
    }
    if (windowState && currentWindow.cloaked)
        return {MaiErrorCode::NotSupported, "cloaked windows cannot change state across virtual desktops"};
    const auto resolved = resolveElementPoint();
    if (resolved) return resolved;
    if (canceled()) return {MaiErrorCode::Canceled, "computer operation canceled"};
    DesktopComputerActionResult receipt;
    if (request.dryRun) {
        receipt.state = "validated";
        receipt.dryRun = true;
        return success(std::move(receipt));  // No activation, input, focused text read or token consumption.
    }
    if (windowState) {
        // Retire this owner's holds without revoking the authorization that the
        // setter is about to consume. External Stop still uses atomic revoke.
        const auto released = impl_->inputOwnership->release(sessionId, request.turnId);
        if (released) return released;
        if (inputCanceled()) return {MaiErrorCode::Canceled, "window state operation canceled"};
        state->token.clear();
        if (progress) {
            progress->windowOperationAttempted = true;
            progress->needsObservation = true;
        }
        DesktopInputBatch batch;
        const auto changed = impl_->backend->setWindowState(
            currentWindow,
            action == "minimize" ? DesktopWindowStateAction::Minimize : DesktopWindowStateAction::Restore,
            inputCanceled);
        if (changed) return changed;
        const auto after = impl_->backend->windows();
        if (!after) return after.error();
        std::size_t count = 0;
        for (const auto& window : after.value()) {
            if (!windowIdentityMatches(currentWindow, window)) continue;
            receipt.afterWindow = window;
            ++count;
        }
        if (count != 1 || receipt.afterWindow->cloaked || receipt.afterWindow->minimized != (action == "minimize"))
            return {MaiErrorCode::Busy, "window state request was not verified; observe again"};
        if (inputCanceled()) return {MaiErrorCode::Canceled, "window state operation canceled after request"};
        receipt.beforeWindow = currentWindow;
        const auto focus = impl_->backend->isFocused(*receipt.afterWindow);
        if (focus) receipt.focused = focus.value();
        receipt.state = action == "minimize" ? "minimized" : "restored";
        receipt.dispatched = true;
        return success(std::move(receipt));
    }
    if (action == "verify") {
        const auto focused = impl_->backend->isFocused(state->window);
        if (!focused) return focused.error();
        if (!focused.value()) return {MaiErrorCode::Busy, "selected window is not focused; observe and focus it first"};
        const auto secure = impl_->backend->secureInput();
        if (!secure) return secure.error();
        if (secure.value()) return {MaiErrorCode::NotSupported, "secure inputs cannot be read back"};
        state->token.clear();
        const auto actual = impl_->backend->focusedText(state->window);
        if (!actual) return actual.error();
        if (canceled()) return {MaiErrorCode::Canceled, "computer operation canceled"};
        receipt.actualText = actual.value();
        const auto normalized = [](const std::string& value) {
            QString text = QString::fromUtf8(value.data(), int(value.size()));
            return text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        };
        receipt.verified = normalized(actual.value()) == normalized(request.expectedText);
        receipt.state = receipt.verified ? "verified" : "mismatch";
        return success(std::move(receipt));
    }
    // Once an OS operation is attempted its outcome may be partial. Require a
    // new observation even when activation or an input batch reports failure.
    state->token.clear();
    if (progress) {
        progress->windowOperationAttempted = true;
        progress->needsObservation = true;
    }
    DesktopInputBatch inputBatch;
    const MaiError activated = action == "focus" ? impl_->backend->activate(state->window, inputCanceled)
                                                 : impl_->backend->activateForInput(state->window, inputCanceled);
    if (activated) return activated;
    if (canceled()) return {MaiErrorCode::Canceled, "computer operation canceled"};
    const auto focused =
        action == "focus" ? impl_->backend->isFocused(state->window) : impl_->backend->inputFocused(state->window);
    if (!focused) return focused.error();
    if (!focused.value()) return {MaiErrorCode::Busy, "selected window did not become focused"};
    if (action == "focus") {
        receipt.state = "focused";
        receipt.dispatched = true;
        receipt.focused = focused.value();
        return success(std::move(receipt));
    }
    if (writesText || batchText || keyNeedsInputCheck) {
        const auto secure = impl_->backend->secureInput();
        if (!secure) return secure.error();
        if (secure.value())
            return {MaiErrorCode::NotSupported, "password and other secure inputs need manual user entry"};
    }
    // Activation can change layout or replace a control. The early locator
    // check avoids focusing an already invalid target; this read guards the
    // exact native point immediately before the existing input executor runs.
    const auto resolvedAfterActivation = resolveElementPoint();
    if (resolvedAfterActivation) return resolvedAfterActivation;
    DesktopNativeInput input;
    input.expectedWindowId = state->window.id;
    input.expectedProcessId = state->window.processId;
    input.isCanceled = inputCanceled;
    input.text = request.text;
    input.key = request.key;
    input.lines = request.scrollLines;
    input.durationMs = request.durationMs;
    input.button = request.button;
    input.scrollAxis = request.scrollAxis;
    input.x = state->window.x + request.xRatio * (std::max)(1.0, state->window.width - 1);
    input.y = state->window.y + request.yRatio * (std::max)(1.0, state->window.height - 1);
    if (elementPoint) {
        input.x = elementPoint->first;
        input.y = elementPoint->second;
    }
    input.toX = state->window.x + request.toXRatio * (std::max)(1.0, state->window.width - 1);
    input.toY = state->window.y + request.toYRatio * (std::max)(1.0, state->window.height - 1);
    input.kind = action == "move"           ? DesktopNativeInputKind::Move
                 : action == "click"        ? DesktopNativeInputKind::Click
                 : action == "double_click" ? DesktopNativeInputKind::DoubleClick
                 : action == "right_click"  ? DesktopNativeInputKind::RightClick
                 : action == "drag"         ? DesktopNativeInputKind::Drag
                 : action == "type"         ? DesktopNativeInputKind::Type
                 : action == "replace_text" ? DesktopNativeInputKind::ReplaceFocusedText
                 : action == "key"          ? DesktopNativeInputKind::Key
                 : action == "mouse_down"   ? DesktopNativeInputKind::MouseDown
                 : action == "key_down"     ? DesktopNativeInputKind::KeyDown
                                            : DesktopNativeInputKind::Scroll;
    const auto created = impl_->inputOwnership->prepare(sessionId, request, true);
    if (!created) return created.error();
    const auto native = created.value();
    DesktopInputPort port;
    if (eventBatch) {
        const auto displays = impl_->backend->displays();
        if (!displays) return displays.error();
        if (std::none_of(displays.value().begin(), displays.value().end(), validDisplay))
            return {MaiErrorCode::InvalidInput, "typed window input needs active native display geometry"};
        port = inputPort(state->window.x, state->window.y, state->window.width, state->window.height, displays.value());
        if (elementPoint) {
            for (auto& event : request.events) {
                if (event.kind != DesktopInputEventKind::AbsoluteMove) continue;
                event.x = (elementPoint->first - state->window.x) / (std::max)(1.0, state->window.width - 1);
                event.y = (elementPoint->second - state->window.y) / (std::max)(1.0, state->window.height - 1);
            }
        }
    }
    if (progress) progress->inputDispatchAttempted = true;
    const MaiError sent =
        eventBatch ? native->inputEvents(request.events, input, port, request.inputOptions) : native->input(input);
    if (sent) return inputDispatchFailure(sent);
    if (canceled())
        return inputDispatchFailure({MaiErrorCode::Canceled, "computer operation canceled after input dispatch"});
    receipt.state = "dispatched";
    receipt.dispatched = true;
    if (eventBatch) receipt.acceptedEvents = request.events.size();
    receipt.inputState = inputState(sessionId);
    return success(std::move(receipt));
}

MaiError DesktopComputerController::clear(const std::string& sessionId, const std::string& expectedTurn) {
    const auto observed = impl_->get(sessionId);
    const bool hadObservation = bool(observed);
    if (observed && !expectedTurn.empty() && observed->turnId != expectedTurn)
        return {MaiErrorCode::Busy, "selected observation belongs to another task"};
    const auto released = releaseInput(sessionId, expectedTurn);
    if (released) return released;
    // releaseInput 已按任务撤销观察；不能再 erase 并发新任务刚发布的状态。
    if (!hadObservation) return {MaiErrorCode::NotFound, "this Session has no selected window"};
    return {};
}

MaiResult<std::vector<DesktopComputerWindow>> DesktopComputerController::listWindows(
    const std::function<bool()>& canceled) {
    if (canceled && canceled()) return {MaiErrorCode::Canceled, "window discovery canceled"};
    const auto listed = impl_->backend->windows();
    if (!listed) return listed.error();
    std::vector<DesktopComputerWindow> result;
    for (const auto& window : listed.value()) {
        if (canceled && canceled()) return {MaiErrorCode::Canceled, "window discovery canceled"};
        const auto app = impl_->backend->applicationId(window);
        if (app && !app.value().empty()) result.push_back({app.value(), window.id, window.title});
    }
    return result;
}

MaiResult<DesktopComputerWindow> DesktopComputerController::getWindow(const DesktopComputerWindow& requested,
                                                                      const std::function<bool()>& canceled) {
    if (!requested.id) return {MaiErrorCode::InvalidInput, "window id must be positive"};
    if (canceled && canceled()) return {MaiErrorCode::Canceled, "window discovery canceled"};
    const auto listed = impl_->backend->windows();
    if (!listed) return listed.error();
    for (const auto& window : listed.value()) {
        if (window.id != requested.id) continue;
        const auto app = impl_->backend->applicationId(window);
        if (!app) return app.error();
        if (!requested.app.empty() && requested.app != app.value())
            return {MaiErrorCode::InvalidInput, "window app identity changed"};
        return DesktopComputerWindow{app.value(), window.id, window.title};
    }
    return {MaiErrorCode::NotFound, "window no longer exists"};
}

MaiResult<std::vector<DesktopNativeApplication>> DesktopComputerController::applications(
    const std::function<bool()>& canceled) {
    if (canceled && canceled()) return {MaiErrorCode::Canceled, "application discovery canceled"};
    return impl_->backend->applications(canceled);
}

MaiError DesktopComputerController::launchApplication(const std::string& app, const std::function<bool()>& canceled) {
    if (app.empty() || app.size() > 32768 || app.find('\0') != std::string::npos)
        return {MaiErrorCode::InvalidInput, "application identity is empty or invalid"};
    if (canceled && canceled()) return {MaiErrorCode::Canceled, "application launch canceled"};
    const auto registered = impl_->backend->applications(canceled);
    if ((!registered && registered.error().code() == MaiErrorCode::Canceled) || (canceled && canceled()))
        return {MaiErrorCode::Canceled, "application discovery canceled before launch"};
    const bool known = registered && std::any_of(registered.value().begin(), registered.value().end(),
                                                 [&](const auto& candidate) { return candidate.id == app; });
    if (!known) {
        const auto path = MaiFilePath::fromUtf8(app);
        if (!path.isAbsolute() || app.find('"') != std::string::npos || app.find('\n') != std::string::npos ||
            app.find('\r') != std::string::npos || app.find('\t') != std::string::npos)
            return {MaiErrorCode::InvalidInput,
                    "app must be a discovered identity or literal absolute executable path"};
        if (!MaiFileSystem::exists(path)) return {MaiErrorCode::NotFound, "application path does not exist"};
    }
    std::lock_guard<std::mutex> operation(desktopOperationMutex);
    // 应用启动可能改变前台；不能越过其他控制器的全局按住输入所有权。
    DesktopComputerAction gate;
    const auto owner = impl_->inputOwnership->prepare({}, gate, false);
    if (!owner) return owner.error();
    if (canceled && canceled()) return {MaiErrorCode::Canceled, "application launch canceled before dispatch"};
    DesktopInputBatch batch;
    return impl_->backend->launchApplication(app, canceled);
}

MaiError DesktopComputerController::activateWindow(const DesktopComputerWindow& requested,
                                                   const std::function<bool()>& canceled) {
    const auto current = getWindow(requested, canceled);
    if (!current) return current.error();
    std::lock_guard<std::mutex> operation(desktopOperationMutex);
    DesktopComputerAction gate;
    const auto owner = impl_->inputOwnership->prepare({}, gate, false);
    if (!owner) return owner.error();
    const auto listed = impl_->backend->windows();
    if (!listed) return listed.error();
    for (const auto& window : listed.value()) {
        if (window.id != current.value().id) continue;
        const auto app = impl_->backend->applicationId(window);
        if (!app || app.value() != current.value().app)
            return {MaiErrorCode::InvalidInput, "window identity changed before activation"};
        if (canceled && canceled()) return {MaiErrorCode::Canceled, "window activation canceled"};
        DesktopInputBatch batch;
        return impl_->backend->activate(window, canceled);
    }
    return {MaiErrorCode::NotFound, "window disappeared before activation"};
}

MaiResult<DesktopComputerWindowState> DesktopComputerController::getWindowState(
    const std::string& sessionId, const DesktopComputerWindow& requested, const std::string& workspace,
    bool includeScreenshot, bool includeText, std::function<bool()> canceled, const std::string& turnId) {
    const auto cleanup = qScopeGuard([&] {
        if (canceled && canceled()) releaseInput(sessionId, turnId);
    });
    if (!includeScreenshot && !includeText)
        return {MaiErrorCode::InvalidInput, "request a screenshot, accessibility text, or both"};
    const auto selected = getWindow(requested, canceled);
    if (!selected) return selected.error();
    const auto observed =
        observe(sessionId, {}, workspace, includeText, selected.value().id, includeScreenshot, canceled, turnId);
    if (!observed) return observed.error();
    const auto state = impl_->get(sessionId);
    if (!state) return {MaiErrorCode::Canceled, "window observation was released"};
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto app = impl_->backend->applicationId(state->window);
    const auto facts = Json::parse(observed.value().json, nullptr, false);
    if (state->invalidated.load() || impl_->get(sessionId) != state || !app || app.value() != selected.value().app ||
        state->window.id != selected.value().id || !facts.is_object() ||
        state->token != facts.value("snapshot_id", std::string()) || state->imagePath != observed.value().imagePath)
        return {MaiErrorCode::InvalidInput, "window identity changed during observation"};
    DesktopComputerWindowState result;
    state->canonicalApp = selected.value().app;
    result.window = selected.value();
    result.imageCoordinatesUsable = state->screenshotCoordinatesUsable;
    result.nativeSnapshotId = state->token;
    if (includeText && state->accessibility) {
        result.accessibility = state->accessibility;
        for (const auto& id : state->indexedElementIds) result.indexedElements.push_back(state->elements.at(id));
        // 只给真正发布过的元素分配索引。原生事实映射不到当前树时保持缺失，不能猜控件。
        const auto line = [&](const std::string& nativeId) -> std::optional<std::string> {
            for (std::size_t index = 0; index < result.indexedElements.size(); ++index) {
                const auto& element = result.indexedElements[index];
                if (element.nativeId == nativeId)
                    return std::to_string(index) + " " + element.role + " " + element.label;
            }
            return {};
        };
        if (result.accessibility->focusedElement)
            result.accessibility->focusedElement = line(*result.accessibility->focusedElement);
        if (result.accessibility->selectedElements) {
            std::vector<std::string> selectedLines;
            for (const auto& id : *result.accessibility->selectedElements)
                if (const auto value = line(id)) selectedLines.push_back(*value);
            result.accessibility->selectedElements = std::move(selectedLines);
        }
        result.accessibility->elements = result.indexedElements;
        const auto scale = state->window.coordinateScale;
        if (std::isfinite(scale) && scale > 0) {
            for (const auto& element : result.indexedElements)
                result.elementBounds.push_back({(element.x - state->window.x) / scale,
                                                (element.y - state->window.y) / scale, element.width / scale,
                                                element.height / scale});
        }
    }
    if (includeScreenshot) {
        const auto scale = state->window.coordinateScale;
        if (!std::isfinite(scale) || scale <= 0)
            return {MaiErrorCode::NotSupported, "native window coordinate scale is unavailable"};
        result.screenshots.push_back({state->screenshotId, observed.value().imagePath, state->window.width / scale,
                                      state->window.height / scale, state->window.x / scale, state->window.y / scale, 0,
                                      state->screenshotUrl, facts.value("image_width", 0),
                                      facts.value("image_height", 0)});
    }
    return result;
}

MaiResult<DesktopComputerActionResult> DesktopComputerController::performSky(const std::string& sessionId,
                                                                             const DesktopComputerSkyAction& request,
                                                                             DesktopComputerActionProgress* progress) {
    if (progress) *progress = {};
    bool completed = false;
    const auto cleanup = qScopeGuard([&] {
        if (!completed) releaseInput(sessionId, request.turnId);
    });
    if (request.isCanceled && request.isCanceled()) {
        releaseInput(sessionId, request.turnId);
        return {MaiErrorCode::Canceled, "computer operation canceled"};
    }
    if (sessionId.empty() || request.window.app.empty())
        return {MaiErrorCode::InvalidInput, "session and returned window identity are required"};
    const auto selected = getWindow(request.window, request.isCanceled);
    if (!selected) return selected.error();
    const auto state = impl_->get(sessionId);
    if (!state) return {MaiErrorCode::NotFound, "get_window_state is required before input"};
    DesktopComputerAction action;
    action.turnId = request.turnId;
    action.isCanceled = [this, state, sessionId, callback = request.isCanceled] {
        return state->invalidated.load() || impl_->get(sessionId) != state || (callback && callback());
    };
    action.windowId = request.window.id;
    std::optional<DesktopNativeElement> element;
    DesktopNativeWindow nativeWindow;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->invalidated.load() || state->token.empty() || state->desktopScope ||
            state->window.id != request.window.id || state->canonicalApp != request.window.app ||
            std::chrono::steady_clock::now() - state->observedAt > std::chrono::minutes(5))
            return {MaiErrorCode::InvalidInput, "window state expired; observe again"};
        if (!request.screenshotId.empty() && request.screenshotId != state->screenshotId)
            return {MaiErrorCode::InvalidInput, "screenshotId is not from the current window state"};
        if (!state->screenshotId.empty() && !state->screenshotCoordinatesUsable && !request.elementIndex &&
            (request.x || request.y))
            return {MaiErrorCode::NotSupported,
                    "window image/native coordinate alignment is unverified; use native element observations"};
        nativeWindow = state->window;
        action.snapshotId = state->token;
        if (request.elementIndex) {
            if (*request.elementIndex >= state->indexedElementIds.size())
                return {MaiErrorCode::InvalidInput, "element_index is not in the current tree"};
            action.elementId = state->indexedElementIds[*request.elementIndex];
            element = state->elements.at(action.elementId);
        }
    }
    const auto position = [&](double x, double y, double& rx, double& ry) -> MaiError {
        const double scale = nativeWindow.coordinateScale;
        if (!std::isfinite(scale) || scale <= 0 || !std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 ||
            x * scale >= nativeWindow.width || y * scale >= nativeWindow.height)
            return {MaiErrorCode::InvalidInput, "window-relative coordinates are outside current bounds"};
        rx = x * scale / (std::max)(1.0, nativeWindow.width - 1);
        ry = y * scale / (std::max)(1.0, nativeWindow.height - 1);
        return {};
    };
    if (request.method == "set_value" || request.method == "perform_secondary_action") {
        if (!element) return {MaiErrorCode::InvalidInput, "element_index is required"};
        std::lock_guard<std::mutex> operation(desktopOperationMutex);
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->invalidated.load() || impl_->get(sessionId) != state || state->token != action.snapshotId)
            return {MaiErrorCode::InvalidInput, "window state was replaced; observe again"};
        const auto windows = impl_->backend->windows();
        if (!windows) return windows.error();
        if (std::count_if(windows.value().begin(), windows.value().end(),
                          [&](const auto& now) { return currentWindowMatches(nativeWindow, now); }) != 1)
            return {MaiErrorCode::InvalidInput, "element window changed; observe again"};
        const auto canceled = [&] {
            return state->invalidated.load() || impl_->get(sessionId) != state ||
                   (request.isCanceled && request.isCanceled());
        };
        const auto resolved = impl_->backend->resolveElement(nativeWindow, *element, canceled);
        if (!resolved) return resolved.error();
        if (!elementMatches(*element, resolved.value()))
            return {MaiErrorCode::InvalidInput, "element changed; observe again"};
        if (canceled()) return {MaiErrorCode::Canceled, "element observation was replaced or canceled"};
        const auto owner = impl_->inputOwnership->prepare(sessionId, action, false);
        if (!owner) return owner.error();
        state->token.clear();
        DesktopInputBatch batch;
        if (progress) {
            progress->windowOperationAttempted = true;
            progress->needsObservation = true;
        }
        const auto activated = impl_->backend->activateForInput(nativeWindow, canceled);
        if (activated) return activated;
        const auto focused = impl_->backend->inputFocused(nativeWindow);
        if (!focused) return focused.error();
        if (!focused.value()) return {MaiErrorCode::Busy, "target window did not become focused"};
        if (canceled()) return {MaiErrorCode::Canceled, "element operation canceled"};
        if (progress) progress->inputDispatchAttempted = true;
        const auto changed =
            request.method == "set_value"
                ? impl_->backend->setElementValue(nativeWindow, *element, request.text, canceled)
                : impl_->backend->performSecondaryAction(nativeWindow, *element, request.secondaryAction, canceled);
        if (changed) return changed;
        if (canceled()) return {MaiErrorCode::Canceled, "element operation canceled after dispatch"};
        DesktopComputerActionResult result;
        result.state = "dispatched";
        result.dispatched = true;
        completed = true;
        return result;
    }
    if (request.method == "click") {
        if (request.button != DesktopMouseButton::Left && request.button != DesktopMouseButton::Middle &&
            request.button != DesktopMouseButton::Right)
            return {MaiErrorCode::InvalidInput, "Sky click supports left, middle and right buttons"};
        if (request.elementIndex && (request.x || request.y))
            return {MaiErrorCode::InvalidInput, "use element_index or x/y, never both"};
        // 一个定位事件加成对按下/释放不得超过普通执行器的 256 事件上限。
        if (request.clickCount < 1 || request.clickCount > 127)
            return {MaiErrorCode::InvalidInput, "click_count must be 1..127"};
        action.button = request.button;
        action.action = request.clickCount == 2 ? "double_click" : "click";
        if (!request.elementIndex) {
            if (!request.x || !request.y) return {MaiErrorCode::InvalidInput, "click requires x and y"};
            const auto error = position(*request.x, *request.y, action.xRatio, action.yRatio);
            if (error) return error;
        }
        if (request.clickCount > 2) {
            if (element) {
                const double scale = nativeWindow.coordinateScale;
                const auto error =
                    position((element->x + element->width / 2 - nativeWindow.x) / scale,
                             (element->y + element->height / 2 - nativeWindow.y) / scale, action.xRatio, action.yRatio);
                if (error) return error;
            }
            action.action = "input_events";
            DesktopInputEvent move;
            move.kind = DesktopInputEventKind::AbsoluteMove;
            move.x = action.xRatio;
            move.y = action.yRatio;
            action.events.push_back(move);
            DesktopInputEvent down;
            down.kind = DesktopInputEventKind::MouseButton;
            down.button = request.button == DesktopMouseButton::Left     ? 1
                          : request.button == DesktopMouseButton::Middle ? 2
                                                                         : 3;
            auto up = down;
            up.release = true;
            for (int count = 0; count < request.clickCount; ++count) {
                action.events.push_back(down);
                action.events.push_back(up);
            }
        }
    } else if (request.method == "press_key") {
        action.action = "key";
        action.key = request.key;
    } else if (request.method == "type_text") {
        if (request.text.empty()) {
            DesktopComputerActionResult result;
            result.state = "unchanged";
            completed = true;
            return result;
        }
        action.action = "type";
        action.text = request.text;
    } else if (request.method == "drag" || request.method == "scroll") {
        if (!request.x || !request.y) return {MaiErrorCode::InvalidInput, "operation requires x and y"};
        const auto error = position(*request.x, *request.y, action.xRatio, action.yRatio);
        if (error) return error;
        if (request.method == "drag") {
            action.action = "drag";
            const auto destination = position(request.toX, request.toY, action.toXRatio, action.toYRatio);
            if (destination) return destination;
        } else {
            if (!std::isfinite(request.scrollX) || !std::isfinite(request.scrollY) ||
                std::abs(request.scrollX) > 32767 || std::abs(request.scrollY) > 32767)
                return {MaiErrorCode::InvalidInput, "scroll deltas must be finite and within 32767"};
            action.action = "input_events";
            DesktopInputEvent move;
            move.kind = DesktopInputEventKind::AbsoluteMove;
            move.x = action.xRatio;
            move.y = action.yRatio;
            action.events.push_back(move);
            for (const bool horizontal : {true, false}) {
                const auto delta = horizontal ? request.scrollX : -request.scrollY;
                if (std::round(delta) == 0) continue;
                DesktopInputEvent wheel;
                wheel.kind = DesktopInputEventKind::Wheel;
                wheel.horizontal = horizontal;
                wheel.wheelDelta120 = static_cast<std::int32_t>(std::round(delta));
                action.events.push_back(wheel);
            }
        }
    } else {
        return {MaiErrorCode::InvalidInput, "unsupported Sky input method"};
    }
    const auto result = perform(sessionId, action, progress);
    completed = bool(result);
    return result;
}
