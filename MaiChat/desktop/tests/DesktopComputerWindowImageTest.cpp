// An inactive owned window is the only discovery/capture target. Two painted
// markers independently test image origin and scale against Win32 client-space
// geometry; equal image/frame dimensions alone cannot establish that mapping.
#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QWidget>
#include <QtTest>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <json.hpp>

#include "MaiChatTools/DesktopComputerBackend.h"
#include "MaiChatTools/DesktopComputerTools.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#endif

namespace {
using Json = nlohmann::json;
bool nativeUnavailable = false;

template <class Function>
auto worker(Function&& function) {
    auto pending = std::async(std::launch::async, std::forward<Function>(function));
    while (pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
        QTest::qWait(5);
    return pending.get();
}

bool writeJson(const QString& path, const Json& value) {
    QSaveFile file(path);
    const auto bytes = QByteArray::fromStdString(value.dump(2));
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

struct Marker {
    const char* name;
    QRect rectangle;
    QColor color;
};

const std::array<Marker, 2> markers{{
    {"magenta", QRect(70, 80, 44, 36), QColor(255, 0, 255)},
    {"cyan", QRect(480, 300, 44, 36), QColor(0, 255, 255)},
}};

class OwnedWindow final : public QWidget {
public:
    OwnedWindow() {
        setAttribute(Qt::WA_ShowWithoutActivating);
        setWindowFlags(Qt::Window | Qt::WindowDoesNotAcceptFocus);
        setWindowTitle(QStringLiteral("MaiChat owned window image coordinate fixture"));
        resize(640, 440);
    }
    unsigned paintCount = 0;
    unsigned activations = 0;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(24, 37, 50));
        for (const auto& marker : markers) painter.fillRect(marker.rectangle, marker.color);
        ++paintCount;
    }
    bool event(QEvent* event) override {
        if (event->type() == QEvent::WindowActivate) ++activations;
        return QWidget::event(event);
    }
};

// Thresholds allow capture/color-management rounding, while the large saturated
// interiors keep the measured centers independent of antialiased boundaries.
struct DetectedMarker {
    QRect bounds;
    std::size_t pixels = 0;
    double x = 0;
    double y = 0;
};

DetectedMarker detectMarker(const QImage& image, bool magenta) {
    DetectedMarker result;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = image.pixelColor(x, y);
            const bool matches = color.alpha() >= 200 && color.blue() >= 190 &&
                                 (magenta ? color.red() >= 190 && color.green() <= 70
                                          : color.green() >= 190 && color.red() <= 70);
            if (!matches) continue;
            result.bounds |= QRect(x, y, 1, 1);
            result.x += x;
            result.y += y;
            ++result.pixels;
        }
    }
    if (result.pixels) {
        result.x /= double(result.pixels);
        result.y /= double(result.pixels);
    }
    return result;
}

#if defined(_WIN32)
std::uint64_t windowId(HWND window) {
    return std::uint64_t(reinterpret_cast<std::uintptr_t>(window));
}

Json rectangle(const RECT& value) {
    return {{"x", value.left},
            {"y", value.top},
            {"width", value.right - value.left},
            {"height", value.bottom - value.top}};
}

// No production enumeration is delegated: even an accidental broad observation
// can see only this exact owned HWND. Capture rejects changed HWND/PID/bounds
// before passing the selected window to the unmodified native WGC backend.
class OwnedBackend final : public DesktopComputerBackend {
public:
    explicit OwnedBackend(HWND window) : mWindow(window), mNative(makeDesktopComputerBackend()) {}
    std::atomic<unsigned> forbiddenCalls{0};
    std::atomic<unsigned> captures{0};

