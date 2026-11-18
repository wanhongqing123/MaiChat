// Opt-in owned Qt/UIA regression. All input and retained pixels belong to this
// process. UIA/controller/tool calls run on workers while the GUI pumps events;
// querying a same-process Qt provider on its GUI thread can otherwise deadlock.
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QSaveFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <json.hpp>

#include "MaiChatTools/DesktopComputerTools.h"
#include "MaiChatTools/DesktopComputerBackend.h"
#include "MaiTool.h"
#include "MaiTime.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#endif

namespace {
using Json = nlohmann::json;

int unavailable(const char* reason) {
    std::fprintf(stderr, "owned computer actions SKIP: %s; native actions were not fully verified\n", reason);
    return 77;
}

void pump(int milliseconds = 40) {
    QElapsedTimer timer;
    timer.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    } while (timer.elapsed() < milliseconds);
}

template<class Function>
auto worker(Function&& function) {
    auto pending = std::async(std::launch::async, std::forward<Function>(function));
    while (pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) pump(5);
    return pending.get();
}

bool writeJson(const QString& path, const Json& value) {
    QSaveFile file(path);
    const auto bytes = QByteArray::fromStdString(value.dump(2));
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

#if defined(_WIN32)
std::uint64_t windowId(HWND window) {
    return std::uint64_t(reinterpret_cast<std::uintptr_t>(window));
}

HWND nativeHandle(QWidget& window) {
    return reinterpret_cast<HWND>(window.winId());
}

bool physicalInputReleased() {
    for (int key = 1; key < 255; ++key)
        if ((GetAsyncKeyState(key) & 0x8000) != 0) return false;
    return true;
}

// Native physical marker positions stay clear of the smaller main window and
// popup. Their alternating black/white pattern is checked only in memory.
std::vector<QPoint> markerPoints(int height) {
    return {{20, 20}, {44, 20}, {20, height - 20}, {44, height - 20}};
}

class OwnedBackground final : public QWidget {
public:
    explicit OwnedBackground(const DesktopNativeDisplay& display)
        : QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint), display_(display) {}
    void setPhase(int phase) {
        phase_ = phase;
        repaint();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), phase_ == 1 ? QColor(41, 104, 61) : QColor(112, 58, 127));
        const double scaleX = width() / display_.width;
        const double scaleY = height() / display_.height;
        const auto markers = markerPoints(int(display_.height));
        for (std::size_t index = 0; index < markers.size(); ++index) {
            const auto& point = markers[index];
            const bool white = (int(index) + phase_) % 2 != 0;
            painter.fillRect(QRectF((point.x() - 6) * scaleX, (point.y() - 6) * scaleY, 12 * scaleX, 12 * scaleY),
                             white ? Qt::white : Qt::black);
        }
    }

private:
    DesktopNativeDisplay display_;
    int phase_ = 0;
};

// The background remains visible when the smaller main window is minimized.
// Check native physical bounds and every surface above it before AND after
// capture, so an unrelated notification never becomes retained test evidence.
struct OwnedDisplay {
    HWND background = nullptr;
    DesktopNativeDisplay display;
    std::atomic<std::int64_t> coverageBarrier{0};
    std::atomic<int> expectedPhase{0};
    mutable Json frameChecks = Json::array();

    bool owns(HWND window) const {
        DWORD process = 0;
        return window && IsWindow(window) && GetWindowThreadProcessId(window, &process) &&
               process == GetCurrentProcessId();
    }

