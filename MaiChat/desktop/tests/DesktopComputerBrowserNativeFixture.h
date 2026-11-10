// Private implementation included by the existing display-native test only.
// This is deterministic native-browser execution, not visual-model acceptance.
#pragma once

#include <QStandardPaths>
#include <QUrl>

#include <atomic>
#include <limits>
#include <thread>

#include "MaiThread.h"

#if defined(Q_OS_WIN)
#include <tlhelp32.h>
#endif

namespace {

struct BrowserLoopbackFixture {
    httplib::Server server;
    std::thread worker;
    std::atomic<bool> workerExited{false};
    std::atomic<int> firstRequests{0}, secondRequests{0}, firstReady{0}, secondReady{0}, rejectedRequests{0};
    std::atomic<bool> secondCookie{false};
    const std::string nonce = QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-').toStdString();
    const std::string base = "/owned-browser-" + nonce;
    const std::string cookie = "maichat_fixture_" + nonce + "=" + nonce;
    const std::string secondMarker = "MaiChat owned browser SECOND BODY " + nonce;
    int port = 0;

    bool requestIsLocal(const httplib::Request& request, const std::string& path) const {
        return request.remote_addr == "127.0.0.1" && request.target == path &&
               request.get_header_value("Host") == "127.0.0.1:" + std::to_string(port);
    }
    bool hasCookie(const httplib::Request& request) const {
        const std::string values = request.get_header_value("Cookie");
        std::size_t start = 0;
        while (start < values.size()) {
            const auto end = values.find(';', start);
            std::string value = values.substr(start, end == std::string::npos ? end : end - start);
            while (!value.empty() && value.front() == ' ') value.erase(value.begin());
            if (value == cookie) return true;
            if (end == std::string::npos) break;
            start = end + 1;
        }
        return false;
    }
    void respond(httplib::Response& response, bool second) {
        const std::string marker = second ? secondMarker : "MaiChat owned browser FIRST " + nonce;
        const std::string title = "MaiChat owned browser tab " + std::string(second ? "SECOND " : "FIRST ") + nonce;
        const std::string ready = base + (second ? "/second-ready" : "/first-ready");
        response.set_header("Cache-Control", "no-store");
        response.set_header("Connection", "close");
        response.set_header("Content-Security-Policy",
                            "default-src 'none'; style-src 'unsafe-inline'; "
                            "script-src 'unsafe-inline'; connect-src 'self'; img-src data:; base-uri 'none'");
        response.set_content("<!doctype html><html><head><meta charset='utf-8'><title>" + title +
                                 "</title><link rel='icon' href='data:,'></head>"
                                 "<body style='background:#263147;color:white;font:28px sans-serif;padding:64px'>"
                                 "<h1>" +
                                 marker + "</h1><p>" +
                                 (second ? "Same private browser profile: cookie verified by the server."
                                         : "Use the native address bar to open the second owned page.") +
                                 "</p><p>All page content belongs to this local fixture.</p>"
                                 "<script>fetch('" +
                                 ready + "',{credentials:'same-origin'});</script></body></html>",
                             "text/html; charset=utf-8");
    }
    bool listen() {
        server.set_read_timeout(2);
        server.set_write_timeout(2);
        server.set_payload_max_length(4096);
        server.Get(base + "/first", [this](const httplib::Request& request, httplib::Response& response) {
            if (!requestIsLocal(request, base + "/first")) {
                response.status = 403;
                return;
            }
            ++firstRequests;
            response.set_header("Set-Cookie", cookie + "; Path=/; HttpOnly; SameSite=Strict");
            respond(response, false);
        });
        server.Get(base + "/second", [this](const httplib::Request& request, httplib::Response& response) {
            if (!requestIsLocal(request, base + "/second") || !hasCookie(request)) {
                response.status = 403;
                return;
            }
            secondCookie = true;
            ++secondRequests;
            respond(response, true);
        });
        for (bool second : {false, true}) {
            const std::string path = base + (second ? "/second-ready" : "/first-ready");
            server.Get(path, [this, path, second](const httplib::Request& request, httplib::Response& response) {
                if (!requestIsLocal(request, path) || !hasCookie(request)) {
                    response.status = 403;
                    return;
                }
                if (second)
                    ++secondReady;
                else
                    ++firstReady;
                response.set_header("Connection", "close");
                response.status = 204;
            });
        }
        // The browser's per-process proxy points here too. This server never
        // forwards HTTP/CONNECT traffic; non-fixture routes are rejected.
        server.set_error_handler([this](const httplib::Request&, httplib::Response& response) {
            ++rejectedRequests;
            response.set_header("Connection", "close");
            response.status = 403;
        });
        port = server.bind_to_any_port("127.0.0.1");
        if (port <= 0) return false;
        worker = std::thread([this] {
            MaiThread::setCurrentName("mai-owned-browser-http");
            server.listen_after_bind();
            workerExited = true;
        });
        // stop() closes the bound listener only after listen_after_bind starts.
        // Do not launch a browser while that transition is still pending.
        return waitUntil([&] { return server.is_running() || workerExited.load(); }, 2000) && server.is_running();
    }
    ~BrowserLoopbackFixture() {
        if (worker.joinable()) {
            while (!server.is_running() && !workerExited.load()) QThread::msleep(1);
            server.stop();
            worker.join();
        }
    }
    std::string url(bool second) const {
        return "http://127.0.0.1:" + std::to_string(port) + base + (second ? "/second" : "/first");
    }
    Json receipt() const {
        return {{"bind", "127.0.0.1"},
                {"port", port},
                {"first_requests", firstRequests.load()},
                {"second_requests", secondRequests.load()},
                {"first_ready", firstReady.load()},
                {"second_ready", secondReady.load()},
                {"second_cookie_verified", secondCookie.load()},
                {"rejected_nonfixture_requests", rejectedRequests.load()}};
    }
};

#if defined(Q_OS_WIN)
QString installedFixtureBrowser() {
    for (const auto& name : {QStringLiteral("Google/Chrome/Application/chrome.exe"),
                             QStringLiteral("Microsoft/Edge/Application/msedge.exe")})
        for (const auto& base : {qEnvironmentVariable("ProgramFiles"), qEnvironmentVariable("ProgramFiles(x86)")}) {
            const QFileInfo candidate(QDir(base).filePath(name));
            if (!base.isEmpty() && candidate.isFile()) return candidate.canonicalFilePath();
        }
    return {};
}

struct OwnedFixtureBrowser {
    QProcess process;
    HANDLE job = nullptr;
    HANDLE nativeProcess = nullptr;
    std::uint64_t pid = 0;
    QString program;
    QString profile;
    QByteArray profileReceipt;
    bool contained = false;
    DWORD primaryThreadId = 0;
    Json launchFailure;

