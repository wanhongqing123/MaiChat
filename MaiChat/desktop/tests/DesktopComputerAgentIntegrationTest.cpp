// Exercises the production creation-center conversation, Agent loop and
// desktop_computer tool. The deterministic slot uses an in-process editor; the
// native slot delegates every platform operation to production and controls a
// separately owned editor process through the same real conversation UI.
// Model observations use metadata/UIA only; native captures are separate fixture
// evidence and never enter the model conversation.
#include <QApplication>
#include <QBuffer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QProcess>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QThread>
#include <QUuid>
#include <QVBoxLayout>
#include <QtTest>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <json.hpp>

#include "MaiChatTools/DesktopComputerBackend.h"
#include "MaiChatTools/DesktopComputerTools.h"
#include "agent/AgentChatPanel.h"
#include "agent/AgentController.h"
#include "markdown/MarkdownView.h"

namespace {

using Json = nlohmann::json;

QString evidenceDirectory() {
    const QString explicitPath = qEnvironmentVariable("MAICHAT_COMPUTER_AGENT_EVIDENCE_DIR");
    return explicitPath.isEmpty() ? qEnvironmentVariable("MAICHAT_AGENT_EVIDENCE_DIR") : explicitPath;
}

bool saveEvidence(QWidget& widget, const QString& name) {
    const QString directory = evidenceDirectory();
    if (directory.isEmpty()) return true;
    return QDir().mkpath(directory) && widget.grab().save(QDir(directory).filePath(name + QStringLiteral(".png")));
}

struct ScriptState {
    std::atomic<bool> started{false};
    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};
    std::atomic<bool> verified{false};
    std::atomic<bool> declaredTool{false};
    std::atomic<bool> receivedChinesePrompt{false};
    std::mutex mutex;
    std::string error;
    std::vector<Json> calls;
    std::vector<Json> results;

    MaiError fail(const std::string& message) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            error = message;
        }
        failed = true;
        return {MaiErrorCode::Protocol, message};
    }
};

// The editor operations stay on the GUI thread. This seam never uses global
// mouse/keyboard input and therefore also runs under an offscreen Qt platform.
class EditorBackend final : public DesktopComputerBackend {
public:
    EditorBackend(QWidget& scene, QWidget& editorWindow, QLineEdit& editor)
        : scene_(scene), editorWindow_(editorWindow), editor_(editor) {}

    MaiResult<std::vector<DesktopNativeWindow>> windows() override {
        std::vector<DesktopNativeWindow> output;
        onUi([&] {
            const QPoint topLeft = editorWindow_.mapToGlobal(QPoint());
            output.push_back({9001, std::uint64_t(QCoreApplication::applicationPid()),
                "MaiChat isolated agent editor", "Integration fixture",
                double(topLeft.x()), double(topLeft.y()),
                double(editorWindow_.width()), double(editorWindow_.height())});
        });
        return output;
    }
    MaiResult<std::vector<DesktopNativeElement>> elements(const DesktopNativeWindow&) override {
        std::vector<DesktopNativeElement> output;
        onUi([&] {
            const QPoint point = editor_.mapToGlobal(QPoint());
            output.push_back({"text", "Isolated agent editor input", double(point.x()), double(point.y()),
                              double(editor_.width()), double(editor_.height())});
        });
        return output;
    }
    MaiError capture(const DesktopNativeWindow&, MaiScreenshot& output) override {
        ++captureCount;
        QImage image;
        onUi([&] { image = editorWindow_.grab().toImage(); });
        QByteArray png;
        QBuffer buffer(&png);
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
            return {MaiErrorCode::Internal, "could not encode isolated editor screenshot"};
        output.width = image.width();
        output.height = image.height();
        output.pngBytes.assign(png.begin(), png.end());
        return {};
    }
    MaiError activate(const DesktopNativeWindow&) override {
        onUi([&] { focused_ = true; });
        return {};
    }
    MaiResult<bool> isFocused(const DesktopNativeWindow&) override {
        bool result = false;
        onUi([&] { result = focused_; });
        return result;
    }
    MaiResult<bool> secureInput() override {
        bool secure = false;
        onUi([&] { secure = editor_.echoMode() != QLineEdit::Normal; });
        return secure;
    }
    MaiError input(const DesktopNativeInput& input) override {
        if (input.isCanceled && input.isCanceled()) return {MaiErrorCode::Canceled, "fixture input canceled"};
        MaiError result;
        onUi([&] {
            if (input.kind == DesktopNativeInputKind::Click) {
                editor_.setFocus(Qt::MouseFocusReason);
            } else if (input.kind == DesktopNativeInputKind::Type) {
                editor_.insert(QString::fromUtf8(input.text.data(), int(input.text.size())));
            } else if (input.kind == DesktopNativeInputKind::ReplaceFocusedText) {
                editor_.selectAll();
                if (input.text.empty()) QTest::keyClick(&editor_, Qt::Key_Backspace);
                else editor_.insert(QString::fromUtf8(input.text.data(), int(input.text.size())));
            } else if (input.kind == DesktopNativeInputKind::Key && input.key == "Ctrl+A") {
                QTest::keyClick(&editor_, Qt::Key_A, Qt::ControlModifier);
            } else if (input.kind == DesktopNativeInputKind::Key && input.key == "Enter") {
                QTest::keyClick(&editor_, Qt::Key_Return);
            } else {
                result = {MaiErrorCode::NotSupported, "unexpected integration fixture input"};
                return;
            }
            ++inputCount;
            if (input.kind == DesktopNativeInputKind::ReplaceFocusedText && input.text.empty()) sawEmptyReplacement = true;
            if (input.kind == DesktopNativeInputKind::Key && input.key == "Ctrl+A") sawSelectAll = true;
            evidenceOk = evidenceOk && saveEvidence(scene_, QStringLiteral("computer-agent-step-%1")
                                                                 .arg(inputCount, 2, 10, QLatin1Char('0')));
        });
        return result;
    }
    MaiResult<std::string> focusedText(const DesktopNativeWindow&) override {
        std::string output;
        onUi([&] {
            ++readCount;
            output = editor_.text().toUtf8().toStdString();
        });
        return output;
    }