    bool covered() const {
        if (!owns(background) || !IsWindowVisible(background) || IsIconic(background)) return false;
        DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_DEFAULT;
        if (FAILED(DwmGetWindowAttribute(background, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners))) ||
            corners != DWMWCP_DONOTROUND)
            return false;
        RECT frame{};
        if (!GetWindowRect(background, &frame) || frame.left > display.x || frame.top > display.y ||
            frame.right < display.x + display.width || frame.bottom < display.y + display.height)
            return false;
        RECT visible{};
        if (FAILED(DwmGetWindowAttribute(background, DWMWA_EXTENDED_FRAME_BOUNDS, &visible, sizeof(visible))) ||
            visible.left > display.x || visible.top > display.y || visible.right < display.x + display.width ||
            visible.bottom < display.y + display.height)
            return false;
        for (HWND window = GetTopWindow(nullptr); window; window = GetWindow(window, GW_HWNDNEXT)) {
            if (window == background) return true;
            if (!IsWindowVisible(window) || IsIconic(window)) continue;
            DWORD cloaked = 0;
            if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
                continue;
            RECT bounds{};
            if (!GetWindowRect(window, &bounds)) return false;
            const bool intersects = bounds.right > display.x && bounds.bottom > display.y &&
                                    bounds.left < display.x + display.width && bounds.top < display.y + display.height;
            if (intersects && !owns(window)) return false;
        }
        return false;
    }

    bool surfaceReady() const { return covered() && owns(GetForegroundWindow()); }
    bool ready() const { return surfaceReady() && physicalInputReleased(); }

    Json readinessDiagnostic() const {
        const auto rectangle = [](const RECT& bounds) -> Json {
            return {{"x", bounds.left}, {"y", bounds.top},
                    {"width", bounds.right - bounds.left}, {"height", bounds.bottom - bounds.top}};
        };
        const auto covers = [&](const RECT& bounds) {
            return bounds.left <= display.x && bounds.top <= display.y &&
                   bounds.right >= display.x + display.width && bounds.bottom >= display.y + display.height;
        };
        DWORD backgroundPid = 0;
        GetWindowThreadProcessId(background, &backgroundPid);
        RECT outer{};
        const bool hasOuter = GetWindowRect(background, &outer) != FALSE;
        const DWORD outerError = hasOuter ? 0 : GetLastError();
        RECT visible{};
        const HRESULT visibleStatus = DwmGetWindowAttribute(background, DWMWA_EXTENDED_FRAME_BOUNDS,
                                                             &visible, sizeof(visible));
        DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_DEFAULT;
        const HRESULT cornerStatus = DwmGetWindowAttribute(background, DWMWA_WINDOW_CORNER_PREFERENCE,
                                                            &corners, sizeof(corners));
        const HWND foreground = GetForegroundWindow();
        DWORD foregroundPid = 0;
        GetWindowThreadProcessId(foreground, &foregroundPid);
        Json blockingSurface = nullptr;
        std::string obstruction;
        bool reachedBackground = false;
        unsigned above = 0;
        for (HWND window = GetTopWindow(nullptr); window; window = GetWindow(window, GW_HWNDNEXT)) {
            if (window == background) {
                reachedBackground = true;
                break;
            }
            if (!IsWindowVisible(window) || IsIconic(window)) continue;
            DWORD cloaked = 0;
            const HRESULT cloakStatus = DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            if (SUCCEEDED(cloakStatus) && cloaked) continue;
            ++above;
            RECT bounds{};
            const bool hasBounds = GetWindowRect(window, &bounds) != FALSE;
            const DWORD boundsError = hasBounds ? 0 : GetLastError();
            const bool intersects = hasBounds && bounds.right > display.x && bounds.bottom > display.y &&
                                    bounds.left < display.x + display.width && bounds.top < display.y + display.height;
            if (!hasBounds || (intersects && !owns(window))) {
                DWORD process = 0;
                GetWindowThreadProcessId(window, &process);
                obstruction = hasBounds ? "foreign_surface_above_background" : "surface_geometry_unavailable";
                // Only the first blocking surface's geometry/identity is kept;
                // no foreign title, body, application path or pixels are read.
                blockingSurface = {{"window_id", std::to_string(windowId(window))}, {"process_id", process},
                                    {"owned", owns(window)}, {"bounds_available", hasBounds},
                                    {"bounds", hasBounds ? rectangle(bounds) : Json(nullptr)},
                                    {"get_window_rect_error", boundsError}, {"intersects_display", intersects},
                                    {"dwm_cloak_hresult", std::int32_t(cloakStatus)}};
                break;
            }
        }
        const bool backgroundOwned = owns(background);
        const bool backgroundVisible = IsWindowVisible(background) != FALSE;
        const bool backgroundMinimized = IsIconic(background) != FALSE;
        const bool foregroundOwned = owns(foreground);
        const bool released = physicalInputReleased();
        std::string reason = "ready";
        if (!backgroundOwned) reason = "background_identity_unavailable";
        else if (!backgroundVisible) reason = "background_hidden";
        else if (backgroundMinimized) reason = "background_minimized";
        else if (FAILED(cornerStatus)) reason = "corner_preference_unavailable";
        else if (corners != DWMWCP_DONOTROUND) reason = "background_not_confirmed_nonrounded";
        else if (!hasOuter) reason = "background_outer_bounds_unavailable";
        else if (!covers(outer)) reason = "background_outer_bounds_do_not_cover_display";
        else if (FAILED(visibleStatus)) reason = "background_dwm_bounds_unavailable";
        else if (!covers(visible)) reason = "background_dwm_bounds_do_not_cover_display";
        else if (!obstruction.empty()) reason = obstruction;
        else if (!reachedBackground) reason = "background_not_reached_in_z_order";
        else if (!foregroundOwned) reason = "foreground_not_owned";
        else if (!released) reason = "physical_input_held";
        return {{"reason", reason}, {"recorded_at_ms", MaiTime::getCurrentTime()},
                {"display_bounds", {{"x", display.x}, {"y", display.y}, {"width", display.width}, {"height", display.height}}},
                {"background_window_id", std::to_string(windowId(background))}, {"background_process_id", backgroundPid},
                {"background_owned", backgroundOwned}, {"background_visible", backgroundVisible},
                {"background_minimized", backgroundMinimized}, {"background_outer_bounds", hasOuter ? rectangle(outer) : Json(nullptr)},
                {"get_window_rect_error", outerError}, {"outer_covers_display", hasOuter && covers(outer)},
                {"background_dwm_bounds", SUCCEEDED(visibleStatus) ? rectangle(visible) : Json(nullptr)},
                {"dwm_bounds_hresult", std::int32_t(visibleStatus)},
                {"dwm_bounds_cover_display", SUCCEEDED(visibleStatus) && covers(visible)},
                {"corner_preference_hresult", std::int32_t(cornerStatus)}, {"corner_preference", int(corners)},
                {"background_reached_in_z_order", reachedBackground}, {"visible_surfaces_examined_above", above},
                {"blocking_surface", std::move(blockingSurface)},
                {"foreground_window_id", std::to_string(windowId(foreground))}, {"foreground_process_id", foregroundPid},
                {"foreground_owned", foregroundOwned}, {"physical_input_released", released}};
    }

    bool acceptsFrame(const MaiScreenshot& frame, std::int64_t barrier) const {
        const auto image = QImage::fromData(frame.pngBytes.data(), int(frame.pngBytes.size()), "PNG");
        const int phase = expectedPhase.load();
        const double scaleX = display.width > 0 ? image.width() / display.width : 0;
        const double scaleY = display.height > 0 ? image.height() / display.height : 0;
        // Production PNGs may be reduced to fit their encoding size limit.
        // Permit uniform reduction with at most one output pixel of rounding;
        // retain native dimensions for input and locate markers only in PNGs.
        const double aspectTolerance = display.width > 0 && display.height > 0
            ? 1.0 / display.width + 1.0 / display.height : 0;
        const bool dimensions = !image.isNull() && display.width >= 64 && display.height >= 64 &&
                                 frame.width == image.width() && frame.height == image.height() &&
                                 scaleX > 0 && scaleY > 0 && scaleX <= 1 && scaleY <= 1 &&
                                 scaleX - scaleY <= aspectTolerance && scaleY - scaleX <= aspectTolerance &&
                                 12 * scaleX >= 2 && 12 * scaleY >= 2;
        // Keep the usual 3x3 patch when the scaled marker has room, falling
        // back to its interior center for smaller markers to avoid edge blends.
        const int radiusX = std::max(0, std::min(1, int(6 * scaleX - 1.5)));
        const int radiusY = std::max(0, std::min(1, int(6 * scaleY - 1.5)));
        Json sampled = Json::array();
        bool markersMatch = dimensions && phase > 0;
        if (dimensions) {
            const auto markers = markerPoints(int(display.height));
            for (std::size_t index = 0; index < markers.size(); ++index) {
                const auto nativePoint = markers[index];
                const QPoint point(int((nativePoint.x() + 0.5) * scaleX), int((nativePoint.y() + 0.5) * scaleY));
                const auto color = image.pixelColor(point);
                const bool white = (int(index) + phase) % 2 != 0;
                bool patchMatches = image.rect().contains(QRect(point - QPoint(radiusX, radiusY),
                                                                 point + QPoint(radiusX, radiusY)));
                for (int dy = -radiusY; dy <= radiusY && patchMatches; ++dy)
                    for (int dx = -radiusX; dx <= radiusX; ++dx) {
                        const auto pixel = image.pixelColor(point + QPoint(dx, dy));
                        patchMatches = patchMatches && (white ? pixel.red() >= 200 && pixel.green() >= 200 && pixel.blue() >= 200
                                                               : pixel.red() <= 48 && pixel.green() <= 48 && pixel.blue() <= 48);
                    }
                markersMatch = markersMatch && patchMatches;
                sampled.push_back({{"native_x", nativePoint.x()}, {"native_y", nativePoint.y()},
                                    {"png_x", point.x()}, {"png_y", point.y()}, {"expected_white", white},
                                    {"rgb", {color.red(), color.green(), color.blue()}}, {"matched", patchMatches}});
            }
        }
        const bool fresh = barrier > 0 && frame.frameReceivedAtMs > barrier;
        const bool admitted = fresh && markersMatch;
        frameChecks.push_back({{"phase", phase}, {"coverage_confirmed_at_ms", coverageBarrier.load()},
                                {"requested_after_frame_received_at_ms", barrier},
                                {"frame_received_at_ms", frame.frameReceivedAtMs}, {"frame_sequence", frame.frameSequence},
                                {"dimensions_valid", dimensions}, {"native_size", {display.width, display.height}},
                                {"png_size", {image.width(), image.height()}}, {"png_scale", {scaleX, scaleY}},
                                {"sample_radius", {radiusX, radiusY}}, {"received_after_barrier", fresh},
                                {"markers", std::move(sampled)}, {"accepted_by_memory_gate", admitted}});
        return admitted;
    }
};

