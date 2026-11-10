// Opt-in native display regression. Both processes and all editor contents are
// owned fixtures. An owned fullscreen editor backs the captured display;
// native file dialogs may show real Windows quick-access labels, so captures
// remain private evidence. A lost foreground cancels capture/input.
// This proves native execution, not a visual model's autonomous decisions.
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QShortcut>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QLabel>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <json.hpp>

#include "MaiChatTools/DesktopComputerPlatform.h"
#include "MaiChatTools/DesktopComputerTools.h"
#include "MaiScreenshot.h"

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#include <uiautomation.h>
#include <wrl/client.h>
#endif

namespace {
using Json = nlohmann::json;

bool writeJson(const QString& path, const Json& value) {
    QSaveFile file(path);
    const auto bytes = QByteArray::fromStdString(value.dump(2));
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

Json readJson(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? Json::parse(file.readAll().toStdString(), nullptr, false) : Json();
}

bool waitUntil(const std::function<bool()>& condition, int timeout = 5000) {
    QElapsedTimer elapsed;
    elapsed.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (condition()) return true;
        QThread::msleep(10);
    } while (elapsed.elapsed() < timeout);
    return condition();
}

int childEditor(const QString& title, const QString& statePath) {
    QWidget window;
    window.setWindowTitle(title);
    window.setStyleSheet(
        QStringLiteral("QWidget { background:#263147; color:white; font-size:24px; }"
                       "QLineEdit { background:white; color:#182335; padding:12px; }"));
    auto* layout = new QVBoxLayout(&window);
    layout->addStretch();
    layout->addWidget(new QLabel(QStringLiteral("MaiChat owned native display fixture"), &window));
    auto* input = new QLineEdit(&window);
    input->setAccessibleName(QStringLiteral("Display native editor"));
    layout->addWidget(input);
    auto* confirmed = new QLabel(QStringLiteral("Waiting for owned input"), &window);
    layout->addWidget(confirmed);
    layout->addStretch();
    int enterCount = 0;
    const auto save = [&] {
        writeJson(statePath, {{"pid", std::uint64_t(QCoreApplication::applicationPid())},
                              {"ready", window.isVisible()},
                              {"title", title.toStdString()},
                              {"text", input->text().toUtf8().toStdString()},
                              {"confirmed", confirmed->text().toUtf8().toStdString()},
                              {"enters", enterCount}});
    };
    QObject::connect(input, &QLineEdit::textChanged, &window, save);
    QObject::connect(input, &QLineEdit::returnPressed, &window, [&] {
        ++enterCount;
        confirmed->setText(input->text());
        save();
    });
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, &window, save);
    window.showFullScreen();
    heartbeat.start(50);
    QTimer::singleShot(0, &window, save);
    return QApplication::exec();
}

// A real document workflow stays separate from the original click/type/Enter
// regression. State records widget contents and completed I/O for assertions;
// every mutation below is reached through native UI input.
int childDocumentEditor(const QString& title, const QString& statePath, const QString& documentRoot) {
    const QString root = QFileInfo(documentRoot).canonicalFilePath();
    if (root.isEmpty()) return 1;
    QWidget window;
    // Cover unrelated floating surfaces with our own disposable backdrop;
    // never hide or change another application to make screenshot ownership
    // pass. The native guard still rejects any foreign surface above this root.
    window.setWindowFlag(Qt::WindowStaysOnTopHint);
    window.setWindowTitle(title);
    window.setStyleSheet(
        QStringLiteral("QWidget { background:#263147; color:white; font-size:22px; }"
                       "QPlainTextEdit { background:white; color:#182335; padding:12px; }"));
    auto* layout = new QVBoxLayout(&window);
    layout->addWidget(new QLabel(QStringLiteral("MaiChat owned document workflow fixture"), &window));
    auto* saveButton = new QPushButton(QStringLiteral("Save As (Ctrl+Shift+S)"), &window);
    auto* openButton = new QPushButton(QStringLiteral("Open (Ctrl+O)"), &window);
    layout->addWidget(saveButton);
    layout->addWidget(openButton);
    auto* input = new QPlainTextEdit(&window);
    input->setAccessibleName(QStringLiteral("Owned document editor"));
    layout->addWidget(input);
    auto* status = new QLabel(QStringLiteral("Unsaved owned document"), &window);
    layout->addWidget(status);
    QString savedPath;
    QString openedPath;
    QString dialogKind;
    QString error;
    int saves = 0;
    int opens = 0;
    const auto publish = [&] {
        writeJson(statePath, {{"pid", std::uint64_t(QCoreApplication::applicationPid())},
                              {"window_id", std::to_string(std::uint64_t(window.winId()))},
                              {"ready", window.isVisible()},
                              {"title", window.windowTitle().toStdString()},
                              {"text", input->toPlainText().toUtf8().toStdString()},
                              {"saved_path", savedPath.toUtf8().toStdString()},
                              {"opened_path", openedPath.toUtf8().toStdString()},
                              {"dialog", dialogKind.toStdString()},
                              {"saves", saves},
                              {"opens", opens},
                              {"error", error.toStdString()}});
    };
    const auto selectedOwnedPath = [&](const QString& selected) {
        const QFileInfo file(selected);
        // Native file dialogs remain native. The editor refuses any I/O whose
        // real parent is outside the directory created by this parent process.
        return !selected.isEmpty() && QFileInfo(file.absolutePath()).canonicalFilePath() == root &&
               file.fileName() == QStringLiteral("owned-document.txt");
    };
    const auto chooseDocument = [&](bool saving) {
        if (!dialogKind.isEmpty()) return;
        dialogKind = saving ? QStringLiteral("save") : QStringLiteral("open");
        error.clear();
        publish();
        QFileDialog dialog(
            &window, saving ? QStringLiteral("Save owned document") : QStringLiteral("Open owned document"), root);
        dialog.setAcceptMode(saving ? QFileDialog::AcceptSave : QFileDialog::AcceptOpen);
        dialog.setFileMode(saving ? QFileDialog::AnyFile : QFileDialog::ExistingFile);
        dialog.setNameFilter(QStringLiteral("Text files (*.txt)"));
        dialog.selectFile(QStringLiteral("choose-owned-file.txt"));
        // Do not set DontUseNativeDialog: the parent requires an owned Windows
        // #32770 modal and observes it through the production display backend.
        if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().size() != 1) {
            error = QStringLiteral("Owned document dialog was canceled");
        } else {
            const QString path = QDir::cleanPath(QFileInfo(dialog.selectedFiles().front()).absoluteFilePath());
            if (!selectedOwnedPath(path)) {
                error = QStringLiteral("Document path is outside the owned fixture");
            } else if (saving) {
                const QByteArray bytes = input->toPlainText().toUtf8();
                QSaveFile file(path);
                if (QFile::exists(path) || !file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
                    !file.commit()) {
                    error = QStringLiteral("Owned document could not be saved as a new file");
                } else {
                    savedPath = path;
                    ++saves;
                    window.setWindowTitle(title + QStringLiteral(" - owned-document.txt"));
                    status->setText(QStringLiteral("Saved owned-document.txt"));
                }
            } else {
                QFile file(path);
                if (!file.open(QIODevice::ReadOnly)) {
                    error = QStringLiteral("Owned document could not be reopened");
                } else {
                    const QByteArray bytes = file.readAll();
                    if (file.error() != QFile::NoError || QString::fromUtf8(bytes).toUtf8() != bytes) {
                        error = QStringLiteral("Owned document did not contain complete UTF-8 text");
                    } else {
                        input->setPlainText(QString::fromUtf8(bytes));
                        openedPath = path;
                        ++opens;
                        status->setText(QStringLiteral("Reopened owned-document.txt from disk"));
                    }
                }
            }
        }
        dialogKind.clear();
        input->setFocus();
        publish();
    };
    QObject::connect(saveButton, &QPushButton::clicked, &window, [&] { chooseDocument(true); });
    QObject::connect(openButton, &QPushButton::clicked, &window, [&] { chooseDocument(false); });
    auto* saveShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")), &window);
    auto* openShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+O")), &window);
    QObject::connect(saveShortcut, &QShortcut::activated, &window, [&] { chooseDocument(true); });
    QObject::connect(openShortcut, &QShortcut::activated, &window, [&] { chooseDocument(false); });
    QObject::connect(input, &QPlainTextEdit::textChanged, &window, publish);
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, &window, publish);
    window.showFullScreen();
    input->setFocus();
    heartbeat.start(50);
    QTimer::singleShot(0, &window, publish);
    return QApplication::exec();
}

