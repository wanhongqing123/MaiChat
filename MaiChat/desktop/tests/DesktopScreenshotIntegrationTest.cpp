// Exercises the real Creation Center composer, Agent loop, native screenshot,
// image feedback and media card. Only this process's nonce-titled window is captured;
// the scripted model decodes PNG bytes and makes no claim of visual understanding.
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QUuid>
#include <QVBoxLayout>
#include <QtTest>

#include <atomic>
#include <memory>
#include <mutex>
#include <variant>

#include <json.hpp>
#if defined(Q_OS_MACOS)
#include <CoreGraphics/CoreGraphics.h>
#endif
#include "MaiScreenshot.h"
#include "agent/AgentChatPanel.h"
#include "agent/AgentController.h"
#include "markdown/MarkdownView.h"

namespace {
using Json = nlohmann::json;

struct ScriptState {
    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};
    std::mutex mutex;
    std::string error;
    std::string imagePath;
    QByteArray png;
    Json trace = Json::array();
    int requests = 0;
    MaiError fail(const std::string& message) {
        error = message;
        failed = true;
        return {MaiErrorCode::Protocol, message};
    }
};

class ScreenshotScript final : public MaiModelClient {
public:
    ScreenshotScript(std::shared_ptr<ScriptState> state, QString title, QSize pixels, QString prompt)
        : state_(std::move(state)), title_(title.toUtf8().toStdString()), pixels_(pixels),
          prompt_(prompt.toUtf8().toStdString()) {}

    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>& cancel) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (cancel.load()) return {MaiErrorCode::Canceled, "screenshot fixture canceled"};
        const int step = state_->requests++;
        if (request.model != "deepseek-flash") return state_->fail("selected model did not reach the Agent request");
        bool screenshotDeclared = false, mediaDeclared = false, promptFound = false;
        for (const auto& tool : request.tools) {
            screenshotDeclared |= tool.name == "screenshot";
            mediaDeclared |= tool.name == "agent_send_media";
        }
        for (const auto& message : request.messages)
            promptFound |= message.role == MaiModelRole::User && message.content == prompt_;
        if (!screenshotDeclared || !mediaDeclared || !promptFound)
            return state_->fail("real Agent request lacks the prompt or production tool declarations");
        if (!sink.onToolCall) return state_->fail("missing Agent tool dispatch callback");
        if (step == 0) {
            const Json input{{"mode", "window"}, {"windowTitle", title_}};
            state_->trace.push_back({{"tool", "screenshot"}, {"input", input}});
            sink.onToolCall({"owned-capture", "screenshot", input.dump()});
            return {};
        }
        const char* expectedId = step == 1 ? "owned-capture" : "owned-delivery";
        const MaiModelMessage* paired = nullptr;
        int pairedCount = 0;
        for (const auto& message : request.messages)
            if (message.role == MaiModelRole::ToolResult && message.toolCallId == expectedId) {
                paired = &message;
                ++pairedCount;
            }
        if (pairedCount != 1) return state_->fail("missing or duplicate paired production tool result");
        const Json result = Json::parse(paired->content, nullptr, false);
        state_->trace.push_back({{"call_id", expectedId}, {"result", result.is_discarded() ? Json(paired->content) : result}});
        if (!result.is_object()) return state_->fail(paired->content);
        if (step == 1) {
            if (result.value("type", "") != "image" || result.value("mime_type", "") != "image/png" ||
                result.value("mode", "") != "window" || !result.contains("image_path") ||
                !result["image_path"].is_string()) return state_->fail("screenshot JSON contract is incomplete");
            state_->imagePath = result["image_path"].get<std::string>();
            const QString relative = QString::fromStdString(state_->imagePath);
            const QString root = QFileInfo(QString::fromStdString(request.workingDirectory)).canonicalFilePath();
            const QString absolute = QFileInfo(QDir(root).filePath(relative)).canonicalFilePath();
            if (relative.isEmpty() || QFileInfo(relative).isAbsolute() || root.isEmpty() ||
                !absolute.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive))
                return state_->fail("screenshot image_path is not an existing workspace-relative file");
            int images = 0;
            for (const auto& message : request.messages)
                for (const auto& image : message.images) {
                    ++images;
                    const QString attachment = QString::fromStdString(image.path);
                    if (!QFileInfo(attachment).isAbsolute() || QFileInfo(attachment).canonicalFilePath() != absolute ||
                        image.mimeType != "image/png") return state_->fail("model attachment does not identify the saved native PNG");
                }
            if (images != 1) return state_->fail("second model request must contain exactly one native image attachment");
            QFile file(absolute);
            if (!file.open(QIODevice::ReadOnly)) return state_->fail("cannot read native PNG attachment");
            state_->png = file.readAll();
            const QImage decoded = QImage::fromData(state_->png, "PNG");
            if (!state_->png.startsWith(QByteArray::fromHex("89504e470d0a1a0a")) || decoded.isNull() ||
                decoded.size() != pixels_ || result.value("width", 0) != decoded.width() ||
                result.value("height", 0) != decoded.height())
                return state_->fail("saved native PNG dimensions do not match the isolated owner window");
            const Json input{{"file_path", state_->imagePath}, {"type", "image"}};
            state_->trace.push_back({{"tool", "agent_send_media"}, {"input", input},
                                     {"attachment", "same_saved_native_png"}, {"decoded_width", decoded.width()},
                                     {"decoded_height", decoded.height()}, {"png_bytes", state_->png.size()}});
            sink.onToolCall({"owned-delivery", "agent_send_media", input.dump()});
            return {};
        }
        if (step != 2 || result.value("delivery", "") != "current_ai_session" ||
            result.value("path", "") != state_->imagePath || result.value("type", "") != "image" ||
            result.value("mime_type", "") != "image/png" || result.value("bytes", qint64(0)) != state_->png.size())
            return state_->fail("media receipt did not deliver the same original native PNG");
        if (sink.onText) sink.onText("The captured fixture image is attached.");
        state_->done = true;
        return {};
    }
    MaiWireApi wireApi() const override { return MaiWireApi::ChatCompletions; }