    int inputCount = 0;
    int readCount = 0;
    int captureCount = 0;
    bool sawEmptyReplacement = false;
    bool sawSelectAll = false;
    bool evidenceOk = true;

private:
    void onUi(const std::function<void()>& action) {
        if (QThread::currentThread() == scene_.thread()) action();
        else QMetaObject::invokeMethod(&scene_, action, Qt::BlockingQueuedConnection);
    }

    QWidget& scene_;
    QWidget& editorWindow_;
    QLineEdit& editor_;
    bool focused_ = false;
};

// Each tool call consumes only IDs and snapshots from the preceding real tool
// result. A broken dispatch or readback stops the script before a final answer.
class ComputerScriptModel final : public MaiModelClient {
public:
    ComputerScriptModel(std::shared_ptr<ScriptState> state, std::string prompt, std::string expected,
                        std::string expectedTitle = "MaiChat isolated agent editor",
                        std::string inputLabel = "Isolated agent editor input", std::string selectAllKey = "Ctrl+A",
                        bool native = false, std::function<bool()> confirmEditor = {})
        : state_(std::move(state)), prompt_(std::move(prompt)), expected_(std::move(expected)),
          expectedTitle_(std::move(expectedTitle)), inputLabel_(std::move(inputLabel)),
          selectAllKey_(std::move(selectAllKey)), native_(native), confirmEditor_(std::move(confirmEditor)) {
        steps_ = {
            {{"action", "list"}}, {{"action", "observe"}}, {{"action", "click"}},
            {{"action", "observe"}}, {{"action", "type"}, {"text", native_ ? "原生中文草稿" : "Old draft"}},
            {{"action", "observe"}}, {{"action", "key"}, {"key", "Ctrl+A"}},
            {{"action", "observe"}}, {{"action", "type"}, {"text", native_ ? "组合键替换中文" : "Intermediate text"}},
            {{"action", "observe"}}, {{"action", "replace_text"}, {"text", ""}},
            {{"action", "observe"}}, {{"action", "replace_text"}, {"text", expected_}},
            {{"action", "observe"}}, {{"action", "key"}, {"key", "Enter"}},
            {{"action", "observe"}}, {{"action", "verify"}, {"expected_text", expected_}}};
    }

    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>& cancel) override {
        if (cancel.load()) return {MaiErrorCode::Canceled, "integration script canceled"};
        state_->started = true;
        for (const auto& tool : request.tools)
            if (tool.name == "desktop_computer") state_->declaredTool = true;
        for (const auto& message : request.messages) {
            // No step requests vision: even a synthetic image observation would
            // violate the model-facing metadata/UIA-only computer contract.
            if (!message.images.empty())
                return state_->fail("computer metadata workflow unexpectedly sent model images");
            if (message.role == MaiModelRole::User && message.content == prompt_) state_->receivedChinesePrompt = true;
        }
        if (!state_->declaredTool.load())
            return state_->fail("desktop_computer was absent from the real Agent tool list");
        if (next_ != 0) {
            Json result;
            bool found = false;
            for (const auto& message : request.messages) {
                if (message.role != MaiModelRole::ToolResult || message.toolCallId != lastCallId_) continue;
                result = Json::parse(message.content, nullptr, false);
                found = true;
            }
            if (!found || !result.is_object()) {
                std::string detail;
                for (const auto& message : request.messages)
                    if (message.role == MaiModelRole::ToolResult && message.toolCallId == lastCallId_)
                        detail = message.content;
                return state_->fail("the Agent did not return a valid paired computer tool result: " + detail);
            }
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                state_->results.push_back(result);
            }
            const std::string previousAction = steps_[next_ - 1]["action"];
            if (previousAction == "list") {
                if (!result.contains("windows") || !result["windows"].is_array())
                    return state_->fail("list did not return visible windows");
                for (const auto& window : result["windows"])
                    if (window.value("title", std::string()) == expectedTitle_)
                        windowId_ = window.value("window_id", std::string());
                if (windowId_.empty()) return state_->fail("the isolated editor was absent from list");
                if (native_) selectAllKey_ = result.value("select_all_key", selectAllKey_);
            } else if (previousAction == "observe") {
                observed_ = result;
                snapshotId_ = result.value("snapshot_id", std::string());
                if (snapshotId_.empty() || result.value("window_id", std::string()) != windowId_)
                    return state_->fail("observe did not return a snapshot for the listed editor ID");
                if (result.contains("image_path") || result.value("has_image", false))
                    return state_->fail("computer observe exposed an image in its metadata result");
            } else if (previousAction == "verify") {
                if (!result.value("verified", false) || result.value("actual_text", std::string()) != expected_)
                    return state_->fail("the actual editor text did not match expected_text");
                state_->verified = true;
            } else if (!result.value("dispatched", false) || result.value("verified", true)) {
                return state_->fail("an input action did not report dispatched=true and verified=false");
            }
        }
        if (next_ == steps_.size()) {
            if (!state_->verified.load()) return state_->fail("refusing final answer before verified readback");
            if (confirmEditor_ && !confirmEditor_())
                return state_->fail("native readback did not also match the owned child's text and Enter confirmation");
            if (sink.onText) sink.onText("已读回确认：" + expected_);
            state_->done = true;
            return {};
        }

        Json arguments = steps_[next_];
        const std::string action = arguments["action"];
        if (action != "list") arguments["window_id"] = windowId_;
        if (action != "list" && action != "observe") arguments["snapshot_id"] = snapshotId_;
        if (action == "click") {
            int matches = 0;
            if (observed_.contains("elements") && observed_["elements"].is_array()) {
                for (const auto& element : observed_["elements"]) {
                    if (element.value("label", std::string()) != inputLabel_) continue;
                    arguments["x_ratio"] = element["center_x_ratio"];
                    arguments["y_ratio"] = element["center_y_ratio"];
                    ++matches;
                }
            }
            if (matches != 1) return state_->fail("observation did not uniquely locate the editor accessible label");
        }
        if (action == "key" && arguments.value("key", std::string()) == "Ctrl+A") arguments["key"] = selectAllKey_;
        if (action == "observe") arguments["include_elements"] = true;
        lastCallId_ = "computer-agent-fixture-" + std::to_string(next_);
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->calls.push_back(arguments);
        }
        if (sink.onText) sink.onText("正在操作隔离编辑器：" + action + "。\n");
        if (sink.onToolCall) sink.onToolCall({lastCallId_, "desktop_computer", arguments.dump()});
        ++next_;
        return {};
    }
    MaiWireApi wireApi() const override { return MaiWireApi::ChatCompletions; }

