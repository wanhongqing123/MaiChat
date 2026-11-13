// 文件作用：验证桌面端创作中心会话切换、工作区选择及跨会话状态隔离。
// 运行边界：由测试进程驱动实际组件或替身，不作为正式 App 的用户入口。
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QDataStream>
#include <QDebug>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFuture>
#include <QFutureWatcher>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPushButton>
#include <QPointer>
#include <QSemaphore>
#include <QScrollBar>
#include <QScrollArea>
#include <QSet>
#include <QSignalSpy>
#include <QSplitter>
#include <QSplitterHandle>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTextBlock>
#include <QTextCursor>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>
#include <QtTest>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <variant>

#include "MaiSqliteStore.h"
#include "agent/AgentChatPanel.h"
#include "agent/AgentController.h"
#include "agent/AgentSessionList.h"
#include "agent/DesktopSessionSyncHost.h"
#include "markdown/MarkdownView.h"
#include "ui/ComposerTextEdit.h"
#include "ui/DesktopAudioPlayback.h"
#include "ui/UiZoom.h"

// AI 面板：**别的会话说的话不能灌进这个会话的界面**。
//
// 子 Agent 是并发跑在另一个会话里的。面板以前每个事件处理都忽略 sessionId——
// 那时候只有一个会话在跑，无害；子 Agent 一来就不是了：
//
//   正文   子的回答会接进父的答案里，用户看到「AI 说了些我没问的话」
//   状态   子跑完会把发送按钮从「停止」翻回「发送」，而父还在跑
//   标题   子的任务名会顶掉头部那一行
//
// 授权和提问是**例外**，那两样刻意不过滤：子 Agent 没有自己的界面，
// 滤掉的话它会永远挂在闸门上等一个不会来的点头。

namespace {

// 走普通 MIME 插入入口，不读取或覆盖操作系统剪贴板。
class MimeComposer final : public ComposerTextEdit {
public:
    using ComposerTextEdit::canInsertFromMimeData;
    using ComposerTextEdit::insertFromMimeData;
};

QString ownedComposerText() {
    return QString::fromUtf8("OWNED: https://example.invalid/issue/42\nChange-Id: I0123456789 中文 🙂 **literal**");
}

QString ownedComposerHtml() {
    return QStringLiteral("<p style=\"background-color:#111111\"><span style=\"color:#c5c5c5;"
                          "background-color:#111111;font-family:'Courier New';font-size:28px;font-weight:700\">"
                          "OWNED: <a href=\"https://example.invalid/issue/42\">https://example.invalid/issue/42</a>"
                          "<br>Change-Id: I0123456789 中文 🙂 **literal**</span></p>");
}

void dropOwnedComposerMime(QTextEdit& editor, const QMimeData& mime) {
    auto* target = editor.viewport();
    const QPointF position(target->width() / 2.0, target->height() / 2.0);
    QDragEnterEvent enter(position.toPoint(), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &enter);
    QDragMoveEvent move(position.toPoint(), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &move);
    QDropEvent drop(position, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &drop);
}

// 有声播放控件的回归只用本次临时目录中的静音文件，不打开用户媒体或发起模型网络请求。
bool writeInlineAudioFixture(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    constexpr quint32 sampleRate = 8000;
    constexpr quint32 bytes = sampleRate * 5 * 2;
    stream.writeRawData("RIFF", 4);
    stream << quint32(36 + bytes);
    stream.writeRawData("WAVEfmt ", 8);
    stream << quint32(16) << quint16(1) << quint16(1) << sampleRate << sampleRate * 2 << quint16(2) << quint16(16);
    stream.writeRawData("data", 4);
    stream << bytes;
    return stream.status() == QDataStream::Ok && file.write(QByteArray(bytes, '\0')) == bytes;
}

// Capture only isolated local fixtures; no user history or model request is involved.
bool captureAgentFixture(QWidget& panel, const QString& name) {
    const QString output = qEnvironmentVariable("MAICHAT_AGENT_EVIDENCE_DIR");
    if (output.isEmpty()) return true;
    if (!QDir().mkpath(output)) return false;
    QTest::qWait(80);
    return panel.grab().save(QDir(output).filePath(name + QStringLiteral(".png")));
}

// 一个什么都不说的模型。这份用例不需要真跑起来的回答——
// 事件是直接往总线上发的，那才是要测的路径。
class SilentModel final : public MaiModelClient {
public:
    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>& cancel) override {
        (void)request;
        (void)sink;
        (void)cancel;
        return {};
    }
    MaiWireApi wireApi() const override {
        return MaiWireApi::ChatCompletions;
    }
};

class CapturingModel final : public MaiModelClient {
public:
    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>& cancel) override {
        (void)cancel;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            request_ = request;
        }
        if (sink.onText) sink.onText("done");
        return {};
    }
    MaiWireApi wireApi() const override {
        return MaiWireApi::ChatCompletions;
    }
    MaiModelRequest request() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return request_;
    }

private:
    mutable std::mutex mutex_;
    MaiModelRequest request_;
};

class PdfGeneratingModel final : public MaiModelClient {
public:
    MaiError stream(const MaiModelRequest&, const MaiStreamSink& sink, const std::atomic<bool>&) override {
        if (calls_++ == 0) {
            if (sink.onToolCall)
                sink.onToolCall(MaiToolInvocation{"pdf-call", "generate_pdf",
                                                  R"({"content":"# Preview test","output_path":"preview.pdf"})"});
        } else if (sink.onText) {
            sink.onText("PDF created");
        }
        return {};
    }
    MaiWireApi wireApi() const override {
        return MaiWireApi::ChatCompletions;
    }

private:
    std::atomic<int> calls_{0};
};

class MediaDeliveringModel final : public MaiModelClient {
public:
    MaiError stream(const MaiModelRequest&, const MaiStreamSink& sink, const std::atomic<bool>&) override {
        if (calls_++ == 0) {
            if (sink.onToolCall) {
                sink.onToolCall(MaiToolInvocation{"media-call-1", "agent_send_media",
                                                  R"({"file_path":"sample.mp4","type":"video"})"});
                sink.onToolCall(MaiToolInvocation{"media-call-2", "agent_send_media",
                                                  R"({"file_path":"second.mp4","type":"video"})"});
            }
        } else if (sink.onText) {
            sink.onText("Video delivered");
        }
        return {};
    }
    MaiWireApi wireApi() const override {
        return MaiWireApi::ChatCompletions;
    }

private:
    std::atomic<int> calls_{0};
};

class AudioDeliveringModel final : public MaiModelClient {
public:
    MaiError stream(const MaiModelRequest&, const MaiStreamSink& sink, const std::atomic<bool>&) override {
        if (calls_++ == 0) {
            if (sink.onToolCall)
                sink.onToolCall(MaiToolInvocation{"audio-call", "agent_send_media",
                                                  R"({"file_path":"sample.wav","type":"audio"})"});
        } else if (sink.onText) {
            sink.onText("Audio delivered");
        }
        return {};
    }
    MaiWireApi wireApi() const override {
        return MaiWireApi::ChatCompletions;
    }

private:
    std::atomic<int> calls_{0};
};

class DelayedFirstChunkModel final : public MaiModelClient {
public:
    explicit DelayedFirstChunkModel(bool reasoning, int warmupCalls = 0)
        : reasoning_(reasoning), warmupCalls_(warmupCalls) {}

    MaiError stream(const MaiModelRequest&, const MaiStreamSink& sink, const std::atomic<bool>& cancel) override {
        if (calls_.fetch_add(1) < warmupCalls_) {
            std::string answer;
            for (int i = 0; i < 500; ++i) answer += "previous answer ";
            if (sink.onText) sink.onText(answer);
            return {};
        }
        entered_.store(true);
        gate_.tryAcquire(1, 3000);
        if (cancel.load()) return {};
        if (reasoning_ && sink.onReasoning) sink.onReasoning("Thinking");
        if (sink.onText) sink.onText("Answer");
        return {};
    }

    MaiWireApi wireApi() const override {
        return MaiWireApi::ChatCompletions;
    }
    void release() {
        gate_.release();
    }
    bool entered() const {
        return entered_.load();
    }

private:
    QSemaphore gate_;
    std::atomic<bool> entered_{false};
    std::atomic<int> calls_{0};
    bool reasoning_ = false;
    int warmupCalls_ = 0;
};

class ApprovalModel final : public MaiModelClient {
public:
    MaiError stream(const MaiModelRequest&, const MaiStreamSink& sink, const std::atomic<bool>&) override {
        if (calls_++ == 0) {
            if (sink.onToolCall)
                sink.onToolCall(MaiToolInvocation{"call-send", "maichat_send_text",
                                                  R"({"peer_id":"house-multi-ai-code","text":"hello"})"});
        } else if (sink.onText) {
            sink.onText("done");
        }
        return {};
    }

    MaiWireApi wireApi() const override {
        return MaiWireApi::ChatCompletions;
    }

private:
    std::atomic<int> calls_{0};
};

class ApprovalTool final : public MaiTool {
public:
    explicit ApprovalTool(std::atomic<int>& executed) : executed_(executed) {}
    std::string name() const override {
        return "maichat_send_text";
    }
    std::string description() const override {
        return "Send a test message.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object"})";
    }
    bool requiresApproval(const std::string&) const override {
        return true;
    }
    bool requiresPerCallApproval(const std::string&) const override {
        return true;
    }
    MaiToolResult execute(const std::string&, const MaiToolContext&) override {
        ++executed_;
        return MaiToolResult::success("queued");
    }

private:
    std::atomic<int>& executed_;
};

class QuestionModel final : public MaiModelClient {
public:
    MaiError stream(const MaiModelRequest&, const MaiStreamSink& sink, const std::atomic<bool>&) override {
        if (calls_++ == 0) {
            if (sink.onToolCall)
                sink.onToolCall(MaiToolInvocation{"call-question", "question", R"({"question":"Choose A or B"})"});
        } else if (sink.onText) {
            sink.onText("answered");
        }
        return {};
    }

    MaiWireApi wireApi() const override {
        return MaiWireApi::ChatCompletions;
    }

private:
    std::atomic<int> calls_{0};
};

class PartialReasoningModel final : public MaiModelClient {
public:
    std::atomic<bool> release{false};

    MaiError stream(const MaiModelRequest&, const MaiStreamSink& sink, const std::atomic<bool>& cancel) override {
        if (sink.onReasoning) sink.onReasoning("partial reasoning");
        while (!release.load() && !cancel.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (sink.onReasoning && !cancel.load()) sink.onReasoning(" completed");
        return {};
    }

    MaiWireApi wireApi() const override {
        return MaiWireApi::ChatCompletions;
    }
};

// 只生成固定的可见发言；门控工具阻塞期间，Qt 事件循环继续渲染真实面板。
// 不经过任何供应商、用户账号或付费任务。
struct IntermediateProgressState {
    std::atomic<int> entered{0};
    std::atomic<int> released{0};
};

class IntermediateProgressModel final : public MaiModelClient {
public:
    explicit IntermediateProgressModel(bool consecutiveTools = false) : consecutiveTools_(consecutiveTools) {}
    MaiError stream(const MaiModelRequest&, const MaiStreamSink& sink, const std::atomic<bool>&) override {
        const int step = calls_++;
        const bool finished = step >= (consecutiveTools_ ? 1 : 2);
        const std::string text = step == 0   ? "Progress A: inspect local inputs."
                                 : !finished ? "Progress B: validate observed results."
                                             : "Final C: both local checks completed.";
        MaiModelTextItem item;
        item.id = "fixture-message-" + std::to_string(step);
        item.index = 0;
        item.phase = finished ? MaiModelTextPhase::FinalAnswer : MaiModelTextPhase::Commentary;
        if (sink.onTextItemStarted) sink.onTextItemStarted(item);
        if (sink.onTextItemDelta)
            sink.onTextItemDelta(item, text);
        else if (sink.onText)
            sink.onText(text);
        if (sink.onTextItemCompleted) sink.onTextItemCompleted(item);
        if (!finished && sink.onToolCall) {
            sink.onToolCall(
                MaiToolInvocation{step == 0 ? "X" : "Y", step == 0 ? "progress_probe_x" : "progress_probe_y", "{}"});
            if (consecutiveTools_) sink.onToolCall(MaiToolInvocation{"Y", "progress_probe_y", "{}"});
        }
        return {};
    }
    MaiWireApi wireApi() const override {
        return MaiWireApi::Responses;
    }

private:
    int calls_ = 0;
    bool consecutiveTools_ = false;
};

class IntermediateProgressTool final : public MaiTool {
public:
    IntermediateProgressTool(std::shared_ptr<IntermediateProgressState> state, int stage)
        : state_(std::move(state)), stage_(stage) {}
    std::string name() const override {
        return stage_ == 1 ? "progress_probe_x" : "progress_probe_y";
    }
    std::string description() const override {
        return "Read-only deterministic UI check.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object"})";
    }
    MaiToolResult execute(const std::string&, const MaiToolContext& context) override {
        state_->entered.store(stage_);
        while (state_->released.load() < stage_ && !context.cancel->load())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return MaiToolResult::success("Observed local fixture stage " + std::to_string(stage_));
    }

private:
    std::shared_ptr<IntermediateProgressState> state_;
    int stage_;
};

struct StoppingComposerState {
    std::atomic<bool> entered{false};
    std::atomic<bool> canceled{false};
    std::atomic<bool> release{false};
};

class StoppingComposerModel final : public MaiModelClient {
public:
    MaiError stream(const MaiModelRequest&, const MaiStreamSink& sink, const std::atomic<bool>&) override {
        if (calls_++ == 0 && sink.onToolCall)
            sink.onToolCall(MaiToolInvocation{"composer-hold", "composer_hold", "{}"});
        else if (sink.onText)
            sink.onText("Completed fixture work.");
        return {};
    }
    MaiWireApi wireApi() const override {
        return MaiWireApi::ChatCompletions;
    }

private:
    int calls_ = 0;
};

class StoppingComposerTool final : public MaiTool {
public:
    explicit StoppingComposerTool(std::shared_ptr<StoppingComposerState> state) : state_(std::move(state)) {}
    std::string name() const override {
        return "composer_hold";
    }
    std::string description() const override {
        return "Hold a local composer fixture.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object"})";
    }
    MaiToolResult execute(const std::string&, const MaiToolContext& context) override {
        state_->entered = true;
        while (!state_->release.load()) {
            if (context.cancel->load()) state_->canceled = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return MaiToolResult::success("Fixture released.");
    }

private:
    std::shared_ptr<StoppingComposerState> state_;
};

QString selectedDocumentText(MarkdownView& view) {
    view.selectAll();
    const QString result = view.selectedText();
    view.clearSelection();
    return result;
}

QWidget* progressToolGroup(AgentChatPanel& panel, const QString& tool) {
    for (QPushButton* disclosure : panel.findChildren<QPushButton*>(QStringLiteral("agentToolGroupDisclosure"))) {
        QWidget* group = disclosure->parentWidget();
        for (QLabel* label : group->findChildren<QLabel*>())
            if (label->text().contains(tool)) return group;
    }
    return nullptr;
}

// QWidget::grab 只渲染本测试创建的 Qt 子树，不裁剪桌面，因此不会带入被覆盖的用户窗口。
bool captureProgressFixture(AgentChatPanel& panel, const QString& name) {
    const QString directory = qEnvironmentVariable("MAICHAT_AGENT_PROGRESS_EVIDENCE_DIR");
    if (directory.isEmpty()) return true;
    if (!QDir().mkpath(directory)) return false;
    return panel.grab().save(QDir(directory).filePath(name + QStringLiteral(".png")));
}

QString mediaImportPendingText(const AgentChatPanel& panel) {
    QStringList pending;
    const auto* tray = panel.findChild<QScrollArea*>(QStringLiteral("agentMediaDraftTray"));
    if (!tray) return {};
    for (const auto* label : tray->findChildren<QLabel*>())
        if (label->text().startsWith(QStringLiteral("正在导入媒体"))) pending.push_back(label->text());
    return pending.join(QStringLiteral(" | "));
}

// Block only this test's media worker and retain its state until the worker exits.
// Every assertion-return path releases it and restores the original capacity;
// never use waitForDone(), which could wait for unrelated work in the same pool.
class ScopedComposerImportBlocker {
public:
    explicit ScopedComposerImportBlocker(QThreadPool* pool)
        : pool_(pool), previousMax_(pool->maxThreadCount()), gate_(std::make_shared<Gate>()) {
        pool_->setMaxThreadCount(1);
        blocker_ = QtConcurrent::run(pool_, [gate = gate_] {
            gate->entered.release();
            gate->release.acquire();
        });
    }
    ~ScopedComposerImportBlocker() {
        release();
    }
    bool waitUntilEntered() {
        return gate_->entered.tryAcquire(1, 1000);
    }
    bool isBlocked() const {
        return !blocker_.isFinished();
    }
    void release() {
        if (released_) return;
        released_ = true;
        gate_->release.release();
        pool_->setMaxThreadCount(previousMax_);
        blocker_.waitForFinished();
    }

private:
    struct Gate {
        QSemaphore entered;
        QSemaphore release;
    };
    QThreadPool* pool_;
    int previousMax_;
    std::shared_ptr<Gate> gate_;
    QFuture<void> blocker_;
    bool released_ = false;
};

enum class MediaImportTermination { Cancel, Deadline };

void verifyMediaImportTermination(MediaImportTermination termination) {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString imagePath = directory.filePath(QStringLiteral("terminal-import.png"));
    QImage image(32, 24, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::blue);
    QVERIFY(image.save(imagePath, "PNG"));
    auto model = std::make_unique<CapturingModel>();
    CapturingModel* captured = model.get();
    AgentController controller(std::move(model), QString());
    AgentChatPanel panel(controller);
    panel.resize(700, 500);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel, 1000));
    panel.openSession(controller.createSession(directory.path()));
    auto* editor = panel.findChild<QTextEdit*>(QStringLiteral("agentPromptEditor"));
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    QVERIFY(editor && send);
    const auto pasteFixture = [editor, imagePath] {
        auto* mime = new QMimeData;
        mime->setUrls({QUrl::fromLocalFile(imagePath)});
        QApplication::clipboard()->setMimeData(mime);
        editor->paste();
    };
    // A normal completed import creates the production pool. Clear its marker
    // before blocking that pool, so only the second import can affect this case.
    pasteFixture();
    QTRY_VERIFY_WITH_TIMEOUT(editor->toPlainText().contains(QChar(0xFFFC)), 1000);
    editor->clear();
    QTRY_VERIFY_WITH_TIMEOUT(mediaImportPendingText(panel).isEmpty(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        panel.findChildren<QFutureWatcherBase*>(QStringLiteral("agentMediaImportWatcher")).isEmpty(), 1000);
    auto* pool = qApp->findChild<QThreadPool*>(QStringLiteral("agentComposerMediaImportPool"));
    QVERIFY2(pool, "The ordinary composer import must expose its named diagnostic pool.");
    ScopedComposerImportBlocker blocker(pool);
    QVERIFY(blocker.waitUntilEntered());
    const QString text = QStringLiteral("Keep this text while media imports.");
    editor->setPlainText(text);
    pasteFixture();
    QTRY_VERIFY_WITH_TIMEOUT(!mediaImportPendingText(panel).isEmpty(), 1000);
    QCOMPARE(editor->toPlainText(), text);
    const QString capturePrefix = QString::fromUtf8(qgetenv("MAICHAT_CAPTURE_MEDIA_IMPORT_PREFIX"));
    const QString captureKind = termination == MediaImportTermination::Cancel
                                    ? QStringLiteral("-cancel") : QStringLiteral("-deadline");
    if (!capturePrefix.isEmpty())
        QVERIFY(panel.grab().save(capturePrefix + captureKind + QStringLiteral("-pending.png")));
    auto* watcher = panel.findChild<QFutureWatcherBase*>(QStringLiteral("agentMediaImportWatcher"));
    QVERIFY(watcher);
    auto* deadline = watcher->findChild<QTimer*>(QStringLiteral("agentMediaImportDeadline"));
    QVERIFY(deadline);
    QVERIFY(deadline->isActive());
    QVERIFY(deadline->interval() > 0 && deadline->interval() <= 60000);
    QSignalSpy importFinished(watcher, &QFutureWatcherBase::finished);
    if (termination == MediaImportTermination::Cancel) {
        auto* cancel = panel.findChild<QPushButton*>(QStringLiteral("agentCancelMediaImports"));
        QVERIFY(cancel && cancel->isEnabled());
        cancel->click();
    } else {
        // Exercise the real production timer and event delivery, with no clock
        // substitute or test-only cancellation API.
        deadline->start(20);
    }
    QTRY_VERIFY_WITH_TIMEOUT(mediaImportPendingText(panel).isEmpty(), 1000);
    QCOMPARE(editor->toPlainText(), text);
    if (!capturePrefix.isEmpty())
        QVERIFY(panel.grab().save(capturePrefix + captureKind + QStringLiteral("-recovered.png")));
    QVERIFY(blocker.isBlocked());
    QCOMPARE(importFinished.count(), 0);
    QVERIFY(send->isEnabled());
    QSignalSpy turnFinished(&controller, &AgentController::turnFinished);
    send->click();
    QTRY_COMPARE_WITH_TIMEOUT(turnFinished.count(), 1, 1000);
    const MaiModelRequest request = captured->request();
    QVERIFY(!request.messages.empty());
    QCOMPARE(QString::fromStdString(request.messages.back().content), text);
    QVERIFY(request.messages.back().images.empty());
    const QString laterDraft = QStringLiteral("New text after the import was released.");
    editor->setPlainText(laterDraft);
    blocker.release();
    QTRY_COMPARE_WITH_TIMEOUT(importFinished.count(), 1, 1000);
    QCOMPARE(editor->toPlainText(), laterDraft);
    QVERIFY(!editor->toPlainText().contains(QChar(0xFFFC)));
    QVERIFY(mediaImportPendingText(panel).isEmpty());
}

}  // namespace

class AgentPanelSessionTest : public QObject {
    Q_OBJECT

private slots:
    void newSessionAvoidsFilesystemRoot();
    void emptySessionShowsWelcomeAndFirstMessageHidesIt();
    void newSessionUsesAppWorkspaceInsteadOfProcessDirectory();
    void newSessionOffersWorkspaceChoice();
    void chosenWorkspaceKeepsCurrentSession();
    void explicitNewSessionUsesChosenDirectory();
    void chosenWorkspaceRejectsBusySession();
    void sessionToolEventsRefreshListAndRecoverDeletedSelection();
    void localSessionMenuOffersDelete();
    void answerFromAnotherSessionDoesNotLeakIn();
    void anotherSessionFailingDoesNotShowAnError();
    void anotherSessionTitleDoesNotRenameTheHeader();
    void currentSessionTitleStaysInTheSessionList();
    void subAgentCardsKeepTaskAndStreamedLatestLineVisible();
    void subAgentCardsUseConsistentWidthAndExpandableSummary();
    void subAgentCardsStayBesideTheirSpawnPartsAcrossReplay_data();
    void subAgentCardsStayBesideTheirSpawnPartsAcrossReplay();
    void subAgentCardsWaitForTheirHistoryPage();
    void approvalFromAnotherSessionIsStillShown();
    void pendingApprovalIsRestoredAfterSessionSwitch();
    void approvedChildToolCompletesWhileParentPageIsCached();
    void pendingQuestionIsRestoredOnlyInItsSession();
    void questionUsesNormalTranscriptAndSurvivesColdReplay();
    void childQuestionUsesParentComposerWithoutConsumingMedia();
    void childQuestionKeepsItsPositionBetweenParentParts();
    void nestedQuestionsFollowOrderAndReleaseAfterCancellation();
    void rootQuestionAfterSteeringKeepsItsPartPosition();
    void stoppedQuestionReleasesComposerWithoutLosingDraft();
    void externalQuestionAnswerReleasesComposerWithoutLosingDraft();
    void expiredQuestionKeepsDraftUntilExplicitResend();
    void composerDraftsStayInTheirSessions();
    void waitingIndicatorSurvivesSwitchingAwayAndBack();
    void reasoningStatusSurvivesSwitchBeforeCheckpoint();
    void thinkingContentNeverRendersLiveOrAfterRestore();
    void intermediateProgressStaysVisibleAcrossToolsAndHistoryRestore();
    void consecutiveToolsShareDisclosureWithoutChangingReceipts();
    void busyComposerAcceptsOrderedSteeringWithoutStopping();
    void historyOpensLatestPageAndPrependsWithoutMovingReadingAnchor();
    void sessionSwitchRetainsUnchangedRenderedMessages();
    void cachedSessionAddsBackgroundMessagesWithoutReplacingHistory();
    void clearedCachedSessionDoesNotRestoreRemovedHistory();
    void boundedSessionPagesPreserveComposerDrafts();
    void answeredCachedQuestionDoesNotReclaimComposer();
    void emptyCachedSessionCanLoadEarlierBackgroundHistory();
    void changedCompletedMessageRefreshesItsCachedPage();
    void collapsedToolDetailsAreFormattedOnlyOnDisclosure();
    void stopKeepsUnacceptedDraftAndInputEditable();
    void quoteActionFollowsStreamedOwnerAfterSessionRestore();
    void waitingIndicatorAppearsBeforeFirstModelChunk();
    void waitingIndicatorClearsWhenModelHasNoReasoning();
    void unconfiguredModelOpensConfigurationInsteadOfFailingTurn();
    void desktopAgentSuppliesMarkdownSystemPrompt();
    void composerMatchesImLayoutAndUsesEmbeddedSendAction();
    void composerTextMimeUsesOnlyLiteralText_data();
    void composerTextMimeUsesOnlyLiteralText();
    void composerMimeHandlerAndInlineImagesKeepTheirExistingPath();
    void normalAgentComposerDropsSourceFormattingAndSendsLiteralText();
    void conversationAndComposerUseResizableSplitter();
    void composerResizeHandleAlignsWithEditorBorder();
    void composerPlaceholderFitsAtMinimumHeight();
    void conversationUsesAvailableWidth();
    void pastedImageUsesTheSharedComposerAndReachesTheModel();
    void mediaImportDoesNotWaitForUnrelatedGlobalPoolWork();
    void cancelingMediaImportKeepsTextAndIgnoresLateCompletion();
    void mediaImportDeadlineReleasesTheComposer();
    void cancelingCompletedImportThenDestroyingPanelCleansOwnedCopy();
    void pastedVideoDraftSurvivesSessionSwitchAndReachesModel();
    void videoImportFinishesInItsOriginalSession();
    void actionPanelMatchesMobileModelAndPolicyFlow();
    void mainModelChoicesStaySeparateFromReasoningEffort();
    void mainModelValueFollowsTheSelectedSession();
    void firstModelSelectionPreservesTheBlankPageDraft();
    void newAgentOutputDoesNotInterruptReadingHistory();
    void switchingSessionsShowsLatestMessage();
    void inlineMediaBubblesRespectMessageBoundary();
    void completedPdfToolShowsPreviewCardAfterRestore();
    void videoArtifactUsesCompactPlayBadge();
    void audioArtifactUsesCompactPlaybackCard();
    void inlineAudioPausesWhenSwitchingSessions();
    void quotedMessageIsSentAndShownInTheComposer();
    void remotePromptAppearsInOpenOwnerSession();
    void localPromptKeepsOneBubbleAfterUserMessageEvent();
};

