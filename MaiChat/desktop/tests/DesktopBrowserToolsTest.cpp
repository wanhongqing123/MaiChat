#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <tlhelp32.h>
#endif

#include <httplib.h>
#include <json.hpp>

#include "MaiChatTools/DesktopBrowserTools.h"
#include "MaiThread.h"
#include "MaiTool.h"

namespace {

#define CHECK(value)                                                                       \
    do {                                                                                   \
        if (!(value)) {                                                                    \
            std::fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, #value); \
            return 1;                                                                      \
        }                                                                                  \
    } while (false)

struct FixtureServer {
    httplib::Server server;
    std::thread worker;
    int port = 0;
    std::atomic<bool> releaseLoading{false};

    FixtureServer() {
        // 使用同一份 vendored HTTP fixture；浏览器测试不依赖 Qt Network。
        server.Get("/page", [](const httplib::Request&, httplib::Response& response) {
            response.set_content(R"HTML(<!doctype html><html><head><meta name="viewport"
                content="width=device-width,initial-scale=1"><title>Agent Browser Fixture</title></head>
                <body style="background:white;font-size:22px"><h1>Agent Browser Fixture</h1>
                <input aria-label="Note" id="note"><input aria-label="Password" type="password">
                <button aria-label="Run" onclick="document.getElementById('result').textContent=
                'clicked:'+document.getElementById('note').value">Run</button>
                <p id="result">idle</p>
                <button aria-label="Deferred disable" id="deferred-disable"
                  onclick="document.getElementById('result').textContent='unsafe disabled click'">Guard A</button>
                <button aria-label="Deferred label" id="deferred-label"
                  onclick="document.getElementById('result').textContent='unsafe label click'">Guard B</button>
                <button aria-label="Below fold" style="display:block;margin-top:1600px"
                  onclick="document.getElementById('result').textContent='below fold clicked'">Below fold</button>
                <button aria-label="Hidden" style="display:none" onclick="alert('unsafe')">Hidden</button>
                <script>
                  const a=document.getElementById('deferred-disable');
                  a.scrollIntoView=()=>setTimeout(()=>a.setAttribute('aria-disabled','true'),0);
                  const b=document.getElementById('deferred-label');
                  b.scrollIntoView=()=>setTimeout(()=>b.setAttribute('aria-label','Changed label'),0);
                </script></body></html>)HTML",
                                 "text/html; charset=utf-8");
        });
        // 模拟原生遮挡产生的 hidden 标志；用于精确复现门禁，不冒称系统遮挡测试。
        server.Get("/hidden", [](const httplib::Request&, httplib::Response& response) {
            response.set_content(R"(<!doctype html><html><head><title>Hidden Fixture</title>
                <script>Object.defineProperty(document,'visibilityState',{get:()=> 'hidden'});</script>
                </head><body style="background:#d9f2ff;font-size:24px">
                <h1>Hidden Fixture</h1><input aria-label="Note"><button aria-label="Run"
                onclick="document.getElementById('result').textContent='clicked'">Run</button>
                <p id="result">idle</p></body></html>)",
                                 "text/html; charset=utf-8");
        });
        server.Get("/loading", [this](const httplib::Request&, httplib::Response& response) {
            response.set_chunked_content_provider(
                "text/html; charset=utf-8", [this](std::size_t offset, httplib::DataSink& sink) {
                    if (offset == 0) {
                        const std::string prefix = "<!doctype html><title>Loading Fixture</title><body>Still loading";
                        return sink.write(prefix.data(), prefix.size());
                    }
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                    while (!releaseLoading && std::chrono::steady_clock::now() < deadline) QThread::msleep(10);
                    sink.write("</body>", 7);
                    sink.done();
                    return true;
                });
        });
        port = server.bind_to_any_port("127.0.0.1");
        if (port > 0)
            worker = std::thread([this] {
                MaiThread::setCurrentName("mai-browser-http");
                server.listen_after_bind();
            });
    }
    ~FixtureServer() {
        releaseLoading = true;
        server.stop();
        if (worker.joinable()) worker.join();
    }
};

std::string element(const nlohmann::json& snapshot, const std::string& label) {
    for (const auto& value : snapshot["elements"])
        if (value.value("label", "") == label) return value["id"].get<std::string>();
    return {};
}