Json awaitManualActivation(QWidget& main, QLabel& caption, const OwnedDisplay& owned, int limitMs) {
    const HWND selected = nativeHandle(main);
    Json result{{"attempted", false}, {"configured_wait_ms", limitMs}, {"actual_wait_ms", 0},
                 {"activated", false}, {"source", "manual"}, {"stop_reason", "entry_preconditions_not_met"}};
    if (limitMs <= 0 || !owned.covered() || !physicalInputReleased() || !owned.owns(selected) ||
        !IsWindowVisible(selected) || IsIconic(selected))
        return result;
    result["attempted"] = true;
    const auto originalTitle = main.windowTitle();
    const auto originalCaption = caption.text();
    const auto restorePrompt = qScopeGuard([&] {
        main.setWindowTitle(originalTitle);
        caption.setText(originalCaption);
    });
    main.setWindowTitle(QStringLiteral("MaiChat owned native test - click this title bar to continue"));
    caption.setText(QStringLiteral("Click this title bar, then release the mouse button."));
    main.repaint();
    std::fprintf(stderr, "owned manual activation: click test title bar HWND=%llu; waiting at most %d ms\n",
                 static_cast<unsigned long long>(windowId(selected)), limitMs);
    QElapsedTimer elapsed;
    elapsed.start();
    std::string reason = "timeout";
    bool activated = false;
    while (elapsed.elapsed() < limitMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        if (!owned.covered()) {
            reason = "owned_coverage_lost";
            break;
        }
        if (!owned.owns(selected) || !IsWindowVisible(selected) || IsIconic(selected)) {
            reason = "owned_main_unavailable";
            break;
        }
        if (GetForegroundWindow() == selected && physicalInputReleased()) {
            activated = true;
            reason = "owned_main_foreground_and_all_input_released";
            break;
        }
        bool unrelatedInput = false;
        for (int key = 1; key < 255; ++key)
            if (key != VK_LBUTTON && (GetAsyncKeyState(key) & 0x8000) != 0) unrelatedInput = true;
        if (unrelatedInput) {
            reason = "unrelated_physical_input";
            break;
        }
        // A real activating click necessarily has a brief left-button DOWN.
        // Permit only that click over this exact main/child HWND and continue
        // waiting for UP; no synthetic state or physical key is released here.
        if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) {
            POINT pointer{};
            const HWND hit = GetCursorPos(&pointer) ? WindowFromPoint(pointer) : nullptr;
            if (!hit || (hit != selected && !IsChild(selected, hit))) {
                reason = "left_click_outside_owned_main";
                break;
            }
        }
        QThread::msleep(2);
    }
    result["actual_wait_ms"] = elapsed.elapsed();
    result["activated"] = activated;
    result["stop_reason"] = reason;
    result["foreground_window_id"] = std::to_string(windowId(GetForegroundWindow()));
    result["physical_input_released"] = physicalInputReleased();
    return result;
}

class OwnedInputSession final : public DesktopNativeInputSession {
public:
    OwnedInputSession(const OwnedDisplay& owned, std::shared_ptr<DesktopNativeInputSession> native)
        : owned_(owned), native_(std::move(native)) {}
    MaiError input(const DesktopNativeInput& input) override {
        auto guarded = input;
        // A synthetic DOWN is expected during the call, so cancellation checks
        // ownership/coverage rather than mistaking our own hold for user input.
        guarded.isCanceled = [&] { return !owned_.surfaceReady() || (input.isCanceled && input.isCanceled()); };
        return native_->input(guarded);
    }
    DesktopNativeInputState state() const override { return native_->state(); }
    MaiError validate() const override { return native_->validate(); }
    MaiError resetControls() override { return native_->resetControls(); }
    MaiError releaseAll() override { return native_->releaseAll(); }
    DesktopInputCapabilities capabilities() const override { return native_->capabilities(); }

private:
    const OwnedDisplay& owned_;
    std::shared_ptr<DesktopNativeInputSession> native_;
};

// Narrow discovery and reject capture before pixels reach the controller's
// PNG writer. This decorator owns no executor or input state; every operation
// delegates to the exact production backend/native input session.
class OwnedBackend final : public DesktopComputerBackend {
public:
    explicit OwnedBackend(const OwnedDisplay& owned) : owned_(owned), native_(makeDesktopComputerBackend()) {}
    std::atomic<bool> cancelNextQueuedState{false};
    std::atomic<unsigned> queuedStateCancellationChecks{0};
    MaiResult<std::vector<DesktopNativeWindow>> windows() override {
        auto result = native_->windows();
        if (!result) return result.error();
        auto& values = result.value();
        values.erase(std::remove_if(values.begin(), values.end(), [&](const auto& window) {
            return window.processId != GetCurrentProcessId() ||
                   !owned_.owns(reinterpret_cast<HWND>(std::uintptr_t(window.id)));
        }), values.end());
        return result;
    }
    MaiResult<std::vector<DesktopNativeDisplay>> displays() override {
        return std::vector<DesktopNativeDisplay>{owned_.display};
    }
    MaiResult<std::vector<DesktopNativeElement>> elements(const DesktopNativeWindow& window) override {
        return native_->elements(window);
    }
    MaiResult<DesktopNativeAccessibilitySnapshot> inspectElements(
        const DesktopNativeWindow& window, const std::function<bool()>& canceled) override {
        return native_->inspectElements(window, canceled);
    }
    MaiResult<DesktopNativeElement> resolveElement(const DesktopNativeWindow& window,
                                                   const DesktopNativeElement& observed,
                                                   const std::function<bool()>& canceled) override {
        return native_->resolveElement(window, observed, canceled);
    }
    MaiError setWindowState(const DesktopNativeWindow& window, DesktopWindowStateAction action,
                            const std::function<bool()>& canceled) override {
        if (cancelNextQueuedState.exchange(false)) {
            unsigned checks = 0;
            // In the current native implementation the second cancellation
            // check is after successful ShowWindowAsync queueing. The request
            // can still execute after Canceled; this gate does not roll it back.
            const auto result = native_->setWindowState(window, action, [&] {
                ++checks;
                return checks >= 2 || (canceled && canceled());
            });
            queuedStateCancellationChecks.store(checks);
            return result;
        }
        return native_->setWindowState(window, action, canceled);
    }
    MaiError capture(const DesktopNativeWindow& window, MaiScreenshot& output) override {
        if (!owned_.ready()) return canceledCapture(output);
        const auto result = native_->capture(window, output);
        return owned_.ready() ? result : canceledCapture(output);
    }
    MaiError captureDisplay(const DesktopNativeDisplay& display, MaiScreenshot& output,
                            const CaptureOptions& options) override {
        if (!owned_.ready() || display.id != owned_.display.id) return canceledCapture(output);
        const auto barrier = owned_.coverageBarrier.load();
        if (barrier <= 0) return canceledCapture(output);
        auto guarded = options;
        guarded.afterFrameReceivedAtMs = std::max(options.afterFrameReceivedAtMs, barrier);
        guarded.isCanceled = [&] { return !owned_.ready() || (options.isCanceled && options.isCanceled()); };
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        do {
            const auto result = native_->captureDisplay(display, output, guarded);
            if (!owned_.ready()) return canceledCapture(output);
            if (result) return result;
            if (owned_.acceptsFrame(output, guarded.afterFrameReceivedAtMs)) return {};
            // Reject stale/wrong-phase bytes in memory. Require a genuinely
            // later native frame on retry; never alter the returned timestamp.
            guarded.afterFrameSequence = std::max(guarded.afterFrameSequence, output.frameSequence);
            guarded.afterFrameReceivedAtMs = std::max(guarded.afterFrameReceivedAtMs, output.frameReceivedAtMs);
            output = {};
        } while (std::chrono::steady_clock::now() < deadline && !guarded.isCanceled());
        output = {};
        return {MaiErrorCode::Busy, "owned background markers were not verified in a fresh native frame; pixels discarded"};
    }
    MaiError activate(const DesktopNativeWindow& window) override { return native_->activate(window); }
    MaiError activate(const DesktopNativeWindow& window, const std::function<bool()>& canceled) override {
        return native_->activate(window, canceled);
    }
    MaiResult<bool> isFocused(const DesktopNativeWindow& window) override { return native_->isFocused(window); }
    MaiError activateForInput(const DesktopNativeWindow& window, const std::function<bool()>& canceled) override {
        return native_->activateForInput(window, canceled);
    }
    MaiResult<bool> inputFocused(const DesktopNativeWindow& window) override { return native_->inputFocused(window); }
    MaiResult<bool> secureInput() override { return native_->secureInput(); }
    MaiError input(const DesktopNativeInput& input) override {
        if (!owned_.ready()) return {MaiErrorCode::Canceled, "owned input surface changed"};
        return native_->input(input);
    }
    std::shared_ptr<DesktopNativeInputSession> createInputSession() override {
        auto session = native_->createInputSession();
        return session ? std::make_shared<OwnedInputSession>(owned_, std::move(session)) : nullptr;
    }
    DesktopInputCapabilities inputCapabilities() const override { return native_->inputCapabilities(); }
    MaiResult<std::string> focusedText(const DesktopNativeWindow& window) override { return native_->focusedText(window); }

private:
    static MaiError canceledCapture(MaiScreenshot& output) {
        output = {};
        return {MaiErrorCode::Canceled, "owned display coverage or foreground changed; pixels discarded"};
    }
    const OwnedDisplay& owned_;
    std::shared_ptr<DesktopComputerBackend> native_;
};