namespace {

// 一套跑起来的面板 + 它背后的核心。
struct Harness {
    std::unique_ptr<AgentController> controller;
    std::unique_ptr<AgentChatPanel> panel;
    QString mine;
    QString other;

    Harness() {
        // 空的数据库路径 = 内存库，用例之间互不影响。
        controller = std::make_unique<AgentController>(std::make_unique<SilentModel>(), QString());
        panel = std::make_unique<AgentChatPanel>(*controller);
        panel->resize(700, 500);
        panel->show();
        mine = controller->createSession(QDir::currentPath());
        other = controller->createSession(QDir::currentPath());
        panel->openSession(mine);
    }

    MarkdownView* view() const {
        return panel->findChild<MarkdownView*>();
    }

    QPushButton* sendButton() const {
        return panel->findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    }

    // 直接往事件总线上发一条，模拟另一个会话（子 Agent）在动。
    void publishFromOther(MaiEventType type, const QString& detail = QString()) {
        MaiEvent event;
        event.id = "evt_test";
        event.type = type;
        event.sessionId = other.toUtf8().constData();
        event.messageId = "msg_other";
        event.partId = "prt_other";
        event.field = "text";
        event.delta = detail.toUtf8().constData();
        event.detail = detail.toUtf8().constData();
        controller->agent().eventBus().publish(event);
        // 事件是排队送到主线程的，等它到。
        QTest::qWait(120);
    }
};

}  // namespace

void AgentPanelSessionTest::emptySessionShowsWelcomeAndFirstMessageHidesIt() {
    Harness harness;
    harness.panel->resize(1100, 800);
    QCoreApplication::processEvents();
    auto* welcome = harness.panel->findChild<QWidget*>(QStringLiteral("agentWelcomeView"));
    auto* heading = harness.panel->findChild<QLabel*>(QStringLiteral("agentWelcomeHeading"));
    auto* editor = harness.panel->findChild<QTextEdit*>(QStringLiteral("agentPromptEditor"));
    QVERIFY(welcome != nullptr && heading != nullptr && editor != nullptr);
    QCOMPARE(heading->text(), QStringLiteral("从这里开始创作"));
    const auto welcomeCards = harness.panel->findChildren<QFrame*>(QStringLiteral("agentWelcomeFeature"));
    // 账号资料与头像已加入正式能力展示，数量和明确条目一起覆盖，避免保留旧欢迎页预期。
    QCOMPARE(welcomeCards.size(), 19);
    QList<QString> titles;
    for (const QFrame* card : welcomeCards) titles.append(card->accessibleName());
    QVERIFY(titles.contains(QStringLiteral("终端与进程")));
    QVERIFY(titles.contains(QStringLiteral("电脑操作")));
    QVERIFY(titles.contains(QStringLiteral("日历导入")));
    QVERIFY(titles.contains(QStringLiteral("账号资料与头像")));
    QVERIFY(!titles.contains(QStringLiteral("当前位置")));
    QVERIFY(welcome->isVisibleTo(harness.panel.get()));
    QVERIFY(harness.view()->isEmpty());
    QCOMPARE(welcome->property("welcomeColumns").toInt(), 4);
    const auto titlesFit = [welcome] {
        for (QFrame* card : welcome->findChildren<QFrame*>(QStringLiteral("agentWelcomeFeature"))) {
            const QList<QLabel*> labels = card->findChildren<QLabel*>();
            if (labels.size() < 2) return false;
            const QLabel* title = labels.last();
            if (title->fontMetrics().horizontalAdvance(title->text()) > title->width()) return false;
        }
        return true;
    };
    QVERIFY2(titlesFit(), "wide Desktop welcome cards must show complete titles");
    const QString capture = qEnvironmentVariable("MAICHAT_WELCOME_CAPTURE");
    if (!capture.isEmpty()) QVERIFY(harness.panel->grab().save(capture));
    harness.panel->resize(700, 500);
    QCoreApplication::processEvents();
    auto* scroll = qobject_cast<QScrollArea*>(welcome);
    QVERIFY(scroll != nullptr);
    QCOMPARE(welcome->property("welcomeColumns").toInt(), 3);
    QVERIFY2(titlesFit(), "medium Desktop welcome cards must show complete titles");
    QVERIFY(!scroll->horizontalScrollBar()->isVisible());
    QVERIFY(scroll->verticalScrollBar()->maximum() > 0);
    const QString compactCapture = qEnvironmentVariable("MAICHAT_WELCOME_COMPACT_CAPTURE");
    if (!compactCapture.isEmpty()) QVERIFY(harness.panel->grab().save(compactCapture));
    harness.panel->resize(550, 500);
    QCoreApplication::processEvents();
    QCOMPARE(welcome->property("welcomeColumns").toInt(), 2);
    QVERIFY(scroll->verticalScrollBar()->maximum() > 0);

    editor->setPlainText(QStringLiteral("检查欢迎页切换"));
    harness.sendButton()->click();
    QTRY_VERIFY_WITH_TIMEOUT(!welcome->isVisibleTo(harness.panel.get()), 3000);
    QVERIFY(!harness.view()->isEmpty());

    harness.panel->openSession(harness.other);
    QTRY_VERIFY_WITH_TIMEOUT(welcome->isVisibleTo(harness.panel.get()), 3000);
    harness.panel->openSession(harness.mine);
    QTRY_VERIFY_WITH_TIMEOUT(!welcome->isVisibleTo(harness.panel.get()), 3000);
}

void AgentPanelSessionTest::remotePromptAppearsInOpenOwnerSession() {
    AgentController controller(std::make_unique<SilentModel>(), QString());
    AgentChatPanel panel(controller);
    panel.resize(700, 500);
    panel.show();
    const QString sessionId = controller.createSession(QDir::currentPath());
    panel.openSession(sessionId);
    auto* view = panel.findChild<MarkdownView*>();
    QVERIFY(view != nullptr);

    // Same entry point used when an iPhone sends into a Mac-owned Session.
    DesktopSessionSyncHost host(controller);
    const QJsonObject result = host.applyCommand(sessionId, {{QStringLiteral("action"), QStringLiteral("prompt")},
                                                             {QStringLiteral("text"), QStringLiteral("你好")}});
    QVERIFY(result.value(QStringLiteral("ok")).toBool());
    const QString userId = result.value(QStringLiteral("user_message_id")).toString();
    QVERIFY(!userId.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(view->contains(userId), 3000);
    view->selectAll();
    QCOMPARE(view->selectedText().count(QStringLiteral("你好")), 1);
    view->clearSelection();
    const QString capturePath = qEnvironmentVariable("MAICHAT_REMOTE_USER_CAPTURE");
    if (!capturePath.isEmpty()) QVERIFY(panel.grab().save(capturePath));
}

void AgentPanelSessionTest::localPromptKeepsOneBubbleAfterUserMessageEvent() {
    AgentController controller(std::make_unique<SilentModel>(), QString());
    AgentChatPanel panel(controller);
    panel.resize(700, 500);
    panel.show();
    const QString sessionId = controller.createSession(QDir::currentPath());
    panel.openSession(sessionId);
    auto* editor = panel.findChild<QTextEdit*>(QStringLiteral("agentPromptEditor"));
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    auto* view = panel.findChild<MarkdownView*>();
    QVERIFY(editor != nullptr && send != nullptr && view != nullptr);
    editor->setPlainText(QStringLiteral("本机消息"));
    send->click();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.agent().isBusy(sessionId.toStdString()), 3000);
    const auto messages = controller.agent().listMessagesPage(sessionId.toStdString(), "", 2);
    QCOMPARE(messages.size(), std::size_t(2));
    const QString userId = QString::fromStdString(messages.back().id);
    QCOMPARE(messages.back().role, MaiRole::User);
    QTRY_VERIFY_WITH_TIMEOUT(view->contains(userId), 3000);
    QCoreApplication::processEvents();  // Deliver queued MessageUpdated after the local button path.
    QVERIFY(view->contains(userId + QStringLiteral("-quote-action")));
    QVERIFY(!view->contains(QStringLiteral("local-1")));
    view->selectAll();
    QCOMPARE(view->selectedText().count(QStringLiteral("本机消息")), 1);
    view->clearSelection();

    const QString otherSession = controller.createSession(QDir::currentPath());
    panel.openSession(otherSession);
    panel.openSession(sessionId);
    QVERIFY(view->contains(userId));
    view->selectAll();
    QCOMPARE(view->selectedText().count(QStringLiteral("本机消息")), 1);
    view->clearSelection();
}

void AgentPanelSessionTest::completedPdfToolShowsPreviewCardAfterRestore() {
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    AgentController controller(std::make_unique<PdfGeneratingModel>(), QString());
    AgentChatPanel panel(controller);
    panel.resize(700, 500);
    panel.show();
    const QString session = controller.createSession(workspace.path());
    panel.openSession(session);
    QVERIFY(controller.sendPrompt(session, QStringLiteral("Create a PDF")));
    QTRY_VERIFY_WITH_TIMEOUT(!controller.agent().listPendingPermissions().empty(), 5000);
    const auto pending = controller.agent().listPendingPermissions();
    QVERIFY(controller.approvePermission(QString::fromStdString(pending.front().id)));
    controller.agent().waitIdle();
    QTRY_VERIFY_WITH_TIMEOUT(panel.findChild<QPushButton*>(QStringLiteral("agentPdfPreviewButton")) != nullptr, 5000);
    auto* toolSummary = panel.findChild<QPushButton*>(QStringLiteral("agentToolGroupDisclosure"));
    QVERIFY(toolSummary != nullptr);
    QVERIFY(toolSummary->text().contains(QStringLiteral("工具调用 1 次")));
    const QString capturePath = qEnvironmentVariable("MAICHAT_AGENT_TOOL_CAPTURE");
    if (!capturePath.isEmpty()) QVERIFY(panel.grab().save(capturePath));
    for (QWidget* card : panel.findChildren<QWidget*>(QStringLiteral("agentToolCard"))) QVERIFY(!card->isVisible());
    toolSummary->click();
    QTRY_VERIFY(!panel.findChildren<QWidget*>(QStringLiteral("agentToolCard")).isEmpty());
    QVERIFY(QFile::exists(workspace.filePath(QStringLiteral("preview.pdf"))));

    panel.openSession(session);
    QTRY_VERIFY_WITH_TIMEOUT(panel.findChild<QPushButton*>(QStringLiteral("agentPdfPreviewButton")) != nullptr, 5000);
}

void AgentPanelSessionTest::videoArtifactUsesCompactPlayBadge() {
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    QVERIFY(QFile::copy(QStringLiteral(MAICHAT_AGENT_VIDEO_FIXTURE), workspace.filePath(QStringLiteral("sample.mp4"))));
    QVERIFY(QFile::copy(QStringLiteral(MAICHAT_AGENT_VIDEO_FIXTURE), workspace.filePath(QStringLiteral("second.mp4"))));
    AgentController controller(std::make_unique<MediaDeliveringModel>(), QString());
    AgentChatPanel panel(controller);
    panel.resize(700, 550);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    QString openedVideo;
    QStringList videoPlaylist;
    panel.setOpenVideoCallback([&](const QString& path, const QStringList& paths) {
        openedVideo = path;
        videoPlaylist = paths;
    });
    const QString session = controller.createSession(workspace.path());
    panel.openSession(session);
    QVERIFY(controller.sendPrompt(session, QStringLiteral("Show the video")));
    controller.agent().waitIdle();
    QTRY_COMPARE_WITH_TIMEOUT(panel.findChildren<QPushButton*>(QStringLiteral("agentMediaVideoPlay")).size(), 2, 5000);
    auto* play = panel.findChildren<QPushButton*>(QStringLiteral("agentMediaVideoPlay")).first();
    QCOMPARE(play->size(), QSize(48, 48));
    QVERIFY(play->styleSheet().contains(QStringLiteral("border-radius:24px")));
    auto* cover = panel.findChild<QLabel*>(QStringLiteral("agentMediaVideoCover"));
    QVERIFY(cover != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(!cover->pixmap(Qt::ReturnByValue).isNull(), 5000);
    play->click();
    QCOMPARE(QFileInfo(openedVideo).canonicalFilePath(),
             QFileInfo(workspace.filePath(QStringLiteral("sample.mp4"))).canonicalFilePath());
    QVERIFY(videoPlaylist.contains(openedVideo));
    QCOMPARE(videoPlaylist.size(), 2);
    QTRY_VERIFY_WITH_TIMEOUT(panel.findChild<QPushButton*>(QStringLiteral("agentToolGroupDisclosure")) == nullptr,
                             5000);
    const QString capturePath = qEnvironmentVariable("MAICHAT_AGENT_VIDEO_CAPTURE");
    if (!capturePath.isEmpty()) QVERIFY(panel.grab().save(capturePath));
}

void AgentPanelSessionTest::audioArtifactUsesCompactPlaybackCard() {
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    QVERIFY(QFile::copy(QStringLiteral(MAICHAT_AGENT_AUDIO_FIXTURE), workspace.filePath(QStringLiteral("sample.wav"))));
    AgentController controller(std::make_unique<AudioDeliveringModel>(), QString());
    AgentChatPanel panel(controller);
    panel.resize(700, 500);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    const QString session = controller.createSession(workspace.path());
    panel.openSession(session);
    QVERIFY(controller.sendPrompt(session, QStringLiteral("Show the audio")));
    controller.agent().waitIdle();
    QTRY_VERIFY_WITH_TIMEOUT(panel.findChild<QPushButton*>(QStringLiteral("agentMediaAudioPlay")) != nullptr, 5000);
    auto* play = panel.findChild<QPushButton*>(QStringLiteral("agentMediaAudioPlay"));
    QCOMPARE(play->size(), QSize(240, 58));
    QCOMPARE(play->accessibleName(), QStringLiteral("播放音频"));
    QVERIFY(panel.findChild<QLabel*>(QStringLiteral("agentMediaAudioDuration")) != nullptr);
    const QString capturePath = qEnvironmentVariable("MAICHAT_AGENT_AUDIO_CAPTURE");
    if (!capturePath.isEmpty()) QVERIFY(panel.grab().save(capturePath));
}

void AgentPanelSessionTest::inlineAudioPausesWhenSwitchingSessions() {
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    QVERIFY(writeInlineAudioFixture(workspace.filePath(QStringLiteral("sample.wav"))));
    AgentController controller(std::make_unique<AudioDeliveringModel>(), QString());
    AgentChatPanel panel(controller);
    panel.resize(700, 500);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    const QString first = controller.createSession(workspace.path());
    panel.openSession(first);
    QVERIFY(controller.sendPrompt(first, QStringLiteral("Show the audio")));
    controller.agent().waitIdle();
    QTRY_VERIFY_WITH_TIMEOUT(panel.findChild<QPushButton*>(QStringLiteral("agentMediaAudioPlay")), 5000);
    auto* play = panel.findChild<QPushButton*>(QStringLiteral("agentMediaAudioPlay"));
    QPointer<DesktopAudioPlayback> audio =
        panel.findChild<DesktopAudioPlayback*>(QStringLiteral("agentInlineAudioPlayback"));
    QVERIFY(audio);
    auto* duration = panel.findChild<QLabel*>(QStringLiteral("agentMediaAudioDuration"));
    QVERIFY(duration);
    QTRY_COMPARE_WITH_TIMEOUT(duration->text(), QStringLiteral("0:05"), 3000);
    play->click();
    QTRY_VERIFY_WITH_TIMEOUT(
        audio && audio->state() == DesktopAudioPlayback::State::Playing && audio->positionUs() > 100000, 3000);
    const QString second = controller.createSession(workspace.path());
    panel.openSession(second);
    QVERIFY(audio);
    QTRY_COMPARE_WITH_TIMEOUT(audio->state(), DesktopAudioPlayback::State::Paused, 1500);
    const qint64 pausedAt = audio->positionUs();
    QTest::qWait(150);
    QVERIFY(qAbs(audio->positionUs() - pausedAt) < 60000);
    panel.openSession(first);
    QVERIFY(audio);
    QCOMPARE(audio->state(), DesktopAudioPlayback::State::Paused);
    QCOMPARE(panel.findChild<QPushButton*>(QStringLiteral("agentMediaAudioPlay"))->accessibleName(),
             QStringLiteral("播放音频"));
    play->click();
    QTRY_VERIFY_WITH_TIMEOUT(
        audio->state() == DesktopAudioPlayback::State::Playing && audio->positionUs() > pausedAt + 50000, 2000);
    audio->stop();
}

void AgentPanelSessionTest::quotedMessageIsSentAndShownInTheComposer() {
    AgentController controller(std::make_unique<CapturingModel>(), QString());
    AgentChatPanel panel(controller);
    panel.resize(800, 600);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    const QString session = controller.createSession(QDir::currentPath());
    panel.openSession(session);
    auto* editor = panel.findChild<QTextEdit*>(QStringLiteral("agentPromptEditor"));
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    QVERIFY(editor != nullptr && send != nullptr);
    editor->setPlainText(QStringLiteral("first"));
    send->click();
    controller.agent().waitIdle();
    QTRY_COMPARE(send->toolTip(), QStringLiteral("发送消息"));
    QTRY_VERIFY_WITH_TIMEOUT(!panel.findChildren<QPushButton*>(QStringLiteral("agentQuoteAction")).isEmpty(), 3000);
    auto* quote = panel.findChildren<QPushButton*>(QStringLiteral("agentQuoteAction")).first();
    const QString quotedId = quote->property("messageId").toString();
    QVERIFY(!quotedId.isEmpty());
    quote->click();
    auto* strip = panel.findChild<QWidget*>(QStringLiteral("agentQuoteStrip"));
    QVERIFY(strip != nullptr && strip->isVisible());
    const QString capturePath = qEnvironmentVariable("MAICHAT_AGENT_QUOTE_CAPTURE");
    if (!capturePath.isEmpty()) QVERIFY(panel.grab().save(capturePath));
    editor->setPlainText(QStringLiteral("second"));
    send->click();
    controller.agent().waitIdle();
    QTRY_VERIFY(!strip->isVisible());
    bool saved = false;
    for (const MaiMessage& message : controller.agent().listMessages(session.toStdString()))
        if (message.role == MaiRole::User)
            for (const MaiMessagePart& part : message.parts)
                if (const auto* quoted = std::get_if<MaiQuotePart>(&part.body))
                    saved |= QString::fromStdString(quoted->messageId) == quotedId;
    QVERIFY(saved);
}

void AgentPanelSessionTest::newSessionAvoidsFilesystemRoot() {
    struct RestoreCurrentDirectory {
        QString original = QDir::currentPath();
        ~RestoreCurrentDirectory() {
            QDir::setCurrent(original);
        }
    } restore;
    QVERIFY(QDir::setCurrent(QDir::rootPath()));

    AgentController controller(std::make_unique<SilentModel>(), QString());
    AgentChatPanel panel(controller);
    panel.openSession(QString());
    const auto sessions = controller.agent().listSessions();
    QCOMPARE(sessions.size(), std::size_t(1));
    QVERIFY(QString::fromStdString(sessions.front().directory) != QDir::rootPath());
}

void AgentPanelSessionTest::newAgentOutputDoesNotInterruptReadingHistory() {
    Harness harness;
    QVERIFY(QTest::qWaitForWindowExposed(harness.panel.get()));
    auto* view = harness.view();
    auto publish = [&](const char* partId, const QString& text) {
        MaiEvent event;
        event.id = "evt_scroll_test";
        event.type = MaiEventType::MessagePartDelta;
        event.sessionId = harness.mine.toUtf8().constData();
        event.messageId = "msg_scroll_test";
        event.partId = partId;
        event.field = "text";
        event.delta = text.toUtf8().constData();
        harness.controller->agent().eventBus().publish(event);
        QTest::qWait(120);
    };
    publish("part_history", QStringLiteral("history line\n").repeated(250));
    auto* bar = view->verticalScrollBar();
    QVERIFY(bar->maximum() > 0);
    bar->setValue(qMax(0, bar->maximum() / 3));
    QVERIFY(!view->isAtBottom());
    const int position = bar->value();

    publish("part_new", QStringLiteral("A new answer arrived while reading history"));
    QCOMPARE(bar->value(), position);
    publish("part_new", QStringLiteral(" and it kept streaming"));
    QCOMPARE(bar->value(), position);
}

void AgentPanelSessionTest::switchingSessionsShowsLatestMessage() {
    Harness harness;
    QVERIFY(QTest::qWaitForWindowExposed(harness.panel.get()));
    QVERIFY(harness.controller->sendPrompt(harness.mine, QStringLiteral("历史消息\n").repeated(250)));
    harness.controller->agent().waitIdle();
    harness.panel->openSession(harness.mine);
    auto* view = harness.view();
    QVERIFY(view != nullptr);
    auto* bar = view->verticalScrollBar();
    QTRY_VERIFY(bar->maximum() > 0);
    bar->setValue(0);
    QVERIFY(!view->isAtBottom());

    harness.panel->openSession(harness.other);
    harness.panel->openSession(harness.mine);
    QTRY_VERIFY(view->isAtBottom());
}

// 同一消息中的媒体提及按数量决定是否生成气泡；跨消息不合并数量，避免正文里的文件名变成错误附件。
void AgentPanelSessionTest::inlineMediaBubblesRespectMessageBoundary() {
    class ListingModel final : public MaiModelClient {
    public:
        MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink, const std::atomic<bool>&) override {
            std::string prompt;
            for (auto it = request.messages.rbegin(); it != request.messages.rend(); ++it)
                if (it->role == MaiModelRole::User) {
                    prompt = it->content;
                    break;
                }
            const char* reply = prompt == "first"    ? "clip.mp4 voice.wav"
                                : prompt == "second" ? "third.mp4 fourth.mp4 fifth.mp4"
                                                     : "clip.mp4 voice.wav third.mp4 fourth.mp4 fifth.mp4";
            if (sink.onText) sink.onText(reply);
            return {};
        }
        MaiWireApi wireApi() const override {
            return MaiWireApi::ChatCompletions;
        }
    };

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    for (const QString& name : {QStringLiteral("clip.mp4"), QStringLiteral("voice.wav"), QStringLiteral("third.mp4"),
                                QStringLiteral("fourth.mp4"), QStringLiteral("fifth.mp4")}) {
        QFile file(directory.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("test", 4) == 4);
    }
    AgentController controller(std::make_unique<ListingModel>(), QString());
    AgentChatPanel panel(controller);
    panel.resize(700, 500);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    QString openedVideo;
    QStringList videoPlaylist;
    panel.setOpenVideoCallback([&](const QString& path, const QStringList& paths) {
        openedVideo = path;
        videoPlaylist = paths;
    });
    const QString session = controller.createSession(directory.path());
    panel.openSession(session);
    QVERIFY(controller.sendPrompt(session, QStringLiteral("first")));
    controller.agent().waitIdle();
    QTRY_COMPARE(panel.findChildren<QPushButton*>(QStringLiteral("agentTextMediaBubble")).size(), 2);
    for (QPushButton* bubble : panel.findChildren<QPushButton*>(QStringLiteral("agentTextMediaBubble"))) {
        if (bubble->text().contains(QStringLiteral("clip.mp4"))) QTest::mouseClick(bubble, Qt::LeftButton);
    }
    // 回调返回解析过符号链接的路径；/var 与 /private/var 仍必须指向同一个已创建文件。
    QCOMPARE(QFileInfo(openedVideo).canonicalFilePath(),
             QFileInfo(directory.filePath(QStringLiteral("clip.mp4"))).canonicalFilePath());
    QVERIFY(videoPlaylist.contains(openedVideo));
    QVERIFY(controller.sendPrompt(session, QStringLiteral("second")));
    controller.agent().waitIdle();
    QTRY_COMPARE(panel.findChildren<QPushButton*>(QStringLiteral("agentTextMediaBubble")).size(), 5);

    const QString listing = controller.createSession(directory.path());
    panel.openSession(listing);
    QVERIFY(controller.sendPrompt(listing, QStringLiteral("listing")));
    controller.agent().waitIdle();
    // Retained pages keep their media controls alive. Only controls belonging
    // to the selected page may be visible over this five-file text listing.
    const auto noVisibleMediaBubble = [&panel] {
        for (QPushButton* bubble : panel.findChildren<QPushButton*>(QStringLiteral("agentTextMediaBubble")))
            if (bubble->isVisible()) return false;
        return true;
    };
    QTRY_VERIFY(noVisibleMediaBubble());
}