    bool rejectLaunch(const char* stage) {
        launchFailure = {{"stage", stage}, {"win32_error", GetLastError()}};
        return false;
    }

    bool start(const QString& executable, const QString& profilePath, const QStringList& arguments) {
        program = executable;
        profile = profilePath;
        job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            return rejectLaunch("private_job_configuration");
        process.setProgram(program);
        process.setArguments(arguments);
        process.setStandardOutputFile(QProcess::nullDevice());
        process.setStandardErrorFile(QProcess::nullDevice());
        process.setCreateProcessArgumentsModifier(
            [](QProcess::CreateProcessArguments* create) { create->flags |= CREATE_SUSPENDED; });
        process.start();
        if (!process.waitForStarted(5000)) return rejectLaunch("suspended_process_start");
        pid = std::uint64_t(process.processId());
        nativeProcess = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_QUOTA | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, DWORD(pid));
        if (!nativeProcess || GetProcessId(nativeProcess) != pid) return rejectLaunch("new_process_handle_identity");
        wchar_t actualProgram[32768]{};
        DWORD programLength = 32768;
        if (!QueryFullProcessImageNameW(nativeProcess, 0, actualProgram, &programLength) ||
            QFileInfo(QString::fromWCharArray(actualProgram, int(programLength)))
                    .canonicalFilePath()
                    .compare(program, Qt::CaseInsensitive) != 0)
            return rejectLaunch("installed_process_image_identity");
        if (!AssignProcessToJobObject(job, nativeProcess)) return rejectLaunch("assign_private_job");
        contained = true;
        BOOL inJob = FALSE;
        if (!IsProcessInJob(nativeProcess, job, &inJob) || !inJob) return rejectLaunch("verify_private_job");
        // Before the suspended browser has executed there must be exactly one
        // thread. Discover it by this new PID, never by a saved Qt stack pointer.
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return rejectLaunch("new_process_thread_snapshot");
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        DWORD primaryThread = 0;
        int count = 0;
        if (Thread32First(snapshot, &entry)) do {
                if (entry.th32OwnerProcessID == pid) {
                    primaryThread = entry.th32ThreadID;
                    ++count;
                }
            } while (Thread32Next(snapshot, &entry));
        CloseHandle(snapshot);
        if (count != 1) {
            launchFailure = {{"stage", "unique_suspended_primary_thread"}, {"owned_thread_count", count}};
            return false;
        }
        const HANDLE thread =
            OpenThread(THREAD_QUERY_LIMITED_INFORMATION | THREAD_SUSPEND_RESUME, FALSE, primaryThread);
        if (!thread) return rejectLaunch("open_owned_primary_thread");
        const bool threadOwned = GetProcessIdOfThread(thread) == pid && GetThreadId(thread) == primaryThread;
        const DWORD previousSuspendCount = threadOwned ? ResumeThread(thread) : DWORD(-1);
        CloseHandle(thread);
        if (!threadOwned || previousSuspendCount != 1) {
            launchFailure = {{"stage", "resume_verified_primary_thread"},
                             {"thread_owned", threadOwned},
                             {"previous_suspend_count", previousSuspendCount}};
            return false;
        }
        primaryThreadId = primaryThread;
        return true;
    }
    bool identityIntact() const {
        BOOL inJob = FALSE;
        return contained && nativeProcess && process.state() == QProcess::Running && process.processId() == pid &&
               GetProcessId(nativeProcess) == pid && WaitForSingleObject(nativeProcess, 0) == WAIT_TIMEOUT &&
               IsProcessInJob(nativeProcess, job, &inJob) && inJob &&
               QFileInfo(profile).canonicalFilePath() == profile &&
               process.arguments().count(QStringLiteral("--user-data-dir=") + profile) == 1;
    }
    bool profileIntact() const {
        QFile marker(QDir(profile).filePath(QStringLiteral("DevToolsActivePort")));
        return identityIntact() && !profileReceipt.isEmpty() && marker.open(QIODevice::ReadOnly) &&
               marker.readAll() == profileReceipt;
    }
    bool close() {
        bool empty = true;
        if (job) {
            TerminateJobObject(job, 1);
            empty = waitUntil(
                [&] {
                    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
                    return QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting,
                                                     sizeof(accounting), nullptr) &&
                           accounting.ActiveProcesses == 0;
                },
                5000);
            CloseHandle(job);
            job = nullptr;
        }
        if (process.state() != QProcess::NotRunning) {
            // The QProcess is always ours, including a failed suspended launch
            // which could not be assigned to the private Job.
            process.kill();
            process.waitForFinished(3000);
        }
        if (nativeProcess) {
            CloseHandle(nativeProcess);
            nativeProcess = nullptr;
        }
        contained = false;
        return empty && process.state() == QProcess::NotRunning;
    }
    ~OwnedFixtureBrowser() {
        close();
    }
};