private:
    std::shared_ptr<ScriptState> state_;
    std::string prompt_;
    std::string expected_;
    std::string expectedTitle_;
    std::string inputLabel_;
    std::string selectAllKey_;
    bool native_ = false;
    std::function<bool()> confirmEditor_;
    Json observed_;
    std::vector<Json> steps_;
    std::size_t next_ = 0;
    std::string lastCallId_;
    std::string windowId_;
    std::string snapshotId_;
};

Json readOwnedEditorState(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return Json::parse(file.readAll().toStdString(), nullptr, false);
}

bool saveJsonEvidence(const QString& path, const Json& value) {
    const QByteArray bytes = QByteArray::fromStdString(value.dump(2));
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

struct OwnedNativeEditor {
    QProcess process;
    ~OwnedNativeEditor() {
        if (process.state() == QProcess::NotRunning) return;
        process.terminate();
        if (!process.waitForFinished(2000)) {
            process.kill();
            process.waitForFinished(2000);
        }
    }
};

// Only discovery is narrowed. All capture, activation, input, foreground,
// secure-input and text-read operations use the production implementation.
class ScopedNativeBackend final : public DesktopComputerBackend {
public:
    ScopedNativeBackend(std::shared_ptr<DesktopComputerBackend> native, DesktopNativeWindow target,
                        QString screenshotDirectory)
        : native_(std::move(native)), target_(std::move(target)), screenshotDirectory_(std::move(screenshotDirectory)) {}

    MaiResult<std::vector<DesktopNativeWindow>> windows() override {
        const auto listed = remember(native_->windows(), "windows");
        if (!listed) return listed.error();
        std::vector<DesktopNativeWindow> owned;
        for (const auto& window : listed.value())
            if (window.processId == target_.processId) owned.push_back(window);
        return owned;
    }
    MaiResult<std::vector<DesktopNativeElement>> elements(const DesktopNativeWindow& window) override {
        return remember(native_->elements(window), "elements");
    }
    MaiError capture(const DesktopNativeWindow& window, MaiScreenshot& output) override {
        const MaiError result = remember(native_->capture(window, output), "capture");
        if (result) return result;
        const int index = captures.fetch_add(1);
        if (!screenshotDirectory_.isEmpty()) {
            QSaveFile file(QDir(screenshotDirectory_).filePath(QStringLiteral("native-editor-%1.png")
                                                                 .arg(index, 3, 10, QLatin1Char('0'))));
            if (!file.open(QIODevice::WriteOnly) ||
                file.write(reinterpret_cast<const char*>(output.pngBytes.data()), qint64(output.pngBytes.size())) !=
                    qint64(output.pngBytes.size()) || !file.commit())
                return remember(MaiError{MaiErrorCode::Internal, "could not preserve production native editor screenshot"}, "evidence");
        }
        return {};
    }
    MaiError activate(const DesktopNativeWindow& window) override {
        return remember(native_->activate(window), "activate");
    }
    MaiError activateForInput(const DesktopNativeWindow& window) override {
        return remember(native_->activateForInput(window), "activateForInput");
    }
    MaiResult<bool> isFocused(const DesktopNativeWindow& window) override {
        const auto focused = remember(native_->isFocused(window), "isFocused");
#if defined(Q_OS_WIN)
        if (focused && !focused.value() &&
            GetForegroundWindow() != reinterpret_cast<HWND>(std::uintptr_t(target_.id))) lostForeground = true;
#endif
        return focused;
    }
    MaiResult<bool> inputFocused(const DesktopNativeWindow& window) override {
        const auto focused = remember(native_->inputFocused(window), "inputFocused");
#if defined(Q_OS_WIN)
        if (focused && !focused.value() &&
            GetForegroundWindow() != reinterpret_cast<HWND>(std::uintptr_t(target_.id))) lostForeground = true;
#endif
        return focused;
    }
    MaiResult<bool> secureInput() override { return remember(native_->secureInput(), "secureInput"); }
    MaiError input(const DesktopNativeInput& input) override {
        beganInput = true;
        ++inputs;
        return remember(native_->input(input), "input");
    }
    MaiResult<std::string> focusedText(const DesktopNativeWindow& window) override {
        ++reads;
        return remember(native_->focusedText(window), "focusedText");
    }
    Json diagnostics() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return Json{{"backend", "production_native_scoped_to_owned_child"},
                    {"child_pid", target_.processId}, {"window_id", std::to_string(target_.id)},
                    {"input_attempts", inputs.load()}, {"captures", captures.load()},
                    {"text_reads", reads.load()}, {"failures", failures_}};
    }

    std::atomic<int> captures{0};
    std::atomic<int> inputs{0};
    std::atomic<int> reads{0};
    std::atomic<bool> beganInput{false};
    std::atomic<bool> lostForeground{false};

private:
    MaiError remember(MaiError error, const char* operation) {
        if (error) {
            bool ownedForeground = true;
#if defined(Q_OS_WIN)
            ownedForeground = GetForegroundWindow() == reinterpret_cast<HWND>(std::uintptr_t(target_.id));
#endif
            if (error.code() == MaiErrorCode::Busy && !ownedForeground) lostForeground = true;
            std::lock_guard<std::mutex> lock(mutex_);
            failures_.push_back({{"operation", operation}, {"code", maiErrorCodeToString(error.code())},
                                 {"message", error.message()}, {"owned_foreground", ownedForeground}});
        }
        return error;
    }
    template <typename T>
    MaiResult<T> remember(MaiResult<T> result, const char* operation) {
        if (!result) remember(result.error(), operation);
        return result;
    }

    std::shared_ptr<DesktopComputerBackend> native_;
    DesktopNativeWindow target_;
    QString screenshotDirectory_;
    mutable std::mutex mutex_;
    Json failures_ = Json::array();
};

