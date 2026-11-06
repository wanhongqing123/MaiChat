// Native integration across two owned processes. The child only hosts a Qt
// editor; the parent uses the production tool/controller/native backend. Child
// state is an independent assertion source, never a substitute for UIA readback.
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <json.hpp>

#include "MaiChatTools/DesktopComputerPlatform.h"
#include "MaiChatTools/DesktopComputerTools.h"
#include "MaiTool.h"

namespace {

using Json = nlohmann::json;

bool writeJson(const QString& path, const Json& value) {
    const QByteArray bytes = QByteArray::fromStdString(value.dump(2));
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

Json readJson(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return Json::parse(file.readAll().toStdString(), nullptr, false);
}

bool waitUntil(const std::function<bool()>& condition, int timeoutMs = 3000) {
    QElapsedTimer elapsed;
    elapsed.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        if (condition()) return true;
        QThread::msleep(10);
    } while (elapsed.elapsed() < timeoutMs);
    return condition();
}

int unavailable(const std::string& reason) {
    std::fprintf(stderr, "native two-process computer test SKIP: %s; native workflow was not fully verified\n",
                 reason.c_str());
    return 77;
}

// The only process terminated by this test is the QProcess it starts here.
// No process names, titles or enumerated PIDs are used for process termination.
struct OwnedEditorProcess {
    QProcess process;
    ~OwnedEditorProcess() {
        if (process.state() == QProcess::NotRunning) return;
        process.terminate();
        if (!process.waitForFinished(2000)) {
            process.kill();
            process.waitForFinished(2000);
        }
    }
};

int configuredFocusWaitSeconds() {
    bool valid = false;
    const int seconds = qEnvironmentVariableIntValue("MAICHAT_COMPUTER_NATIVE_WAIT_FOCUS_SECONDS", &valid);
    if (!valid || seconds <= 0) return 0;
    return seconds > 120 ? 120 : seconds;
}

#if defined(Q_OS_WIN)
bool allPhysicalInputReleased() {
    // The high bit is the current down state. Checking the complete virtual-key
    // range covers mouse buttons and keys without confusing Caps/NumLock toggles
    // with held input. This only reads OS state and never releases a user's key.
    for (int key = 1; key < 255; ++key) {
        if ((GetAsyncKeyState(key) & 0x8000) != 0) return false;
    }
    return true;
}

int awaitOwnedForeground(QProcess& child, const DesktopNativeWindow& window, int seconds,
                         const QString& readyPath, Json& ready) {
    const HWND handle = reinterpret_cast<HWND>(std::uintptr_t(window.id));
    QElapsedTimer elapsed;
    QElapsedTimer stable;
    elapsed.start();
    while (elapsed.elapsed() < seconds * 1000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        DWORD currentPid = 0;
        if (child.state() != QProcess::Running ||
            !GetWindowThreadProcessId(handle, &currentPid) || currentPid != window.processId) {
            ready["focus_wait_state"] = "owned_editor_exited";
            writeJson(readyPath, ready);
            std::fprintf(stderr, "owned editor disappeared while waiting for real foreground\n");
            return 1;
        }
        if (GetForegroundWindow() == handle && allPhysicalInputReleased()) {
            if (!stable.isValid()) stable.start();
            if (stable.elapsed() >= 200) {
                ready["focus_wait_state"] = "foreground_ready";
                ready["foreground_idle_stable_ms"] = stable.elapsed();
                if (!writeJson(readyPath, ready)) return 1;
                std::fprintf(stderr, "owned editor is actually foreground with all input released for 200 ms; continuing strict native tools\n");
                return 0;
            }
        } else {
            stable.invalidate();
        }
        QThread::msleep(10);
    }
    ready["focus_wait_state"] = "timed_out";
    writeJson(readyPath, ready);
    return unavailable("timed out waiting for the owned editor to be the actual foreground with released keyboard and mouse");
}
#endif

int runEditor(const QString& title, const QString& statePath) {
    QWidget window;
    window.setWindowTitle(title);
    auto* layout = new QVBoxLayout(&window);
    auto* heading = new QLabel(QStringLiteral("独立进程原生电脑操作验收"), &window);
    auto* editor = new QLineEdit(&window);
    editor->setAccessibleName(QStringLiteral("Native integration editor input"));
    editor->setContextMenuPolicy(Qt::CustomContextMenu);
    auto* confirmed = new QLabel(QStringLiteral("等待 Enter 确认"), &window);
    auto* status = new QLabel(QStringLiteral("仅使用此测试创建的编辑器内容"), &window);
    layout->addWidget(heading);
    layout->addWidget(editor);
    layout->addWidget(confirmed);
    layout->addWidget(status);
    layout->addStretch();
    int enterCount = 0;
    int rightClickCount = 0;
    const auto save = [&] {
        const Json state{{"ready", window.isVisible()},
                         {"pid", std::uint64_t(QCoreApplication::applicationPid())},
                         {"title", title.toUtf8().toStdString()},
                         {"text", editor->text().toUtf8().toStdString()},
                         {"selected_text", editor->selectedText().toUtf8().toStdString()},
                         {"confirmed_text", confirmed->text().toUtf8().toStdString()},
                         {"enter_count", enterCount}, {"right_click_count", rightClickCount}};
        if (!writeJson(statePath, state)) {
            std::fprintf(stderr, "could not save owned editor fixture state\n");
            QCoreApplication::exit(1);
        }
    };
    QObject::connect(editor, &QLineEdit::textChanged, &window, [&] { save(); });
    QObject::connect(editor, &QLineEdit::selectionChanged, &window, save);
    QObject::connect(editor, &QLineEdit::returnPressed, &window, [&] {
        ++enterCount;
        confirmed->setText(editor->text());
        save();
    });
    QObject::connect(editor, &QLineEdit::customContextMenuRequested, &window, [&](const QPoint&) {
        ++rightClickCount;
        save();
    });
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, &window, save);
    window.resize(640, 300);
    window.show();
    heartbeat.start(100);
    QTimer::singleShot(0, &window, save);
    return QApplication::exec();
}