struct BrowserDwmApi {
    HMODULE module = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    decltype(&DwmGetWindowAttribute) get =
        module ? reinterpret_cast<decltype(&DwmGetWindowAttribute)>(GetProcAddress(module, "DwmGetWindowAttribute"))
               : nullptr;
    decltype(&DwmSetWindowAttribute) set =
        module ? reinterpret_cast<decltype(&DwmSetWindowAttribute)>(GetProcAddress(module, "DwmSetWindowAttribute"))
               : nullptr;
    ~BrowserDwmApi() {
        if (module) FreeLibrary(module);
    }
};

const BrowserDwmApi& browserDwmApi() {
    static const BrowserDwmApi api;
    return api;
}

HRESULT browserVisibleFrame(HWND window, RECT& bounds) {
    const auto& api = browserDwmApi();
    return api.get ? api.get(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)) : E_NOTIMPL;
}

bool browserNonRounded(HWND window) {
    const auto& api = browserDwmApi();
    DWM_WINDOW_CORNER_PREFERENCE preference = DWMWCP_DEFAULT;
    return api.get && SUCCEEDED(api.get(window, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference))) &&
           preference == DWMWCP_DONOTROUND;
}

bool browserVisibleFrameCovers(HWND window, const DesktopNativeDisplay& display) {
    RECT bounds{};
    return SUCCEEDED(browserVisibleFrame(window, bounds)) && bounds.left <= display.x && bounds.top <= display.y &&
           bounds.right >= display.x + display.width && bounds.bottom >= display.y + display.height;
}
#endif