MaiResult<bool> waitForOwnedNativeForeground(DesktopComputerBackend& backend, const DesktopNativeWindow& target,
                                           QProcess& child, const QString& readyPath) {
    bool parsed = false;
    int seconds = qEnvironmentVariableIntValue("MAICHAT_COMPUTER_NATIVE_WAIT_FOCUS_SECONDS", &parsed);
    seconds = !parsed || seconds < 0 ? 0 : (seconds > 120 ? 120 : seconds);
    Json ready{{"child_pid", target.processId}, {"window_id", std::to_string(target.id)},
               {"title", target.title}, {"wait_focus_seconds", seconds}, {"state", "waiting_for_actual_foreground"}};
    if (!readyPath.isEmpty() && !saveJsonEvidence(readyPath, ready))
        return {MaiErrorCode::Internal, "could not save native creation-center readiness evidence"};
    qInfo("native creation-center editor ready: child_pid=%llu window_id=%llu title=%s wait_seconds=%d",
          static_cast<unsigned long long>(target.processId), static_cast<unsigned long long>(target.id),
          target.title.c_str(), seconds);
    QElapsedTimer elapsed;
    QElapsedTimer stable;
    elapsed.start();
    do {
        if (child.state() != QProcess::Running)
            return {MaiErrorCode::NotFound, "owned editor exited while waiting for actual foreground"};
        bool foreground = false;
        bool released = true;
#if defined(Q_OS_WIN)
        DWORD pid = 0;
        const HWND handle = reinterpret_cast<HWND>(std::uintptr_t(target.id));
        if (!GetWindowThreadProcessId(handle, &pid) || pid != target.processId)
            return {MaiErrorCode::NotFound, "owned editor window identity changed while waiting for actual foreground"};
        foreground = GetForegroundWindow() == handle;
        for (int key = 1; key < 255 && released; ++key) released = (GetAsyncKeyState(key) & 0x8000) == 0;
#else
        const auto focused = backend.isFocused(target);
        if (!focused) return focused.error();
        foreground = focused.value();
#endif
        if (foreground && released) {
            if (!stable.isValid()) stable.start();
            if (stable.elapsed() >= 200) {
                ready["state"] = "actual_foreground_and_idle";
                if (!readyPath.isEmpty() && !saveJsonEvidence(readyPath, ready))
                    return {MaiErrorCode::Internal, "could not save native foreground readiness evidence"};
                return true;
            }
        } else {
            stable.invalidate();
            if (seconds == 0) break;
        }
        QTest::qWait(10);
    } while (elapsed.elapsed() < (seconds == 0 ? 250 : seconds * 1000));
    ready["state"] = "not_foreground_no_input_sent";
    if (!readyPath.isEmpty() && !saveJsonEvidence(readyPath, ready))
        return {MaiErrorCode::Internal, "could not save native foreground timeout evidence"};
    return false;
}

