#include "MaiChatTools/DesktopComputerTools.h"
#include "MaiChatTools/DesktopComputerBackend.h"
#include "MaiChatTools/DesktopComputerKeys.h"
#include "MaiChatTools/DesktopComputerInputProtocol.h"
#include "MaiChatTools/DesktopComputerInputSession.h"

#include <QDir>
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
    return !before.minimized && !before.cloaked && before.boundsAvailable &&
           !now.minimized && !now.cloaked && now.boundsAvailable &&
           windowIdentityMatches(before, now) &&
           std::abs(before.x - now.x) < 3 && std::abs(before.y - now.y) < 3 && std::abs(before.width - now.width) < 3 &&
           std::abs(before.height - now.height) < 3;
}

bool observationWindowMatches(const DesktopNativeWindow& before, const DesktopNativeWindow& now) {
    if (before.minimized != now.minimized || before.cloaked != now.cloaked ||
        before.boundsAvailable != now.boundsAvailable || !windowIdentityMatches(before, now)) return false;
    return (before.minimized || before.cloaked) ? true : currentWindowMatches(before, now);
}

bool executionWindowMatches(const DesktopNativeWindow& before, const DesktopNativeWindow& now) {
    // A screenshot's receiver is an exact spatial restriction. Even a small
    // move requires fresh evidence; the legacy window tolerance is unchanged.
    return currentWindowMatches(before, now) && before.x == now.x && before.y == now.y &&
           before.width == now.width && before.height == now.height;
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
    Json details{{"reason", "point_outside_target"}, {"execution_window_id", std::to_string(window.id)},
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
    MaiResult<DesktopNativeAccessibilitySnapshot> inspectElements(
        const DesktopNativeWindow& window, const std::function<bool()>& isCanceled) override {
        return maiDesktopInspectElements(window, isCanceled);
    }
    MaiResult<DesktopNativeElement> resolveElement(
        const DesktopNativeWindow& window, const DesktopNativeElement& observedElement,
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
           before.automationId == now.automationId && before.enabled == now.enabled && before.offscreen == now.offscreen &&
           now.enabled && !now.offscreen && validElementBounds(now) && before.x == now.x && before.y == now.y &&
           before.width == now.width && before.height == now.height;
}

void unavailableAccessibility(Json& output, const std::string& reason, const std::string& detail = {}) {
    output["elements"] = Json::array();
    output["accessibility"] = {{"provider", "backend"}, {"status", "unavailable"}, {"visited_nodes", 0},
                               {"returned_elements", 0}, {"counts_known", false}, {"truncated", false},
                               {"limit_reason", reason}};
    if (!detail.empty()) output["element_error"] = detail;
}

void publishElements(Json& output, ObservedWindow& state, const DesktopNativeAccessibilitySnapshot& snapshot,
                     double x, double y, double width, double height) {
    Json items = Json::array();
    for (const auto& item : snapshot.elements) {
        if (!validElementBounds(item)) continue;
        const double centerX = (item.x + item.width / 2 - x) / width;
        const double centerY = (item.y + item.height / 2 - y) / height;
        if (!ratio(centerX) || !ratio(centerY)) continue;
        const QString label = QString::fromUtf8(item.label.c_str(), int(item.label.size())).left(160);
        Json element{{"role", item.role}, {"label", label.toUtf8().toStdString()},
                     {"enabled", item.enabled}, {"offscreen", item.offscreen},
                     {"center_x_ratio", centerX}, {"center_y_ratio", centerY},
                     {"bounds", {{"x_ratio", (item.x - x) / width}, {"y_ratio", (item.y - y) / height},
                                  {"width_ratio", item.width / width}, {"height_ratio", item.height / height}}}};
        if (!item.automationId.empty()) element["automation_id"] = item.automationId;
        if (!item.nativeId.empty() || item.reference) {
            const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
            state.elements.emplace(id, item);
            element["element_id"] = id;
        }
        items.push_back(std::move(element));
    }
    output["elements"] = std::move(items);
    output["accessibility"] = {{"provider", snapshot.provider}, {"status", snapshot.status},
                               {"visited_nodes", snapshot.visitedNodes}, {"returned_elements", output["elements"].size()},
                               {"provider_elements", snapshot.elements.size()},
                               {"counts_known", snapshot.countsKnown}, {"truncated", snapshot.truncated},
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
    return {{"buttons_down", state.buttonsDown},
            {"keys_down", state.keysDown},
            {"raw_keys_down", rawKeys},
            {"touch_pointers", state.touchPointers},
            {"pen_active", state.penActive},
            {"devices", state.devices},
            {"controllers", controllers},
            {"scope", "owned_synthetic_inputs"},
            {"idle_release_ms", 120000},
            {"idle_remaining_ms", state.idleRemainingMs}};
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

MaiResult<DesktopNativeElement> DesktopComputerBackend::resolveElement(
    const DesktopNativeWindow& window, const DesktopNativeElement& observedElement,
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
            if (executionWindow)
                return {MaiErrorCode::InvalidInput, "execution window is ambiguous; observe again"};
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
        if (std::count_if(windows.value().begin(), windows.value().end(), [&](const auto& window) {
                return executionWindowMatches(*executionWindow, window);
            }) != 1)
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
    output["input_coordinate_space"] = {{"space", "display"}, {"space_id", std::to_string(selected.id)},
                                         {"unit", "ratio"}};
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
            unavailableAccessibility(output, "source_missing", sources ? "selected element source is missing or ambiguous"
                                                                         : sources.error().message());
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
                    if (!currentSources || std::count_if(currentSources.value().begin(), currentSources.value().end(),
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
        receiverChanged = !windows || std::count_if(windows.value().begin(), windows.value().end(),
            [&](const auto& now) { return executionWindowMatches(*executionWindow, now); }) != 1;
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
        return {MaiErrorCode::Busy, "minimized or cloaked windows support metadata observation only; no screenshot is available"};
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
    output["native_screen_bounds_unit"] = nativeScreenUnit();
    if (includeElements) {
        if (metadataOnly)
            unavailableAccessibility(output, "window_has_no_input_geometry");
        else {
            state->elementWindow = selected;
            const auto elements = impl_->backend->inspectElements(selected, isCanceled);
            if (elements)
                publishElements(output, *state, elements.value(), selected.x, selected.y, selected.width, selected.height);
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
    output["input_coordinate_space"] = {{"space", "window"}, {"space_id", std::to_string(selected.id)},
                                         {"unit", "ratio"}};
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
                if (progress) { progress->inputDispatchAttempted = true; progress->needsObservation = true; }
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
            if (progress) { progress->inputDispatchAttempted = true; progress->needsObservation = true; }
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
    const bool elementClick = action == "click" || action == "double_click" || action == "right_click";
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
    if (action != "focus" && !windowState && !eventBatch && !positioned && !writesText && action != "key" && action != "key_down" &&
        action != "verify")
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
        if (!after || std::count_if(after.value().begin(), after.value().end(),
            [&](const auto& now) { return currentWindowMatches(*state->elementWindow, now); }) != 1)
            return {MaiErrorCode::InvalidInput, "element source window changed while resolving; observe again"};
        const auto& target = state->desktopScope ? state->display : DesktopNativeDisplay{
            state->window.id, state->window.x, state->window.y, state->window.width, state->window.height};
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
                return executionPointError(receiver,
                    state->display.x + x * (std::max)(1.0, state->display.width - 1),
                    state->display.y + y * (std::max)(1.0, state->display.height - 1));
            };
            const bool positioned = action == "move" || action == "click" || action == "double_click" ||
                action == "right_click" || action == "drag" || action == "scroll" || action == "mouse_down";
            if (positioned) {
                const auto error = elementPoint ? executionPointError(receiver, elementPoint->first, elementPoint->second)
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
                            contact.phase == DesktopInputPhase::CancelAll) continue;
                        if (const auto error = point(contact.x, contact.y)) return error;
                    }
                } else if (event.kind == DesktopInputEventKind::Pen &&
                           event.pen.phase != DesktopInputPhase::Up && event.pen.phase != DesktopInputPhase::Cancel &&
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
        if (elementPoint) { input.x = elementPoint->first; input.y = elementPoint->second; }
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
        if (progress) { progress->inputDispatchAttempted = true; progress->needsObservation = true; }
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
            if (inputCanceled())
                return {MaiErrorCode::Canceled, "execution observation was revoked after input"};
            if (state->executionWindow) {
                const auto windows = impl_->backend->windows();
                if (!windows) return windows.error();
                if (std::count_if(windows.value().begin(), windows.value().end(), [&](const auto& now) {
                        return executionIdentityMatches(*state->executionWindow, now);
                    }) != 1)
                    return {MaiErrorCode::InvalidInput,
                            "execution window identity changed after input; observe explicitly before further input"};
            }
            auto observed = observeDisplay(sessionId, state->workspace, state->display.id, inputCanceled,
                                          state->frameSequence, MaiTime::getCurrentTime(), 0, request.turnId,
                                          state->elementWindowId, state->executionWindow ? state->executionWindow->id : 0);
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
        if (progress) { progress->windowOperationAttempted = true; progress->needsObservation = true; }
        DesktopInputBatch batch;
        const auto changed = impl_->backend->setWindowState(currentWindow, action == "minimize" ?
            DesktopWindowStateAction::Minimize : DesktopWindowStateAction::Restore, inputCanceled);
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
    if (progress) { progress->windowOperationAttempted = true; progress->needsObservation = true; }
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
    if (elementPoint) { input.x = elementPoint->first; input.y = elementPoint->second; }
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
    }
    if (progress) progress->inputDispatchAttempted = true;
    const MaiError sent = eventBatch ? native->inputEvents(request.events, input, port, request.inputOptions)
                                    : native->input(input);
    if (sent) return inputDispatchFailure(sent);
    if (canceled())
        return inputDispatchFailure({MaiErrorCode::Canceled, "computer operation canceled after input dispatch"});
    receipt.state = "dispatched";
    receipt.dispatched = true;
    if (eventBatch) receipt.acceptedEvents = request.events.size();
    receipt.inputState = inputState(sessionId);
    return success(std::move(receipt));
}

MaiError DesktopComputerController::clear(const std::string& sessionId) {
    const bool hadObservation = bool(impl_->get(sessionId));
    const auto released = releaseInput(sessionId);
    if (released) return released;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->sessions.erase(sessionId) == 0 && !hadObservation)
        return {MaiErrorCode::NotFound, "this Session has no selected window"};
    return {};
}

namespace {

class DesktopComputerTool final : public MaiTool {
public:
    explicit DesktopComputerTool(std::shared_ptr<DesktopComputerController> controller)
        : controller_(std::move(controller)) {}
    std::string name() const override {
        return "desktop_computer";
    }
    MaiError onTurnFinished(const std::string& session, const std::string& turn) override {
        return controller_->releaseInput(session, turn);
    }
    std::string description() const override {
        // Keep image scope, native bounds and token lifetime together: the model
        // must not infer window coordinates from an unrelated whole-screen image.
        return "Operate the desktop through native mouse, keyboard, touch and pen input. Choose operations and "
               "observation timing from the task and current state; no fixed click/type sequence is required.\n"
               "CAPABILITIES: list discovers windows, list_displays discovers displays, and list_keys "
               "lists named keys. Discovery can include minimized/cloaked windows and unavailable bounds; "
               "minimized/cloaked windows can be observed as metadata, but need available bounds for ordinary input. "
               "Use window_id to select an untitled window. target=display observe provides a whole-screen image and "
               "native "
               "bounds; target=window observe provides window bounds and Accessibility/UIA elements; "
               "include_image=true also captures that window. "
#if defined(Q_OS_WIN)
               "Prefer window images for one-window visual work. "
#else
               "Window images are for reading; their native-frame alignment is unverified. Do not derive input "
               "positions from them: use display observe with execution_window_id or native element_id. "
#endif
               "Specify target explicitly on observe/input; omitted target means window. Click, "
               "double_click and drag accept a mouse button; right_click is a fixed right-button alias. "
               "move positions the pointer, scroll supports either axis, key taps a key/chord, and type "
               "inserts Unicode text. These actions may be selected independently as needed.\n"
               "RECEIVER: display observe accepts execution_window_id to retain display coordinates while "
               "restricting every new input to that observed window. The restriction persists in successor "
               "snapshots; actions cannot override it. Its window must already be focused (including owned "
               "popups); no implicit focus is performed. Mouse/touch/pen points must be inside its main bounds "
               "and hit that window family. Select an outside popup explicitly before pointing outside those "
               "bounds. Changed identity/geometry requires a new observation; foreign focus rejects input. "
               "Restricted native relative motion is unsupported; use absolute motion. Omit the receiver only "
               "for intentional whole-display/cross-application work.\n"
               "WINDOW STATE: minimize/restore require target=window and its current snapshot_id. Results include "
               "before_window, after_window and actual focused when readable; observe again afterwards. Cloaked "
               "windows are never moved across virtual desktops. ELEMENTS: click/double_click/right_click can use "
               "element_id from the same current snapshot OR x_ratio and y_ratio, exclusively. Native identities "
               "are re-resolved before input; changed/disabled/offscreen elements require a new observation. For "
               "a popup or taskbar, observe target=display with element_window_id selecting that one discovered "
               "window as accessibility source; input remains display-scoped and does not force-focus the source. "
               "Accessibility status/counts describe bounded provider coverage. Native bounds use labeled Windows "
               "screen_physical_pixels or macOS screen_points; model ratios keep their existing meaning. "
               "Failures report input_dispatch_attempted/window_operation_attempted, owned input_state and "
               "needs_observation; attempted input may be partial, so inspect state before retrying.\n"
               "PRIMITIVE BATCHES: input_events sends an ordered typed event batch through the same "
               "target and owner: absolute/relative motion, stationary buttons, wheel in 120 units per "
               "tick, raw physical keys and Unicode. Touch and pen use separate system pointer types; "
               "check input_capabilities before using them. A schema type alone does not establish native "
               "availability or successful delivery. Virtual controllers, hardware-HID emulation and "
               "controller feedback are deferred and are not offered by the current native backend.\n"
               "COORDINATES: x_ratio/y_ratio and drag destination to_x_ratio/to_y_ratio are fractions "
               "0..1 of the selected target bounds, never pixels. Top-left is (0,0), bottom-right "
               "(1,1), center (0.5,0.5); x increases right, y down. For target=display, a point "
               "(px,py) in the inspected full-screen image of size (Iw,Ih) gives "
               "x_ratio=px/(Iw-1), y_ratio=py/(Ih-1). Use that actual image's dimensions, including "
               "when viewing a scaled preview; do not divide preview pixels by original dimensions "
               "or apply an extra DPI/zoom factor. A crop or browser viewport has a different origin "
               "and cannot supply screen ratios directly. Never mix monitors or image/token pairs.\n"
               "WINDOW COORDINATES AND FOCUS: window observations identify one exact window_id "
               "or window_title. Element center_x_ratio/center_y_ratio are already window-relative; "
               "use them directly. Other window coordinates refer to the full observed window bounds, "
               "not just web content. Window input activates/checks that window, without freezing "
               "its position. Display move does not focus; display key/type uses the current keyboard "
               "focus. Establish focus only when needed; a screenshot alone is not proof of focus.\n"
               "CROSS-SCOPE CONVERSION: never apply whole-display screenshot ratios directly to "
               "a window. Only with known matching display/window bounds, first compute u=px/(Iw-1), "
               "v=py/(Ih-1) from the actual full-display image. For native display bounds D and window "
               "bounds W, window x_ratio=(D.x+u*(D.width-1)-W.x)/(W.width-1), and "
               "y_ratio=(D.y+v*(D.height-1)-W.y)/(W.height-1). Native bounds can differ from image "
               "pixels and have negative origins. If bounds are unknown, changed, degenerate or the "
               "point is outside 0..1, re-observe rather than guess/clamp.\n"
               "OBSERVATION AND TOKENS: use a fresh snapshot bound to the same session, target "
               "and coordinate space. A mutating action consumes it. Display action results include "
               "a new image and observation.snapshot_id; use this observation when sufficient. "
               "Window actions require a renewed observation for further input. Observe when the "
               "target/token is unknown, expired or changed, or when more evidence is needed. "
               "A screenshot does not refresh a computer snapshot_id or bind an input target; "
               "standalone screenshot captures the display under the pointer, which may be another "
               "monitor. Do not substitute unrelated images for a bound visual observation. "
               "If input was dispatched but observation failed, inspect the current state before "
               "retrying the mutation. Display observe accepts wait_ms=0..5000 for paint/loading; "
               "a fresh frame alone does not prove application completion.\n"
               "INPUT STATE: mouse_down/mouse_up and key_down/key_up control independent holds; "
               "key_down/key_up accept a single physical key, not a chord. Held modifiers combine "
               "with pointer, wheel and key input; move preserves held mouse buttons. Capture and "
               "observe do not release inputs. Click/key taps and complete drag release only their "
               "own transient presses. drag moves to a supplied endpoint and releases; it does not "
               "check visual alignment. input_state reports this caller's buttons_down/keys_down. "
               "reset_inputs releases owned controls and stops repeat while retaining the native "
               "context and configuration. Reset is serialized "
               "with input and returns Busy without effects while another operation is in flight. release_all "
               "tears down this caller's input session. mouse_up, key_up, reset_inputs and release_all remain usable "
               "without a "
               "fresh snapshot or foreground focus, and do not adopt/release physical user holds. "
               "One session/task/target owns held input at a time. Release before changing target. "
               "Owned holds are released at task end/cancel, dispatch failure, clear/destruction or "
               "120 seconds of owner inactivity. Inspect state after interruption/expiry; do not "
               "assume an earlier hold remains active.\n"
               "KEYBOARD AND READBACK: Meta taps the Windows launcher; Cmd+Space can open Spotlight. "
               "list_keys returns the accepted shared keyboard names without observing or injecting input. "
               "Native platform support is checked before dispatch; an unavailable key is not replaced "
               "with a different key. key accepts named keys and chords such as Ctrl+A, Cmd+A, "
               "Ctrl+Shift, ControlRight+F4 or Ctrl+Plus. Named Equal/Minus refer to physical keys; "
               "literal punctuation resolves through the current layout. NumpadEnter is distinct "
               "from Enter. type enters Unicode text, including Chinese and emoji; use it for text "
               "instead of spelling characters as hardware keys. Literal type/replace_text requires no owned "
               "Control/Shift/Alt/Meta holds; use key for composed physical input. "
               "replace_text selects all in the focused editable control then types the replacement "
               "(an empty string deletes its text). Window-only verify reads the focused non-password "
               "control and compares expected_text; "
               "only verified=true confirms that readback, not application save/persistence. "
               "dry_run validates a planned action without activating or injecting input. "
               "Approval follows the Agent policy; OS focus, interactive-desktop and platform "
               "permissions still apply. If focus or permissions are refused, explain the condition "
               "and wait for user assistance instead of repeatedly retrying or inventing shell scripts. "
               "clear only forgets the selected window and snapshot and releases owned inputs; "
               "it does not clear input text. After clear, observe again.";
    }
    std::string parametersSchema() const override {
        return maiDesktopComputerInputSchema(R"({"type":"object","properties":{
          "action":{"type":"string","enum":["list","list_displays","list_keys","observe","focus","minimize","restore","move","click","double_click","right_click","drag","mouse_down","mouse_up","type","replace_text","key","key_down","key_up","scroll","input_state","reset_inputs","release_all","verify","clear"]},
          "button":{"type":"string","enum":["left","right","middle","back","forward"],"description":"Button for click/double_click/drag/mouse_down/mouse_up; default left. right_click always means right."},
          "axis":{"type":"string","enum":["vertical","horizontal"],"description":"Scroll axis, default vertical. Positive lines mean up vertically or right horizontally."},
          "target":{"type":"string","enum":["window","display"],"description":"Set explicitly on observe/input. display: selected whole screen and its bound image. window: selected window bounds/UIA and optional include_image. Omitted means window."},
          "display_id":{"type":"string","description":"Exact ID from list_displays; required for display observe/input. Keep the image, display_id and snapshot_id from the same observation."},
          "element_window_id":{"type":"string","description":"Display observe only: inspect this one discovered window for UIA/AX elements (including separate popup/taskbar). Accessibility source only; input stays scoped to display_id."},
          "execution_window_id":{"type":"string","description":"Display observe only: bind a visible discovered window as receiver for all subsequent inputs. Coordinates remain display-relative; main bounds, identity and foreground window family are checked. Actions cannot override or clear it."},
          "element_id":{"type":"string","minLength":1,"description":"Opaque ID from the current snapshot for click/double_click/right_click. Supply element_id OR both x_ratio/y_ratio, never both; changed native evidence requires observing again."},
          "window_title":{"type":"string","maxLength":500,"description":"Exact title for window observe; supply this OR window_id, not both."},
          "window_id":{"type":"string","description":"ID from list/window observe. Required for window input; coordinates refer to this window's full observed bounds."},
          "include_elements":{"type":"boolean","description":"Window observe only: include Accessibility/UIA elements. Their center_x_ratio/center_y_ratio can be used directly for window input."},
          "include_image":{"type":"boolean","description":"Window observe only: capture the selected window with actual image dimensions. Default false preserves metadata-only observation; requires an image-capable primary model."},
          "snapshot_id":{"type":"string","description":"Fresh token for this session and target. Observe returns snapshot_id; display input returns the next token at observation.snapshot_id. Mutations consume it; owned up/release actions need no token. Standalone screenshot never renews it."},
          "wait_ms":{"type":"integer","minimum":0,"maximum":5000,"description":"Display observe only: wait before acquiring a fresh frame, e.g. for application paint/loading."},
          "x_ratio":{"type":"number","minimum":0,"maximum":1,"description":"Horizontal fraction of selected display/window bounds: 0 left, 1 right. For the bound full-display image use px/(inspected image width-1), never raw pixels or browser-viewport ratios."},
          "y_ratio":{"type":"number","minimum":0,"maximum":1,"description":"Vertical fraction of selected display/window bounds: 0 top, 1 bottom. For the bound full-display image use py/(inspected image height-1). Whole-screen ratios are not window ratios."},
          "to_x_ratio":{"type":"number","minimum":0,"maximum":1,"description":"Drag destination x in the SAME target coordinate space as x_ratio; never a pixel offset."},
          "to_y_ratio":{"type":"number","minimum":0,"maximum":1,"description":"Drag destination y in the SAME target coordinate space as y_ratio; never a pixel offset."},
          "duration_ms":{"type":"integer","minimum":1,"maximum":5000},
          "text":{"type":"string","minLength":0,"maxLength":4000},
          "key":{"type":"string","minLength":1,"maxLength":128,"description":"key accepts a single key or modifier chord; key_down/key_up accept one physical key only. list_keys gives accepted names. Examples: Ctrl+A, Cmd+A, Ctrl+Shift, ControlRight+F4, Ctrl+Plus, NumpadEnter"},
          "lines":{"type":"integer","minimum":-10,"maximum":10},
          "expected_text":{"type":"string","maxLength":4000},"dry_run":{"type":"boolean"}
        },"required":["action"],"additionalProperties":false})");
    }
    bool requiresApproval(const std::string& raw) const override {
        return requiresPerCallApproval(raw);
    }
    bool requiresPerCallApproval(const std::string& raw) const override {
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object() || !args.value("action", Json{}).is_string()) return true;
        const auto action = args["action"].get<std::string>();
        if (action == "input_events") {
            const auto batch = maiDesktopParseInputBatch(
                args.value("events", Json{}).dump(),
                args.contains("input_options") ? args["input_options"].dump() : std::string());
            return !batch || !maiDesktopInputEventsOnlyRelease(batch.value().events);
        }
        // Cleanup cannot depend on a new approval dialog or a still-valid
        // observation; these operations only release this caller's own ledger.
        return action != "input_capabilities" && action != "feedback" && action != "clear" && action != "release_all" &&
               action != "reset_inputs" && action != "mouse_up" && action != "key_up" && action != "input_state";
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        Json args = Json::parse(raw, nullptr, false);
        bool releaseEvents = false;
        if (args.is_object() && args.value("action", Json{}) == "input_events") {
            const auto parsed = maiDesktopParseInputBatch(
                args.value("events", Json{}).dump(),
                args.contains("input_options") ? args["input_options"].dump() : std::string());
            releaseEvents = parsed && maiDesktopInputEventsOnlyRelease(parsed.value().events);
        }
        const bool cleanup =
            args.is_object() && args.value("action", Json{}).is_string() &&
            (args["action"] == "release_all" || args["action"] == "reset_inputs" || args["action"] == "mouse_up" ||
             args["action"] == "key_up" || args["action"] == "input_state" || releaseEvents);
        if (context.isCanceled()) {
            // Host Stop retires the turn independently of action dry-run. A
            // validation-only request cannot keep canceled synthetic holds.
            controller_->releaseInput(context.sessionId, context.turnId);
            if (!cleanup) return MaiToolResult::failure(MaiErrorCode::Canceled, "computer operation canceled");
        }
        if (context.sessionId.empty())
            return MaiToolResult::failure(MaiErrorCode::NotConfigured, "a local Desktop Session is required");
        if (!args.is_object() || !args.value("action", Json{}).is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "computer action is required");
        // Validate types before .value<T>() and before any backend access.
        // Workspace belongs to the host context; it is never a tool argument.
        for (auto field = args.begin(); field != args.end(); ++field) {
            const auto& name = field.key();
            const bool known = name == "action" || name == "target" || name == "window_title" ||
                name == "window_id" || name == "display_id" || name == "element_window_id" ||
                name == "execution_window_id" || name == "element_id" ||
                name == "include_elements" || name == "include_image" || name == "snapshot_id" || name == "wait_ms" ||
                name == "x_ratio" || name == "y_ratio" || name == "to_x_ratio" || name == "to_y_ratio" ||
                name == "duration_ms" || name == "text" || name == "key" || name == "lines" || name == "expected_text" ||
                name == "dry_run" || name == "button" || name == "axis" || name == "events" || name == "input_options";
            if (!known) return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                      "unexpected desktop_computer parameter: " + name);
            if ((name == "window_id" || name == "display_id" || name == "element_window_id" || name == "execution_window_id") &&
                !field.value().is_string())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, name + " must be a decimal string");
            if ((name == "target" || name == "window_title" || name == "snapshot_id" || name == "element_id" ||
                 name == "text" || name == "key" || name == "expected_text" || name == "button" || name == "axis") &&
                !field.value().is_string())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, name + " must be a string");
            if ((name == "include_elements" || name == "dry_run") && !field.value().is_boolean())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, name + " must be boolean");
            if ((name == "x_ratio" || name == "y_ratio" || name == "to_x_ratio" || name == "to_y_ratio") &&
                !field.value().is_number())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, name + " must be a number");
            if ((name == "lines" || name == "wait_ms" || name == "duration_ms") && !field.value().is_number_integer())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, name + " must be an integer");
        }
        if (args["action"] == "input_capabilities") {
            if (args.size() != 1)
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "input_capabilities has no other parameters");
            return MaiToolResult::success(maiDesktopInputCapabilitiesJson(controller_->capabilities()));
        }
        if (args["action"] == "feedback")
            return MaiToolResult::failure(MaiErrorCode::NotSupported,
                "virtual-controller feedback is deferred; ordinary input results use their action receipt");
        if (args["action"] == "input_events") return executeInputEvents(args, context);
        if (args["action"] == "input_state" || args["action"] == "release_all" || args["action"] == "reset_inputs") {
            if (args.size() != 1)
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "owned input state actions have no other parameters");
            if (args["action"] == "release_all") {
                const auto released =
                    controller_->releaseInput(context.sessionId, context.isCanceled() ? context.turnId : std::string());
                if (released) return failure(released);
            }
            if (args["action"] == "reset_inputs") {
                const auto reset = controller_->resetInput(context.sessionId, context.turnId);
                if (reset) return failure(reset);
            }
            auto result = inputStateJson(controller_->inputState(context.sessionId));
            if (args["action"] == "reset_inputs") result["state"] = "inputs_reset";
            return MaiToolResult::success(result.dump());
        }
        if (args["action"] == "mouse_up" || args["action"] == "key_up") {
            const bool mouse = args["action"] == "mouse_up";
            for (auto field = args.begin(); field != args.end(); ++field) {
                const auto& key = field.key();
                if (key != "action" && key != "target" && key != "window_id" && key != "display_id" &&
                    key != "snapshot_id" && key != "dry_run" && key != (mouse ? "button" : "key"))
                    return MaiToolResult::failure(MaiErrorCode::InvalidInput, "unexpected input-release parameter");
            }
            if (args.contains("target") &&
                (!args["target"].is_string() || (args["target"] != "display" && args["target"] != "window")))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "target must be display or window");
            if (args.contains("display_id") && args.contains("window_id"))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "release refers to at most one input target");
            DesktopComputerAction request;
            request.action = args["action"].get<std::string>();
            request.turnId = context.turnId;
            if (args.contains("dry_run") && !args["dry_run"].is_boolean())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "dry_run must be boolean");
            request.dryRun = args.value("dry_run", false);
            request.isCanceled = [&context] { return context.isCanceled(); };
            if (mouse) {
                const auto button = mouseButtonFrom(args.value("button", Json("left")));
                if (!button) return failure(button.error());
                request.button = button.value();
            } else {
                if (!args.value("key", Json{}).is_string())
                    return MaiToolResult::failure(MaiErrorCode::InvalidInput, "key_up needs one named key");
                request.key = args["key"].get<std::string>();
            }
            request.desktopScope = args.value("target", std::string("window")) == "display";
            for (const char* field : {"display_id", "window_id"})
                if (args.contains(field)) {
                    const auto id = windowIdFrom(args[field], field);
                    if (!id) return failure(id.error());
                    if (std::string(field) == "display_id")
                        request.displayId = id.value();
                    else
                        request.windowId = id.value();
                }
            const auto released = controller_->perform(context.sessionId, request);
            if (!released) return failure(released.error());
            Json output = inputStateJson(released.value().inputState);
            output["state"] = released.value().state;
            output["dry_run"] = released.value().dryRun;
            output["dispatched"] = released.value().dispatched;
            return MaiToolResult::success(output.dump());
        }
        if (args["action"] == "list_keys") {
            if (args.size() != 1)
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "list_keys has no other parameters");
            // This ordinary immutable directory has no dependency on a selected
            // session window, permissions, capture, focus or native input state.
            return MaiToolResult::success(Json{{"key_names", maiDesktopKeyNames()},
                                               {"key_action", "key"},
                                               {"text_action", "type"},
                                               {"native_support_checked_at_dispatch", true},
                                               {"note",
                                                "Names describe the shared grammar. Unsupported native mappings fail "
                                                "explicitly; use type for Unicode text."}}
                                              .dump());
        }
        if (args.contains("target")) {
            if (!args["target"].is_string() || (args["target"] != "display" && args["target"] != "window"))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "target must be display or window");
            if (args["target"] == "display") return executeDisplay(args, context);
            args.erase("target");
        }
        if (args["action"] == "list_displays") return executeDisplay(args, context);
        auto exact = [&args](std::initializer_list<const char*> allowed) {
            for (auto item = args.begin(); item != args.end(); ++item) {
                bool found = false;
                for (const char* field : allowed) found |= item.key() == field;
                if (!found) return false;
            }
            return true;
        };
        const std::string action = args["action"].get<std::string>();
        if (action == "list") {
            if (!exact({"action"}))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "list has no other parameters");
            const auto windows = controller_->windows();
            if (!windows) return failure(windows.error());
            Json items = Json::array();
            for (const auto& window : windows.value()) items.push_back(windowJson(window));
            Json output{{"windows", items}, {"count", items.size()},
                        {"next_step", "Observe the selected window_id before an action."}};