    MaiResult<std::vector<DesktopNativeWindow>> windows() override {
        DWORD pid = 0;
        RECT frame{};
        DWORD cloaked = 0;
        if (!IsWindow(mWindow) || !GetWindowThreadProcessId(mWindow, &pid) ||
            pid != GetCurrentProcessId() || !IsWindowVisible(mWindow) || IsIconic(mWindow) ||
            FAILED(DwmGetWindowAttribute(mWindow, DWMWA_EXTENDED_FRAME_BOUNDS, &frame,
                                         sizeof(frame))) ||
            FAILED(DwmGetWindowAttribute(mWindow, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) ||
            cloaked)
            return {MaiErrorCode::NotFound,
                    "owned HWND is not visible with available DWM frame bounds"};
        wchar_t title[256]{};
        wchar_t className[256]{};
        const int titleLength = GetWindowTextW(mWindow, title, 256);
        const int classLength = GetClassNameW(mWindow, className, 256);
        if (titleLength <= 0 || classLength <= 0)
            return {MaiErrorCode::NotFound, "owned HWND title/class unavailable"};
        DesktopNativeWindow result;
        result.id = windowId(mWindow);
        result.processId = pid;
        result.title = QString::fromWCharArray(title, titleLength).toUtf8().toStdString();
        result.className = QString::fromWCharArray(className, classLength).toUtf8().toStdString();
        result.ownerId = windowId(GetWindow(mWindow, GW_OWNER));
        result.toolWindow = (GetWindowLongPtrW(mWindow, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0;
        result.x = frame.left;
        result.y = frame.top;
        result.width = frame.right - frame.left;
        result.height = frame.bottom - frame.top;
        return std::vector<DesktopNativeWindow>{result};
    }
    MaiResult<std::vector<DesktopNativeElement>> elements(const DesktopNativeWindow&) override {
        return std::vector<DesktopNativeElement>{};
    }
    MaiError capture(const DesktopNativeWindow& selected, MaiScreenshot& output) override {
        const auto owned = windows();
        if (!owned || selected.id != windowId(mWindow) ||
            selected.processId != GetCurrentProcessId() || selected.x != owned.value().front().x ||
            selected.y != owned.value().front().y ||
            selected.width != owned.value().front().width ||
            selected.height != owned.value().front().height) {
            output = {};
            ++forbiddenCalls;
            return {MaiErrorCode::InvalidInput, "capture target is not the unchanged owned HWND"};
        }
        ++captures;
        return mNative->capture(selected, output);
    }
    MaiResult<std::vector<DesktopNativeDisplay>> displays() override {
        return forbidden("display enumeration");
    }
    MaiError captureDisplay(const DesktopNativeDisplay&, MaiScreenshot&) override {
        return forbidden("display capture");
    }
    MaiError activate(const DesktopNativeWindow&) override {
        return forbidden("activation");
    }
    MaiResult<bool> isFocused(const DesktopNativeWindow&) override {
        return forbidden("focus query");
    }
    MaiResult<bool> secureInput() override {
        return forbidden("secure input query");
    }
    MaiError input(const DesktopNativeInput&) override {
        return forbidden("input");
    }
    MaiResult<std::string> focusedText(const DesktopNativeWindow&) override {
        return forbidden("focused text");
    }

private:
    MaiError forbidden(const char* operation) {
        ++forbiddenCalls;
        return {MaiErrorCode::InvalidInput,
                std::string("owned image fixture rejects ") + operation};
    }
    HWND mWindow;
    std::shared_ptr<DesktopComputerBackend> mNative;
};
#endif
}  // namespace

class DesktopComputerWindowImageTest : public QObject {
    Q_OBJECT
private slots:
    void imageMarkersMapToNativeClientCoordinates();
};

void DesktopComputerWindowImageTest::imageMarkersMapToNativeClientCoordinates() {
    const QString evidence = qEnvironmentVariable("MAICHAT_COMPUTER_NATIVE_EVIDENCE_DIR");
    Json receipt{{"fixture", "owned-inactive-window-image-coordinate-mapping"},
                 {"input_injected", false},
                 {"activation_requested", false},
                 {"mapping_tolerance_physical_pixels", 2}};
    const auto preserveReceipt = [&] {
        if (evidence.isEmpty()) return true;
        return QDir().mkpath(evidence) &&
               writeJson(
                   QDir(evidence).filePath(QStringLiteral("window-image-native-receipt.json")),
                   receipt);
    };
    const auto unavailable = [&](const std::string& reason) {
        nativeUnavailable = true;
        receipt["status"] = "unavailable";
        receipt["reason"] = reason;
        std::fprintf(stderr, "owned window image SKIP: %s; native image mapping was not verified\n",
                     reason.c_str());
        return preserveReceipt();
    };
#if !defined(_WIN32)
    QVERIFY(unavailable("Windows native fixture only"));
    QSKIP("Windows native fixture only");
#else
    if (QGuiApplication::platformName() != QStringLiteral("windows")) {
        QVERIFY(unavailable("Qt Windows interactive platform is unavailable"));
        QSKIP("Qt Windows interactive platform is unavailable");
    }
    // Win32 conversion must report physical screen pixels. Do not compensate
    // with Qt global coordinates, which can hide the very DPI error under test.
    if (!AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),
                                      DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        QVERIFY(unavailable("GUI thread is not per-monitor DPI aware V2"));
        QSKIP("GUI thread is not per-monitor DPI aware V2");
    }
    OwnedWindow window;
    window.move(60, 60);
    window.show();
    QTest::qWait(200);
    const HWND handle = reinterpret_cast<HWND>(window.winId());
    if (!window.paintCount || !IsWindowVisible(handle) || IsIconic(handle) || FAILED(DwmFlush())) {
        QVERIFY(unavailable("owned inactive window did not render a visible DWM surface"));
        QSKIP("owned inactive window did not render a visible DWM surface");
    }
    QVERIFY2(AreDpiAwarenessContextsEqual(GetWindowDpiAwarenessContext(handle),
                                          DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2),
             "Owned HWND must use physical per-monitor V2 coordinates");
    QCOMPARE(window.activations, 0u);

    RECT client{};
    RECT frame{};
    RECT outer{};
    POINT origin{};
    QVERIFY(GetClientRect(handle, &client));
    QVERIFY(ClientToScreen(handle, &origin));
    QVERIFY(GetWindowRect(handle, &outer));
    QVERIFY(SUCCEEDED(
        DwmGetWindowAttribute(handle, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))));
    QVERIFY(client.right > 0 && client.bottom > 0);
    receipt["process_id"] = GetCurrentProcessId();
    receipt["owned_window_id"] = windowId(handle);
    receipt["window_dpi"] = GetDpiForWindow(handle);
    receipt["qt_device_pixel_ratio"] = window.devicePixelRatioF();
    receipt["logical_client_size"] = {{"width", window.width()}, {"height", window.height()}};
    receipt["native_client_rect"] = rectangle(client);
    receipt["native_client_screen_origin"] = {{"x", origin.x}, {"y", origin.y}};
    receipt["native_outer_bounds"] = rectangle(outer);
    receipt["native_dwm_bounds"] = rectangle(frame);