struct OwnedProcess {
    QProcess process;
    ~OwnedProcess() {
        if (process.state() == QProcess::NotRunning) return;
        process.terminate();
        if (!process.waitForFinished(2000)) {
            process.kill();
            process.waitForFinished(2000);
        }
    }
};

int skip(const char* reason) {
    std::fprintf(stderr, "native display test SKIP: %s; no native acceptance claimed\n", reason);
    return 77;
}

int runParent() {
    if (qEnvironmentVariable("MAICHAT_RUN_COMPUTER_DISPLAY_NATIVE") != QStringLiteral("1"))
        return skip("set the explicit owned-desktop opt-in to run");
#if !defined(Q_OS_WIN)
    return skip("this owned foreground guard is Windows-only");
#else
    if (QApplication::platformName() != QStringLiteral("windows")) return skip("real Windows QPA required");
    QTemporaryDir workspace;
    if (!workspace.isValid()) return 1;
    const QString evidence = qEnvironmentVariable("MAICHAT_COMPUTER_NATIVE_EVIDENCE_DIR");
    if (!evidence.isEmpty() && !QDir().mkpath(evidence)) return 1;
    const QString statePath = workspace.filePath(QStringLiteral("owned-state.json"));
    const QString title =
        QStringLiteral("MaiChat display fixture %1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    OwnedProcess child;
    child.process.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--editor"), title, statePath});
    if (!child.process.waitForStarted(5000)) return 1;
    const auto childPid = std::uint64_t(child.process.processId());
    if (!waitUntil([&] {
            const auto state = readJson(statePath);
            return state.is_object() && state.value("ready", false) && state.value("pid", std::uint64_t(0)) == childPid;
        }))
        return 1;
    DesktopNativeWindow target;
    const auto windows = maiDesktopWindows();
    if (!windows) return skip(windows.error().message().c_str());
    for (const auto& window : windows.value()) {
        if (window.processId == childPid && window.title == title.toStdString()) target = window;
    }
    if (!target.id) return skip("owned fullscreen editor is not an enumerable window");
    const HWND handle = reinterpret_cast<HWND>(std::uintptr_t(target.id));
    const auto ownedForeground = [&] {
        DWORD pid = 0;
        return child.process.state() == QProcess::Running && GetForegroundWindow() == handle &&
               GetWindowThreadProcessId(handle, &pid) != 0 && pid == childPid;
    };
    if (!ownedForeground()) return skip("owned editor did not obtain foreground; no forced activation");
    for (int key = 1; key < 255; ++key) {
        if (GetAsyncKeyState(key) & 0x8000) return skip("physical input is currently held");
    }
    DesktopComputerController controller;
    const auto displays = controller.displays();
    if (!displays) return skip(displays.error().message().c_str());
    DesktopNativeDisplay display;
    for (const auto& candidate : displays.value()) {
        // Do not capture an uncovered desktop or infer pixels from logical Qt
        // sizes. Production enumeration and UIA use physical pointer bounds.
        if (target.x <= candidate.x && target.y <= candidate.y &&
            target.x + target.width >= candidate.x + candidate.width &&
            target.y + target.height >= candidate.y + candidate.height)
            display = candidate;
    }
    if (!display.id) return skip("owned fullscreen window does not cover one native display");
    const auto elements = maiDesktopElements(target);
    if (!elements) return skip(elements.error().message().c_str());
    DesktopNativeElement editor;
    for (const auto& element : elements.value()) {
        if (element.label == "Display native editor") editor = element;
    }
    if (editor.width <= 0 || editor.height <= 0) return skip("owned editor has no UIA bounds");
    const double x = editor.x + editor.width / 2;
    const double y = editor.y + editor.height / 2;
    const auto canceled = [&] {
        const POINT point{LONG(x), LONG(y)};
        return !ownedForeground() || GetAncestor(WindowFromPoint(point), GA_ROOT) != handle;
    };
    auto observed = controller.observeDisplay("owned-display", workspace.path().toStdString(), display.id, canceled);
    if (!observed) return skip(observed.error().message().c_str());
    Json trace = Json::array();
    int result = 0;
    int captures = 0;
    const auto preserve = [&](const DesktopComputerObservation& image) {
        if (evidence.isEmpty()) return true;
        return QFile::copy(QString::fromStdString(image.imagePath),
                           QDir(evidence).filePath(QStringLiteral("display-%1.png").arg(captures++)));
    };
    if (!preserve(observed.value())) return 1;
    const std::string expected = QStringLiteral("Native display 中文 input verified").toUtf8().toStdString();
    for (const char* kind : {"click", "type", "key"}) {
        if (canceled()) {
            result = 1;
            break;
        }
        const auto snapshot = Json::parse(observed.value().json);
        DesktopComputerAction action;
        action.desktopScope = true;
        action.workspace = workspace.path().toStdString();
        action.displayId = display.id;
        action.snapshotId = snapshot.value("snapshot_id", std::string());
        action.action = kind;
        action.xRatio = (x - display.x) / (display.width - 1);
        action.yRatio = (y - display.y) / (display.height - 1);
        action.text = expected;
        action.key = "Enter";
        action.isCanceled = canceled;
        const auto sent = controller.perform("owned-display", action);
        if (!sent) {
            trace.push_back({{"action", kind}, {"error", sent.error().message()}});
            result = 1;
            break;
        }
        trace.push_back(
            {{"action", kind}, {"state", sent.value().state}, {"observation_error", sent.value().observationError}});
        if (!sent.value().dispatched || sent.value().observation.imagePath.empty()) {
            result = 1;
            break;
        }
        const auto next = Json::parse(sent.value().observation.json);
        if (next.value("frame_sequence", std::uint64_t(0)) <= snapshot.value("frame_sequence", std::uint64_t(0)) ||
            !preserve(sent.value().observation)) {
            result = 1;
            break;
        }
        observed = sent.value().observation;
    }
    const bool confirmed = result == 0 && waitUntil([&] {
                               const auto state = readJson(statePath);
                               return state.is_object() && state.value("pid", std::uint64_t(0)) == childPid &&
                                      state.value("text", std::string()) == expected &&
                                      state.value("confirmed", std::string()) == expected &&
                                      state.value("enters", 0) == 1;
                           });
    if (!confirmed) result = 1;
    if (result == 0) {
        const auto text = maiDesktopFocusedText(target);
        if (!text || text.value() != expected) result = 1;
    }
    const auto capture = maiScreenshotCaptureState();
    // Four observations must reuse the original resident worker/session; a
    // restart cannot be used to manufacture a post-action frame sequence.
    if (capture.workerStarts != 1 || capture.sessionStarts != 1 || capture.framesReceived < 4) result = 1;
    if (!evidence.isEmpty()) {
        writeJson(QDir(evidence).filePath(QStringLiteral("display-native-receipt.json")),
                  {{"backend", "production_native_display"},
                   {"visual_model", false},
                   {"child_pid", childPid},
                   {"display_id", std::to_string(display.id)},
                   {"steps", trace},
                   {"worker_starts", capture.workerStarts},
                   {"session_starts", capture.sessionStarts},
                   {"frames_received", capture.framesReceived},
                   {"owned_state", readJson(statePath)},
                   {"exit_code", result}});
    }
    std::fprintf(stderr, "native display owned click/type/Enter/fresh WGC/readback exit=%d\n", result);
    return result;
#endif
}

#if defined(Q_OS_WIN)
HRESULT queryDwmCloaking(HWND window, DWORD& cloaked) {
    // One trusted System32 module supplies both the guard and its diagnostic
    // metadata. Keep it loaded while the cached function pointer is in use.
    struct DwmApi {
        HMODULE module = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        decltype(&DwmGetWindowAttribute) attribute =
            module ? reinterpret_cast<decltype(&DwmGetWindowAttribute)>(GetProcAddress(module, "DwmGetWindowAttribute"))
                   : nullptr;
        ~DwmApi() {
            if (module) FreeLibrary(module);
        }
    };
    static const DwmApi api;
    cloaked = 0;
    return api.attribute ? api.attribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)) : E_NOTIMPL;
}

