// Opt-in native display regression. Both processes and all editor contents are
// owned fixtures. Fullscreen covers the captured display with fixture pixels;
// a lost foreground cancels capture/input rather than touching another app.
// This proves native execution, not a visual model's autonomous decisions.
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QLineEdit>
#include <QProcess>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QLabel>

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
}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const auto arguments = app.arguments();
    if (arguments.size() == 4 && arguments[1] == QStringLiteral("--editor"))
        return childEditor(arguments[2], arguments[3]);
    return arguments.size() == 1 ? runParent() : 1;
}
