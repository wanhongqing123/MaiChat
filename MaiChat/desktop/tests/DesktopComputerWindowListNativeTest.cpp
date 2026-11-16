// Opt-in owned-window discovery regression. No input, focus activation,
// screenshot, UIA inspection or application body text is requested. Results
// are retained only for this fixture's exact HWNDs and PID.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QThread>
#include <QWidget>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "MaiChatTools/DesktopComputerTools.h"

namespace {
std::atomic<unsigned> nativeActivations{0};
LRESULT CALLBACK passiveProcedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_ACTIVATE && LOWORD(wp) != WA_INACTIVE) ++nativeActivations;
    return DefWindowProcW(window, message, wp, lp);
}
std::uint64_t id(HWND window) {
    return std::uint64_t(reinterpret_cast<std::uintptr_t>(window));
}
void pump() {
    QElapsedTimer elapsed;
    elapsed.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    } while (elapsed.elapsed() < 40);
}
std::string utf8(const std::wstring& value) {
    return QString::fromStdWString(value).toUtf8().toStdString();
}
std::string className(HWND window) {
    wchar_t value[256]{};
    const int length = GetClassNameW(window, value, int(std::size(value)));
    return QString::fromWCharArray(value, length).toUtf8().toStdString();
}
class PassiveQtWindow final : public QWidget {
public:
    PassiveQtWindow() {
        setAttribute(Qt::WA_ShowWithoutActivating);
        setWindowFlags(Qt::Window | Qt::WindowDoesNotAcceptFocus);
        setWindowTitle(QStringLiteral("MaiChat own Qt window list %1").arg(uint(GetCurrentProcessId())));
        resize(300, 200);
    }
    unsigned activations = 0;

protected:
    bool event(QEvent* event) override {
        if (event->type() == QEvent::WindowActivate) ++activations;
        return QWidget::event(event);
    }
};
struct Owned {
    std::string label;
    HWND window = nullptr;
    bool listed = true;
    bool minimized = false;
    bool tool = false;
    HWND owner = nullptr;
    std::string title;
    std::string className;
};
class Fixture {
public:
    Fixture() {
        windowClass_.lpfnWndProc = passiveProcedure;
        windowClass_.hInstance = GetModuleHandleW(nullptr);
        windowClass_.lpszClassName = L"MaiOwnedWindowListNative20261117";
        windowClass_.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassW(&windowClass_);
    }
    ~Fixture() {
        for (auto it = native_.rbegin(); it != native_.rend(); ++it)
            if (IsWindow(*it)) DestroyWindow(*it);
    }
    bool create() {
        foregroundBefore_ = GetForegroundWindow();
        qt_.move(40, 40);
        qt_.show();
        pump();
        add("qt_normal", reinterpret_cast<HWND>(qt_.winId()), true, false, false, nullptr,
            qt_.windowTitle().toUtf8().toStdString());
        const auto root = createNative("native_normal", WS_OVERLAPPEDWINDOW, 0, 80, 80, 320, 240);
        createNative("minimized", WS_OVERLAPPEDWINDOW, 0, 120, 100, 360, 240, nullptr, SW_SHOWMINNOACTIVE, true);
        const auto tiny = createNative("tiny_64x32", WS_POPUP | WS_BORDER, WS_EX_NOACTIVATE, 160, 120, 64, 32);
        createNative("untitled", WS_OVERLAPPEDWINDOW, WS_EX_NOACTIVATE, 200, 140, 320, 220, nullptr, SW_SHOWNOACTIVATE,
                     false, false, true);
        createNative("owned_dialog", WS_POPUP | WS_CAPTION | WS_SYSMENU, WS_EX_NOACTIVATE, 240, 160, 280, 180, root);
        createNative("owned_popup", WS_POPUP | WS_BORDER, WS_EX_NOACTIVATE, 280, 180, 260, 160, root);
        createNative("visible_tool", WS_POPUP | WS_CAPTION, WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, 320, 200, 260, 160,
                     root, SW_SHOWNOACTIVATE, false, true);
        createNative("hidden", WS_OVERLAPPEDWINDOW, WS_EX_NOACTIVATE, 360, 220, 320, 220, nullptr, -1, false, false,
                     false, false);
        createNative("visible_child", WS_CHILD | WS_BORDER, 0, 0, 0, 100, 80, root, SW_SHOWNOACTIVATE, false, false,
                     false, false);
        createNative("message_only", 0, 0, 0, 0, 100, 80, HWND_MESSAGE, -1, false, false, false, false);
        // Offscreen visible windows test discovery/capacity without covering
        // the user's desktop or adding 205 taskbar entries. Show never activates.
        const int x = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 1000;
        const int y = GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN) + 1000;
        for (unsigned index = 0; index < 205; ++index)
            createNative("offscreen_" + std::to_string(index), WS_POPUP | WS_BORDER,
                         WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, x + int(index % 10), y + int(index % 10), 90, 60, nullptr,
                         SW_SHOWNOACTIVATE, false, true);
        pump();
        bool valid = root && tiny && owned_.size() == 216;
        for (const auto& item : owned_) {
            DWORD pid = 0;
            GetWindowThreadProcessId(item.window, &pid);
            valid = valid && IsWindow(item.window) && pid == GetCurrentProcessId() &&
                    bool(IsIconic(item.window)) == item.minimized;
            if (item.listed) valid = valid && IsWindowVisible(item.window);
        }
        RECT rectangle{};
        valid = valid && GetWindowRect(tiny, &rectangle) && rectangle.right - rectangle.left < 80 &&
                rectangle.bottom - rectangle.top < 50;
        setup_.insert(QStringLiteral("tiny_actual_width"), int(rectangle.right - rectangle.left));
        setup_.insert(QStringLiteral("tiny_actual_height"), int(rectangle.bottom - rectangle.top));
        setup_.insert(QStringLiteral("created_count"), int(owned_.size()));
        setup_.insert(QStringLiteral("fixture_valid"), valid);
        return valid;
    }
    void run() {
        const auto ordinary = maiDesktopWindows();
        check("ordinary_enumeration_succeeded", bool(ordinary));
        if (ordinary) inspect("ordinary", ordinary.value());
        auto controller = std::make_shared<DesktopComputerController>();
        const auto projected = controller->windows();  // Same production path used by model action=list.
        check("production_controller_enumeration_succeeded", bool(projected));
        if (projected) inspect("controller", projected.value());
        check("enumeration_does_not_change_foreground", GetForegroundWindow() == foregroundBefore_);
        check("owned_creation_and_listing_never_activated", nativeActivations == 0 && qt_.activations == 0);
        for (const auto& item : owned_) {
            DWORD pid = 0;
            GetWindowThreadProcessId(item.window, &pid);
            check("own_hwnd_and_pid_survive_enumeration", IsWindow(item.window) && pid == GetCurrentProcessId(),
                  item.label);
            check("own_minimized_state_preserved", bool(IsIconic(item.window)) == item.minimized, item.label);
        }
    }
    int save(const QString& directory, bool ready) {
        const QJsonObject report{{"fixture", "sdk-free-owned-window-list"},
                                 {"pid", int(GetCurrentProcessId())},
                                 {"precondition_skip", !ready},
                                 {"setup", setup_},
                                 {"failures", failures_},
                                 {"checks", checks_},
                                 {"cases", cases_},
                                 {"owned_results", results_},
                                 {"input_sent", false},
                                 {"screenshots_taken", false},
                                 {"foreign_metadata_saved", false},
                                 {"foreground_unchanged", GetForegroundWindow() == foregroundBefore_},
                                 {"native_activations", int(nativeActivations.load())},
                                 {"qt_activations", int(qt_.activations)}};
        QSaveFile file(QDir(directory).filePath(QStringLiteral("window-list-native-receipt.json")));
        const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
        const bool saved = file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
        std::printf("Owned window list: checks=%d failures=%d precondition_skip=%d\n", checks_, failures_, !ready);
        return !saved ? 2 : !ready ? 77 : failures_ ? 1 : 0;
    }