void AgentPanelSessionTest::answerFromAnotherSessionDoesNotLeakIn() {
    Harness harness;
    QVERIFY(QTest::qWaitForWindowExposed(harness.panel.get()));
    MarkdownView* view = harness.view();
    QVERIFY(view != nullptr);
    const int before = view->itemCount();

    harness.publishFromOther(MaiEventType::MessagePartDelta, QStringLiteral("这是子 Agent 说的话"));

    // 一条都不该多出来。多出来的话用户会以为 AI 自己说了些没问过的东西。
    QCOMPARE(view->itemCount(), before);
    view->selectAll();
    QVERIFY(!view->selectedText().contains(QStringLiteral("子 Agent 说的话")));
}

void AgentPanelSessionTest::actionPanelMatchesMobileModelAndPolicyFlow() {
    Harness harness;
    QVERIFY(harness.controller->setModel(harness.mine, QStringLiteral("deepseek-flash")));
    harness.panel->setModelLabel(QStringLiteral("deepseek-flash"));
    harness.panel->setReasoningEffortLabel(QStringLiteral("high"));
    QVERIFY(QTest::qWaitForWindowExposed(harness.panel.get()));
    QPushButton* chip = harness.panel->findChild<QPushButton*>(QStringLiteral("agentModelChip"));
    QPushButton* policy = harness.panel->findChild<QPushButton*>(QStringLiteral("agentApprovalPolicy"));
    QPushButton* more = harness.panel->findChild<QPushButton*>(QStringLiteral("agentMoreActions"));
    auto* title = harness.panel->findChild<QLabel*>(QStringLiteral("agentHeaderTitle"));
    QVERIFY(chip != nullptr);
    QVERIFY(policy && more && title);
    QVERIFY(!chip->isVisible());
    QVERIFY(!policy->isVisible());
    QCOMPARE(title->text(), QStringLiteral("创作中心"));
    QTest::mouseClick(more, Qt::LeftButton);
    auto* actions = harness.panel->findChild<QWidget*>(QStringLiteral("agentActionPanel"));
    QVERIFY(actions && actions->isVisible());
    auto* modelSection = actions->findChild<QPushButton*>(QStringLiteral("agentEffortSection"));
    QVERIFY(modelSection);
    QLabel* selectedValue = nullptr;
    for (QLabel* label : modelSection->findChildren<QLabel*>()) {
        if (label->text() == QStringLiteral("标准")) selectedValue = label;
    }
    QVERIFY(selectedValue);
    const QPoint valueCenter = selectedValue->mapTo(actions, selectedValue->rect().center());
    const QString hoverCapture = qEnvironmentVariable("MAICHAT_AGENT_UI_HOVER_CAPTURE");
    if (!hoverCapture.isEmpty()) QVERIFY(actions->grab().save(hoverCapture));
    // The remote macOS runner cannot put its system cursor inside the popup.
    // Check the composited row instead of sampling the transparent child button.
    const QImage actionImage = actions->grab().toImage();
    QVERIFY(actionImage.rect().contains(valueCenter));
    QCOMPARE(actionImage.pixelColor(valueCenter).alpha(), 255);
    QTest::mouseClick(modelSection, Qt::LeftButton);
    const QString capturePath = qEnvironmentVariable("MAICHAT_AGENT_UI_CAPTURE");
    if (!capturePath.isEmpty()) {
        QVERIFY(harness.panel->grab().save(capturePath));
        QVERIFY(actions->grab().save(capturePath + QStringLiteral(".popup.png")));
    }
    auto* option = actions->findChild<QPushButton*>(QStringLiteral("agentEffortOption-max"));
    QVERIFY(option && option->isVisible());
    auto* selectedOption = actions->findChild<QPushButton*>(QStringLiteral("agentEffortOption-high"));
    QVERIFY(selectedOption && selectedOption->isChecked());
    QSignalSpy selected(harness.panel.get(), &AgentChatPanel::reasoningEffortChangeRequested);
    QTest::mouseClick(option, Qt::LeftButton);
    QCOMPARE(selected.count(), 1);
    QCOMPARE(selected.first().first().toString(), QStringLiteral("max"));
    MaiSession session;
    QVERIFY(harness.controller->agent().getSession(harness.mine.toStdString(), session));
    QCOMPARE(QString::fromStdString(session.model), QStringLiteral("deepseek-flash"));
    QVERIFY(harness.panel->applySelectedModel(QStringLiteral("glm-5.3-flash")));
    QVERIFY(harness.controller->agent().getSession(harness.mine.toStdString(), session));
    QCOMPARE(QString::fromStdString(session.model), QStringLiteral("glm-5.3-flash"));
}

void AgentPanelSessionTest::mainModelChoicesStaySeparateFromReasoningEffort() {
    Harness harness;
    QVERIFY(harness.controller->setModel(harness.mine, QStringLiteral("deepseek-flash")));
    harness.panel->setModelLabel(QStringLiteral("deepseek-flash"));
    harness.panel->setReasoningEffortLabel(QStringLiteral("high"));
    connect(
        harness.panel.get(), &AgentChatPanel::modelChangeRequested, harness.panel.get(),
        [&harness](const QString& model) { QVERIFY(harness.panel->applySelectedModel(model)); });
    QVERIFY(QTest::qWaitForWindowExposed(harness.panel.get()));
    auto* more = harness.panel->findChild<QPushButton*>(QStringLiteral("agentMoreActions"));
    QVERIFY(more);
    QTest::mouseClick(more, Qt::LeftButton);
    auto* actions = harness.panel->findChild<QWidget*>(QStringLiteral("agentActionPanel"));
    QVERIFY(actions && actions->isVisible());
    auto* models = actions->findChild<QPushButton*>(QStringLiteral("agentModelSection"));
    auto* effort = actions->findChild<QPushButton*>(QStringLiteral("agentEffortSection"));
    QVERIFY2(models, "The main-model row must be available alongside reasoning effort.");
    QVERIFY(effort);
    QTest::mouseClick(effort, Qt::LeftButton);
    auto* high = actions->findChild<QPushButton*>(QStringLiteral("agentEffortOption-high"));
    QVERIFY(high && high->isVisible() && high->isChecked());
    QTest::mouseClick(models, Qt::LeftButton);
    QVERIFY(!high->isVisible());
    const QStringList ids{QStringLiteral("glm-5.3"), QStringLiteral("glm-5.3-flash"),
                          QStringLiteral("deepseek-flash"), QStringLiteral("deepseek-v4-pro")};
    for (const QString& id : ids) {
        auto* option = actions->findChild<QPushButton*>(QStringLiteral("agentModelOption-") + id);
        QVERIFY2(option && option->isVisible(), qPrintable(id));
        QCOMPARE(option->isChecked(), id == QStringLiteral("deepseek-flash"));
    }
    const QString capture = qEnvironmentVariable("MAICHAT_SESSION_MODEL_CAPTURE");
    if (!capture.isEmpty()) {
        QCoreApplication::processEvents();
        QVERIFY(actions->grab().save(capture));
    }
    QSignalSpy modelChanged(harness.panel.get(), &AgentChatPanel::modelChangeRequested);
    QSignalSpy effortChanged(harness.panel.get(), &AgentChatPanel::reasoningEffortChangeRequested);
    auto* pro =
        actions->findChild<QPushButton*>(QStringLiteral("agentModelOption-deepseek-v4-pro"));
    QTest::mouseClick(pro, Qt::LeftButton);
    QCOMPARE(modelChanged.count(), 1);
    QCOMPARE(modelChanged.first().first().toString(), QStringLiteral("deepseek-v4-pro"));
    QCOMPARE(effortChanged.count(), 0);
    QVERIFY(!actions->isVisible());
    MaiSession session;
    QVERIFY(harness.controller->agent().getSession(harness.mine.toStdString(), session));
    QCOMPARE(QString::fromStdString(session.model), QStringLiteral("deepseek-v4-pro"));
    auto* value = actions->findChild<QLabel*>(QStringLiteral("agentModelValue"));
    QVERIFY(value);
    QCOMPARE(value->text(), QStringLiteral("deepseek-v4-pro"));
}

void AgentPanelSessionTest::mainModelValueFollowsTheSelectedSession() {
    Harness harness;
    QVERIFY(harness.controller->setModel(harness.mine, QStringLiteral("glm-5.3-flash")));
    QVERIFY(harness.controller->setModel(harness.other, QStringLiteral("deepseek-v4-pro")));
    harness.panel->openSession(harness.mine);
    auto* value = harness.panel->findChild<QLabel*>(QStringLiteral("agentModelValue"));
    QVERIFY2(value, "The main-model value must be projected from the selected session.");
    QCOMPARE(value->text(), QStringLiteral("glm-5.3-flash"));
    harness.panel->openSession(harness.other);
    QCOMPARE(value->text(), QStringLiteral("deepseek-v4-pro"));
    harness.panel->openSession(harness.mine);
    QCOMPARE(value->text(), QStringLiteral("glm-5.3-flash"));

    // An explicit future-session default must not copy the just-selected page.
    QVERIFY(harness.controller->setDefaultModel(QStringLiteral("deepseek-flash")));
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    harness.panel->openNewSessionInDirectory(workspace.path());
    QVERIFY(harness.panel->sessionId() != harness.mine &&
            harness.panel->sessionId() != harness.other);
    QCOMPARE(value->text(), QStringLiteral("deepseek-flash"));
    MaiSession first, other;
    QVERIFY(harness.controller->agent().getSession(harness.mine.toStdString(), first));
    QVERIFY(harness.controller->agent().getSession(harness.other.toStdString(), other));
    QCOMPARE(QString::fromStdString(first.model), QStringLiteral("glm-5.3-flash"));
    QCOMPARE(QString::fromStdString(other.model), QStringLiteral("deepseek-v4-pro"));
}

void AgentPanelSessionTest::firstModelSelectionPreservesTheBlankPageDraft() {
    AgentController controller(std::make_unique<SilentModel>(), QString());
    AgentChatPanel panel(controller);
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    panel.setDefaultWorkspaceDirectory(workspace.path());
    auto* editor = panel.findChild<QTextEdit*>(QStringLiteral("agentPromptEditor"));
    QVERIFY(editor);
    QVERIFY(panel.sessionId().isEmpty());
    const QString draft = QStringLiteral("Unsent first-page draft before selecting a model");
    editor->setPlainText(draft);
    QVERIFY(panel.applySelectedModel(QStringLiteral("glm-5.3-flash")));
    QVERIFY(!panel.sessionId().isEmpty());
    QCOMPARE(editor->toPlainText(), draft);
    MaiSession session;
    QVERIFY(controller.agent().getSession(panel.sessionId().toStdString(), session));
    QCOMPARE(QString::fromStdString(session.model), QStringLiteral("glm-5.3-flash"));
}

void AgentPanelSessionTest::anotherSessionFailingDoesNotShowAnError() {
    // 子 Agent 跑挂了是**它自己的事**：父那边会从 wait_agent 拿到结果，
    // 该怎么应对由模型决定。直接在父的界面上弹一条红字的话，用户会以为
    // 自己刚才那句话出错了。
    Harness harness;
    QVERIFY(QTest::qWaitForWindowExposed(harness.panel.get()));
    MarkdownView* view = harness.view();
    QVERIFY(view != nullptr);
    const int before = view->itemCount();

    harness.publishFromOther(MaiEventType::SessionError, QStringLiteral("子 Agent 炸了"));

    QCOMPARE(view->itemCount(), before);
    view->selectAll();
    QVERIFY(!view->selectedText().contains(QStringLiteral("炸了")));
}

void AgentPanelSessionTest::anotherSessionTitleDoesNotRenameTheHeader() {
    Harness harness;
    QVERIFY(QTest::qWaitForWindowExposed(harness.panel.get()));

    QLabel* title = nullptr;
    for (QLabel* label : harness.panel->findChildren<QLabel*>()) {
        if (label->text() == QStringLiteral("创作中心")) title = label;
    }
    QVERIFY(title != nullptr);

    harness.publishFromOther(MaiEventType::SessionUpdated, QStringLiteral("子任务的名字"));
    QCOMPARE(title->text(), QStringLiteral("创作中心"));

    // A 发起改名后用户可能已经切到 B；持久化标题的事件仍须刷新整个侧栏，
    // 同时不能切走当前选择、替换当前页头部或插入另一会话的消息。
    AgentSessionList sessions(*harness.controller);
    connect(harness.panel.get(), &AgentChatPanel::sessionListChanged, &sessions, [&] {
        sessions.refresh();
        sessions.setCurrent(harness.panel->sessionId());
    });
    sessions.setCurrent(harness.mine);
    auto* list = sessions.findChild<QListWidget*>();
    QVERIFY(list != nullptr);
    const int messageItems = harness.view()->itemCount();
    const QString renamed = QStringLiteral("已改名的后台对话");
    const auto result = harness.controller->agent().submit(
        MaiUpdateSession{harness.other.toStdString(), renamed.toStdString(), "", "", ""});
    QVERIFY(result);
    QTRY_COMPARE(list->findItems(renamed, Qt::MatchExactly).size(), 1);
    QVERIFY(list->currentItem() != nullptr);
    QCOMPARE(list->currentItem()->data(Qt::UserRole).toString(), harness.mine);
    QCOMPARE(harness.panel->sessionId(), harness.mine);
    QCOMPARE(title->text(), QStringLiteral("创作中心"));
    QCOMPARE(harness.view()->itemCount(), messageItems);
}

void AgentPanelSessionTest::currentSessionTitleStaysInTheSessionList() {
    Harness harness;
    const QString titled =
        harness.controller->createSession(QDir::currentPath(), QStringLiteral("A named conversation"));
    QVERIFY(!titled.isEmpty());
    harness.panel->openSession(titled);
    auto* title = harness.panel->findChild<QLabel*>(QStringLiteral("agentHeaderTitle"));
    QVERIFY(title);
    QCOMPARE(title->text(), QStringLiteral("创作中心"));
}

void AgentPanelSessionTest::subAgentCardsKeepTaskAndStreamedLatestLineVisible() {
    Harness harness;
    QVERIFY(QTest::qWaitForWindowExposed(harness.panel.get()));
    QVERIFY(harness.controller->sendPrompt(harness.mine, QStringLiteral("Review fixture")));
    harness.controller->agent().waitIdle();
    harness.panel->openSession(harness.mine);
    const QString task = QStringLiteral("logic-func-reviewer");
    const auto spawned = harness.controller->agent().spawnSubAgent(harness.mine.toStdString(), task.toStdString(),
                                                                   "Read-only fixture task");
    QVERIFY(spawned);
    harness.controller->agent().waitIdle();
    const auto publish = [&](const QString& delta, const char* partId = "prt_sub_card_layout") {
        MaiEvent event;
        event.id = "evt_sub_card_layout";
        event.type = MaiEventType::MessagePartDelta;
        event.sessionId = spawned.value();
        event.messageId = "msg_sub_card_layout";
        event.partId = partId;
        event.field = "text";
        event.delta = delta.toStdString();
        harness.controller->agent().eventBus().publish(event);
        QCoreApplication::processEvents();
    };
    publish(QStringLiteral("代码审查已完成，发现两个需要核实的问题"));
    publish(QStringLiteral("。"));
    auto* card = harness.panel->findChild<QFrame*>(QStringLiteral("agentSubTaskCard"));
    QVERIFY(card != nullptr);
    QLabel* name = nullptr;
    QLabel* latest = nullptr;
    for (auto* label : card->findChildren<QLabel*>()) {
        if (label->text().startsWith(QStringLiteral("子任务"))) name = label;
        if (label->text().startsWith(QStringLiteral("代码审查"))) latest = label;
    }
    QVERIFY(name != nullptr);
    QTRY_VERIFY(name->width() >= name->fontMetrics().horizontalAdvance(name->text()));
    QVERIFY(latest != nullptr);
    QCOMPARE(latest->text(), QStringLiteral("代码审查已完成，发现两个需要核实的问题。"));
    QTRY_VERIFY(latest->width() >= latest->fontMetrics().horizontalAdvance(latest->text()));
    QVERIFY(card->rect().contains(name->geometry()));
    QVERIFY(card->rect().contains(latest->geometry()));
    QCOMPARE(harness.panel->sessionId(), harness.mine);
    const QString capture = qEnvironmentVariable("MAICHAT_SUB_AGENT_CARD_CAPTURE");
    if (!capture.isEmpty()) QVERIFY(harness.panel->grab().save(capture));
    // 工具执行后的新正文片段、后续轮次都重新开始摘要，不能拼接上一片段的尾巴。
    publish(QStringLiteral("新的检查开始"), "prt_sub_card_next");
    publish(QStringLiteral("。"), "prt_sub_card_next");
    QTRY_COMPARE(latest->text(), QStringLiteral("新的检查开始。"));
}

void AgentPanelSessionTest::subAgentCardsUseConsistentWidthAndExpandableSummary() {
    struct RestoreFont {
        QFont previous = QApplication::font();
        ~RestoreFont() {
            QApplication::setFont(previous);
        }
    } restoreFont;
    QFont font;
    font.setFamilies({QStringLiteral("Segoe UI"), QStringLiteral("Microsoft YaHei UI")});
    font.setPixelSize(13);
    QApplication::setFont(font);
    Harness harness;
    harness.panel->resize(1050, 760);
    QVERIFY(QTest::qWaitForWindowExposed(harness.panel.get()));
    const QStringList tasks{QStringLiteral("code-spec-reviewer"), QStringLiteral("code-safety-reviewer"),
                            QStringLiteral("logic-func-reviewer")};
    QStringList childIds;
    for (int index = 0; index < tasks.size(); ++index) {
        const auto child = harness.controller->agent().spawnSubAgent(
            harness.mine.toStdString(), tasks[index].toStdString(), "Read-only local fixture");
        QVERIFY(child);
        childIds << QString::fromStdString(child.value());
        harness.controller->agent().waitIdle();
        MaiEvent delta;
        delta.id = "evt_child_style_" + std::to_string(index);
        delta.type = MaiEventType::MessagePartDelta;
        delta.sessionId = child.value();
        delta.messageId = "msg_child_style_" + std::to_string(index);
        delta.partId = "prt_child_style_" + std::to_string(index);
        delta.field = "text";
        const QString text =
            index == 0 ? QStringLiteral("规范检查完成。")
                       : QStringLiteral("审查已完成，以下是最近的检查进展。\n") +
                             QStringLiteral("已核对任务归属、后台状态和长内容展示，没有访问外部服务。\n").repeated(7) +
                             QStringLiteral("最终摘要：当前检查项已完成，可展开查看保留的进展内容。");
        delta.delta = text.toStdString();
        harness.controller->agent().eventBus().publish(delta);
        QCoreApplication::processEvents();
    }
    const auto cards = harness.panel->findChildren<QFrame*>(QStringLiteral("agentSubTaskCard"));
    QCOMPARE(cards.size(), 3);
    QTRY_COMPARE(cards[0]->width(), cards[1]->width());
    QTRY_COMPARE(cards[1]->width(), cards[2]->width());
    QVERIFY(cards[0]->width() >= harness.view()->viewport()->width() - 2 * UiZoom::s(48) - 2);
    auto* summary = cards[1]->findChild<QLabel*>(QStringLiteral("agentSubTaskSummary"));
    auto* disclosure = cards[1]->findChild<QPushButton*>(QStringLiteral("agentSubTaskDisclosure"));
    auto* title = cards[1]->findChild<QLabel*>(QStringLiteral("agentSubTaskTitle"));
    auto* state = cards[1]->findChild<QLabel*>(QStringLiteral("agentSubTaskState"));
    QVERIFY(summary && disclosure && title && state);
    auto* welcome = harness.panel->findChild<QWidget*>(QStringLiteral("agentWelcomeView"));
    QVERIFY(welcome);
    QTRY_VERIFY(!welcome->isVisible());
    QTRY_VERIFY(disclosure->isVisible());
    QVERIFY(title->geometry().right() < state->geometry().left());
    const int collapsedHeight = cards[1]->height();
    QVERIFY(summary->height() <= summary->fontMetrics().lineSpacing() * 3);
    QVERIFY(captureAgentFixture(*harness.panel, QStringLiteral("sub-task-cards-collapsed")));
    QTest::mouseClick(disclosure, Qt::LeftButton);
    QTRY_VERIFY(cards[1]->height() > collapsedHeight);
    QVERIFY(summary->text().contains(QStringLiteral("审查已完成")));
    QVERIFY(summary->text().contains(QStringLiteral("最终摘要")));
    QCOMPARE(summary->textFormat(), Qt::PlainText);
    QVERIFY(summary->textInteractionFlags() & Qt::TextSelectableByMouse);
    QVERIFY(captureAgentFixture(*harness.panel, QStringLiteral("sub-task-card-expanded")));
    harness.panel->resize(650, 760);
    // Offscreen controls intentionally keep their old physical geometry until
    // visible again. The view's content frames are authoritative for all rows.
    const auto frame = [&](int index) { return harness.view()->itemRect(QStringLiteral("sub-") + childIds[index]); };
    QTRY_COMPARE(frame(0).width(), frame(1).width());
    QTRY_VERIFY(summary->width() < 600);
    if (summary->hasHeightForWidth()) QTRY_VERIFY(summary->height() >= summary->heightForWidth(summary->width()));
    QTRY_VERIFY(frame(0).bottom() < frame(1).top());
    QTRY_VERIFY(frame(1).bottom() < frame(2).top());
    QVERIFY(captureAgentFixture(*harness.panel, QStringLiteral("sub-task-card-expanded-narrow")));
    harness.panel->resize(1050, 760);
    QTRY_VERIFY(summary->width() > 600);
    QTest::mouseClick(disclosure, Qt::LeftButton);
    QTRY_COMPARE(cards[1]->height(), collapsedHeight);
}

// Child event delivery can lag behind the parent stream. Persisted spawn receipts,
// rather than the arrival time of child output, own the cards' transcript position.
void AgentPanelSessionTest::subAgentCardsStayBesideTheirSpawnPartsAcrossReplay_data() {
    QTest::addColumn<bool>("receiptInitiallyReady");
    QTest::newRow("receipt-before-child") << true;
    QTest::newRow("child-output-and-question-before-receipt") << false;
}