// The process handle is created by this test, and the root HWND comes from its
// private state channel. Titles and application names never confer ownership.
// A native modal is allowed only through its same-process GW_OWNER chain.
struct OwnedDocumentDesktop {
    QProcess& process;
    HWND root;
    std::uint64_t pid;
    DesktopNativeDisplay display;

    bool owns(HWND window) const {
        if (process.state() != QProcess::Running || !IsWindow(root)) return false;
        HWND current = GetAncestor(window, GA_ROOT);
        for (int depth = 0; current && depth < 64; ++depth) {
            DWORD actualPid = 0;
            if (!GetWindowThreadProcessId(current, &actualPid) || actualPid != pid) return false;
            if (current == root) return true;
            current = GetWindow(current, GW_OWNER);
        }
        return false;
    }

    bool ownsPixels(HWND window) const {
        if (process.state() != QProcess::Running || !IsWindow(root) || !IsWindow(window)) return false;
        DWORD rootPid = 0;
        DWORD windowPid = 0;
        if (!GetWindowThreadProcessId(root, &rootPid) || rootPid != pid) return false;
        // Native dialogs can create ornamental windows (for example shadows)
        // without a GW_OWNER chain. Their pixels still belong to the exact
        // process this test started. This never grants foreground/input rights:
        // owns() retains its strict root/modal owner-chain contract.
        return GetWindowThreadProcessId(window, &windowPid) != 0 && windowPid == pid;
    }