#if defined(Q_OS_MAC)
            output["platform"] = "macos";
            output["select_all_key"] = "Cmd+A";
#elif defined(Q_OS_WIN)
            output["platform"] = "windows";
            output["select_all_key"] = "Ctrl+A";
#else
            output["platform"] = "unsupported";
#endif
            return MaiToolResult::success(output.dump());
        }
        if (action == "observe") {
            for (auto field = args.begin(); field != args.end(); ++field)
                if (field.key() != "action" && field.key() != "window_title" && field.key() != "window_id" &&
                    field.key() != "include_elements" && field.key() != "include_image")
                    return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                  "unexpected window observe parameter: " + field.key());
            if ((args.contains("include_image") && !args["include_image"].is_boolean()) ||
                (args.contains("include_elements") && !args["include_elements"].is_boolean()))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "observe image/element flags must be boolean");
            const bool includeImage = args.value("include_image", false);
            const auto modelInfo = maiCloudModelInfo(context.model);
            if (includeImage && ((modelInfo && !modelInfo->supportsImages) ||
                                context.model.compare(0, 6, "local:") == 0))
                return MaiToolResult::failure(MaiErrorCode::NotSupported,
                                              "Window images require an image-capable primary model");
            if (args.contains("window_title") == args.contains("window_id"))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "window observe needs exactly one of window_title or window_id");
            if (context.root.empty())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "host workspace is missing; observe requires host context.root");
            std::uint64_t id = 0;
            if (args.contains("window_id")) {
                const auto parsed = windowIdFrom(args["window_id"]);
                if (!parsed) return failure(parsed.error());
                id = parsed.value();
            }
            const auto observed = controller_->observe(
                context.sessionId, args.value("window_title", std::string()), context.root,
                args.value("include_elements", true), id, includeImage, [&context] { return context.isCanceled(); },
                context.turnId);
            if (!observed) return failure(observed.error());
            if (includeImage)
                return MaiToolResult::successWithImages(observed.value().json,
                                                        {MaiToolImage{observed.value().imagePath, "image/png"}});
            return MaiToolResult::success(observed.value().json);
        }
        if (action == "clear") {
            if (!exact({"action"}))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "clear has no other parameters");
            const MaiError cleared = controller_->clear(context.sessionId);
            if (cleared) return failure(cleared);
            // Preserve the state field while making the cleared scope explicit to the Agent.
            return MaiToolResult::success(
                R"({"state":"cleared","scope":"window_observation",)"
                R"("text_changed":false,"next_step":"Call observe before any further window action."})");
        }
        if (!args.contains("snapshot_id"))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "snapshot_id is required for window actions");
        if (!args.value("snapshot_id", Json{}).is_string() || !args.value("window_id", Json{}).is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "action needs an observed snapshot and window ID");
        const auto id = windowIdFrom(args["window_id"]);
        if (!id) return failure(id.error());
        const bool positioned = action == "click" || action == "double_click" || action == "right_click" ||
                                action == "drag" || action == "scroll" || action == "move" || action == "mouse_down";
        const bool buttonAction =
            action == "click" || action == "double_click" || action == "drag" || action == "mouse_down";
        const bool writesText = action == "type" || action == "replace_text";
        const bool elementClick = action == "click" || action == "double_click" || action == "right_click";
        for (auto field = args.begin(); field != args.end(); ++field) {
            const std::string& name = field.key();
            const bool allowed =
                name == "action" || name == "snapshot_id" || name == "window_id" || name == "dry_run" ||
                (positioned && (name == "x_ratio" || name == "y_ratio")) ||
                (elementClick && name == "element_id") ||
                (action == "drag" && (name == "to_x_ratio" || name == "to_y_ratio" || name == "duration_ms")) ||
                (writesText && name == "text") || ((action == "key" || action == "key_down") && name == "key") ||
                (buttonAction && name == "button") || (action == "scroll" && (name == "lines" || name == "axis")) ||
                (action == "verify" && name == "expected_text");
            if (!allowed)
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "unexpected computer action parameter: " + name);
        }
        const auto locator = validateLocator(args, positioned, elementClick);
        if (locator) return failure(locator);
        if ((positioned && !args.contains("element_id") &&
             (!args.value("x_ratio", Json{}).is_number() || !args.value("y_ratio", Json{}).is_number())) ||
            (writesText && !args.value("text", Json{}).is_string()) ||
            ((action == "key" || action == "key_down") && !args.value("key", Json{}).is_string()) ||
            (action == "scroll" && !args.value("lines", Json{}).is_number_integer()) ||
            (action == "verify" && !args.value("expected_text", Json{}).is_string()) ||
            (action == "drag" &&
             (!args.value("to_x_ratio", Json{}).is_number() || !args.value("to_y_ratio", Json{}).is_number())) ||
            (args.contains("duration_ms") && !args["duration_ms"].is_number_integer()) ||
            (args.contains("dry_run") && !args["dry_run"].is_boolean()))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "computer action parameters are incomplete");
        DesktopComputerAction request;
        request.action = action;
        request.snapshotId = args["snapshot_id"].get<std::string>();
        request.elementId = args.value("element_id", std::string());
        request.windowId = id.value();
        request.xRatio = args.value("x_ratio", 0.0);
        request.yRatio = args.value("y_ratio", 0.0);
        request.toXRatio = args.value("to_x_ratio", 0.0);
        request.toYRatio = args.value("to_y_ratio", 0.0);
        request.text = args.value("text", std::string());
        request.key = args.value("key", std::string());
        request.expectedText = args.value("expected_text", std::string());
        if (args.contains("lines") && (args["lines"].get<double>() < -10 || args["lines"].get<double>() > 10))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "scroll lines must be bounded");
        if (args.contains("duration_ms") &&
            (args["duration_ms"].get<double>() < 1 || args["duration_ms"].get<double>() > 200))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "drag duration must be 1..200 milliseconds");
        request.scrollLines = args.value("lines", 0);
        request.durationMs = args.value("duration_ms", 200);
        request.turnId = context.turnId;
        if (buttonAction) {
            const auto button = mouseButtonFrom(args.value("button", Json("left")));
            if (!button) return failure(button.error());
            request.button = button.value();
        }
        if (action == "scroll") {
            const auto axis = scrollAxisFrom(args.value("axis", Json("vertical")));
            if (!axis) return failure(axis.error());
            request.scrollAxis = axis.value();
        }
        request.dryRun = args.value("dry_run", false);
        request.isCanceled = [&context] { return context.isCanceled(); };
        DesktopComputerActionProgress progress;
        const auto acted = controller_->perform(context.sessionId, request, &progress);
        if (!acted) return actionFailure(acted.error(), context.sessionId, progress);
        Json output{
            {"state", acted.value().state},
            {"action", action},
            {"window_id", std::to_string(request.windowId)},
            {"dispatched", acted.value().dispatched},
            {"verified", acted.value().verified},
            {"dry_run", acted.value().dryRun},
            {"next_step",
             request.dryRun
                 ? "No input was sent; the snapshot remains available."
                 : "Observe the same window again before the next action. Verify business persistence separately."}};
        output["input_state"] = inputStateJson(controller_->inputState(context.sessionId));
        if (acted.value().beforeWindow) output["before_window"] = windowJson(*acted.value().beforeWindow);
        if (acted.value().afterWindow) output["after_window"] = windowJson(*acted.value().afterWindow);
        if (acted.value().focused) output["focused"] = *acted.value().focused;
        if (action == "verify") {
            const QString actual =
                QString::fromUtf8(acted.value().actualText.data(), int(acted.value().actualText.size()));
            output["actual_text"] = actual.left(4000).toUtf8().toStdString();
            output["actual_text_truncated"] = actual.size() > 4000;
        }
        return MaiToolResult::success(output.dump());
    }