private:
    void add(std::string label, HWND window, bool listed, bool minimized, bool tool, HWND owner, std::string title) {
        owned_.push_back(
            {std::move(label), window, listed, minimized, tool, owner, std::move(title), className(window)});
    }
    HWND createNative(const std::string& label, DWORD style, DWORD extended, int x, int y, int width, int height,
                      HWND owner = nullptr, int show = SW_SHOWNOACTIVATE, bool minimized = false, bool tool = false,
                      bool untitled = false, bool listed = true) {
        const std::wstring title = untitled ? std::wstring()
                                            : L"MaiChat owned list " + QString::fromStdString(label).toStdWString() +
                                                  L" PID " + std::to_wstring(GetCurrentProcessId());
        const auto window = CreateWindowExW(extended, windowClass_.lpszClassName, title.c_str(), style, x, y, width,
                                            height, owner, nullptr, windowClass_.hInstance, nullptr);
        native_.push_back(window);
        add(label, window, listed, minimized, tool, owner == HWND_MESSAGE || (style & WS_CHILD) ? nullptr : owner,
            utf8(title));
        if (window && show >= 0) ShowWindow(window, show);
        return window;
    }
    void check(const char* name, bool passed, const std::string& label = {}) {
        ++checks_;
        if (!passed) ++failures_;
        cases_.append(QJsonObject{
            {"name", QString::fromLatin1(name)}, {"owned_case", QString::fromStdString(label)}, {"passed", passed}});
        if (!passed) std::printf("FAIL %s owned=%s\n", name, label.c_str());
    }
    void inspect(const char* path, const std::vector<DesktopNativeWindow>& values) {
        // Never dump the desktop-wide return value. Keep only this fixture's
        // exact windows, then verify PID in addition to HWND against reuse.
        std::map<std::uint64_t, const DesktopNativeWindow*> found;
        for (const auto& value : values)
            if (value.processId == GetCurrentProcessId() &&
                std::any_of(owned_.begin(), owned_.end(),
                            [&](const auto& item) { return id(item.window) == value.id; }))
                found.emplace(value.id, &value);
        unsigned offscreenFound = 0;
        for (const auto& item : owned_) {
            const auto match = found.find(id(item.window));
            const bool present = match != found.end();
            check("owned_window_presence", present == item.listed, std::string(path) + ":" + item.label);
            if (!present) continue;
            const auto& value = *match->second;
            check("owned_pid_and_hwnd_exact", value.id == id(item.window) && value.processId == GetCurrentProcessId(),
                  item.label);
            check("actual_title_preserved_including_empty", value.title == item.title, item.label);
            check("class_name_preserved", value.className == item.className, item.label);
            check("minimized_metadata", value.minimized == item.minimized, item.label);
            check("tool_and_owner_metadata", value.toolWindow == item.tool && value.ownerId == id(item.owner),
                  item.label);
            if (item.minimized)
                check("minimized_is_not_fake_clickable_geometry",
                      !value.boundsAvailable && value.x == 0 && value.y == 0 && value.width == 0 && value.height == 0,
                      item.label);
            if (item.label.find("offscreen_") == 0) ++offscreenFound;
            results_.append(QJsonObject{{"path", QString::fromLatin1(path)},
                                        {"owned_case", QString::fromStdString(item.label)},
                                        {"window_id", QString::number(value.id)},
                                        {"process_id", int(value.processId)},
                                        {"title", QString::fromStdString(value.title)},
                                        {"class_name", QString::fromStdString(value.className)},
                                        {"minimized", value.minimized},
                                        {"cloaked", value.cloaked},
                                        {"bounds_available", value.boundsAvailable},
                                        {"tool_window", value.toolWindow},
                                        {"owner_window_id", QString::number(value.ownerId)}});
        }
        check("more_than_200_own_windows_not_silently_capped", offscreenFound == 205, path);
    }
    PassiveQtWindow qt_;
    WNDCLASSW windowClass_{};
    HWND foregroundBefore_ = nullptr;
    std::vector<HWND> native_;
    std::vector<Owned> owned_;
    QJsonObject setup_;
    QJsonArray cases_, results_;
    int checks_ = 0, failures_ = 0;
};
}  // namespace

int main(int argc, char** argv) {
    bool run = false;
    for (int i = 1; i < argc; ++i)
        if (qstrcmp(argv[i], "--run-owned-windows") == 0) run = true;
    if (!run) {
        std::puts("SKIP: --run-owned-windows --output-dir <owned directory> required");
        return 77;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    QApplication application(argc, argv);
    const auto args = application.arguments();
    const int output = args.indexOf(QStringLiteral("--output-dir"));
    if (output < 0 || output + 1 >= args.size()) return 2;
    const auto directory = args[output + 1];
    if (!QDir().mkpath(directory)) return 2;
    Fixture fixture;
    const bool ready = fixture.create();
    if (ready) fixture.run();
    return fixture.save(directory, ready);
}
#else
#include <cstdio>
int main() {
    std::puts("SKIP: Windows-only owned window discovery test");
    return 77;
}
#endif