void AgentPanelSessionTest::subAgentCardsStayBesideTheirSpawnPartsAcrossReplay() {
    QFETCH(bool, receiptInitiallyReady);
    const QString evidenceSuffix = receiptInitiallyReady ? QString() : QStringLiteral("-late-receipt");
    QTemporaryDir storage;
    QVERIFY(storage.isValid());
    const QString database = storage.filePath(QStringLiteral("sub-agent-timeline.db"));
    const QString root = QStringLiteral("ses_timeline_root");
    const QStringList children{QStringLiteral("ses_timeline_child_a"), QStringLiteral("ses_timeline_child_b"),
                               QStringLiteral("ses_timeline_child_c"), QStringLiteral("ses_timeline_child_d")};
    const QStringList tasks{QStringLiteral("first-code-review"), QStringLiteral("first-spec-review"),
                            QStringLiteral("second-code-review"), QStringLiteral("second-spec-review")};
    const QStringList before{QStringLiteral("prt_timeline_001_before"), QStringLiteral("prt_timeline_011_before")};
    const QStringList spawn{QStringLiteral("prt_timeline_002_spawn"), QStringLiteral("prt_timeline_003_spawn"),
                            QStringLiteral("prt_timeline_012_spawn"), QStringLiteral("prt_timeline_013_spawn")};
    const QStringList after{QStringLiteral("prt_timeline_004_after"), QStringLiteral("prt_timeline_014_after")};
    {
        auto opened = makeMaiSqliteStore(database.toStdString());
        QVERIFY(opened);
        auto store = std::move(opened.value());
        MaiSession session;
        session.id = root.toStdString();
        session.directory = storage.path().toStdString();
        session.created = session.updated = 1;
        store->putSession(session);
        for (int batch = 0; batch < 2; ++batch) {
            MaiMessage message;
            message.id = "msg_timeline_" + std::to_string(batch + 1);
            message.role = MaiRole::Assistant;
            message.created = 100 * (batch + 1);
            // A receipt still in flight belongs to an unfinished parent turn;
            // changing an already terminal message would intentionally replace
            // its retained page and would test history edits instead of delivery.
            message.completed = receiptInitiallyReady ? message.created + 10 : 0;
            message.parts.push_back({before[batch].toStdString(),
                                     MaiTextPart{batch == 0 ? "阶段一：派发第一批子任务" : "阶段二：派发第二批子任务"},
                                     message.created});
            for (int index = 2 * batch; index < 2 * batch + 2; ++index) {
                MaiToolPart tool;
                tool.tool = "spawn_agent";
                tool.callId = "call_" + spawn[index].toStdString();
                tool.input = "{\"task_name\":\"" + tasks[index].toStdString() +
                             "\",\"prompt\":\"Local timeline fixture\"}";
                if (receiptInitiallyReady) tool.output = "Started sub-agent " + children[index].toStdString() + " for \"" +
                              tasks[index].toStdString() +
                              "\". Use wait_agent with that id when you need its answer.";
                tool.state = receiptInitiallyReady ? MaiToolState::Completed : MaiToolState::Running;
                message.parts.push_back({spawn[index].toStdString(), std::move(tool), message.created + index + 1});
            }
            message.parts.push_back({after[batch].toStdString(),
                                     MaiTextPart{batch == 0 ? "阶段一：整理第一批结果" : "阶段二：整理第二批结果"},
                                     message.created + 9});
            store->putMessage(session.id, message);
        }
        QVERIFY(!store->lastWriteError().hasError());
    }
    QStringList expectedOrder;
    for (int batch = 0; batch < 2; ++batch)
        expectedOrder << before[batch] << QStringLiteral("tool-group-") + spawn[2 * batch]
                      << QStringLiteral("sub-") + children[2 * batch]
                      << QStringLiteral("sub-") + children[2 * batch + 1] << after[batch];
    const auto orderFailure = [&expectedOrder](MarkdownView* view) {
        QString previous;
        for (const QString& id : expectedOrder) {
            if (!view->contains(id) || view->itemRect(id).height() <= 0)
                return QStringLiteral("Missing timeline item: %1").arg(id);
            if (!previous.isEmpty() && view->itemRect(previous).bottom() > view->itemRect(id).top())
                return QStringLiteral("%1 at %2 must precede %3 at %4")
                    .arg(previous).arg(view->itemRect(previous).bottom()).arg(id).arg(view->itemRect(id).top());
            previous = id;
        }
        return QString();
    };
    {
        AgentController controller(std::make_unique<SilentModel>(), database);
        QVERIFY2(controller.openError().isEmpty(), qPrintable(controller.openError()));
        if (!receiptInitiallyReady) {
            // Startup correctly recovers tools left Running by a previous
            // process. This case represents a current in-flight parent instead,
            // so publish its active checkpoint after the controller is alive.
            auto opened = makeMaiSqliteStore(database.toStdString());
            QVERIFY(opened);
            auto store = std::move(opened.value());
            for (int batch = 0; batch < 2; ++batch) {
                MaiMessage message;
                QVERIFY(store->getMessage(root.toStdString(), "msg_timeline_" + std::to_string(batch + 1), message));
                message.completed = 0;
                for (auto& part : message.parts)
                    if (auto* tool = std::get_if<MaiToolPart>(&part.body)) {
                        tool->state = MaiToolState::Running;
                        tool->output.clear();
                        tool->error.clear();
                    }
                store->putMessage(root.toStdString(), message);
            }
            QVERIFY(!store->lastWriteError().hasError());
        }
        AgentChatPanel panel(controller);
        panel.resize(1100, 1000);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        panel.openSession(root);
        auto* view = panel.findChild<MarkdownView*>();
        QVERIFY(view);
        QVERIFY(view->contains(after.last()));
        // Metadata and first output arrive after both parent batches were rendered.
        // This is an external store writer, not a paid model or an in-memory child mock.
        {
            auto opened = makeMaiSqliteStore(database.toStdString());
            QVERIFY(opened);
            auto store = std::move(opened.value());
            for (int index = 0; index < children.size(); ++index) {
                MaiSession child;
                child.id = children[index].toStdString();
                child.title = tasks[index].toStdString();
                child.directory = storage.path().toStdString();
                child.parentId = root.toStdString();
                child.depth = 1;
                child.created = child.updated = 1000 + index;
                store->putSession(child);
                MaiMessage report;
                report.id = "msg_" + child.id;
                report.role = MaiRole::Assistant;
                report.created = report.completed = 2000 + index;
                report.parts.push_back({"prt_" + child.id, MaiTextPart{"本地子任务检查结果"}, report.created});
                if (!receiptInitiallyReady && index == 0) {
                    MaiToolPart question;
                    question.tool = "question";
                    question.input = R"({"question":"Which local fixture should I inspect?"})";
                    question.output = "The current fixture";
                    question.state = MaiToolState::Completed;
                    report.parts.push_back({"prt_timeline_002x_question", std::move(question), 103});
                }
                store->putMessage(child.id, report);
            }
            QVERIFY(!store->lastWriteError().hasError());
        }
        QCOMPARE(controller.subAgents(root).size(), std::size_t(4));
        const auto publish = [&](int index, const std::string& text) {
            MaiEvent event;
            event.id = "evt_timeline_" + std::to_string(index);
            event.type = MaiEventType::MessagePartDelta;
            event.sessionId = children[index].toStdString();
            event.messageId = "msg_" + event.sessionId;
            event.partId = "prt_" + event.sessionId;
            event.field = "text";
            event.delta = text;
            controller.agent().eventBus().publish(event);
        };
        // Reverse arrival order must not reverse either batch or move batch one
        // below the later parent message and its independent batch two.
        for (int index = children.size() - 1; index >= 0; --index) publish(index, "本地子任务检查结果");
        QTRY_VERIFY_WITH_TIMEOUT(view->contains(QStringLiteral("sub-") + children.front()), 3000);
        if (!receiptInitiallyReady) {
            MaiEvent question;
            question.type = MaiEventType::MessagePartUpdated;
            question.sessionId = children.front().toStdString();
            question.messageId = "msg_" + question.sessionId;
            question.partId = "prt_timeline_002x_question";
            controller.agent().eventBus().publish(question);
            QTRY_VERIFY(view->contains(QStringLiteral("prt_timeline_002x_question-question")));
            auto opened = makeMaiSqliteStore(database.toStdString());
            QVERIFY(opened);
            auto store = std::move(opened.value());
            for (int batch = 0; batch < 2; ++batch) {
                MaiMessage message;
                QVERIFY(store->getMessage(root.toStdString(), "msg_timeline_" + std::to_string(batch + 1), message));
                for (int index = 2 * batch; index < 2 * batch + 2; ++index) {
                    auto* tool = std::get_if<MaiToolPart>(&message.parts[1 + index % 2].body);
                    QVERIFY(tool);
                    tool->output = "Started sub-agent " + children[index].toStdString() + " for \"" +
                                   tasks[index].toStdString() +
                                   "\". Use wait_agent with that id when you need its answer.";
                    tool->state = MaiToolState::Completed;
                }
                store->putMessage(root.toStdString(), message);
                for (int index = 2 * batch; index < 2 * batch + 2; ++index) {
                    MaiEvent updated;
                    updated.type = MaiEventType::MessagePartUpdated;
                    updated.sessionId = root.toStdString();
                    updated.messageId = message.id;
                    updated.partId = spawn[index].toStdString();
                    controller.agent().eventBus().publish(updated);
                }
            }
            QVERIFY(!store->lastWriteError().hasError());
        }
        QTRY_VERIFY2_WITH_TIMEOUT(orderFailure(view).isEmpty(), qPrintable(orderFailure(view)), 3000);
        if (!receiptInitiallyReady) {
            const QString question = QStringLiteral("prt_timeline_002x_question-question");
            QTRY_VERIFY(view->itemRect(QStringLiteral("sub-") + children.front()).bottom() <= view->itemRect(question).top());
            QTRY_VERIFY(view->itemRect(question).bottom() <= view->itemRect(QStringLiteral("tool-group-") + spawn[1]).top());
        }
        QVERIFY(captureAgentFixture(panel, QStringLiteral("sub-agent-timeline-live") + evidenceSuffix));
        const auto originalCards = panel.findChildren<QFrame*>(QStringLiteral("agentSubTaskCard"));
        QCOMPARE(originalCards.size(), 4);
        const QPointer<QFrame> retained = originalCards.front();
        const QString other = controller.createSession(storage.path(), QStringLiteral("Timeline warm page"));
        panel.openSession(other);
        publish(0, " late status while cached");
        QCoreApplication::processEvents();
        panel.openSession(root);
        QTRY_VERIFY2_WITH_TIMEOUT(orderFailure(view).isEmpty(), qPrintable(orderFailure(view)), 3000);
        QVERIFY(retained);
        QTRY_VERIFY(retained && retained->isVisible());
        publish(0, " final status");
        QTRY_VERIFY2_WITH_TIMEOUT(orderFailure(view).isEmpty(), qPrintable(orderFailure(view)), 3000);
        QCOMPARE(panel.findChildren<QFrame*>(QStringLiteral("agentSubTaskCard")).size(), 4);
        QVERIFY(captureAgentFixture(panel, QStringLiteral("sub-agent-timeline-warm") + evidenceSuffix));
    }
    // New controller and panel reopen the actual SQLite file, with no retained
    // page, live child event cache, or active sub-agent process to preserve order.
    AgentController restored(std::make_unique<SilentModel>(), database);
    QVERIFY2(restored.openError().isEmpty(), qPrintable(restored.openError()));
    AgentChatPanel panel(restored);
    panel.resize(1100, 1000);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    panel.openSession(root);
    auto* view = panel.findChild<MarkdownView*>();
    QVERIFY(view);
    QTRY_VERIFY2_WITH_TIMEOUT(orderFailure(view).isEmpty(), qPrintable(orderFailure(view)), 3000);
    QCOMPARE(panel.findChildren<QFrame*>(QStringLiteral("agentSubTaskCard")).size(), 4);
    QVERIFY(captureAgentFixture(panel, QStringLiteral("sub-agent-timeline-cold") + evidenceSuffix));
}

void AgentPanelSessionTest::subAgentCardsWaitForTheirHistoryPage() {
    QTemporaryDir storage;
    QVERIFY(storage.isValid());
    const QString database = storage.filePath(QStringLiteral("sub-agent-paging.db"));
    const std::string root = "ses_sub_agent_paging_root";
    const QString childId = QStringLiteral("ses_sub_agent_paging_child");
    const QString before = QStringLiteral("prt_paging_000_before");
    const QString spawn = QStringLiteral("prt_paging_001_spawn");
    const QString after = QStringLiteral("prt_paging_002_after");
    {
        auto opened = makeMaiSqliteStore(database.toStdString());
        QVERIFY(opened);
        auto store = std::move(opened.value());
        MaiSession session;
        session.id = root;
        session.directory = storage.path().toStdString();
        session.created = session.updated = 1;
        store->putSession(session);
        MaiSession child = session;
        child.id = childId.toStdString();
        child.parentId = root;
        child.title = "Earlier-page child";
        child.depth = 1;
        // An old assistant can spawn after >40 later steering inputs. Its
        // child's newer creation time must not fabricate a visible tail anchor.
        child.created = child.updated = 10000;
        store->putSession(child);
        for (int index = 0; index < 45; ++index) {
            MaiMessage message;
            message.id = QStringLiteral("msg_sub_agent_paging_%1").arg(index, 4, 10, QLatin1Char('0')).toStdString();
            message.role = index == 0 ? MaiRole::Assistant : MaiRole::User;
            message.created = index + 1;
            message.completed = index == 0 ? 0 : message.created;
            if (index == 0) {
                MaiToolPart tool;
                tool.tool = "spawn_agent";
                tool.callId = "call_earlier_spawn";
                tool.input = R"({"task_name":"Earlier-page child","prompt":"Local fixture"})";
                tool.output = "Started sub-agent " + child.id +
                              " for \"Earlier-page child\". Use wait_agent with that id when you need its answer.";
                tool.state = MaiToolState::Completed;
                message.parts.push_back({before.toStdString(), MaiTextPart{"Earlier parent before spawn"}, 1});
                message.parts.push_back({spawn.toStdString(), std::move(tool), 2});
                message.parts.push_back({after.toStdString(), MaiTextPart{"Earlier parent after spawn"}, 3});
            } else {
                message.parts.push_back({"prt_" + message.id, MaiTextPart{"Later parent message"}, index + 1});
            }
            store->putMessage(root, message);
        }
        QVERIFY(!store->lastWriteError().hasError());
    }
    AgentController controller(std::make_unique<SilentModel>(), database);
    AgentChatPanel panel(controller);
    panel.resize(1000, 700);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    panel.openSession(QString::fromStdString(root));
    auto* view = panel.findChild<MarkdownView*>();
    auto* older = panel.findChild<QPushButton*>(QStringLiteral("agentLoadEarlierButton"));
    QVERIFY(view && older && !older->isHidden());
    const QString card = QStringLiteral("sub-") + childId;
    QVERIFY(!view->contains(before));
    QVERIFY2(!view->contains(card), "A child whose spawn is outside the loaded history must not appear at the tail");
    older->click();
    QTRY_VERIFY(view->contains(before));
    QTRY_VERIFY(view->contains(card));
    const QString group = QStringLiteral("tool-group-") + spawn;
    QTRY_VERIFY(view->contains(group));
    QTRY_VERIFY(view->itemRect(before).bottom() <= view->itemRect(group).top());
    QTRY_VERIFY(view->itemRect(group).bottom() <= view->itemRect(card).top());
    QTRY_VERIFY(view->itemRect(card).bottom() <= view->itemRect(after).top());
    QTRY_VERIFY(view->itemRect(after).bottom() <= view->itemRect(QStringLiteral("msg_sub_agent_paging_0001")).top());
    view->verticalScrollBar()->setValue(qMax(0, qRound(view->itemRect(before).top()) - 20));
    QVERIFY(captureAgentFixture(panel, QStringLiteral("sub-agent-timeline-paged")));
}

void AgentPanelSessionTest::approvalFromAnotherSessionIsStillShown() {
    // **这条和上面几条方向相反，是故意的。**
    //
    // 子 Agent 没有自己的界面。它要审批时如果被过滤掉，它会永远挂在闸门上，
    // 而用户只看到「一直在跑」。所以授权和提问**不按会话过滤**。
    Harness harness;
    QVERIFY(QTest::qWaitForWindowExposed(harness.panel.get()));

    QSignalSpy spy(harness.controller.get(), &AgentController::permissionAsked);
    MaiEvent asked;
    asked.id = "evt_perm";
    asked.type = MaiEventType::PermissionAsked;
    asked.sessionId = harness.other.toUtf8().constData();
    asked.messageId = "msg_other";
    asked.partId = "prt_other";
    asked.permissionId = "per_other";
    harness.controller->agent().eventBus().publish(asked);
    QTest::qWait(120);

    // 信号要到得了面板。面板拿这个 id 去核心查待裁决列表（那份列表是全局的，
    // 不分会话），所以子 Agent 的授权照样答得了。
    QCOMPARE(spy.count(), 1);
}

void AgentPanelSessionTest::newSessionUsesAppWorkspaceInsteadOfProcessDirectory() {
    QTemporaryDir processDirectory;
    QVERIFY(processDirectory.isValid());
    struct RestoreCurrentDirectory {
        QString original = QDir::currentPath();
        ~RestoreCurrentDirectory() {
            QDir::setCurrent(original);
        }
    } restore;
    QVERIFY(QDir::setCurrent(processDirectory.path()));

    AgentController controller(std::make_unique<SilentModel>(), QString());
    AgentChatPanel panel(controller);
    panel.openSession();
    const auto sessions = controller.agent().listSessions();
    QCOMPARE(sessions.size(), std::size_t(1));
    const QString workspace = QString::fromStdString(sessions.front().directory);
    QVERIFY(workspace.endsWith(QStringLiteral("Workspace")));
    QVERIFY(workspace.startsWith(
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).canonicalPath()));
    QVERIFY(QFileInfo(workspace).isWritable());
    QVERIFY(!QDir(workspace).isRoot());
    QVERIFY(QDir(workspace).canonicalPath() != processDirectory.path());
}

void AgentPanelSessionTest::newSessionOffersWorkspaceChoice() {
    Harness harness;
    auto* more = harness.panel->findChild<QPushButton*>(QStringLiteral("agentMoreActions"));
    auto* editor = harness.panel->findChild<ComposerTextEdit*>();
    auto* workspace = harness.panel->findChild<QPushButton*>(QStringLiteral("agentChooseWorkspaceButton"));
    auto* openWorkspace = harness.panel->findChild<QPushButton*>(QStringLiteral("agentOpenWorkspaceButton"));
    QVERIFY(more != nullptr);
    QVERIFY(editor != nullptr);
    QVERIFY(workspace != nullptr);
    QVERIFY(openWorkspace != nullptr);
    QVERIFY(more->menu() == nullptr);
    auto* actionPanel = harness.panel->findChild<QWidget*>(QStringLiteral("agentActionPanel"));
    QVERIFY(actionPanel != nullptr);
    QCOMPARE(workspace->parentWidget(), openWorkspace->parentWidget());
    QCOMPARE(workspace->parentWidget()->parentWidget(), actionPanel);
    QVERIFY(!workspace->isVisible());
    more->click();
    QTRY_VERIFY(workspace->isVisible());
    QVERIFY(openWorkspace->isVisible());
    const QString capturePath = qEnvironmentVariable("MAICHAT_WORKSPACE_MENU_CAPTURE");
    if (!capturePath.isEmpty()) QVERIFY(actionPanel->grab().save(capturePath));
    MaiSession session;
    QVERIFY(harness.controller->agent().getSession(harness.mine.toStdString(), session));
    QVERIFY(workspace->toolTip().contains(QDir::toNativeSeparators(QString::fromStdString(session.directory))));
    QVERIFY(!workspace->text().startsWith(QStringLiteral("工作目录 ·")));
    QCOMPARE(openWorkspace->accessibleName(), QStringLiteral("打开当前工作目录"));
    QVERIFY(openWorkspace->width() < workspace->width());
    auto* modelSection = actionPanel->findChild<QPushButton*>(QStringLiteral("agentEffortSection"));
    QVERIFY(modelSection != nullptr);
    QVERIFY(modelSection->accessibleName().startsWith(QStringLiteral("思考强度：")));
}

void AgentPanelSessionTest::chosenWorkspaceKeepsCurrentSession() {
    QTemporaryDir firstDirectory;
    QTemporaryDir secondDirectory;
    QVERIFY(firstDirectory.isValid());
    QVERIFY(secondDirectory.isValid());
    AgentController controller(std::make_unique<SilentModel>(), QString());
    AgentChatPanel panel(controller);

    panel.openSessionInDirectory(firstDirectory.path());
    const QString firstSession = panel.sessionId();
    QVERIFY(!firstSession.isEmpty());
    QSignalSpy finished(&controller, &AgentController::turnFinished);
    QVERIFY(controller.sendPrompt(firstSession, QStringLiteral("Keep this conversation")));
    QVERIFY(finished.wait(15000));
    const auto before = controller.agent().listMessages(firstSession.toStdString());
    QVERIFY(!before.empty());
    auto* editor = panel.findChild<QTextEdit*>(QStringLiteral("agentPromptEditor"));
    QVERIFY(editor != nullptr);
    editor->setPlainText(QStringLiteral("Keep the unsent draft"));
    panel.openSessionInDirectory(secondDirectory.path());
    const QString secondSession = panel.sessionId();
    QCOMPARE(secondSession, firstSession);
    QCOMPARE(editor->toPlainText(), QStringLiteral("Keep the unsent draft"));
    QCOMPARE(controller.agent().listSessions().size(), std::size_t(1));
    QCOMPARE(controller.agent().listMessages(firstSession.toStdString()).size(), before.size());

    MaiSession current;
    QVERIFY(controller.agent().getSession(firstSession.toStdString(), current));
    QCOMPARE(QDir(QString::fromStdString(current.directory)).canonicalPath(),
             QDir(secondDirectory.path()).canonicalPath());
}

void AgentPanelSessionTest::explicitNewSessionUsesChosenDirectory() {
    QTemporaryDir firstDirectory;
    QTemporaryDir secondDirectory;
    QVERIFY(firstDirectory.isValid());
    QVERIFY(secondDirectory.isValid());
    AgentController controller(std::make_unique<SilentModel>(), QString());
    AgentChatPanel panel(controller);
    panel.openSessionInDirectory(firstDirectory.path());
    const QString firstSession = panel.sessionId();
    QVERIFY(!firstSession.isEmpty());

    panel.openNewSessionInDirectory(secondDirectory.path());
    const QString secondSession = panel.sessionId();
    QVERIFY(!secondSession.isEmpty());
    QVERIFY(secondSession != firstSession);
    QCOMPARE(controller.agent().listSessions().size(), std::size_t(2));
    MaiSession first;
    MaiSession second;
    QVERIFY(controller.agent().getSession(firstSession.toStdString(), first));
    QVERIFY(controller.agent().getSession(secondSession.toStdString(), second));
    QCOMPARE(QDir(QString::fromStdString(first.directory)).canonicalPath(),
             QDir(firstDirectory.path()).canonicalPath());
    QCOMPARE(QDir(QString::fromStdString(second.directory)).canonicalPath(),
             QDir(secondDirectory.path()).canonicalPath());
}

void AgentPanelSessionTest::chosenWorkspaceRejectsBusySession() {
    QTemporaryDir firstDirectory;
    QTemporaryDir secondDirectory;
    QVERIFY(firstDirectory.isValid());
    QVERIFY(secondDirectory.isValid());
    auto model = std::make_unique<DelayedFirstChunkModel>(false);
    auto* delayed = model.get();
    AgentController controller(std::move(model), QString());
    AgentChatPanel panel(controller);
    panel.openSessionInDirectory(firstDirectory.path());
    const QString sessionId = panel.sessionId();
    QVERIFY(controller.sendPrompt(sessionId, QStringLiteral("Keep the current workspace")));
    QTRY_VERIFY(delayed->entered());

    panel.openSessionInDirectory(secondDirectory.path());
    QCOMPARE(panel.sessionId(), sessionId);
    QCOMPARE(controller.agent().listSessions().size(), std::size_t(1));
    MaiSession current;
    QVERIFY(controller.agent().getSession(sessionId.toStdString(), current));
    QCOMPARE(QDir(QString::fromStdString(current.directory)).canonicalPath(),
             QDir(firstDirectory.path()).canonicalPath());
    delayed->release();
}

void AgentPanelSessionTest::sessionToolEventsRefreshListAndRecoverDeletedSelection() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AgentController controller(std::make_unique<SilentModel>(), QString());
    AgentChatPanel panel(controller);
    const QString current = controller.createSession(directory.path());
    panel.openSession(current);
    QSignalSpy changed(&panel, &AgentChatPanel::sessionListChanged);

    const auto created =
        controller.agent().submit(MaiCreateSession{directory.path().toStdString(), "Another topic", ""});
    QVERIFY(created);
    QTRY_VERIFY(changed.count() >= 1);
    QCOMPARE(panel.sessionId(), current);

    QVERIFY(controller.agent().submit(MaiDeleteSession{created.value()}));
    QTRY_VERIFY(changed.count() >= 2);
    QCOMPARE(panel.sessionId(), current);

    QVERIFY(controller.agent().submit(MaiDeleteSession{current.toStdString()}));
    QTRY_VERIFY(panel.sessionId() != current);
    QVERIFY(!panel.sessionId().isEmpty());
    QCOMPARE(controller.agent().listSessions().size(), std::size_t(1));
}

void AgentPanelSessionTest::localSessionMenuOffersDelete() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AgentController controller(std::make_unique<SilentModel>(), QString());
    const QString sessionId = controller.createSession(directory.path(), QStringLiteral("测试对话"));
    QVERIFY(!sessionId.isEmpty());
    const QString anotherId = controller.createSession(directory.path(), QStringLiteral("另一段对话"));
    QVERIFY(!anotherId.isEmpty());
    AgentSessionList list(controller);
    list.resize(330, 480);
    list.show();
    list.refresh();
    list.setCurrent(sessionId);
    auto* rows = list.findChild<QListWidget*>();
    QVERIFY(rows != nullptr);
    QListWidgetItem* sessionRow = nullptr;
    for (int index = 0; index < rows->count(); ++index) {
        if (rows->item(index)->data(Qt::UserRole).toString() == anotherId) sessionRow = rows->item(index);
    }
    QVERIFY(sessionRow != nullptr);
    if (qEnvironmentVariableIsSet("MAICHAT_SESSION_LIST_SCREENSHOT"))
        QVERIFY(list.grab().save(qEnvironmentVariable("MAICHAT_SESSION_LIST_SCREENSHOT")));
    const QRect row = rows->visualItemRect(sessionRow);
    QSignalSpy requested(&list, &AgentSessionList::deleteSessionRequested);
    QSignalSpy selected(&list, &AgentSessionList::selected);
    QTest::mouseClick(rows->viewport(), Qt::LeftButton, Qt::NoModifier,
                      QPoint(row.right() - UiZoom::s(24), row.center().y()));
    auto* menu = list.findChild<QMenu*>(QStringLiteral("agentSessionMenu"));
    QTRY_VERIFY(menu != nullptr && menu->isVisible());
    if (qEnvironmentVariableIsSet("MAICHAT_SESSION_MENU_SCREENSHOT"))
        QVERIFY(menu->grab().save(qEnvironmentVariable("MAICHAT_SESSION_MENU_SCREENSHOT")));
    QCOMPARE(menu->actions().size(), 1);
    QCOMPARE(menu->actions().first()->text(), QStringLiteral("删除对话"));
    menu->actions().first()->trigger();
    QCOMPARE(requested.size(), 1);
    QCOMPARE(requested.takeFirst().at(0).toString(), anotherId);
    QCOMPARE(selected.size(), 0);
    menu->close();
}

// 待审批操作切换会话后仍需绑定原会话；返回时显示原请求而非重新发起操作。
void AgentPanelSessionTest::pendingApprovalIsRestoredAfterSessionSwitch() {
    std::atomic<int> executed{0};
    AgentController controller(std::make_unique<ApprovalModel>(), QString(), [&executed](MaiToolRegistry& tools) {
        tools.add(std::make_unique<ApprovalTool>(executed));
    });
    AgentChatPanel panel(controller);
    panel.resize(800, 550);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    const QString session = controller.createSession(QDir::currentPath());
    panel.openSession(session);
    auto* editor = panel.findChild<QTextEdit*>();
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    QVERIFY(editor != nullptr);
    QVERIFY(send != nullptr);
    editor->setPlainText(QStringLiteral("send the report"));
    send->click();
    QTRY_COMPARE(controller.agent().listPendingPermissions().size(), std::size_t(1));
    // 审批等待期间继续受理输入；取消任务由独立 Stop 控件负责。
    QCOMPARE(send->toolTip(), QStringLiteral("发送消息"));
    QVERIFY(send->isEnabled());
    auto* stop = panel.findChild<QPushButton*>(QStringLiteral("agentStopButton"));
    QVERIFY(stop != nullptr && stop->isVisible() && stop->isEnabled());

    auto approvalIsVisible = [&panel] {
        for (QLabel* label : panel.findChildren<QLabel*>()) {
            if (label->text() == QStringLiteral("等你点头") && label->isVisible()) return true;
        }
        return false;
    };
    QTRY_VERIFY(approvalIsVisible());

    panel.openSession(controller.createSession(QDir::currentPath()));
    QCOMPARE(send->toolTip(), QStringLiteral("发送消息"));
    QVERIFY(send->isEnabled());
    QVERIFY(stop->isHidden());
    auto unrelatedApprovalIsVisible = [&panel] {
        for (QWidget* card : panel.findChildren<QWidget*>(QStringLiteral("agentToolCard"))) {
            if (card->isVisible()) return true;
        }
        return false;
    };
    QTRY_VERIFY(!unrelatedApprovalIsVisible());
    panel.openSession(session);
    QCOMPARE(send->toolTip(), QStringLiteral("发送消息"));
    QVERIFY(send->isEnabled());
    QVERIFY(stop->isVisible() && stop->isEnabled());
    QTRY_VERIFY2(approvalIsVisible(), "A pending send must not become an unapprovable queue item");
    QPushButton* allow = nullptr;
    for (QPushButton* button : panel.findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("允许") && button->isVisible()) allow = button;
    }
    QVERIFY(allow != nullptr);
    allow->click();
    controller.agent().waitIdle();
    QCOMPARE(executed.load(), 1);
}