int runBrowserWorkflow() {
    if (qEnvironmentVariable("MAICHAT_RUN_COMPUTER_DISPLAY_NATIVE") != QStringLiteral("1"))
        return skip("set the explicit owned-desktop opt-in to run the private browser workflow");
#if !defined(Q_OS_WIN)
    return skip("private native browser identity checks require Windows");
#else
    if (QApplication::platformName() != QStringLiteral("windows")) return skip("real Windows QPA required");
    const QString program = installedFixtureBrowser();
    if (program.isEmpty()) return skip("an installed Chrome or Edge binary was not found");
    QTemporaryDir workspace;
    if (!workspace.isValid()) return 1;
    QTemporaryDir profile(QDir(workspace.path()).filePath(QStringLiteral("browser-profile-XXXXXX")));
    if (!profile.isValid() || !QDir(profile.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty())
        return 1;
    const QString profilePath = QFileInfo(profile.path()).canonicalFilePath();
    QString evidence = qEnvironmentVariable("MAICHAT_COMPUTER_NATIVE_EVIDENCE_DIR");
    if (!evidence.isEmpty()) {
        evidence = QDir(evidence).filePath(
            QStringLiteral("browser-workflow-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        if (!QDir().mkpath(evidence)) return 1;
    }
    BrowserLoopbackFixture server;
    if (!server.listen()) return skip("owned loopback HTTP fixture could not listen");
    // These flags affect only the disposable browser. They suppress background
    // traffic and send HTTP(S) proxy traffic to a local server that never
    // forwards it; this is not an OS network sandbox or a zero-egress claim.
    const QStringList arguments{QStringLiteral("--user-data-dir=") + profilePath,
                                QStringLiteral("--profile-directory=Default"),
                                QStringLiteral("--no-first-run"),
                                QStringLiteral("--no-default-browser-check"),
                                QStringLiteral("--disable-sync"),
                                QStringLiteral("--disable-background-networking"),
                                QStringLiteral("--disable-component-update"),
                                QStringLiteral("--disable-default-apps"),
                                QStringLiteral("--disable-extensions"),
                                QStringLiteral("--disable-domain-reliability"),
                                QStringLiteral("--disable-breakpad"),
                                QStringLiteral("--metrics-recording-only"),
                                QStringLiteral("--disable-quic"),
                                QStringLiteral("--force-webrtc-ip-handling-policy=disable_non_proxied_udp"),
                                QStringLiteral("--proxy-server=http://127.0.0.1:") + QString::number(server.port),
                                QStringLiteral("--proxy-bypass-list=127.0.0.1;localhost"),
                                QStringLiteral("--host-resolver-rules=MAP * 0.0.0.0, EXCLUDE 127.0.0.1"),
                                QStringLiteral("--remote-debugging-port=0"),
                                QStringLiteral("--force-renderer-accessibility"),
                                QStringLiteral("--new-window"),
                                QString::fromStdString(server.url(false))};
    OwnedFixtureBrowser browser;
    bool captureAttempted = false;
    const auto beforeInputExit = [&](const char* reason, int code = 77) {
        const bool stopped = browser.close();
        const bool removed = stopped && profile.remove();
        const Json cleanup{{"reason", reason},
                           {"browser_pid", browser.pid},
                           {"owned_job_stopped", stopped},
                           {"private_profile_removed", removed},
                           {"input_dispatched", false},
                           {"capture_attempted", captureAttempted},
                           {"pixels_captured", captureAttempted ? Json(nullptr) : Json(false)}};
        const bool recorded =
            evidence.isEmpty() || writeJson(QDir(evidence).filePath(QStringLiteral("browser-cleanup.json")), cleanup);
        if (stopped && removed && recorded && code == 77) return skip(reason);
        std::fprintf(stderr, "native browser pre-input failure: %s; cleanup_stopped=%d profile_removed=%d\n", reason,
                     stopped, removed);
        return 1;
    };
    const bool launched = browser.start(program, profilePath, arguments);
    const Json launch{{"program", program.toStdString()},
                      {"private_profile", profilePath.toStdString()},
                      {"browser_pid", browser.pid},
                      {"job_contained", browser.contained},
                      {"primary_thread_id", browser.primaryThreadId},
                      {"launched", launched},
                      {"failure", browser.launchFailure},
                      {"input_dispatched", false},
                      {"pixels_captured", false}};
    if (!evidence.isEmpty() && !writeJson(QDir(evidence).filePath(QStringLiteral("browser-launch.json")), launch))
        return beforeInputExit("browser launch evidence could not be written", 1);
    if (!launched) return beforeInputExit("private browser process/job identity was not established");
    if (!waitUntil(
            [&] {
                if (!browser.identityIntact()) return true;
                QFile marker(QDir(profilePath).filePath(QStringLiteral("DevToolsActivePort")));
                if (marker.open(QIODevice::ReadOnly)) {
                    const QByteArray bytes = marker.readAll();
                    const auto lines = bytes.split('\n');
                    bool validPort = false;
                    const int port = lines.isEmpty() ? 0 : lines.front().toInt(&validPort);
                    if (validPort && port > 0 && port <= 65535 && lines.size() > 1 &&
                        lines[1].startsWith("/devtools/browser/"))
                        browser.profileReceipt = bytes;
                }
                return browser.profileIntact() && server.firstReady.load() > 0;
            },
            15000) ||
        !browser.profileIntact() || server.firstReady.load() == 0)
        return beforeInputExit("fresh profile marker and first local page were not established");
    DesktopComputerController controller;
    const auto displays = controller.displays();
    if (!displays) return beforeInputExit(displays.error().message().c_str());
    DesktopNativeWindow target;
    DesktopNativeDisplay display;
    bool ready = waitUntil(
        [&] {
            if (!browser.profileIntact()) return true;
            const auto windows = controller.windows();
            if (!windows) return false;
            int candidates = 0;
            for (const auto& window : windows.value()) {
                if (window.processId != browser.pid) continue;
                const HWND handle = reinterpret_cast<HWND>(std::uintptr_t(window.id));
                DWORD actualPid = 0;
                if (GetAncestor(handle, GA_ROOT) != handle || GetWindow(handle, GW_OWNER) ||
                    !GetWindowThreadProcessId(handle, &actualPid) || actualPid != browser.pid)
                    continue;
                target = window;
                ++candidates;
            }
            return candidates == 1;
        },
        5000);
    if (!ready || !target.id || !browser.profileIntact())
        return beforeInputExit("unique owned normal browser root was not established");
    const HWND root = reinterpret_cast<HWND>(std::uintptr_t(target.id));
    const auto monitor =
        std::uint64_t(reinterpret_cast<std::uintptr_t>(MonitorFromWindow(root, MONITOR_DEFAULTTONEAREST)));
    for (const auto& candidate : displays.value())
        if (candidate.id == monitor) display = candidate;
    if (!display.id) return beforeInputExit("owned normal browser did not have an active native display");
    // The real browser is its own display-sized backdrop. Capture is refused if
    // another process contributes visible pixels above it. Same-PID ornaments
    // can contribute pixels but never gain foreground/input authority.
    OwnedDocumentDesktop owned{browser.process, reinterpret_cast<HWND>(std::uintptr_t(target.id)), browser.pid,
                               display};
    if (!browser.profileIntact() || !owned.owns(root))
        return beforeInputExit("owned browser identity changed before placement");
    // This preference belongs only to the verified disposable HWND and dies
    // with its private Job. No theme, other application or persistent profile
    // setting is changed. Unknown/unsupported DWM behavior remains a boundary.
    const auto& dwm = browserDwmApi();
    DWM_WINDOW_CORNER_PREFERENCE oldCorner = DWMWCP_DEFAULT;
    DWM_WINDOW_CORNER_PREFERENCE newCorner = DWMWCP_DEFAULT;
    const DWM_WINDOW_CORNER_PREFERENCE requestedCorner = DWMWCP_DONOTROUND;
    const HRESULT cornerBefore =
        dwm.get ? dwm.get(root, DWMWA_WINDOW_CORNER_PREFERENCE, &oldCorner, sizeof(oldCorner)) : E_NOTIMPL;
    HRESULT cornerSet = E_NOTIMPL;
    HRESULT cornerAfter = E_NOTIMPL;
    if (SUCCEEDED(cornerBefore) && browser.profileIntact() && owned.owns(root) && dwm.set) {
        cornerSet = dwm.set(root, DWMWA_WINDOW_CORNER_PREFERENCE, &requestedCorner, sizeof(requestedCorner));
        if (SUCCEEDED(cornerSet) && browser.profileIntact() && owned.owns(root) && dwm.get)
            cornerAfter = dwm.get(root, DWMWA_WINDOW_CORNER_PREFERENCE, &newCorner, sizeof(newCorner));
    }
    const bool cornerConfirmed =
        SUCCEEDED(cornerBefore) && SUCCEEDED(cornerSet) && SUCCEEDED(cornerAfter) && newCorner == DWMWCP_DONOTROUND;
    const Json cornerPolicy{{"root_window_id", std::to_string(target.id)},
                            {"query_before_hresult", std::int32_t(cornerBefore)},
                            {"preference_before", SUCCEEDED(cornerBefore) ? Json(int(oldCorner)) : Json(nullptr)},
                            {"requested_preference", int(requestedCorner)},
                            {"set_hresult", std::int32_t(cornerSet)},
                            {"query_after_hresult", std::int32_t(cornerAfter)},
                            {"preference_after", SUCCEEDED(cornerAfter) ? Json(int(newCorner)) : Json(nullptr)},
                            {"nonrounded_confirmed", cornerConfirmed},
                            {"input_dispatched", false},
                            {"pixels_captured", false}};
    if (!evidence.isEmpty() &&
        !writeJson(QDir(evidence).filePath(QStringLiteral("browser-corner-policy.json")), cornerPolicy))
        return beforeInputExit("browser corner policy evidence could not be written", 1);
    if (!cornerConfirmed) return beforeInputExit("nonrounded owned normal browser could not be confirmed");
    const Json beforePlacement = owned.describeBlockingSurface(root);
    // Ordinary Chrome keeps the omnibox visible. Only this verified process's
    // exact root is positioned; SWP_NOACTIVATE never takes foreground from
    // another application. All existing guards must subsequently pass.
    const auto rectangle = [](const RECT& bounds) {
        return Json{{"x", bounds.left},
                    {"y", bounds.top},
                    {"width", bounds.right - bounds.left},
                    {"height", bounds.bottom - bounds.top}};
    };
    RECT requested{LONG(display.x), LONG(display.y), LONG(display.x + display.width), LONG(display.y + display.height)};
    Json placementAttempts = Json::array();
    bool positioned = false;
    const auto previousDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    for (int pass = 0; pass < 2; ++pass) {
        if (!browser.profileIntact() || !owned.owns(root)) break;
        positioned = SetWindowPos(root, HWND_TOPMOST, requested.left, requested.top, requested.right - requested.left,
                                  requested.bottom - requested.top, SWP_NOACTIVATE | SWP_SHOWWINDOW) != 0;
        const DWORD placementError = positioned ? 0 : GetLastError();
        RECT outer{}, visible{};
        const bool outerValid = GetWindowRect(root, &outer) != 0;
        const HRESULT visibleResult = browserVisibleFrame(root, visible);
        placementAttempts.push_back(
            {{"requested_outer", rectangle(requested)},
             {"set_window_pos_ok", positioned},
             {"win32_error", placementError},
             {"measured_outer", outerValid ? rectangle(outer) : Json(nullptr)},
             {"visible_frame_hresult", std::int32_t(visibleResult)},
             {"measured_visible", SUCCEEDED(visibleResult) ? rectangle(visible) : Json(nullptr)}});
        if (!positioned || !outerValid || FAILED(visibleResult) || visible.right <= visible.left ||
            visible.bottom <= visible.top || browserVisibleFrameCovers(root, display))
            break;
        // Compensate only this HWND's measured invisible native borders. There
        // is no fixed padding and no change to styles or coverage requirements.
        const std::int64_t left = std::int64_t(display.x) - (std::int64_t(visible.left) - outer.left);
        const std::int64_t top = std::int64_t(display.y) - (std::int64_t(visible.top) - outer.top);
        const std::int64_t right =
            std::int64_t(display.x + display.width) + (std::int64_t(outer.right) - visible.right);
        const std::int64_t bottom =
            std::int64_t(display.y + display.height) + (std::int64_t(outer.bottom) - visible.bottom);
        const auto fits = [](std::int64_t value) {
            return value >= (std::numeric_limits<LONG>::min)() && value <= (std::numeric_limits<LONG>::max)();
        };
        if (!fits(left) || !fits(top) || !fits(right) || !fits(bottom) || right <= left || bottom <= top ||
            right - left > (std::numeric_limits<int>::max)() || bottom - top > (std::numeric_limits<int>::max)())
            break;
        requested = {LONG(left), LONG(top), LONG(right), LONG(bottom)};
    }
    const Json afterPlacement = owned.describeBlockingSurface(root);
    if (previousDpi) SetThreadDpiAwarenessContext(previousDpi);
    const Json placement{{"kind", "owned_normal_window_native_rectangle"},
                         {"before", beforePlacement},
                         {"after", afterPlacement},
                         {"attempts", placementAttempts},
                         {"topmost", true},
                         {"no_activate", true},
                         {"visible_frame_covers_display", browserVisibleFrameCovers(root, display)}};
    if (!evidence.isEmpty() && !writeJson(QDir(evidence).filePath(QStringLiteral("browser-placement.json")), placement))
        return beforeInputExit("browser placement evidence could not be written", 1);
    if (!positioned) return beforeInputExit("verified owned normal browser could not be positioned over its display");
    // GetWindowRect can include invisible resize borders. Require the actual
    // DWM visible frame too; do not claim the underlying desktop as owned.
    const auto canceled = [&] {
        return !browser.profileIntact() || owned.canceled() || !browserNonRounded(root) ||
               !browserVisibleFrameCovers(root, display);
    };
    QElapsedTimer stable;
    ready = waitUntil(
        [&] {
            if (canceled() || !OwnedDocumentDesktop::physicalInputReleased()) {
                stable.invalidate();
                return false;
            }
            if (!stable.isValid()) stable.start();
            return stable.elapsed() >= 200;
        },
        5000);
    HWND startupBlocker = nullptr;
    const char* startupReason = owned.blockingReason(&startupBlocker);
    const Json startup{
        {"browser_pid", browser.pid},
        {"program", program.toStdString()},
        {"private_profile", profilePath.toStdString()},
        {"profile_marker_verified", browser.profileIntact()},
        {"job_contained", browser.contained},
        {"root_window_id", std::to_string(target.id)},
        {"corner_policy", cornerPolicy},
        {"nonrounded_still_confirmed", browserNonRounded(root)},
        {"initial_placement", placement},
        {"visible_frame_covers_display", browserVisibleFrameCovers(root, display)},
        {"foreground_ready", ready},
        {"guard_reason", startupReason ? Json(startupReason) : Json(nullptr)},
        {"blocking_surface", startupBlocker ? owned.describeBlockingSurface(startupBlocker) : Json(nullptr)},
        {"input_dispatched", false},
        {"pixels_captured", false}};
    if (!evidence.isEmpty() && !writeJson(QDir(evidence).filePath(QStringLiteral("browser-readiness.json")), startup))
        return beforeInputExit("browser readiness evidence could not be written", 1);
    if (!ready) return beforeInputExit("owned browser foreground/coverage/input conditions were not ready");
    const std::string session = "owned-browser-workflow";
    captureAttempted = true;
    auto observed = controller.observeDisplay(session, workspace.path().toStdString(), display.id, canceled);
    if (!observed) return beforeInputExit(observed.error().message().c_str());
    Json trace = Json::array(), failures = Json::array();
    int captures = 0;
    const auto fail = [&](const std::string& reason) {
        failures.push_back(reason);
        return false;
    };
    const auto preserve = [&](const char* stage, const DesktopComputerObservation& image) {
        const QString filename = QStringLiteral("browser-%1-%2.png").arg(captures++).arg(QString::fromUtf8(stage));
        if (!evidence.isEmpty() &&
            !QFile::copy(QString::fromStdString(image.imagePath), QDir(evidence).filePath(filename)))
            return fail("could not preserve private browser evidence");
        trace.push_back(
            {{"stage", stage}, {"image", filename.toStdString()}, {"observation", Json::parse(image.json)}});
        return true;
    };
    const auto action = [&](const char* stage, const char* kind, const std::string& value) {
        if (canceled() || !OwnedDocumentDesktop::physicalInputReleased())
            return fail(std::string(stage) + ": owned input conditions changed");
        DesktopComputerAction input;
        input.desktopScope = true;
        input.workspace = workspace.path().toStdString();
        input.displayId = display.id;
        input.snapshotId = Json::parse(observed.value().json).value("snapshot_id", std::string());
        input.action = kind;
        if (input.action == "key")
            input.key = value;
        else
            input.text = value;
        Json cancellation;
        input.isCanceled = [&] {
            HWND blocker = nullptr;
            const char* reason = owned.blockingReason(&blocker);
            const bool profileIntact = browser.profileIntact();
            const bool frameCovers = browserVisibleFrameCovers(root, display);
            const bool nonRounded = profileIntact && owned.owns(root) && browserNonRounded(root);
            if (profileIntact && !reason && frameCovers && nonRounded) return false;
            cancellation = {{"reason", reason           ? reason
                                       : !profileIntact ? "private_profile_identity_changed"
                                       : !nonRounded    ? "normal_window_corner_policy_not_confirmed"
                                                        : "visible_frame_does_not_cover_display"},
                            {"nonrounded_confirmed", nonRounded},
                            {"visible_frame_covers_display", frameCovers},
                            {"blocking_surface", blocker ? owned.describeBlockingSurface(blocker) : Json(nullptr)}};
            return true;
        };
        const auto result = controller.perform(session, input);
        if (!result) {
            trace.push_back({{"stage", stage}, {"error", result.error().message()}, {"cancellation", cancellation}});
            return fail(std::string(stage) + ": " + result.error().message());
        }
        trace.push_back({{"stage", stage},
                         {"action", kind},
                         {"state", result.value().state},
                         {"dispatched", result.value().dispatched},
                         {"observation_error", result.value().observationError},
                         {"cancellation", cancellation}});
        if (!result.value().dispatched || result.value().observation.imagePath.empty()) {
            // Diagnose the already-dispatched action without another input or
            // capture. A missing compositor frame must remain a failure even
            // if the focused field independently reports the requested text.
            const HWND foreground = GetForegroundWindow();
            const bool profileOwned = browser.profileIntact();
            const bool rootOwned = owned.owns(owned.root);
            const bool foregroundOwned = owned.owns(foreground);
            HWND blocker = nullptr;
            const char* reason = owned.blockingReason(&blocker);
            const auto provider = maiScreenshotCaptureState();
            Json diagnostic{
                {"stage", stage},
                {"diagnostic", "post_action_observation_failure"},
                {"observation_error", result.value().observationError},
                {"profile_identity_intact", profileOwned},
                {"root_identity_intact", rootOwned},
                {"foreground_owned", foregroundOwned},
                {"foreground_window_id", std::to_string(std::uint64_t(reinterpret_cast<std::uintptr_t>(foreground)))},
                {"guard_reason", reason ? Json(reason) : Json(nullptr)},
                {"root_geometry", rootOwned ? owned.describeBlockingSurface(owned.root) : Json(nullptr)},
                {"blocking_surface", blocker ? owned.describeBlockingSurface(blocker) : Json(nullptr)},
                {"capture_provider",
                 {{"worker_running", provider.workerRunning},
                  {"capturing", provider.capturing},
                  {"worker_starts", provider.workerStarts},
                  {"device_creations", provider.deviceCreations},
                  {"session_starts", provider.sessionStarts},
                  {"frames_received", provider.framesReceived}}}};
            if (profileOwned && rootOwned && foregroundOwned) {
                DWORD actualPid = 0;
                const DWORD thread = GetWindowThreadProcessId(foreground, &actualPid);
                GUITHREADINFO gui{};
                gui.cbSize = sizeof(gui);
                if (actualPid == browser.pid && GetGUIThreadInfo(thread, &gui) && owned.owns(gui.hwndFocus))
                    diagnostic["native_focus"] = owned.describeBlockingSurface(gui.hwndFocus);
            }
            if (profileOwned && rootOwned && foreground == owned.root) {
                const auto text = maiDesktopFocusedText(nativeWindow(owned.root, browser.pid));
                if (!browser.profileIntact() || !owned.owns(owned.root) || GetForegroundWindow() != owned.root)
                    diagnostic["focused_text"] = {{"state", "owned_scope_changed_during_read"}};
                else if (text)
                    diagnostic["focused_text"] = {{"ok", true}, {"actual_text", text.value()}};
                else
                    diagnostic["focused_text"] = {{"ok", false},
                                                  {"error_code", std::int32_t(text.error().code())},
                                                  {"error", text.error().message()}};
            } else {
                diagnostic["focused_text"] = {{"state", "not_read_without_exact_owned_root_foreground"}};
            }
            trace.push_back(diagnostic);
            return fail(std::string(stage) + ": no post-action image");
        }
        observed = result.value().observation;
        return preserve(stage, observed.value());
    };
    const auto urlReadback = [&](const char* stage, const std::string& expected) {
        bool scopeIntact = true;
        std::string actual, error;
        const bool matched = waitUntil([&] {
            if (canceled()) {
                scopeIntact = false;
                return true;
            }
            const auto text = maiDesktopFocusedText(target);
            if (text) {
                actual = text.value();
                error.clear();
            } else
                error = text.error().message();
            // Chrome may elide only the http:// scheme in the focused omnibox.
            // Require the entire remaining host, port and nonce path exactly.
            return text && (actual == expected || actual == expected.substr(7));
        });
        trace.push_back({{"stage", stage},
                         {"actual_uia_url", actual},
                         {"expected_url", expected},
                         {"readback_error", error},
                         {"scope_intact", scopeIntact},
                         {"matched", matched && scopeIntact}});
        return matched && scopeIntact ? true : fail(std::string(stage) + ": exact owned URL was not read back");
    };
    bool passed = preserve("first-page", observed.value()) && action("first-address-focus", "key", "Ctrl+L") &&
                  urlReadback("first-url", server.url(false)) && action("second-url-type", "type", server.url(true)) &&
                  urlReadback("typed-second-url", server.url(true)) && action("navigate-second", "key", "Enter");
    if (passed) {
        bool scopeIntact = true;
        const bool loaded = waitUntil([&] {
            if (canceled()) {
                scopeIntact = false;
                return true;
            }
            return server.secondReady.load() > 0 && server.secondCookie.load();
        });
        if (!loaded || !scopeIntact)
            passed = fail("second local page/cookie proof did not complete in the same owned browser");
    }
    if (passed)
        passed = action("second-address-focus", "key", "Ctrl+L") &&
                 urlReadback("navigated-second-url", server.url(true)) && action("return-to-page", "key", "Escape");
    bool pageMarker = false;
    if (passed) {
        bool markerScopeIntact = true;
        pageMarker = waitUntil([&] {
            if (canceled()) {
                markerScopeIntact = false;
                return true;
            }
            const auto elements = maiDesktopElements(target);
            if (elements)
                for (const auto& element : elements.value())
                    if (element.label == server.secondMarker && element.width > 0 && element.height > 0 &&
                        element.x + element.width > display.x && element.y + element.height > display.y &&
                        element.x < display.x + display.width && element.y < display.y + display.height)
                        return true;
            return false;
        });
        pageMarker = pageMarker && markerScopeIntact;
        if (!pageMarker || canceled())
            passed = fail("second page marker was not present in the owned native accessibility tree");
    }
    if (passed) {
        const auto final =
            controller.observeDisplay(session, workspace.path().toStdString(), display.id, canceled, 0, 0, 500);
        if (!final)
            passed = fail(final.error().message());
        else
            passed = preserve("second-page-normal-wait-500ms", final.value());
    }
    const auto capture = maiScreenshotCaptureState();
    if (capture.workerStarts != 1 || capture.sessionStarts != 1 || capture.framesReceived < std::uint64_t(captures))
        passed = fail("browser observations did not retain one resident capture worker/session");
    const bool sameIdentity = browser.profileIntact();
    if (!sameIdentity) passed = fail("private browser process/profile identity changed before completion");
    const bool stopped = browser.close();
    const bool removed = stopped && profile.remove();
    if (!stopped || !removed) passed = fail("owned browser job/profile cleanup did not complete");
    const Json receipt{{"workflow", "native_private_browser_address_bar_navigation"},
                       {"visual_model", false},
                       {"startup", startup},
                       {"same_process_profile", sameIdentity},
                       {"page_marker_uia", pageMarker},
                       {"server", server.receipt()},
                       {"steps", trace},
                       {"failures", failures},
                       {"network_boundary", "per-process suppression/local refusing proxy, not OS isolation"},
                       {"owned_job_stopped", stopped},
                       {"private_profile_removed", removed},
                       {"worker_starts", capture.workerStarts},
                       {"session_starts", capture.sessionStarts},
                       {"frames_received", capture.framesReceived},
                       {"exit_code", passed ? 0 : 1}};
    if (!evidence.isEmpty()) {
        if (!writeJson(QDir(evidence).filePath(QStringLiteral("browser-workflow-receipt.json")), receipt))
            passed = false;
        std::fprintf(stderr, "native private browser evidence=%s\n", evidence.toUtf8().constData());
    }
    std::fprintf(stderr, "native private browser same-profile/Ctrl+L/cookie/UIA exit=%d; visual_model=false\n",
                 passed ? 0 : 1);
    return passed ? 0 : 1;
#endif
}

}  // namespace