    const char* blockingReason(HWND* blockingSurface = nullptr) const {
        if (blockingSurface) *blockingSurface = nullptr;
        const HWND foreground = GetForegroundWindow();
        if (!owns(foreground)) {
            if (blockingSurface) *blockingSurface = foreground;
            return "foreground_not_owned";
        }
        RECT rootBounds{};
        if (!GetWindowRect(root, &rootBounds) || rootBounds.left > display.x || rootBounds.top > display.y ||
            rootBounds.right < display.x + display.width || rootBounds.bottom < display.y + display.height)
            return "owned_root_not_fullscreen";
        // Fullscreen geometry alone does not prove that captured pixels are
        // ours: a foreign top-level notification or other overlay cancels the
        // fixture. Windows behind the owned fullscreen root are not captured.
        for (HWND window = GetTopWindow(nullptr); window; window = GetWindow(window, GW_HWNDNEXT)) {
            if (window == root) return nullptr;
            if (!IsWindowVisible(window) || IsIconic(window)) continue;
            DWORD cloaked = 0;
            // IsWindowVisible includes DWM-cloaked helper/virtual-desktop
            // surfaces not visible in the desktop image. Only a successful
            // affirmative DWM query excludes one; failure remains strict.
            if (SUCCEEDED(queryDwmCloaking(window, cloaked)) && cloaked != 0) continue;
            RECT bounds{};
            if (!GetWindowRect(window, &bounds)) {
                if (blockingSurface) *blockingSurface = window;
                return "surface_geometry_unavailable";
            }
            const bool intersects = bounds.right > display.x && bounds.bottom > display.y &&
                                    bounds.left < display.x + display.width && bounds.top < display.y + display.height;
            if (intersects && !ownsPixels(window)) {
                if (blockingSurface) *blockingSurface = window;
                return "foreign_surface_above_owned_root";
            }
        }
        return "owned_root_missing_from_z_order";
    }

    bool canceled() const {
        return blockingReason() != nullptr;
    }

    Json describeBlockingSurface(HWND window) const {
        wchar_t className[256]{};
        const int classLength = GetClassNameW(window, className, 256);
        DWORD actualPid = 0;
        const bool hasPid = GetWindowThreadProcessId(window, &actualPid) != 0;
        RECT bounds{};
        const bool hasBounds = GetWindowRect(window, &bounds) != 0;
        Json metadata{{"window_id", std::to_string(std::uint64_t(reinterpret_cast<std::uintptr_t>(window)))},
                      {"window_valid", IsWindow(window) != 0},
                      {"native_class", QString::fromWCharArray(className, classLength).toUtf8().toStdString()},
                      {"style", std::uint32_t(GetWindowLongPtrW(window, GWL_STYLE))},
                      {"exstyle", std::uint32_t(GetWindowLongPtrW(window, GWL_EXSTYLE))},
                      {"same_owned_pid", hasPid && actualPid == pid},
                      {"geometry", nullptr},
                      {"dwm_cloaked", nullptr},
                      {"dwm_cloaked_flags", nullptr}};
        if (hasBounds)
            metadata["geometry"] = {{"x", bounds.left},
                                    {"y", bounds.top},
                                    {"width", bounds.right - bounds.left},
                                    {"height", bounds.bottom - bounds.top}};
        DWORD cloaked = 0;
        const HRESULT result = queryDwmCloaking(window, cloaked);
        if (SUCCEEDED(result)) {
            metadata["dwm_cloaked"] = cloaked != 0;
            metadata["dwm_cloaked_flags"] = cloaked;
        }
        metadata["dwm_query_hresult"] = std::int32_t(result);
        return metadata;
    }

    HWND nativeModal() const {
        const HWND foreground = GetForegroundWindow();
        wchar_t className[64]{};
        if (foreground == root || !owns(foreground) || !GetClassNameW(foreground, className, 64)) return nullptr;
        return QString::fromWCharArray(className) == QStringLiteral("#32770") ? foreground : nullptr;
    }

    static bool physicalInputReleased() {
        for (int key = 1; key < 255; ++key)
            if (GetAsyncKeyState(key) & 0x8000) return false;
        return true;
    }
};

DesktopNativeWindow nativeWindow(HWND handle, std::uint64_t pid) {
    RECT bounds{};
    GetWindowRect(handle, &bounds);
    DesktopNativeWindow window;
    window.id = std::uint64_t(reinterpret_cast<std::uintptr_t>(handle));
    window.processId = pid;
    window.x = bounds.left;
    window.y = bounds.top;
    window.width = bounds.right - bounds.left;
    window.height = bounds.bottom - bounds.top;
    return window;
}