private:
    std::shared_ptr<ScriptState> state_;
    std::string title_;
    QSize pixels_;
    std::string prompt_;
};

struct StopAgentOnExit {
    AgentController& controller;
    QString session;
    ~StopAgentOnExit() {
        if (!controller.agent().isBusy(session.toStdString())) return;
        controller.interrupt(session);
        QElapsedTimer timer;
        timer.start();
        while (controller.agent().isBusy(session.toStdString()) && timer.elapsed() < 5000) QTest::qWait(10);
    }
};

bool saveBytes(const QString& path, const QByteArray& bytes) {
    QSaveFile output(path);
    return output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.commit();
}
} // namespace

class DesktopScreenshotIntegrationTest final : public QObject {
    Q_OBJECT
public:
    bool nativeSkipped = false;
private:
    void skipNative(const char* reason) { nativeSkipped = true; QSKIP(reason); }
private slots:
    void nativeCaptureReturnsToCreationCenter();
};

void DesktopScreenshotIntegrationTest::nativeCaptureReturnsToCreationCenter() {
#if defined(Q_OS_WIN)
    if (QApplication::platformName() != QStringLiteral("windows"))
        return skipNative("A real Windows QPA desktop is required; no native capture attempted.");
#elif defined(Q_OS_MACOS)
    if (QApplication::platformName() != QStringLiteral("cocoa"))
        return skipNative("A real Cocoa desktop is required; no native capture attempted.");
#else
    return skipNative("Native screenshot integration is available only on Windows and macOS.");
#endif
    if (!maiIsScreenshotSupported()) return skipNative("This OS does not provide the native screenshot backend.");
#if defined(Q_OS_MACOS)
    if (!CGPreflightScreenCaptureAccess())
        return skipNative("Screen recording permission is unavailable; no native capture or permission prompt attempted.");
#endif
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    const QString nonce = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QWidget owned(nullptr, Qt::Window | Qt::FramelessWindowHint);
    owned.setAttribute(Qt::WA_ShowWithoutActivating);
    owned.setWindowTitle(QStringLiteral("MaiChat isolated screenshot %1").arg(nonce));
    owned.setFixedSize(480, 320);
    owned.setStyleSheet(QStringLiteral("background:#16394f;color:#ffffff;"));
    auto* fixtureLayout = new QVBoxLayout(&owned);
    auto* marker = new QLabel(QStringLiteral("OWNED NATIVE CAPTURE\n%1").arg(nonce), &owned);
    marker->setAlignment(Qt::AlignCenter);
    marker->setWordWrap(true);
    marker->setStyleSheet(QStringLiteral("background:#ce7537;padding:30px;font-size:22px;"));
    fixtureLayout->addWidget(marker);
    owned.show();
    QVERIFY(QTest::qWaitForWindowExposed(&owned));
    const QSize pixels(qRound(owned.width() * owned.devicePixelRatioF()), qRound(owned.height() * owned.devicePixelRatioF()));
    const QString prompt = QStringLiteral("Capture only my isolated test window '%1' and show that same image here.")
                               .arg(owned.windowTitle());
    auto state = std::make_shared<ScriptState>();
    AgentController controller(std::make_unique<ScreenshotScript>(state, owned.windowTitle(), pixels, prompt),
                               workspace.filePath(QStringLiteral("screenshot-agent.sqlite")));
    QVERIFY(controller.openError().isEmpty());
    QVERIFY(controller.setApprovalPolicy(MaiApprovalPolicy::Never));
    QVERIFY(controller.setDefaultModel(QStringLiteral("deepseek-flash")));
    AgentChatPanel panel(controller);
    panel.setAttribute(Qt::WA_ShowWithoutActivating);
    panel.resize(900, 760);
    panel.setModelLabel(QStringLiteral("deepseek-flash"));
    const QString session = controller.createSession(workspace.path(), QStringLiteral("Owned screenshot integration"));
    QVERIFY(!session.isEmpty());
    StopAgentOnExit stop{controller, session};
    panel.openSession(session);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    auto* editor = panel.findChild<QTextEdit*>(QStringLiteral("agentPromptEditor"));
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    QVERIFY(editor && send);
    editor->setPlainText(prompt);
    send->click(); // Local widget submission never injects OS input or activates another window.
    QTRY_VERIFY_WITH_TIMEOUT(state->done.load() || state->failed.load(), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.agent().isBusy(session.toStdString()), 5000);
    const QString evidence = qEnvironmentVariable("MAICHAT_SCREENSHOT_EVIDENCE_DIR");
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!evidence.isEmpty()) {
            QVERIFY(QDir().mkpath(evidence));
            if (!state->png.isEmpty()) QVERIFY(saveBytes(QDir(evidence).filePath(QStringLiteral("native-owned-window.png")), state->png));
            const Json transcript{{"backend", "production_native_window_screenshot"}, {"model", "deepseek-flash"},
                                  {"requests", state->requests}, {"error", state->error}, {"trace", state->trace}};
            QByteArray sanitized = QByteArray::fromStdString(transcript.dump(2));
            sanitized.replace(workspace.path().toUtf8(), "<temporary-workspace>");
            sanitized.replace(QDir::toNativeSeparators(workspace.path()).toUtf8(), "<temporary-workspace>");
            QByteArray escaped = QDir::toNativeSeparators(workspace.path()).toUtf8();
            escaped.replace("\\", "\\\\");
            sanitized.replace(escaped, "<temporary-workspace>");
            QVERIFY(saveBytes(QDir(evidence).filePath(QStringLiteral("screenshot-transcript.json")), sanitized));
        }
        if (state->failed.load()) {
            // Only explicit unavailable capabilities skip. Missing windows, timeouts,
            // malformed metadata and filesystem failures remain test failures.
            if (state->error.find("Allow Screen & System Audio Recording") != std::string::npos ||
                state->error == "Windows Graphics Capture is not supported on this system")
                return skipNative(state->error.c_str());
            QFAIL(state->error.c_str());
        }
        QCOMPARE(state->requests, 3);
        QVERIFY(state->done.load());
    }
    QCOMPARE(QDir(workspace.filePath(QStringLiteral("screenshots"))).entryList({QStringLiteral("*.png")}, QDir::Files).size(), 1);
    int completed = 0, attached = 0;
    QString deliveredPart;
    for (const auto& message : controller.agent().listMessages(session.toStdString()))
        for (const auto& part : message.parts) {
            if (const auto* tool = std::get_if<MaiToolPart>(&part.body)) {
                QVERIFY(tool->tool == "screenshot" || tool->tool == "agent_send_media");
                QCOMPARE(tool->state, MaiToolState::Completed);
                ++completed;
                if (tool->tool == "agent_send_media") deliveredPart = QString::fromStdString(part.id);
            }
            if (const auto* image = std::get_if<MaiImagePart>(&part.body)) {
                QCOMPARE(QFileInfo(QString::fromStdString(image->path)).canonicalFilePath(),
                         QFileInfo(workspace.filePath(QString::fromStdString(state->imagePath))).canonicalFilePath());
                QCOMPARE(image->mimeType, std::string("image/png"));
                ++attached;
            }
        }
    QCOMPARE(completed, 2);
    QCOMPARE(attached, 1);
    QVERIFY(controller.agent().listPendingPermissions().empty());
    auto* history = panel.findChild<MarkdownView*>();
    QVERIFY(history);
    QTRY_VERIFY_WITH_TIMEOUT(history->contains(deliveredPart + QStringLiteral("-agent-media")), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(panel.findChildren<QPushButton*>(QStringLiteral("agentMediaImageOpen")).size(), 1, 5000);
    auto* open = panel.findChild<QPushButton*>(QStringLiteral("agentMediaImageOpen"));
    QTRY_VERIFY_WITH_TIMEOUT(open->isVisible(), 5000);
    auto* cover = open->parentWidget()->findChild<QLabel*>();
    QVERIFY(cover);
    QTRY_VERIFY_WITH_TIMEOUT(cover->pixmap() && !cover->pixmap()->isNull(), 5000);
    if (!evidence.isEmpty()) {
        QVERIFY(owned.grab().save(QDir(evidence).filePath(QStringLiteral("owned-fixture-widget.png"))));
        QVERIFY(panel.grab().save(QDir(evidence).filePath(QStringLiteral("creation-center-delivered-image.png"))));
    }
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setAttribute(Qt::AA_Use96Dpi, true);
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("MaiChatScreenshotIntegration"));
    QCoreApplication::setApplicationName(QStringLiteral("isolated-native-screenshot-test"));
    DesktopScreenshotIntegrationTest test;
    const int result = QTest::qExec(&test, argc, argv);
    return result == 0 && test.nativeSkipped ? 77 : result;
}
#include "DesktopScreenshotIntegrationTest.moc"