struct Workflow {
    std::shared_ptr<DesktopComputerController> controller;
    std::shared_ptr<OwnedBackend> backend;
    MaiToolRegistry registry;
    MaiTool* tool = nullptr;
    MaiToolContext context;
    std::atomic<bool> canceled{false};
    Json trace = Json::array();
    QString evidence;
    int checks = 0;
    int failures = 0;
    bool attemptedInput = false;

    explicit Workflow(const QString& workspace, const OwnedDisplay& owned, QString evidenceDirectory)
        : evidence(std::move(evidenceDirectory)) {
        backend = std::make_shared<OwnedBackend>(owned);
        controller = std::make_shared<DesktopComputerController>(backend);
        registerDesktopComputerTools(registry, controller);
        tool = registry.find("desktop_computer");
        context.sessionId = "owned-actions-" + std::to_string(GetCurrentProcessId());
        context.turnId = "owned-actions-turn";
        context.root = workspace.toUtf8().toStdString();
        context.cancel = &canceled;
    }

    void check(bool passed, const char* reason) {
        ++checks;
        if (!passed) {
            ++failures;
            std::fprintf(stderr, "owned computer actions FAIL: %s\n", reason);
        }
        trace.push_back({{"check", reason}, {"passed", passed}});
    }

    MaiToolResult call(const Json& request, const char* stage) {
        const auto action = request.value("action", std::string());
        if (action == "click" || action == "double_click" || action == "right_click") attemptedInput = true;
        const auto result = worker([&] { return tool->execute(request.dump(), context); });
        Json record{{"stage", stage}, {"request", request}, {"ok", !result.hasError()}};
        if (result.hasError()) {
            record["error"] = result.error().message();
            record["error_code"] = int(result.error().code());
        } else {
            record["result"] = Json::parse(result.output(), nullptr, false);
            if (!evidence.isEmpty()) {
                Json images = Json::array();
                for (std::size_t index = 0; index < result.images().size(); ++index) {
                    const QString filename = QString::fromLatin1(stage) + QStringLiteral("-%1.png").arg(qulonglong(index));
                    const bool copied = QFile::copy(QString::fromStdString(result.images()[index].path),
                                                     QDir(evidence).filePath(filename));
                    check(copied, "owned tool screenshot evidence is preserved");
                    images.push_back(filename.toStdString());
                }
                record["owned_images"] = std::move(images);
            }
        }
        trace.push_back(std::move(record));
        return result;
    }

    Json observe(std::uint64_t id, const char* stage) {
        const auto result = call({{"action", "observe"}, {"target", "window"},
                                  {"window_id", std::to_string(id)}, {"include_elements", true}}, stage);
        check(!result.hasError(), "owned window observation succeeds");
        return result.hasError() ? Json::object() : Json::parse(result.output(), nullptr, false);
    }

    static std::string element(const Json& snapshot, const char* label) {
        std::string result;
        if (!snapshot.contains("elements") || !snapshot["elements"].is_array()) return result;
        for (const auto& item : snapshot["elements"]) {
            if (item.value("label", std::string()) != label) continue;
            if (!result.empty()) return {};
            result = item.value("element_id", std::string());
        }
        return result;
    }

    static Json click(std::uint64_t id, const Json& snapshot, const std::string& elementId) {
        return {{"action", "click"}, {"target", "window"}, {"window_id", std::to_string(id)},
                {"snapshot_id", snapshot.value("snapshot_id", std::string())}, {"element_id", elementId}};
    }
};