Json ownedFocusedElement(const OwnedDocumentDesktop& owned, HWND expectedModal) {
    using Microsoft::WRL::ComPtr;
    if (owned.canceled() || owned.nativeModal() != expectedModal) return {{"state", "owned_modal_changed"}};
    struct ComApartment {
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~ComApartment() {
            if (SUCCEEDED(result)) CoUninitialize();
        }
    } apartment;
    ComPtr<IUIAutomation> automation;
    HRESULT result = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_IUIAutomation,
                                      reinterpret_cast<void**>(automation.GetAddressOf()));
    if (FAILED(result)) return {{"state", "uia_unavailable"}, {"hresult", std::int32_t(result)}};
    ComPtr<IUIAutomationElement> focused;
    result = automation->GetFocusedElement(focused.GetAddressOf());
    if (FAILED(result) || !focused)
        return {{"state", "focused_element_unavailable"}, {"hresult", std::int32_t(result)}};
    int processId = 0;
    if (FAILED(focused->get_CurrentProcessId(&processId)) || std::uint64_t(processId) != owned.pid)
        return {{"state", "focused_element_not_owned"}};
    ComPtr<IUIAutomationElement> root;
    ComPtr<IUIAutomationTreeWalker> walker;
    if (FAILED(automation->ElementFromHandle(expectedModal, root.GetAddressOf())) ||
        FAILED(automation->get_ControlViewWalker(walker.GetAddressOf())) || !root || !walker)
        return {{"state", "owned_ancestry_unavailable"}};
    ComPtr<IUIAutomationElement> current = focused;
    bool belongs = false;
    for (int depth = 0; current && depth < 128; ++depth) {
        BOOL same = FALSE;
        if (SUCCEEDED(automation->CompareElements(current.Get(), root.Get(), &same)) && same) {
            belongs = true;
            break;
        }
        ComPtr<IUIAutomationElement> parent;
        walker->GetParentElement(current.Get(), parent.GetAddressOf());
        current = parent;
    }
    if (!belongs) return {{"state", "focused_element_outside_owned_modal"}};
    // Read identifying properties only after proving this is a descendant of
    // the exact owned modal. Do not enumerate sidebar items or other windows.
    const auto stringProperty = [&](auto getter) {
        BSTR value = nullptr;
        const HRESULT status = (focused.Get()->*getter)(&value);
        const std::string text = SUCCEEDED(status) && value
                                     ? QString::fromWCharArray(value, int(SysStringLen(value))).toUtf8().toStdString()
                                     : std::string();
        if (value) SysFreeString(value);
        return text;
    };
    CONTROLTYPEID controlType = 0;
    BOOL hasFocus = FALSE;
    BOOL focusable = FALSE;
    UIA_HWND nativeHandle = nullptr;
    RECT bounds{};
    focused->get_CurrentControlType(&controlType);
    focused->get_CurrentHasKeyboardFocus(&hasFocus);
    focused->get_CurrentIsKeyboardFocusable(&focusable);
    focused->get_CurrentNativeWindowHandle(&nativeHandle);
    focused->get_CurrentBoundingRectangle(&bounds);
    const Json metadata{
        {"state", "owned_modal_descendant"},
        {"control_type", controlType},
        {"name", stringProperty(&IUIAutomationElement::get_CurrentName)},
        {"automation_id", stringProperty(&IUIAutomationElement::get_CurrentAutomationId)},
        {"native_class", stringProperty(&IUIAutomationElement::get_CurrentClassName)},
        {"has_keyboard_focus", hasFocus != FALSE},
        {"keyboard_focusable", focusable != FALSE},
        {"native_window_id", std::to_string(std::uint64_t(reinterpret_cast<std::uintptr_t>(nativeHandle)))},
        {"geometry",
         {{"x", bounds.left},
          {"y", bounds.top},
          {"width", bounds.right - bounds.left},
          {"height", bounds.bottom - bounds.top}}}};
    if (owned.canceled() || owned.nativeModal() != expectedModal) return {{"state", "owned_modal_changed"}};
    return metadata;
}
#endif