void AgentPanelSessionTest::approvedChildToolCompletesWhileParentPageIsCached() {
    struct Gate {
        std::atomic<bool> entered{false};
        std::atomic<bool> released{false};
    };
    class ChildApprovalTool final : public MaiTool {
    public:
        explicit ChildApprovalTool(std::shared_ptr<Gate> gate) : gate_(std::move(gate)) {}
        std::string name() const override {
            return "maichat_send_text";
        }
        std::string description() const override {
            return "Local child approval fixture";
        }
        std::string parametersSchema() const override {
            return R"({"type":"object"})";
        }
        bool requiresApproval(const std::string&) const override {
            return true;
        }
        bool requiresPerCallApproval(const std::string&) const override {
            return true;
        }
        MaiToolResult execute(const std::string&, const MaiToolContext& context) override {
            gate_->entered.store(true);
            while (!gate_->released.load() && !context.isCanceled())
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            return context.isCanceled() ? MaiToolResult::failure(MaiErrorCode::Canceled, "Fixture canceled")
                                        : MaiToolResult::success("Child fixture completed");
        }

    private:
        std::shared_ptr<Gate> gate_;
    };
    auto gate = std::make_shared<Gate>();
    AgentController controller(std::make_unique<ApprovalModel>(), QString(), [gate](MaiToolRegistry& tools) {
        tools.add(std::make_unique<ChildApprovalTool>(gate));
    });
    struct ReleaseOnExit {
        std::shared_ptr<Gate> gate;
        ~ReleaseOnExit() {
            gate->released.store(true);
        }
    } release{gate};
    AgentChatPanel panel(controller);
    panel.resize(900, 650);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    const QString parent = controller.createSession(QDir::currentPath());
    const QString other = controller.createSession(QDir::currentPath());
    panel.openSession(parent);
    const auto child = controller.agent().spawnSubAgent(parent.toStdString(), "approval-reviewer", "Fixture task");
    QVERIFY(child);
    QTRY_COMPARE(controller.agent().listPendingPermissions().size(), std::size_t(1));
    QPushButton* allow = nullptr;
    const auto approvalIsVisible = [&] {
        for (QPushButton* button : panel.findChildren<QPushButton*>())
            if (button->isVisible() && button->text() == QStringLiteral("允许")) {
                allow = button;
                return true;
            }
        return false;
    };
    QTRY_VERIFY(approvalIsVisible());
    allow->click();
    QTRY_VERIFY(gate->entered.load());
    QPointer<QPushButton> summary = panel.findChild<QPushButton*>(QStringLiteral("agentToolGroupDisclosure"));
    QVERIFY(summary);
    panel.openSession(other);
    gate->released.store(true);
    controller.agent().waitIdle();
    QCoreApplication::processEvents();
    panel.openSession(parent);
    QTRY_VERIFY(summary && summary->isVisible());
    QTRY_VERIFY(summary->text().contains(QStringLiteral("已完成")));
    QTest::mouseClick(summary, Qt::LeftButton);
    bool foundOutput = false;
    for (QLabel* label : panel.findChildren<QLabel*>(QStringLiteral("agentToolDetail")))
        foundOutput |= label->text().contains(QStringLiteral("Child fixture completed"));
    QVERIFY(foundOutput);
}

// The question tool is persisted protocol state; the visible conversation uses
// its real part ID without inventing a second Core User message.
void AgentPanelSessionTest::questionUsesNormalTranscriptAndSurvivesColdReplay() {
    AgentController controller(std::make_unique<QuestionModel>(), QString());
    AgentChatPanel panel(controller);
    panel.resize(1000, 700);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    const QString session = controller.createSession(QDir::currentPath());
    panel.openSession(session);
    auto* editor = panel.findChild<QTextEdit*>();
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    auto* view = panel.findChild<MarkdownView*>();
    QVERIFY(editor && send && view);
    editor->setPlainText(QStringLiteral("ask me"));
    send->click();
    QTRY_COMPARE(controller.pendingQuestions().size(), std::size_t(1));
    QTRY_VERIFY(panel.findChild<MarkdownView*>()->contains(
        QString::fromStdString(controller.pendingQuestions().front().partId) + QStringLiteral("-question")));
    const auto question = controller.pendingQuestions().front();
    const QString questionItem = QString::fromStdString(question.partId) + QStringLiteral("-question");
    const QString answerItem = QString::fromStdString(question.partId) + QStringLiteral("-answer");
    QTRY_VERIFY(view->contains(questionItem));
    QCOMPARE(editor->placeholderText(), QStringLiteral("交给它做点什么…"));
    QCOMPARE(send->toolTip(), QStringLiteral("发送消息"));
    QCOMPARE(view->itemRect(questionItem).left(), qreal(UiZoom::s(48)));
    QVERIFY(panel.findChildren<QWidget*>(QStringLiteral("agentToolGroup")).empty());
    QVERIFY(captureAgentFixture(panel, QStringLiteral("normal-question-pending")));
    editor->setPlainText(QStringLiteral("Use A"));
    send->click();
    QTRY_VERIFY(view->contains(answerItem));
    controller.agent().waitIdle();
    QCoreApplication::processEvents();
    for (int i = 0; i < 3; ++i) panel.openSession(controller.createSession(QDir::currentPath()));
    panel.openSession(session);
    QVERIFY(view->contains(questionItem));
    QVERIFY(view->contains(answerItem));
    view->selectAll();
    QCOMPARE(view->selectedText().count(QStringLiteral("Choose A or B")), 1);
    QCOMPARE(view->selectedText().count(QStringLiteral("Use A")), 1);
    view->clearSelection();
    int userMessages = 0;
    for (const auto& message : controller.agent().listMessages(session.toStdString()))
        if (message.role == MaiRole::User) ++userMessages;
    QCOMPARE(userMessages, 1);
    QVERIFY(captureAgentFixture(panel, QStringLiteral("normal-question-answered-replay")));
}

void AgentPanelSessionTest::childQuestionUsesParentComposerWithoutConsumingMedia() {
    AgentController controller(std::make_unique<QuestionModel>(), QString());
    AgentChatPanel panel(controller);
    panel.resize(1000, 700);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    const QString root = controller.createSession(QDir::currentPath());
    panel.openSession(root);
    const auto child = controller.agent().spawnSubAgent(root.toStdString(), "question-reviewer", "Ask a question");
    QVERIFY(child);
    QTRY_COMPARE(controller.pendingQuestions().size(), std::size_t(1));
    QTRY_VERIFY(panel.findChild<MarkdownView*>()->contains(
        QString::fromStdString(controller.pendingQuestions().front().partId) + QStringLiteral("-question")));
    const auto question = controller.pendingQuestions().front();
    auto* editor = panel.findChild<QTextEdit*>();
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    auto* view = panel.findChild<MarkdownView*>();
    QVERIFY(editor && send && view);
    const QString questionItem = QString::fromStdString(question.partId) + QStringLiteral("-question");
    const QString answerItem = QString::fromStdString(question.partId) + QStringLiteral("-answer");
    QTRY_VERIFY(view->contains(questionItem));
    QTemporaryDir media;
    QImage image(10, 10, QImage::Format_RGB32);
    image.fill(Qt::blue);
    const QString path = media.filePath(QStringLiteral("draft.png"));
    QVERIFY(image.save(path));
    QTextImageFormat format;
    format.setName(path);
    editor->textCursor().insertImage(format);
    send->click();
    QCOMPARE(controller.pendingQuestions().size(), std::size_t(1));
    QVERIFY(editor->toPlainText().contains(QChar(0xFFFC)));
    editor->textCursor().insertText(QStringLiteral("Use A"));
    send->click();
    controller.agent().waitIdle();
    QTRY_VERIFY(controller.pendingQuestions().empty());
    QVERIFY(editor->toPlainText().contains(QChar(0xFFFC)));
    QVERIFY(!editor->toPlainText().contains(QStringLiteral("Use A")));
    QTRY_VERIFY(view->contains(answerItem));
    for (int i = 0; i < 3; ++i) panel.openSession(controller.createSession(QDir::currentPath()));
    panel.openSession(root);
    QVERIFY(view->contains(questionItem));
    QVERIFY(view->contains(answerItem));
    QCOMPARE(controller.agent().listMessages(root.toStdString()).size(), std::size_t(0));
    QVERIFY(captureAgentFixture(panel, QStringLiteral("normal-child-question-replayed")));
}

// The parent message starts before the child, but its later text part is
// created after the question. Cross-session display order follows actual parts.
void AgentPanelSessionTest::childQuestionKeepsItsPositionBetweenParentParts() {
    class TimelineModel final : public MaiModelClient {
    public:
        MaiError stream(const MaiModelRequest&, const MaiStreamSink& sink, const std::atomic<bool>&) override {
            const int call = calls_++;
            if (call == 0) {
                if (sink.onText) sink.onText("Root before question");
                if (sink.onToolCall) sink.onToolCall({"root-probe", "progress_probe_x", "{}"});
            } else if (call == 1) {
                if (sink.onToolCall)
                    sink.onToolCall({"child-question", "question", R"({"question":"Child timeline question"})"});
            } else if (sink.onText)
                sink.onText("Root after question");
            return {};
        }
        MaiWireApi wireApi() const override {
            return MaiWireApi::ChatCompletions;
        }

    private:
        std::atomic<int> calls_{0};
    };
    auto progress = std::make_shared<IntermediateProgressState>();
    AgentController controller(std::make_unique<TimelineModel>(), QString(), [progress](MaiToolRegistry& tools) {
        tools.add(std::make_unique<IntermediateProgressTool>(progress, 1));
    });
    struct ReleaseOnExit {
        std::shared_ptr<IntermediateProgressState> progress;
        ~ReleaseOnExit() {
            progress->released.store(1);
        }
    } release{progress};
    AgentChatPanel panel(controller);
    panel.resize(1000, 700);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    const QString root = controller.createSession(QDir::currentPath());
    panel.openSession(root);
    QVERIFY(controller.sendPrompt(root, QStringLiteral("run timeline fixture")));
    QTRY_COMPARE(progress->entered.load(), 1);
    const auto child = controller.agent().spawnSubAgent(root.toStdString(), "question-reviewer", "Ask a question");
    QVERIFY(child);
    QTRY_COMPARE(controller.pendingQuestions().size(), std::size_t(1));
    const auto question = controller.pendingQuestions().front();
    auto* view = panel.findChild<MarkdownView*>();
    const QString questionItem = QString::fromStdString(question.partId) + QStringLiteral("-question");
    QTRY_VERIFY(view->contains(questionItem));
    progress->released.store(1);
    QString laterPart;
    const auto findLaterPart = [&] {
        for (const auto& message : controller.agent().listMessages(root.toStdString()))
            for (const auto& part : message.parts)
                if (const auto* text = std::get_if<MaiTextPart>(&part.body))
                    if (text->text == "Root after question") laterPart = QString::fromStdString(part.id);
        return !laterPart.isEmpty() && view->contains(laterPart);
    };
    QTRY_VERIFY(findLaterPart());
    QVERIFY(view->itemRect(questionItem).top() < view->itemRect(laterPart).top());
    for (const auto* quote : panel.findChildren<QPushButton*>(QStringLiteral("agentQuoteAction")))
        QVERIFY(quote->property("messageId").toString() != QString::fromStdString(question.messageId));
    auto* editor = panel.findChild<QTextEdit*>();
    editor->setPlainText(QStringLiteral("Continue"));
    panel.findChild<QPushButton*>(QStringLiteral("agentSendButton"))->click();
    controller.agent().waitIdle();
    QCoreApplication::processEvents();
    for (int i = 0; i < 3; ++i) panel.openSession(controller.createSession(QDir::currentPath()));
    panel.openSession(root);
    QVERIFY(view->itemRect(questionItem).top() < view->itemRect(laterPart).top());
}

void AgentPanelSessionTest::nestedQuestionsFollowOrderAndReleaseAfterCancellation() {
    class NestedModel final : public MaiModelClient {
    public:
        MaiError stream(const MaiModelRequest&, const MaiStreamSink& sink, const std::atomic<bool>&) override {
            if (calls_++ < 2) {
                if (sink.onToolCall)
                    sink.onToolCall({"nested-question", "question", R"({"question":"Nested fixture question"})"});
            } else if (sink.onText)
                sink.onText("done");
            return {};
        }
        MaiWireApi wireApi() const override {
            return MaiWireApi::ChatCompletions;
        }

    private:
        std::atomic<int> calls_{0};
    };
    AgentController controller(std::make_unique<NestedModel>(), QString());
    AgentChatPanel panel(controller);
    panel.resize(1000, 700);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    const QString root = controller.createSession(QDir::currentPath());
    panel.openSession(root);
    const auto child = controller.agent().spawnSubAgent(root.toStdString(), "child-reviewer", "Ask first");
    QVERIFY(child);
    QTRY_COMPARE(controller.pendingQuestions().size(), std::size_t(1));
    const auto first = controller.pendingQuestions().front();
    const auto nested = controller.agent().spawnSubAgent(first.sessionId, "nested-reviewer", "Ask second");
    QVERIFY(nested);
    QTRY_COMPARE(controller.pendingQuestions().size(), std::size_t(2));
    auto* editor = panel.findChild<QTextEdit*>();
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    auto* view = panel.findChild<MarkdownView*>();
    editor->setPlainText(QStringLiteral("First answer"));
    send->click();
    QTRY_COMPARE(controller.pendingQuestions().size(), std::size_t(1));
    QCOMPARE(controller.pendingQuestions().front().sessionId, nested.value());
    QTRY_VERIFY(view->contains(QString::fromStdString(first.partId) + QStringLiteral("-answer")));
    const auto cancelled =
        controller.interrupt(QString::fromStdString(nested.value()),
                             QString::fromStdString(controller.agent().activeTurnId(nested.value())));
    QVERIFY(cancelled);
    controller.agent().waitIdle();
    QCoreApplication::processEvents();
    editor->setPlainText(QStringLiteral("A new ordinary root message"));
    send->click();
    controller.agent().waitIdle();
    QVERIFY(editor->toPlainText().isEmpty());
    QCOMPARE(controller.agent().listMessages(root.toStdString()).size(), std::size_t(2));
}

void AgentPanelSessionTest::rootQuestionAfterSteeringKeepsItsPartPosition() {
    class SteeredQuestionModel final : public MaiModelClient {
    public:
        std::atomic<bool> sawQueuedInput{false};
        MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink, const std::atomic<bool>&) override {
            const int call = calls_++;
            if (call == 0 && sink.onToolCall) {
                sink.onToolCall({"held-root", "progress_probe_x", "{}"});
                sink.onToolCall({"ask-root", "question", R"({"question":"Root question after queued input"})"});
            } else if (call == 1 && sink.onToolCall) {
                // 补充会使旧的未执行问题取消；模型看到补充后再决定是否重新提问。
                for (const auto& message : request.messages)
                    if (message.role == MaiModelRole::User && message.content == "Queued root instruction")
                        sawQueuedInput = true;
                sink.onToolCall(
                    {"replanned-root-question", "question", R"({"question":"Root question after queued input"})"});
            } else if (sink.onText)
                sink.onText("done");
            return {};
        }
        MaiWireApi wireApi() const override {
            return MaiWireApi::ChatCompletions;
        }

    private:
        std::atomic<int> calls_{0};
    };
    auto progress = std::make_shared<IntermediateProgressState>();
    auto model = std::make_unique<SteeredQuestionModel>();
    auto* observed = model.get();
    AgentController controller(std::move(model), QString(), [progress](MaiToolRegistry& tools) {
        tools.add(std::make_unique<IntermediateProgressTool>(progress, 1));
    });
    struct ReleaseOnExit {
        std::shared_ptr<IntermediateProgressState> state;
        ~ReleaseOnExit() {
            state->released.store(1);
        }
    } release{progress};
    AgentChatPanel panel(controller);
    panel.resize(1000, 700);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    const QString root = controller.createSession(QDir::currentPath());
    panel.openSession(root);
    QVERIFY(controller.sendPrompt(root, QStringLiteral("Run steered question fixture")));
    QTRY_COMPARE(progress->entered.load(), 1);
    auto* editor = panel.findChild<QTextEdit*>();
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    editor->setPlainText(QStringLiteral("Queued root instruction"));
    send->click();
    const auto recent = controller.agent().listMessagesPage(root.toStdString(), {}, 1);
    QVERIFY(!recent.empty() && recent.front().role == MaiRole::User);
    const QString userItem = QString::fromStdString(recent.front().id);
    progress->released.store(1);
    QTRY_COMPARE(controller.pendingQuestions().size(), std::size_t(1));
    QVERIFY(observed->sawQueuedInput.load());
    bool canceledOriginalQuestion = false;
    for (const auto& message : controller.agent().listMessages(root.toStdString()))
        for (const auto& part : message.parts)
            if (const auto* tool = std::get_if<MaiToolPart>(&part.body))
                canceledOriginalQuestion |= tool->callId == "ask-root" && tool->state == MaiToolState::Canceled;
    QVERIFY(canceledOriginalQuestion);
    const QString questionItem =
        QString::fromStdString(controller.pendingQuestions().front().partId) + QStringLiteral("-question");
    auto* view = panel.findChild<MarkdownView*>();
    QTRY_VERIFY(view->contains(questionItem));
    QVERIFY(view->itemRect(questionItem).top() > view->itemRect(userItem).top());
    editor->setPlainText(QStringLiteral("Continue"));
    send->click();
    controller.agent().waitIdle();
    QCoreApplication::processEvents();
    for (int i = 0; i < 3; ++i) panel.openSession(controller.createSession(QDir::currentPath()));
    panel.openSession(root);
    QVERIFY(view->itemRect(questionItem).top() > view->itemRect(userItem).top());
}

void AgentPanelSessionTest::pendingQuestionIsRestoredOnlyInItsSession() {
    AgentController controller(std::make_unique<QuestionModel>(), QString());
    AgentChatPanel panel(controller);
    panel.resize(800, 550);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    const QString session = controller.createSession(QDir::currentPath());
    panel.openSession(session);
    auto* editor = panel.findChild<QTextEdit*>();
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    QVERIFY(editor != nullptr);
    QVERIFY(send != nullptr);
    editor->setPlainText(QStringLiteral("ask me"));
    send->click();
    QTRY_COMPARE(controller.pendingQuestions().size(), std::size_t(1));
    QTRY_VERIFY(panel.findChild<MarkdownView*>()->contains(
        QString::fromStdString(controller.pendingQuestions().front().partId) + QStringLiteral("-question")));
    QTRY_COMPARE(editor->placeholderText(), QStringLiteral("交给它做点什么…"));
    QCOMPARE(send->text(), QString());
    QCOMPARE(send->toolTip(), QStringLiteral("发送消息"));
    QVERIFY(!send->icon().isNull());

    panel.openSession(controller.createSession(QDir::currentPath()));
    QCOMPARE(editor->placeholderText(), QStringLiteral("交给它做点什么…"));
    QCOMPARE(send->toolTip(), QStringLiteral("发送消息"));
    panel.openSession(session);
    QCOMPARE(editor->placeholderText(), QStringLiteral("交给它做点什么…"));
    QCOMPARE(send->text(), QString());
    QCOMPARE(send->toolTip(), QStringLiteral("发送消息"));
    editor->setPlainText(QStringLiteral("A"));
    send->click();
    controller.agent().waitIdle();
    QTRY_VERIFY(controller.pendingQuestions().empty());
}

// A resolved question must release the ordinary composer. Keep a draft written
// while waiting, and exercise the actual Stop/Send controls without a provider.
void AgentPanelSessionTest::stoppedQuestionReleasesComposerWithoutLosingDraft() {
    AgentController controller(std::make_unique<QuestionModel>(), QString());
    AgentChatPanel panel(controller);
    panel.resize(1000, 650);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    const QString session = controller.createSession(QDir::currentPath());
    panel.openSession(session);
    auto* editor = panel.findChild<QTextEdit*>();
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    auto* stop = panel.findChild<QPushButton*>(QStringLiteral("agentStopButton"));
    QVERIFY(editor && send && stop);
    editor->setPlainText(QStringLiteral("ask me"));
    send->click();
    QTRY_COMPARE(controller.pendingQuestions().size(), std::size_t(1));
    QTRY_VERIFY(panel.findChild<MarkdownView*>()->contains(
        QString::fromStdString(controller.pendingQuestions().front().partId) + QStringLiteral("-question")));
    QTRY_COMPARE(send->toolTip(), QStringLiteral("发送消息"));
    editor->setPlainText(QStringLiteral("Which workspace is active now?"));
    stop->click();
    controller.agent().waitIdle();
    QVERIFY(controller.pendingQuestions().empty());
    // Send retains the same normal label while a question is pending. Wait for
    // the actual terminal UI event, not that now-identical label, before resend.
    QTRY_VERIFY(!stop->isVisible());
    QTRY_COMPARE(send->toolTip(), QStringLiteral("发送消息"));
    QCOMPARE(editor->placeholderText(), QStringLiteral("交给它做点什么…"));
    QCOMPARE(editor->toPlainText(), QStringLiteral("Which workspace is active now?"));
    QVERIFY(captureAgentFixture(panel, QStringLiteral("question-stopped-draft-preserved")));
    send->click();
    controller.agent().waitIdle();
    QCOMPARE(editor->toPlainText(), QString());
    int userMessages = 0;
    for (const auto& message : controller.agent().listMessages(session.toStdString()))
        if (message.role == MaiRole::User) ++userMessages;
    QCOMPARE(userMessages, 2);
}

// Answers may come from another endpoint. Core events must clear this panel's
// stale route even though this panel did not submit or consume the answer draft.
void AgentPanelSessionTest::externalQuestionAnswerReleasesComposerWithoutLosingDraft() {
    AgentController controller(std::make_unique<QuestionModel>(), QString());
    AgentChatPanel panel(controller);
    panel.resize(1000, 650);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    const QString session = controller.createSession(QDir::currentPath());
    panel.openSession(session);
    auto* editor = panel.findChild<QTextEdit*>();
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    QVERIFY(editor && send);
    QSignalSpy answered(&controller, &AgentController::questionAnswered);
    editor->setPlainText(QStringLiteral("ask me"));
    send->click();
    QTRY_COMPARE(controller.pendingQuestions().size(), std::size_t(1));
    QTRY_VERIFY(panel.findChild<MarkdownView*>()->contains(
        QString::fromStdString(controller.pendingQuestions().front().partId) + QStringLiteral("-question")));
    QTRY_COMPARE(send->toolTip(), QStringLiteral("发送消息"));
    const auto question = controller.pendingQuestions().front();
    editor->setPlainText(QStringLiteral("Keep this unsent draft"));
    QVERIFY(controller.agent().submit(MaiReplyQuestion{question.id, "A"}));
    controller.agent().waitIdle();
    QTRY_COMPARE(answered.count(), 1);
    QTRY_COMPARE(send->toolTip(), QStringLiteral("发送消息"));
    QCOMPARE(editor->toPlainText(), QStringLiteral("Keep this unsent draft"));
    QVERIFY(captureAgentFixture(panel, QStringLiteral("question-external-answer-draft-preserved")));
}

// Resolve in Core before queued GUI events arrive. An expired answer must stay
// a draft; it must neither repeat raw NotFound notices nor become a new prompt
// implicitly. A deliberate subsequent Send is the ordinary message action.
void AgentPanelSessionTest::expiredQuestionKeepsDraftUntilExplicitResend() {
    AgentController controller(std::make_unique<QuestionModel>(), QString());
    AgentChatPanel panel(controller);
    panel.resize(1000, 650);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    const QString session = controller.createSession(QDir::currentPath());
    panel.openSession(session);
    auto* editor = panel.findChild<QTextEdit*>();
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    auto* view = panel.findChild<MarkdownView*>();
    QVERIFY(editor && send && view);
    editor->setPlainText(QStringLiteral("ask me"));
    send->click();
    QTRY_COMPARE(controller.pendingQuestions().size(), std::size_t(1));
    QTRY_VERIFY(panel.findChild<MarkdownView*>()->contains(
        QString::fromStdString(controller.pendingQuestions().front().partId) + QStringLiteral("-question")));
    QTRY_COMPARE(send->toolTip(), QStringLiteral("发送消息"));
    const auto question = controller.pendingQuestions().front();
    editor->setPlainText(QStringLiteral("My answer draft"));
    QVERIFY(controller.agent().submit(MaiReplyQuestion{question.id, "A"}));
    controller.agent().waitIdle();
    // No processEvents/QTRY between settlement and this click: it is a real
    // submission against a panel whose cached question has just become stale.
    send->click();
    QCOMPARE(editor->toPlainText(), QStringLiteral("My answer draft"));
    view->selectAll();
    QVERIFY(!view->selectedText().contains(QStringLiteral("question is no longer pending")));
    view->clearSelection();
    QCOMPARE(send->toolTip(), QStringLiteral("发送消息"));
    int userMessages = 0;
    for (const auto& message : controller.agent().listMessages(session.toStdString()))
        if (message.role == MaiRole::User) ++userMessages;
    QCOMPARE(userMessages, 1);
    QVERIFY(captureAgentFixture(panel, QStringLiteral("question-expired-draft-preserved")));
    send->click();
    controller.agent().waitIdle();
    QCOMPARE(editor->toPlainText(), QString());
    userMessages = 0;
    for (const auto& message : controller.agent().listMessages(session.toStdString()))
        if (message.role == MaiRole::User) ++userMessages;
    QCOMPARE(userMessages, 2);
}

