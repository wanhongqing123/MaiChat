#include "MaiChatTools/DesktopComputerTools.h"
#include "MaiChatTools/DesktopComputerBackend.h"
#include "MaiChatTools/DesktopComputerKeys.h"

#include <QDir>
#include <QSaveFile>
#include <QString>
#include <QUuid>

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <mutex>

#include <json.hpp>

#include "MaiChatTools/DesktopComputerPlatform.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiPathGuard.h"
#include "MaiScreenshot.h"
#include "MaiTool.h"

namespace {

using Json = nlohmann::json;

struct ObservedWindow {
    DesktopNativeWindow window;
    std::string token;
    std::chrono::steady_clock::time_point observedAt;
    std::mutex mutex;
};

bool currentWindowMatches(const DesktopNativeWindow& before, const DesktopNativeWindow& now) {
    return before.id == now.id && before.processId == now.processId && before.title == now.title &&
           std::abs(before.x - now.x) < 3 && std::abs(before.y - now.y) < 3 && std::abs(before.width - now.width) < 3 &&
           std::abs(before.height - now.height) < 3;
}

bool ratio(double value) {
    return std::isfinite(value) && value >= 0 && value <= 1;
}

MaiToolResult failure(const MaiError& error) {
    return MaiToolResult::failure(error.code(), error.message());
}

std::mutex desktopOperationMutex;

class NativeComputerBackend final : public DesktopComputerBackend {
public:
    MaiResult<std::vector<DesktopNativeWindow>> windows() override {
        return maiDesktopWindows();
    }
    MaiResult<std::vector<DesktopNativeElement>> elements(const DesktopNativeWindow& window) override {
        return maiDesktopElements(window);
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
    MaiResult<bool> isFocused(const DesktopNativeWindow& window) override {
        return maiDesktopWindowIsFocused(window);
    }
    MaiError activateForInput(const DesktopNativeWindow& window) override {
        const auto ready = maiDesktopWindowAcceptsInput(window);
        if (!ready) return ready.error();
        return ready.value() ? MaiError{} : activate(window);
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
    MaiResult<std::string> focusedText(const DesktopNativeWindow& window) override {
        return maiDesktopFocusedText(window);
    }
};

bool validText(const std::string& text, bool allowEmpty) {
    if ((!allowEmpty && text.empty()) || text.size() > 4000 || text.find('\0') != std::string::npos) return false;
    const QString decoded = QString::fromUtf8(text.data(), int(text.size()));
    return decoded.toUtf8().toStdString() == text;
}

MaiResult<std::uint64_t> windowIdFrom(const Json& value) {
    if (!value.is_string()) return {MaiErrorCode::InvalidInput, "window_id must be a decimal string"};
    const std::string id = value.get<std::string>();
    if (id.empty() || id.size() > 20 || !std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return {MaiErrorCode::InvalidInput, "invalid window ID"};
    errno = 0;
    char* end = nullptr;
    const auto parsed = std::strtoull(id.c_str(), &end, 10);
    if (errno == ERANGE || !end || *end || !parsed) return {MaiErrorCode::InvalidInput, "invalid window ID"};
    return std::uint64_t(parsed);
}

Json windowJson(const DesktopNativeWindow& window) {
    return {{"window_id", std::to_string(window.id)},
            {"process_id", window.processId},
            {"title", window.title},
            {"application", window.application},
            {"bounds", {{"x", window.x}, {"y", window.y}, {"width", window.width}, {"height", window.height}}}};
}

}  // namespace

MaiError DesktopComputerBackend::activateForInput(const DesktopNativeWindow& window) {
    return activate(window);
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

    std::shared_ptr<ObservedWindow> get(const std::string& sessionId) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = sessions.find(sessionId);
        return found == sessions.end() ? nullptr : found->second;
    }
};

DesktopComputerController::DesktopComputerController() : DesktopComputerController(nullptr) {}
DesktopComputerController::DesktopComputerController(std::shared_ptr<DesktopComputerBackend> backend)
    : impl_(std::make_unique<Impl>()) {
    impl_->backend = backend ? std::move(backend) : makeDesktopComputerBackend();
}
DesktopComputerController::~DesktopComputerController() = default;

MaiResult<std::vector<DesktopNativeWindow>> DesktopComputerController::windows() {
    return impl_->backend->windows();
}

MaiResult<DesktopComputerObservation> DesktopComputerController::observe(const std::string& sessionId,
                                                                         const std::string& windowTitle,
                                                                         const std::string& workspace,
                                                                         bool includeElements, std::uint64_t windowId,
                                                                         bool includeImage,
                                                                         std::function<bool()> isCanceled) {
    // Cancellation is checked synchronously and never retained with session state.
    const auto canceled = [&] { return isCanceled && isCanceled(); };
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
    if (!std::isfinite(selected.width) || !std::isfinite(selected.height) || selected.width <= 0 ||
        selected.height <= 0)
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
        const std::string resolved = maiResolvePathWithinRoot(workspace,
            MaiFilePath::fromUtf8(captureDirectory).append(MaiFilePath::fromUtf8(name)).toUtf8());
        if (resolved.empty())
            return {MaiErrorCode::InvalidInput, "screenshot path leaves the Session workspace"};
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
                     [&](const DesktopNativeWindow& now) { return currentWindowMatches(selected, now); }))
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
        {"bounds", {{"x", selected.x}, {"y", selected.y}, {"width", selected.width}, {"height", selected.height}}},
        {"image_width", capture.width},
        {"image_height", capture.height},
        {"has_image", includeImage},
        {"coordinate_space", "relative fractions 0..1 within the captured window"}};
    if (includeImage) {
        output["image_path"] = relativeImagePath;
        output["mime_type"] = "image/png";
    }
    if (includeElements) {
        const auto elements = impl_->backend->elements(selected);
        if (elements) {
            Json items = Json::array();
            for (const auto& item : elements.value()) {
                const double centerX = (item.x + item.width / 2 - selected.x) / selected.width;
                const double centerY = (item.y + item.height / 2 - selected.y) / selected.height;
                if (!ratio(centerX) || !ratio(centerY)) continue;
                const QString label = QString::fromUtf8(item.label.c_str(), int(item.label.size())).left(160);
                items.push_back({{"role", item.role},
                                 {"label", label.toUtf8().toStdString()},
                                 {"center_x_ratio", centerX},
                                 {"center_y_ratio", centerY},
                                 {"bounds",
                                  {{"x_ratio", (item.x - selected.x) / selected.width},
                                   {"y_ratio", (item.y - selected.y) / selected.height},
                                   {"width_ratio", item.width / selected.width},
                                   {"height_ratio", item.height / selected.height}}}});
            }
            output["elements"] = items;
        } else {
            output["elements"] = Json::array();
            output["element_error"] = elements.error().message();
        }
    }
    if (canceled()) return {MaiErrorCode::Canceled, "computer observation canceled"};
    auto state = std::make_shared<ObservedWindow>();
    state->window = selected;
    state->token = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    state->observedAt = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->sessions[sessionId] = state;
    }
    output["snapshot_id"] = state->token;
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
                                                                          const DesktopComputerAction& request) {
    const std::string& action = request.action;
    const bool positioned = action == "click" || action == "double_click" || action == "right_click" ||
                            action == "drag" || action == "scroll";
    const bool writesText = action == "type" || action == "replace_text";
    if (action != "focus" && !positioned && !writesText && action != "key" && action != "verify")
        return {MaiErrorCode::InvalidInput, "unknown computer action"};
    if (positioned && (!ratio(request.xRatio) || !ratio(request.yRatio)))
        return {MaiErrorCode::InvalidInput, "window coordinates must be fractions from 0 to 1"};
    if (action == "drag" &&
        (!ratio(request.toXRatio) || !ratio(request.toYRatio) || request.durationMs < 1 || request.durationMs > 200))
        return {MaiErrorCode::InvalidInput, "drag needs bounded end coordinates and 1..200 milliseconds"};
    if (writesText && !validText(request.text, action == "replace_text"))
        return {MaiErrorCode::InvalidInput,
                "text must be valid UTF-8 within 4000 bytes; only replacement may be empty"};
    if (action == "verify" && !validText(request.expectedText, true))
        return {MaiErrorCode::InvalidInput, "expected text must be valid UTF-8 within 4000 bytes"};
    bool keyNeedsInputCheck = false;
    if (action == "key") {
        const auto parsed = maiDesktopParseKey(request.key);
        if (!parsed) return parsed.error();
        // A shortcut can write through the clipboard or delete a protected
        // value just like type(). Only navigation away is exempt from the
        // same secure-input check; raw character keys cannot bypass it.
        keyNeedsInputCheck = parsed.value().key != DesktopKeyCode::Tab &&
                             parsed.value().key != DesktopKeyCode::Escape;
    }
    if (action == "scroll" && (request.scrollLines == 0 || request.scrollLines < -10 || request.scrollLines > 10))
        return {MaiErrorCode::InvalidInput, "scroll lines must be -10..10, excluding zero"};
    const auto canceled = [&] { return request.isCanceled && request.isCanceled(); };
    if (canceled()) return {MaiErrorCode::Canceled, "computer operation canceled"};
    const auto state = impl_->get(sessionId);
    if (!state) return {MaiErrorCode::NotFound, "observe a window in this Session first"};
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->token.empty() || state->token != request.snapshotId || state->window.id != request.windowId ||
        std::chrono::steady_clock::now() - state->observedAt > std::chrono::minutes(5))
        return {MaiErrorCode::InvalidInput, "window observation expired; observe again"};
    // Only one controller may manipulate the physical desktop at a time. The
    // native layer also checks target identity between multi-event inputs.
    std::lock_guard<std::mutex> desktopLock(desktopOperationMutex);
    const auto listed = impl_->backend->windows();
    if (!listed) return listed.error();
    bool stillVisible = false;
    for (const auto& window : listed.value()) {
        if (window.id == request.windowId) {
            stillVisible = currentWindowMatches(state->window, window);
            break;
        }
    }
    if (!stillVisible) return {MaiErrorCode::InvalidInput, "selected window changed; observe again"};
    if (canceled()) return {MaiErrorCode::Canceled, "computer operation canceled"};
    DesktopComputerActionResult receipt;
    if (request.dryRun) {
        receipt.state = "validated";
        receipt.dryRun = true;
        return receipt;  // No activation, input, focused text read or token consumption.
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
        return receipt;
    }
    // Once an OS operation is attempted its outcome may be partial. Require a
    // new observation even when activation or an input batch reports failure.
    state->token.clear();
    const MaiError activated = action == "focus" ? impl_->backend->activate(state->window)
                                                : impl_->backend->activateForInput(state->window);
    if (activated) return activated;
    if (canceled()) return {MaiErrorCode::Canceled, "computer operation canceled"};
    const auto focused = action == "focus" ? impl_->backend->isFocused(state->window)
                                           : impl_->backend->inputFocused(state->window);
    if (!focused) return focused.error();
    if (!focused.value()) return {MaiErrorCode::Busy, "selected window did not become focused"};
    if (action == "focus") {
        receipt.state = "focused";
        receipt.dispatched = true;
        return receipt;
    }
    if (writesText || keyNeedsInputCheck) {
        const auto secure = impl_->backend->secureInput();
        if (!secure) return secure.error();
        if (secure.value())
            return {MaiErrorCode::NotSupported, "password and other secure inputs need manual user entry"};
    }
    DesktopNativeInput input;
    input.expectedWindowId = state->window.id;
    input.expectedProcessId = state->window.processId;
    input.isCanceled = request.isCanceled;
    input.text = request.text;
    input.key = request.key;
    input.lines = request.scrollLines;
    input.durationMs = request.durationMs;
    input.x = state->window.x + request.xRatio * (std::max)(1.0, state->window.width - 1);
    input.y = state->window.y + request.yRatio * (std::max)(1.0, state->window.height - 1);
    input.toX = state->window.x + request.toXRatio * (std::max)(1.0, state->window.width - 1);
    input.toY = state->window.y + request.toYRatio * (std::max)(1.0, state->window.height - 1);
    input.kind = action == "click"          ? DesktopNativeInputKind::Click
                 : action == "double_click" ? DesktopNativeInputKind::DoubleClick
                 : action == "right_click"  ? DesktopNativeInputKind::RightClick
                 : action == "drag"         ? DesktopNativeInputKind::Drag
                 : action == "type"         ? DesktopNativeInputKind::Type
                 : action == "replace_text" ? DesktopNativeInputKind::ReplaceFocusedText
                 : action == "key"          ? DesktopNativeInputKind::Key
                                            : DesktopNativeInputKind::Scroll;
    const MaiError sent = impl_->backend->input(input);
    if (sent) return sent;
    if (canceled()) return {MaiErrorCode::Canceled, "computer operation canceled"};
    receipt.state = "dispatched";
    receipt.dispatched = true;
    return receipt;
}