#ifdef Q_OS_WIN
// 只定位本测试新建的浏览器主进程窗口；不按标题猜用户窗口，不操作现有浏览器。
HWND ownedBrowserWindow() {
    const auto snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return nullptr;
    std::vector<DWORD> children;
    PROCESSENTRY32W process{};
    process.dwSize = sizeof(process);
    if (::Process32FirstW(snapshot, &process)) {
        do {
            if (process.th32ParentProcessID == ::GetCurrentProcessId() &&
                (_wcsicmp(process.szExeFile, L"msedge.exe") == 0 || _wcsicmp(process.szExeFile, L"chrome.exe") == 0))
                children.push_back(process.th32ProcessID);
        } while (::Process32NextW(snapshot, &process));
    }
    ::CloseHandle(snapshot);
    struct Windows {
        const std::vector<DWORD>* children;
        HWND found = nullptr;
    } windows{&children};
    ::EnumWindows(
        [](HWND window, LPARAM opaque) -> BOOL {
            auto& state = *reinterpret_cast<Windows*>(opaque);
            DWORD pid = 0;
            ::GetWindowThreadProcessId(window, &pid);
            if (::IsWindowVisible(window) &&
                std::find(state.children->begin(), state.children->end(), pid) != state.children->end()) {
                state.found = window;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&windows));
    return windows.found;
}
#endif

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    FixtureServer server;
    const int port = server.port;
    CHECK(port > 0);
    const bool hiddenCase = argc == 2 && std::string(argv[1]) == "--hidden";
    const std::string url = "http://127.0.0.1:" + std::to_string(port) + (hiddenCase ? "/hidden" : "/page");
    QTemporaryDir workspace;
    CHECK(workspace.isValid());
    auto manager = std::make_shared<DesktopBrowserManager>();
    MaiToolRegistry registry;
    registerDesktopBrowserTools(registry, manager);
    MaiTool* tool = registry.find("desktop_browser");
    CHECK(tool != nullptr);
    CHECK(nlohmann::json::parse(tool->parametersSchema()).is_object());
    if (!hiddenCase) {
        const auto schema = nlohmann::json::parse(tool->parametersSchema());
        CHECK(schema["properties"].contains("mode"));
        CHECK(schema["properties"].contains("wait_ms"));
    }
    CHECK(tool->requiresPerCallApproval(R"({"action":"open"})"));
    CHECK(tool->requiresPerCallApproval(R"({"action":"click"})"));
    CHECK(!tool->requiresApproval(R"({"action":"observe"})"));

    // A canceled request must not launch a profile or acquire a browser Session.
    CHECK(manager->open("canceled-owner", url, [] { return true; }).code() == MaiErrorCode::Canceled);
    CHECK(manager->observe("canceled-owner").error().code() == MaiErrorCode::NotFound);

    const MaiError opened = manager->open("owner", url);
    if (opened) std::fprintf(stderr, "open failed: %s\n", opened.message().c_str());
    CHECK(!opened);
    CHECK(manager->observe("owner", [] { return true; }).error().code() == MaiErrorCode::Canceled);
    CHECK(manager->observe("other").error().code() == MaiErrorCode::NotFound);
    nlohmann::json first;
    for (int attempt = 0; attempt < 60; ++attempt) {
        const auto observed = manager->observe("owner");
        if (!observed && hiddenCase) {
            std::fprintf(stderr, "hidden observation failed: %s\n", observed.error().message().c_str());
            CHECK(observed);
        }
        if (observed) {
            const auto value = nlohmann::json::parse(observed.value());
            if (value.value("state", "") == "ready" && value.value("text", "").find("idle") != std::string::npos) {
                first = value;
                break;
            }
        }
        QThread::msleep(100);
    }
    CHECK(first.is_object());
    CHECK(first["url"] == url);
    if (hiddenCase) {
        CHECK(first["visibility"] == "hidden");
        const auto image = workspace.filePath(QStringLiteral("hidden.png")).toUtf8().toStdString();
        CHECK(!manager->screenshot("owner", url, image));
        const QString copy = qEnvironmentVariable("MAICHAT_BROWSER_TEST_SCREENSHOT");
        if (!copy.isEmpty()) CHECK(QFile::copy(QString::fromStdString(image), copy));
        const auto note = element(first, "Note");
        CHECK(!note.empty());
        CHECK(!manager->act("owner", "type", url, first["snapshot_id"], note, "Note", "hidden OK"));
        const auto observed = manager->observe("owner");
        CHECK(observed);
        const auto next = nlohmann::json::parse(observed.value());
        const auto button = element(next, "Run");
        CHECK(!button.empty());
        CHECK(!manager->act("owner", "click", url, next["snapshot_id"], button, "Run", ""));
        CHECK(!manager->close("owner"));
        return 0;
    }
    const std::string note = element(first, "Note");
    const std::string password = element(first, "Password");
    CHECK(!note.empty() && !password.empty());
    CHECK(manager->act("owner", "type", url, first["snapshot_id"], note, "Note", "canceled text", [] { return true; })
              .code() == MaiErrorCode::Canceled);
    CHECK(manager->act("owner", "type", url, first["snapshot_id"], password, "Password", "secret").code() ==
          MaiErrorCode::InvalidInput);
    CHECK(!manager->act("owner", "type", url, first["snapshot_id"], note, "Note", "desktop OK"));
    CHECK(manager->act("owner", "type", url, first["snapshot_id"], note, "Note", "stale").code() ==
          MaiErrorCode::InvalidInput);
    const auto second = manager->observe("owner");
    CHECK(second);
    const auto next = nlohmann::json::parse(second.value());
    const std::string button = element(next, "Run");
    CHECK(!button.empty());
    CHECK(!manager->act("owner", "click", url, next["snapshot_id"], button, "Run", ""));
    bool clicked = false;
    for (int attempt = 0; attempt < 30; ++attempt) {
        const auto observed = manager->observe("owner");
        if (observed && observed.value().find("clicked:desktop OK") != std::string::npos) {
            clicked = true;
            break;
        }
        QThread::msleep(50);
    }
    CHECK(clicked);
    for (const auto* label : {"Deferred disable", "Deferred label"}) {
        const auto observed = manager->observe("owner");
        CHECK(observed);
        const auto snapshot = nlohmann::json::parse(observed.value());
        CHECK(element(snapshot, "Hidden").empty());
        const auto id = element(snapshot, label);
        CHECK(!id.empty());
        CHECK(!manager->act("owner", "click", url, snapshot["snapshot_id"], id, label, ""));
        QThread::msleep(50);
        const auto verified = manager->observe("owner");
        CHECK(verified && verified.value().find("unsafe ") == std::string::npos);
    }
    const auto offscreen = manager->observe("owner");
    CHECK(offscreen);
    const auto offscreenSnapshot = nlohmann::json::parse(offscreen.value());
    const auto offscreenId = element(offscreenSnapshot, "Below fold");
    CHECK(!offscreenId.empty());
    CHECK(!manager->act("owner", "click", url, offscreenSnapshot["snapshot_id"], offscreenId, "Below fold", ""));
    QThread::msleep(50);
    const auto offscreenResult = manager->observe("owner");
    CHECK(offscreenResult && offscreenResult.value().find("below fold clicked") != std::string::npos);
    CHECK(manager
              ->screenshot("owner", "https://wrong.example/",
                           workspace.filePath(QStringLiteral("wrong.png")).toStdString())
              .code() == MaiErrorCode::InvalidInput);
    const QString image = workspace.filePath(QStringLiteral("page.png"));
    const QString canceledImage = workspace.filePath(QStringLiteral("canceled.png"));
    CHECK(manager->screenshot("owner", url, canceledImage.toUtf8().toStdString(), [] { return true; }).code() ==
          MaiErrorCode::Canceled);
    CHECK(!QFile::exists(canceledImage));
    const MaiError captured = manager->screenshot("owner", url, image.toUtf8().toStdString());
    if (captured) std::fprintf(stderr, "screenshot failed: %s\n", captured.message().c_str());
    CHECK(!captured && QFile::exists(image) && QFileInfo(image).size() > 100);
    const QString copy = qEnvironmentVariable("MAICHAT_BROWSER_TEST_SCREENSHOT");
    if (!copy.isEmpty()) {
        QFile::remove(copy);
        CHECK(QFile::copy(image, copy));
    }
    CHECK(!manager->close("owner"));
    CHECK(manager->observe("owner").error().code() == MaiErrorCode::NotFound);
    MaiToolContext context;
    context.sessionId = "loading-owner";
    context.root = workspace.path().toUtf8().toStdString();
    const auto execute = [&](const nlohmann::json& args) { return tool->execute(args.dump(), context); };
    CHECK(execute({{"action", "open"}, {"url", url}, {"mode", "invalid"}}).error().code() ==
          MaiErrorCode::InvalidInput);
    for (const auto value : {nlohmann::json(-1), nlohmann::json(5001), nlohmann::json(1.5), nlohmann::json("1")})
        CHECK(execute({{"action", "observe"}, {"wait_ms", value}}).error().code() == MaiErrorCode::InvalidInput);
    const auto loadingUrl = "http://127.0.0.1:" + std::to_string(port) + "/loading";
    CHECK(!execute({{"action", "open"}, {"url", loadingUrl}, {"mode", "headless"}}).hasError());
    nlohmann::json loading;
    for (int attempt = 0; attempt < 20; ++attempt) {
        const auto result = execute({{"action", "observe"}, {"wait_ms", 0}});
        CHECK(!result.hasError());
        loading = nlohmann::json::parse(result.output());
        if (loading.value("url", "") == loadingUrl) break;
        QThread::msleep(30);
    }
    CHECK(loading["state"] == "loading");
    CHECK(!loading.value("snapshot_id", "").empty());
    CHECK(loading["elements"].empty());
    const auto loadingImage = execute({{"action", "screenshot"}, {"expected_url", loadingUrl}});
    CHECK(!loadingImage.hasError() && loadingImage.images().size() == 1);
    const auto started = std::chrono::steady_clock::now();
    const auto timed = execute({{"action", "observe"}, {"wait_ms", 180}});
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    CHECK(!timed.hasError());
    CHECK(nlohmann::json::parse(timed.output())["state"] == "loading");
    CHECK(elapsed >= 140 && elapsed < 1800);
    server.releaseLoading = true;
    const auto ready = execute({{"action", "observe"}, {"wait_ms", 5000}});
    CHECK(!ready.hasError());
    CHECK(nlohmann::json::parse(ready.output())["ready_state"] == "complete");
    CHECK(!execute({{"action", "close"}}).hasError());
    CHECK(!execute({{"action", "open"}, {"url", url}, {"mode", "windowed"}}).hasError());
    const auto windowed = execute({{"action", "observe"}, {"wait_ms", 5000}});
    CHECK(!windowed.hasError());
    CHECK(nlohmann::json::parse(windowed.output())["mode"] == "windowed");
#ifdef Q_OS_WIN
    const auto ownedWindow = ownedBrowserWindow();
    CHECK(ownedWindow != nullptr);
    RECT bounds{};
    CHECK(::GetWindowRect(ownedWindow, &bounds));
    struct OwnedOccluder {
        HWND window = nullptr;
        ~OwnedOccluder() {
            if (window) ::DestroyWindow(window);
        }
    } occluder;
    occluder.window =
        ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"STATIC", L"Owned browser cover", WS_POPUP | SS_CENTER,
                          bounds.left - 8, bounds.top - 8, bounds.right - bounds.left + 16,
                          bounds.bottom - bounds.top + 16, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    CHECK(occluder.window != nullptr);
    ::ShowWindow(occluder.window, SW_SHOWNOACTIVATE);
    ::UpdateWindow(occluder.window);
    nlohmann::json occluded;
    for (int attempt = 0; attempt < 12; ++attempt) {
        const auto result = execute({{"action", "observe"}, {"wait_ms", 0}});
        CHECK(!result.hasError());
        occluded = nlohmann::json::parse(result.output());
        if (occluded["visibility"] == "hidden") break;
        QThread::msleep(50);
    }
    CHECK(occluded["state"] == "ready");
    const auto coveredCapture = execute({{"action", "screenshot"}, {"expected_url", url}});
    if (coveredCapture.hasError())
        std::fprintf(stderr, "occluded capture failed: %s\n", coveredCapture.error().message().c_str());
    CHECK(!coveredCapture.hasError() && coveredCapture.images().size() == 1);
    const QString copyWindowed = qEnvironmentVariable("MAICHAT_BROWSER_TEST_WINDOWED_SCREENSHOT");
    if (!copyWindowed.isEmpty())
        CHECK(QFile::copy(QString::fromStdString(coveredCapture.images().front().path), copyWindowed));
    const auto coveredNote = element(occluded, "Note");
    CHECK(!manager->act(context.sessionId, "type", url, occluded["snapshot_id"], coveredNote, "Note", "covered OK"));
    std::printf("owned windowed browser covered: visibility=%s; DOM/type/PNG succeeded\n",
                occluded["visibility"].get<std::string>().c_str());
#endif
    CHECK(!execute({{"action", "close"}}).hasError());
    return 0;
}