void AgentPanelSessionTest::composerDraftsStayInTheirSessions() {
    Harness harness;
    QVERIFY(QTest::qWaitForWindowExposed(harness.panel.get()));
    auto* editor = harness.panel->findChild<QTextEdit*>();
    QVERIFY(editor != nullptr);
    editor->setPlainText(QStringLiteral("unsent in first session"));
    harness.panel->openSession(harness.other);
    QCOMPARE(editor->toPlainText(), QString());
    editor->setPlainText(QStringLiteral("unsent in second session"));
    harness.panel->openSession(harness.mine);
    QCOMPARE(editor->toPlainText(), QStringLiteral("unsent in first session"));
    harness.panel->openSession(harness.other);
    QCOMPARE(editor->toPlainText(), QStringLiteral("unsent in second session"));
}

void AgentPanelSessionTest::waitingIndicatorSurvivesSwitchingAwayAndBack() {
    auto model = std::make_unique<DelayedFirstChunkModel>(true);
    DelayedFirstChunkModel* delayed = model.get();
    AgentController controller(std::move(model), QString());
    AgentChatPanel panel(controller);
    panel.resize(700, 500);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    const QString session = controller.createSession(QDir::currentPath());
    panel.openSession(session);
    auto* editor = panel.findChild<QTextEdit*>();
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    QVERIFY(editor != nullptr);
    QVERIFY(send != nullptr);
    editor->setPlainText(QStringLiteral("long-running request"));
    send->click();
    QTRY_VERIFY(delayed->entered());
    panel.openSession(controller.createSession(QDir::currentPath()));
    panel.openSession(session);
    auto hasVisibleThinking = [&panel] {
        for (QWidget* card : panel.findChildren<QWidget*>(QStringLiteral("agentThinkingCard"))) {
            if (card->isVisible()) return true;
        }
        return false;
    };
    QVERIFY2(hasVisibleThinking(), "The active session must show its waiting state immediately");
    delayed->release();
    controller.agent().waitIdle();
}

void AgentPanelSessionTest::reasoningStatusSurvivesSwitchBeforeCheckpoint() {
    auto model = std::make_unique<PartialReasoningModel>();
    PartialReasoningModel* controlled = model.get();
    AgentController controller(std::move(model), QString());
    AgentChatPanel panel(controller);
    panel.resize(700, 500);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    const QString session = controller.createSession(QDir::currentPath());
    panel.openSession(session);
    auto* editor = panel.findChild<QTextEdit*>();
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    QVERIFY(editor != nullptr);
    QVERIFY(send != nullptr);
    QSignalSpy reasoning(&controller, &AgentController::reasoningDelta);
    editor->setPlainText(QStringLiteral("inspect"));
    send->click();
    QTRY_VERIFY(reasoning.count() >= 1);

    panel.openSession(controller.createSession(QDir::currentPath()));
    panel.openSession(session);
    QWidget* card = nullptr;
    for (QWidget* item : panel.findChildren<QWidget*>(QStringLiteral("agentThinkingCard"))) {
        if (item->isVisible()) card = item;
    }
    QVERIFY(card != nullptr);
    auto* status = card->findChild<QLabel*>();
    QVERIFY(status != nullptr);
    QCOMPARE(status->text(), QStringLiteral("正在思考…"));
    QVERIFY(card->findChild<QLabel*>(QStringLiteral("agentThinkingBody")) == nullptr);
    controlled->release.store(true);
    controller.agent().waitIdle();
}

// 模型首包延迟时，发送后的等待状态仍要立即可见。
void AgentPanelSessionTest::waitingIndicatorAppearsBeforeFirstModelChunk() {
    auto model = std::make_unique<DelayedFirstChunkModel>(true, 1);
    DelayedFirstChunkModel* delayed = model.get();
    AgentController controller(std::move(model), QString());
    const QString session = controller.createSession(QDir::currentPath());
    QVERIFY(controller.sendPrompt(session, QStringLiteral("previous question")));
    controller.agent().waitIdle();
    AgentChatPanel panel(controller);
    panel.resize(700, 500);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    panel.openSession(session);

    auto* editor = panel.findChild<QTextEdit*>();
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    QVERIFY(editor != nullptr);
    QVERIFY(send != nullptr);
    editor->setPlainText(QStringLiteral("你好"));
    QElapsedTimer timer;
    timer.start();
    QTest::mouseClick(send, Qt::LeftButton);
    qInfo() << "Agent prompt UI submit elapsed ms:" << timer.elapsed();

    QTRY_VERIFY(delayed->entered());
    qInfo() << "Agent request reached model stream after ms:" << timer.elapsed();
    auto* waiting = panel.findChild<QWidget*>(QStringLiteral("agentThinkingCard"));
    QVERIFY2(waiting != nullptr, "The waiting indicator must appear before the model sends data");
    QVERIFY(waiting->isVisible());

    delayed->release();
    controller.agent().waitIdle();
    QTRY_VERIFY(panel.findChildren<QWidget*>(QStringLiteral("agentThinkingCard")).isEmpty());
}

void AgentPanelSessionTest::waitingIndicatorClearsWhenModelHasNoReasoning() {
    auto model = std::make_unique<DelayedFirstChunkModel>(false);
    DelayedFirstChunkModel* delayed = model.get();
    AgentController controller(std::move(model), QString());
    AgentChatPanel panel(controller);
    panel.resize(700, 500);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    panel.openSession(controller.createSession(QDir::currentPath()));

    auto* editor = panel.findChild<QTextEdit*>();
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    QVERIFY(editor != nullptr);
    QVERIFY(send != nullptr);
    editor->setPlainText(QStringLiteral("你好"));
    QTest::mouseClick(send, Qt::LeftButton);
    QTRY_VERIFY(delayed->entered());
    QVERIFY(panel.findChild<QWidget*>(QStringLiteral("agentThinkingCard")) != nullptr);

    delayed->release();
    controller.agent().waitIdle();
    QTRY_VERIFY(panel.findChildren<QWidget*>(QStringLiteral("agentThinkingCard")).isEmpty());
    QVERIFY(panel.findChild<MarkdownView*>()->itemCount() >= 2);
}

// 思考流可很长；执行时只有短状态，完成后不保留思考正文。
void AgentPanelSessionTest::thinkingContentNeverRendersLiveOrAfterRestore() {
    class ReasoningModel final : public MaiModelClient {
    public:
        MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                        const std::atomic<bool>& cancel) override {
            (void)request;
            (void)cancel;
            if (sink.onReasoning) {
                for (int i = 0; i < 20; ++i) sink.onReasoning("想一想，这段思考内容不应进入界面布局。");
            }
            if (sink.onText) sink.onText("回答完了。");
            return {};
        }
        MaiWireApi wireApi() const override {
            return MaiWireApi::ChatCompletions;
        }
    };

    AgentController controller(std::make_unique<ReasoningModel>(), QString());
    AgentChatPanel panel(controller);
    panel.resize(700, 500);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    const QString mine = controller.createSession(QDir::currentPath());
    panel.openSession(mine);
    auto* editor = panel.findChild<QTextEdit*>();
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    QVERIFY(editor != nullptr);
    QVERIFY(send != nullptr);
    editor->setPlainText(QStringLiteral("问点什么"));
    QTest::mouseClick(send, Qt::LeftButton);

    controller.agent().waitIdle();
    QTRY_VERIFY(panel.findChildren<QWidget*>(QStringLiteral("agentThinkingCard")).isEmpty());
    auto* view = panel.findChild<MarkdownView*>();
    QVERIFY(view != nullptr);
    view->selectAll();
    QVERIFY(view->selectedText().contains(QStringLiteral("回答完了")));
    QVERIFY(!view->selectedText().contains(QStringLiteral("想一想")));

    const QString other = controller.createSession(QDir::currentPath());
    panel.openSession(other);
    panel.openSession(mine);
    QTRY_VERIFY(panel.findChildren<QWidget*>(QStringLiteral("agentThinkingCard")).isEmpty());
    view->selectAll();
    QVERIFY(view->selectedText().contains(QStringLiteral("回答完了")));
    QVERIFY(!view->selectedText().contains(QStringLiteral("想一想")));
}

void AgentPanelSessionTest::intermediateProgressStaysVisibleAcrossToolsAndHistoryRestore() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString database = directory.filePath(QStringLiteral("progress-fixture.db"));
    QString session;
    QStringList textPartIds;
    const QStringList expectedText{QStringLiteral("Progress A: inspect local inputs."),
                                   QStringLiteral("Progress B: validate observed results."),
                                   QStringLiteral("Final C: both local checks completed.")};
    const auto sequence = [](AgentController& controller, const QString& id) {
        QStringList result;
        for (const MaiMessage& message : controller.agent().listMessages(id.toStdString())) {
            if (message.role != MaiRole::Assistant) continue;
            for (const MaiMessagePart& part : message.parts) {
                if (const auto* text = std::get_if<MaiTextPart>(&part.body))
                    result.append(QString::fromStdString(text->text));
                else if (const auto* tool = std::get_if<MaiToolPart>(&part.body))
                    result.append(QString::fromStdString(tool->callId));
            }
        }
        return result;
    };
    const auto allExpectedParts = [&] {
        return QStringList{expectedText[0], QStringLiteral("X"), expectedText[1], QStringLiteral("Y"), expectedText[2]};
    };
    {
        auto state = std::make_shared<IntermediateProgressState>();
        AgentController controller(std::make_unique<IntermediateProgressModel>(), database,
                                   [state](MaiToolRegistry& tools) {
                                       tools.add(std::make_unique<IntermediateProgressTool>(state, 1));
                                       tools.add(std::make_unique<IntermediateProgressTool>(state, 2));
                                   });
        QVERIFY2(controller.openError().isEmpty(), qPrintable(controller.openError()));
        // 断言中途返回时也释放门控，防止测试清理被未完成的工具阻塞。
        struct ReleaseOnExit {
            std::shared_ptr<IntermediateProgressState> state;
            ~ReleaseOnExit() {
                state->released.store(2);
            }
        } releaseOnExit{state};
        AgentChatPanel panel(controller);
        panel.resize(1000, 800);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        session = controller.createSession(directory.path(), QStringLiteral("Intermediate progress fixture"));
        panel.openSession(session);
        auto* view = panel.findChild<MarkdownView*>();
        QVERIFY(view != nullptr);
        QVERIFY(controller.sendPrompt(session, QStringLiteral("Run two local fixture checks.")));

        QTRY_COMPARE_WITH_TIMEOUT(state->entered.load(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(selectedDocumentText(*view).contains(expectedText[0]), 5000);
        QVERIFY(controller.agent().isBusy(session.toStdString()));
        QCOMPARE(state->released.load(), 0);
        QCOMPARE(sequence(controller, session), (QStringList{expectedText[0], QStringLiteral("X")}));
        QVERIFY(!selectedDocumentText(*view).contains(expectedText[1]));
        QVERIFY(captureProgressFixture(panel, QStringLiteral("01-tool-x-waiting")));

        state->released.store(1);
        QTRY_COMPARE_WITH_TIMEOUT(state->entered.load(), 2, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(selectedDocumentText(*view).contains(expectedText[1]), 5000);
        QVERIFY(controller.agent().isBusy(session.toStdString()));
        QCOMPARE(sequence(controller, session),
                 (QStringList{expectedText[0], QStringLiteral("X"), expectedText[1], QStringLiteral("Y")}));
        QWidget* firstGroup = nullptr;
        QWidget* secondGroup = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((firstGroup = progressToolGroup(panel, QStringLiteral("progress_probe_x"))) != nullptr,
                                 5000);
        QTRY_VERIFY_WITH_TIMEOUT(
            (secondGroup = progressToolGroup(panel, QStringLiteral("progress_probe_y"))) != nullptr, 5000);
        // 工具 Y 不能折叠进 X 的旧组，跨过已显示的中间正文 B。
        QVERIFY2(firstGroup != secondGroup, "Tool groups must not cross intermediate assistant text.");
        for (QWidget* group : {firstGroup, secondGroup}) {
            auto* summary = group->findChild<QPushButton*>(QStringLiteral("agentToolGroupDisclosure"));
            QVERIFY(summary != nullptr);
            QVERIFY(summary->text().contains(QStringLiteral("工具调用 1 次")));
        }
        for (const MaiMessage& message : controller.agent().listMessages(session.toStdString()))
            if (message.role == MaiRole::Assistant)
                for (const MaiMessagePart& part : message.parts)
                    if (std::holds_alternative<MaiTextPart>(part.body))
                        textPartIds.append(QString::fromStdString(part.id));
        QCOMPARE(textPartIds.size(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(
            view->itemRect(textPartIds[0]).height() > 0 && view->itemRect(textPartIds[1]).height() > 0, 5000);
        const auto groupTop = [view](QWidget* group) {
            return group->mapTo(view->viewport(), QPoint()).y() + view->verticalScrollBar()->value();
        };
        QVERIFY(view->itemRect(textPartIds[0]).bottom() <= groupTop(firstGroup));
        QVERIFY(groupTop(firstGroup) + firstGroup->height() <= view->itemRect(textPartIds[1]).top());
        QVERIFY(view->itemRect(textPartIds[1]).bottom() <= groupTop(secondGroup));
        QVERIFY(captureProgressFixture(panel, QStringLiteral("02-tool-y-waiting")));

        const QString other = controller.createSession(directory.path(), QStringLiteral("Empty fixture session"));
        panel.openSession(other);
        panel.openSession(session);
        QTRY_VERIFY_WITH_TIMEOUT(selectedDocumentText(*view).contains(expectedText[0]) &&
                                     selectedDocumentText(*view).contains(expectedText[1]),
                                 5000);
        firstGroup = progressToolGroup(panel, QStringLiteral("progress_probe_x"));
        secondGroup = progressToolGroup(panel, QStringLiteral("progress_probe_y"));
        QVERIFY(firstGroup != nullptr && secondGroup != nullptr && firstGroup != secondGroup);

        state->released.store(2);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.agent().isBusy(session.toStdString()), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(selectedDocumentText(*view).contains(expectedText[2]), 5000);
        QCOMPARE(sequence(controller, session), allExpectedParts());
        textPartIds.clear();
        for (const MaiMessage& message : controller.agent().listMessages(session.toStdString()))
            if (message.role == MaiRole::Assistant)
                for (const MaiMessagePart& part : message.parts)
                    if (std::holds_alternative<MaiTextPart>(part.body))
                        textPartIds.append(QString::fromStdString(part.id));
        QCOMPARE(textPartIds.size(), 3);
        QCOMPARE(QSet<QString>(textPartIds.begin(), textPartIds.end()).size(), 3);
        const QString visible = selectedDocumentText(*view);
        int previous = -1;
        for (const QString& text : expectedText) {
            QCOMPARE(visible.count(text), 1);
            QVERIFY(visible.indexOf(text) > previous);
            previous = visible.indexOf(text);
        }
        QVERIFY(captureProgressFixture(panel, QStringLiteral("03-completed")));
    }
    // 重新构造 controller 和面板，确保恢复的是已关闭重开的 SQLite 数据，非旧视图缓存。
    AgentController restored(std::make_unique<SilentModel>(), database);
    QVERIFY2(restored.openError().isEmpty(), qPrintable(restored.openError()));
    AgentChatPanel panel(restored);
    panel.resize(1000, 800);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    panel.openSession(session);
    auto* view = panel.findChild<MarkdownView*>();
    QVERIFY(view != nullptr);
    QCOMPARE(sequence(restored, session), allExpectedParts());
    QTRY_VERIFY_WITH_TIMEOUT(selectedDocumentText(*view).contains(expectedText[2]), 5000);
    const QString visible = selectedDocumentText(*view);
    for (int i = 0; i < expectedText.size(); ++i) {
        QCOMPARE(visible.count(expectedText[i]), 1);
        QVERIFY(view->contains(textPartIds[i]));
        QVERIFY(view->itemRect(textPartIds[i]).height() > 0);
        if (i > 0) QVERIFY(view->itemRect(textPartIds[i - 1]).bottom() <= view->itemRect(textPartIds[i]).top());
    }
    QWidget* firstGroup = progressToolGroup(panel, QStringLiteral("progress_probe_x"));
    QWidget* secondGroup = progressToolGroup(panel, QStringLiteral("progress_probe_y"));
    QVERIFY(firstGroup != nullptr && secondGroup != nullptr && firstGroup != secondGroup);
    const int firstTop = firstGroup->mapTo(view->viewport(), QPoint()).y() + view->verticalScrollBar()->value();
    const int secondTop = secondGroup->mapTo(view->viewport(), QPoint()).y() + view->verticalScrollBar()->value();
    QVERIFY(view->itemRect(textPartIds[0]).bottom() <= firstTop);
    QVERIFY(firstTop + firstGroup->height() <= view->itemRect(textPartIds[1]).top());
    QVERIFY(view->itemRect(textPartIds[1]).bottom() <= secondTop);
    QVERIFY(secondTop + secondGroup->height() <= view->itemRect(textPartIds[2]).top());
    QVERIFY(captureProgressFixture(panel, QStringLiteral("04-sqlite-restored")));
}

void AgentPanelSessionTest::consecutiveToolsShareDisclosureWithoutChangingReceipts() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto state = std::make_shared<IntermediateProgressState>();
    AgentController controller(std::make_unique<IntermediateProgressModel>(true), QString(),
                               [state](MaiToolRegistry& tools) {
                                   tools.add(std::make_unique<IntermediateProgressTool>(state, 1));
                                   tools.add(std::make_unique<IntermediateProgressTool>(state, 2));
                               });
    struct ReleaseOnExit {
        std::shared_ptr<IntermediateProgressState> state;
        ~ReleaseOnExit() {
            state->released.store(2);
        }
    } releaseOnExit{state};
    AgentChatPanel panel(controller);
    panel.resize(1000, 800);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    const QString session = controller.createSession(directory.path());
    panel.openSession(session);
    QVERIFY(controller.sendPrompt(session, QStringLiteral("Run adjacent local checks.")));
    QTRY_COMPARE_WITH_TIMEOUT(state->entered.load(), 1, 5000);
    state->released.store(1);
    QTRY_COMPARE_WITH_TIMEOUT(state->entered.load(), 2, 5000);
    QWidget* firstGroup = nullptr;
    QWidget* secondGroup = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((firstGroup = progressToolGroup(panel, QStringLiteral("progress_probe_x"))) != nullptr,
                             5000);
    QTRY_VERIFY_WITH_TIMEOUT((secondGroup = progressToolGroup(panel, QStringLiteral("progress_probe_y"))) != nullptr,
                             5000);
    QCOMPARE(firstGroup, secondGroup);
    auto* summary = firstGroup->findChild<QPushButton*>(QStringLiteral("agentToolGroupDisclosure"));
    QVERIFY(summary != nullptr);
    QVERIFY(summary->text().contains(QStringLiteral("工具调用 2 次")));
    state->released.store(2);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.agent().isBusy(session.toStdString()), 5000);
    QStringList receipts;
    for (const MaiMessage& message : controller.agent().listMessages(session.toStdString()))
        for (const MaiMessagePart& part : message.parts)
            if (const auto* tool = std::get_if<MaiToolPart>(&part.body)) {
                QCOMPARE(tool->state, MaiToolState::Completed);
                receipts.append(QString::fromStdString(tool->callId));
            }
    QCOMPARE(receipts, (QStringList{QStringLiteral("X"), QStringLiteral("Y")}));
}

void AgentPanelSessionTest::pastedImageUsesTheSharedComposerAndReachesTheModel() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString imagePath = directory.filePath(QStringLiteral("pasted.png"));
    QImage image(32, 24, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::blue);
    QVERIFY(image.save(imagePath, "PNG"));

    auto model = std::make_unique<CapturingModel>();
    CapturingModel* captured = model.get();
    AgentController controller(std::move(model), QString());
    AgentChatPanel panel(controller);
    panel.resize(700, 500);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    const QString sessionId = controller.createSession(directory.path());
    QVERIFY(controller.setModel(sessionId, QStringLiteral("glm-5.3-flash")));
    panel.openSession(sessionId);

    QTextEdit* editor = panel.findChild<QTextEdit*>();
    QVERIFY(editor != nullptr);
    auto* mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(imagePath)});
    QApplication::clipboard()->setMimeData(mime);
    editor->setFocus();
    QTest::keyClick(editor, Qt::Key_V, Qt::ControlModifier);
    QTRY_VERIFY(editor->toPlainText().contains(QChar(0xFFFC)));
    QVERIFY(QFile::remove(imagePath));
    const QString otherSession = controller.createSession(directory.path());
    panel.openSession(otherSession);
    QVERIFY(!editor->toPlainText().contains(QChar(0xFFFC)));
    panel.openSession(sessionId);
    QVERIFY(editor->toPlainText().contains(QChar(0xFFFC)));

    QPushButton* send = panel.findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    QVERIFY(send != nullptr);
    QSignalSpy finished(&controller, &AgentController::turnFinished);
    QTest::mouseClick(send, Qt::LeftButton);
    QVERIFY(finished.wait(15000));

    const MaiModelRequest request = captured->request();
    QVERIFY(!request.messages.empty());
    QCOMPARE(request.messages.back().images.size(), std::size_t(1));
    const QString importedPath = QString::fromStdString(request.messages.back().images.front().path);
    QVERIFY(importedPath != imagePath);
    QVERIFY(importedPath.startsWith(directory.path() + QStringLiteral("/ComposerMedia/")));
    QVERIFY(QFileInfo(importedPath).isFile());
    // 思考正文不再占用消息条目；附件与回复仍须可见。
    QVERIFY(panel.findChild<MarkdownView*>()->itemCount() >= 2);
    panel.openSession(sessionId);
    QVERIFY(panel.findChild<MarkdownView*>()->itemCount() >= 2);
}

void AgentPanelSessionTest::mediaImportDoesNotWaitForUnrelatedGlobalPoolWork() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString imagePath = directory.filePath(QStringLiteral("pool-blocked.png"));
    QImage image(32, 24, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::blue);
    QVERIFY(image.save(imagePath, "PNG"));

    auto model = std::make_unique<CapturingModel>();
    CapturingModel* captured = model.get();
    AgentController controller(std::move(model), QString());
    AgentChatPanel panel(controller);
    panel.resize(700, 500);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel, 1000));
    const QString sessionId = controller.createSession(directory.path());
    QVERIFY(controller.setModel(sessionId, QStringLiteral("glm-5.3-flash")));
    panel.openSession(sessionId);
    auto* editor = panel.findChild<QTextEdit*>(QStringLiteral("agentPromptEditor"));
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    auto* tray = panel.findChild<QScrollArea*>(QStringLiteral("agentMediaDraftTray"));
    QVERIFY(editor && send && tray);

    struct PoolGate {
        QSemaphore entered;
        QSemaphore release;
    };
    auto gate = std::make_shared<PoolGate>();
    QThreadPool* pool = QThreadPool::globalInstance();
    const int previousMax = pool->maxThreadCount();
    pool->setMaxThreadCount(1);
    const QFuture<void> blocker = QtConcurrent::run([gate] {
        gate->entered.release();
        gate->release.acquire();
    });
    // Declare after the panel so an assertion failure releases this test's own
    // worker before panel destruction queues cleanup. Never wait on unrelated
    // global work; restore the original pool capacity on every return path.
    struct RestoreGlobalPool {
        QThreadPool* pool;
        int previousMax;
        std::shared_ptr<PoolGate> gate;
        QFuture<void> blocker;
        ~RestoreGlobalPool() {
            gate->release.release();
            blocker.waitForFinished();
            pool->setMaxThreadCount(previousMax);
        }
    } restoreGlobalPool{pool, previousMax, gate, blocker};
    QVERIFY2(gate->entered.tryAcquire(1, 1000), "The owned global-pool blocker must start before pasting.");
    QVERIFY(!blocker.isFinished());

    auto* mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(imagePath)});
    QApplication::clipboard()->setMimeData(mime);
    editor->setFocus();
    QTest::keyClick(editor, Qt::Key_V, Qt::ControlModifier);
    qInfo().noquote() << "media-import global-pool after paste: pending=" << mediaImportPendingText(panel)
                      << "active=" << pool->activeThreadCount() << "max=" << pool->maxThreadCount();
    // A shared pool occupied by another long-running job must not leave a local
    // media copy pending. This controlled mechanism is not a live-incident claim.
    QTRY_VERIFY_WITH_TIMEOUT(editor->toPlainText().contains(QChar(0xFFFC)), 1000);
    const auto hasPendingImport = [tray] {
        for (const auto* label : tray->findChildren<QLabel*>())
            if (label->text().startsWith(QStringLiteral("正在导入媒体"))) return true;
        return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(!hasPendingImport(), 1000);
    QVERIFY(!blocker.isFinished());
    QVERIFY(send->isEnabled());
    QVERIFY(QFile::remove(imagePath));
    QSignalSpy finished(&controller, &AgentController::turnFinished);
    QTest::mouseClick(send, Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 1000);
    QVERIFY(!blocker.isFinished());
    const MaiModelRequest request = captured->request();
    QVERIFY(!request.messages.empty());
    QCOMPARE(request.messages.back().images.size(), std::size_t(1));
    const QString importedPath = QString::fromStdString(request.messages.back().images.front().path);
    QVERIFY(importedPath.startsWith(directory.path() + QStringLiteral("/ComposerMedia/")));
    QVERIFY(QFileInfo(importedPath).isFile());
}

void AgentPanelSessionTest::cancelingMediaImportKeepsTextAndIgnoresLateCompletion() {
    verifyMediaImportTermination(MediaImportTermination::Cancel);
}

void AgentPanelSessionTest::mediaImportDeadlineReleasesTheComposer() {
    verifyMediaImportTermination(MediaImportTermination::Deadline);
}