// A separate explicit mode proves the native operations which do not require
// foreground. It never creates a controller/input session or captures pixels;
// success here does not imply that the full ID-click workflow was exercised.
int runNativeStateOnly() {
    if (qEnvironmentVariable("MAICHAT_RUN_COMPUTER_ACTIONS_NATIVE") != QStringLiteral("1"))
        return unavailable("set MAICHAT_RUN_COMPUTER_ACTIONS_NATIVE=1 for the isolated owned fixture");
    if (QApplication::platformName() != QStringLiteral("windows")) return unavailable("real Windows QPA required");
    const QString evidence = qEnvironmentVariable("MAICHAT_COMPUTER_NATIVE_EVIDENCE_DIR");
    if (!evidence.isEmpty() && !QDir().mkpath(evidence)) return 1;
    QTemporaryDir workspace;
    if (!workspace.isValid()) return 1;

    const HWND foregroundBefore = GetForegroundWindow();
    QWidget window;
    window.setAttribute(Qt::WA_ShowWithoutActivating);
    window.setWindowTitle(QStringLiteral("MaiChat owned native state-only %1").arg(GetCurrentProcessId()));
    window.resize(480, 260);
    window.move(80, 80);
    QLabel caption(QStringLiteral("Owned native accessibility and state only; no input"), &window);
    caption.setGeometry(20, 15, 440, 30);
    auto* button = new QPushButton(QStringLiteral("Owned state-only button"), &window);
    button->setAccessibleName(QStringLiteral("Owned state-only button"));
    button->setObjectName(QStringLiteral("ownedStateOnlyButton"));
    button->setGeometry(25, 65, 270, 55);
    window.show();
    pump(100);
    const HWND selected = nativeHandle(window);
    DesktopNativeWindow target;
    target.id = windowId(selected);
    target.processId = GetCurrentProcessId();
    int checks = 0;
    int failures = 0;
    Json steps = Json::array();
    const auto check = [&](bool passed, const char* reason) {
        ++checks;
        if (!passed) {
            ++failures;
            std::fprintf(stderr, "owned state-only FAIL: %s\n", reason);
        }
        steps.push_back({{"check", reason}, {"passed", passed}});
    };
    const auto bounds = [](const DesktopNativeElement& item) -> Json {
        return {{"x", item.x}, {"y", item.y}, {"width", item.width}, {"height", item.height}};
    };
    check(IsWindow(selected) && IsWindowVisible(selected) && GetForegroundWindow() != selected,
          "known owned HWND is visible without taking foreground");
    const auto inspected = worker([&] { return maiDesktopInspectElements(target); });
    check(bool(inspected), "native UIA inspection uses the known inactive HWND/PID");
    std::optional<DesktopNativeElement> observed;
    std::size_t matches = 0;
    if (inspected) {
        steps.push_back({{"stage", "native-inspection"}, {"provider", inspected.value().provider},
                         {"status", inspected.value().status}, {"visited", inspected.value().visitedNodes},
                         {"returned", inspected.value().elements.size()}, {"truncated", inspected.value().truncated},
                         {"limit_reason", inspected.value().limitReason}});
        check(inspected.value().status == "available" && inspected.value().countsKnown &&
                  inspected.value().visitedNodes > 0 && inspected.value().visitedNodes <= 250 &&
                  inspected.value().elements.size() <= 80,
              "inactive owned UIA tree has complete bounded traversal evidence");
        for (const auto& item : inspected.value().elements)
            if (item.role == "button" && item.label == "Owned state-only button") {
                ++matches;
                observed = item;
            }
        check(matches == 1 && observed && !observed->nativeId.empty(),
              "inactive owned button exposes a unique native identity");
    } else {
        steps.push_back({{"stage", "native-inspection"}, {"error", inspected.error().message()}});
    }
    if (matches == 1 && observed && !observed->nativeId.empty()) {
        const auto same = worker([&] { return maiDesktopResolveElement(target, *observed); });
        check(same && same.value().nativeId == observed->nativeId && same.value().role == observed->role &&
                  same.value().label == observed->label && same.value().x == observed->x && same.value().y == observed->y &&
                  same.value().width == observed->width && same.value().height == observed->height,
              "fresh native resolution identifies the same inactive button");
        button->move(button->x() + 65, button->y() + 55);
        pump(60);
        const auto moved = worker([&] { return maiDesktopResolveElement(target, *observed); });
        check(moved && moved.value().nativeId == observed->nativeId &&
                  (moved.value().x != observed->x || moved.value().y != observed->y) &&
                  moved.value().width > 0 && moved.value().height > 0,
              "fresh resolution returns actual changed native bounds after layout movement");
        steps.push_back({{"stage", "layout-movement"}, {"before_native_bounds", bounds(*observed)},
                         {"after_native_bounds", moved ? bounds(moved.value()) : Json(nullptr)},
                         {"error", moved ? std::string() : moved.error().message()}});
        delete button;
        button = nullptr;
        pump(60);
        const auto destroyed = worker([&] { return maiDesktopResolveElement(target, *observed); });
        check(!destroyed && destroyed.error().code() == MaiErrorCode::NotFound,
              "destroyed owned button identity is rejected by native resolution");
        const auto canceledResolve = worker([&] {
            return maiDesktopResolveElement(target, *observed, [] { return true; });
        });
        check(!canceledResolve && canceledResolve.error().code() == MaiErrorCode::Canceled,
              "pre-canceled native resolution returns Canceled");
    }
    const auto canceledInspect = worker([&] { return maiDesktopInspectElements(target, [] { return true; }); });
    check(!canceledInspect && canceledInspect.error().code() == MaiErrorCode::Canceled,
          "pre-canceled native inspection returns Canceled");
    const auto canceledState = worker([&] {
        return maiDesktopSetWindowState(target, DesktopWindowStateAction::Minimize, [] { return true; });
    });
    check(canceledState.code() == MaiErrorCode::Canceled && !IsIconic(selected),
          "pre-canceled inactive state request does not minimize the window");
    const auto minimized = worker([&] { return maiDesktopSetWindowState(target, DesktopWindowStateAction::Minimize); });
    pump(40);
    check(!minimized && IsIconic(selected), "native inactive minimize is verified by IsIconic");
    steps.push_back({{"stage", "minimize"}, {"error", minimized.message()}, {"minimized", bool(IsIconic(selected))},
                     {"foreground_is_owned_main", GetForegroundWindow() == selected}});
    const auto restored = worker([&] { return maiDesktopSetWindowState(target, DesktopWindowStateAction::Restore); });
    pump(40);
    check(!restored && !IsIconic(selected), "native inactive restore is verified by IsIconic");
    steps.push_back({{"stage", "restore"}, {"error", restored.message()}, {"minimized", bool(IsIconic(selected))},
                     {"foreground_is_owned_main", GetForegroundWindow() == selected}});
    unsigned cancellationChecks = 0;
    const auto queuedCancel = worker([&] {
        return maiDesktopSetWindowState(target, DesktopWindowStateAction::Minimize,
                                         [&] { return ++cancellationChecks >= 2; });
    });
    pump(60);
    check(queuedCancel.code() == MaiErrorCode::Canceled && cancellationChecks == 2,
          "inactive state cancellation after successful queueing reports Canceled");
    steps.push_back({{"stage", "queued-cancellation-readback"}, {"cancellation_checks", cancellationChecks},
                     {"minimized_after_gui_pump", bool(IsIconic(selected))},
                     {"foreground_is_owned_main", GetForegroundWindow() == selected}});
    const auto recovered = worker([&] { return maiDesktopSetWindowState(target, DesktopWindowStateAction::Restore); });
    pump(40);
    check(!recovered && !IsIconic(selected), "inactive fixture recovers actual state after queued cancellation");
    DWORD processAfter = 0;
    GetWindowThreadProcessId(selected, &processAfter);
    check(IsWindow(selected) && processAfter == target.processId, "owned HWND/PID survives native state operations");
    check(GetForegroundWindow() != selected, "inactive native operations do not leave the owned window foreground");
    const Json receipt{{"fixture", "owned-native-accessibility-and-state-only"}, {"pid", GetCurrentProcessId()},
                        {"window_id", std::to_string(target.id)}, {"checks", checks}, {"failures", failures},
                        {"steps", steps}, {"input_not_tested", true}, {"input_injection_requested", false},
                        {"focus_requests_made", false}, {"screenshots_requested", false}, {"real_model_used", false},
                        {"foreground_before_window_id", std::to_string(windowId(foregroundBefore))},
                        {"foreground_after_window_id", std::to_string(windowId(GetForegroundWindow()))}};
    if (!evidence.isEmpty() && !writeJson(QDir(evidence).filePath(QStringLiteral("actions-native-state-only-receipt.json")), receipt))
        return 2;
    std::printf("Owned native state-only: checks=%d failures=%d input_not_tested=true\n", checks, failures);
    return failures ? 1 : 0;
}