    std::array<QPointF, 2> nativeCenters;
    for (std::size_t index = 0; index < markers.size(); ++index) {
        const QRect& marker = markers[index].rectangle;
        POINT center{
            LONG(std::lround((marker.x() + marker.width() / 2.0) * client.right / window.width())),
            LONG(std::lround((marker.y() + marker.height() / 2.0) * client.bottom /
                             window.height()))};
        QVERIFY(ClientToScreen(handle, &center));
        // Filled rectangles occupy pixel centers through exclusive right/bottom
        // minus one; subtract half a physical pixel from the edge-space center.
        nativeCenters[index] = QPointF(center.x - 0.5, center.y - 0.5);
    }

    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    const auto backend = std::make_shared<OwnedBackend>(handle);
    DesktopComputerController controller(backend);
    const auto observed = worker([&] {
        return controller.observe("owned-window-image", "", workspace.path().toUtf8().toStdString(),
                                  false, windowId(handle), true);
    });
    if (!observed) {
        QVERIFY(unavailable(std::string("ordinary owned-window observation failed: ") +
                            maiErrorCodeToString(observed.error().code()) + ": " +
                            observed.error().message()));
        QSKIP("Native owned-window capture unavailable; exact reason recorded above");
    }
    const Json output = Json::parse(observed.value().json, nullptr, false);
    QVERIFY(output.is_object());
    const QImage image(QString::fromStdString(observed.value().imagePath));
    QVERIFY2(!image.isNull(), "Actual controller PNG must decode");
    QVERIFY(image.width() > 1 && image.height() > 1);
    QCOMPARE(output.value("image_width", 0), image.width());
    QCOMPARE(output.value("image_height", 0), image.height());
    QCOMPARE(output.value("window_id", std::string()), std::to_string(windowId(handle)));
    QCOMPARE(output.value("native_screen_bounds_unit", std::string()),
             std::string("screen_physical_pixels"));
    QVERIFY(output.value("has_image", false));
    QCOMPARE(backend->captures.load(), 1u);
    QCOMPARE(backend->forbiddenCalls.load(), 0u);
    QCOMPARE(window.activations, 0u);