enum class InputPosition { None, Center, TextStart, SelectionDrag };

class NativeWorkflow {
public:
    NativeWorkflow(MaiTool& tool, MaiToolContext context, DesktopNativeWindow window,
                   QProcess& child, QString statePath, QString evidencePath)
        : tool_(tool), context_(std::move(context)), window_(std::move(window)),
          child_(child), statePath_(std::move(statePath)), evidencePath_(std::move(evidencePath)) {}

    ~NativeWorkflow() {
        if (evidencePath_.isEmpty()) return;
        const Json evidence{{"backend", "production_native_two_owned_processes"},
                            {"parent_pid", std::uint64_t(QCoreApplication::applicationPid())},
                            {"child_pid", window_.processId}, {"window_id", std::to_string(window_.id)},
                            {"input_attempted", inputAttempted_}, {"exit_code", exitCode_}, {"steps", transcript_}};
        if (!writeJson(QDir(evidencePath_).filePath(QStringLiteral("native-transcript.json")), evidence))
            std::fprintf(stderr, "could not save native integration transcript\n");
        const Json state = readJson(statePath_);
        if (state.is_object()) writeJson(QDir(evidencePath_).filePath(QStringLiteral("owned-editor-state.json")), state);
    }

    int exitCode() const { return exitCode_; }

    bool expect(bool condition, const char* reason) {
        if (condition) return true;
        std::fprintf(stderr, "native two-process computer test failed: %s\n", reason);
        exitCode_ = 1;
        return false;
    }

    bool observe(const QString& stage) {
        if (!call({{"action", "observe"}, {"window_id", std::to_string(window_.id)},
                   {"include_elements", true}, {"include_image", true}}, stage)) return false;
        snapshot_ = output_;
        if (!expect(snapshot_.value("window_id", std::string()) == std::to_string(window_.id), "observed a different window ID") ||
            !expect(snapshot_.value("process_id", std::uint64_t(0)) == window_.processId, "observed a different process ID") ||
            !expect(!snapshot_.value("snapshot_id", std::string()).empty(), "observation omitted its snapshot token") ||
            !expect(snapshot_.value("has_image", false), "native observation omitted its WGC image") ||
            !expect(lastImages_.size() == 1, "native observation did not return one image attachment")) return false;
        const QString source = QString::fromStdString(lastImages_.front().path);
        if (!expect(QFileInfo(source).size() > 100, "native window screenshot is empty")) return false;
        if (!evidencePath_.isEmpty()) {
            const QString destination = QDir(evidencePath_).filePath(
                QStringLiteral("%1-%2.png").arg(screenshotCount_++, 2, 10, QLatin1Char('0')).arg(stage));
            QFile::remove(destination);
            if (!expect(QFile::copy(source, destination), "could not preserve native window screenshot")) return false;
        }
        return true;
    }