int runDocumentWorkflow() {
    if (qEnvironmentVariable("MAICHAT_RUN_COMPUTER_DISPLAY_NATIVE") != QStringLiteral("1"))
        return skip("set the explicit owned-desktop opt-in to run the document workflow");
#if !defined(Q_OS_WIN)
    return skip("native document ownership checks require Windows");
#else
    if (QApplication::platformName() != QStringLiteral("windows")) return skip("real Windows QPA required");
    QTemporaryDir workspace;
    if (!workspace.isValid()) return 1;
    const QString documents = workspace.filePath(QStringLiteral("documents"));
    if (!QDir().mkpath(documents)) return 1;
    const QString documentPath = QDir(documents).filePath(QStringLiteral("owned-document.txt"));
    const QString statePath = workspace.filePath(QStringLiteral("document-state.json"));
    const QString title =
        QStringLiteral("MaiChat document fixture %1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QString evidence = qEnvironmentVariable("MAICHAT_COMPUTER_NATIVE_EVIDENCE_DIR");
    if (!evidence.isEmpty()) {
        evidence = QDir(evidence).filePath(
            QStringLiteral("document-workflow-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        if (!QDir().mkpath(evidence)) return 1;
    }
    OwnedProcess child;
    child.process.start(QCoreApplication::applicationFilePath(),
                        {QStringLiteral("--document-editor"), title, statePath, documents});
    if (!child.process.waitForStarted(5000)) return 1;
    const auto pid = std::uint64_t(child.process.processId());
    if (!waitUntil([&] {
            const auto state = readJson(statePath);
            return state.is_object() && state.value("ready", false) && state.value("pid", std::uint64_t(0)) == pid;
        }))
        return 1;
    const auto ready = readJson(statePath);
    DesktopNativeWindow target;
    const auto windows = maiDesktopWindows();
    if (!windows) return skip(windows.error().message().c_str());
    for (const auto& window : windows.value())
        if (window.processId == pid && std::to_string(window.id) == ready.value("window_id", std::string()))
            target = window;
    if (!target.id) return skip("owned document root is not an enumerable native window");
    DesktopComputerController controller;
    const auto displays = controller.displays();
    if (!displays) return skip(displays.error().message().c_str());
    DesktopNativeDisplay display;
    for (const auto& candidate : displays.value())
        if (target.x <= candidate.x && target.y <= candidate.y &&
            target.x + target.width >= candidate.x + candidate.width &&
            target.y + target.height >= candidate.y + candidate.height)
            display = candidate;
    if (!display.id) return skip("owned document root does not cover a native display");
    OwnedDocumentDesktop owned{child.process, reinterpret_cast<HWND>(std::uintptr_t(target.id)), pid, display};
    // QWidget visibility can precede Windows foreground/z-order readiness.
    // Wait only for the exact guard used by subsequent capture and input;
    // never activate a window, dismiss an overlay, or release physical input.
    bool waitConfigured = false;
    const int waitSeconds = qEnvironmentVariableIntValue("MAICHAT_COMPUTER_NATIVE_WAIT_FOCUS_SECONDS", &waitConfigured);
    const int waitMs = waitConfigured && waitSeconds >= 0 ? (std::min)(waitSeconds, 30) * 1000 : 5000;
    QElapsedTimer waiting;
    QElapsedTimer stable;
    waiting.start();
    Json blockedCounts = Json::object();
    Json blockingSurfaces = Json::object();
    std::string lastBlock;
    bool nativeReady = false;
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (!owned.owns(owned.root)) return 1;
        HWND blockingSurface = nullptr;
        const char* blocked = owned.blockingReason(&blockingSurface);
        if (!blocked && !OwnedDocumentDesktop::physicalInputReleased()) blocked = "physical_input_held";
        if (!blocked) {
            if (!stable.isValid()) stable.start();
            if (stable.elapsed() >= (waitMs == 0 ? 0 : 200)) {
                nativeReady = true;
                break;
            }
        } else {
            lastBlock = blocked;
            blockedCounts[lastBlock] = blockedCounts.value(lastBlock, 0) + 1;
            if (blockingSurface) {
                const Json metadata = owned.describeBlockingSurface(blockingSurface);
                const std::string id = metadata["window_id"].get<std::string>();
                if (!blockingSurfaces.contains(id))
                    blockingSurfaces[id] = {
                        {"first_observation", metadata}, {"checks", 0}, {"first_elapsed_ms", waiting.elapsed()}};
                auto& surface = blockingSurfaces[id];
                surface["last_observation"] = metadata;
                surface["checks"] = surface.value("checks", 0) + 1;
                surface["last_elapsed_ms"] = waiting.elapsed();
            }
            stable.invalidate();
        }
        if (waiting.elapsed() >= waitMs) break;
        QThread::msleep(10);
    } while (waiting.elapsed() <= waitMs);
    const Json readiness{{"child_pid", pid},
                         {"root_window_id", std::to_string(target.id)},
                         {"display_id", std::to_string(display.id)},
                         {"wait_ms", waitMs},
                         {"elapsed_ms", waiting.elapsed()},
                         {"stable_ms", stable.isValid() ? stable.elapsed() : 0},
                         {"state", nativeReady ? "owned_foreground_ready" : "environment_not_ready"},
                         {"blocked_checks", blockedCounts},
                         {"blocking_surfaces", blockingSurfaces},
                         {"last_block", lastBlock},
                         {"input_dispatched", false},
                         {"pixels_captured", false}};
    if (!evidence.isEmpty() &&
        !writeJson(QDir(evidence).filePath(QStringLiteral("document-workflow-readiness.json")), readiness))
        return 1;
    std::fprintf(stderr, "owned document readiness=%s elapsed_ms=%lld last_block=%s\n",
                 nativeReady ? "ready" : "environment_not_ready", static_cast<long long>(waiting.elapsed()),
                 lastBlock.c_str());
    if (!nativeReady)
        return skip("timed out waiting for unchanged owned foreground/coverage/input guards; environment boundary");
    const auto elements = maiDesktopElements(target);
    if (!elements) return skip(elements.error().message().c_str());
    DesktopNativeElement editor;
    for (const auto& element : elements.value())
        if (element.label == "Owned document editor") editor = element;
    if (editor.width <= 0 || editor.height <= 0) return skip("owned document editor has no UIA bounds");
    const double editorX = editor.x + editor.width / 2;
    const double editorY = editor.y + editor.height / 2;
    const std::string session = "owned-document-workflow";
    auto observed = controller.observeDisplay(session, workspace.path().toStdString(), display.id,
                                              [&] { return owned.canceled(); });
    if (!observed) return skip(observed.error().message().c_str());
    Json trace = Json::array();
    Json failures = Json::array();
    Json modals = Json::array();
    int screenshots = 0;
    const auto fail = [&](const std::string& reason) {
        failures.push_back(reason);
        return false;
    };
    const auto preserve = [&](const char* stage, const DesktopComputerObservation& observation) {
        const QString name = QStringLiteral("document-%1-%2.png").arg(screenshots++).arg(QString::fromUtf8(stage));
        if (!evidence.isEmpty() &&
            !QFile::copy(QString::fromStdString(observation.imagePath), QDir(evidence).filePath(name)))
            return fail("could not preserve an owned display screenshot");
        trace.push_back(
            {{"stage", stage}, {"image", name.toStdString()}, {"observation", Json::parse(observation.json)}});
        return true;
    };
    const auto refresh = [&](const char* stage, int waitMs = 100) {
        const auto next = controller.observeDisplay(
            session, workspace.path().toStdString(), display.id, [&] { return owned.canceled(); }, 0, 0, waitMs);
        if (!next) return fail(std::string(stage) + ": " + next.error().message());
        observed = next.value();
        return preserve(stage, observed.value());
    };
    const auto action = [&](const char* stage, const char* kind, const std::string& value = std::string()) {
        const bool positioned = std::string(kind) == "click";
        const POINT point{LONG(editorX), LONG(editorY)};
        QElapsedTimer actionTime;
        actionTime.start();
        Json cancellation;
        const char* cancellationPhase = "before_input";
        const auto recordCancellation = [&](const char* reason, HWND blockingSurface) {
            const Json sample{
                {"phase", cancellationPhase},
                {"elapsed_ms", actionTime.elapsed()},
                {"reason", reason},
                {"owned_pid", pid},
                {"owned_root", owned.describeBlockingSurface(owned.root)},
                {"foreground_owned", owned.owns(GetForegroundWindow())},
                {"blocking_surface", blockingSurface ? owned.describeBlockingSurface(blockingSurface) : Json(nullptr)}};
            if (cancellation.is_null()) cancellation = {{"checks", 0}, {"first", sample}};
            cancellation["checks"] = cancellation.value("checks", 0) + 1;
            cancellation["latest"] = sample;
        };
        const auto canceled = [&] {
            HWND blockingSurface = nullptr;
            const char* reason = owned.blockingReason(&blockingSurface);
            const HWND pointerSurface = !reason && positioned ? WindowFromPoint(point) : nullptr;
            if (!reason && positioned && !owned.owns(pointerSurface)) {
                reason = "pointer_surface_not_owned";
                blockingSurface = GetAncestor(pointerSurface, GA_ROOT);
            }
            if (!reason) return false;
            // Capture the actual blocker synchronously in the callback: a
            // transient popup can disappear before perform() returns. Record
            // structure only; never query a foreign window title or contents.
            recordCancellation(reason, blockingSurface);
            return true;
        };
        if (canceled() || !OwnedDocumentDesktop::physicalInputReleased()) {
            if (cancellation.is_null()) recordCancellation("physical_input_held", nullptr);
            trace.push_back({{"stage", stage}, {"action", kind}, {"cancellation", cancellation}});
            return fail(std::string(stage) + ": owned foreground or released physical input was lost");
        }
        DesktopComputerAction input;
        input.desktopScope = true;
        input.workspace = workspace.path().toStdString();
        input.displayId = display.id;
        input.snapshotId = Json::parse(observed.value().json).value("snapshot_id", std::string());
        input.action = kind;
        input.xRatio = (editorX - display.x) / (display.width - 1);
        input.yRatio = (editorY - display.y) / (display.height - 1);
        if (input.action == "key")
            input.key = value;
        else
            input.text = value;
        input.isCanceled = canceled;
        cancellationPhase = "perform_callback";
        const auto sent = controller.perform(session, input);
        if (!sent) {
            trace.push_back({{"stage", stage},
                             {"action", kind},
                             {"error_code", std::int32_t(sent.error().code())},
                             {"error", sent.error().message()},
                             {"cancellation", cancellation}});
            return fail(std::string(stage) + ": " + sent.error().message());
        }
        trace.push_back({{"stage", stage},
                         {"action", kind},
                         {"state", sent.value().state},
                         {"dispatched", sent.value().dispatched},
                         {"cancellation", cancellation},
                         {"observation_error", sent.value().observationError}});
        if (!sent.value().dispatched || sent.value().observation.imagePath.empty())
            return fail(std::string(stage) + ": input did not produce its required image receipt");
        const auto before = Json::parse(observed.value().json);
        const auto after = Json::parse(sent.value().observation.json);
        if (after.value("frame_sequence", std::uint64_t(0)) <= before.value("frame_sequence", std::uint64_t(0)))
            return fail(std::string(stage) + ": post-input frame sequence did not advance");
        observed = sent.value().observation;
        return preserve(stage, observed.value());
    };
    const auto awaitState = [&](const char* stage, const std::function<bool(const Json&)>& condition) {
        bool valid = true;
        const bool reached = waitUntil([&] {
            const auto state = readJson(statePath);
            if (owned.canceled() || (state.is_object() && !state.value("error", std::string()).empty())) {
                valid = false;
                return true;
            }
            return state.is_object() && state.value("pid", std::uint64_t(0)) == pid && condition(state);
        });
        if (!valid || !reached) return fail(std::string(stage) + ": owned UI condition did not complete");
        trace.push_back({{"stage", stage}, {"owned_state", readJson(statePath)}});
        return true;
    };
    const auto chooseFile = [&](const char* mode) {
        if (!awaitState(mode, [&](const Json& state) {
                return state.value("dialog", std::string()) == mode && owned.nativeModal();
            }))
            return false;
        const HWND modal = owned.nativeModal();
        modals.push_back({{"kind", mode},
                          {"window_id", std::to_string(std::uint64_t(reinterpret_cast<std::uintptr_t>(modal)))},
                          {"native_class", "#32770"},
                          {"same_process_owner_chain", true}});
        if (!refresh("native-dialog") || !action("filename-focus", "key", "Alt+N") ||
            !action("filename-select", "key", "Ctrl+A") ||
            !action("filename-type", "type", QDir::toNativeSeparators(documentPath).toUtf8().toStdString()))
            return false;
        QElapsedTimer readbackTime;
        readbackTime.start();
        Json observations = Json::array();
        int readbackChecks = 0;
        bool modalUnchanged = true;
        // A post-input compositor frame can precede consumption of the queued
        // characters. Poll the same exact full-path assertion without sending
        // another character, shortcut or Enter, and stop on any modal change.
        const bool pathMatched = waitUntil([&] {
            if (owned.canceled() || owned.nativeModal() != modal) {
                modalUnchanged = false;
                return true;
            }
            const auto filename = maiDesktopFocusedText(nativeWindow(modal, pid));
            ++readbackChecks;
            Json readback{{"ok", bool(filename)}, {"focused_element", ownedFocusedElement(owned, modal)}};
            if (filename)
                readback["actual_text"] = filename.value();
            else {
                readback["error_code"] = std::int32_t(filename.error().code());
                readback["error"] = filename.error().message();
            }
            if (observations.empty() || observations.back()["readback"] != readback)
                observations.push_back({{"elapsed_ms", readbackTime.elapsed()}, {"readback", readback}});
            if (owned.canceled() || owned.nativeModal() != modal) {
                modalUnchanged = false;
                return true;
            }
            return filename && QDir::cleanPath(QDir::fromNativeSeparators(QString::fromStdString(filename.value()))) ==
                                   documentPath;
        });
        trace.push_back({{"stage", "filename-readback"},
                         {"dialog", mode},
                         {"checks", readbackChecks},
                         {"elapsed_ms", readbackTime.elapsed()},
                         {"modal_unchanged", modalUnchanged},
                         {"path_matched", pathMatched && modalUnchanged},
                         {"observations", observations}});
        if (!modalUnchanged) return fail("filename readback lost the unchanged verified native modal");
        if (!pathMatched) return fail("native filename edit did not independently read back the requested owned path");
        return action("filename-confirm", "key", "Enter");
    };
    const QString expected = QStringLiteral("Owned native document 中文 first line\nReopened from disk, line 2.");
    const auto diskMatches = [&] {
        QFile file(documentPath);
        return file.open(QIODevice::ReadOnly) && file.readAll() == expected.toUtf8() && file.error() == QFile::NoError;
    };
    bool passed =
        preserve("initial", observed.value()) && action("editor-focus", "click") &&
        action("first-line", "type", QStringLiteral("Owned native document 中文 first line").toUtf8().toStdString()) &&
        action("new-line", "key", "Enter") && action("second-line", "type", "Reopened from disk, line 2.") &&
        awaitState(
            "typed-document",
            [&](const Json& state) { return state.value("text", std::string()) == expected.toUtf8().toStdString(); }) &&
        refresh("typed-document") && action("save-as", "key", "Ctrl+Shift+S") && chooseFile("save") &&
        awaitState("saved-document", [&](const Json& state) {
            return state.value("saves", 0) == 1 && state.value("dialog", std::string()).empty() &&
                   state.value("saved_path", std::string()) == documentPath.toUtf8().toStdString();
        });
    if (passed && !diskMatches()) passed = fail("independent disk read after Save As did not match both lines");
    if (passed) {
        passed = refresh("saved-document") && action("clear-select", "key", "Ctrl+A") &&
                 action("clear-delete", "key", "Backspace") &&
                 awaitState("cleared-document",
                            [](const Json& state) {
                                return state.value("text", std::string()).empty() && state.value("saves", 0) == 1 &&
                                       state.value("opens", 0) == 0;
                            }) &&
                 refresh("cleared-document");
        if (passed && !diskMatches()) passed = fail("clearing the editor unexpectedly changed the saved file");
    }
    if (passed) {
        passed = action("open-document", "key", "Ctrl+O") && chooseFile("open") &&
                 awaitState("reopened-document", [&](const Json& state) {
                     return state.value("opens", 0) == 1 && state.value("dialog", std::string()).empty() &&
                            state.value("opened_path", std::string()) == documentPath.toUtf8().toStdString() &&
                            state.value("text", std::string()) == expected.toUtf8().toStdString();
                 });
    }
    std::string actualUiText;
    if (passed) {
        // Save As changes the title. Read the current record by the already
        // verified native identity rather than retaining pre-save metadata.
        const auto current = controller.windows();
        if (!current) {
            passed = fail("could not refresh the owned document window after reopen");
        } else {
            bool found = false;
            for (const auto& window : current.value()) {
                if (window.id == target.id && window.processId == pid) {
                    target = window;
                    found = true;
                }
            }
            if (!found) passed = fail("owned document identity disappeared after reopen");
        }
    }
    if (passed) {
        const auto text = maiDesktopFocusedText(target);
        if (text) actualUiText = text.value();
        // Windows accessibility may encode paragraph breaks as CRLF. No other
        // trimming or substring matching is permitted for this independent read.
        if (!text ||
            QString::fromStdString(actualUiText).replace(QStringLiteral("\r\n"), QStringLiteral("\n")) != expected)
            passed = fail("reopened editor UIA text did not match the complete document");
        if (passed && !diskMatches()) passed = fail("independent final disk read did not match the reopened UI");
        // Functional completion is already checked above. A normal display
        // observation wait lets the native Open dialog's closing fade settle
        // before preserving the final visual evidence; input/guards are unchanged.
        if (passed) passed = refresh("reopened-document-normal-wait-500ms", 500);
    }
    const auto capture = maiScreenshotCaptureState();
    if (capture.workerStarts != 1 || capture.sessionStarts != 1 || capture.framesReceived < std::uint64_t(screenshots))
        passed = fail("document observations did not reuse one resident native capture worker and session");
    if (!evidence.isEmpty()) {
        if (QFile::exists(documentPath) &&
            !QFile::copy(documentPath, QDir(evidence).filePath(QStringLiteral("saved-document.txt"))))
            passed = fail("could not preserve the independently read owned document");
        const Json receipt{
            {"backend", "production_native_display"},
            {"workflow", "save_as_clear_reopen"},
            {"visual_model", false},
            {"child_pid", pid},
            {"root_window_id", std::to_string(target.id)},
            {"display_id", std::to_string(display.id)},
            {"native_dialogs", modals},
            {"steps", trace},
            {"failures", failures},
            {"final_owned_state", readJson(statePath)},
            {"actual_uia_text", actualUiText},
            {"disk_matches", diskMatches()},
            {"expected_sha256",
             QCryptographicHash::hash(expected.toUtf8(), QCryptographicHash::Sha256).toHex().toStdString()},
            {"worker_starts", capture.workerStarts},
            {"session_starts", capture.sessionStarts},
            {"frames_received", capture.framesReceived},
            {"exit_code", passed ? 0 : 1}};
        if (!writeJson(QDir(evidence).filePath(QStringLiteral("document-workflow-receipt.json")), receipt))
            passed = false;
        std::fprintf(stderr, "native document workflow evidence=%s\n", evidence.toUtf8().constData());
    }
    std::fprintf(stderr, "native owned Save As/clear/reopen/disk/UIA workflow exit=%d; visual_model=false\n",
                 passed ? 0 : 1);
    return passed ? 0 : 1;
#endif
}
}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const auto arguments = app.arguments();
    if (arguments.size() == 4 && arguments[1] == QStringLiteral("--editor"))
        return childEditor(arguments[2], arguments[3]);
    if (arguments.size() == 5 && arguments[1] == QStringLiteral("--document-editor"))
        return childDocumentEditor(arguments[2], arguments[3], arguments[4]);
    if (arguments.size() == 2 && arguments[1] == QStringLiteral("--workflow")) return runDocumentWorkflow();
    return arguments.size() == 1 ? runParent() : 1;
}