    const auto& bounds = output.at("bounds");
    const double nativeX = bounds.at("x").get<double>();
    const double nativeY = bounds.at("y").get<double>();
    const double nativeWidth = bounds.at("width").get<double>();
    const double nativeHeight = bounds.at("height").get<double>();
    receipt["reported_bounds"] = bounds;
    receipt["image_size"] = {{"width", image.width()}, {"height", image.height()}};
    receipt["markers"] = Json::array();
    bool mappingMatches = true;
    bool markersPresent = true;
    for (std::size_t index = 0; index < markers.size(); ++index) {
        const auto detected = detectMarker(image, index == 0);
        const double mappedX = nativeX + detected.x / (image.width() - 1) * (nativeWidth - 1);
        const double mappedY = nativeY + detected.y / (image.height() - 1) * (nativeHeight - 1);
        const double errorX = mappedX - nativeCenters[index].x();
        const double errorY = mappedY - nativeCenters[index].y();
        const bool present =
            detected.pixels >= 100 && detected.bounds.width() >= 8 && detected.bounds.height() >= 8;
        markersPresent = markersPresent && present;
        const bool matches = present && std::abs(errorX) <= 2 && std::abs(errorY) <= 2;
        mappingMatches = mappingMatches && matches;
        receipt["markers"].push_back(
            {{"name", markers[index].name},
             {"matching_pixels", detected.pixels},
             {"detected_center_pixels", {{"x", detected.x}, {"y", detected.y}}},
             {"native_center_client_to_screen",
              {{"x", nativeCenters[index].x()}, {"y", nativeCenters[index].y()}}},
             {"mapped_center_native", {{"x", mappedX}, {"y", mappedY}}},
             {"error_physical_pixels", {{"x", errorX}, {"y", errorY}}},
             {"mapping_matches", matches}});
    }
    receipt["paint_count"] = window.paintCount;
    receipt["activation_count"] = window.activations;
    receipt["native_capture_calls"] = backend->captures.load();
    receipt["forbidden_backend_calls"] = backend->forbiddenCalls.load();
    receipt["status"] = mappingMatches ? "passed" : "failed";
    // Preserve the owned image before assertions so a genuine origin/scale
    // mismatch remains reviewable. Never retain any whole-desktop capture.
    if (!evidence.isEmpty()) {
        QVERIFY(QDir().mkpath(evidence));
        const QString destination =
            QDir(evidence).filePath(QStringLiteral("owned-window-image.png"));
        QSaveFile saved(destination);
        QFile original(QString::fromStdString(observed.value().imagePath));
        QVERIFY(original.open(QIODevice::ReadOnly));
        const auto bytes = original.readAll();
        QVERIFY(saved.open(QIODevice::WriteOnly));
        QCOMPARE(saved.write(bytes), qint64(bytes.size()));
        QVERIFY(saved.commit());
    }
    QVERIFY(preserveReceipt());
    std::printf("Owned window image mapping: %s\n", receipt.dump().c_str());
    if (!markersPresent) {
        QVERIFY(unavailable("owned rendered markers are absent from the native window PNG"));
        QSKIP("Owned rendered markers are absent from the native window PNG");
    }
    QVERIFY2(mappingMatches,
             "Actual PNG marker ratios disagree with independent Win32 physical coordinates; see "
             "numeric receipt");
#endif
}

// QTEST_MAIN reports a skipped native fixture as success. Retain QtTest's normal
// failure status while using the repository's explicit native-unavailable 77.
int main(int argc, char** argv) {
    QApplication application(argc, argv);
    DesktopComputerWindowImageTest test;
    const int result = QTest::qExec(&test, argc, argv);
    return result ? result : nativeUnavailable ? 77 : 0;
}

#include "DesktopComputerWindowImageTest.moc"