    bool action(const char* name, Json fields, const QString& stage,
                InputPosition position = InputPosition::None) {
        if (!observe(stage + QStringLiteral("-before"))) return false;
        fields["action"] = name;
        fields["window_id"] = std::to_string(window_.id);
        fields["snapshot_id"] = snapshot_["snapshot_id"];
        if (position != InputPosition::None) {
            // Use the same UIA element coordinates available to a real model.
            // Child Qt coordinates can be logical pixels at 150% DPI, while
            // the production Windows observation/input boundary is physical.
            Json input;
            int matches = 0;
            if (snapshot_.contains("elements") && snapshot_["elements"].is_array()) {
                for (const auto& element : snapshot_["elements"]) {
                    if (element.value("label", std::string()) == "Native integration editor input") {
                        input = element;
                        ++matches;
                    }
                }
            }
            if (!expect(matches == 1, "UIA observation did not uniquely expose the owned editor accessible name")) return false;
            const Json& bounds = input["bounds"];
            fields["x_ratio"] = position == InputPosition::Center ? input["center_x_ratio"].get<double>() :
                bounds["x_ratio"].get<double>() + bounds["width_ratio"].get<double>() * 0.025;
            fields["y_ratio"] = input["center_y_ratio"];
            if (position == InputPosition::SelectionDrag) {
                fields["to_x_ratio"] = bounds["x_ratio"].get<double>() + bounds["width_ratio"].get<double>() * 0.3;
                fields["to_y_ratio"] = fields["y_ratio"];
                fields["duration_ms"] = 100;
            }
        }
        if (!call(fields, stage)) return false;
        if (std::string(name) == "verify") return true;
        return expect(output_.value("dispatched", false), "native action did not report dispatched=true") &&
            expect(!output_.value("verified", true), "input dispatch falsely reported verified=true");
    }

    bool stateMatches(const std::function<bool(const Json&)>& condition, const char* reason) {
        const bool matched = waitUntil([&] {
            if (child_.state() != QProcess::Running) return false;
            const Json state = readJson(statePath_);
            return validState(state) && condition(state);
        });
        return expect(matched, reason);
    }

    bool verify(const std::string& expected, const QString& stage) {
        return action("verify", {{"expected_text", expected}}, stage) &&
            expect(output_.value("verified", false), "production focusedText did not verify the expected text") &&
            expect(!output_.value("dispatched", true), "readback unexpectedly dispatched input") &&
            expect(output_.value("actual_text", std::string()) == expected, "production UIA readback differed from expected text");
    }

private:
    bool validState(const Json& state) const {
        return state.is_object() && state.value("ready", false) &&
            state.value("pid", std::uint64_t(0)) == window_.processId &&
            state.value("title", std::string()) == window_.title &&
            state.contains("text") && state["text"].is_string();
    }

    bool call(const Json& request, const QString& stage) {
        if (!expect(child_.state() == QProcess::Running, "owned editor process exited before the operation")) return false;
        const std::string action = request.value("action", std::string());
        // Mark an input-capable call before execution: even a failed call may
        // have dispatched a partial native sequence. Only readiness work before
        // the first such attempt can report an environment skip. Focus itself
        // activates the owned window without sending keyboard/mouse input.
        if (action != "list" && action != "observe" && action != "focus" && action != "verify" && action != "clear")
            inputAttempted_ = true;
        const MaiToolResult result = tool_.execute(request.dump(), context_);
        Json record{{"stage", stage.toUtf8().toStdString()}, {"request", request}, {"input_attempted", inputAttempted_}};
        if (result.hasError()) {
            const MaiError& error = result.error();
            record["error"] = error.message();
            transcript_.push_back(record);
            std::fprintf(stderr, "native step %s (%s): %s\n", stage.toUtf8().constData(),
                         request.value("action", std::string()).c_str(), error.message().c_str());
            if (!inputAttempted_ && (error.code() == MaiErrorCode::Busy || error.code() == MaiErrorCode::NotConfigured ||
                                    error.code() == MaiErrorCode::NotSupported)) exitCode_ = unavailable(error.message());
            else exitCode_ = 1;
            return false;
        }
        output_ = Json::parse(result.output(), nullptr, false);
        lastImages_ = result.images();
        record["result"] = output_;
        transcript_.push_back(record);
        return expect(output_.is_object(), "native computer tool returned invalid JSON");
    }

    MaiTool& tool_;
    MaiToolContext context_;
    DesktopNativeWindow window_;
    QProcess& child_;
    QString statePath_;
    QString evidencePath_;
    Json snapshot_;
    Json output_;
    Json transcript_ = Json::array();
    std::vector<MaiToolImage> lastImages_;
    int screenshotCount_ = 0;
    bool inputAttempted_ = false;
    int exitCode_ = 0;
};