private:
    MaiError validateLocator(const Json& args, bool positioned, bool elementClick) const {
        if (!positioned) return {};
        const bool element = args.contains("element_id");
        const bool x = args.contains("x_ratio");
        const bool y = args.contains("y_ratio");
        if (element) {
            if (!elementClick || !args["element_id"].is_string() || args["element_id"].get<std::string>().empty())
                return {MaiErrorCode::InvalidInput, "element_id must be a nonempty ID for click, double_click or right_click"};
            if (x || y) return {MaiErrorCode::InvalidInput, "use element_id OR both x_ratio and y_ratio, exclusively"};
        } else if (!x || !y)
            return {MaiErrorCode::InvalidInput, "positioned input needs both x_ratio and y_ratio, or a supported element_id"};
        return {};
    }

    MaiToolResult actionFailure(const MaiError& error, const std::string& session,
                                const DesktopComputerActionProgress& progress) const {
        Json output{{"error", error.message()}, {"error_code", maiErrorCodeToString(error.code())},
                    {"input_dispatch_attempted", progress.inputDispatchAttempted},
                    {"window_operation_attempted", progress.windowOperationAttempted},
                    {"needs_observation", progress.needsObservation},
                    {"input_state", inputStateJson(controller_->inputState(session))}};
        const auto details = Json::parse(error.message(), nullptr, false);
        if (details.is_object() && details.value("reason", Json{}) == "point_outside_target") {
            for (const char* field : {"reason", "target_bounds", "requested_px", "requested_point", "execution_window_id",
                                      "native_screen_bounds_unit"})
                if (details.contains(field)) output[field] = details[field];
            output["status"] = "rejected";
        }
        return MaiToolResult::failure(error.code(), output.dump());
    }
    MaiToolResult executeInputEvents(const Json& args, const MaiToolContext& context) {
        for (auto field = args.begin(); field != args.end(); ++field)
            if (field.key() != "action" && field.key() != "target" && field.key() != "window_id" &&
                field.key() != "display_id" && field.key() != "snapshot_id" && field.key() != "events" &&
                field.key() != "input_options" && field.key() != "dry_run")
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "unexpected input_events parameter");
        if ((args.contains("target") &&
             (!args["target"].is_string() || (args["target"] != "display" && args["target"] != "window"))) ||
            (args.contains("dry_run") && !args["dry_run"].is_boolean()))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "input_events target/dry_run is invalid");
        const auto parsed =
            maiDesktopParseInputBatch(args.value("events", Json{}).dump(),
                                      args.contains("input_options") ? args["input_options"].dump() : std::string());
        if (!parsed) {
            if (!args.value("dry_run", false)) controller_->releaseInput(context.sessionId, context.turnId);
            return failure(parsed.error());
        }
        const bool releasing = maiDesktopInputEventsOnlyRelease(parsed.value().events);
        const bool display = args.value("target", std::string("window")) == "display";
        if (args.contains(display ? "window_id" : "display_id"))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "input_events cannot mix input targets");
        DesktopComputerAction request;
        request.action = "input_events";
        request.events = parsed.value().events;
        request.inputOptions = parsed.value().options;
        request.desktopScope = display;
        request.workspace = context.root;
        request.turnId = context.turnId;
        request.isCanceled = [&context] { return context.isCanceled(); };
        request.dryRun = args.value("dry_run", false);
        const char* targetField = display ? "display_id" : "window_id";
        if (!releasing || args.contains(targetField)) {
            const auto target = windowIdFrom(args.value(targetField, Json{}), targetField);
            if (!target) return failure(target.error());
            if (display)
                request.displayId = target.value();
            else
                request.windowId = target.value();
        }
        if (!releasing || args.contains("snapshot_id")) {
            if (!args.value("snapshot_id", Json{}).is_string())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "input_events needs a fresh snapshot_id");
            request.snapshotId = args["snapshot_id"].get<std::string>();
        }
        if (display && !releasing) {
            const auto modelInfo = maiCloudModelInfo(context.model);
            if ((modelInfo && !modelInfo->supportsImages) || context.model.compare(0, 6, "local:") == 0)
                return MaiToolResult::failure(MaiErrorCode::NotSupported,
                                              "Desktop screen operation requires an image-capable primary model");
        }
        DesktopComputerActionProgress progress;
        const auto result = controller_->perform(context.sessionId, request, &progress);
        if (!result) return actionFailure(result.error(), context.sessionId, progress);
        Json output{{"action", "input_events"},
                    {"state", result.value().state},
                    {"dispatched", result.value().dispatched},
                    {"dry_run", result.value().dryRun},
                    {"accepted_events", result.value().acceptedEvents},
                    {"input_state", inputStateJson(result.value().inputState)}};
        if (!result.value().observationError.empty()) {
            output["observation_error"] = result.value().observationError;
            output["next_step"] = "Input was dispatched. Observe again; do not repeat the batch blindly.";
        }
        if (!result.value().observation.json.empty()) {
            output["observation"] = Json::parse(result.value().observation.json);
            output["next_step"] = "Inspect the screen; use observation.snapshot_id for the next input.";
            return MaiToolResult::successWithImages(output.dump(),
                                                    {MaiToolImage{result.value().observation.imagePath, "image/png"}});
        }
        if (!releasing)
            output["next_step"] = request.dryRun ? "No input was sent; the snapshot remains available."
                                                 : "Observe again before the next input; verify task results.";
        return MaiToolResult::success(output.dump());
    }

    MaiToolResult executeDisplay(const Json& args, const MaiToolContext& context) {
        const std::string action = args["action"].get<std::string>();
        if (action == "list_displays") {
            if (args.size() != 1)
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "list_displays has no other parameters");
            const auto listed = controller_->displays();
            if (!listed) return failure(listed.error());
            Json items = Json::array();
            for (const auto& display : listed.value()) items.push_back(displayJson(display));
            return MaiToolResult::success(Json{
                {"displays", items}, {"next_step", "Observe target=display with one display_id before desktop input."}}
                                              .dump());
        }
        // Use the same current image boundary as the resident screenshot tool.
        // A text-only model must not inject against pixels it cannot observe.
        const auto modelInfo = maiCloudModelInfo(context.model);
        if ((modelInfo && !modelInfo->supportsImages) || context.model.compare(0, 6, "local:") == 0)
            return MaiToolResult::failure(
                MaiErrorCode::NotSupported,
                "Desktop screen operation requires an image-capable primary model; window metadata remains available.");
        const auto parsedId = windowIdFrom(args.value("display_id", Json{}), "display_id");
        if (!parsedId) return failure(parsedId.error());
        const bool observing = action == "observe";
        const bool positioned = action == "move" || action == "click" || action == "double_click" ||
                                action == "right_click" || action == "drag" || action == "scroll" || action == "move" ||
                                action == "mouse_down";
        const bool buttonAction =
            action == "click" || action == "double_click" || action == "drag" || action == "mouse_down";
        const bool writesText = action == "type" || action == "replace_text";
        const bool elementClick = action == "click" || action == "double_click" || action == "right_click";
        if (!observing && !positioned && !writesText && action != "key" && action != "key_down")
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "unsupported desktop action");
        for (auto field = args.begin(); field != args.end(); ++field) {
            const std::string& name = field.key();
            const bool allowed =
                name == "action" || name == "target" || name == "display_id" ||
                (observing && (name == "wait_ms" || name == "element_window_id" || name == "execution_window_id")) ||
                (!observing && (name == "snapshot_id" || name == "dry_run")) ||
                (positioned && (name == "x_ratio" || name == "y_ratio")) ||
                (elementClick && name == "element_id") ||
                (action == "drag" && (name == "to_x_ratio" || name == "to_y_ratio" || name == "duration_ms")) ||
                (writesText && name == "text") || ((action == "key" || action == "key_down") && name == "key") ||
                (buttonAction && name == "button") || (action == "scroll" && (name == "lines" || name == "axis"));
            if (!allowed)
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "unexpected desktop action parameter: " + name);
        }
        if (observing) {
            if (context.root.empty()) return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                "host workspace is missing; display observe requires host context.root");
            std::uint64_t sourceId = 0;
            std::uint64_t executionId = 0;
            if (args.contains("element_window_id")) {
                const auto source = windowIdFrom(args["element_window_id"], "element_window_id");
                if (!source) return failure(source.error());
                sourceId = source.value();
            }
            if (args.contains("execution_window_id")) {
                const auto receiver = windowIdFrom(args["execution_window_id"], "execution_window_id");
                if (!receiver) return failure(receiver.error());
                executionId = receiver.value();
            }
            if (args.contains("wait_ms") && (!args["wait_ms"].is_number_integer() ||
                                             args["wait_ms"].get<double>() < 0 || args["wait_ms"].get<double>() > 5000))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "display observation wait must be 0..5000ms");
            const auto observed = controller_->observeDisplay(
                context.sessionId, context.root, parsedId.value(), [&context] { return context.isCanceled(); }, 0, 0,
                args.value("wait_ms", 0), context.turnId, sourceId, executionId);
            if (!observed) return failure(observed.error());
            return MaiToolResult::successWithImages(observed.value().json,
                                                    {MaiToolImage{observed.value().imagePath, "image/png"}});
        }
        const auto locator = validateLocator(args, positioned, elementClick);
        if (locator) return failure(locator);
        if (!args.contains("snapshot_id"))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "snapshot_id is required for display actions");
        if (!args.value("snapshot_id", Json{}).is_string() ||
            (positioned && !args.contains("element_id") &&
             (!args.value("x_ratio", Json{}).is_number() || !args.value("y_ratio", Json{}).is_number())) ||
            (writesText && !args.value("text", Json{}).is_string()) ||
            ((action == "key" || action == "key_down") && !args.value("key", Json{}).is_string()) ||
            (action == "scroll" && !args.value("lines", Json{}).is_number_integer()) ||
            (action == "drag" &&
             (!args.value("to_x_ratio", Json{}).is_number() || !args.value("to_y_ratio", Json{}).is_number())) ||
            (args.contains("duration_ms") && !args["duration_ms"].is_number_integer()) ||
            (args.contains("dry_run") && !args["dry_run"].is_boolean()))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "desktop action parameters are incomplete");
        if (args.contains("lines") && (args["lines"].get<double>() < -10 || args["lines"].get<double>() > 10))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "scroll lines must be bounded");
        if (args.contains("duration_ms") &&
            (args["duration_ms"].get<double>() < 1 || args["duration_ms"].get<double>() > 5000))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "desktop drag duration must be 1..5000ms");
        DesktopComputerAction request;
        request.desktopScope = true;
        request.workspace = context.root;
        request.displayId = parsedId.value();
        request.action = action;
        request.snapshotId = args["snapshot_id"].get<std::string>();
        request.elementId = args.value("element_id", std::string());
        request.xRatio = args.value("x_ratio", 0.0);
        request.yRatio = args.value("y_ratio", 0.0);
        request.toXRatio = args.value("to_x_ratio", 0.0);
        request.toYRatio = args.value("to_y_ratio", 0.0);
        request.text = args.value("text", std::string());
        request.key = args.value("key", std::string());
        request.scrollLines = args.value("lines", 0);
        request.durationMs = args.value("duration_ms", 200);
        request.turnId = context.turnId;
        if (buttonAction) {
            const auto button = mouseButtonFrom(args.value("button", Json("left")));
            if (!button) return failure(button.error());
            request.button = button.value();
        }
        if (action == "scroll") {
            const auto axis = scrollAxisFrom(args.value("axis", Json("vertical")));
            if (!axis) return failure(axis.error());
            request.scrollAxis = axis.value();
        }
        request.dryRun = args.value("dry_run", false);
        request.isCanceled = [&context] { return context.isCanceled(); };
        DesktopComputerActionProgress progress;
        const auto result = controller_->perform(context.sessionId, request, &progress);
        if (!result) return actionFailure(result.error(), context.sessionId, progress);
        Json output{{"target", "display"},
                    {"display_id", std::to_string(request.displayId)},
                    {"action", action},
                    {"state", result.value().state},
                    {"dispatched", result.value().dispatched},
                    {"verified", result.value().verified},
                    {"dry_run", result.value().dryRun}};
        output["input_state"] = inputStateJson(controller_->inputState(context.sessionId));
        if (!result.value().observationError.empty()) {
            output["observation_error"] = result.value().observationError;
            output["next_step"] = "Input was dispatched. Observe again; do not blindly repeat the input.";
        }
        if (!result.value().observation.json.empty()) {
            output["observation"] = Json::parse(result.value().observation.json);
            output["next_step"] =
                "Inspect the returned screen; use observation.snapshot_id for the next action. Verify task results.";
            return MaiToolResult::successWithImages(output.dump(),
                                                    {MaiToolImage{result.value().observation.imagePath, "image/png"}});
        }
        return MaiToolResult::success(output.dump());
    }

    std::shared_ptr<DesktopComputerController> controller_;
};

}  // namespace

void registerDesktopComputerTools(MaiToolRegistry& registry, std::shared_ptr<DesktopComputerController> controller) {
    registry.add(std::make_unique<DesktopComputerTool>(std::move(controller)));
}