int run() {
    if (qEnvironmentVariable("MAICHAT_RUN_COMPUTER_ACTIONS_NATIVE") != QStringLiteral("1"))
        return unavailable("set MAICHAT_RUN_COMPUTER_ACTIONS_NATIVE=1 for the isolated owned fixture");
    if (QApplication::platformName() != QStringLiteral("windows")) return unavailable("real Windows QPA required");
    if (!physicalInputReleased()) return unavailable("physical input is currently held");
    QTemporaryDir workspace;
    if (!workspace.isValid()) return 1;
    const QString evidence = qEnvironmentVariable("MAICHAT_COMPUTER_NATIVE_EVIDENCE_DIR");
    if (!evidence.isEmpty() && !QDir().mkpath(evidence)) return 1;

    const auto displays = worker([] { return maiDesktopDisplays(); });
    if (!displays) return unavailable("native display inventory is unavailable");
    const auto primary = std::find_if(displays.value().begin(), displays.value().end(),
                                      [](const auto& display) { return display.primary; });
    if (primary == displays.value().end()) return unavailable("primary display is unavailable");
    OwnedBackground background(*primary);
    background.setWindowTitle(QStringLiteral("MaiChat owned actions background"));
    background.show();
    const HWND backdrop = nativeHandle(background);
    // This preference belongs solely to our soon-destroyed HWND. It prevents
    // transparent rounded corners from admitting desktop pixels into evidence.
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_DONOTROUND;
    if (FAILED(DwmSetWindowAttribute(backdrop, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners))))
        return unavailable("owned background's non-rounded corner coverage cannot be confirmed");
    const bool backgroundPositioned = SetWindowPos(backdrop, HWND_TOPMOST, int(primary->x), int(primary->y),
                                                    int(primary->width), int(primary->height),
                                                    SWP_NOACTIVATE | SWP_SHOWWINDOW) != FALSE;
    const DWORD backgroundPositionError = backgroundPositioned ? 0 : GetLastError();
    OwnedDisplay owned{backdrop, *primary};

    QWidget main(nullptr, Qt::Window | Qt::WindowStaysOnTopHint);
    main.setWindowTitle(QStringLiteral("MaiChat owned computer actions %1").arg(GetCurrentProcessId()));
    main.resize(620, 420);
    main.setStyleSheet(QStringLiteral("QWidget { background: #f1f4f7; color: #142331; }"));
    QLabel caption(QStringLiteral("Owned native action fixture"), &main);
    caption.setGeometry(30, 20, 540, 30);
    int clicks = 0;
    const auto addButton = [&] {
        auto* button = new QPushButton(QStringLiteral("Owned action button"), &main);
        button->setAccessibleName(QStringLiteral("Owned action button"));
        button->setObjectName(QStringLiteral("ownedActionButton"));
        button->setGeometry(30, 75, 280, 55);
        QObject::connect(button, &QPushButton::clicked, &main, [&] { ++clicks; });
        button->show();
        return button;
    };
    auto* button = addButton();
    QPushButton anonymous(QString(), &main);
    anonymous.setGeometry(340, 75, 160, 55);
    QLabel anonymousCaption(QStringLiteral("Anonymous control above"), &main);
    anonymousCaption.setGeometry(340, 140, 220, 30);
    main.show();
    const HWND mainHandle = nativeHandle(main);
    const bool mainPositioned = SetWindowPos(mainHandle, HWND_TOPMOST, int(primary->x + 80), int(primary->y + 80),
                                              620, 420, SWP_SHOWWINDOW) != FALSE;
    const DWORD mainPositionError = mainPositioned ? 0 : GetLastError();
    main.activateWindow();
    pump(150);
    Json startupActivation{{"attempted", false},
                            {"foreground_before_window_id", std::to_string(windowId(GetForegroundWindow()))}};
    bool startupActivationDenied = false;
    if (owned.covered() && physicalInputReleased()) {
        DesktopNativeWindow activationTarget;
        activationTarget.id = windowId(mainHandle);
        activationTarget.processId = GetCurrentProcessId();
        // Qt can report active before the real foreground changes. Make one
        // normal production request on a worker while the owning GUI pumps;
        // refusal is preserved, with no input-queue attachment or policy change.
        startupActivation["attempted"] = true;
        const auto activation = worker([&] {
            return maiDesktopActivateWindow(activationTarget,
                                              [&] { return !owned.covered() || !physicalInputReleased(); });
        });
        startupActivationDenied = activation.hasError();
        startupActivation["error_code"] = maiErrorCodeToString(activation.code());
        startupActivation["error"] = activation.message();
        startupActivation["verified_exact_native_foreground"] = GetForegroundWindow() == mainHandle;
    }
    startupActivation["foreground_after_window_id"] = std::to_string(windowId(GetForegroundWindow()));
    bool manualSettingValid = false;
    const int requestedManualWait = qEnvironmentVariableIntValue("MAICHAT_COMPUTER_NATIVE_MANUAL_ACTIVATION_MS", &manualSettingValid);
    const int manualWait = manualSettingValid ? std::max(0, std::min(60000, requestedManualWait)) : 0;
    startupActivation["configured_manual_wait_ms"] = manualWait;
    startupActivation["continuation_source"] = "normal_native_api";
    if (startupActivationDenied && startupActivation.value("error_code", std::string()) == "busy" && manualWait > 0) {
        const auto manual = awaitManualActivation(main, caption, owned, manualWait);
        startupActivation["manual_activation"] = manual;
        if (manual.value("activated", false)) {
            startupActivationDenied = false;
            startupActivation["continuation_source"] = "manual";
            // A mistaken manual click on our button is separate from the
            // subsequent native-click oracle and is retained as setup evidence.
            startupActivation["manual_button_click_baseline"] = clicks;
            clicks = 0;
        }
    }
    if (startupActivationDenied || !owned.ready()) {
        auto diagnostic = owned.readinessDiagnostic();
        diagnostic["normal_native_activation"] = startupActivation;
        diagnostic["startup_failure"] = startupActivationDenied ? "normal_native_activation_refused" : "owned_readiness_guard";
        diagnostic["background_set_window_pos_ok"] = backgroundPositioned;
        diagnostic["background_set_window_pos_error"] = backgroundPositionError;
        diagnostic["main_set_window_pos_ok"] = mainPositioned;
        diagnostic["main_set_window_pos_error"] = mainPositionError;
        diagnostic["main_window_id"] = std::to_string(windowId(mainHandle));
        diagnostic["qt_main_active"] = main.isActiveWindow();
        diagnostic["qt_background_active"] = background.isActiveWindow();
        std::fprintf(stderr, "owned actions startup readiness: %s\n", diagnostic.dump().c_str());
        if (!evidence.isEmpty() && !writeJson(QDir(evidence).filePath(QStringLiteral("actions-native-readiness.json")), diagnostic))
            return 2;
        return unavailable(startupActivationDenied ? "normal Windows activation of the owned main window was denied"
                                                     : "owned foreground and complete display coverage were not established");
    }

    Workflow workflow(workspace.path(), owned, evidence);
    if (!workflow.tool) return 1;
    workflow.trace.push_back({{"stage", "startup-normal-native-activation"}, {"result", startupActivation}});
    const auto cleanup = qScopeGuard([&] {
        worker([&] { return workflow.controller->releaseInput(workflow.context.sessionId, workflow.context.turnId); });
        worker([&] { return workflow.controller->clear(workflow.context.sessionId); });
        worker([&] { return workflow.controller->clear(workflow.context.sessionId + "-evidence"); });
        worker([] { return maiStopScreenshotCapture(); });
        const QString receiptPath = QDir(evidence).filePath(QStringLiteral("actions-native-receipt.json"));
        if (!evidence.isEmpty() && !QFileInfo::exists(receiptPath)) {
            writeJson(receiptPath, {{"fixture", "owned-qt-uia-window-and-popup-actions"},
                                    {"checks", workflow.checks}, {"failures", workflow.failures},
                                    {"steps", workflow.trace}, {"early_exit", true},
                                    {"frame_checks", owned.frameChecks},
                                    {"real_model_used", false}});
        }
    });
    // Record the coverage time only after DWM has presented the owned desktop.
    // Repaint a different owned phase after this real wall-clock barrier, while
    // the native worker waits. Both phase frames are decoded/checked in memory;
    // no Controller or file writer sees these priming images.
    int verifiedPhases = 0;
    for (int phase = 1; phase <= 2; ++phase) {
        background.repaint();
        if (FAILED(DwmFlush()) || !owned.ready()) return unavailable("owned coverage could not be flushed before frame barrier");
        owned.coverageBarrier.store(MaiTime::getCurrentTime());
        owned.expectedPhase.store(phase);
        QTimer::singleShot(20, &background, [&, phase] {
            background.setPhase(phase);
            DwmFlush();
        });
        MaiScreenshot priming;
        const auto proof = worker([&] {
            return workflow.backend->captureDisplay(owned.display, priming, DesktopComputerBackend::CaptureOptions{});
        });
        workflow.check(!proof && !priming.pngBytes.empty(), "fresh owned background phase passes the in-memory pixel gate");
        if (proof || priming.pngBytes.empty()) return 1;
        ++verifiedPhases;
        priming = {};
    }
    workflow.trace.push_back({{"stage", "owned-background-frame-proof"}, {"verified_phases", verifiedPhases},
                              {"coverage_barrier_ms", owned.coverageBarrier.load()}});
    const auto inventory = worker([&] { return workflow.controller->windows(); });
    if (!inventory) return unavailable("native window inventory is unavailable");
    const auto selected = std::find_if(inventory.value().begin(), inventory.value().end(), [&](const auto& window) {
        return window.id == windowId(mainHandle) && window.processId == GetCurrentProcessId();
    });
    if (selected == inventory.value().end()) return unavailable("exact owned main HWND/PID is unavailable");
    const DesktopNativeWindow target = *selected;
    const auto inspected = worker([&] { return maiDesktopInspectElements(target); });
    if (!inspected || inspected.value().status == "unavailable") return unavailable("owned Qt UIA provider unavailable");
    workflow.check(inspected.value().countsKnown && inspected.value().visitedNodes > 0,
                   "native UIA returns known traversal counts");
    workflow.check(std::any_of(inspected.value().elements.begin(), inspected.value().elements.end(), [](const auto& item) {
        return item.role == "button" && item.label.empty() && !item.nativeId.empty();
    }), "label-less owned control retains its native identity");
    workflow.trace.push_back({{"stage", "native-inspection"}, {"provider", inspected.value().provider},
                              {"status", inspected.value().status}, {"visited", inspected.value().visitedNodes},
                              {"returned", inspected.value().elements.size()}, {"truncated", inspected.value().truncated},
                              {"limit_reason", inspected.value().limitReason}});

    const auto preserve = [&](const char* stage) {
        if (!owned.ready()) return false;
        const auto observed = worker([&] {
            return workflow.controller->observe(workflow.context.sessionId + "-evidence", "", workflow.context.root,
                                                  false, target.id, true, [&] { return !owned.ready(); });
        });
        if (!observed || !owned.ready()) {
            if (observed && !observed.value().imagePath.empty()) QFile::remove(QString::fromStdString(observed.value().imagePath));
            return false;
        }
        if (evidence.isEmpty()) return true;
        return QFile::copy(QString::fromStdString(observed.value().imagePath),
                           QDir(evidence).filePath(QString::fromLatin1(stage) + QStringLiteral(".png")));
    };
    workflow.check(preserve("owned-main-before"), "production window capture retains owned pixels");
    auto snapshot = workflow.observe(target.id, "before-id-click");
    auto element = Workflow::element(snapshot, "Owned action button");
    workflow.check(!element.empty(), "tool observation exposes a snapshot-bound native control ID");
    if (element.empty() || !owned.ready()) return 1;
    const auto firstRequest = Workflow::click(target.id, snapshot, element);
    const auto clicked = workflow.call(firstRequest, "id-click");
    pump();
    workflow.check(!clicked.hasError() && clicks == 1, "native element ID click reaches exactly one owned button");
    const auto stale = workflow.call(firstRequest, "stale-id");
    pump();
    workflow.check(stale.hasError() && clicks == 1, "consumed snapshot cannot repeat an element click");

    snapshot = workflow.observe(target.id, "before-layout-change");
    element = Workflow::element(snapshot, "Owned action button");
    button->move(button->x() + 60, button->y() + 70);
    pump();
    const auto moved = workflow.call(Workflow::click(target.id, snapshot, element), "moved-element");
    workflow.check(moved.hasError() && clicks == 1, "changed native element bounds reject input");
    snapshot = workflow.observe(target.id, "before-control-destruction");
    element = Workflow::element(snapshot, "Owned action button");
    delete button;
    button = nullptr;
    pump();
    const auto missing = workflow.call(Workflow::click(target.id, snapshot, element), "destroyed-element");
    workflow.check(missing.hasError() && clicks == 1, "destroyed native element rejects input");
    button = addButton();
    pump();

    snapshot = workflow.observe(target.id, "before-minimize");
    DesktopComputerAction minimize;
    minimize.action = "minimize";
    minimize.windowId = target.id;
    minimize.snapshotId = snapshot.value("snapshot_id", std::string());
    minimize.turnId = workflow.context.turnId;
    const auto minimized = worker([&] { return workflow.controller->perform(workflow.context.sessionId, minimize); });
    pump();
    workflow.check(bool(minimized) && IsIconic(mainHandle), "ordinary controller verifies native minimize state");
    const auto oldState = worker([&] { return workflow.controller->perform(workflow.context.sessionId, minimize); });
    workflow.check(!oldState, "window state action consumes its prior observation token");
    const auto metadata = workflow.observe(target.id, "minimized-metadata");
    workflow.check(metadata.value("minimized", false) && !metadata.value("has_image", true),
                   "minimized observation is state-only without a screenshot");
    const auto restored = workflow.call({{"action", "restore"}, {"target", "window"},
                                         {"window_id", std::to_string(target.id)},
                                         {"snapshot_id", metadata.value("snapshot_id", std::string())}}, "restore");
    pump();
    workflow.check(!restored.hasError() && !IsIconic(mainHandle), "tool restores exact owned HWND without a focus workaround");
    const auto canceledState = worker([&] {
        return maiDesktopSetWindowState(target, DesktopWindowStateAction::Minimize, [] { return true; });
    });
    workflow.check(canceledState.code() == MaiErrorCode::Canceled && !IsIconic(mainHandle),
                   "pre-canceled native state operation sends no minimize request");
    const auto canceledInspect = worker([&] { return maiDesktopInspectElements(target, [] { return true; }); });
    workflow.check(!canceledInspect && canceledInspect.error().code() == MaiErrorCode::Canceled,
                   "native accessibility inspection honors cancellation");

    unsigned queuedChecks = 0;
    const auto queuedCancel = worker([&] {
        return maiDesktopSetWindowState(target, DesktopWindowStateAction::Minimize,
                                         [&] { return ++queuedChecks >= 2; });
    });
    pump(60);
    workflow.check(queuedCancel.code() == MaiErrorCode::Canceled && queuedChecks == 2,
                   "native cancellation after successful queueing reports Canceled");
    workflow.trace.push_back({{"stage", "native-cancel-after-queue"}, {"cancellation_checks", queuedChecks},
                              {"minimized_after_gui_pump", bool(IsIconic(mainHandle))}});
    const auto recoveredNative = worker([&] { return maiDesktopSetWindowState(target, DesktopWindowStateAction::Restore); });
    pump();
    workflow.check(!recoveredNative && !IsIconic(mainHandle), "owned fixture recovers actual state after queued native cancellation");

    snapshot = workflow.observe(target.id, "before-controller-queued-cancel");
    DesktopComputerAction queued = minimize;
    queued.snapshotId = snapshot.value("snapshot_id", std::string());
    workflow.backend->cancelNextQueuedState.store(true);
    DesktopComputerActionProgress queuedProgress;
    const auto controllerCanceled = worker([&] {
        return workflow.controller->perform(workflow.context.sessionId, queued, &queuedProgress);
    });
    pump(60);
    workflow.check(!controllerCanceled && controllerCanceled.error().code() == MaiErrorCode::Canceled &&
                       workflow.backend->queuedStateCancellationChecks.load() == 2 &&
                       queuedProgress.windowOperationAttempted && queuedProgress.needsObservation &&
                       !queuedProgress.inputDispatchAttempted,
                   "queued cancellation retains window-attempt and fresh-observation facts without claiming input dispatch");
    workflow.trace.push_back({{"stage", "controller-cancel-after-queue"},
                              {"window_operation_attempted", queuedProgress.windowOperationAttempted},
                              {"input_dispatch_attempted", queuedProgress.inputDispatchAttempted},
                              {"needs_observation", queuedProgress.needsObservation},
                              {"minimized_after_gui_pump", bool(IsIconic(mainHandle))}});
    const auto recoveredController = worker([&] { return maiDesktopSetWindowState(target, DesktopWindowStateAction::Restore); });
    pump();
    workflow.check(!recoveredController && !IsIconic(mainHandle), "owned fixture recovers actual state after controller cancellation");

    // Restore intentionally need not focus. This ordinary focus request can be
    // denied by Windows; it never attaches input queues or changes OS policy.
    const auto focused = worker([&] { return maiDesktopActivateWindow(target); });
    if (focused || !owned.ready()) {
        workflow.check(false, "owned main window regains ordinary foreground before popup input");
    } else {
        int menuClicks = 0;
        QMenu menu;
        menu.addAction(QStringLiteral("Owned popup action"), [&] { ++menuClicks; });
        menu.popup(main.mapToGlobal(QPoint(80, 240)));
        pump(100);
        const HWND menuHandle = nativeHandle(menu);
        const auto popupInventory = worker([&] { return workflow.controller->windows(); });
        bool discovered = false;
        if (popupInventory)
            for (const auto& window : popupInventory.value())
                discovered = discovered || (window.id == windowId(menuHandle) && window.processId == target.processId);
        workflow.check(discovered && menuHandle != mainHandle, "separate owned Qt popup is discoverable by native ID");
        if (discovered && owned.ready()) {
            const auto popupObserved = workflow.call({{"action", "observe"}, {"target", "display"},
                                                       {"display_id", std::to_string(primary->id)},
                                                       {"element_window_id", std::to_string(windowId(menuHandle))}}, "popup-display-observe");
            workflow.check(!popupObserved.hasError() && owned.ready(), "display observation inspects the selected owned popup");
            if (!popupObserved.hasError() && owned.ready()) {
                const auto popupSnapshot = Json::parse(popupObserved.output(), nullptr, false);
                const auto popupElement = Workflow::element(popupSnapshot, "Owned popup action");
                workflow.check(!popupElement.empty(), "popup item has a real UIA-backed element ID");
                if (!popupElement.empty()) {
                    const auto popupClick = workflow.call({{"action", "click"}, {"target", "display"},
                                                            {"display_id", std::to_string(primary->id)},
                                                            {"snapshot_id", popupSnapshot.value("snapshot_id", std::string())},
                                                            {"element_id", popupElement}}, "popup-display-id-click");
                    pump();
                    const auto output = popupClick.hasError() ? Json::object() : Json::parse(popupClick.output(), nullptr, false);
                    workflow.check(!popupClick.hasError() && output.value("dispatched", false) && menuClicks == 1,
                                   "display element ID uses native click and retains its dispatch receipt after popup closes");
                }
            }
        }
        menu.close();
    }
    workflow.check(preserve("owned-main-after"), "final production capture retains owned pixels");
    const auto released = worker([&] {
        return workflow.controller->releaseInput(workflow.context.sessionId, workflow.context.turnId);
    });
    workflow.check(!released && workflow.controller->inputState(workflow.context.sessionId).empty(),
                   "owned synthetic input is released at completion");
    const Json receipt{{"fixture", "owned-qt-uia-window-and-popup-actions"}, {"pid", GetCurrentProcessId()},
                        {"main_window_id", std::to_string(target.id)}, {"display_id", std::to_string(primary->id)},
                        {"checks", workflow.checks}, {"failures", workflow.failures}, {"steps", workflow.trace},
                        {"input_attempted", workflow.attemptedInput}, {"verified_background_phases", verifiedPhases},
                        {"coverage_barrier_ms", owned.coverageBarrier.load()}, {"frame_checks", owned.frameChecks},
                        {"real_model_used", false}, {"taskbar_modified", false}};
    if (!evidence.isEmpty() && !writeJson(QDir(evidence).filePath(QStringLiteral("actions-native-receipt.json")), receipt))
        return 2;
    std::printf("Owned computer actions: checks=%d failures=%d\n", workflow.checks, workflow.failures);
    return workflow.failures ? 1 : 0;
}
#endif
}  // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
#if defined(_WIN32)
    if (application.arguments().contains(QStringLiteral("--native-state-only"))) return runNativeStateOnly();
    return run();
#else
    return unavailable("Windows native fixture only");
#endif
}