// QTest assertions may exit a function early. Keep the GUI event loop alive
// while stopping the worker so a pending blocking editor call can finish.
struct StopAgentOnExit {
    AgentController& controller;
    QString session;
    ~StopAgentOnExit() {
        if (session.isEmpty() || !controller.agent().isBusy(session.toStdString())) return;
        controller.interrupt(session);
        QElapsedTimer elapsed;
        elapsed.start();
        while (controller.agent().isBusy(session.toStdString()) && elapsed.elapsed() < 5000)
            QTest::qWait(10);
    }
};

} // namespace

class DesktopComputerAgentIntegrationTest final : public QObject {
    Q_OBJECT
public:
    bool nativeSkipped = false;

private:
    void skipNative(const char* reason) {
        nativeSkipped = true;
        QSKIP(reason);
    }
    void failOrSkipNativeReadiness(const MaiError& error) {
        // Missing capabilities and foreground readiness can skip before input.
        // Unexpected identity, protocol or I/O failures must stay visible.
        if (error.code() == MaiErrorCode::Busy || error.code() == MaiErrorCode::NotConfigured ||
            error.code() == MaiErrorCode::NotSupported) return skipNative(error.message().c_str());
        QFAIL(error.message().c_str());
    }

private slots:
    void chinesePromptTraversesRealAgentAndReadsBackEditor();
    void nativePromptTraversesRealAgentAndReadsBackOwnedProcess();
};