MaiError DesktopComputerController::clear(const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->sessions.erase(sessionId) == 0) return {MaiErrorCode::NotFound, "this Session has no selected window"};
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
    std::string description() const override {
        return "Operate visible PC applications through native keyboard/mouse input. First list windows, "
               "then observe one window_id (or a unique exact window_title) to get window metadata, "
               "Accessibility/UIA elements and snapshot_id. Observation does not capture window images. "
               "Use the screenshot tool for screen images. "
               "Use window-relative 0..1 coordinates for "
               "click, double_click, right_click, drag and scroll; focus activates that window. "
               "key accepts shortcuts such as Ctrl+A, Cmd+A or Shift+Home. type enters Unicode; "
               "replace_text selects all in the focused editable control then types the replacement "
               "(an empty string deletes its text). Re-observe after every operation before reusing "
               "the window. verify reads the focused non-password control and compares expected_text; "
               "only verified=true confirms that readback, not application save/persistence. "
               "dry_run validates a planned action without activating or injecting input. "
               "Approval follows the Agent policy; OS focus, interactive-desktop and platform "
            "permissions still apply. If focus or permissions are refused, explain the condition "
            "and wait for user assistance instead of repeatedly retrying or inventing shell scripts. "
            "clear only forgets the selected window and snapshot; "
               "it does not clear input text. After clear, observe again.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{
          "action":{"type":"string","enum":["list","observe","focus","click","double_click","right_click","drag","type","replace_text","key","scroll","verify","clear"]},
          "window_title":{"type":"string","maxLength":500},"window_id":{"type":"string"},
          "include_elements":{"type":"boolean"},"snapshot_id":{"type":"string"},
          "x_ratio":{"type":"number","minimum":0,"maximum":1},"y_ratio":{"type":"number","minimum":0,"maximum":1},
          "to_x_ratio":{"type":"number","minimum":0,"maximum":1},"to_y_ratio":{"type":"number","minimum":0,"maximum":1},
          "duration_ms":{"type":"integer","minimum":1,"maximum":200},
          "text":{"type":"string","minLength":0,"maxLength":4000},
          "key":{"type":"string","minLength":1,"maxLength":64,"description":"Single key or modifier chord, e.g. Ctrl+A, Cmd+A, Shift+Home"},
          "lines":{"type":"integer","minimum":-10,"maximum":10},
          "expected_text":{"type":"string","maxLength":4000},"dry_run":{"type":"boolean"}
        },"required":["action"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string& raw) const override {
        return requiresPerCallApproval(raw);
    }
    bool requiresPerCallApproval(const std::string& raw) const override {
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object() || !args.value("action", Json{}).is_string()) return true;
        return args["action"].get<std::string>() != "clear";
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        if (context.isCanceled()) return MaiToolResult::failure(MaiErrorCode::Canceled, "computer operation canceled");
        if (context.sessionId.empty())
            return MaiToolResult::failure(MaiErrorCode::NotConfigured, "a local Desktop Session is required");
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object() || !args.value("action", Json{}).is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "computer action is required");
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
            Json output{{"windows", items}, {"next_step", "Observe the selected window_id before an action."}};
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
            if (args.contains("include_image"))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "observe returns metadata only; use screenshot for screen images");
            if (!exact({"action", "window_title", "window_id", "include_elements"}) ||
                (args.contains("window_title") == args.contains("window_id")) ||
                (args.contains("window_title") && !args["window_title"].is_string()) ||
                (args.contains("include_elements") && !args["include_elements"].is_boolean()) || context.root.empty())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "observe needs one exact window title or ID and a workspace");
            std::uint64_t id = 0;
            if (args.contains("window_id")) {
                const auto parsed = windowIdFrom(args["window_id"]);
                if (!parsed) return failure(parsed.error());
                id = parsed.value();
            }
            const auto observed =
                controller_->observe(context.sessionId, args.value("window_title", std::string()), context.root,
                                     args.value("include_elements", true), id, false,
                                     [&context] { return context.isCanceled(); });
            if (!observed) return failure(observed.error());
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
        if (!args.value("snapshot_id", Json{}).is_string() || !args.value("window_id", Json{}).is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "action needs an observed snapshot and window ID");
        const auto id = windowIdFrom(args["window_id"]);
        if (!id) return failure(id.error());
        const bool positioned = action == "click" || action == "double_click" || action == "right_click" ||
                                action == "drag" || action == "scroll";
        const bool writesText = action == "type" || action == "replace_text";
        for (auto field = args.begin(); field != args.end(); ++field) {
            const std::string& name = field.key();
            const bool allowed =
                name == "action" || name == "snapshot_id" || name == "window_id" || name == "dry_run" ||
                (positioned && (name == "x_ratio" || name == "y_ratio")) ||
                (action == "drag" && (name == "to_x_ratio" || name == "to_y_ratio" || name == "duration_ms")) ||
                (writesText && name == "text") || (action == "key" && name == "key") ||
                (action == "scroll" && name == "lines") || (action == "verify" && name == "expected_text");
            if (!allowed)
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "unexpected computer action parameter");
        }
        if ((positioned &&
             (!args.value("x_ratio", Json{}).is_number() || !args.value("y_ratio", Json{}).is_number())) ||
            (writesText && !args.value("text", Json{}).is_string()) ||
            (action == "key" && !args.value("key", Json{}).is_string()) ||
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
        request.dryRun = args.value("dry_run", false);
        request.isCanceled = [&context] { return context.isCanceled(); };
        const auto acted = controller_->perform(context.sessionId, request);
        if (!acted) return failure(acted.error());
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
        if (action == "verify") {
            const QString actual =
                QString::fromUtf8(acted.value().actualText.data(), int(acted.value().actualText.size()));
            output["actual_text"] = actual.left(4000).toUtf8().toStdString();
            output["actual_text_truncated"] = actual.size() > 4000;
        }
        return MaiToolResult::success(output.dump());
    }

private:
    std::shared_ptr<DesktopComputerController> controller_;
};

}  // namespace

void registerDesktopComputerTools(MaiToolRegistry& registry, std::shared_ptr<DesktopComputerController> controller) {
    registry.add(std::make_unique<DesktopComputerTool>(std::move(controller)));
}