void AgentPanelSessionTest::cancelingCompletedImportThenDestroyingPanelCleansOwnedCopy() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString imagePath = directory.filePath(QStringLiteral("completed-before-cancel.png"));
    QImage image(32, 24, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::blue);
    QVERIFY(image.save(imagePath, "PNG"));
    AgentController controller(std::make_unique<SilentModel>(), QString());
    auto panel = std::make_unique<AgentChatPanel>(controller);
    panel->resize(700, 500);
    panel->show();
    QVERIFY(QTest::qWaitForWindowExposed(panel.get(), 1000));
    panel->openSession(controller.createSession(directory.path()));
    auto* editor = panel->findChild<QTextEdit*>(QStringLiteral("agentPromptEditor"));
    QVERIFY(editor);
    auto* mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(imagePath)});
    QApplication::clipboard()->setMimeData(mime);
    editor->paste();
    auto* watcher = panel->findChild<QFutureWatcherBase*>(QStringLiteral("agentMediaImportWatcher"));
    auto* cancel = panel->findChild<QPushButton*>(QStringLiteral("agentCancelMediaImports"));
    QVERIFY(watcher && cancel && cancel->isEnabled());
    QSignalSpy deliveredFinished(watcher, &QFutureWatcherBase::finished);
    QElapsedTimer elapsed;
    elapsed.start();
    // Do not pump GUI events or call waitForFinished(): the copy must physically
    // finish on its worker while its GUI completion is still queued. Waiting on
    // the future could instead steal that worker task into this GUI thread.
    while (!watcher->isFinished() && elapsed.elapsed() < 1000)
        QTest::qSleep(5);
    QVERIFY2(watcher->isFinished(), "The owned copy must complete before canceling the queued GUI result.");
    QCOMPARE(deliveredFinished.count(), 0);
    QVERIFY(!editor->toPlainText().contains(QChar(0xFFFC)));
    const QString storage = directory.filePath(QStringLiteral("ComposerMedia"));
    const QStringList importedDirectories = QDir(storage).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QCOMPARE(importedDirectories.size(), 1);
    const QString ownedDirectory = QDir(storage).filePath(importedDirectories.front());
    QVERIFY(QFileInfo(QDir(ownedDirectory).filePath(QFileInfo(imagePath).fileName())).isFile());
    // Cancel and destroy without an intervening event loop. Cleanup must not
    // depend on the panel receiving the already-queued watcher completion.
    cancel->click();
    panel.reset();
    QVERIFY(QFileInfo(imagePath).isFile());
    QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(ownedDirectory), 1000);
    QVERIFY(QFileInfo(imagePath).isFile());
}

void AgentPanelSessionTest::pastedVideoDraftSurvivesSessionSwitchAndReachesModel() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString videoPath = directory.filePath(QStringLiteral("clip sample.mp4"));
    QVERIFY(QFile::copy(QStringLiteral(MAICHAT_AGENT_VIDEO_FIXTURE), videoPath));
    auto model = std::make_unique<CapturingModel>();
    CapturingModel* captured = model.get();
    AgentController controller(std::move(model), QString());
    AgentChatPanel panel(controller);
    panel.resize(900, 700);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    const QString sessionId = controller.createSession(directory.path());
    panel.openSession(sessionId);
    auto* editor = panel.findChild<QTextEdit*>(QStringLiteral("agentPromptEditor"));
    QVERIFY(editor);
    auto* attach = panel.findChild<QPushButton*>(QStringLiteral("agentAttachMediaButton"));
    QVERIFY(attach);
    QVERIFY(attach->menu());
    const auto actions = attach->menu()->actions();
    QVERIFY(std::any_of(actions.cbegin(), actions.cend(),
                        [](const QAction* action) { return action->text() == QStringLiteral("打开摄像头拍照"); }));
    QVERIFY(std::any_of(actions.cbegin(), actions.cend(),
                        [](const QAction* action) { return action->text() == QStringLiteral("播放本地音乐"); }));
    auto* mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(videoPath)});
    QApplication::clipboard()->setMimeData(mime);
    editor->setFocus();
    QTest::keyClick(editor, Qt::Key_V, Qt::ControlModifier);
    QTRY_VERIFY(editor->toPlainText().contains(QChar(0xFFFC)));
    QCOMPARE(editor->placeholderText(), QStringLiteral("添加说明（可选）"));
    editor->moveCursor(QTextCursor::End);
    editor->insertPlainText(QStringLiteral("\n请处理这个视频"));
    const QString other = controller.createSession(directory.path());
    panel.openSession(other);
    QVERIFY(!editor->toPlainText().contains(QChar(0xFFFC)));
    panel.openSession(sessionId);
    QVERIFY(editor->toPlainText().contains(QChar(0xFFFC)));
    QVERIFY(editor->toPlainText().contains(QStringLiteral("请处理这个视频")));
    auto* tray = panel.findChild<QScrollArea*>(QStringLiteral("agentMediaDraftTray"));
    QVERIFY(tray);
    QTRY_VERIFY(tray->isVisible());
    QTRY_VERIFY(editor->mapTo(&panel, QPoint(0, editor->height())).y() <= panel.height());
    QString previewed;
    panel.setOpenVideoCallback([&previewed](const QString& path, const QStringList&) { previewed = path; });
    auto* preview = tray->findChild<QPushButton*>(QStringLiteral("agentDraftVideoPreview"));
    QVERIFY(preview);
    QTest::mouseClick(preview, Qt::LeftButton);
    QVERIFY(previewed != videoPath);
    QVERIFY(QFileInfo(previewed).isFile());
    auto* remove = tray->findChild<QPushButton*>(QStringLiteral("agentRemoveDraftMedia"));
    QVERIFY(remove);
    QTest::mouseClick(remove, Qt::LeftButton);
    QTRY_VERIFY(!tray->isVisible());
    QVERIFY(!editor->toPlainText().contains(QChar(0xFFFC)));
    QTRY_VERIFY(!QFileInfo(previewed).exists());
    editor->setFocus();
    QTest::keyClick(editor, Qt::Key_V, Qt::ControlModifier);
    QTRY_VERIFY(tray->isVisible());
    QTRY_VERIFY(editor->toPlainText().contains(QChar(0xFFFC)));
    QVERIFY(QFile::remove(videoPath));
    const QByteArray capture = qgetenv("MAICHAT_TEST_CAPTURE_PATH");
    if (!capture.isEmpty()) {
        QTest::qWait(700);
        QVERIFY(panel.grab().save(QString::fromUtf8(capture)));
    }
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    QVERIFY(send);
    QSignalSpy finished(&controller, &AgentController::turnFinished);
    QTest::mouseClick(send, Qt::LeftButton);
    QVERIFY(finished.wait(15000));
    const MaiModelRequest request = captured->request();
    QVERIFY(!request.messages.empty());
    QVERIFY(QString::fromStdString(request.messages.back().content).contains(QStringLiteral("请处理这个视频")));
    const auto messages = controller.agent().listMessages(sessionId.toStdString());
    QString storedPath;
    for (const MaiMessage& message : messages)
        for (const MaiMessagePart& part : message.parts)
            if (const auto* video = std::get_if<MaiVideoPart>(&part.body))
                storedPath = QString::fromStdString(video->path);
    QVERIFY(!storedPath.isEmpty());
    QVERIFY(storedPath != videoPath);
    QVERIFY(storedPath.startsWith(directory.path() + QStringLiteral("/ComposerMedia/")));
    QVERIFY(QFileInfo(storedPath).isFile());
    QVERIFY(QString::fromStdString(request.messages.back().content).contains(storedPath));
    QVERIFY(panel.findChild<QPushButton*>(QStringLiteral("agentUserVideoPlay")));
    panel.openSession(sessionId);
    QVERIFY(panel.findChild<QPushButton*>(QStringLiteral("agentUserVideoPlay")));
    panel.openSession(other);
    QVERIFY(QFile::copy(QStringLiteral(MAICHAT_AGENT_VIDEO_FIXTURE), videoPath));
    editor->setFocus();
    QTest::keyClick(editor, Qt::Key_V, Qt::ControlModifier);
    QTRY_VERIFY(tray->isVisible());
    QTRY_VERIFY(editor->toPlainText().contains(QChar(0xFFFC)));
    QSignalSpy mediaOnlyFinished(&controller, &AgentController::turnFinished);
    QTest::mouseClick(send, Qt::LeftButton);
    QVERIFY(mediaOnlyFinished.wait(15000));
    bool mediaOnly = false;
    for (const MaiMessage& message : controller.agent().listMessages(other.toStdString())) {
        if (message.role != MaiRole::User || !message.text().empty()) continue;
        for (const MaiMessagePart& part : message.parts)
            if (std::get_if<MaiVideoPart>(&part.body)) mediaOnly = true;
    }
    QVERIFY(mediaOnly);
}

void AgentPanelSessionTest::videoImportFinishesInItsOriginalSession() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString videoPath = directory.filePath(QStringLiteral("large-video.mp4"));
    QVERIFY(QFile::copy(QStringLiteral(MAICHAT_AGENT_VIDEO_FIXTURE), videoPath));
    QFile large(videoPath);
    QVERIFY(large.open(QIODevice::ReadWrite));
    QVERIFY(large.resize(32 * 1024 * 1024));
    large.close();
    AgentController controller(std::make_unique<SilentModel>(), QString());
    AgentChatPanel panel(controller);
    panel.resize(900, 700);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    const QString first = controller.createSession(directory.path());
    const QString second = controller.createSession(directory.path());
    panel.openSession(first);
    auto* editor = panel.findChild<QTextEdit*>(QStringLiteral("agentPromptEditor"));
    QVERIFY(editor);
    auto* mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(videoPath)});
    QApplication::clipboard()->setMimeData(mime);
    QElapsedTimer elapsed;
    elapsed.start();
    editor->setFocus();
    QTest::keyClick(editor, Qt::Key_V, Qt::ControlModifier);
    QVERIFY2(elapsed.elapsed() < 1000, "Import must not copy a large video on the UI thread");
    panel.openSession(second);
    QCOMPARE(panel.sessionId(), second);
    QVERIFY(!editor->toPlainText().contains(QChar(0xFFFC)));
    QTRY_VERIFY_WITH_TIMEOUT(
        QDir(directory.filePath(QStringLiteral("ComposerMedia"))).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size() ==
            1,
        15000);
    QTest::qWait(100);
    panel.openSession(first);
    QTRY_VERIFY_WITH_TIMEOUT(editor->toPlainText().contains(QChar(0xFFFC)), 15000);
    QVERIFY(!panel.findChild<QScrollArea*>(QStringLiteral("agentMediaDraftTray"))->isHidden());
}

void AgentPanelSessionTest::unconfiguredModelOpensConfigurationInsteadOfFailingTurn() {
    Harness harness;
    QVERIFY(QTest::qWaitForWindowExposed(harness.panel.get()));
    harness.panel->setModelLabel(QStringLiteral("未配置模型"));
    QSignalSpy requested(harness.panel.get(), &AgentChatPanel::modelConfigurationRequested);
    auto* editor = harness.panel->findChild<QTextEdit*>();
    QVERIFY(editor != nullptr);
    editor->setPlainText(QStringLiteral("你好"));
    QPushButton* send = harness.sendButton();
    QVERIFY(send != nullptr);
    QTest::mouseClick(send, Qt::LeftButton);

    QCOMPARE(requested.count(), 1);
    QCOMPARE(editor->toPlainText(), QStringLiteral("你好"));
}

void AgentPanelSessionTest::desktopAgentSuppliesMarkdownSystemPrompt() {
    auto model = std::make_unique<CapturingModel>();
    CapturingModel* observer = model.get();
    AgentController controller(std::move(model), QString());
    const QString sessionId = controller.createSession(QDir::currentPath());
    QVERIFY(controller.sendPrompt(sessionId, QStringLiteral("show repositories")));
    controller.agent().waitIdle();

    const MaiModelRequest request = observer->request();
    QVERIFY(!request.messages.empty());
    QCOMPARE(request.messages.front().role, MaiModelRole::User);
    const QString prompt = QString::fromStdString(request.baseInstructions);
    QVERIFY(prompt.contains(QStringLiteral("GitHub Flavored Markdown")));
    QVERIFY(prompt.contains(QStringLiteral("table row")));
    QVERIFY(prompt.contains(QStringLiteral("delimiter row")));
    QVERIFY(prompt.contains(QStringLiteral("ask the user to choose")));
    QVERIFY(prompt.contains(QStringLiteral("A model choice is")));
    QVERIFY(prompt.contains(QStringLiteral("Classify an input rejection")));
    QVERIFY(prompt.contains(QStringLiteral("FFmpeg/OpenCV edit")));
    QVERIFY(prompt.contains(QStringLiteral("FFmpeg oil-paint-style derivative")));
    QVERIFY(prompt.contains(QStringLiteral("explicitly forbids a recognizable real-person")));
}

void AgentPanelSessionTest::composerMatchesImLayoutAndUsesEmbeddedSendAction() {
    Harness harness;
    harness.panel->resize(1200, 700);
    QVERIFY(QTest::qWaitForWindowExposed(harness.panel.get()));

    auto* editor = harness.panel->findChild<ComposerTextEdit*>();
    auto* card = harness.panel->findChild<QWidget*>(QStringLiteral("agentComposerCard"));
    auto* directory = harness.panel->findChild<QLabel*>(QStringLiteral("agentWorkingDirectory"));
    auto* policy = harness.panel->findChild<QPushButton*>(QStringLiteral("agentApprovalPolicy"));
    auto* model = harness.panel->findChild<QPushButton*>(QStringLiteral("agentModelChip"));
    auto* more = harness.panel->findChild<QPushButton*>(QStringLiteral("agentMoreActions"));
    auto* send = harness.panel->findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    QVERIFY(editor != nullptr);
    QVERIFY(policy != nullptr);
    QVERIFY(model != nullptr);
    QVERIFY(more != nullptr);
    QVERIFY(send != nullptr);
    QCOMPARE(card, nullptr);
    QCOMPARE(directory, nullptr);
    QCOMPARE(send->parentWidget(), editor);
    QCOMPARE(send->text(), QString());
    QVERIFY(!send->icon().isNull());
    QCOMPARE(send->size(), QSize(UiZoom::s(36), UiZoom::s(36)));
    QVERIFY(editor->rect().contains(send->geometry().topLeft()));
    QVERIFY(editor->rect().contains(send->geometry().bottomRight()));
    QVERIFY(harness.panel->findChild<QPushButton*>(QStringLiteral("agentAttachImage")) == nullptr);
    QVERIFY(editor->width() >= harness.panel->width() - UiZoom::s(60));
    QVERIFY(editor->styleSheet().contains(QStringLiteral("border:1px solid")));
    QVERIFY(!editor->isAncestorOf(model));
    QVERIFY(!editor->isAncestorOf(policy));
    QVERIFY(more->menu() == nullptr);
    QVERIFY(!model->isVisible());
    QVERIFY(!policy->isVisible());
    auto* actions = harness.panel->findChild<QWidget*>(QStringLiteral("agentActionPanel"));
    QVERIFY(actions != nullptr);
    QVERIFY(actions->findChild<QPushButton*>(QStringLiteral("agentEffortSection")) != nullptr);
    QVERIFY(actions->findChild<QPushButton*>(QStringLiteral("agentPolicySection")) != nullptr);
    QVERIFY(actions->findChild<QPushButton*>(QStringLiteral("agentClearConversation")) != nullptr);
    QVERIFY(policy->menu() != nullptr);
    QVERIFY(policy->menu()->styleSheet().contains(QStringLiteral("QMenu::item:selected")));
    QCOMPARE(policy->menu()->actions().size(), 3);
    QCOMPARE(policy->menu()->actions()[0]->text(), QStringLiteral("请求批准"));
    QCOMPARE(policy->menu()->actions()[1]->text(), QStringLiteral("帮我批准"));
    QCOMPARE(policy->menu()->actions()[2]->text(), QStringLiteral("完全访问"));
}

void AgentPanelSessionTest::composerTextMimeUsesOnlyLiteralText_data() {
    QTest::addColumn<QString>("format");
    for (const char* format : {"plain", "html", "markdown", "qrichtext", "markdown-only", "html-only"})
        QTest::newRow(format) << QString::fromLatin1(format);
}

void AgentPanelSessionTest::composerTextMimeUsesOnlyLiteralText() {
    QFETCH(QString, format);
    MimeComposer editor;
    QMimeData mime;
    const QString text = ownedComposerText();
    // text/markdown 排第一时，Qt 6.12 默认解析分支会绕过 acceptRichText。
    if (format.startsWith(QStringLiteral("markdown"))) mime.setData("text/markdown", text.toUtf8());
    if (format == QStringLiteral("html") || format == QStringLiteral("html-only")) mime.setHtml(ownedComposerHtml());
    if (format == QStringLiteral("qrichtext")) mime.setData("application/x-qrichtext", ownedComposerHtml().toUtf8());
    if (!format.endsWith(QStringLiteral("-only"))) mime.setText(text);
    editor.insertFromMimeData(&mime);
    const QString expected = format == QStringLiteral("html-only") ? QString() : text;
    QCOMPARE(editor.toPlainText(), expected);
    QCOMPARE(editor.canInsertFromMimeData(&mime), format != QStringLiteral("html-only"));
    for (QTextBlock block = editor.document()->begin(); block.isValid(); block = block.next()) {
        QCOMPARE(block.blockFormat().background().style(), Qt::NoBrush);
        for (auto fragment = block.begin(); !fragment.atEnd(); ++fragment) {
            const QTextCharFormat style = fragment.fragment().charFormat();
            QCOMPARE(style.background().style(), Qt::NoBrush);
            QVERIFY(!style.isAnchor());
            QVERIFY(!style.font().bold());
            QVERIFY(style.foreground().color() != QColor(QStringLiteral("#c5c5c5")));
        }
    }
    if (!expected.isEmpty()) {
        editor.undo();
        QVERIFY(editor.toPlainText().isEmpty());
        editor.redo();
        QCOMPARE(editor.toPlainText(), expected);
    }
}

void AgentPanelSessionTest::composerMimeHandlerAndInlineImagesKeepTheirExistingPath() {
    MimeComposer editor;
    editor.setPlainText(QStringLiteral("existing draft"));
    QImage image(4, 3, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::blue);
    QMimeData mime;
    mime.setImageData(image);
    mime.setHtml(ownedComposerHtml());
    int calls = 0;
    bool originalMime = false;
    QImage handledImage;
    editor.setMimeHandler([&](const QMimeData* original) {
        ++calls;
        originalMime = original == &mime;
        handledImage = qvariant_cast<QImage>(original->imageData());
        return true;
    });
    QVERIFY(editor.canInsertFromMimeData(&mime));
    QCOMPARE(calls, 0); // 可插入性查询不能调用有副作用的业务 handler。
    editor.insertFromMimeData(&mime);
    QCOMPARE(calls, 1);
    QVERIFY(originalMime);
    QCOMPARE(handledImage, image);
    QCOMPARE(editor.toPlainText(), QStringLiteral("existing draft"));
    mime.setText(QStringLiteral("ALT TEXT MUST NOT LEAK"));
    editor.insertFromMimeData(&mime);
    QCOMPARE(calls, 2);
    QCOMPARE(editor.toPlainText(), QStringLiteral("existing draft"));
    // 纯文本粘贴策略不能禁止业务层用 QTextCursor 插入内联图片。
    editor.document()->addResource(QTextDocument::ImageResource, QUrl(QStringLiteral("owned-image")), image);
    QTextCursor cursor = editor.textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertImage(QStringLiteral("owned-image"));
    QVERIFY(editor.toPlainText().contains(QChar(0xFFFC)));
}

void AgentPanelSessionTest::normalAgentComposerDropsSourceFormattingAndSendsLiteralText() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto model = std::make_unique<CapturingModel>();
    auto* captured = model.get();
    AgentController controller(std::move(model), QString());
    AgentChatPanel panel(controller);
    panel.resize(900, 600);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    const QString session = controller.createSession(directory.path());
    panel.openSession(session);
    auto* editor = panel.findChild<ComposerTextEdit*>(QStringLiteral("agentPromptEditor"));
    QVERIFY(editor);
    QMimeData mime;
    mime.setHtml(ownedComposerHtml());
    mime.setText(ownedComposerText());
    dropOwnedComposerMime(*editor, mime);
    QCOMPARE(editor->toPlainText(), ownedComposerText());
    QVERIFY(captureAgentFixture(panel, QStringLiteral("composer-owned-source-formatting")));
    QVERIFY(!editor->acceptRichText());
    QSignalSpy finished(&controller, &AgentController::turnFinished);
    QTest::mouseClick(panel.findChild<QPushButton*>(QStringLiteral("agentSendButton")), Qt::LeftButton);
    QVERIFY(finished.wait(15000));
    const auto request = captured->request();
    QVERIFY(!request.messages.empty());
    QCOMPARE(QString::fromStdString(request.messages.back().content), ownedComposerText());
}