void DesktopComputerAgentIntegrationTest::chinesePromptTraversesRealAgentAndReadsBackEditor() {
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    const QString prompt = QStringLiteral("在隔离编辑器输入中文，使用 Ctrl+A 替换草稿，清空后写入最终内容并按 Enter，最后读回确认。");
    const QString expected = QStringLiteral("自动化验证：中文内容已替换。");
    auto state = std::make_shared<ScriptState>();
    QWidget scene;
    scene.setWindowTitle(QStringLiteral("MaiChat creation center computer integration fixture"));
    auto* layout = new QHBoxLayout(&scene);
    auto* editorWindow = new QGroupBox(QStringLiteral("隔离编辑器 · 进程内测试后端"), &scene);
    auto* editorLayout = new QVBoxLayout(editorWindow);
    auto* editor = new QLineEdit(editorWindow);
    editor->setAccessibleName(QStringLiteral("Isolated agent editor input"));
    auto* confirmed = new QLabel(QStringLiteral("等待 Enter 确认"), editorWindow);
    QObject::connect(editor, &QLineEdit::returnPressed, confirmed, [editor, confirmed] { confirmed->setText(editor->text()); });
    editorLayout->addWidget(editor);
    editorLayout->addWidget(confirmed);
    editorLayout->addStretch();
    scene.resize(1300, 800);
    auto backend = std::make_shared<EditorBackend>(scene, *editorWindow, *editor);
    AgentController controller(
        std::make_unique<ComputerScriptModel>(state, prompt.toUtf8().toStdString(), expected.toUtf8().toStdString()),
        workspace.filePath(QStringLiteral("computer-agent.sqlite")),
        [backend](MaiToolRegistry& tools) {
            registerDesktopComputerTools(tools, std::make_shared<DesktopComputerController>(backend));
        });
    QVERIFY(controller.setApprovalPolicy(MaiApprovalPolicy::Never));
    // Destroy the borrowed-controller panel before the controller, while the
    // fixture editor outlives the worker and its backend callbacks.
    auto panel = std::make_unique<AgentChatPanel>(controller, &scene);
    layout->addWidget(panel.get(), 3);
    layout->addWidget(editorWindow, 2);
    const QString session = controller.createSession(workspace.path(), QStringLiteral("电脑操作中文验收"));
    QVERIFY(!session.isEmpty());
    StopAgentOnExit stop{controller, session};
    panel->openSession(session);
    scene.show();
    QVERIFY(QTest::qWaitForWindowExposed(&scene));
    auto* promptEditor = panel->findChild<QTextEdit*>(QStringLiteral("agentPromptEditor"));
    auto* send = panel->findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    QVERIFY(promptEditor && send);
    promptEditor->setPlainText(prompt);
    QVERIFY(saveEvidence(scene, QStringLiteral("computer-agent-chinese-user-input")));
    QTest::mouseClick(send, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(state->started.load(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(state->done.load() || state->failed.load(), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.agent().isBusy(session.toStdString()), 5000);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        const QString directory = evidenceDirectory();
        if (!directory.isEmpty()) {
            Json transcript{{"prompt", prompt.toUtf8().toStdString()},
                            {"expected_text", expected.toUtf8().toStdString()},
                            {"backend", "in_process_editor_no_native_injection"},
                            {"error", state->error},
                            {"calls", state->calls}, {"results", state->results}};
            const QByteArray bytes = QByteArray::fromStdString(transcript.dump(2));
            QSaveFile file(QDir(directory).filePath(QStringLiteral("computer-agent-transcript.json")));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(bytes), qint64(bytes.size()));
            QVERIFY(file.commit());
        }
        QVERIFY2(!state->failed.load(), state->error.c_str());
        QCOMPARE(state->calls.size(), std::size_t(17));
        QCOMPARE(state->results.size(), state->calls.size());
    }
    QVERIFY(state->declaredTool.load());
    QVERIFY(state->receivedChinesePrompt.load());
    QVERIFY(state->verified.load());
    QCOMPARE(editor->text(), expected);
    QCOMPARE(confirmed->text(), expected);
    QVERIFY(backend->sawSelectAll);
    QVERIFY(backend->sawEmptyReplacement);
    QCOMPARE(backend->inputCount, 7);
    QCOMPARE(backend->readCount, 1);
    QCOMPARE(backend->captureCount, 0);
    QVERIFY(backend->evidenceOk);
    QVERIFY(controller.agent().listPendingPermissions().empty());
    bool sawUser = false;
    bool sawFinal = false;
    int completedComputerTools = 0;
    for (const auto& message : controller.agent().listMessages(session.toStdString())) {
        if (message.role == MaiRole::User && QString::fromStdString(message.text()) == prompt) sawUser = true;
        if (message.role == MaiRole::Assistant && QString::fromStdString(message.text()).contains(QStringLiteral("已读回确认：") + expected))
            sawFinal = true;
        for (const auto& part : message.parts) {
            const auto* tool = std::get_if<MaiToolPart>(&part.body);
            if (tool && tool->tool == "desktop_computer" && tool->state == MaiToolState::Completed) ++completedComputerTools;
        }
    }
    QVERIFY(sawUser);
    QVERIFY(sawFinal);
    QCOMPARE(completedComputerTools, 17);
    auto* history = panel->findChild<MarkdownView*>();
    QVERIFY(history);
    QTRY_VERIFY_WITH_TIMEOUT((history->selectAll(), history->selectedText().contains(expected)), 5000);
    history->clearSelection();
    QVERIFY(saveEvidence(scene, QStringLiteral("computer-agent-verified-editor-and-conversation")));
}

void DesktopComputerAgentIntegrationTest::nativePromptTraversesRealAgentAndReadsBackOwnedProcess() {
#if defined(Q_OS_WIN)
    if (QApplication::platformName() != QStringLiteral("windows"))
        return skipNative("Native creation-center integration requires a real Windows QPA session; no native input was sent.");
#endif
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    QString editorExecutable = QDir(QCoreApplication::applicationDirPath()).filePath(
        QStringLiteral("desktop_computer_native_integration_test"));
#if defined(Q_OS_WIN)
    editorExecutable += QStringLiteral(".exe");
#endif
    QVERIFY2(QFileInfo::exists(editorExecutable), "The separately hosted native editor target must be built beside this test.");
    QString directory = qEnvironmentVariable("MAICHAT_COMPUTER_NATIVE_EVIDENCE_DIR");
    if (directory.isEmpty()) directory = evidenceDirectory();
    const QString childImages = directory.isEmpty() ? QString() : QDir(directory).filePath(QStringLiteral("native-editor"));
    const QString panelImages = directory.isEmpty() ? QString() : QDir(directory).filePath(QStringLiteral("creation-center"));
    if (!directory.isEmpty()) {
        QVERIFY(QDir().mkpath(childImages));
        QVERIFY(QDir().mkpath(panelImages));
    }
    const QString title = QStringLiteral("MaiChat Creation Native 请单击此测试窗口 %1 %2")
        .arg(QCoreApplication::applicationPid()).arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString statePath = workspace.filePath(QStringLiteral("owned-native-editor.json"));
    OwnedNativeEditor owned;
    owned.process.start(editorExecutable, {QStringLiteral("--editor"), title, statePath});
    QVERIFY2(owned.process.waitForStarted(5000), qPrintable(owned.process.errorString()));
    const std::uint64_t childPid = std::uint64_t(owned.process.processId());
    QVERIFY(childPid != 0 && childPid != std::uint64_t(QCoreApplication::applicationPid()));
    const auto stateBelongsToChild = [&](const Json& value) {
        return value.is_object() && value.value("ready", false) && value.value("pid", std::uint64_t(0)) == childPid &&
            value.value("title", std::string()) == title.toUtf8().toStdString();
    };
    QTRY_VERIFY_WITH_TIMEOUT(stateBelongsToChild(readOwnedEditorState(statePath)), 5000);
    auto production = makeDesktopComputerBackend();
    QVERIFY(production);
    DesktopNativeWindow target;
    MaiError discoveryError;
    const auto discoverChild = [&] {
        const auto listed = production->windows();
        if (!listed) {
            discoveryError = listed.error();
            return true;
        }
        int matches = 0;
        for (const auto& window : listed.value()) {
            if (window.processId == childPid && window.title == title.toUtf8().toStdString()) {
                target = window;
                ++matches;
            }
        }
        return matches == 1;
    };
    QTRY_VERIFY_WITH_TIMEOUT(discoverChild(), 5000);
    if (discoveryError) return failOrSkipNativeReadiness(discoveryError);
    QVERIFY(target.id != 0 && target.processId == childPid);
    auto backend = std::make_shared<ScopedNativeBackend>(production, target, childImages);
    const QString prompt = QStringLiteral("操作我打开的独立编辑器：输入中文，用全选快捷键替换草稿，清空后写入最终中文，按 Enter 并读回确认。");
    const QString expected = QStringLiteral("创作中心原生链路：中文输入与替换已读回。");
    const std::string expectedText = expected.toUtf8().toStdString();
    const auto confirmChild = [=] {
        QElapsedTimer elapsed;
        elapsed.start();
        do {
            const Json child = readOwnedEditorState(statePath);
            if (child.is_object() && child.value("pid", std::uint64_t(0)) == childPid &&
                child.value("title", std::string()) == target.title && child.value("text", std::string()) == expectedText &&
                child.value("confirmed_text", std::string()) == expectedText && child.value("enter_count", 0) == 1) return true;
            QThread::msleep(10);
        } while (elapsed.elapsed() < 2000);
        return false;
    };
    auto script = std::make_shared<ScriptState>();
#if defined(Q_OS_MACOS)
    const std::string defaultSelectAll = "Cmd+A";
#else
    const std::string defaultSelectAll = "Ctrl+A";
#endif
    AgentController controller(
        std::make_unique<ComputerScriptModel>(script, prompt.toUtf8().toStdString(), expectedText, target.title,
            "Native integration editor input", defaultSelectAll, true, confirmChild),
        workspace.filePath(QStringLiteral("native-creation-center.sqlite")),
        [backend](MaiToolRegistry& tools) {
            registerDesktopComputerTools(tools, std::make_shared<DesktopComputerController>(backend));
        });
    QVERIFY(controller.setApprovalPolicy(MaiApprovalPolicy::Never));
    AgentChatPanel panel(controller);
    panel.setWindowTitle(QStringLiteral("创作中心 · 原生电脑操作测试"));
    panel.resize(1000, 800);
    const QString session = controller.createSession(workspace.path(), QStringLiteral("创作中心真实原生输入"));
    QVERIFY(!session.isEmpty());
    // These captures must outlive StopAgentOnExit because it pumps queued UI
    // events while a failed/early assertion stops the worker.
    bool evidenceOk = true;
    int capturedUiInput = 0;
    StopAgentOnExit stop{controller, session};
    panel.openSession(session);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    const QString readyPath = QDir(directory.isEmpty() ? workspace.path() : directory)
        .filePath(QStringLiteral("creation-native-ready.json"));
    const auto foregroundReady = waitForOwnedNativeForeground(*backend, target, owned.process, readyPath);
    if (!foregroundReady) return failOrSkipNativeReadiness(foregroundReady.error());
    if (!foregroundReady.value()) {
        QVERIFY(!backend->beganInput.load());
        return skipNative(
            "Owned native editor is not the actual foreground with all keyboard/mouse input released for 200 ms; no "
            "native input was sent.");
    }
    // Capture is separate native fixture evidence. It is never returned by the
    // metadata/UIA-only model tool or attached to the real conversation.
    // Read-only preflight checks capture, UIA and secure-input permissions before
    // the real conversation is allowed to dispatch input.
    MaiScreenshot preflightCapture;
    const MaiError captured = backend->capture(target, preflightCapture);
    if (captured) return failOrSkipNativeReadiness(captured);
    QVERIFY(!preflightCapture.pngBytes.empty());
    const auto elements = backend->elements(target);
    if (!elements) return failOrSkipNativeReadiness(elements.error());
    int namedInputs = 0;
    for (const auto& element : elements.value())
        if (element.label == "Native integration editor input") ++namedInputs;
    QCOMPARE(namedInputs, 1);
    const auto secure = backend->secureInput();
    if (!secure) return failOrSkipNativeReadiness(secure.error());
    if (secure.value()) return skipNative("The focused input is secure; no native input was sent.");
    const auto initialText = backend->focusedText(target);
    if (!initialText) return failOrSkipNativeReadiness(initialText.error());
    const auto stillFocused = backend->isFocused(target);
    if (!stillFocused) return failOrSkipNativeReadiness(stillFocused.error());
    if (!stillFocused.value()) return skipNative("Owned editor lost actual foreground during read-only preflight; no native input was sent.");
    QVERIFY(!backend->beganInput.load());

    auto* promptEditor = panel.findChild<QTextEdit*>(QStringLiteral("agentPromptEditor"));
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    QVERIFY(promptEditor && send);
    QObject::connect(&controller, &AgentController::toolPartChanged, &panel,
        [&](const QString& owner, const QString&, const QString&) {
            const int count = backend->inputs.load();
            if (owner != session || count <= capturedUiInput || panelImages.isEmpty()) return;
            capturedUiInput = count;
            evidenceOk = evidenceOk && panel.grab().save(QDir(panelImages).filePath(
                QStringLiteral("native-tool-input-%1.png").arg(count, 2, 10, QLatin1Char('0'))));
        });
    promptEditor->setPlainText(prompt);
    if (!panelImages.isEmpty()) QVERIFY(panel.grab().save(QDir(panelImages).filePath(QStringLiteral("chinese-user-input.png"))));
    // This local widget click submits the actual composer without activating
    // the parent window or changing the already verified OS foreground.
    send->click();
    QTRY_VERIFY_WITH_TIMEOUT(script->started.load(), 5000);
    QElapsedTimer completion;
    completion.start();
    while (!script->done.load() && !script->failed.load() && completion.elapsed() < 60000) QTest::qWait(20);
    const bool finished = script->done.load() || script->failed.load();
    if (!finished) controller.interrupt(session);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.agent().isBusy(session.toStdString()), 5000);
    const Json child = readOwnedEditorState(statePath);
    std::string scriptError;
    std::size_t calls = 0;
    std::size_t results = 0;
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        scriptError = script->error;
        calls = script->calls.size();
        results = script->results.size();
        if (!directory.isEmpty()) {
            const Json transcript{{"prompt", prompt.toUtf8().toStdString()}, {"expected_text", expectedText},
                                  {"backend", backend->diagnostics()}, {"script_error", scriptError},
                                  {"calls", script->calls}, {"results", script->results}, {"owned_editor_state", child}};
            QVERIFY(saveJsonEvidence(QDir(directory).filePath(QStringLiteral("native-creation-transcript.json")), transcript));
        }
    }
    if (!panelImages.isEmpty()) QVERIFY(panel.grab().save(QDir(panelImages).filePath(QStringLiteral("native-conversation-result.png"))));
    QVERIFY2(finished, "Native creation-center Agent workflow timed out; partial native screenshots and tool transcript were preserved.");
    if (script->failed.load()) {
        // Readiness may skip before input begins. Once a native operation was
        // attempted, a partial workflow must fail visibly rather than skip.
        if (backend->lostForeground.load() && !backend->beganInput.load()) {
            qWarning("Native workflow stopped after actual foreground changed: %s", scriptError.c_str());
            return skipNative("The owned editor lost actual foreground before native input began; tool/error evidence was preserved.");
        }
        QFAIL(scriptError.c_str());
    }
    QVERIFY(script->done.load());
    QVERIFY(script->verified.load());
    QVERIFY(script->receivedChinesePrompt.load());
    QVERIFY(script->declaredTool.load());
    QCOMPARE(calls, std::size_t(17));
    QCOMPARE(results, calls);
    QCOMPARE(backend->inputs.load(), 7);
    QCOMPARE(backend->captures.load(), 1);  // Only the explicit preflight capture.
    QVERIFY(backend->reads.load() >= 2);    // Real preflight and real tool verify.
    QVERIFY(stateBelongsToChild(child));
    QCOMPARE(child.value("text", std::string()), expectedText);
    QCOMPARE(child.value("confirmed_text", std::string()), expectedText);
    QCOMPARE(child.value("enter_count", 0), 1);
    QVERIFY(confirmChild());
    QVERIFY(evidenceOk);
    bool sawUser = false;
    bool sawFinal = false;
    int completedTools = 0;
    for (const auto& message : controller.agent().listMessages(session.toStdString())) {
        if (message.role == MaiRole::User && QString::fromStdString(message.text()) == prompt) sawUser = true;
        if (message.role == MaiRole::Assistant && QString::fromStdString(message.text()).contains(QStringLiteral("已读回确认：") + expected)) sawFinal = true;
        for (const auto& part : message.parts) {
            const auto* tool = std::get_if<MaiToolPart>(&part.body);
            if (tool && tool->tool == "desktop_computer" && tool->state == MaiToolState::Completed) ++completedTools;
        }
    }
    QVERIFY(sawUser);
    QVERIFY(sawFinal);
    QCOMPARE(completedTools, 17);
    QVERIFY(controller.agent().listPendingPermissions().empty());
    auto* history = panel.findChild<MarkdownView*>();
    QVERIFY(history);
    QTRY_VERIFY_WITH_TIMEOUT((history->selectAll(), history->selectedText().contains(expected)), 5000);
    history->clearSelection();
    MaiScreenshot completedCapture;
    QVERIFY(!backend->capture(target, completedCapture));
    if (!panelImages.isEmpty()) QVERIFY(panel.grab().save(QDir(panelImages).filePath(QStringLiteral("native-verified-final.png"))));
}

int main(int argc, char** argv) {
    // Keep Qt 5.15's QTEST_MAIN widget setup while exposing native QSKIP as a
    // process status: Windows GUI targets may have no capturable stdout.
    TESTLIB_SELFCOVERAGE_START("DesktopComputerAgentIntegrationTest")
    QT_PREPEND_NAMESPACE(QTest::Internal::callInitMain)<DesktopComputerAgentIntegrationTest>();
    QApplication app(argc, argv);
    app.setAttribute(Qt::AA_Use96Dpi, true);
    QTEST_DISABLE_KEYPAD_NAVIGATION
    DesktopComputerAgentIntegrationTest test;
    QTEST_SET_MAIN_SOURCE_PATH
    const int result = QTest::qExec(&test, argc, argv);
    return result == 0 && test.nativeSkipped ? 77 : result;
}
#include "DesktopComputerAgentIntegrationTest.moc"