int runParent() {
#if defined(Q_OS_WIN)
    if (QApplication::platformName() != QStringLiteral("windows"))
        return unavailable("a real Windows QPA session is required");
#endif
    QTemporaryDir workspace;
    if (!workspace.isValid()) return 1;
    const QString title = QStringLiteral("MaiChat Native Editor 请单击此测试窗口 %1 %2")
        .arg(QCoreApplication::applicationPid()).arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString statePath = workspace.filePath(QStringLiteral("owned-editor.json"));
    const QString evidencePath = qEnvironmentVariable("MAICHAT_COMPUTER_NATIVE_EVIDENCE_DIR");
    if (!evidencePath.isEmpty() && !QDir().mkpath(evidencePath)) return 1;
    OwnedEditorProcess editor;
    editor.process.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--editor"), title, statePath});
    if (!editor.process.waitForStarted(5000)) {
        std::fprintf(stderr, "could not start owned editor: %s\n", editor.process.errorString().toUtf8().constData());
        return 1;
    }
    const std::uint64_t childPid = std::uint64_t(editor.process.processId());
    if (!waitUntil([&] {
            const Json state = readJson(statePath);
            return state.is_object() && state.value("ready", false) &&
                state.value("pid", std::uint64_t(0)) == childPid && state.value("title", std::string()) == title.toUtf8().toStdString();
        }, 5000)) {
        std::fprintf(stderr, "owned editor did not publish ready state; child status=%d\n", int(editor.process.state()));
        return 1;
    }

    DesktopNativeWindow native;
    MaiError enumerationError;
    int matches = 0;
    const bool selected = waitUntil([&] {
        const auto listed = maiDesktopWindows();
        if (!listed) {
            enumerationError = listed.error();
            return true;
        }
        matches = 0;
        for (const auto& window : listed.value()) {
            if (window.title == title.toUtf8().toStdString() && window.processId == childPid) {
                native = window;
                ++matches;
            }
        }
        return matches != 0;
    });
    if (enumerationError) return unavailable(enumerationError.message());
    if (!selected || matches != 1 || native.id == 0 || childPid == std::uint64_t(QCoreApplication::applicationPid())) {
        std::fprintf(stderr, "could not uniquely match the owned child editor PID/title\n");
        return 1;
    }
#if defined(Q_OS_WIN)
    DWORD observedPid = 0;
    const HWND childHandle = reinterpret_cast<HWND>(std::uintptr_t(native.id));
    if (!GetWindowThreadProcessId(childHandle, &observedPid) || observedPid != childPid) return 1;
#endif
    std::fprintf(stderr, "native two-process fixture: parent_pid=%llu owned_child_pid=%llu window_id=%llu\n",
                 static_cast<unsigned long long>(QCoreApplication::applicationPid()),
                 static_cast<unsigned long long>(childPid), static_cast<unsigned long long>(native.id));

    const int focusWaitSeconds = configuredFocusWaitSeconds();
    const QString readyPath = QDir(evidencePath.isEmpty() ? workspace.path() : evidencePath)
        .filePath(QStringLiteral("native-ready.json"));
    Json ready{{"parent_pid", std::uint64_t(QCoreApplication::applicationPid())},
               {"child_pid", childPid}, {"window_id", std::to_string(native.id)},
               {"title", native.title}, {"wait_focus_seconds", focusWaitSeconds},
               {"required_idle_stable_ms", 200},
               {"focus_wait_state", focusWaitSeconds == 0 ? "disabled" : "waiting"}};
    if (!writeJson(readyPath, ready)) return 1;
    std::fprintf(stderr, "owned editor ready: child_pid=%llu window_id=%llu wait_seconds=%d title=%s ready_file=%s\n",
                 static_cast<unsigned long long>(childPid), static_cast<unsigned long long>(native.id), focusWaitSeconds,
                 native.title.c_str(), readyPath.toUtf8().constData());
    std::fflush(stderr);
#if defined(Q_OS_WIN)
    // An optional human click must be confirmed from actual OS state. Waiting
    // never activates, raises, minimizes or injects into any window. Default
    // runs proceed immediately to the existing strict focus action below.
    if (focusWaitSeconds > 0) {
        const int waited = awaitOwnedForeground(editor.process, native, focusWaitSeconds, readyPath, ready);
        if (waited != 0) return waited;
    }
#endif

    MaiToolRegistry registry;
    registerDesktopComputerTools(registry, std::make_shared<DesktopComputerController>());
    MaiTool* tool = registry.find("desktop_computer");
    if (!tool) return 1;
    MaiToolContext context;
    context.sessionId = "native-owned-editor-" + std::to_string(childPid);
    context.root = workspace.path().toUtf8().toStdString();
    NativeWorkflow workflow(*tool, context, native, editor.process, statePath, evidencePath);
    const std::string draft = QStringLiteral("原生输入中文草稿").toUtf8().toStdString();
    const std::string replacement = QStringLiteral("Ctrl+A 后的中文替换").toUtf8().toStdString();
    const std::string finalText = QStringLiteral("双进程原生验证：中文内容已替换。").toUtf8().toStdString();
#if defined(Q_OS_MAC)
    const char* selectAllKey = "Cmd+A";
#else
    const char* selectAllKey = "Ctrl+A";
#endif
    const auto textIs = [&](const std::string& expected, const char* reason) {
        return workflow.stateMatches([&](const Json& state) { return state.value("text", std::string()) == expected; }, reason);
    };

    if (!workflow.action("focus", Json::object(), QStringLiteral("focus"))) return workflow.exitCode();
    if (!workflow.action("click", Json::object(), QStringLiteral("click-editor"), InputPosition::Center)) return workflow.exitCode();
    if (!workflow.action("type", {{"text", draft}}, QStringLiteral("type-chinese")) ||
        !textIs(draft, "Unicode input did not arrive in the owned child editor")) return workflow.exitCode();
    if (!workflow.action("key", {{"key", selectAllKey}}, QStringLiteral("select-all")) ||
        !workflow.stateMatches([&](const Json& state) { return state.value("selected_text", std::string()) == draft; },
                               "Select All did not select the child editor text")) return workflow.exitCode();
    if (!workflow.action("type", {{"text", replacement}}, QStringLiteral("replace-selection")) ||
        !textIs(replacement, "typing over the selected text did not replace it")) return workflow.exitCode();
    if (!workflow.action("replace_text", {{"text", ""}}, QStringLiteral("empty-replacement")) ||
        !textIs("", "empty replacement did not clear the child editor")) return workflow.exitCode();
    if (!workflow.action("replace_text", {{"text", finalText}}, QStringLiteral("final-chinese")) ||
        !textIs(finalText, "final Chinese replacement did not arrive")) return workflow.exitCode();
    if (!workflow.action("key", {{"key", "Enter"}}, QStringLiteral("confirm-enter")) ||
        !workflow.stateMatches([&](const Json& state) {
            return state.value("enter_count", 0) == 1 && state.value("confirmed_text", std::string()) == finalText;
        }, "Enter did not confirm the actual child editor text")) return workflow.exitCode();
    if (!workflow.verify(finalText, QStringLiteral("uia-readback"))) return workflow.exitCode();

    // Verify text first, then exercise bounded mouse gestures only in the same
    // child input. The custom context handler records right-click without a menu.
    if (!workflow.action("double_click", Json::object(), QStringLiteral("double-click"), InputPosition::TextStart) ||
        !workflow.stateMatches([](const Json& state) { return !state.value("selected_text", std::string()).empty(); },
                               "native double-click did not select editor text")) return workflow.exitCode();
    if (!workflow.action("right_click", Json::object(), QStringLiteral("right-click"), InputPosition::TextStart) ||
        !workflow.stateMatches([](const Json& state) { return state.value("right_click_count", 0) == 1; },
                               "native right-click was not delivered to the child editor")) return workflow.exitCode();
    if (!workflow.action("drag", Json::object(), QStringLiteral("selection-drag"), InputPosition::SelectionDrag) ||
        !workflow.stateMatches([](const Json& state) { return !state.value("selected_text", std::string()).empty(); },
                               "native drag did not select child editor text")) return workflow.exitCode();
    if (!workflow.verify(finalText, QStringLiteral("uia-readback-after-gestures"))) return workflow.exitCode();
    if (!workflow.observe(QStringLiteral("completed-owned-editor"))) return workflow.exitCode();
    if (!textIs(finalText, "mouse gestures unexpectedly changed the child editor text")) return workflow.exitCode();
    std::fprintf(stderr, "native two-process computer workflow verified: Unicode, Ctrl+A, empty replacement, Enter, gestures and real focusedText readback\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const QStringList arguments = app.arguments();
    if (arguments.size() == 4 && arguments[1] == QStringLiteral("--editor"))
        return runEditor(arguments[2], arguments[3]);
    if (arguments.size() != 1) return 1;
    return runParent();
}