void AgentPanelSessionTest::conversationAndComposerUseResizableSplitter() {
    Harness harness;
    harness.panel->resize(1200, 800);
    QVERIFY(QTest::qWaitForWindowExposed(harness.panel.get()));

    auto* splitter = harness.panel->findChild<QSplitter*>(QStringLiteral("agentMessageComposerSplitter"));
    QVERIFY(splitter != nullptr);
    QCOMPARE(splitter->orientation(), Qt::Vertical);
    QCOMPARE(splitter->count(), 2);
    QVERIFY(!splitter->childrenCollapsible());
    QCOMPARE(splitter->handleWidth(), 1);

    auto* editor = harness.panel->findChild<ComposerTextEdit*>();
    QVERIFY(editor != nullptr);
    QCOMPARE(editor->minimumHeight(), UiZoom::s(96));
    QVERIFY(editor->maximumHeight() > UiZoom::s(112));

    QSplitterHandle* handle = splitter->handle(1);
    QVERIFY(handle != nullptr);
    const int before = splitter->sizes().at(0);
    const int editorBefore = editor->height();
    const QPoint start = handle->rect().center();
    const QPoint finish = start + QPoint(0, -60);
    QTest::mousePress(handle, Qt::LeftButton, Qt::NoModifier, start);
    QMouseEvent move(QEvent::MouseMove, QPointF(finish), QPointF(handle->mapToGlobal(finish)), Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(handle, &move);
    QTest::mouseRelease(handle, Qt::LeftButton, Qt::NoModifier, finish);
    QVERIFY(splitter->sizes().at(0) != before);
    QVERIFY(editor->height() > editorBefore);
}

void AgentPanelSessionTest::composerResizeHandleAlignsWithEditorBorder() {
    Harness harness;
    harness.panel->resize(1200, 800);
    QVERIFY(QTest::qWaitForWindowExposed(harness.panel.get()));
    auto* splitter = harness.panel->findChild<QSplitter*>(QStringLiteral("agentMessageComposerSplitter"));
    auto* editor = harness.panel->findChild<ComposerTextEdit*>();
    QVERIFY(splitter != nullptr);
    QVERIFY(editor != nullptr);
    QSplitterHandle* handle = splitter->handle(1);
    QVERIFY(handle != nullptr);

    const QRect handleOnScreen(handle->mapToGlobal(QPoint(0, 0)), handle->size());
    const QRect editorOnScreen(editor->mapToGlobal(QPoint(0, 0)), editor->size());
    QCOMPARE(editorOnScreen.top(), handleOnScreen.bottom() + 1);
    QCOMPARE(editorOnScreen.center().x(), handleOnScreen.center().x());
}

void AgentPanelSessionTest::composerPlaceholderFitsAtMinimumHeight() {
    Harness harness;
    harness.panel->resize(1200, 700);
    QVERIFY(QTest::qWaitForWindowExposed(harness.panel.get()));
    auto* splitter = harness.panel->findChild<QSplitter*>(QStringLiteral("agentMessageComposerSplitter"));
    auto* editor = harness.panel->findChild<ComposerTextEdit*>();
    QVERIFY(splitter != nullptr);
    QVERIFY(editor != nullptr);
    splitter->setSizes({10000, 0});
    QApplication::processEvents();

    const qreal requiredHeight = editor->fontMetrics().lineSpacing() + 2 * editor->document()->documentMargin();
    QVERIFY2(editor->viewport()->height() >= requiredHeight,
             qPrintable(QStringLiteral("placeholder viewport %1px, one line needs %2px")
                            .arg(editor->viewport()->height())
                            .arg(requiredHeight)));
    QVERIFY2(!editor->verticalScrollBar()->isVisible(), "An empty composer must not expose a scroll bar or its corner");
    QVERIFY2(!editor->horizontalScrollBar()->isVisible(),
             "An empty composer must not expose a horizontal scroll bar corner");
}

void AgentPanelSessionTest::conversationUsesAvailableWidth() {
    Harness harness;
    harness.panel->resize(1200, 700);
    QVERIFY(QTest::qWaitForWindowExposed(harness.panel.get()));

    MarkdownView* view = harness.view();
    QVERIFY(view != nullptr);
    view->addItem(QStringLiteral("wide-document"), MarkdownView::Style::Document,
                  QStringLiteral("内容应该使用窗口可用宽度"));
    QTRY_COMPARE(qRound(view->itemRect(QStringLiteral("wide-document")).left()), UiZoom::s(48));
    QCOMPARE(qRound(view->viewport()->width() - view->itemRect(QStringLiteral("wide-document")).right()),
             UiZoom::s(48));
}

void AgentPanelSessionTest::historyOpensLatestPageAndPrependsWithoutMovingReadingAnchor() {
    QTemporaryDir storage;
    QVERIFY(storage.isValid());
    const QString database = storage.filePath(QStringLiteral("paged-history.db"));
    const std::string sessionId = "ses_paged_fixture";
    {
        auto opened = makeMaiSqliteStore(database.toUtf8().toStdString());
        QVERIFY(opened);
        auto store = std::move(opened.value());
        MaiSession session;
        session.id = sessionId;
        session.directory = storage.path().toUtf8().toStdString();
        session.created = session.updated = 1;
        store->putSession(session);
        for (int i = 0; i < 121; ++i) {
            MaiMessage message;
            message.id = QStringLiteral("msg_page_%1").arg(i, 4, 10, QLatin1Char('0')).toStdString();
            message.created = i + 1;
            message.completed = i + 1;
            message.parts.push_back(
                {"prt_" + message.id,
                 MaiTextPart{"Fixture history line " + std::to_string(i) + "\nMore fixture lines\nThird fixture line"},
                 i + 1});
            store->putMessage(sessionId, message);
        }
        QVERIFY(!store->lastWriteError().hasError());
    }
    AgentController controller(std::make_unique<SilentModel>(), database);
    AgentChatPanel panel(controller);
    panel.resize(1000, 700);
    panel.show();
    panel.openSession(QString::fromStdString(sessionId));
    auto* view = panel.findChild<MarkdownView*>();
    QVERIFY(view);
    QVERIFY(view->contains(QStringLiteral("msg_page_0120")));
    QVERIFY(!view->contains(QStringLiteral("msg_page_0080")));
    QCOMPARE(panel.findChildren<QPushButton*>(QStringLiteral("agentQuoteAction")).size(), 40);
    auto* older = panel.findChild<QPushButton*>(QStringLiteral("agentLoadEarlierButton"));
    QVERIFY(older && !older->isHidden());
    QCoreApplication::processEvents();
    auto* bar = view->verticalScrollBar();
    const QString anchor = QStringLiteral("msg_page_0090");
    bar->setValue(qMax(0, int(view->itemRect(anchor).top()) - 50));
    const qreal viewportOffset = view->itemRect(anchor).top() - bar->value();
    QPointer<QPushButton> existingFooter;
    for (QPushButton* quote : panel.findChildren<QPushButton*>(QStringLiteral("agentQuoteAction")))
        if (quote->property("messageId").toString() == anchor) existingFooter = quote;
    QVERIFY(existingFooter);
    older->click();
    QTRY_VERIFY(view->contains(QStringLiteral("msg_page_0041")));
    QVERIFY(!view->contains(QStringLiteral("msg_page_0040")));
    QCOMPARE(panel.findChildren<QPushButton*>(QStringLiteral("agentQuoteAction")).size(), 80);
    QTRY_VERIFY(qAbs(view->itemRect(anchor).top() - bar->value() - viewportOffset) <= 2.0);
    QVERIFY(existingFooter);  // Prepending must not recreate existing quote controls.
    older->click();
    QTRY_VERIFY(view->contains(QStringLiteral("msg_page_0001")));
    older->click();
    QTRY_VERIFY(view->contains(QStringLiteral("msg_page_0000")));
    QVERIFY(older->isHidden());
    QCOMPARE(panel.findChildren<QPushButton*>(QStringLiteral("agentQuoteAction")).size(), 121);
}

// A rich latest page is bounded by messages, yet contains hundreds of tool
// controls. Revisiting it must retain those controls instead of recreating them.
void AgentPanelSessionTest::sessionSwitchRetainsUnchangedRenderedMessages() {
    QTemporaryDir storage;
    QVERIFY(storage.isValid());
    const QString database = storage.filePath(QStringLiteral("switch-history.db"));
    const QStringList sessions{QStringLiteral("ses_switch_a"), QStringLiteral("ses_switch_b")};
    {
        auto opened = makeMaiSqliteStore(database.toStdString());
        QVERIFY(opened);
        auto store = std::move(opened.value());
        for (const QString& id : sessions) {
            MaiSession session;
            session.id = id.toStdString();
            session.directory = storage.path().toStdString();
            session.created = session.updated = 1;
            store->putSession(session);
            for (int index = 0; index < 80; ++index) {
                MaiMessage message;
                message.id = QStringLiteral("msg_%1_%2").arg(id).arg(index, 4, 10, QLatin1Char('0')).toStdString();
                message.role = index % 2 == 0 ? MaiRole::User : MaiRole::Assistant;
                message.created = message.completed = index + 1;
                const QString text =
                    message.role == MaiRole::User
                        ? QStringLiteral("Fixture request %1").arg(index)
                        : QStringLiteral("## Fixture result %1\n\n").arg(index) +
                              QStringLiteral("The stable result contains **formatted text**, `code`, and a table.\n\n")
                                  .repeated(8) +
                              QStringLiteral(
                                  "| Check | Result |\n| --- | --- |\n| Desktop | Ready |\n| Shared core | Ready "
                                  "|\n\n") +
                              QStringLiteral(
                                  "```cpp\nfor (int item = 0; item < 10; ++item) {\n    consume(item);\n}\n```\n");
                message.parts.push_back({"prt_" + message.id, MaiTextPart{text.toStdString()}, index + 1});
                if (message.role == MaiRole::Assistant) {
                    for (int toolIndex = 0; toolIndex < 12; ++toolIndex) {
                        MaiToolPart tool;
                        tool.callId = "call_" + message.id + "_" + std::to_string(toolIndex);
                        tool.tool = "shell";
                        tool.input = R"({"command":"fixture only"})";
                        tool.output = std::string(4096, 'x');
                        tool.state = MaiToolState::Completed;
                        message.parts.push_back({tool.callId, std::move(tool), index + 1});
                    }
                }
                store->putMessage(session.id, message);
            }
        }
        QVERIFY(!store->lastWriteError().hasError());
    }
    AgentController controller(std::make_unique<SilentModel>(), database);
    AgentChatPanel panel(controller);
    panel.resize(1100, 750);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    QElapsedTimer timer;
    timer.start();
    const auto page = controller.agent().listMessagesPage(sessions[0].toStdString(), {}, 40);
    const qint64 readUs = timer.nsecsElapsed() / 1000;
    QCOMPARE(page.size(), std::size_t(40));
    timer.restart();
    panel.openSession(sessions[0]);
    const qint64 coldUs = timer.nsecsElapsed() / 1000;
    QCoreApplication::processEvents();
    const qint64 coldPaintUs = timer.nsecsElapsed() / 1000;
    QPointer<QPushButton> original;
    for (QPushButton* button : panel.findChildren<QPushButton*>(QStringLiteral("agentQuoteAction")))
        if (button->property("messageId").toString() == QStringLiteral("msg_ses_switch_a_0079")) original = button;
    QVERIFY(original);
    panel.openSession(sessions[1]);
    QCoreApplication::processEvents();
    timer.restart();
    panel.openSession(sessions[0]);
    const qint64 revisitUs = timer.nsecsElapsed() / 1000;
    QCoreApplication::processEvents();
    const qint64 revisitPaintUs = timer.nsecsElapsed() / 1000;
    qInfo("switch_fixture read_us=%lld cold_us=%lld cold_paint_us=%lld revisit_us=%lld revisit_paint_us=%lld", readUs,
          coldUs, coldPaintUs, revisitUs, revisitPaintUs);
    QVERIFY2(original, "Switching to another session destroyed the unchanged rendered message");
    QVERIFY(original->isVisible());
    QVERIFY(captureAgentFixture(panel, QStringLiteral("session-switch-retained-rich-page")));
}

void AgentPanelSessionTest::cachedSessionAddsBackgroundMessagesWithoutReplacingHistory() {
    AgentController controller(std::make_unique<SilentModel>(), QString());
    const QString first = controller.createSession(QDir::currentPath());
    const QString second = controller.createSession(QDir::currentPath());
    QVERIFY(controller.sendPrompt(first, QStringLiteral("Stable history fixture")));
    controller.agent().waitIdle();
    AgentChatPanel panel(controller);
    panel.resize(850, 600);
    panel.show();
    panel.openSession(first);
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    QPointer<QPushButton> original;
    for (QPushButton* quote : panel.findChildren<QPushButton*>(QStringLiteral("agentQuoteAction")))
        if (quote->isVisible()) original = quote;
    QVERIFY(original);
    panel.openSession(second);
    QVERIFY(controller.sendPrompt(first, QStringLiteral("Message received while another page was visible")));
    controller.agent().waitIdle();
    QCoreApplication::processEvents();
    panel.openSession(first);
    QCoreApplication::processEvents();
    QVERIFY(original && original->isVisible());
    auto* view = panel.findChild<MarkdownView*>();
    QVERIFY(view);
    view->selectAll();
    const QString text = view->selectedText();
    QVERIFY(text.contains(QStringLiteral("Stable history fixture")));
    QVERIFY(text.contains(QStringLiteral("Message received while another page was visible")));
    view->clearSelection();
}

void AgentPanelSessionTest::clearedCachedSessionDoesNotRestoreRemovedHistory() {
    AgentController controller(std::make_unique<SilentModel>(), QString());
    const QString first = controller.createSession(QDir::currentPath());
    const QString second = controller.createSession(QDir::currentPath());
    QVERIFY(controller.sendPrompt(first, QStringLiteral("History that will be cleared")));
    controller.agent().waitIdle();
    AgentChatPanel panel(controller);
    panel.resize(850, 600);
    panel.show();
    panel.openSession(first);
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    panel.openSession(second);
    QVERIFY(controller.agent().submit(MaiClearMessages{first.toStdString()}));
    panel.openSession(first);
    auto* view = panel.findChild<MarkdownView*>();
    auto* welcome = panel.findChild<QWidget*>(QStringLiteral("agentWelcomeView"));
    QVERIFY(view && welcome);
    QVERIFY(view->isEmpty());
    QVERIFY(welcome->isVisible());
}

void AgentPanelSessionTest::boundedSessionPagesPreserveComposerDrafts() {
    AgentController controller(std::make_unique<SilentModel>(), QString());
    QStringList sessions;
    for (int index = 0; index < 4; ++index) {
        sessions << controller.createSession(QDir::currentPath());
        QVERIFY(controller.sendPrompt(sessions.back(), QStringLiteral("Page fixture %1").arg(index)));
        controller.agent().waitIdle();
    }
    AgentChatPanel panel(controller);
    panel.resize(850, 600);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    auto* editor = panel.findChild<QTextEdit*>();
    QVERIFY(editor);
    const auto visibleQuote = [&panel]() -> QPushButton* {
        for (QPushButton* quote : panel.findChildren<QPushButton*>(QStringLiteral("agentQuoteAction")))
            if (quote->isVisible()) return quote;
        return nullptr;
    };
    panel.openSession(sessions[0]);
    QCoreApplication::processEvents();
    QPointer<QPushButton> firstQuote = visibleQuote();
    QVERIFY(firstQuote);
    editor->setPlainText(QStringLiteral("Draft belongs to the first page"));
    panel.openSession(sessions[1]);
    QCoreApplication::processEvents();
    QPointer<QPushButton> secondQuote = visibleQuote();
    QVERIFY(secondQuote);
    panel.openSession(sessions[2]);
    panel.openSession(sessions[0]);
    QCoreApplication::processEvents();
    QVERIFY(firstQuote && firstQuote->isVisible());
    QCOMPARE(editor->toPlainText(), QStringLiteral("Draft belongs to the first page"));
    panel.openSession(sessions[3]);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY2(secondQuote.isNull(), "The least recently used fourth page must release retained widgets");
    panel.openSession(sessions[0]);
    QCOMPARE(editor->toPlainText(), QStringLiteral("Draft belongs to the first page"));
    QVERIFY(firstQuote && firstQuote->isVisible());
}

void AgentPanelSessionTest::answeredCachedQuestionDoesNotReclaimComposer() {
    AgentController controller(std::make_unique<QuestionModel>(), QString());
    AgentChatPanel panel(controller);
    panel.resize(850, 600);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    const QString first = controller.createSession(QDir::currentPath());
    const QString second = controller.createSession(QDir::currentPath());
    panel.openSession(first);
    auto* editor = panel.findChild<QTextEdit*>();
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    QVERIFY(editor && send);
    editor->setPlainText(QStringLiteral("ask me"));
    send->click();
    QTRY_COMPARE(controller.pendingQuestions().size(), std::size_t(1));
    QTRY_VERIFY(panel.findChild<MarkdownView*>()->contains(
        QString::fromStdString(controller.pendingQuestions().front().partId) + QStringLiteral("-question")));
    QTRY_COMPARE(send->toolTip(), QStringLiteral("发送消息"));
    const auto question = controller.pendingQuestions().front();
    editor->setPlainText(QStringLiteral("Keep the answer draft"));
    panel.openSession(second);
    QVERIFY(controller.agent().submit(MaiReplyQuestion{question.id, "A"}));
    controller.agent().waitIdle();
    QCoreApplication::processEvents();
    panel.openSession(first);
    QCOMPARE(send->toolTip(), QStringLiteral("发送消息"));
    QCOMPARE(editor->toPlainText(), QStringLiteral("Keep the answer draft"));
    send->click();
    controller.agent().waitIdle();
    QCOMPARE(editor->toPlainText(), QString());
}

void AgentPanelSessionTest::emptyCachedSessionCanLoadEarlierBackgroundHistory() {
    AgentController controller(std::make_unique<SilentModel>(), QString());
    const QString first = controller.createSession(QDir::currentPath());
    const QString second = controller.createSession(QDir::currentPath());
    AgentChatPanel panel(controller);
    panel.resize(850, 600);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    panel.openSession(first);
    panel.openSession(second);
    for (int index = 0; index < 45; ++index) {
        QVERIFY(controller.sendPrompt(first, QStringLiteral("Deferred fixture number %1").arg(index)));
        controller.agent().waitIdle();
    }
    panel.openSession(first);
    auto* older = panel.findChild<QPushButton*>(QStringLiteral("agentLoadEarlierButton"));
    auto* view = panel.findChild<MarkdownView*>();
    QVERIFY(older && view);
    QVERIFY2(older->isVisible(), "A previously empty cached page must expose the older history received in background");
    older->click();
    QVERIFY(older->isVisible());
    older->click();
    QVERIFY(older->isHidden());
    view->selectAll();
    QVERIFY(view->selectedText().contains(QStringLiteral("Deferred fixture number 0")));
    view->clearSelection();
}

void AgentPanelSessionTest::changedCompletedMessageRefreshesItsCachedPage() {
    QTemporaryDir storage;
    QVERIFY(storage.isValid());
    const QString database = storage.filePath(QStringLiteral("changed-message.db"));
    const std::string sessionId = "ses_changed_complete";
    MaiMessage message;
    message.id = "msg_changed_complete";
    message.role = MaiRole::Assistant;
    message.created = message.completed = 1;
    message.parts.push_back({"prt_changed_complete", MaiTextPart{"Original completed result"}, 1});
    {
        auto opened = makeMaiSqliteStore(database.toStdString());
        QVERIFY(opened);
        auto store = std::move(opened.value());
        MaiSession session;
        session.id = sessionId;
        session.directory = storage.path().toStdString();
        session.created = session.updated = 1;
        store->putSession(session);
        store->putMessage(sessionId, message);
    }
    AgentController controller(std::make_unique<SilentModel>(), database);
    const QString second = controller.createSession(storage.path());
    AgentChatPanel panel(controller);
    panel.resize(850, 600);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    panel.openSession(QString::fromStdString(sessionId));
    panel.openSession(second);
    {
        auto opened = makeMaiSqliteStore(database.toStdString());
        QVERIFY(opened);
        auto store = std::move(opened.value());
        std::get<MaiTextPart>(message.parts.front().body).text = "Updated completed result";
        store->putMessage(sessionId, message);
        QVERIFY(!store->lastWriteError().hasError());
    }
    panel.openSession(QString::fromStdString(sessionId));
    auto* view = panel.findChild<MarkdownView*>();
    QVERIFY(view);
    view->selectAll();
    const QString text = view->selectedText();
    QVERIFY(text.contains(QStringLiteral("Updated completed result")));
    QVERIFY(!text.contains(QStringLiteral("Original completed result")));
    view->clearSelection();
}

void AgentPanelSessionTest::collapsedToolDetailsAreFormattedOnlyOnDisclosure() {
    QTemporaryDir storage;
    QVERIFY(storage.isValid());
    const QString database = storage.filePath(QStringLiteral("large-tools.db"));
    const std::string sessionId = "ses_tool_detail_fixture";
    {
        auto opened = makeMaiSqliteStore(database.toUtf8().toStdString());
        QVERIFY(opened);
        auto store = std::move(opened.value());
        MaiSession session;
        session.id = sessionId;
        session.directory = storage.path().toUtf8().toStdString();
        session.created = session.updated = 1;
        store->putSession(session);
        MaiMessage message;
        message.id = "msg_many_tools_fixture";
        message.role = MaiRole::Assistant;
        message.created = message.completed = 1;
        for (int i = 0; i < 55; ++i) {
            MaiToolPart tool;
            tool.tool = "read";
            tool.callId = "call_fixture_" + std::to_string(i);
            tool.input = "{\"path\":\"fixture.txt\"}";
            tool.output = "<fixture-literal>" + std::string(200000, 'x');
            tool.state = MaiToolState::Completed;
            message.parts.push_back({"prt_fixture_" + std::to_string(i), std::move(tool), i + 1});
        }
        store->putMessage(sessionId, message);
        QVERIFY(!store->lastWriteError().hasError());
    }
    AgentController controller(std::make_unique<SilentModel>(), database);
    AgentChatPanel panel(controller);
    panel.resize(1000, 700);
    panel.show();
    panel.openSession(QString::fromStdString(sessionId));
    const auto labels = panel.findChildren<QLabel*>(QStringLiteral("agentToolDetail"));
    QCOMPARE(labels.size(), 55);
    for (QLabel* detail : labels) {
        QVERIFY(detail->text().isEmpty());
        QCOMPARE(detail->textFormat(), Qt::PlainText);
    }
    auto* disclosure = labels.first()->parentWidget()->findChild<QPushButton*>(QStringLiteral("agentToolDisclosure"));
    QVERIFY(disclosure);
    disclosure->click();
    QVERIFY(labels.first()->text().contains(QStringLiteral("<fixture-literal>")));
    QVERIFY(labels.first()->text().size() < 12500);
    for (int i = 1; i < labels.size(); ++i) QVERIFY(labels[i]->text().isEmpty());
}

void AgentPanelSessionTest::busyComposerAcceptsOrderedSteeringWithoutStopping() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto state = std::make_shared<IntermediateProgressState>();
    AgentController controller(std::make_unique<IntermediateProgressModel>(),
                               directory.filePath(QStringLiteral("busy-input.sqlite")),
                               [state](MaiToolRegistry& tools) {
                                   tools.add(std::make_unique<IntermediateProgressTool>(state, 1));
                                   tools.add(std::make_unique<IntermediateProgressTool>(state, 2));
                               });
    struct ReleaseOnExit {
        std::shared_ptr<IntermediateProgressState> state;
        ~ReleaseOnExit() {
            state->released = 2;
        }
    } releaseOnExit{state};
    AgentChatPanel panel(controller);
    panel.resize(1000, 800);
    panel.show();
    const QString session = controller.createSession(directory.path(), QStringLiteral("Busy input fixture"));
    panel.openSession(session);
    auto* editor = panel.findChild<QTextEdit*>(QStringLiteral("agentPromptEditor"));
    QVERIFY(editor);
    editor->setPlainText(QStringLiteral("Initial fixture request."));
    QTest::keyClick(editor, Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(state->entered.load(), 1, 5000);
    const std::string turn = controller.agent().activeTurnId(session.toStdString());
    QVERIFY(!turn.empty());
    QVERIFY(editor->isEnabled() && !editor->isReadOnly());
    for (const QString& text : {QStringLiteral("Supplement B."), QStringLiteral("Supplement C.")}) {
        editor->setPlainText(text);
        QTest::keyClick(editor, Qt::Key_Return);
        QCOMPARE(editor->toPlainText(), QString());
    }
    QStringList users;
    for (const MaiMessage& message : controller.agent().listMessages(session.toStdString()))
        if (message.role == MaiRole::User) users.append(QString::fromStdString(message.text()));
    QCOMPARE(users, (QStringList{QStringLiteral("Initial fixture request."), QStringLiteral("Supplement B."),
                                 QStringLiteral("Supplement C.")}));
    QCOMPARE(controller.agent().activeTurnId(session.toStdString()), turn);
    QVERIFY(controller.agent().isBusy(session.toStdString()));
    QCOMPARE(state->released.load(), 0);
    QVERIFY(captureAgentFixture(panel, QStringLiteral("busy-ordered-input")));
    state->released = 2;
    QTRY_VERIFY_WITH_TIMEOUT(!controller.agent().isBusy(session.toStdString()), 5000);
}

void AgentPanelSessionTest::stopKeepsUnacceptedDraftAndInputEditable() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto state = std::make_shared<StoppingComposerState>();
    AgentController controller(
        std::make_unique<StoppingComposerModel>(), directory.filePath(QStringLiteral("stopping-input.sqlite")),
        [state](MaiToolRegistry& tools) { tools.add(std::make_unique<StoppingComposerTool>(state)); });
    struct ReleaseOnExit {
        std::shared_ptr<StoppingComposerState> state;
        ~ReleaseOnExit() {
            state->release = true;
        }
    } releaseOnExit{state};
    AgentChatPanel panel(controller);
    panel.resize(1000, 800);
    panel.show();
    const QString session = controller.createSession(directory.path(), QStringLiteral("Stopping input fixture"));
    panel.openSession(session);
    auto* editor = panel.findChild<QTextEdit*>(QStringLiteral("agentPromptEditor"));
    QVERIFY(editor);
    editor->setPlainText(QStringLiteral("Initial fixture request."));
    QTest::keyClick(editor, Qt::Key_Return);
    QTRY_VERIFY_WITH_TIMEOUT(state->entered.load(), 5000);
    auto* stop = panel.findChild<QPushButton*>(QStringLiteral("agentStopButton"));
    QVERIFY2(stop, "Stop must be a separate action; submitting text must never interrupt the task.");
    editor->setPlainText(QStringLiteral("Keep this unsent draft."));
    stop->click();
    QTRY_VERIFY_WITH_TIMEOUT(state->canceled.load(), 2000);
    QVERIFY(controller.agent().isBusy(session.toStdString()));
    QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(editor->toPlainText(), QStringLiteral("Keep this unsent draft."));
    QVERIFY(editor->isEnabled() && !editor->isReadOnly());
    editor->moveCursor(QTextCursor::End);
    QTest::keyClicks(editor, " More input.");
    const QString preserved = editor->toPlainText();
    QVERIFY(preserved.endsWith(QStringLiteral(" More input.")));
    QVERIFY(captureAgentFixture(panel, QStringLiteral("stop-preserved-draft")));
    state->release = true;
    QTRY_VERIFY_WITH_TIMEOUT(!controller.agent().isBusy(session.toStdString()), 5000);
    QTest::qWait(100);
    QCOMPARE(editor->toPlainText(), preserved);
    int users = 0;
    for (const MaiMessage& message : controller.agent().listMessages(session.toStdString()))
        if (message.role == MaiRole::User) ++users;
    QCOMPARE(users, 1);
}

void AgentPanelSessionTest::quoteActionFollowsStreamedOwnerAfterSessionRestore() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto state = std::make_shared<IntermediateProgressState>();
    AgentController controller(std::make_unique<IntermediateProgressModel>(),
                               directory.filePath(QStringLiteral("quote-order.sqlite")),
                               [state](MaiToolRegistry& tools) {
                                   tools.add(std::make_unique<IntermediateProgressTool>(state, 1));
                                   tools.add(std::make_unique<IntermediateProgressTool>(state, 2));
                               });
    struct ReleaseOnExit {
        std::shared_ptr<IntermediateProgressState> state;
        ~ReleaseOnExit() {
            state->released = 2;
        }
    } releaseOnExit{state};
    AgentChatPanel panel(controller);
    panel.resize(1000, 800);
    panel.show();
    const QString session = controller.createSession(directory.path(), QStringLiteral("Quote order fixture"));
    panel.openSession(session);
    QVERIFY(controller.sendPrompt(session, QStringLiteral("Run local fixture checks.")));
    QTRY_COMPARE_WITH_TIMEOUT(state->entered.load(), 1, 5000);
    QString assistantId;
    for (const MaiMessage& message : controller.agent().listMessages(session.toStdString()))
        if (message.role == MaiRole::Assistant && !message.parts.empty())
            assistantId = QString::fromStdString(message.id);
    QVERIFY(!assistantId.isEmpty());
    const QString other = controller.createSession(directory.path(), QStringLiteral("Other fixture session"));
    panel.openSession(other);
    panel.openSession(session);
    auto* view = panel.findChild<MarkdownView*>();
    QVERIFY(view);
    const QString footerId = assistantId + QStringLiteral("-quote-action");
    QTRY_VERIFY(view->contains(footerId));
    QPointer<QPushButton> footer;
    for (auto* button : panel.findChildren<QPushButton*>(QStringLiteral("agentQuoteAction")))
        if (button->property("messageId").toString() == assistantId) footer = button;
    QVERIFY(footer);
    state->released = 1;
    QTRY_COMPARE_WITH_TIMEOUT(state->entered.load(), 2, 5000);
    QWidget* group = nullptr;
    QTRY_VERIFY((group = progressToolGroup(panel, QStringLiteral("progress_probe_y"))) != nullptr);
    const auto groupBottom = [&] {
        return group->mapTo(view->viewport(), QPoint()).y() + view->verticalScrollBar()->value() + group->height();
    };
    QTRY_VERIFY_WITH_TIMEOUT(view->itemRect(footerId).top() >= groupBottom(), 3000);
    QVERIFY(footer);
    state->released = 2;
    QTRY_VERIFY_WITH_TIMEOUT(!controller.agent().isBusy(session.toStdString()), 5000);
    QString lastTextPart;
    for (const MaiMessage& message : controller.agent().listMessages(session.toStdString()))
        if (QString::fromStdString(message.id) == assistantId)
            for (const MaiMessagePart& part : message.parts)
                if (std::holds_alternative<MaiTextPart>(part.body)) lastTextPart = QString::fromStdString(part.id);
    QVERIFY(!lastTextPart.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(view->itemRect(footerId).top() >= view->itemRect(lastTextPart).bottom(), 3000);
    QVERIFY(footer);
    QVERIFY(captureAgentFixture(panel, QStringLiteral("quote-after-owner")));
}

QTEST_MAIN(AgentPanelSessionTest)
#include "AgentPanelSessionTest.moc"
