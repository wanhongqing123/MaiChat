// 文件作用：验证远端操作只落到所属桌面 Agent，会话快照不泄漏本机媒体路径。
#include <QJsonArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QAbstractButton>
#include <QDir>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QQueue>
#include <QSet>
#include <QScopeGuard>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTest>
#include <QTimer>
#include <QWindow>
#include <QUuid>
#include <QtConcurrent>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <utility>

#include "MaiTool.h"
#include "MaiIdGenerator.h"
#include "MaiSqliteStore.h"
#include "MaiSessionSchedule.h"
#include "MaiChatTools/DesktopRemoteSessionTools.h"
#include "agent/AgentController.h"
#include "agent/AgentSessionList.h"
#include "agent/DesktopSessionSyncCoordinator.h"
#include "agent/DesktopSessionSyncHost.h"
#include "agent/DesktopSessionAttachments.h"
#include "agent/RemoteAgentPanel.h"
#include "markdown/MarkdownView.h"
#include "ui/ComposerTextEdit.h"
#include "ui/UiZoom.h"
#include "ui/ImagePreviewDialog.h"

namespace {

QStringList publicationTestAccounts;

QString publicationTestAccount() {
    const auto account = QUuid::createUuid().toString(QUuid::WithoutBraces);
    publicationTestAccounts.append(account);
    return account;
}

class ImagePublicationTool final : public MaiTool {
public:
    explicit ImagePublicationTool(std::string path) : path_(std::move(path)) {}
    std::string name() const override { return "publication_image_fixture"; }
    std::string description() const override { return "Return an owned in-process image fixture"; }
    std::string parametersSchema() const override { return R"({"type":"object","properties":{}})"; }
    MaiToolResult execute(const std::string&, const MaiToolContext&) override {
        return MaiToolResult::successWithImages("Owned image result", {{path_, "image/png"}});
    }
private:
    std::string path_;
};

class ImagePublicationModel final : public MaiModelClient {
public:
    std::atomic_bool holdingAfterImage{false};
    std::atomic_bool release{false};
    MaiError stream(const MaiModelRequest&, const MaiStreamSink& sink, const std::atomic_bool& canceled) override {
        if (calls_++ == 0) {
            if (sink.onText) sink.onText("Before image");
            if (sink.onToolCall) sink.onToolCall({"image_fixture_call", "publication_image_fixture", "{}"});
        } else {
            if (sink.onText) sink.onText("After image");
            holdingAfterImage = true;
            while (!release && !canceled) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return {};
    }
    MaiWireApi wireApi() const override { return MaiWireApi::ChatCompletions; }
private:
    int calls_ = 0;
};

class ScheduledToolModel final : public MaiModelClient {
public:
    explicit ScheduledToolModel(std::string arguments) : arguments_(std::move(arguments)) {}

    MaiError stream(const MaiModelRequest&, const MaiStreamSink& sink, const std::atomic<bool>&) override {
        if (calls_++ == 0 && sink.onToolCall)
            sink.onToolCall(MaiToolInvocation{"remote-schedule-create", "maichat_session_task_create", arguments_});
        else if (sink.onText)
            sink.onText("Scheduled on the owner device");
        return {};
    }

    MaiWireApi wireApi() const override {
        return MaiWireApi::ChatCompletions;
    }

private:
    std::string arguments_;
    int calls_ = 0;
};

// The owner stays busy without any network or paid model call. The native
// Agent receives the supplement while this first model operation is held.
class HeldSteeringModel final : public MaiModelClient {
public:
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    MaiError stream(const MaiModelRequest&, const MaiStreamSink& sink, const std::atomic<bool>& cancel) override {
        entered.store(true);
        while (!release.load() && !cancel.load()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (!cancel.load() && sink.onText) sink.onText("Owner completed the task");
        return {};
    }
    MaiWireApi wireApi() const override {
        return MaiWireApi::ChatCompletions;
    }
};

// 真正进入 question 闸门；模型不访问网络，子会话只在问答投影中暴露问题片段。
class TranscriptQuestionModel final : public MaiModelClient {
public:
    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink, const std::atomic<bool>&) override {
        bool answered = false;
        for (const auto& message : request.messages) answered |= message.role == MaiModelRole::ToolResult;
        if (answered) {
            if (sink.onText) sink.onText("Private task result");
        } else {
            if (sink.onText) sink.onText("Private task analysis");
            if (sink.onToolCall)
                sink.onToolCall({"transcript-question", "question",
                                 R"({"question":"Which result should be kept?","options":["First","Second"]})"});
        }
        return {};
    }
    MaiWireApi wireApi() const override {
        return MaiWireApi::ChatCompletions;
    }
};

// 将原 assistant 留在一次未结束的 stream 内，使补充消息真实挤出最新二十条窗口。
class PagedAssistantQuestionModel final : public MaiModelClient {
public:
    explicit PagedAssistantQuestionModel(bool ask) : ask_(ask) {}
    std::atomic<bool> entered{false};
    std::atomic<bool> emitTail{false};
    std::atomic<bool> tailSent{false};
    std::atomic<bool> release{false};
    MaiError stream(const MaiModelRequest&, const MaiStreamSink& sink, const std::atomic<bool>& cancel) override {
        if (calls_++ != 0) {
            if (sink.onText) sink.onText("Follow-up completed");
            return {};
        }
        if (sink.onText) sink.onText(std::string(1100, 'P'));
        entered = true;
        while (!release && !cancel) {
            if (emitTail.exchange(false)) {
                if (sink.onText) sink.onText(" Late streamed tail outside the latest page");
                tailSent = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (ask_ && !cancel && sink.onToolCall)
            sink.onToolCall({"outside-page-question", "question", R"({"question":"Keep the old result?"})"});
        return {};
    }
    MaiWireApi wireApi() const override {
        return MaiWireApi::ChatCompletions;
    }

private:
    bool ask_;
    int calls_ = 0;
};

}  // namespace

class DesktopSessionSyncHostTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void imagePublicationJournalKeepsTerminalReceipt();
    void remoteTemporaryImagesKeepOrderedStateRows();
    void coordinatorWaitsForPendingImagePublishBeforeUpload();
    void historyImagesNeverEnrollForUpload();
    void temporaryImageLocalTlsCachesAcrossExpiryAndOwnerOffline();
    void assistantToolImagePublishesPendingWhileTheTurnIsStillRunning();
    void appliesRemotePromptToTheExistingLocalSession();
    void remoteSteeringBindsToTheOriginalOwnerTurn();
    void remotePromptQueuesWhileOwnerIsBusy();
    void remotePanelSendsMoreBeforePriorReceiptAndStopsSeparately();
    void remotePromptCreatesScheduledTaskOnItsOwner();
    void resolvedSessionFileStaysInsideItsOwnerWorkspace();
    void omitsLocalMediaPathsFromRemoteHistory();
    void projectsOnlySafeCurlFailureSummaries_data();
    void projectsOnlySafeCurlFailureSummaries();
    void remotePanelDisplaysCurlFailureSummaryAndRefreshesIt();
    void coordinatorRoutesPhoneCommandToTheExistingDesktopSession();
    void coordinatorKeepsInputOrderDuringAttachmentPreparation_data();
    void coordinatorKeepsInputOrderDuringAttachmentPreparation();
    void coordinatorDropsPreparedInputWhenOwnerIsDestroyed();
    void coordinatorPublishesLocalMessagesToSharedRoom();
    void coordinatorBackfillsOlderMessagesIntoSharedRoom();
    void coordinatorDeletesRoomWhenLocalSessionIsRemoved();
    void coordinatorReadsRemoteHistoryWithVerifiedChunks();
    void remotePanelShowsHistoryAndRoutesTextToOwner();
    void remoteBusyComposerQueuesInputsAndStopsSeparately();
    void remoteTransportCompletionPreservesNewDraft();
    void remoteQueueMatchesOwnerReceiptAndRetainsPrivateRecovery();
    void remoteIdleConsecutiveInputsDoNotWaitForTurnAcknowledgement();
    void remoteQueueStorageFailureRetainsDraft();
    void remoteLateInputReceiptDoesNotReplaceNewerOwnerTurn();
    void remotePanelRoutesAttachmentLinkToVerifiedDownload();
    void remotePanelDoesNotRepeatPendingHistory();
    void remotePanelReconcilesAfterReconnect();
    void remotePanelKeepsOwnPromptWhenRoomBroadcastsCanonicalMessage();
    void remotePanelPushCanArriveBeforeInitialPage();
    void sessionListKeepsLocalAndRemoteSelectionsSeparate();
    void sessionListSeparatesDevicesWithTheSameModel();
    void remoteLifecycleCreatesAndDeletesInRequestedWorkspace();
    void coordinatorCreatesFirstRemoteSessionAndMatchesLifecycleResult();
    void coordinatorAppliesPhoneLifecycleToDesktop();
    void sessionListOffersLifecycleActionsForRemoteDevice();
    void agentToolsReuseRemoteLifecycleCoordinator();
    void remotePermissionButtonsUseSharedWireDecisions();
    void ownerQuestionAnswersRequireMatchingConversationAndReceipt();
    void childQuestionProjectionKeepsRootHistoryAndPrivatePartsSeparate();
    void coordinatorPublishesChildQuestionChangesInRootRoom();
    void coordinatorBoundsMessageFramesAndRestoresFullQuestionState();
    void coordinatorPublishesUpdatedAssistantOutsideLatestPage();
    void remoteQuestionUsesNormalComposerAndExplicitAnswerReceipt();
    void remoteQuestionProjectionKeepsRootCursorAndStableDisplayRows();
    void remoteRejectedQuestionRecoveryKeepsItsQuestionBinding();
    void remoteQuestionPartsInterleaveWithLaterParentProgress();
    void remoteOldOwnerUpdatesStayOutsideRootHistoryCursor();
    void remoteToolGroupsKeepMetadataOutsideOrdinaryText();
    void remoteToolGroupsRetainExpandedObjectsDuringUpdatesAndPrepend();
    void remoteLegacyToolSnapshotsSplitAtQuestionsAndMediaWithoutLeakingDetails();
    void remoteQuestionAppendRetainsExistingTextSelectionAndToolGroup();
    void remoteChildQuestionProjectionRetainsLaterTextIdentityAndToolGroups();
};

void DesktopSessionSyncHostTest::initTestCase() {
    QStandardPaths::setTestModeEnabled(true);
}

void DesktopSessionSyncHostTest::cleanupTestCase() {
    QVERIFY(QStandardPaths::isTestModeEnabled());
    for (const auto& account : publicationTestAccounts) {
        const auto digest = QString::fromLatin1(QCryptographicHash::hash(account.toUtf8(),
            QCryptographicHash::Sha256).toHex().left(24));
        for (const auto location : {QStandardPaths::AppDataLocation, QStandardPaths::CacheLocation}) {
            const auto root = QDir(QStandardPaths::writableLocation(location)).absolutePath();
            const auto target = QDir::cleanPath(root + QStringLiteral("/session-attachments/") + digest);
            // Remove only the UUID accounts created by this run, under Qt's
            // explicit test-mode root; never remove preferences or user data.
            QVERIFY(target.startsWith(root + QStringLiteral("/session-attachments/")));
            QVERIFY(!QFileInfo::exists(target) || QDir(target).removeRecursively());
        }
    }
}

void DesktopSessionSyncHostTest::imagePublicationJournalKeepsTerminalReceipt() {
    const QString account = publicationTestAccount();
    DesktopSessionRoom room{QStringLiteral("windows"), QStringLiteral("original_login"), QStringLiteral("session")};
    const QString message = QStringLiteral("message"), part = QStringLiteral("part");
    QVERIFY(DesktopSessionAttachments::imagePublication(account, room, message, part).isEmpty());
    QVERIFY(DesktopSessionAttachments::enrollTemporaryImage(account, room, message, part));
    const auto pending = DesktopSessionAttachments::imagePublication(account, room, message, part);
    QCOMPARE(pending.value(QStringLiteral("state")).toString(), QStringLiteral("pending"));
    // A failed first attempt is terminal across adapter instances and login replacement.
    DesktopVideoCircleAccess access;
    access.accountId = account;
    const auto failed = DesktopSessionAttachments::uploadTemporaryImage(access, room, message, part, QString());
    QVERIFY(!failed.ok());
    room.login = QStringLiteral("replacement_login");
    QVERIFY(DesktopSessionAttachments::enrollTemporaryImage(account, room, message, part));
    QCOMPARE(DesktopSessionAttachments::imagePublication(account, room, message, part)
                 .value(QStringLiteral("state")).toString(), QStringLiteral("failed"));
}

void DesktopSessionSyncHostTest::remoteTemporaryImagesKeepOrderedStateRows() {
    QTemporaryDir data;
    RemoteAgentPanel panel(nullptr, data.filePath(QStringLiteral("outbox.json")));
    const QJsonObject owner{{QStringLiteral("device_slot"), QStringLiteral("windows")},
        {QStringLiteral("login_session_id"), QStringLiteral("login")}, {QStringLiteral("id"), QStringLiteral("session")},
        {QStringLiteral("online"), false}};
    panel.openSession(owner);
    const QJsonObject image{{QStringLiteral("id"), QStringLiteral("image")},
        {QStringLiteral("kind"), QStringLiteral("image")},
        {QStringLiteral("remoteImage"), QJsonObject{{QStringLiteral("state"), QStringLiteral("pending")},
                                                 {QStringLiteral("retention"), QStringLiteral("temporary")}}}};
    const QJsonObject message{{QStringLiteral("id"), QStringLiteral("message")},
        {QStringLiteral("role"), QStringLiteral("assistant")},
        {QStringLiteral("parts"), QJsonArray{
            QJsonObject{{QStringLiteral("id"), QStringLiteral("before")}, {QStringLiteral("kind"), QStringLiteral("text")},
                        {QStringLiteral("text"), QStringLiteral("Before image")}}, image,
            QJsonObject{{QStringLiteral("id"), QStringLiteral("after")}, {QStringLiteral("kind"), QStringLiteral("text")},
                        {QStringLiteral("text"), QStringLiteral("After image")}}}}};
    panel.receivePayload(owner, QStringLiteral("history_page"),
        {{QStringLiteral("ok"), true}, {QStringLiteral("messages"), QJsonArray{message}}});
    auto* view = panel.findChild<MarkdownView*>();
    QVERIFY(view);
    QCOMPARE(view->itemCount(), 3);
    const auto cards = panel.findChildren<QLabel*>(QStringLiteral("remoteSessionImage"));
    QCOMPARE(cards.size(), 1);
    QCOMPARE(cards.first()->text(), QStringLiteral("图片正在发布…"));
    panel.receivePayload(owner, QStringLiteral("event"),
        {{QStringLiteral("messages"), QJsonArray{message}}});
    QCOMPARE(panel.findChildren<QLabel*>(QStringLiteral("remoteSessionImage")), cards);
}

void DesktopSessionSyncHostTest::coordinatorWaitsForPendingImagePublishBeforeUpload() {
    QTemporaryDir workspace;
    AgentController::ModelConfig model;
    AgentController controller(model, {});
    const auto session = controller.createSession(workspace.path(), QStringLiteral("Image publication"));
    const auto account = publicationTestAccount();
    QList<DesktopSessionSyncCoordinator::Completion> pendingAcks;
    QString imageMessage, imagePart;
    DesktopSessionSyncCoordinator coordinator(
        [&](const QString& path, const QByteArray&, const QJsonObject& body,
            DesktopSessionSyncCoordinator::Completion completion) {
            if (path == QStringLiteral("session-sync/presence")) {
                completion({{QStringLiteral("login_session_id"), QStringLiteral("owner_login")}}, {});
                return;
            }
            for (const auto& value : body.value(QStringLiteral("messages")).toArray()) {
                const auto message = value.toObject();
                for (const auto& item : message.value(QStringLiteral("parts")).toArray()) {
                    const auto part = item.toObject();
                    if (part.value(QStringLiteral("remoteImage")).toObject().value(QStringLiteral("state")) !=
                        QStringLiteral("pending")) continue;
                    imageMessage = message.value(QStringLiteral("id")).toString();
                    imagePart = part.value(QStringLiteral("id")).toString();
                    QVERIFY(!QJsonDocument(body).toJson().contains(workspace.path().toUtf8()));
                    pendingAcks.append(std::move(completion));
                    return;
                }
            }
            completion({}, {});
        }, [&] { return &controller; }, nullptr, QStringLiteral("image-test"), QStringLiteral("Desktop"));
    coordinator.setAttachmentAccessProvider([&] {
        DesktopVideoCircleAccess access;
        access.accountId = account;  // Missing credentials fail locally after the ACK; no HTTP.
        return access;
    });
    coordinator.start();
    coordinator.connectionReady();
    QVERIFY(controller.agent().submit(MaiSendPrompt{session.toStdString(), "Image event",
        {{workspace.filePath(QStringLiteral("private-screen.png")).toStdString(), "image/png"}}}));
    QTRY_VERIFY_WITH_TIMEOUT(!pendingAcks.isEmpty(), 3000);
    const auto room = coordinator.localRoom(session);
    auto state = [&] {
        return DesktopSessionAttachments::imagePublication(account, room, imageMessage, imagePart)
            .value(QStringLiteral("state")).toString();
    };
    QCOMPARE(state(), QStringLiteral("pending"));
    QTest::qWait(100);
    QCOMPARE(state(), QStringLiteral("pending"));
    pendingAcks.takeFirst()({}, {});
    QTRY_COMPARE_WITH_TIMEOUT(state(), QStringLiteral("failed"), 3000);
    // Later ordinary snapshots must retain the terminal journal receipt.
    coordinator.connectionLost();
    coordinator.connectionReady();
    QTest::qWait(400);
    QCOMPARE(state(), QStringLiteral("failed"));
}

void DesktopSessionSyncHostTest::historyImagesNeverEnrollForUpload() {
    AgentController::ModelConfig model;
    AgentController controller(model, {});
    const auto session = controller.createSession({}, QStringLiteral("Legacy image history"));
    QVERIFY(controller.agent().submit(MaiSendPrompt{session.toStdString(), "Old image",
        {{"/private/old-image.png", "image/png"}}}));
    QTRY_VERIFY_WITH_TIMEOUT(!controller.agent().isBusy(session.toStdString()), 3000);
    QTest::qWait(2);
    DesktopSessionSyncHost host(controller);
    const auto account = publicationTestAccount();
    host.setImagePublicationScope(account, QStringLiteral("windows"), QStringLiteral("owner_login"));
    const auto messages = host.historyPage(session, {}, 20).value(QStringLiteral("messages")).toArray();
    bool sawImage = false;
    for (const auto& value : messages) {
        const auto message = value.toObject();
        host.enrollLiveImages(session, message.value(QStringLiteral("id")).toString());
        for (const auto& item : host.historyMessage(session, message.value(QStringLiteral("id")).toString())
                                    .value(QStringLiteral("parts")).toArray()) {
            const auto part = item.toObject();
            if (part.value(QStringLiteral("kind")) != QStringLiteral("image")) continue;
            sawImage = true;
            QVERIFY(!part.contains(QStringLiteral("remoteImage")));
            QVERIFY(!part.contains(QStringLiteral("path")));
        }
    }
    QVERIFY(sawImage);
}

void DesktopSessionSyncHostTest::temporaryImageLocalTlsCachesAcrossExpiryAndOwnerOffline() {
    const auto endpoint = qEnvironmentVariable("MAICHAT_IMAGE_TEST_ENDPOINT");
    if (endpoint.isEmpty()) QSKIP("Run run_session_image_local_tls.py for the disposable local TLS fixture");
    QFile certificate(qEnvironmentVariable("MAICHAT_IMAGE_TEST_CA"));
    QVERIFY(certificate.open(QIODevice::ReadOnly));
    DesktopVideoCircleAccess access;
    access.authBaseUrl = endpoint + QStringLiteral("/auth");
    access.mediaBaseUrl = endpoint;
    access.accountId = publicationTestAccount();
    access.accountToken = QStringLiteral("author-token");
    access.caPem = certificate.readAll();
    access.proxyUrl = QStringLiteral("");
    access.signedUploadHostSuffix = QStringLiteral("localhost");
    QTemporaryDir files;
    QImage pixels(80, 40, QImage::Format_RGBA8888);
    pixels.fill(Qt::blue);
    const auto source = files.filePath(QStringLiteral("screen.png"));
    QVERIFY(pixels.save(source));
    const DesktopSessionRoom room{QStringLiteral("mac"), QStringLiteral("login_a"), QStringLiteral("session_a")};
    const QString messageId = QStringLiteral("message_a"), partId = QStringLiteral("part_a");
    QVERIFY(DesktopSessionAttachments::enrollTemporaryImage(access.accountId, room, messageId, partId));
    const auto uploaded = DesktopSessionAttachments::uploadTemporaryImage(access, room, messageId, partId, source);
    QVERIFY2(uploaded.ok(), qPrintable(uploaded.error));
    const auto ready = uploaded.metadata;
    const QJsonObject owner{{QStringLiteral("device_slot"), room.slot},
        {QStringLiteral("login_session_id"), QStringLiteral("replacement_login")},
        {QStringLiteral("id"), room.session}, {QStringLiteral("online"), false}};
    const auto payload = [&](const QJsonObject& image) {
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("messages"), QJsonArray{QJsonObject{
            {QStringLiteral("id"), messageId}, {QStringLiteral("role"), QStringLiteral("assistant")},
            {QStringLiteral("parts"), QJsonArray{QJsonObject{{QStringLiteral("id"), partId},
                {QStringLiteral("kind"), QStringLiteral("image")}, {QStringLiteral("remoteImage"), image}}}}}}}};
    };
    const auto show = [&](RemoteAgentPanel& panel) {
        panel.setImageAccessProvider([&] { return access; });
        panel.openSession(owner);
        panel.receivePayload(owner, QStringLiteral("history_page"), payload(ready));
        panel.resize(600, 420);
        panel.show();
    };
    RemoteAgentPanel first(nullptr, files.filePath(QStringLiteral("first.json")));
    show(first);
    auto* card = first.findChild<QLabel*>(QStringLiteral("remoteSessionImage"));
    QVERIFY(card);
    QTRY_VERIFY_WITH_TIMEOUT(!card->pixmap().isNull(), 3000);
    QCOMPARE(card->pixmap().size(), QSize(80, 40));
    const auto rendered = card->grab().toImage();
    QRect blueBounds;
    for (int y = 0; y < rendered.height(); ++y)
        for (int x = 0; x < rendered.width(); ++x)
            if (rendered.pixelColor(x, y) == QColor(Qt::blue))
                blueBounds = blueBounds.united(QRect(x, y, 1, 1));
    QVERIFY(!blueBounds.isEmpty());
    QVERIFY2(qAbs(blueBounds.width() - 2 * blueBounds.height()) <= 2,
        qPrintable(QStringLiteral("Displayed 2:1 image was stretched to %1x%2 in a %3x%4 card")
            .arg(blueBounds.width()).arg(blueBounds.height()).arg(card->width()).arg(card->height())));
    QTest::mouseClick(card, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(first.findChild<ImagePreviewDialog*>(QStringLiteral("remoteSessionImagePreview")), 1000);
    auto* preview = first.findChild<ImagePreviewDialog*>(QStringLiteral("remoteSessionImagePreview"));
    QVERIFY(preview->isVisible());
    QVERIFY(!preview->isModal());
    QCOMPARE(preview->sourceImage().size(), QSize(80, 40));
    preview->close();
    const auto screenshot = qEnvironmentVariable("MAICHAT_IMAGE_TEST_SCREENSHOT");
    if (!screenshot.isEmpty()) QVERIFY(first.grab().save(screenshot));
    const auto expiry = ready.value(QStringLiteral("expires_at")).toVariant().toLongLong();
    QTest::qWait(int(qMax<qint64>(0, expiry - QDateTime::currentMSecsSinceEpoch()) + 30));
    RemoteAgentPanel reopened(nullptr, files.filePath(QStringLiteral("second.json")));
    show(reopened);
    auto* cached = reopened.findChild<QLabel*>(QStringLiteral("remoteSessionImage"));
    QVERIFY(cached);
    QTRY_VERIFY_WITH_TIMEOUT(!cached->pixmap().isNull(), 3000);
    QCOMPARE(cached->pixmap().size(), QSize(80, 40));
    access.accountId = publicationTestAccount();
    const auto uncached = DesktopSessionAttachments::downloadTemporaryImage(access, room, messageId, partId, ready);
    QVERIFY(!uncached.ok());
    QCOMPARE(uncached.error, QStringLiteral("图片已过期"));
    QVERIFY(uncached.path.isEmpty());
}

void DesktopSessionSyncHostTest::assistantToolImagePublishesPendingWhileTheTurnIsStillRunning() {
    QTemporaryDir workspace;
    const auto source = workspace.filePath(QStringLiteral("owned-image.png"));
    QImage pixels(8, 4, QImage::Format_RGBA8888);
    pixels.fill(Qt::green);
    QVERIFY(pixels.save(source));
    auto model = std::make_unique<ImagePublicationModel>();
    auto* scripted = model.get();
    AgentController controller(std::move(model), {}, [&](MaiToolRegistry& registry) {
        registry.add(std::make_unique<ImagePublicationTool>(source.toStdString()));
    });
    const auto release = qScopeGuard([&] { scripted->release = true; });
    const auto session = controller.createSession(workspace.path(), QStringLiteral("Live assistant image"));
    const auto account = publicationTestAccount();
    bool sawPending = false;
    DesktopSessionSyncCoordinator coordinator(
        [&](const QString& path, const QByteArray&, const QJsonObject& body,
            DesktopSessionSyncCoordinator::Completion completion) {
            if (path == QStringLiteral("session-sync/presence")) {
                completion({{QStringLiteral("login_session_id"), QStringLiteral("owner_login")}}, {});
                return;
            }
            for (const auto& item : body.value(QStringLiteral("messages")).toArray()) {
                const auto message = item.toObject();
                if (message.value(QStringLiteral("role")) != QStringLiteral("assistant")) continue;
                for (const auto& value : message.value(QStringLiteral("parts")).toArray()) {
                    const auto part = value.toObject();
                    if (part.value(QStringLiteral("kind")) != QStringLiteral("image")) continue;
                    sawPending |= part.value(QStringLiteral("remoteImage")).toObject()
                        .value(QStringLiteral("state")) == QStringLiteral("pending");
                }
            }
            completion({}, {});
        }, [&] { return &controller; }, nullptr, QStringLiteral("assistant-image-test"), QStringLiteral("Desktop"));
    coordinator.setAttachmentAccessProvider([&] {
        DesktopVideoCircleAccess access;
        access.accountId = account;
        return access;
    });
    coordinator.start();
    coordinator.connectionReady();
    QVERIFY(controller.sendPrompt(session, QStringLiteral("Observe the owned image")));
    QTRY_VERIFY_WITH_TIMEOUT(scripted->holdingAfterImage.load(), 3000);
    QVERIFY(controller.agent().isBusy(session.toStdString()));
    QTRY_VERIFY_WITH_TIMEOUT(sawPending, 3000);
    QVERIFY(controller.agent().isBusy(session.toStdString()));
}

void DesktopSessionSyncHostTest::ownerQuestionAnswersRequireMatchingConversationAndReceipt() {
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    AgentController controller(std::make_unique<TranscriptQuestionModel>(), {});
    const QString owner = controller.createSession(workspace.path(), QStringLiteral("Question owner"));
    const QString other = controller.createSession(workspace.path(), QStringLiteral("Other owner"));
    DesktopSessionSyncHost host(controller);
    QVERIFY(controller.sendPrompt(owner, QStringLiteral("Ask one question")));
    QTRY_COMPARE(controller.pendingQuestions().size(), size_t(1));
    const MaiQuestionRequest pending = controller.pendingQuestions().front();
    const QString questionId = QString::fromStdString(pending.id);
    const QJsonObject shown = host.historyPage(owner, {}, 20);
    const QJsonObject question = shown.value(QStringLiteral("questions")).toArray().first().toObject();
    QCOMPARE(question.value(QStringLiteral("sessionId")).toString(), owner);
    QCOMPARE(question.value(QStringLiteral("messageId")).toString(), QString::fromStdString(pending.messageId));
    QCOMPARE(question.value(QStringLiteral("partId")).toString(), QString::fromStdString(pending.partId));
    QVERIFY(question.value(QStringLiteral("asked")).toDouble() > 0);
    QJsonObject answer{{QStringLiteral("action"), QStringLiteral("answer")},
                       {QStringLiteral("id"), questionId},
                       {QStringLiteral("text"), QStringLiteral("First")}};
    const QJsonObject wrong = host.applyCommand(other, answer);
    QVERIFY(!wrong.value(QStringLiteral("ok")).toBool());
    QCOMPARE(wrong.value(QStringLiteral("errorCode")).toString(), QStringLiteral("not_found"));
    QCOMPARE(controller.pendingQuestions().size(), size_t(1));
    QJsonObject withMedia = answer;
    withMedia.insert(QStringLiteral("local_files"), QJsonArray{QStringLiteral("unimported.png")});
    const QJsonObject refusedMedia = host.applyCommand(owner, withMedia);
    QVERIFY(!refusedMedia.value(QStringLiteral("ok")).toBool());
    QCOMPARE(refusedMedia.value(QStringLiteral("errorCode")).toString(), QStringLiteral("invalid_input"));
    const QJsonObject accepted = host.applyCommand(owner, answer);
    QVERIFY2(accepted.value(QStringLiteral("ok")).toBool(),
             qPrintable(accepted.value(QStringLiteral("error")).toString()));
    QCOMPARE(accepted.value(QStringLiteral("question_id")).toString(), questionId);
    QVERIFY(!accepted.contains(QStringLiteral("user_message_id")));
    QTRY_VERIFY(!controller.agent().isBusy(owner.toStdString()));
    const QJsonObject stale = host.applyCommand(owner, answer);
    QVERIFY(!stale.value(QStringLiteral("ok")).toBool());
    QCOMPARE(stale.value(QStringLiteral("errorCode")).toString(), QStringLiteral("not_found"));
    int users = 0;
    bool replayed = false;
    for (const QJsonValue& value : host.historyPage(owner, {}, 20).value(QStringLiteral("messages")).toArray()) {
        const auto message = value.toObject();
        QCOMPARE(message.value(QStringLiteral("sessionId")).toString(), owner);
        users += message.value(QStringLiteral("role")).toString() == QStringLiteral("user");
        for (const auto& partValue : message.value(QStringLiteral("parts")).toArray()) {
            const auto part = partValue.toObject();
            if (part.value(QStringLiteral("tool")) != QStringLiteral("question")) continue;
            replayed = part.value(QStringLiteral("input")).toString().contains(QStringLiteral("Which result")) &&
                       part.value(QStringLiteral("output")).toString() == QStringLiteral("First");
        }
    }
    QCOMPARE(users, 1);  // 答案已是工具结果，不能再创建一份 Core user 消息污染上下文。
    QVERIFY(replayed);
}

void DesktopSessionSyncHostTest::childQuestionProjectionKeepsRootHistoryAndPrivatePartsSeparate() {
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    AgentController controller(std::make_unique<TranscriptQuestionModel>(), {});
    const QString root = controller.createSession(workspace.path(), QStringLiteral("Root owner"));
    const QString other = controller.createSession(workspace.path(), QStringLiteral("Unrelated owner"));
    DesktopSessionSyncHost host(controller);
    const auto child = controller.agent().spawnSubAgent(root.toStdString(), "question-child", "Ask a child question");
    QVERIFY(child);
    QTRY_COMPARE(controller.pendingQuestions().size(), size_t(1));
    const auto pending = controller.pendingQuestions().front();
    QCOMPARE(pending.sessionId, child.value());
    const auto before = host.historyPage(root, {}, 1);
    QVERIFY(before.value(QStringLiteral("messages")).toArray().isEmpty());
    QCOMPARE(before.value(QStringLiteral("questions")).toArray().size(), 1);
    QCOMPARE(before.value(QStringLiteral("questionMessages")).toArray().size(), 1);
    const auto projection = before.value(QStringLiteral("questionMessages")).toArray().first().toObject();
    QCOMPARE(projection.value(QStringLiteral("id")).toString(), QString::fromStdString(pending.messageId));
    QCOMPARE(projection.value(QStringLiteral("sessionId")).toString(), QString::fromStdString(child.value()));
    QVERIFY(projection.value(QStringLiteral("questionProjection")).toBool());
    const auto parts = projection.value(QStringLiteral("parts")).toArray();
    QCOMPARE(parts.size(), 1);
    QCOMPARE(parts.first().toObject().value(QStringLiteral("tool")).toString(), QStringLiteral("question"));
    QVERIFY(!QJsonDocument(projection).toJson().contains("Private task"));
    const auto isolated = host.historyPage(other, {}, 1);
    QVERIFY(isolated.value(QStringLiteral("questions")).toArray().isEmpty());
    QVERIFY(isolated.value(QStringLiteral("questionMessages")).toArray().isEmpty());
    const auto accepted = host.applyCommand(root, {{QStringLiteral("action"), QStringLiteral("answer")},
                                                   {QStringLiteral("id"), QString::fromStdString(pending.id)},
                                                   {QStringLiteral("text"), QStringLiteral("Second")}});
    QVERIFY(accepted.value(QStringLiteral("ok")).toBool());
    QTRY_VERIFY(!controller.agent().isBusy(child.value()));
    const auto after = host.historyPage(root, {}, 1);
    QVERIFY(after.value(QStringLiteral("messages")).toArray().isEmpty());
    QVERIFY(after.value(QStringLiteral("questions")).toArray().isEmpty());
    QCOMPARE(after.value(QStringLiteral("questionMessages")).toArray().size(), 1);
    const auto replayed = after.value(QStringLiteral("questionMessages")).toArray().first().toObject();
    QCOMPARE(
        replayed.value(QStringLiteral("parts")).toArray().first().toObject().value(QStringLiteral("output")).toString(),
        QStringLiteral("Second"));
    QVERIFY(controller.agent().listMessages(root.toStdString()).empty());
}

void DesktopSessionSyncHostTest::coordinatorPublishesChildQuestionChangesInRootRoom() {
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    AgentController controller(std::make_unique<TranscriptQuestionModel>(), {});
    const QString root = controller.createSession(workspace.path(), QStringLiteral("Root live owner"));
    QList<QJsonObject> published;
    DesktopSessionSyncCoordinator coordinator(
        [&](const QString& path, const QByteArray&, const QJsonObject& body,
            DesktopSessionSyncCoordinator::Completion completion) {
            if (path == QStringLiteral("session-sync/publish")) published.append(body);
            QTimer::singleShot(0, &controller, [completion = std::move(completion)] { completion({}, {}); });
        },
        [&] { return &controller; }, nullptr, QStringLiteral("synthetic-owner"), QStringLiteral("Windows"));
    coordinator.start();
    coordinator.connectionReady();
    const auto child = controller.agent().spawnSubAgent(root.toStdString(), "live-child", "Ask a child question");
    QVERIFY(child);
    QTRY_COMPARE(controller.pendingQuestions().size(), size_t(1));
    const auto pending = controller.pendingQuestions().front();
    const auto sawPending = [&] {
        for (const auto& body : published)
            if (body.value(QStringLiteral("session_id")).toString() == root &&
                body.value(QStringLiteral("questions")).toArray().size() == 1 &&
                body.value(QStringLiteral("questionMessages")).toArray().size() == 1)
                return true;
        return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(sawPending(), 5000);
    DesktopSessionSyncHost host(controller);
    QVERIFY(host.applyCommand(root, {{QStringLiteral("action"), QStringLiteral("answer")},
                                     {QStringLiteral("id"), QString::fromStdString(pending.id)},
                                     {QStringLiteral("text"), QStringLiteral("First")}})
                .value(QStringLiteral("ok"))
                .toBool());
    const auto sawAnswer = [&] {
        for (const auto& body : published) {
            if (body.value(QStringLiteral("session_id")).toString() != root ||
                !body.value(QStringLiteral("questions")).toArray().isEmpty())
                continue;
            for (const auto& projected : body.value(QStringLiteral("questionMessages")).toArray())
                for (const auto& value : projected.toObject().value(QStringLiteral("parts")).toArray())
                    if (value.toObject().value(QStringLiteral("output")).toString() == QStringLiteral("First"))
                        return true;
        }
        return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(sawAnswer(), 5000);
    for (const auto& body : published) QCOMPARE(body.value(QStringLiteral("session_id")).toString(), root);
    QVERIFY(controller.agent().listMessages(root.toStdString()).empty());
}

void DesktopSessionSyncHostTest::coordinatorBoundsMessageFramesAndRestoresFullQuestionState() {
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    const QString database = workspace.filePath(QStringLiteral("large-state.sqlite"));
    AgentController controller(std::make_unique<TranscriptQuestionModel>(), database);
    const QString root = controller.createSession(workspace.path(), QStringLiteral("Bounded room state"));
    const std::string childId = MaiIdGenerator::newSessionId();
    const std::string questionMessageId = MaiIdGenerator::newMessageId();
    const std::string rootMessageId = MaiIdGenerator::newMessageId();
    {
        auto store = makeMaiSqliteStore(database.toStdString());
        QVERIFY(store);
        MaiSession child;
        child.id = childId;
        child.parentId = root.toStdString();
        child.depth = 1;
        child.created = MaiTime::getCurrentTime();
        child.updated = child.created;
        store.value()->putSession(child);
        MaiMessage rootMessage;
        rootMessage.id = rootMessageId;
        rootMessage.role = MaiRole::Assistant;
        rootMessage.created = child.created;
        rootMessage.completed = child.created;
        rootMessage.parts.push_back(
            {MaiIdGenerator::newPartId(), MaiTextPart{std::string(48 * 1024 - 512, 'R')}, rootMessage.created});
        store.value()->putMessage(root.toStdString(), rootMessage);
        MaiMessage projected;
        projected.id = questionMessageId;
        projected.role = MaiRole::Assistant;
        projected.created = child.created;
        projected.completed = child.created;
        MaiToolPart question;
        question.tool = "question";
        question.input = "{\"question\":\"" + std::string(24000, 'Q') + "\"}";
        question.output = std::string(24400, 'A');
        question.state = MaiToolState::Completed;
        projected.parts.push_back({MaiIdGenerator::newPartId(), question, projected.created});
        store.value()->putMessage(childId, projected);
    }
    QList<QJsonObject> published;
    bool holdMessageAcks = false;
    QList<DesktopSessionSyncCoordinator::Completion> heldMessageReplies;
    DesktopSessionSyncCoordinator coordinator(
        [&](const QString& path, const QByteArray&, const QJsonObject& body,
            DesktopSessionSyncCoordinator::Completion completion) {
            if (path == QStringLiteral("session-sync/publish")) {
                published.append(body);
                if (holdMessageAcks && !body.value(QStringLiteral("messages")).toArray().isEmpty()) {
                    heldMessageReplies.append(std::move(completion));
                    return;
                }
            }
            QTimer::singleShot(0, &controller, [completion = std::move(completion)] { completion({}, {}); });
        },
        [&] { return &controller; }, nullptr, QStringLiteral("synthetic-large-state"), QStringLiteral("Windows"));
    coordinator.start();
    coordinator.connectionReady();
    QTRY_VERIFY_WITH_TIMEOUT(!published.isEmpty(), 5000);
    for (const auto& body : published)
        QVERIFY2(QJsonDocument(body).toJson(QJsonDocument::Compact).size() <= 95 * 1024,
                 "A root message and child projection exceeded the final publish-frame budget");
    const auto completeStateWasSent = [&] {
        for (const auto& body : published)
            if (body.value(QStringLiteral("messages")).toArray().isEmpty() &&
                body.value(QStringLiteral("questionMessages")).toArray().size() == 1)
                return true;
        return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(completeStateWasSent(), 5000);
    bool retainedRootText = false;
    for (const auto& body : published)
        for (const auto& value : body.value(QStringLiteral("messages")).toArray()) {
            const auto parts = value.toObject().value(QStringLiteral("parts")).toArray();
            retainedRootText |=
                parts.first().toObject().value(QStringLiteral("text")).toString().size() == 48 * 1024 - 512;
        }
    QVERIFY(retainedRootText);
    const int afterCompleteState = published.size();
    MaiEvent idle;
    idle.type = MaiEventType::SessionIdle;
    idle.sessionId = root.toStdString();
    controller.agent().eventBus().publish(idle);
    QTest::qWait(450);
    QCOMPARE(published.size(), afterCompleteState);  // 最终实际状态 digest 已记录，不重复发小状态帧。

    // 两代消息帧倒序返回，较早一代还失败；必须释放全部计数后只补最新问答状态。
    holdMessageAcks = true;
    const auto updateStoredState = [&](char textFill, char answerFill) {
        auto store = makeMaiSqliteStore(database.toStdString());
        if (!store) return false;
        MaiMessage message;
        if (!store.value()->getMessage(root.toStdString(), rootMessageId, message)) return false;
        std::get<MaiTextPart>(message.parts.front().body).text.assign(48 * 1024 - 512, textFill);
        store.value()->putMessage(root.toStdString(), message);
        if (!store.value()->getMessage(childId, questionMessageId, message)) return false;
        std::get<MaiToolPart>(message.parts.front().body).output.assign(24400, answerFill);
        store.value()->putMessage(childId, message);
        controller.agent().eventBus().publish(idle);
        return true;
    };
    QVERIFY(updateStoredState('S', 'A'));
    QTRY_COMPARE_WITH_TIMEOUT(heldMessageReplies.size(), 1, 5000);
    QVERIFY(updateStoredState('T', 'B'));
    QTRY_COMPARE_WITH_TIMEOUT(heldMessageReplies.size(), 2, 5000);
    const int beforeAcks = published.size();
    heldMessageReplies[1]({}, {});
    QCOMPARE(published.size(), beforeAcks);
    heldMessageReplies[0]({}, QStringLiteral("Synthetic older frame failed"));
    const auto newestStateWasRestored = [&] {
        for (int index = beforeAcks; index < published.size(); ++index) {
            const auto body = published[index];
            const auto projection = body.value(QStringLiteral("questionMessages")).toArray();
            if (!body.value(QStringLiteral("messages")).toArray().isEmpty() || projection.size() != 1) continue;
            const auto output = projection.first()
                                    .toObject()
                                    .value(QStringLiteral("parts"))
                                    .toArray()
                                    .first()
                                    .toObject()
                                    .value(QStringLiteral("output"))
                                    .toString();
            if (output.size() == 24400 && output.startsWith(QLatin1Char('B'))) return true;
        }
        return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(newestStateWasRestored(), 5000);
    for (const auto& body : published) QVERIFY(QJsonDocument(body).toJson(QJsonDocument::Compact).size() <= 95 * 1024);
    const int afterLatestState = published.size();
    controller.agent().eventBus().publish(idle);
    QTest::qWait(450);
    QCOMPARE(published.size(), afterLatestState);
}

void DesktopSessionSyncHostTest::coordinatorPublishesUpdatedAssistantOutsideLatestPage() {
    for (const bool asksQuestion : {false, true}) {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        auto model = std::make_unique<PagedAssistantQuestionModel>(asksQuestion);
        auto* gate = model.get();
        AgentController controller(std::move(model), {});
        const QString root = controller.createSession(workspace.path(), QStringLiteral("Paged live owner"));
        DesktopSessionSyncHost host(controller);
        QList<QJsonObject> published;
        DesktopSessionSyncCoordinator coordinator(
            [&](const QString& path, const QByteArray&, const QJsonObject& body,
                DesktopSessionSyncCoordinator::Completion completion) {
                if (path == QStringLiteral("session-sync/publish")) published.append(body);
                QTimer::singleShot(0, &controller, [completion = std::move(completion)] { completion({}, {}); });
            },
            [&] { return &controller; }, nullptr, QStringLiteral("synthetic-paged-owner"), QStringLiteral("Windows"));
        coordinator.start();
        coordinator.connectionReady();
        QVERIFY(controller.sendPrompt(root, QStringLiteral("Hold the stream and continue")));
        QTRY_VERIFY(gate->entered.load());
        const std::string turn = controller.agent().activeTurnId(root.toStdString());
        QVERIFY(!turn.empty());
        for (int index = 0; index < 21; ++index)
            QVERIFY(controller.agent().submit(MaiSteerPrompt{root.toStdString(), turn,
                                                             "outside-page-" + std::to_string(index),
                                                             "Accepted supplement " + std::to_string(index)}));
        const auto recent = host.historyPage(root, {}, 20).value(QStringLiteral("messages")).toArray();
        QCOMPARE(recent.size(), 20);
        for (const auto& value : recent)
            QVERIFY(value.toObject().value(QStringLiteral("id")).toString() != QString::fromStdString(turn));
        gate->emitTail = true;
        QTRY_VERIFY(gate->tailSent.load());
        const auto sawOldTail = [&] {
            for (const auto& body : published)
                for (const auto& value : body.value(QStringLiteral("messages")).toArray()) {
                    const auto message = value.toObject();
                    if (message.value(QStringLiteral("id")).toString() != QString::fromStdString(turn)) continue;
                    for (const auto& part : message.value(QStringLiteral("parts")).toArray())
                        if (part.toObject()
                                .value(QStringLiteral("text"))
                                .toString()
                                .contains(QStringLiteral("Late streamed tail")))
                            return true;
                }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(sawOldTail(), 5000);
        gate->release = true;
        if (asksQuestion) {
            // 流式期间已有补充，尚未开始的问题工具应被取消，而不是等用户回答旧计划。
            // 取消回执仍须把已挤出最新页的原 assistant 更新发布回去。
            QTRY_VERIFY(!controller.agent().isBusy(root.toStdString()));
            QCOMPARE(controller.pendingQuestions().size(), size_t(0));
        }
        QTRY_VERIFY(!controller.agent().isBusy(root.toStdString()));
        const auto sawFinalOldMessage = [&] {
            for (const auto& body : published)
                for (const auto& value : body.value(QStringLiteral("messages")).toArray()) {
                    const auto message = value.toObject();
                    if (message.value(QStringLiteral("id")).toString() != QString::fromStdString(turn) ||
                        message.value(QStringLiteral("completed")).toDouble() <= 0)
                        continue;
                    if (!asksQuestion) return true;
                    for (const auto& part : message.value(QStringLiteral("parts")).toArray())
                        if (part.toObject().value(QStringLiteral("state")).toString() == QStringLiteral("canceled") &&
                            part.toObject()
                                .value(QStringLiteral("output"))
                                .toString()
                                .contains(QStringLiteral("not executed because new user input arrived")))
                            return true;
                }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(sawFinalOldMessage(), 5000);
        for (const auto& body : published) QCOMPARE(body.value(QStringLiteral("session_id")).toString(), root);
    }
}

void DesktopSessionSyncHostTest::remoteLifecycleCreatesAndDeletesInRequestedWorkspace() {
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    AgentController controller(AgentController::ModelConfig{}, {});
    DesktopSessionSyncHost host(controller);
    const QJsonObject created = host.applyCommand(QStringLiteral("session_lifecycle"),
                                                  {{QStringLiteral("action"), QStringLiteral("session_create")},
                                                   {QStringLiteral("workspace_path"), workspace.path()}});
    QVERIFY2(created.value(QStringLiteral("ok")).toBool(),
             qPrintable(created.value(QStringLiteral("error")).toString()));
    const QString id = created.value(QStringLiteral("session_id")).toString();
    QVERIFY(!id.isEmpty());
    MaiSession session;
    QVERIFY(controller.agent().getSession(id.toStdString(), session));
    QCOMPARE(QString::fromStdString(session.directory), QDir(workspace.path()).canonicalPath());
    const QJsonObject removed = host.applyCommand(
        QStringLiteral("session_lifecycle"),
        {{QStringLiteral("action"), QStringLiteral("session_delete")}, {QStringLiteral("target_session_id"), id}});
    QVERIFY2(removed.value(QStringLiteral("ok")).toBool(),
             qPrintable(removed.value(QStringLiteral("error")).toString()));
    QVERIFY(!controller.agent().getSession(id.toStdString(), session));
    const QJsonObject invalid = host.applyCommand(
        QStringLiteral("session_lifecycle"), {{QStringLiteral("action"), QStringLiteral("session_create")},
                                              {QStringLiteral("workspace_path"), QStringLiteral("relative/path")}});
    QVERIFY(!invalid.value(QStringLiteral("ok")).toBool());
    QTemporaryDir defaultWorkspace;
    QVERIFY(defaultWorkspace.isValid());
    host.setDefaultWorkspaceDirectory(defaultWorkspace.path());
    const QJsonObject defaultCreated = host.applyCommand(
        QStringLiteral("session_lifecycle"), {{QStringLiteral("action"), QStringLiteral("session_create")}});
    QVERIFY(defaultCreated.value(QStringLiteral("ok")).toBool());
    const QString defaultId = defaultCreated.value(QStringLiteral("session_id")).toString();
    QVERIFY(controller.agent().getSession(defaultId.toStdString(), session));
    QCOMPARE(QString::fromStdString(session.directory), QDir(defaultWorkspace.path()).canonicalPath());
    QTemporaryDir otherWorkspace;
    QVERIFY(otherWorkspace.isValid());
    const QJsonObject switched = host.applyCommand(QStringLiteral("session_lifecycle"),
                                                   {{QStringLiteral("action"), QStringLiteral("session_set_workspace")},
                                                    {QStringLiteral("target_session_id"), defaultId},
                                                    {QStringLiteral("workspace_path"), otherWorkspace.path()}});
    QVERIFY2(switched.value(QStringLiteral("ok")).toBool(),
             qPrintable(switched.value(QStringLiteral("error")).toString()));
    QCOMPARE(switched.value(QStringLiteral("session_id")).toString(), defaultId);
    QVERIFY(controller.agent().getSession(defaultId.toStdString(), session));
    QCOMPARE(QString::fromStdString(session.directory), QDir(otherWorkspace.path()).canonicalPath());
    QCOMPARE(controller.agent().listSessions().size(), size_t(1));
    const QJsonObject badWorkspace = host.applyCommand(
        QStringLiteral("session_lifecycle"), {{QStringLiteral("action"), QStringLiteral("session_set_workspace")},
                                              {QStringLiteral("target_session_id"), defaultId},
                                              {QStringLiteral("workspace_path"), QStringLiteral("relative/path")}});
    QVERIFY(!badWorkspace.value(QStringLiteral("ok")).toBool());
}

void DesktopSessionSyncHostTest::coordinatorCreatesFirstRemoteSessionAndMatchesLifecycleResult() {
    AgentController controller(AgentController::ModelConfig{}, {});
    const QJsonObject device{{QStringLiteral("device_slot"), QStringLiteral("ios_phone")},
                             {QStringLiteral("device_id"), QStringLiteral("phone-install")},
                             {QStringLiteral("login_session_id"), QStringLiteral("phone-login")},
                             {QStringLiteral("online"), true}};
    QJsonObject sent;
    DesktopSessionSyncCoordinator coordinator(
        [&](const QString& path, const QByteArray&, const QJsonObject& body,
            DesktopSessionSyncCoordinator::Completion completion) {
            if (path == QStringLiteral("session-sync/send") &&
                body.value(QStringLiteral("kind")) == QStringLiteral("command"))
                sent = body;
            QTimer::singleShot(0, &controller, [completion = std::move(completion)] { completion({}, {}); });
        },
        [&] { return &controller; }, nullptr, QStringLiteral("mac-install"), QStringLiteral("Mac"));
    coordinator.start();
    coordinator.acceptDevices(QJsonArray{device});
    QJsonObject completed;
    QString error;
    coordinator.sendRemoteLifecycle(device, {{QStringLiteral("action"), QStringLiteral("session_create")}},
                                    [&](const QJsonObject& value, const QString& failure) {
                                        completed = value;
                                        error = failure;
                                    });
    QCOMPARE(sent.value(QStringLiteral("session_id")).toString(), QStringLiteral("session_lifecycle"));
    QCOMPARE(sent.value(QStringLiteral("to_slot")).toString(), QStringLiteral("ios_phone"));
    QVERIFY(!sent.value(QStringLiteral("payload")).toObject().contains(QStringLiteral("workspace_path")));
    const QString commandId = sent.value(QStringLiteral("client_message_id")).toString();
    QVERIFY(!commandId.isEmpty());
    const QJsonObject result{
        {QStringLiteral("type"), QStringLiteral("command_result")},
        {QStringLiteral("command_id"), commandId},
        {QStringLiteral("result"),
         QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("session_id"), QStringLiteral("ses_new")}}}};
    const QJsonObject envelope{{QStringLiteral("id"), 7},
                               {QStringLiteral("kind"), QStringLiteral("event")},
                               {QStringLiteral("session_id"), QStringLiteral("session_lifecycle")},
                               {QStringLiteral("from_slot"), QStringLiteral("ios_phone")},
                               {QStringLiteral("from_login_session_id"), QStringLiteral("phone-login")},
                               {QStringLiteral("payload"), result}};
    coordinator.acceptInbox(QJsonArray{envelope});
    QVERIFY(error.isEmpty());
    QCOMPARE(completed.value(QStringLiteral("session_id")).toString(), QStringLiteral("ses_new"));
}

void DesktopSessionSyncHostTest::coordinatorAppliesPhoneLifecycleToDesktop() {
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    AgentController controller(AgentController::ModelConfig{}, {});
    QList<QJsonObject> sent;
    int latestPresenceCount = -1;
    DesktopSessionSyncCoordinator coordinator(
        [&](const QString& path, const QByteArray&, const QJsonObject& body,
            DesktopSessionSyncCoordinator::Completion completion) {
            if (path == QStringLiteral("session-sync/send")) sent.append(body);
            if (path == QStringLiteral("session-sync/presence"))
                latestPresenceCount = body.value(QStringLiteral("sessions")).toArray().size();
            QTimer::singleShot(0, &controller, [completion = std::move(completion)] { completion({}, {}); });
        },
        [&] { return &controller; }, nullptr, QStringLiteral("mac-install"), QStringLiteral("Mac"));
    coordinator.setDefaultWorkspaceDirectory(workspace.path());
    coordinator.start();
    coordinator.connectionReady();
    coordinator.acceptInbox(QJsonArray{QJsonObject{
        {QStringLiteral("id"), 1},
        {QStringLiteral("kind"), QStringLiteral("command")},
        {QStringLiteral("session_id"), QStringLiteral("session_lifecycle")},
        {QStringLiteral("from_slot"), QStringLiteral("ios_phone")},
        {QStringLiteral("from_login_session_id"), QStringLiteral("phone-login")},
        {QStringLiteral("client_message_id"), QStringLiteral("create-1")},
        {QStringLiteral("payload"), QJsonObject{{QStringLiteral("action"), QStringLiteral("session_create")}}}}});
    QTRY_VERIFY_WITH_TIMEOUT(latestPresenceCount == 1 && !sent.isEmpty(), 5000);
    QCOMPARE(sent.first().value(QStringLiteral("session_id")).toString(), QStringLiteral("session_lifecycle"));
    const QJsonObject created = sent.first().value(QStringLiteral("payload")).toObject();
    QCOMPARE(created.value(QStringLiteral("command_id")).toString(), QStringLiteral("create-1"));
    const QString id =
        created.value(QStringLiteral("result")).toObject().value(QStringLiteral("session_id")).toString();
    QVERIFY(!id.isEmpty());
    MaiSession session;
    QVERIFY(controller.agent().getSession(id.toStdString(), session));
    QCOMPARE(QString::fromStdString(session.directory), QDir(workspace.path()).canonicalPath());
    QTemporaryDir otherWorkspace;
    QVERIFY(otherWorkspace.isValid());
    coordinator.acceptInbox(QJsonArray{QJsonObject{
        {QStringLiteral("id"), 3},
        {QStringLiteral("kind"), QStringLiteral("command")},
        {QStringLiteral("session_id"), QStringLiteral("session_lifecycle")},
        {QStringLiteral("from_slot"), QStringLiteral("ios_phone")},
        {QStringLiteral("from_login_session_id"), QStringLiteral("phone-login")},
        {QStringLiteral("client_message_id"), QStringLiteral("workspace-1")},
        {QStringLiteral("payload"), QJsonObject{{QStringLiteral("action"), QStringLiteral("session_set_workspace")},
                                                {QStringLiteral("target_session_id"), id},
                                                {QStringLiteral("workspace_path"), otherWorkspace.path()}}}}});
    QTRY_VERIFY_WITH_TIMEOUT(sent.size() >= 2, 5000);
    QVERIFY(controller.agent().getSession(id.toStdString(), session));
    QCOMPARE(QString::fromStdString(session.directory), QDir(otherWorkspace.path()).canonicalPath());
    QCOMPARE(sent.last().value(QStringLiteral("payload")).toObject().value(QStringLiteral("command_id")).toString(),
             QStringLiteral("workspace-1"));
    coordinator.acceptInbox(QJsonArray{QJsonObject{
        {QStringLiteral("id"), 2},
        {QStringLiteral("kind"), QStringLiteral("command")},
        {QStringLiteral("session_id"), QStringLiteral("session_lifecycle")},
        {QStringLiteral("from_slot"), QStringLiteral("ios_phone")},
        {QStringLiteral("from_login_session_id"), QStringLiteral("phone-login")},
        {QStringLiteral("client_message_id"), QStringLiteral("delete-1")},
        {QStringLiteral("payload"), QJsonObject{{QStringLiteral("action"), QStringLiteral("session_delete")},
                                                {QStringLiteral("target_session_id"), id}}}}});
    QTRY_VERIFY_WITH_TIMEOUT(latestPresenceCount == 0 && sent.size() >= 3, 5000);
    QVERIFY(!controller.agent().getSession(id.toStdString(), session));
    const QJsonObject deleted = sent.last().value(QStringLiteral("payload")).toObject();
    QCOMPARE(deleted.value(QStringLiteral("command_id")).toString(), QStringLiteral("delete-1"));
    QCOMPARE(deleted.value(QStringLiteral("result")).toObject().value(QStringLiteral("session_id")).toString(), id);
}

void DesktopSessionSyncHostTest::sessionListOffersLifecycleActionsForRemoteDevice() {
    AgentController controller(AgentController::ModelConfig{}, {});
    AgentSessionList sessions(controller);
    sessions.resize(320, 460);
    const QJsonObject device{{QStringLiteral("device_slot"), QStringLiteral("ios_phone")},
                             {QStringLiteral("device_name"), QStringLiteral("iPhone")},
                             {QStringLiteral("device_id"), QStringLiteral("phone-install")},
                             {QStringLiteral("login_session_id"), QStringLiteral("phone-login")},
                             {QStringLiteral("online"), true}};
    sessions.setRemoteDevices(QJsonArray{device});
    sessions.show();
    auto* list = sessions.findChild<QListWidget*>();
    QVERIFY(list);
    // The empty local device keeps its + entry; the remote header follows it.
    QCOMPARE(list->count(), 2);
    QVERIFY(list->item(0)->text().contains(QStringLiteral("这台")));
    QVERIFY(list->item(1)->text().contains(QStringLiteral("0")));
    if (qEnvironmentVariableIsSet("MAICHAT_REMOTE_SESSION_SCREENSHOT"))
        QVERIFY(sessions.grab().save(qEnvironmentVariable("MAICHAT_REMOTE_SESSION_SCREENSHOT")));
    QSignalSpy create(&sessions, &AgentSessionList::remoteCreateRequested);
    const QRect header = list->visualItemRect(list->item(1));
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {},
                      QPoint(header.right() - UiZoom::s(100), header.center().y()));
    QCOMPARE(create.size(), 1);
    QCOMPARE(create.first().first().toJsonObject().value(QStringLiteral("device_id")).toString(),
             QStringLiteral("phone-install"));
    QJsonObject remote = device;
    remote.insert(QStringLiteral("id"), QStringLiteral("ses_phone"));
    remote.insert(QStringLiteral("title"), QStringLiteral("手机对话"));
    sessions.setRemoteSessions(QJsonArray{remote});
    QCOMPARE(list->count(), 3);
    QSignalSpy remove(&sessions, &AgentSessionList::remoteDeleteRequested);
    const QRect row = list->visualItemRect(list->item(2));
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, QPoint(row.right() - UiZoom::s(25), row.center().y()));
    auto* menu = sessions.findChild<QMenu*>(QStringLiteral("agentRemoteSessionMenu"));
    QVERIFY(menu);
    QVERIFY(menu->actions().size() == 1);
    menu->actions().first()->trigger();
    QCOMPARE(remove.size(), 1);
    QCOMPARE(remove.first().first().toJsonObject().value(QStringLiteral("id")).toString(), QStringLiteral("ses_phone"));
}

void DesktopSessionSyncHostTest::agentToolsReuseRemoteLifecycleCoordinator() {
    AgentController controller(AgentController::ModelConfig{}, {});
    QObject dispatcher;
    QJsonObject lastCommand;
    DesktopSessionSyncCoordinator* coordinatorForReply = nullptr;
    DesktopSessionSyncCoordinator coordinator(
        [&](const QString& path, const QByteArray&, const QJsonObject& body,
            DesktopSessionSyncCoordinator::Completion completion) {
            if (path == QStringLiteral("session-sync/send") &&
                body.value(QStringLiteral("kind")) == QStringLiteral("command")) {
                lastCommand = body;
                const QString id = body.value(QStringLiteral("client_message_id")).toString();
                const QString action =
                    body.value(QStringLiteral("payload")).toObject().value(QStringLiteral("action")).toString();
                const QString targetSlot = body.value(QStringLiteral("to_slot")).toString();
                const QString targetLogin = body.value(QStringLiteral("to_login_session_id")).toString();
                const QString resultId = action == QStringLiteral("session_create") ? QStringLiteral("ses_created")
                                         : targetSlot == QStringLiteral("windows")  ? QStringLiteral("ses_windows")
                                                                                    : QStringLiteral("ses_phone");
                const QJsonObject result =
                    action == QStringLiteral("session_set_workspace") && targetSlot == QStringLiteral("ios_phone")
                        ? QJsonObject{{QStringLiteral("ok"), false},
                                      {QStringLiteral("error"),
                                       QStringLiteral("iPhone does not support Workspace switching")}}
                        : QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("session_id"), resultId}};
                QTimer::singleShot(0, &dispatcher, [&, id, result, targetSlot, targetLogin] {
                    if (!coordinatorForReply) return;
                    coordinatorForReply->acceptInbox(
                        QJsonArray{QJsonObject{{QStringLiteral("id"), 7},
                                               {QStringLiteral("kind"), QStringLiteral("event")},
                                               {QStringLiteral("session_id"), QStringLiteral("session_lifecycle")},
                                               {QStringLiteral("from_slot"), targetSlot},
                                               {QStringLiteral("from_login_session_id"), targetLogin},
                                               {QStringLiteral("payload"),
                                                QJsonObject{{QStringLiteral("type"), QStringLiteral("command_result")},
                                                            {QStringLiteral("command_id"), id},
                                                            {QStringLiteral("result"), result}}}}});
                });
            }
            QTimer::singleShot(0, &dispatcher, [completion = std::move(completion)] { completion({}, {}); });
        },
        [&] { return &controller; }, nullptr, QStringLiteral("mac-install"), QStringLiteral("Mac"));
    coordinatorForReply = &coordinator;
    const QJsonObject device{{QStringLiteral("device_slot"), QStringLiteral("ios_phone")},
                             {QStringLiteral("device_name"), QStringLiteral("iPhone")},
                             {QStringLiteral("device_id"), QStringLiteral("phone-install")},
                             {QStringLiteral("login_session_id"), QStringLiteral("phone-login")},
                             {QStringLiteral("online"), true}};
    QJsonObject remote = device;
    remote.insert(QStringLiteral("id"), QStringLiteral("ses_phone"));
    remote.insert(QStringLiteral("title"), QStringLiteral("手机对话"));
    coordinator.acceptDevices(QJsonArray{device});
    coordinator.acceptCatalog(QJsonArray{remote});
    MaiToolRegistry registry;
    registerDesktopRemoteSessionTools(registry, &dispatcher, [&] { return &coordinator; });
    MaiTool* list = registry.findForModel("maichat_remote_session_list");
    MaiTool* create = registry.findForModel("maichat_remote_session_create");
    MaiTool* remove = registry.findForModel("maichat_remote_session_delete");
    MaiTool* workspace = registry.findForModel("maichat_remote_session_set_workspace");
    QVERIFY(list && create && remove && workspace);
    QVERIFY(!list->requiresApproval("{}"));
    QVERIFY(!create->requiresApproval("{}"));
    QVERIFY(!create->requiresPerCallApproval("{}"));
    QVERIFY(remove->requiresApproval("{}"));
    QVERIFY(remove->requiresPerCallApproval("{}"));
    QVERIFY(workspace->requiresApproval("{}"));
    QVERIFY(workspace->requiresPerCallApproval("{}"));
    MaiToolContext context;
    const MaiToolResult listed = list->execute("{}", context);
    QVERIFY(!listed.hasError());
    const QJsonObject listing = QJsonDocument::fromJson(QByteArray::fromStdString(listed.output())).object();
    QCOMPARE(listing.value(QStringLiteral("devices")).toArray().size(), 1);
    QCOMPARE(listing.value(QStringLiteral("sessions"))
                 .toArray()
                 .first()
                 .toObject()
                 .value(QStringLiteral("session_id"))
                 .toString(),
             QStringLiteral("ses_phone"));
    auto created = QtConcurrent::run([&] { return create->execute(R"({"device_slot":"ios_phone"})", context); });
    QTRY_VERIFY_WITH_TIMEOUT(created.isFinished(), 5000);
    const MaiToolResult createdResult = created.result();
    QVERIFY2(!createdResult.hasError(), createdResult.error().message().c_str());
    QCOMPARE(QJsonDocument::fromJson(QByteArray::fromStdString(createdResult.output()))
                 .object()
                 .value(QStringLiteral("session_id"))
                 .toString(),
             QStringLiteral("ses_created"));
    QVERIFY(!lastCommand.value(QStringLiteral("payload")).toObject().contains(QStringLiteral("workspace_path")));
    auto removed = QtConcurrent::run(
        [&] { return remove->execute(R"({"device_slot":"ios_phone","session_id":"ses_phone"})", context); });
    QTRY_VERIFY_WITH_TIMEOUT(removed.isFinished(), 5000);
    const MaiToolResult removedResult = removed.result();
    QVERIFY2(!removedResult.hasError(), removedResult.error().message().c_str());
    QCOMPARE(lastCommand.value(QStringLiteral("payload")).toObject().value(QStringLiteral("action")).toString(),
             QStringLiteral("session_delete"));
    QCOMPARE(
        lastCommand.value(QStringLiteral("payload")).toObject().value(QStringLiteral("target_session_id")).toString(),
        QStringLiteral("ses_phone"));
    auto unsupported = QtConcurrent::run([&] {
        return workspace->execute(
            R"({"device_slot":"ios_phone","session_id":"ses_phone","workspace_path":"/tmp/other"})", context);
    });
    QTRY_VERIFY_WITH_TIMEOUT(unsupported.isFinished(), 5000);
    const MaiToolResult unsupportedResult = unsupported.result();
    QVERIFY(unsupportedResult.hasError());
    QCOMPARE(QString::fromStdString(unsupportedResult.error().message()),
             QStringLiteral("iPhone does not support Workspace switching"));
    QCOMPARE(lastCommand.value(QStringLiteral("payload")).toObject().value(QStringLiteral("action")).toString(),
             QStringLiteral("session_set_workspace"));

    QJsonObject windowsDevice{{QStringLiteral("device_slot"), QStringLiteral("windows")},
                              {QStringLiteral("device_name"), QStringLiteral("Windows PC")},
                              {QStringLiteral("device_id"), QStringLiteral("windows-install")},
                              {QStringLiteral("login_session_id"), QStringLiteral("windows-login")},
                              {QStringLiteral("online"), true}};
    QJsonObject windowsSession = windowsDevice;
    windowsSession.insert(QStringLiteral("id"), QStringLiteral("ses_windows"));
    windowsSession.insert(QStringLiteral("title"), QStringLiteral("Desktop conversation"));
    coordinator.acceptDevices(QJsonArray{device, windowsDevice});
    coordinator.acceptCatalog(QJsonArray{remote, windowsSession});
    auto changed = QtConcurrent::run([&] {
        return workspace->execute(
            R"({"device_slot":"windows","session_id":"ses_windows","workspace_path":"C:\\Projects\\Existing"})",
            context);
    });
    QTRY_VERIFY_WITH_TIMEOUT(changed.isFinished(), 5000);
    const MaiToolResult changedResult = changed.result();
    QVERIFY2(!changedResult.hasError(), changedResult.error().message().c_str());
    QCOMPARE(lastCommand.value(QStringLiteral("to_slot")).toString(), QStringLiteral("windows"));
    QCOMPARE(lastCommand.value(QStringLiteral("payload")).toObject().value(QStringLiteral("action")).toString(),
             QStringLiteral("session_set_workspace"));
    QCOMPARE(QJsonDocument::fromJson(QByteArray::fromStdString(changedResult.output()))
                 .object()
                 .value(QStringLiteral("workspace_path"))
                 .toString(),
             QStringLiteral("C:\\Projects\\Existing"));
}

void DesktopSessionSyncHostTest::remotePermissionButtonsUseSharedWireDecisions() {
    const QJsonObject remote{{QStringLiteral("id"), QStringLiteral("ses_remote")},
                             {QStringLiteral("device_slot"), QStringLiteral("ios_phone")},
                             {QStringLiteral("login_session_id"), QStringLiteral("phone-login")},
                             {QStringLiteral("online"), true}};
    const QJsonArray permissions{QJsonObject{{QStringLiteral("id"), QStringLiteral("per_one")},
                                             {QStringLiteral("tool"), QStringLiteral("file_write")},
                                             {QStringLiteral("allowForSession"), true}}};
    const QList<QPair<QString, QString>> cases{{QStringLiteral("允许一次"), QStringLiteral("approved")},
                                               {QStringLiteral("本次会话允许"), QStringLiteral("approved_for_session")},
                                               {QStringLiteral("拒绝"), QStringLiteral("denied")}};
    for (const auto& test : cases) {
        QTemporaryDir spool;
        QVERIFY(spool.isValid());
        RemoteAgentPanel panel(nullptr, spool.filePath(QStringLiteral("outbox.json")));
        QJsonObject sent;
        panel.setHistoryHandler([&](const QJsonObject&, const QString&, int, RemoteAgentPanel::Completion completion) {
            panel.receivePayload(remote, QStringLiteral("history_page"),
                                 {{QStringLiteral("ok"), true},
                                  {QStringLiteral("messages"), QJsonArray{}},
                                  {QStringLiteral("permissions"), permissions}});
            completion({});
        });
        panel.setCommandHandler(
            [&](const QJsonObject&, const QJsonObject& payload, RemoteAgentPanel::Completion completion) {
                sent = payload;
                completion({});
            });
        panel.show();
        panel.openSession(remote);
        QPushButton* decision = nullptr;
        for (QPushButton* button : panel.findChildren<QPushButton*>())
            if (button->text() == test.first) decision = button;
        QVERIFY(decision != nullptr);
        QTest::mouseClick(decision, Qt::LeftButton);
        QCOMPARE(sent.value(QStringLiteral("action")).toString(), QStringLiteral("permission"));
        QCOMPARE(sent.value(QStringLiteral("decision")).toString(), test.second);
    }
    AgentController controller(AgentController::ModelConfig{}, {});
    const QString sessionId = controller.createSession({}, QStringLiteral("权限测试"));
    DesktopSessionSyncHost host(controller);
    for (const auto& test : cases) {
        const QJsonObject result =
            host.applyCommand(sessionId, {{QStringLiteral("action"), QStringLiteral("permission")},
                                          {QStringLiteral("id"), QStringLiteral("per_missing")},
                                          {QStringLiteral("decision"), test.second}});
        QVERIFY(!result.value(QStringLiteral("ok")).toBool());
        QVERIFY(result.value(QStringLiteral("error")).toString() != QStringLiteral("权限决定无效"));
    }
}

void DesktopSessionSyncHostTest::appliesRemotePromptToTheExistingLocalSession() {
    AgentController::ModelConfig model;
    AgentController controller(model, {});
    const QString sessionId = controller.createSession({}, QStringLiteral("编译项目"));
    QVERIFY(!sessionId.isEmpty());
    DesktopSessionSyncHost host(controller);
    const QJsonArray sessions = host.rootSessions();
    QCOMPARE(sessions.size(), 1);
    QCOMPARE(sessions.first().toObject().value(QStringLiteral("id")).toString(), sessionId);
    QCOMPARE(sessions.first().toObject().value(QStringLiteral("title")).toString(), QStringLiteral("编译项目"));
    QVERIFY(!host.applyCommand(sessionId, {{QStringLiteral("action"), QStringLiteral("unknown")}})
                 .value(QStringLiteral("ok"))
                 .toBool());
    QCOMPARE(host.applyCommand(sessionId, {{QStringLiteral("action"), QStringLiteral("permission")},
                                           {QStringLiteral("id"), QStringLiteral("per_test")},
                                           {QStringLiteral("decision"), QStringLiteral("grant_everything")}})
                 .value(QStringLiteral("error"))
                 .toString(),
             QStringLiteral("权限决定无效"));
    QVERIFY(host.applyCommand(sessionId, {{QStringLiteral("action"), QStringLiteral("prompt")},
                                          {QStringLiteral("text"), QStringLiteral("继续检查编译错误")}})
                .value(QStringLiteral("ok"))
                .toBool());
    const QJsonObject page = host.historyPage(sessionId, {}, 20);
    QVERIFY(page.value(QStringLiteral("ok")).toBool());
    QVERIFY(page.value(QStringLiteral("permissions")).isArray());
    QVERIFY(page.value(QStringLiteral("questions")).isArray());
    const QJsonArray messages = page.value(QStringLiteral("messages")).toArray();
    QVERIFY(!messages.isEmpty());
    bool sawPrompt = false;
    for (const QJsonValue& value : messages) {
        const QJsonObject message = value.toObject();
        if (message.value(QStringLiteral("role")).toString() != QStringLiteral("user")) continue;
        for (const QJsonValue& part : message.value(QStringLiteral("parts")).toArray())
            sawPrompt |= part.toObject().value(QStringLiteral("text")).toString() == QStringLiteral("继续检查编译错误");
    }
    QVERIFY(sawPrompt);
    QCOMPARE(host.historyPage(QStringLiteral("ses_missing"), {}, 20).value(QStringLiteral("ok")).toBool(), false);
}

void DesktopSessionSyncHostTest::remotePromptCreatesScheduledTaskOnItsOwner() {
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    const QString database = QDir(workspace.path()).filePath(QStringLiteral("sessions.sqlite"));
    const QString when =
        QDateTime::currentDateTimeUtc().addSecs(300).toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'"));
    const QJsonObject arguments{{QStringLiteral("title"), QStringLiteral("远程定时检查")},
                                {QStringLiteral("instruction"), QStringLiteral("检查桌面构建并总结")},
                                {QStringLiteral("execute_at"), when}};
    const QByteArray encoded = QJsonDocument(arguments).toJson(QJsonDocument::Compact);
    auto model = std::make_unique<ScheduledToolModel>(std::string(encoded.constData(), std::size_t(encoded.size())));
    AgentController controller(std::move(model), database);
    const QString sessionId = controller.createSession(workspace.path());
    QVERIFY(!sessionId.isEmpty());
    DesktopSessionSyncHost host(controller);
    const auto opened = MaiSessionScheduleStore::open(database.toUtf8().toStdString() + ".schedule.sqlite");
    QVERIFY(opened.isOk());

    const QJsonObject sent =
        host.applyCommand(sessionId, {{QStringLiteral("action"), QStringLiteral("prompt")},
                                      {QStringLiteral("text"), QStringLiteral("五分钟后检查桌面构建")}});
    QVERIFY2(sent.value(QStringLiteral("ok")).toBool(), qPrintable(sent.value(QStringLiteral("error")).toString()));
    QTRY_COMPARE_WITH_TIMEOUT(controller.agent().listPendingPermissions().size(), std::size_t(1), 5000);
    const QJsonArray permissions = host.historyPage(sessionId, {}, 20).value(QStringLiteral("permissions")).toArray();
    QCOMPARE(permissions.size(), 1);
    QCOMPARE(permissions.first().toObject().value(QStringLiteral("tool")).toString(),
             QStringLiteral("maichat_session_task_create"));
    QVERIFY(opened.value()->list(sessionId.toStdString()).value().empty());
    const QString permissionId = permissions.first().toObject().value(QStringLiteral("id")).toString();
    QVERIFY(!permissionId.isEmpty());
    const QJsonObject approved =
        host.applyCommand(sessionId, {{QStringLiteral("action"), QStringLiteral("permission")},
                                      {QStringLiteral("id"), permissionId},
                                      {QStringLiteral("decision"), QStringLiteral("approved")}});
    QVERIFY2(approved.value(QStringLiteral("ok")).toBool(),
             qPrintable(approved.value(QStringLiteral("error")).toString()));
    controller.agent().waitIdle();
    const auto tasks = opened.value()->list(sessionId.toStdString());
    QVERIFY(tasks.isOk());
    QCOMPARE(tasks.value().size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(tasks.value().front().status), QStringLiteral("scheduled"));
    QCOMPARE(QString::fromStdString(tasks.value().front().instruction), QStringLiteral("检查桌面构建并总结"));
}

void DesktopSessionSyncHostTest::remotePanelSendsMoreBeforePriorReceiptAndStopsSeparately() {
    QTemporaryDir panelSpool;
    RemoteAgentPanel panel(nullptr, panelSpool.filePath(QStringLiteral("outbox.json")));
    const QJsonObject remote{{QStringLiteral("id"), QStringLiteral("ses_queue")},
                             {QStringLiteral("device_slot"), QStringLiteral("mac")},
                             {QStringLiteral("login_session_id"), QStringLiteral("owner_login")},
                             {QStringLiteral("online"), true},
                             {QStringLiteral("busy"), true}};
    QList<QJsonObject> commands;
    panel.setCommandHandler([&](const QJsonObject&, const QJsonObject& payload, RemoteAgentPanel::Completion done) {
        commands.append(payload);
        done({});
    });
    panel.setHistoryHandler([&](const QJsonObject&, const QString&, int, RemoteAgentPanel::Completion done) {
        panel.receivePayload(remote, QStringLiteral("history_page"),
                             {{QStringLiteral("ok"), true}, {QStringLiteral("messages"), QJsonArray{}}});
        done({});
    });
    panel.openSession(remote);
    panel.show();
    auto* editor = panel.findChild<QTextEdit*>(QStringLiteral("remoteAgentComposer"));
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("remoteAgentSendButton"));
    auto* stop = panel.findChild<QPushButton*>(QStringLiteral("remoteAgentStopButton"));
    QVERIFY(editor && send && stop && stop->isVisible());
    for (const auto& text :
         {QStringLiteral("Same message"), QStringLiteral("Same message"), QStringLiteral("Third message")}) {
        editor->setPlainText(text);
        QVERIFY(send->isEnabled());
        QTest::mouseClick(send, Qt::LeftButton);
    }
    QCOMPARE(commands.size(), 3);
    QSet<QString> ids;
    for (const auto& command : commands) {
        QCOMPARE(command.value(QStringLiteral("action")).toString(), QStringLiteral("prompt"));
        QVERIFY(!command.contains(QStringLiteral("turnId")));
        ids.insert(command.value(QStringLiteral("requestId")).toString());
    }
    QCOMPARE(ids.size(), 3);
    QJsonObject stoppable = remote;
    stoppable.insert(QStringLiteral("turnId"), QStringLiteral("msg_known_task"));
    panel.updateSession(stoppable);
    QTest::mouseClick(stop, Qt::LeftButton);
    QCOMPARE(commands.last().value(QStringLiteral("action")).toString(), QStringLiteral("stop"));
    editor->setPlainText(QStringLiteral("A fresh draft"));
    panel.receivePayload(
        remote, QStringLiteral("event"),
        {{QStringLiteral("type"), QStringLiteral("command_result")},
         {QStringLiteral("request_id"), commands[1].value(QStringLiteral("requestId"))},
         {QStringLiteral("result"), QJsonObject{{QStringLiteral("ok"), true},
                                                {QStringLiteral("user_message_id"), QStringLiteral("msg_second")}}}});
    QCOMPARE(editor->toPlainText(), QStringLiteral("A fresh draft"));
    QVERIFY(send->isEnabled());
    auto* transcript = panel.findChild<MarkdownView*>();
    QVERIFY(transcript && transcript->contains(QStringLiteral("msg_second")));
}

void DesktopSessionSyncHostTest::remotePromptQueuesWhileOwnerIsBusy() {
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    auto model = std::make_unique<HeldSteeringModel>();
    auto* held = model.get();
    AgentController controller(std::move(model), {});
    const auto release = qScopeGuard([&] { held->release.store(true); });
    const QString session = controller.createSession(workspace.path());
    DesktopSessionSyncHost host(controller);
    const QJsonObject first{{QStringLiteral("action"), QStringLiteral("prompt")},
                            {QStringLiteral("requestId"), QStringLiteral("input_first")},
                            {QStringLiteral("text"), QStringLiteral("First task")}};
    QVERIFY(host.applyCommand(session, first).value(QStringLiteral("ok")).toBool());
    QTRY_VERIFY_WITH_TIMEOUT(held->entered.load(), 3000);
    const QJsonObject second{{QStringLiteral("action"), QStringLiteral("prompt")},
                             {QStringLiteral("requestId"), QStringLiteral("input_second")},
                             {QStringLiteral("text"), QStringLiteral("Second input while busy")}};
    QElapsedTimer timer;
    timer.start();
    const QJsonObject accepted = host.applyCommand(session, second);
    QVERIFY2(accepted.value(QStringLiteral("ok")).toBool(),
             qPrintable(accepted.value(QStringLiteral("error")).toString()));
    QVERIFY(timer.elapsed() < 1000);
    const QString secondId = accepted.value(QStringLiteral("user_message_id")).toString();
    QVERIFY(!secondId.isEmpty());
    QCOMPARE(host.applyCommand(session, second).value(QStringLiteral("user_message_id")).toString(), secondId);
    QJsonObject third = second;
    third.insert(QStringLiteral("requestId"), QStringLiteral("input_third"));
    third.insert(QStringLiteral("text"), QStringLiteral("Third input while busy"));
    QVERIFY(host.applyCommand(session, third).value(QStringLiteral("ok")).toBool());
    QVERIFY(controller.agent().isBusy(session.toStdString()));
    held->release.store(true);
    controller.agent().waitIdle();
    int users = 0;
    for (const MaiMessage& message : controller.agent().listMessages(session.toStdString()))
        if (message.role == MaiRole::User) ++users;
    QCOMPARE(users, 3);
    QCOMPARE(host.applyCommand(session, second).value(QStringLiteral("user_message_id")).toString(), secondId);
}

void DesktopSessionSyncHostTest::remoteSteeringBindsToTheOriginalOwnerTurn() {
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    auto model = std::make_unique<HeldSteeringModel>();
    auto* held = model.get();
    AgentController controller(std::move(model), {});
    const QString session = controller.createSession(workspace.path());
    DesktopSessionSyncHost host(controller);
    QVERIFY(host.applyCommand(session, {{QStringLiteral("action"), QStringLiteral("prompt")},
                                        {QStringLiteral("text"), QStringLiteral("Prepare an image report")}})
                .value(QStringLiteral("ok"))
                .toBool());
    QTRY_VERIFY_WITH_TIMEOUT(held->entered.load(), 3000);
    const QJsonObject busyPage = host.historyPage(session, {}, 20);
    QVERIFY(busyPage.value(QStringLiteral("busy")).toBool());
    const QString turn = busyPage.value(QStringLiteral("turnId")).toString();
    QVERIFY(!turn.isEmpty());
    QCOMPARE(host.rootSessions().first().toObject().value(QStringLiteral("turnId")).toString(), turn);
    const QJsonObject supplement{{QStringLiteral("action"), QStringLiteral("steer")},
                                 {QStringLiteral("text"), QStringLiteral("Keep the original detail")},
                                 {QStringLiteral("turnId"), turn},
                                 {QStringLiteral("requestId"), QStringLiteral("fixture_supplement_request")}};
    const QJsonObject accepted = host.applyCommand(session, supplement);
    QVERIFY2(accepted.value(QStringLiteral("ok")).toBool(),
             qPrintable(accepted.value(QStringLiteral("error")).toString()));
    const QString user = accepted.value(QStringLiteral("user_message_id")).toString();
    QVERIFY(!user.isEmpty());
    QCOMPARE(host.applyCommand(session, supplement).value(QStringLiteral("user_message_id")).toString(), user);
    QCOMPARE(host.historyPage(session, {}, 20).value(QStringLiteral("turnId")).toString(), turn);
    QVERIFY(controller.agent().isBusy(session.toStdString()));
    QVERIFY(host.applyCommand(session,
                              {{QStringLiteral("action"), QStringLiteral("prompt")},
                               {QStringLiteral("text"), QStringLiteral("Queue without replacing the running task")}})
                .value(QStringLiteral("ok"))
                .toBool());
    QCOMPARE(host.historyPage(session, {}, 20).value(QStringLiteral("turnId")).toString(), turn);
    QJsonObject invalid = supplement;
    invalid.insert(QStringLiteral("requestId"), QStringLiteral("fixture_wrong_turn"));
    invalid.insert(QStringLiteral("turnId"), QStringLiteral("msg_other_turn"));
    QVERIFY(!host.applyCommand(session, invalid).value(QStringLiteral("ok")).toBool());
    held->release.store(true);
    controller.agent().waitIdle();
    QVERIFY(!host.historyPage(session, {}, 20).value(QStringLiteral("busy")).toBool());
    // A lost receipt can be retried after completion, but a new request must not
    // silently create another task on the now-idle owner.
    QCOMPARE(host.applyCommand(session, supplement).value(QStringLiteral("user_message_id")).toString(), user);
    QJsonObject idle = supplement;
    idle.insert(QStringLiteral("requestId"), QStringLiteral("fixture_new_idle_request"));
    QVERIFY(!host.applyCommand(session, idle).value(QStringLiteral("ok")).toBool());
    int matches = 0;
    for (const MaiMessage& message : controller.agent().listMessages(session.toStdString()))
        if (message.id == user.toStdString()) ++matches;
    QCOMPARE(matches, 1);
}

void DesktopSessionSyncHostTest::resolvedSessionFileStaysInsideItsOwnerWorkspace() {
    QTemporaryDir workspace;
    QTemporaryDir outside;
    QVERIFY(workspace.isValid());
    QVERIFY(outside.isValid());
    AgentController controller(AgentController::ModelConfig{}, {});
    const QString sessionId = controller.createSession(workspace.path());
    QVERIFY(!sessionId.isEmpty());
    DesktopSessionSyncHost host(controller);
    const QString folder = QDir(workspace.path()).filePath(QStringLiteral(".maichat/session-attachments"));
    QVERIFY(QDir().mkpath(folder));
    const QString path = QDir(folder).filePath(QStringLiteral("report.txt"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("session attachment"), qint64(18));
    file.close();
    const QJsonObject valid{
        {QStringLiteral("action"), QStringLiteral("prompt")},
        {QStringLiteral("text"), QString()},
        {QStringLiteral("resolved_attachments"),
         QJsonArray{QJsonObject{{QStringLiteral("path"), path}, {QStringLiteral("kind"), QStringLiteral("file")}}}}};
    const QJsonObject accepted = host.applyCommand(sessionId, valid);
    QVERIFY2(accepted.value(QStringLiteral("ok")).toBool(),
             qPrintable(accepted.value(QStringLiteral("error")).toString()));
    const QJsonArray history = host.historyPage(sessionId, {}, 2).value(QStringLiteral("messages")).toArray();
    QVERIFY(!history.isEmpty());
    bool found = false;
    for (const QJsonValue& value : history) {
        for (const QJsonValue& part : value.toObject().value(QStringLiteral("parts")).toArray())
            found |= part.toObject()
                         .value(QStringLiteral("text"))
                         .toString()
                         .contains(QStringLiteral(".maichat/session-attachments/report.txt"));
    }
    QVERIFY(found);
    QFile foreign(QDir(outside.path()).filePath(QStringLiteral("private.txt")));
    QVERIFY(foreign.open(QIODevice::WriteOnly));
    foreign.write("private");
    foreign.close();
    QJsonObject invalid = valid;
    invalid.insert(QStringLiteral("resolved_attachments"),
                   QJsonArray{QJsonObject{{QStringLiteral("path"), foreign.fileName()},
                                          {QStringLiteral("kind"), QStringLiteral("file")}}});
    QCOMPARE(host.applyCommand(sessionId, invalid).value(QStringLiteral("error")).toString(),
             QStringLiteral("会话附件路径无效"));
}

void DesktopSessionSyncHostTest::omitsLocalMediaPathsFromRemoteHistory() {
    AgentController::ModelConfig model;
    AgentController controller(model, {});
    const QString sessionId = controller.createSession({}, QStringLiteral("照片"));
    QVERIFY(!sessionId.isEmpty());
    const std::vector<MaiModelImage> images{{"/private/workspace/secret-photo.png", "image/png"}};
    const auto sent = controller.agent().submit(MaiSendPrompt{sessionId.toStdString(), "Describe this", images});
    QVERIFY(sent);
    DesktopSessionSyncHost host(controller);
    const QJsonArray messages = host.historyPage(sessionId, {}, 20).value(QStringLiteral("messages")).toArray();
    bool sawImage = false;
    for (const QJsonValue& message : messages) {
        for (const QJsonValue& part : message.toObject().value(QStringLiteral("parts")).toArray()) {
            const QJsonObject image = part.toObject();
            if (image.value(QStringLiteral("kind")).toString() != QStringLiteral("image")) continue;
            sawImage = true;
            QCOMPARE(image.value(QStringLiteral("filename")).toString(), QStringLiteral("secret-photo.png"));
            QVERIFY(!image.contains(QStringLiteral("path")));
            QVERIFY(!image.value(QStringLiteral("remotePartId")).toString().isEmpty());
        }
    }
    QVERIFY(sawImage);
}

void DesktopSessionSyncHostTest::projectsOnlySafeCurlFailureSummaries_data() {
    QTest::addColumn<QString>("toolName");
    QTest::addColumn<int>("toolState");
    QTest::addColumn<QString>("storedError");
    QTest::addColumn<QString>("expectedSummary");

    // 远端只需要 curl 的退出码分类；日志里的请求头、签名地址和凭据不能成为正文。
    const QString privateLog = QStringLiteral(
        "Authorization: Bearer FAKE_BEARER_SECRET\n"
        "https://private.example.test/download?signature=FAKE_SIGNATURE&token=FAKE_URL_TOKEN\n"
        "password=FAKE_PASSWORD token=FAKE_TOKEN private response body");
    const auto curlError = [&privateLog](const QJsonValue& exitStatus) {
        return QString::fromUtf8(QJsonDocument(QJsonObject{
            {QStringLiteral("exit_status"), exitStatus},
            {QStringLiteral("log_tail"), privateLog},
            {QStringLiteral("log_truncated"), true}}).toJson(QJsonDocument::Compact));
    };
    const auto addCurl = [&curlError](const char* name, const QJsonValue& code, const QString& summary) {
        QTest::newRow(name) << QStringLiteral("curl") << int(MaiToolState::Error) << curlError(code) << summary;
    };
    addCurl("certificate", 60, QStringLiteral("curl 失败（错误码 60）：证书验证失败"));
    addCurl("ca-file", 77, QStringLiteral("curl 失败（错误码 77）：无法加载 CA 证书"));
    addCurl("dns", 6, QStringLiteral("curl 失败（错误码 6）：无法解析主机名"));
    addCurl("connection", 7, QStringLiteral("curl 失败（错误码 7）：无法连接服务器"));
    addCurl("timeout", 28, QStringLiteral("curl 失败（错误码 28）：请求超时"));
    addCurl("other-valid-code", 42, QStringLiteral("curl 执行失败（错误码 42）"));
    addCurl("string-code", QStringLiteral("60"), {});
    addCurl("boolean-code", true, {});
    addCurl("fractional-code", 60.5, {});
    addCurl("success-code", 0, {});
    addCurl("negative-code", -60, {});
    addCurl("out-of-range-code", 256, {});
    addCurl("huge-code", 1.0e30, {});
    addCurl("null-code", QJsonValue(QJsonValue::Null), {});
    QTest::newRow("missing-code") << QStringLiteral("curl") << int(MaiToolState::Error)
                                  << QString::fromUtf8(QJsonDocument(QJsonObject{
                                         {QStringLiteral("log_tail"), privateLog}}).toJson(QJsonDocument::Compact))
                                  << QString();
    QTest::newRow("non-object-error") << QStringLiteral("curl") << int(MaiToolState::Error)
                                      << QStringLiteral("[60]") << QString();
    QTest::newRow("unstructured-error") << QStringLiteral("curl") << int(MaiToolState::Error)
                                        << privateLog << QString();
    QTest::newRow("completed-curl") << QStringLiteral("curl") << int(MaiToolState::Completed)
                                    << curlError(60) << QString();
    QTest::newRow("canceled-curl") << QStringLiteral("curl") << int(MaiToolState::Canceled)
                                   << curlError(60) << QString();
    QTest::newRow("other-tool") << QStringLiteral("shell") << int(MaiToolState::Error)
                                << curlError(60) << QString();
}

void DesktopSessionSyncHostTest::projectsOnlySafeCurlFailureSummaries() {
    QFETCH(QString, toolName);
    QFETCH(int, toolState);
    QFETCH(QString, storedError);
    QFETCH(QString, expectedSummary);
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    const QString database = workspace.filePath(QStringLiteral("curl-failure-summary.sqlite"));
    AgentController controller(AgentController::ModelConfig{}, database);
    const QString session = controller.createSession(workspace.path(), QStringLiteral("Curl failure fixture"));
    QVERIFY(!session.isEmpty());
    MaiMessage message;
    message.id = MaiIdGenerator::newMessageId();
    message.role = MaiRole::Assistant;
    message.created = 1000;
    message.completed = 2000;
    MaiToolPart curl;
    curl.tool = toolName.toStdString();
    curl.state = static_cast<MaiToolState>(toolState);
    curl.input = "{\"Authorization\":\"Bearer FAKE_INPUT_SECRET\",\"password\":\"FAKE_INPUT_PASSWORD\"}";
    curl.output = "FAKE_OUTPUT_SECRET private response body";
    curl.error = storedError.toStdString();
    message.parts.push_back({MaiIdGenerator::newPartId(), curl, 1001});
    MaiToolPart ordinary = curl;
    ordinary.tool = "shell";
    ordinary.state = MaiToolState::Error;
    message.parts.push_back({MaiIdGenerator::newPartId(), ordinary, 1002});
    MaiToolPart question;
    question.tool = "question";
    question.state = MaiToolState::Error;
    question.input = R"({"question":"Use the bundled CA?"})";
    question.output = "Use the bundled CA";
    question.error = "Question fixture failed";
    message.parts.push_back({MaiIdGenerator::newPartId(), question, 1003});
    {
        // 直接写入私有持久化夹具，不启动模型、curl 或界面；两个正式历史入口读取同一条记录。
        auto store = makeMaiSqliteStore(database.toStdString());
        QVERIFY(store);
        store.value()->putMessage(session.toStdString(), message);
        QVERIFY(!store.value()->lastWriteError().hasError());
    }
    DesktopSessionSyncHost host(controller);
    const QJsonArray page = host.historyPage(session, {}, 20).value(QStringLiteral("messages")).toArray();
    QCOMPARE(page.size(), 1);
    const QList<QJsonObject> serialized{
        page.first().toObject(), host.historyMessage(session, QString::fromStdString(message.id))};
    for (const QJsonObject& snapshot : serialized) {
        const QByteArray wire = QJsonDocument(snapshot).toJson(QJsonDocument::Compact);
        for (const char* secret : {"FAKE_", "Authorization", "Bearer", "signature=", "password=", "token=",
                                   "private.example.test", "private response body", "log_tail", "exit_status"})
            QVERIFY2(!wire.contains(secret), secret);
        const QJsonArray parts = snapshot.value(QStringLiteral("parts")).toArray();
        QCOMPARE(parts.size(), 3);
        const QJsonObject projected = parts.first().toObject();
        QCOMPARE(projected.value(QStringLiteral("tool")).toString(), toolName);
        QCOMPARE(projected.value(QStringLiteral("state")).toString(),
                 QString::fromLatin1(maiToolStateToString(static_cast<MaiToolState>(toolState))));
        QVERIFY(!projected.contains(QStringLiteral("input")));
        QVERIFY(!projected.contains(QStringLiteral("output")));
        QCOMPARE(projected.value(QStringLiteral("error")).toString(), expectedSummary);
        QCOMPARE(projected.contains(QStringLiteral("error")), !expectedSummary.isEmpty());
        const QJsonObject filtered = parts.at(1).toObject();
        QVERIFY(!filtered.contains(QStringLiteral("input")));
        QVERIFY(!filtered.contains(QStringLiteral("output")));
        QVERIFY(!filtered.contains(QStringLiteral("error")));
        const QJsonObject retainedQuestion = parts.at(2).toObject();
        QCOMPARE(retainedQuestion.value(QStringLiteral("input")).toString(), QString::fromStdString(question.input));
        QCOMPARE(retainedQuestion.value(QStringLiteral("output")).toString(), QString::fromStdString(question.output));
        QCOMPARE(retainedQuestion.value(QStringLiteral("error")).toString(), QString::fromStdString(question.error));
    }
}

void DesktopSessionSyncHostTest::remotePanelDisplaysCurlFailureSummaryAndRefreshesIt() {
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    RemoteAgentPanel panel(nullptr, workspace.filePath(QStringLiteral("remote-outbox.json")));
    panel.resize(800, 500);
    const QJsonObject owner{{QStringLiteral("owner_device_id"), QStringLiteral("owned-curl-fixture")},
                             {QStringLiteral("session_id"), QStringLiteral("owned-curl-session")},
                             {QStringLiteral("online"), true}};
    panel.openSession(owner);
    QJsonObject tool{{QStringLiteral("id"), QStringLiteral("part_curl_failure")},
                      {QStringLiteral("kind"), QStringLiteral("tool")},
                      {QStringLiteral("tool"), QStringLiteral("curl")},
                      {QStringLiteral("state"), QStringLiteral("error")}};
    const auto snapshot = [&tool] {
        return QJsonObject{{QStringLiteral("id"), QStringLiteral("msg_curl_failure")},
                            {QStringLiteral("role"), QStringLiteral("assistant")},
                            {QStringLiteral("parts"), QJsonArray{tool}}};
    };
    panel.receivePayload(owner, QStringLiteral("history_page"),
                         {{QStringLiteral("ok"), true},
                          {QStringLiteral("messages"), QJsonArray{snapshot()}}});
    panel.show();
    const auto groups = panel.findChildren<QFrame*>(QStringLiteral("remoteAgentToolGroup"));
    QCOMPARE(groups.size(), 1);
    auto* disclosure = groups.first()->findChild<QAbstractButton*>(QStringLiteral("remoteAgentToolGroupDisclosure"));
    QVERIFY(disclosure);
    disclosure->click();
    auto* detail = groups.first()->findChild<QLabel*>(QStringLiteral("remoteAgentToolDetail"));
    QVERIFY(detail && detail->isVisibleTo(groups.first()));
    QCOMPARE(detail->text(), QStringLiteral("curl · 失败"));
    // 同一 tool id/state 迟到的安全摘要也必须刷新，不能被旧三字段 fingerprint 吞掉。
    for (const QString& summary : {QStringLiteral("curl 失败（错误码 60）：证书验证失败"),
                                   QStringLiteral("curl 失败（错误码 77）：无法加载 CA 证书")}) {
        tool.insert(QStringLiteral("error"), summary);
        panel.receivePayload(owner, QStringLiteral("event"),
                             {{QStringLiteral("ok"), true},
                              {QStringLiteral("messages"), QJsonArray{snapshot()}}});
        QCOMPARE(panel.findChildren<QFrame*>(QStringLiteral("remoteAgentToolGroup")), groups);
        QCOMPARE(groups.first()->findChild<QLabel*>(QStringLiteral("remoteAgentToolDetail")), detail);
        QCOMPARE(detail->textFormat(), Qt::PlainText);
        QVERIFY(detail->text().contains(summary));
    }
    const QString capture = qEnvironmentVariable("MAICHAT_CAPTURE_CURL_FAILURE");
    if (!capture.isEmpty()) {
        QCoreApplication::processEvents();
        QVERIFY(panel.grab().save(capture));
    }
}

void DesktopSessionSyncHostTest::coordinatorKeepsInputOrderDuringAttachmentPreparation_data() {
    QTest::addColumn<bool>("stop");
    QTest::addColumn<bool>("replaceLogin");
    QTest::newRow("failed-import-then-text") << false << false;
    QTest::newRow("stop-bypasses-import-and-preserves-sender-content") << true << false;
    QTest::newRow("replacement-login-does-not-consume-old-inputs") << false << true;
}

void DesktopSessionSyncHostTest::coordinatorKeepsInputOrderDuringAttachmentPreparation() {
    QFETCH(bool, stop);
    QFETCH(bool, replaceLogin);
    QTemporaryDir workspace;
    auto model = std::make_unique<HeldSteeringModel>();
    auto* held = model.get();
    AgentController controller(std::move(model), {});
    const auto release = qScopeGuard([&] { held->release.store(true); });
    const QString session = controller.createSession(workspace.path(), QStringLiteral("Import input order"));
    QVERIFY(controller.agent().submit(MaiSendPrompt{session.toStdString(), "Original held task"}));
    QTRY_VERIFY_WITH_TIMEOUT(held->entered.load(), 3000);
    const QString turn = QString::fromStdString(controller.agent().activeTurnId(session.toStdString()));
    const auto hasSecond = [&] {
        for (const auto& message : controller.agent().listMessages(session.toStdString()))
            if (message.role == MaiRole::User)
                for (const auto& part : message.parts)
                    if (const auto* text = std::get_if<MaiTextPart>(&part.body))
                        if (text->text == "Second queued input") return true;
        return false;
    };
    QList<int> acknowledged;
    QString ownerLogin = QStringLiteral("owner_login");
    DesktopSessionSyncCoordinator coordinator(
        [&](const QString& path, const QByteArray&, const QJsonObject& body,
            DesktopSessionSyncCoordinator::Completion done) {
            if (path == QStringLiteral("session-sync/ack"))
                for (const auto& id : body.value(QStringLiteral("ids")).toArray()) acknowledged.append(id.toInt());
            done(path == QStringLiteral("session-sync/presence")
                     ? QJsonObject{{QStringLiteral("login_session_id"), ownerLogin}}
                     : QJsonObject{},
                 {});
        },
        [&] { return &controller; }, nullptr, QStringLiteral("input-order"), QStringLiteral("Mac"));
    const auto envelope = [&](int id, const QJsonObject& payload) {
        return QJsonObject{{QStringLiteral("id"), id},
                           {QStringLiteral("kind"), QStringLiteral("command")},
                           {QStringLiteral("session_id"), session},
                           {QStringLiteral("from_slot"), QStringLiteral("ios")},
                           {QStringLiteral("from_login_session_id"), QStringLiteral("sender_login")},
                           {QStringLiteral("client_message_id"), QStringLiteral("command_%1").arg(id)},
                           {QStringLiteral("payload"), payload}};
    };
    bool injected = false, textOvertookImport = false;
    coordinator.setAttachmentAccessProvider([&] {
        if (!injected) {
            injected = true;
            // Reentrant delivery precisely holds the first input before import.
            // No fake timing, provider request, or test-only production hook.
            coordinator.acceptInbox(
                QJsonArray{envelope(2, {{QStringLiteral("action"), QStringLiteral("prompt")},
                                        {QStringLiteral("text"), QStringLiteral("Second queued input")}})});
            textOvertookImport = hasSecond();
            if (replaceLogin) {
                ownerLogin = QStringLiteral("replacement_login");
                coordinator.connectionLost();
                coordinator.connectionReady();
            }
            if (stop)
                coordinator.acceptInbox(QJsonArray{envelope(
                    3, {{QStringLiteral("action"), QStringLiteral("stop")}, {QStringLiteral("turnId"), turn}})});
        }
        DesktopVideoCircleAccess access;
        access.accountId =
            QStringLiteral("fixture-account");  // Invalid credentials fail locally without a network call.
        return access;
    });
    coordinator.start();
    coordinator.connectionReady();
    coordinator.acceptInbox(
        QJsonArray{envelope(1, {{QStringLiteral("action"), QStringLiteral("prompt")},
                                {QStringLiteral("text"), QStringLiteral("First input with attachment")},
                                {QStringLiteral("attachments"),
                                 QJsonArray{QJsonObject{{QStringLiteral("attachment_id"),
                                                         QStringLiteral("123e4567-e89b-42d3-a456-426614174000")}}}}})});
    QVERIFY(injected);
    QVERIFY2(!textOvertookImport, "The later text reached Core before the prior attachment was prepared");
    if (stop) {
        QTRY_VERIFY_WITH_TIMEOUT(!controller.agent().isBusy(session.toStdString()), 3000);
        QTest::qWait(100);
        QVERIFY(!hasSecond());
        QVERIFY(acknowledged.contains(3));
    } else if (replaceLogin) {
        QTRY_VERIFY(acknowledged.contains(1) && acknowledged.contains(2));
        QVERIFY(!hasSecond());
    } else {
        QTRY_VERIFY_WITH_TIMEOUT(hasSecond(), 5000);
        QTRY_VERIFY(acknowledged.contains(1) && acknowledged.contains(2));
        QVERIFY(acknowledged.indexOf(1) < acknowledged.indexOf(2));
    }
}

void DesktopSessionSyncHostTest::coordinatorDropsPreparedInputWhenOwnerIsDestroyed() {
    QTemporaryDir workspace;
    auto owner = std::make_unique<AgentController>(AgentController::ModelConfig{}, QString{});
    const QString session = owner->createSession(workspace.path(), QStringLiteral("Replacement owner guard"));
    int replies = 0;
    DesktopSessionSyncCoordinator coordinator(
        [&](const QString& path, const QByteArray&, const QJsonObject&,
            DesktopSessionSyncCoordinator::Completion done) {
            if (path == QStringLiteral("session-sync/send")) ++replies;
            done(path == QStringLiteral("session-sync/presence")
                     ? QJsonObject{{QStringLiteral("login_session_id"), QStringLiteral("old_owner")}}
                     : QJsonObject{},
                 {});
        },
        [&] { return owner.get(); });
    coordinator.setAttachmentAccessProvider([&] {
        owner.reset();  // QPointer has already become null before the provider returns.
        DesktopVideoCircleAccess access;
        access.accountId = QStringLiteral("replacement_account");
        return access;
    });
    coordinator.start();
    coordinator.connectionReady();
    const QJsonObject attachment{
        {QStringLiteral("attachment_id"), QStringLiteral("123e4567-e89b-42d3-a456-426614174000")}};
    const QJsonObject input{{QStringLiteral("action"), QStringLiteral("prompt")},
                            {QStringLiteral("text"), QStringLiteral("Old owner input")},
                            {QStringLiteral("attachments"), QJsonArray{attachment}}};
    const QJsonObject envelope{{QStringLiteral("id"), 1},
                               {QStringLiteral("kind"), QStringLiteral("command")},
                               {QStringLiteral("session_id"), session},
                               {QStringLiteral("from_slot"), QStringLiteral("ios")},
                               {QStringLiteral("from_login_session_id"), QStringLiteral("sender")},
                               {QStringLiteral("client_message_id"), QStringLiteral("old-command")},
                               {QStringLiteral("payload"), input}};
    coordinator.acceptInbox(QJsonArray{envelope});
    QVERIFY(!owner);
    coordinator.connectionReady();  // Cannot reuse a host whose referenced controller was destroyed.
    QCOMPARE(replies, 0);
}

void DesktopSessionSyncHostTest::coordinatorRoutesPhoneCommandToTheExistingDesktopSession() {
    AgentController::ModelConfig model;
    AgentController controller(model, {});
    const QString sessionId = controller.createSession({}, QStringLiteral("桌面任务"));
    QVERIFY(!sessionId.isEmpty());
    int acknowledgements = 0;
    int presenceUpdates = 0;
    QList<QJsonObject> sent;
    DesktopSessionSyncCoordinator coordinator(
        [&](const QString& path, const QByteArray&, const QJsonObject& body,
            DesktopSessionSyncCoordinator::Completion completion) {
            QJsonObject response;
            if (path == QStringLiteral("session-sync/presence")) {
                ++presenceUpdates;
                QCOMPARE(body.value(QStringLiteral("sessions")).toArray().size(), 1);
                response = {{QStringLiteral("online"), true}};
            } else if (path == QStringLiteral("session-sync/send")) {
                sent.append(body);
                response = {{QStringLiteral("id"), sent.size()}};
            } else if (path == QStringLiteral("session-sync/ack")) {
                ++acknowledgements;
                response = {{QStringLiteral("acknowledged"), true}};
            }
            QTimer::singleShot(0, &controller,
                               [completion = std::move(completion), response] { completion(response, {}); });
        },
        [&] { return &controller; }, nullptr, QStringLiteral("desktop-test-install-id"), QStringLiteral("Test Mac"));
    coordinator.start();
    coordinator.connectionReady();
    coordinator.acceptInbox(QJsonArray{QJsonObject{
        {QStringLiteral("id"), 1},
        {QStringLiteral("kind"), QStringLiteral("command")},
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("from_slot"), QStringLiteral("ios_phone")},
        {QStringLiteral("from_login_session_id"), QStringLiteral("phone-login")},
        {QStringLiteral("client_message_id"), QStringLiteral("command-1")},
        {QStringLiteral("payload"), QJsonObject{{QStringLiteral("action"), QStringLiteral("prompt")},
                                                {QStringLiteral("text"), QStringLiteral("检查编译错误")}}}}});
    QTRY_VERIFY_WITH_TIMEOUT(presenceUpdates > 0 && acknowledgements >= 1 && sent.size() >= 1, 5000);
    bool sawResult = false;
    for (const QJsonObject& message : sent) {
        const QString kind = message.value(QStringLiteral("kind")).toString();
        const QJsonObject chunk = message.value(QStringLiteral("payload")).toObject();
        const QJsonDocument document = QJsonDocument::fromJson(
            QByteArray::fromBase64(chunk.value(QStringLiteral("data_base64")).toString().toLatin1()));
        if (kind == QStringLiteral("event") &&
            document.object().value(QStringLiteral("type")).toString() == QStringLiteral("command_result"))
            sawResult = true;
    }
    QVERIFY(sawResult);
}

void DesktopSessionSyncHostTest::coordinatorPublishesLocalMessagesToSharedRoom() {
    AgentController::ModelConfig model;
    AgentController controller(model, {});
    const QString sessionId = controller.createSession({}, QStringLiteral("共享任务"));
    QVERIFY(!sessionId.isEmpty());
    QList<QJsonObject> published;
    DesktopSessionSyncCoordinator coordinator(
        [&](const QString& path, const QByteArray&, const QJsonObject& body,
            DesktopSessionSyncCoordinator::Completion completion) {
            if (path == QStringLiteral("session-sync/publish")) published.append(body);
            QTimer::singleShot(0, &controller, [completion = std::move(completion)] { completion({}, {}); });
        },
        [&] { return &controller; }, nullptr, QStringLiteral("test-install"), QStringLiteral("Mac"));
    coordinator.start();
    coordinator.connectionReady();
    coordinator.acceptInbox(QJsonArray{
        QJsonObject{{QStringLiteral("id"), 1},
                    {QStringLiteral("kind"), QStringLiteral("command")},
                    {QStringLiteral("session_id"), sessionId},
                    {QStringLiteral("from_slot"), QStringLiteral("ios_phone")},
                    {QStringLiteral("from_login_session_id"), QStringLiteral("ios-login")},
                    {QStringLiteral("client_message_id"), QStringLiteral("test-command")},
                    {QStringLiteral("payload"), QJsonObject{{QStringLiteral("action"), QStringLiteral("prompt")},
                                                            {QStringLiteral("text"), QStringLiteral("你好")}}}}});
    QTRY_VERIFY_WITH_TIMEOUT(published.size() >= 2, 5000);
    bool sawUser = false;
    bool sawAssistant = false;
    for (const QJsonObject& item : published) {
        QCOMPARE(item.value(QStringLiteral("session_id")).toString(), sessionId);
        const QJsonArray messages = item.value(QStringLiteral("messages")).toArray();
        if (messages.isEmpty()) continue;
        const QString role = messages.first().toObject().value(QStringLiteral("role")).toString();
        sawUser |= role == QStringLiteral("user");
        sawAssistant |= role == QStringLiteral("assistant");
    }
    QVERIFY(sawUser && sawAssistant);
}

void DesktopSessionSyncHostTest::coordinatorBackfillsOlderMessagesIntoSharedRoom() {
    AgentController::ModelConfig model;
    AgentController controller(model, {});
    const QString sessionId = controller.createSession({}, QStringLiteral("长会话"));
    QVERIFY(!sessionId.isEmpty());
    for (int index = 0; index < 11; ++index) {
        QTRY_VERIFY_WITH_TIMEOUT(!controller.agent().isBusy(sessionId.toStdString()), 3000);
        const auto result =
            controller.agent().submit(MaiSendPrompt{sessionId.toStdString(), "prompt " + std::to_string(index)});
        QVERIFY(result);
    }
    QTRY_VERIFY_WITH_TIMEOUT(!controller.agent().isBusy(sessionId.toStdString()), 3000);
    const auto history = controller.agent().listMessagesPage(sessionId.toStdString(), "", 100);
    QVERIFY(history.size() > 20);
    const QString oldest = QString::fromStdString(history.back().id);
    QSet<QString> published;
    DesktopSessionSyncCoordinator coordinator(
        [&](const QString& path, const QByteArray&, const QJsonObject& body,
            DesktopSessionSyncCoordinator::Completion completion) {
            if (path == QStringLiteral("session-sync/publish"))
                for (const QJsonValue& value : body.value(QStringLiteral("messages")).toArray())
                    published.insert(value.toObject().value(QStringLiteral("id")).toString());
            QTimer::singleShot(0, &controller, [completion = std::move(completion)] { completion({}, {}); });
        },
        [&] { return &controller; }, nullptr, QStringLiteral("test-install"), QStringLiteral("Mac"));
    coordinator.start();
    coordinator.connectionReady();
    QTRY_VERIFY_WITH_TIMEOUT(published.contains(oldest), 15'000);
}

void DesktopSessionSyncHostTest::coordinatorDeletesRoomWhenLocalSessionIsRemoved() {
    AgentController::ModelConfig model;
    AgentController controller(model, {});
    const QString sessionId = controller.createSession({}, QStringLiteral("待删除"));
    QVERIFY(!sessionId.isEmpty());
    QString removedRoom;
    DesktopSessionSyncCoordinator coordinator(
        [&](const QString& path, const QByteArray&, const QJsonObject& body,
            DesktopSessionSyncCoordinator::Completion completion) {
            if (path == QStringLiteral("session-sync/room-delete"))
                removedRoom = body.value(QStringLiteral("session_id")).toString();
            QTimer::singleShot(0, &controller, [completion = std::move(completion)] { completion({}, {}); });
        },
        [&] { return &controller; }, nullptr, QStringLiteral("test-install"), QStringLiteral("Mac"));
    coordinator.start();
    coordinator.connectionReady();
    QCoreApplication::processEvents();
    QVERIFY(controller.agent().submit(MaiDeleteSession{sessionId.toStdString()}));
    QTRY_COMPARE_WITH_TIMEOUT(removedRoom, sessionId, 5000);
}

void DesktopSessionSyncHostTest::coordinatorReadsRemoteHistoryWithVerifiedChunks() {
    AgentController::ModelConfig model;
    AgentController controller(model, {});
    QVERIFY(!controller.createSession({}, QStringLiteral("本地任务")).isEmpty());
    const QJsonObject remote{{QStringLiteral("id"), QStringLiteral("ses_phone")},
                             {QStringLiteral("title"), QStringLiteral("手机任务")},
                             {QStringLiteral("device_slot"), QStringLiteral("ios_phone")},
                             {QStringLiteral("device_name"), QStringLiteral("iPhone")},
                             {QStringLiteral("device_id"), QStringLiteral("phone-install")},
                             {QStringLiteral("login_session_id"), QStringLiteral("phone-login")},
                             {QStringLiteral("online"), true}};
    QJsonArray catalog;
    QJsonObject received;
    QJsonObject sentCommand;
    QJsonObject historyRequest;
    int historyCalls = 0;
    int acknowledgements = 0;
    DesktopSessionSyncCoordinator coordinator(
        [&](const QString& path, const QByteArray&, const QJsonObject& body,
            DesktopSessionSyncCoordinator::Completion completion) {
            QJsonObject response;
            if (path == QStringLiteral("session-sync/send")) {
                sentCommand = body;
                response = {{QStringLiteral("id"), 1}};
            } else if (path == QStringLiteral("session-sync/history")) {
                ++historyCalls;
                historyRequest = body;
                response = {{QStringLiteral("ok"), true},
                            {QStringLiteral("messages"),
                             QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("msg_remote")}}}}};
            } else if (path == QStringLiteral("session-sync/ack")) {
                ++acknowledgements;
                response = {{QStringLiteral("acknowledged"), true}};
            }
            QTimer::singleShot(0, &controller,
                               [completion = std::move(completion), response] { completion(response, {}); });
        },
        [&] { return &controller; }, nullptr, QStringLiteral("mac-install"), QStringLiteral("Mac"));
    coordinator.setCatalogHandler([&](const QJsonArray& value) { catalog = value; });
    coordinator.setPayloadHandler([&](const QJsonObject&, const QString& kind, const QJsonObject& payload) {
        if (kind == QStringLiteral("history_page")) received = payload;
    });
    coordinator.start();
    coordinator.acceptCatalog(QJsonArray{remote});
    QCOMPARE(catalog.size(), 1);
    QString error;
    coordinator.requestRemoteHistory(catalog.first().toObject(), {}, 10,
                                     [&](const QJsonObject&, const QString& value) { error = value; });
    QTRY_VERIFY_WITH_TIMEOUT(received.value(QStringLiteral("ok")).toBool(), 5000);
    QVERIFY(error.isEmpty());
    QCOMPARE(historyRequest.value(QStringLiteral("owner_slot")).toString(), QStringLiteral("ios_phone"));
    QCOMPARE(
        received.value(QStringLiteral("messages")).toArray().first().toObject().value(QStringLiteral("id")).toString(),
        QStringLiteral("msg_remote"));
    QJsonObject offline = remote;
    offline.insert(QStringLiteral("online"), false);
    coordinator.acceptCatalog(QJsonArray{offline});
    coordinator.requestRemoteHistory(offline, {}, 10, [&](const QJsonObject&, const QString& value) { error = value; });
    QTRY_VERIFY_WITH_TIMEOUT(historyCalls == 2 && error.isEmpty(), 5000);
    coordinator.acceptCatalog(QJsonArray{remote});
    const QJsonObject update{
        {QStringLiteral("ok"), true},
        {QStringLiteral("messages"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("msg_pushed")}}}}};
    const QByteArray bytes = QJsonDocument(update).toJson(QJsonDocument::Compact);
    const QString digest = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    coordinator.acceptInbox(
        QJsonArray{QJsonObject{{QStringLiteral("id"), 2},
                               {QStringLiteral("kind"), QStringLiteral("event")},
                               {QStringLiteral("session_id"), QStringLiteral("ses_phone")},
                               {QStringLiteral("from_slot"), QStringLiteral("ios_phone")},
                               {QStringLiteral("from_login_session_id"), QStringLiteral("phone-login")},
                               {QStringLiteral("payload"),
                                QJsonObject{{QStringLiteral("transfer_id"), QStringLiteral("transfer-1")},
                                            {QStringLiteral("chunk_index"), 0},
                                            {QStringLiteral("chunk_count"), 1},
                                            {QStringLiteral("sha256"), digest},
                                            {QStringLiteral("data_base64"), QString::fromLatin1(bytes.toBase64())}}}}});
    QTRY_VERIFY_WITH_TIMEOUT(acknowledgements > 0, 5000);
    coordinator.sendRemoteCommand(
        catalog.first().toObject(),
        {{QStringLiteral("action"), QStringLiteral("prompt")}, {QStringLiteral("text"), QStringLiteral("继续")}},
        [&](const QJsonObject&, const QString& value) { error = value; });
    QTRY_VERIFY_WITH_TIMEOUT(!sentCommand.isEmpty(), 5000);
    QVERIFY(error.isEmpty());
    QCOMPARE(sentCommand.value(QStringLiteral("to_slot")).toString(), QStringLiteral("ios_phone"));
    QCOMPARE(sentCommand.value(QStringLiteral("payload")).toObject().value(QStringLiteral("text")).toString(),
             QStringLiteral("继续"));
    const QString firstCommandId = sentCommand.value(QStringLiteral("client_message_id")).toString();
    bool retryFinished = false;
    coordinator.sendRemoteCommand(
        catalog.first().toObject(),
        {{QStringLiteral("action"), QStringLiteral("prompt")}, {QStringLiteral("text"), QStringLiteral("继续")}},
        [&](const QJsonObject&, const QString& value) {
            error = value;
            retryFinished = true;
        });
    QCOMPARE(sentCommand.value(QStringLiteral("client_message_id")).toString(), firstCommandId);
    QTRY_VERIFY_WITH_TIMEOUT(retryFinished, 5000);
    QVERIFY(error.isEmpty());
}

void DesktopSessionSyncHostTest::remoteBusyComposerQueuesInputsAndStopsSeparately() {
    QTemporaryDir spool;
    QVERIFY(spool.isValid());
    RemoteAgentPanel panel(nullptr, spool.filePath(QStringLiteral("outbox.json")));
    const QJsonObject remote{{QStringLiteral("id"), QStringLiteral("ses_remote_busy_fixture")},
                             {QStringLiteral("device_slot"), QStringLiteral("ios_fixture")},
                             {QStringLiteral("login_session_id"), QStringLiteral("fixture_login")},
                             {QStringLiteral("online"), true},
                             {QStringLiteral("busy"), true},
                             {QStringLiteral("turnId"), QStringLiteral("msg_original_turn")}};
    QList<QJsonObject> sent;
    panel.setCommandHandler(
        [&](const QJsonObject& owner, const QJsonObject& action, RemoteAgentPanel::Completion completion) {
            QCOMPARE(owner.value(QStringLiteral("id")), remote.value(QStringLiteral("id")));
            sent.append(action);
            completion({});
        });
    panel.openSession(remote);
    auto* editor = panel.findChild<QTextEdit*>(QStringLiteral("remoteAgentComposer"));
    QVERIFY(editor && editor->isEnabled() && !editor->isReadOnly());
    editor->setPlainText(QStringLiteral("First fixture supplement."));
    QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(sent.size(), 1);
    QCOMPARE(sent[0].value(QStringLiteral("action")).toString(), QStringLiteral("prompt"));
    QVERIFY(!sent[0].contains(QStringLiteral("turnId")));
    const QString firstRequest = sent[0].value(QStringLiteral("requestId")).toString();
    QVERIFY(!firstRequest.isEmpty());
    editor->setPlainText(QStringLiteral("Second fixture supplement."));
    QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(editor->toPlainText(), QString());
    QCOMPARE(sent.size(), 2);
    panel.receivePayload(remote, QStringLiteral("event"),
                         {{QStringLiteral("type"), QStringLiteral("command_result")},
                          {QStringLiteral("command_id"), firstRequest},
                          {QStringLiteral("result"),
                           QJsonObject{{QStringLiteral("ok"), true},
                                       {QStringLiteral("user_message_id"), QStringLiteral("msg_accepted_fixture")}}}});
    QTRY_COMPARE(sent.size(), 2);
    QCOMPARE(sent[1].value(QStringLiteral("action")).toString(), QStringLiteral("prompt"));
    QVERIFY(!sent[1].contains(QStringLiteral("turnId")));
    QVERIFY(sent[1].value(QStringLiteral("requestId")).toString() != firstRequest);
    editor->setPlainText(QStringLiteral("Unsent text survives Stop."));
    auto* stop = panel.findChild<QPushButton*>(QStringLiteral("remoteAgentStopButton"));
    QVERIFY(stop);
    stop->click();
    QCOMPARE(sent.size(), 3);
    QCOMPARE(sent.last().value(QStringLiteral("action")).toString(), QStringLiteral("stop"));
    QCOMPARE(editor->toPlainText(), QStringLiteral("Unsent text survives Stop."));
    QJsonObject idle = remote;
    idle.insert(QStringLiteral("busy"), false);
    idle.insert(QStringLiteral("turnId"), QString());
    panel.updateSession(idle);
    QTest::qWait(40);
    QCOMPARE(sent.size(), 3);
}

void DesktopSessionSyncHostTest::remoteTransportCompletionPreservesNewDraft() {
    QTemporaryDir spool;
    QVERIFY(spool.isValid());
    RemoteAgentPanel panel(nullptr, spool.filePath(QStringLiteral("outbox.json")));
    const QJsonObject remote{{QStringLiteral("id"), QStringLiteral("ses_remote_draft_fixture")},
                             {QStringLiteral("device_slot"), QStringLiteral("ios_fixture")},
                             {QStringLiteral("login_session_id"), QStringLiteral("fixture_login")},
                             {QStringLiteral("online"), true},
                             {QStringLiteral("busy"), false}};
    RemoteAgentPanel::Completion delivered;
    panel.setCommandHandler([&](const QJsonObject&, const QJsonObject&, RemoteAgentPanel::Completion completion) {
        delivered = std::move(completion);
    });
    panel.openSession(remote);
    auto* editor = panel.findChild<QTextEdit*>(QStringLiteral("remoteAgentComposer"));
    QVERIFY(editor);
    editor->setPlainText(QStringLiteral("First fixture message."));
    QTest::keyClick(editor, Qt::Key_Return);
    QVERIFY(bool(delivered));
    editor->setPlainText(QStringLiteral("New draft while transport waits."));
    delivered({});
    QCOMPARE(editor->toPlainText(), QStringLiteral("New draft while transport waits."));
}

void DesktopSessionSyncHostTest::remoteQueueMatchesOwnerReceiptAndRetainsPrivateRecovery() {
    QTemporaryDir spool;
    const QString path = spool.filePath(QStringLiteral("outbox.json"));
    QJsonObject owner{{QStringLiteral("id"), QStringLiteral("ses_owner_a")},
                      {QStringLiteral("device_slot"), QStringLiteral("fixture_a")},
                      {QStringLiteral("login_session_id"), QStringLiteral("login_a")},
                      {QStringLiteral("online"), true},
                      {QStringLiteral("busy"), true}};
    QJsonObject other = owner;
    other.insert(QStringLiteral("id"), QStringLiteral("ses_owner_b"));
    QList<QJsonObject> sent;
    QString second;
    {
        RemoteAgentPanel panel(nullptr, path);
        panel.setCommandHandler([&](const QJsonObject&, const QJsonObject& action, RemoteAgentPanel::Completion done) {
            sent.append(action);
            done({});
        });
        panel.openSession(owner);
        auto* editor = panel.findChild<QTextEdit*>(QStringLiteral("remoteAgentComposer"));
        for (const QString& text : {QStringLiteral("Retained fixture one"), QStringLiteral("Retained fixture two")}) {
            editor->setPlainText(text);
            QTest::keyClick(editor, Qt::Key_Return);
        }
        QCOMPARE(sent.size(), 2);
        const QString first = sent[0].value(QStringLiteral("requestId")).toString();
        second = sent[1].value(QStringLiteral("requestId")).toString();
        QVERIFY(first != second);
        panel.openSession(other);
        editor->setPlainText(QStringLiteral("Other owner's draft"));
        const QJsonObject receipt{
            {QStringLiteral("type"), QStringLiteral("command_result")},
            {QStringLiteral("request_id"), first},
            {QStringLiteral("result"), QJsonObject{{QStringLiteral("ok"), true},
                                                   {QStringLiteral("user_message_id"), QStringLiteral("msg_first")}}}};
        panel.receivePayload(other, QStringLiteral("event"), receipt);  // Wrong owner cannot consume this input.
        panel.receivePayload(owner, QStringLiteral("event"), receipt);
        QCOMPARE(editor->toPlainText(), QStringLiteral("Other owner's draft"));
    }
    RemoteAgentPanel restored(nullptr, path);
    restored.setCommandHandler([&](const QJsonObject&, const QJsonObject& action, RemoteAgentPanel::Completion done) {
        sent.append(action);
        done({});
    });
    restored.openSession(owner);
    QCOMPARE(sent.size(), 2);  // A restart preserves an unknown input without automatically replaying it.
    auto* editor = restored.findChild<QTextEdit*>(QStringLiteral("remoteAgentComposer"));
    editor->setPlainText(QStringLiteral("Fresh independent input"));
    QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(sent.size(), 3);
    auto* transcript = restored.findChild<MarkdownView*>();
    QVERIFY(transcript);
    editor->setPlainText(QStringLiteral("New draft must survive the retry"));
    transcript->linkActivated(QStringLiteral("maichat-input-retry:") + second);
    QCOMPARE(sent.size(), 4);
    QCOMPARE(sent.last().value(QStringLiteral("requestId")).toString(), second);
    QCOMPARE(sent.last().value(QStringLiteral("text")).toString(), QStringLiteral("Retained fixture two"));
    QCOMPARE(editor->toPlainText(), QStringLiteral("New draft must survive the retry"));
}

void DesktopSessionSyncHostTest::remoteIdleConsecutiveInputsDoNotWaitForTurnAcknowledgement() {
    QTemporaryDir spool;
    QVERIFY(spool.isValid());
    RemoteAgentPanel panel(nullptr, spool.filePath(QStringLiteral("outbox.json")));
    const QJsonObject owner{{QStringLiteral("id"), QStringLiteral("ses_idle_queue")},
                            {QStringLiteral("device_slot"), QStringLiteral("fixture")},
                            {QStringLiteral("login_session_id"), QStringLiteral("login")},
                            {QStringLiteral("online"), true},
                            {QStringLiteral("busy"), false}};
    QList<QJsonObject> sent;
    panel.setCommandHandler(
        [&](const QJsonObject&, const QJsonObject& action, RemoteAgentPanel::Completion completion) {
            sent.append(action);
            completion({});
        });
    panel.openSession(owner);
    auto* editor = panel.findChild<QTextEdit*>(QStringLiteral("remoteAgentComposer"));
    editor->setPlainText(QStringLiteral("Start original fixture"));
    QTest::keyClick(editor, Qt::Key_Return);
    editor->setPlainText(QStringLiteral("Supplement before presence arrives"));
    QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(sent.size(), 2);
    panel.receivePayload(owner, QStringLiteral("event"),
                         {{QStringLiteral("type"), QStringLiteral("command_result")},
                          {QStringLiteral("command_id"), sent.first().value(QStringLiteral("requestId"))},
                          {QStringLiteral("result"),
                           QJsonObject{{QStringLiteral("ok"), true},
                                       {QStringLiteral("user_message_id"), QStringLiteral("msg_initial_fixture")},
                                       {QStringLiteral("turnId"), QStringLiteral("msg_owner_ack_turn")}}}});
    QTRY_COMPARE(sent.size(), 2);
    QCOMPARE(sent.last().value(QStringLiteral("action")).toString(), QStringLiteral("prompt"));
    QVERIFY(!sent.last().contains(QStringLiteral("turnId")));
    QVERIFY(sent[0].value(QStringLiteral("requestId")) != sent[1].value(QStringLiteral("requestId")));
    QVERIFY(!sent.last().contains(QStringLiteral("afterRequest")));
}

void DesktopSessionSyncHostTest::remoteQueueStorageFailureRetainsDraft() {
    QTemporaryDir spool;
    QVERIFY(spool.isValid());
    // A directory is deliberately not a writable spool file.
    RemoteAgentPanel panel(nullptr, spool.path());
    const QJsonObject owner{{QStringLiteral("id"), QStringLiteral("ses_storage_fixture")},
                            {QStringLiteral("device_slot"), QStringLiteral("fixture")},
                            {QStringLiteral("login_session_id"), QStringLiteral("login")},
                            {QStringLiteral("online"), true}};
    int sent = 0;
    panel.setCommandHandler([&](const QJsonObject&, const QJsonObject&, RemoteAgentPanel::Completion) { ++sent; });
    panel.openSession(owner);
    auto* editor = panel.findChild<QTextEdit*>(QStringLiteral("remoteAgentComposer"));
    editor->setPlainText(QStringLiteral("No data loss on storage failure"));
    QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(editor->toPlainText(), QStringLiteral("No data loss on storage failure"));
    QCOMPARE(sent, 0);
}

void DesktopSessionSyncHostTest::remoteLateInputReceiptDoesNotReplaceNewerOwnerTurn() {
    QTemporaryDir spool;
    RemoteAgentPanel panel(nullptr, spool.filePath(QStringLiteral("outbox.json")));
    QJsonObject owner{{QStringLiteral("id"), QStringLiteral("ses_late_receipt_fixture")},
                      {QStringLiteral("device_slot"), QStringLiteral("fixture")},
                      {QStringLiteral("login_session_id"), QStringLiteral("login")},
                      {QStringLiteral("online"), true},
                      {QStringLiteral("busy"), true},
                      {QStringLiteral("turnId"), QStringLiteral("msg_original_turn")}};
    QList<QJsonObject> sent;
    panel.setCommandHandler([&](const QJsonObject&, const QJsonObject& action, RemoteAgentPanel::Completion done) {
        sent.append(action);
        done({});
    });
    panel.openSession(owner);
    auto* editor = panel.findChild<QTextEdit*>(QStringLiteral("remoteAgentComposer"));
    editor->setPlainText(QStringLiteral("Input while original turn runs"));
    QTest::keyClick(editor, Qt::Key_Return);
    owner.insert(QStringLiteral("turnId"), QStringLiteral("msg_newer_turn"));
    panel.updateSession(owner);
    panel.receivePayload(
        owner, QStringLiteral("event"),
        {{QStringLiteral("type"), QStringLiteral("command_result")},
         {QStringLiteral("request_id"), sent.first().value(QStringLiteral("requestId"))},
         {QStringLiteral("result"), QJsonObject{{QStringLiteral("ok"), true},
                                                {QStringLiteral("user_message_id"), QStringLiteral("msg_old_user")},
                                                {QStringLiteral("turnId"), QStringLiteral("msg_original_turn")}}}});
    QCOMPARE(panel.session().value(QStringLiteral("turnId")).toString(), QStringLiteral("msg_newer_turn"));
    editor->setPlainText(QStringLiteral("Input while replacement runs"));
    QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(sent.size(), 2);
    QVERIFY(!sent.last().contains(QStringLiteral("turnId")));
    QCOMPARE(sent.last().value(QStringLiteral("action")).toString(), QStringLiteral("prompt"));
    auto* stop = panel.findChild<QPushButton*>(QStringLiteral("remoteAgentStopButton"));
    QVERIFY(stop && stop->isEnabled());
    QTest::mouseClick(stop, Qt::LeftButton);
    QCOMPARE(sent.last().value(QStringLiteral("action")).toString(), QStringLiteral("stop"));
    QCOMPARE(sent.last().value(QStringLiteral("turnId")).toString(), QStringLiteral("msg_newer_turn"));
}

void DesktopSessionSyncHostTest::remotePanelShowsHistoryAndRoutesTextToOwner() {
    QTemporaryDir spool;
    QVERIFY(spool.isValid());
    RemoteAgentPanel panel(nullptr, spool.filePath(QStringLiteral("outbox.json")));
    panel.resize(900, 680);
    const QJsonObject remote{{QStringLiteral("id"), QStringLiteral("ses_remote")},
                             {QStringLiteral("device_slot"), QStringLiteral("ios_phone")},
                             {QStringLiteral("device_name"), QStringLiteral("测试 iPhone")},
                             {QStringLiteral("login_session_id"), QStringLiteral("login_phone")},
                             {QStringLiteral("online"), true},
                             {QStringLiteral("busy"), false}};
    QJsonObject sent;
    panel.setHistoryHandler(
        [&](const QJsonObject& session, const QString&, int, RemoteAgentPanel::Completion completion) {
            QCOMPARE(session.value(QStringLiteral("id")).toString(), QStringLiteral("ses_remote"));
            const QJsonObject part{{QStringLiteral("kind"), QStringLiteral("text")},
                                   {QStringLiteral("text"), QStringLiteral("所属手机的历史")}};
            const QJsonObject message{{QStringLiteral("id"), QStringLiteral("msg_1")},
                                      {QStringLiteral("role"), QStringLiteral("assistant")},
                                      {QStringLiteral("parts"), QJsonArray{part}}};
            panel.receivePayload(remote, QStringLiteral("history_page"),
                                 {{QStringLiteral("ok"), true}, {QStringLiteral("messages"), QJsonArray{message}}});
            completion({});
        });
    panel.setCommandHandler(
        [&](const QJsonObject& session, const QJsonObject& action, RemoteAgentPanel::Completion completion) {
            QCOMPARE(session.value(QStringLiteral("id")).toString(), QStringLiteral("ses_remote"));
            sent = action;
            completion({});
        });
    panel.show();
    panel.openSession(remote);
    auto* transcript = panel.findChild<MarkdownView*>();
    QVERIFY(transcript != nullptr);
    QVERIFY(transcript->contains(QStringLiteral("msg_1")));
    auto* composer = panel.findChild<QTextEdit*>(QStringLiteral("remoteAgentComposer"));
    QVERIFY(composer != nullptr);
    QVERIFY(dynamic_cast<ComposerTextEdit*>(composer) != nullptr);
    composer->setPlainText(QStringLiteral("继续本机任务"));
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("remoteAgentSendButton"));
    QVERIFY(send != nullptr && send->isEnabled());
    QCOMPARE(send->parentWidget(), composer);
    QVERIFY(!send->icon().isNull());
    auto* attachment = panel.findChild<QPushButton*>(QStringLiteral("remoteAgentAttachButton"));
    QVERIFY(attachment && attachment->isEnabled());
    QTest::mouseClick(send, Qt::LeftButton);
    QCOMPARE(sent.value(QStringLiteral("action")).toString(), QStringLiteral("prompt"));
    QCOMPARE(sent.value(QStringLiteral("text")).toString(), QStringLiteral("继续本机任务"));
    QCOMPARE(composer->toPlainText(), QString());
    panel.receivePayload(remote, QStringLiteral("event"),
                         {{QStringLiteral("type"), QStringLiteral("command_result")},
                          {QStringLiteral("request_id"), sent.value(QStringLiteral("requestId"))},
                          {QStringLiteral("result"),
                           QJsonObject{{QStringLiteral("ok"), true},
                                       {QStringLiteral("user_message_id"), QStringLiteral("msg_phone_input")}}}});
    QVERIFY(transcript->contains(QStringLiteral("remote-thinking")));
    auto* status = panel.findChild<QLabel*>(QStringLiteral("remoteAgentStatus"));
    QVERIFY(status && !status->isVisible());
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QJsonObject active{{QStringLiteral("id"), QStringLiteral("msg_new")},
                             {QStringLiteral("role"), QStringLiteral("assistant")},
                             {QStringLiteral("created"), double(now)},
                             {QStringLiteral("active"), true},
                             {QStringLiteral("parts"), QJsonArray{}}};
    panel.receivePayload(remote, QStringLiteral("history_page"),
                         {{QStringLiteral("ok"), true}, {QStringLiteral("messages"), QJsonArray{active}}});
    QVERIFY(transcript->contains(QStringLiteral("remote-thinking")));
    QJsonObject completed = active;
    completed.insert(QStringLiteral("active"), false);
    completed.insert(QStringLiteral("parts"),
                     QJsonArray{QJsonObject{{QStringLiteral("kind"), QStringLiteral("text")},
                                            {QStringLiteral("text"), QStringLiteral("已完成")}}});
    panel.receivePayload(remote, QStringLiteral("history_page"),
                         {{QStringLiteral("ok"), true}, {QStringLiteral("messages"), QJsonArray{completed}}});
    QVERIFY(!transcript->contains(QStringLiteral("remote-thinking")));
    composer->setPlainText(QStringLiteral("请保留草稿"));
    QTest::mouseClick(send, Qt::LeftButton);
    QCOMPARE(composer->toPlainText(), QString());
    panel.receivePayload(
        remote, QStringLiteral("event"),
        {{QStringLiteral("type"), QStringLiteral("command_result")},
         {QStringLiteral("command_id"), sent.value(QStringLiteral("requestId"))},
         {QStringLiteral("result"),
          QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("所属设备拒绝")}}}});
    QCOMPARE(composer->toPlainText(), QStringLiteral("请保留草稿"));
    panel.setCommandHandler(
        [&](const QJsonObject&, const QJsonObject& action, RemoteAgentPanel::Completion completion) {
            sent = action;
            panel.receivePayload(
                remote, QStringLiteral("event"),
                {{QStringLiteral("type"), QStringLiteral("command_result")},
                 {QStringLiteral("command_id"), sent.value(QStringLiteral("requestId"))},
                 {QStringLiteral("result"),
                  QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("立即拒绝")}}}});
            completion({});
        });
    composer->setPlainText(QStringLiteral("快速拒绝也保留"));
    QTest::mouseClick(send, Qt::LeftButton);
    QCOMPARE(composer->toPlainText(), QStringLiteral("快速拒绝也保留"));
    panel.setCommandHandler(
        [&](const QJsonObject&, const QJsonObject& action, RemoteAgentPanel::Completion completion) {
            sent = action;
            panel.receivePayload(remote, QStringLiteral("event"),
                                 {{QStringLiteral("type"), QStringLiteral("command_result")},
                                  {QStringLiteral("command_id"), sent.value(QStringLiteral("requestId"))},
                                  {QStringLiteral("result"), QJsonObject{{QStringLiteral("ok"), true},
                                                                         {QStringLiteral("user_message_id"),
                                                                          QStringLiteral("msg_accepted_fixture")}}}});
            completion({});
        });
    composer->setPlainText(QStringLiteral("快速确认应清空"));
    QTest::mouseClick(send, Qt::LeftButton);
    QCOMPARE(composer->toPlainText(), QString());
    if (qEnvironmentVariableIsSet("MAICHAT_REMOTE_PANEL_SCREENSHOT"))
        QVERIFY(panel.grab().save(qEnvironmentVariable("MAICHAT_REMOTE_PANEL_SCREENSHOT")));
}

void DesktopSessionSyncHostTest::remotePanelRoutesAttachmentLinkToVerifiedDownload() {
    QTemporaryDir spool;
    QVERIFY(spool.isValid());
    RemoteAgentPanel panel(nullptr, spool.filePath(QStringLiteral("outbox.json")));
    const QString id = QStringLiteral("123e4567-e89b-42d3-a456-426614174000");
    const QJsonObject remote{{QStringLiteral("id"), QStringLiteral("ses_remote")},
                             {QStringLiteral("device_slot"), QStringLiteral("ios_phone")},
                             {QStringLiteral("login_session_id"), QStringLiteral("login_phone")},
                             {QStringLiteral("online"), true}};
    panel.setHistoryHandler([&](const QJsonObject&, const QString&, int, RemoteAgentPanel::Completion completion) {
        panel.receivePayload(
            remote, QStringLiteral("history_page"),
            {{QStringLiteral("ok"), true},
             {QStringLiteral("messages"),
              QJsonArray{
                  QJsonObject{{QStringLiteral("id"), QStringLiteral("msg_attached")},
                              {QStringLiteral("role"), QStringLiteral("user")},
                              {QStringLiteral("parts"),
                               QJsonArray{QJsonObject{{QStringLiteral("kind"), QStringLiteral("file")},
                                                      {QStringLiteral("attachment_id"), id},
                                                      {QStringLiteral("name"), QStringLiteral("notes.pdf")}}}}}}}});
        completion({});
    });
    QString requested;
    panel.setDownloadHandler(
        [&](const QJsonObject&, const QString& attachment, RemoteAgentPanel::DownloadCompletion completion) {
            requested = attachment;
            completion({}, QStringLiteral("隔离测试无下载文件"));
        });
    panel.show();
    panel.openSession(remote);
    auto* view = panel.findChild<MarkdownView*>();
    QVERIFY(view && view->contains(QStringLiteral("msg_attached")));
    QVERIFY(QMetaObject::invokeMethod(view, "linkActivated", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("maichat-attachment:") + id)));
    QCOMPARE(requested, id);
    auto* status = panel.findChild<QLabel*>(QStringLiteral("remoteAgentStatus"));
    QVERIFY(status && status->text().contains(QStringLiteral("隔离测试")));
}

void DesktopSessionSyncHostTest::remotePanelDoesNotRepeatPendingHistory() {
    QTemporaryDir spool;
    QVERIFY(spool.isValid());
    RemoteAgentPanel panel(nullptr, spool.filePath(QStringLiteral("outbox.json")));
    const QJsonObject remote{{QStringLiteral("id"), QStringLiteral("ses_remote")},
                             {QStringLiteral("device_slot"), QStringLiteral("ios_phone")},
                             {QStringLiteral("login_session_id"), QStringLiteral("phone_login")},
                             {QStringLiteral("online"), true}};
    int requests = 0;
    panel.setHistoryHandler(
        [&](const QJsonObject&, const QString&, int limit, RemoteAgentPanel::Completion completion) {
            ++requests;
            QCOMPARE(limit, 10);
            completion({});
        });
    panel.show();
    panel.openSession(remote);
    QCOMPARE(requests, 1);
    QTest::qWait(5500);
    QCOMPARE(requests, 1);
}

void DesktopSessionSyncHostTest::remotePanelReconcilesAfterReconnect() {
    QTemporaryDir spool;
    QVERIFY(spool.isValid());
    RemoteAgentPanel panel(nullptr, spool.filePath(QStringLiteral("outbox.json")));
    const QJsonObject remote{{QStringLiteral("id"), QStringLiteral("ses_remote")},
                             {QStringLiteral("device_slot"), QStringLiteral("ios_phone")},
                             {QStringLiteral("login_session_id"), QStringLiteral("phone_login")},
                             {QStringLiteral("online"), true}};
    QList<int> limits;
    panel.setHistoryHandler([&](const QJsonObject&, const QString&, int limit,
                                RemoteAgentPanel::Completion completion) {
        limits.append(limit);
        QJsonArray messages;
        if (limits.size() > 1) {
            messages.append(QJsonObject{
                {QStringLiteral("id"), QStringLiteral("msg_assistant")},
                {QStringLiteral("role"), QStringLiteral("assistant")},
                {QStringLiteral("parts"), QJsonArray{QJsonObject{{QStringLiteral("kind"), QStringLiteral("text")},
                                                                 {QStringLiteral("text"), QStringLiteral("收到")}}}}});
            messages.append(QJsonObject{
                {QStringLiteral("id"), QStringLiteral("msg_user")},
                {QStringLiteral("role"), QStringLiteral("user")},
                {QStringLiteral("parts"), QJsonArray{QJsonObject{{QStringLiteral("kind"), QStringLiteral("text")},
                                                                 {QStringLiteral("text"), QStringLiteral("你好")}}}}});
        }
        panel.receivePayload(remote, QStringLiteral("history_page"),
                             {{QStringLiteral("ok"), true}, {QStringLiteral("messages"), messages}});
        completion({});
    });
    panel.show();
    panel.openSession(remote);
    QCOMPARE(limits, QList<int>{10});
    QJsonObject offline = remote;
    offline.insert(QStringLiteral("online"), false);
    panel.updateSession(offline);
    panel.updateSession(remote);
    QCOMPARE(limits, (QList<int>{10, 10}));
    auto* transcript = panel.findChild<MarkdownView*>();
    QVERIFY(transcript != nullptr);
    QVERIFY(transcript->contains(QStringLiteral("msg_user")));
    if (qEnvironmentVariableIsSet("MAICHAT_REMOTE_PAIR_CAPTURE"))
        QVERIFY(panel.grab().save(qEnvironmentVariable("MAICHAT_REMOTE_PAIR_CAPTURE")));
}

void DesktopSessionSyncHostTest::remotePanelKeepsOwnPromptWhenRoomBroadcastsCanonicalMessage() {
    QTemporaryDir spool;
    QVERIFY(spool.isValid());
    RemoteAgentPanel panel(nullptr, spool.filePath(QStringLiteral("outbox.json")));
    const QJsonObject remote{{QStringLiteral("id"), QStringLiteral("ses_remote")},
                             {QStringLiteral("device_slot"), QStringLiteral("ios_phone")},
                             {QStringLiteral("login_session_id"), QStringLiteral("phone_login")},
                             {QStringLiteral("online"), true}};
    QList<int> limits;
    QString requestId;
    panel.setHistoryHandler(
        [&](const QJsonObject&, const QString&, int limit, RemoteAgentPanel::Completion completion) {
            limits.append(limit);
            panel.receivePayload(remote, QStringLiteral("history_page"),
                                 {{QStringLiteral("ok"), true}, {QStringLiteral("messages"), QJsonArray{}}});
            completion({});
        });
    panel.setCommandHandler(
        [&](const QJsonObject&, const QJsonObject& payload, RemoteAgentPanel::Completion completion) {
            requestId = payload.value(QStringLiteral("requestId")).toString();
            completion({});
        });
    panel.show();
    panel.openSession(remote);
    QCOMPARE(limits, QList<int>{10});
    auto* composer = panel.findChild<QTextEdit*>(QStringLiteral("remoteAgentComposer"));
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("remoteAgentSendButton"));
    QVERIFY(composer && send);
    auto* transcript = panel.findChild<MarkdownView*>();
    QVERIFY(transcript);
    composer->setPlainText(QStringLiteral("你好"));
    QTest::mouseClick(send, Qt::LeftButton);
    QCOMPARE(transcript->itemCount(), 1);
    QCOMPARE(composer->toPlainText(), QString());
    panel.receivePayload(
        remote, QStringLiteral("event"),
        {{QStringLiteral("type"), QStringLiteral("command_result")},
         {QStringLiteral("request_id"), requestId},
         {QStringLiteral("result"),
          QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("user_message_id"), QStringLiteral("msg_user")}}}});
    QVERIFY(transcript->contains(QStringLiteral("msg_user")));
    QCOMPARE(transcript->itemCount(), 2);  // The second item is the pending assistant indicator.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QJsonObject user{
        {QStringLiteral("id"), QStringLiteral("msg_user")},
        {QStringLiteral("role"), QStringLiteral("user")},
        {QStringLiteral("created"), double(now)},
        {QStringLiteral("parts"), QJsonArray{QJsonObject{{QStringLiteral("kind"), QStringLiteral("text")},
                                                         {QStringLiteral("text"), QStringLiteral("你好")}}}}};
    const QJsonObject assistant{
        {QStringLiteral("id"), QStringLiteral("msg_assistant")},
        {QStringLiteral("role"), QStringLiteral("assistant")},
        {QStringLiteral("created"), double(now + 1)},
        {QStringLiteral("parts"), QJsonArray{QJsonObject{{QStringLiteral("kind"), QStringLiteral("text")},
                                                         {QStringLiteral("text"), QStringLiteral("收到")}}}}};
    panel.receivePayload(remote, QStringLiteral("event"),
                         {{QStringLiteral("ok"), true}, {QStringLiteral("messages"), QJsonArray{assistant, user}}});
    QCOMPARE(limits, QList<int>{10});
    QCOMPARE(transcript->itemCount(), 2);
    QVERIFY(transcript->contains(QStringLiteral("msg_user")));
    QVERIFY(transcript->contains(QStringLiteral("msg_assistant")));
}

void DesktopSessionSyncHostTest::remotePanelPushCanArriveBeforeInitialPage() {
    QTemporaryDir spool;
    QVERIFY(spool.isValid());
    RemoteAgentPanel panel(nullptr, spool.filePath(QStringLiteral("outbox.json")));
    const QJsonObject remote{{QStringLiteral("id"), QStringLiteral("ses_remote")},
                             {QStringLiteral("device_slot"), QStringLiteral("ios_phone")},
                             {QStringLiteral("login_session_id"), QStringLiteral("phone_login")},
                             {QStringLiteral("online"), true}};
    QList<int> limits;
    panel.setHistoryHandler(
        [&](const QJsonObject&, const QString&, int limit, RemoteAgentPanel::Completion completion) {
            Q_UNUSED(completion);
            limits.append(limit);
        });
    panel.show();
    panel.openSession(remote);
    QCOMPARE(limits, QList<int>{10});
    const QJsonObject user{
        {QStringLiteral("id"), QStringLiteral("msg_user")},
        {QStringLiteral("role"), QStringLiteral("user")},
        {QStringLiteral("parts"), QJsonArray{QJsonObject{{QStringLiteral("kind"), QStringLiteral("text")},
                                                         {QStringLiteral("text"), QStringLiteral("你好")}}}}};
    panel.receivePayload(remote, QStringLiteral("event"),
                         {{QStringLiteral("ok"), true}, {QStringLiteral("messages"), QJsonArray{user}}});
    auto* transcript = panel.findChild<MarkdownView*>();
    QVERIFY(transcript && transcript->contains(QStringLiteral("msg_user")));
    panel.receivePayload(remote, QStringLiteral("event"),
                         {{QStringLiteral("ok"), true}, {QStringLiteral("messages"), QJsonArray{user}}});
    QCOMPARE(transcript->itemCount(), 1);
    const QJsonObject older{
        {QStringLiteral("id"), QStringLiteral("msg_older")},
        {QStringLiteral("role"), QStringLiteral("assistant")},
        {QStringLiteral("parts"), QJsonArray{QJsonObject{{QStringLiteral("kind"), QStringLiteral("text")},
                                                         {QStringLiteral("text"), QStringLiteral("旧消息")}}}}};
    panel.receivePayload(remote, QStringLiteral("history_page"),
                         {{QStringLiteral("ok"), true}, {QStringLiteral("messages"), QJsonArray{older}}});
    QCOMPARE(limits, QList<int>{10});
    QCOMPARE(transcript->itemCount(), 2);
    QVERIFY(transcript->contains(QStringLiteral("msg_older")));
    QVERIFY(transcript->contains(QStringLiteral("msg_user")));
}

void DesktopSessionSyncHostTest::sessionListKeepsLocalAndRemoteSelectionsSeparate() {
    AgentController::ModelConfig model;
    AgentController controller(model, {});
    const QString local = controller.createSession({}, QStringLiteral("本地会话"));
    QVERIFY(!local.isEmpty());
    AgentSessionList sessions(controller);
    sessions.resize(300, 500);
    sessions.setRemoteSessions(
        QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("ses_remote")},
                               {QStringLiteral("title"), QStringLiteral("远端会话")},
                               {QStringLiteral("device_slot"), QStringLiteral("ios_phone")},
                               {QStringLiteral("device_name"), QStringLiteral("iPhone 14 Pro Max")},
                               {QStringLiteral("device_id"), QStringLiteral("phone-install")},
                               {QStringLiteral("login_session_id"), QStringLiteral("phone-login")},
                               {QStringLiteral("online"), true}}});
    sessions.show();
    auto* list = sessions.findChild<QListWidget*>();
    QVERIFY(list != nullptr);
    QCOMPARE(list->count(), 4);
    QVERIFY(list->item(0)->text().contains(QStringLiteral("这台")));
    QVERIFY(list->item(2)->text().contains(QStringLiteral("iPhone 14 Pro Max")));
    QSignalSpy localSelection(&sessions, &AgentSessionList::selected);
    QSignalSpy remoteSelection(&sessions, &AgentSessionList::remoteSelected);
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualItemRect(list->item(2)).center());
    QCOMPARE(remoteSelection.size(), 0);
    QCOMPARE(list->count(), 3);
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualItemRect(list->item(2)).center());
    QCOMPARE(list->count(), 4);
    if (qEnvironmentVariableIsSet("MAICHAT_SESSION_GROUP_SCREENSHOT"))
        QVERIFY(sessions.grab().save(qEnvironmentVariable("MAICHAT_SESSION_GROUP_SCREENSHOT")));
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualItemRect(list->item(3)).center());
    QCOMPARE(remoteSelection.size(), 1);
    QCOMPARE(localSelection.size(), 0);
    QCOMPARE(remoteSelection.takeFirst().first().toJsonObject().value(QStringLiteral("id")).toString(),
             QStringLiteral("ses_remote"));
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualItemRect(list->item(1)).center());
    QCOMPARE(localSelection.size(), 1);
    QCOMPARE(localSelection.takeFirst().first().toString(), local);
}

void DesktopSessionSyncHostTest::sessionListSeparatesDevicesWithTheSameModel() {
    AgentController controller(AgentController::ModelConfig{}, {});
    AgentSessionList sessions(controller);
    QJsonArray remote;
    for (const QString& id : {QStringLiteral("phone_one"), QStringLiteral("phone_two")}) {
        remote.append(QJsonObject{{QStringLiteral("id"), id},
                                  {QStringLiteral("title"), QStringLiteral("同型号会话")},
                                  {QStringLiteral("device_slot"), QStringLiteral("ios_phone")},
                                  {QStringLiteral("device_name"), QStringLiteral("iPhone 14 Pro Max")},
                                  {QStringLiteral("device_id"), id},
                                  {QStringLiteral("login_session_id"), QStringLiteral("login_phone")},
                                  {QStringLiteral("online"), true}});
    }
    sessions.setRemoteSessions(remote);
    sessions.show();
    auto* list = sessions.findChild<QListWidget*>();
    QVERIFY(list);
    QCOMPARE(list->count(), 5);
    QVERIFY(list->item(0)->text().contains(QStringLiteral("这台")));
    QVERIFY(list->item(1)->text().contains(QStringLiteral("设备 1")));
    QVERIFY(list->item(3)->text().contains(QStringLiteral("设备 2")));
}

void DesktopSessionSyncHostTest::remoteQuestionUsesNormalComposerAndExplicitAnswerReceipt() {
    QTemporaryDir spool;
    QVERIFY(spool.isValid());
    const QString file = spool.filePath(QStringLiteral("outbox.json"));
    RemoteAgentPanel panel(nullptr, file);
    panel.resize(900, 650);
    const QJsonObject owner{{QStringLiteral("id"), QStringLiteral("ses_question_owner")},
                            {QStringLiteral("device_slot"), QStringLiteral("ios_question")},
                            {QStringLiteral("login_session_id"), QStringLiteral("question_login")},
                            {QStringLiteral("busy"), true},
                            {QStringLiteral("online"), true}};
    const QJsonObject question{
        {QStringLiteral("id"), QStringLiteral("qst_remote_fixture")},
        {QStringLiteral("sessionId"), owner.value(QStringLiteral("id"))},
        {QStringLiteral("messageId"), QStringLiteral("msg_question_fixture")},
        {QStringLiteral("partId"), QStringLiteral("part_question_fixture")},
        {QStringLiteral("asked"), 1000},
        {QStringLiteral("question"), QStringLiteral("Which fixture should continue?")},
        {QStringLiteral("options"), QJsonArray{QStringLiteral("Option A"), QStringLiteral("Option B")}}};
    QList<QJsonObject> commands;
    panel.setCommandHandler(
        [&](const QJsonObject&, const QJsonObject& action, RemoteAgentPanel::Completion completion) {
            commands.append(action);
            completion({});
        });
    panel.openSession(owner);
    panel.show();
    panel.receivePayload(owner, QStringLiteral("history_page"),
                         {{QStringLiteral("ok"), true},
                          {QStringLiteral("messages"), QJsonArray{}},
                          {QStringLiteral("questions"), QJsonArray{question}}});
    QVERIFY(panel.findChildren<QLineEdit*>().isEmpty());
    auto* editor = panel.findChild<QTextEdit*>(QStringLiteral("remoteAgentComposer"));
    auto* view = panel.findChild<MarkdownView*>();
    QVERIFY(editor && view);
    view->selectAll();
    QVERIFY(view->selectedText().contains(QStringLiteral("Which fixture should continue?")));
    QVERIFY(view->selectedText().contains(QStringLiteral("Option A")));
    editor->setPlainText(QStringLiteral("Use Option A"));
    QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(commands.size(), 1);  // A question does not require a parent turn ID.
    QCOMPARE(commands.first().value(QStringLiteral("action")).toString(), QStringLiteral("answer"));
    QCOMPARE(commands.first().value(QStringLiteral("id")).toString(), question.value(QStringLiteral("id")).toString());
    QCOMPARE(editor->toPlainText(), QString());
    const QString request = commands.first().value(QStringLiteral("requestId")).toString();
    QVERIFY(!request.isEmpty());
    panel.receivePayload(owner, QStringLiteral("event"),
                         {{QStringLiteral("type"), QStringLiteral("command_result")},
                          {QStringLiteral("command_id"), request},
                          {QStringLiteral("result"),
                           QJsonObject{{QStringLiteral("ok"), true},
                                       {QStringLiteral("question_id"), question.value(QStringLiteral("id"))}}}});
    QFile saved(file);
    QVERIFY(saved.open(QIODevice::ReadOnly));
    QVERIFY(QJsonDocument::fromJson(saved.readAll()).object().value(QStringLiteral("outbox")).toArray().isEmpty());
    saved.close();  // Windows QSaveFile must be able to atomically replace this private outbox on the next send.
    view->selectAll();
    QCOMPARE(view->selectedText().count(QStringLiteral("Use Option A")), 1);  // ACK precedes canonical output.

    const QJsonObject input{{QStringLiteral("question"), question.value(QStringLiteral("question"))},
                            {QStringLiteral("options"), question.value(QStringLiteral("options"))}};
    const QJsonObject part{
        {QStringLiteral("id"), question.value(QStringLiteral("partId"))},
        {QStringLiteral("kind"), QStringLiteral("tool")},
        {QStringLiteral("tool"), QStringLiteral("question")},
        {QStringLiteral("input"), QString::fromUtf8(QJsonDocument(input).toJson(QJsonDocument::Compact))},
        {QStringLiteral("state"), QStringLiteral("completed")},
        {QStringLiteral("output"), QStringLiteral("Use Option A")}};
    panel.receivePayload(owner, QStringLiteral("event"),
                         {{QStringLiteral("ok"), true},
                          {QStringLiteral("questions"), QJsonArray{}},
                          {QStringLiteral("messages"),
                           QJsonArray{QJsonObject{{QStringLiteral("id"), question.value(QStringLiteral("messageId"))},
                                                  {QStringLiteral("role"), QStringLiteral("assistant")},
                                                  {QStringLiteral("created"), 1000},
                                                  {QStringLiteral("parts"), QJsonArray{part}}}}}});
    view->selectAll();
    QCOMPARE(view->selectedText().count(QStringLiteral("Use Option A")), 1);
    QVERIFY(view->contains(QStringLiteral("remote-answer:part_question_fixture")));
    QJsonObject running = owner;
    running.insert(QStringLiteral("turnId"), QStringLiteral("msg_parent_turn"));
    panel.updateSession(running);
    editor->setPlainText(QStringLiteral("Keep the next normal instruction"));
    QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(commands.size(), 2);
    QCOMPARE(commands.last().value(QStringLiteral("action")).toString(), QStringLiteral("prompt"));
    QVERIFY(!commands.last().contains(QStringLiteral("turnId")));
    QVERIFY(!commands.last().value(QStringLiteral("requestId")).toString().isEmpty());
    int visibleThinkingIndicators = 0;
    for (QLabel* label : panel.findChildren<QLabel*>(QStringLiteral("remoteThinkingIndicator")))
        if (label->isVisible()) ++visibleThinkingIndicators;
    QVERIFY2(visibleThinkingIndicators <= 1, "Retired indicators must be hidden before deferred widget destruction");
    view->clearSelection();
    QCoreApplication::processEvents();
    if (qEnvironmentVariableIsSet("MAICHAT_REMOTE_QUESTION_CAPTURE"))
        QVERIFY(panel.grab().save(qEnvironmentVariable("MAICHAT_REMOTE_QUESTION_CAPTURE")));
}

void DesktopSessionSyncHostTest::remoteQuestionProjectionKeepsRootCursorAndStableDisplayRows() {
    QTemporaryDir spool;
    RemoteAgentPanel panel(nullptr, spool.filePath(QStringLiteral("outbox.json")));
    panel.resize(900, 650);
    const QJsonObject owner{{QStringLiteral("id"), QStringLiteral("ses_root_projection")},
                            {QStringLiteral("device_slot"), QStringLiteral("mac_projection")},
                            {QStringLiteral("login_session_id"), QStringLiteral("projection_login")},
                            {QStringLiteral("online"), true}};
    QStringList cursors;
    panel.setHistoryHandler(
        [&](const QJsonObject&, const QString& before, int, RemoteAgentPanel::Completion completion) {
            cursors.append(before);
            completion({});
        });
    panel.openSession(owner);
    QJsonArray messages;
    for (int index = 9; index >= 0; --index)
        messages.append(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("msg_root_%1").arg(index, 2, 10, QLatin1Char('0'))},
            {QStringLiteral("role"), QStringLiteral("assistant")},
            {QStringLiteral("created"), 2000 + index},
            {QStringLiteral("parts"),
             QJsonArray{QJsonObject{{QStringLiteral("kind"), QStringLiteral("text")},
                                    {QStringLiteral("text"), QStringLiteral("Root fixture %1").arg(index)}}}}});
    const QString input = QString::fromUtf8(
        QJsonDocument(
            QJsonObject{{QStringLiteral("question"), QStringLiteral("Child fixture question")},
                        {QStringLiteral("options"), QJsonArray{QStringLiteral("Continue"), QStringLiteral("Wait")}}})
            .toJson(QJsonDocument::Compact));
    const QJsonObject projection{
        {QStringLiteral("id"), QStringLiteral("msg_aaa_child")},
        {QStringLiteral("sessionId"), QStringLiteral("ses_child")},
        {QStringLiteral("questionProjection"), true},
        {QStringLiteral("role"), QStringLiteral("assistant")},
        {QStringLiteral("created"), 2005},
        {QStringLiteral("parts"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("part_child_question")},
                                                         {QStringLiteral("kind"), QStringLiteral("tool")},
                                                         {QStringLiteral("tool"), QStringLiteral("question")},
                                                         {QStringLiteral("input"), input},
                                                         {QStringLiteral("state"), QStringLiteral("running")}}}}};
    const QJsonObject page{{QStringLiteral("ok"), true},
                           {QStringLiteral("messages"), messages},
                           {QStringLiteral("questionMessages"), QJsonArray{projection}},
                           {QStringLiteral("questions"), QJsonArray{}}};
    panel.receivePayload(owner, QStringLiteral("history_page"), page);
    panel.show();
    auto* view = panel.findChild<MarkdownView*>();
    QVERIFY(view);
    QVERIFY(view->contains(QStringLiteral("remote-question:part_child_question")));
    view->selectAll();
    const QString before = view->selectedText();
    QVERIFY(before.contains(QStringLiteral("Child fixture question")));
    panel.receivePayload(owner, QStringLiteral("event"), page);
    QCOMPARE(view->selectedText(), before);  // Identical snapshots retain the selected row objects.
    QPushButton* older = nullptr;
    for (auto* button : panel.findChildren<QPushButton*>())
        if (button->text() == QStringLiteral("查看更早消息")) older = button;
    QVERIFY(older && !older->isHidden());
    older->click();
    QCOMPARE(cursors.last(), QStringLiteral("msg_root_00"));
}

void DesktopSessionSyncHostTest::remoteRejectedQuestionRecoveryKeepsItsQuestionBinding() {
    QTemporaryDir spool;
    const QString file = spool.filePath(QStringLiteral("outbox.json"));
    RemoteAgentPanel panel(nullptr, file);
    const QJsonObject owner{{QStringLiteral("id"), QStringLiteral("ses_recovery")},
                            {QStringLiteral("device_slot"), QStringLiteral("mac_recovery")},
                            {QStringLiteral("login_session_id"), QStringLiteral("recovery_login")},
                            {QStringLiteral("online"), true}};
    QList<QJsonObject> commands;
    panel.setCommandHandler(
        [&](const QJsonObject&, const QJsonObject& action, RemoteAgentPanel::Completion completion) {
            commands.append(action);
            completion({});
            if (action.value(QStringLiteral("action")) == QStringLiteral("answer"))
                panel.receivePayload(
                    owner, QStringLiteral("event"),
                    {{QStringLiteral("type"), QStringLiteral("command_result")},
                     {QStringLiteral("command_id"), action.value(QStringLiteral("requestId"))},
                     {QStringLiteral("result"),
                      QJsonObject{{QStringLiteral("ok"), false},
                                  {QStringLiteral("error"), QStringLiteral("question is no longer pending")}}}});
        });
    panel.openSession(owner);
    panel.receivePayload(
        owner, QStringLiteral("event"),
        {{QStringLiteral("ok"), true},
         {QStringLiteral("questions"),
          QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("qst_expired")},
                                 {QStringLiteral("partId"), QStringLiteral("part_expired")},
                                 {QStringLiteral("question"), QStringLiteral("Continue the fixture?")}}}}});
    auto* editor = panel.findChild<QTextEdit*>(QStringLiteral("remoteAgentComposer"));
    auto* recover = panel.findChild<QPushButton*>(QStringLiteral("remoteAgentRecoverDraftButton"));
    QVERIFY(editor && recover);
    editor->setPlainText(QStringLiteral("Original answer"));
    QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(commands.size(), 1);
    QCOMPARE(editor->toPlainText(), QString());  // Rejection retains the failed answer outbox, without auto downgrade.
    recover->click();
    QCOMPARE(editor->toPlainText(), QStringLiteral("Original answer"));
    {
        QFile saved(file);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const QJsonObject state = QJsonDocument::fromJson(saved.readAll()).object();
        const QJsonObject drafts = state.value(QStringLiteral("drafts")).toObject();
        QVERIFY(!drafts.isEmpty());
        const QJsonObject draft = drafts.value(QStringLiteral("mac_recovery:recovery_login:ses_recovery")).toObject();
        QCOMPARE(draft.value(QStringLiteral("answer_question")).toObject().value(QStringLiteral("id")).toString(),
                 QStringLiteral("qst_expired"));
    }
    QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(commands.size(), 2);
    QCOMPARE(commands.last().value(QStringLiteral("action")).toString(), QStringLiteral("answer"));
    QCOMPARE(commands.last().value(QStringLiteral("id")).toString(), QStringLiteral("qst_expired"));
}

void DesktopSessionSyncHostTest::remoteQuestionPartsInterleaveWithLaterParentProgress() {
    QTemporaryDir spool;
    RemoteAgentPanel panel(nullptr, spool.filePath(QStringLiteral("outbox.json")));
    panel.resize(1000, 700);
    const QJsonObject owner{{QStringLiteral("id"), QStringLiteral("ses_interleaving")},
                            {QStringLiteral("device_slot"), QStringLiteral("ios_interleaving")},
                            {QStringLiteral("login_session_id"), QStringLiteral("interleaving_login")},
                            {QStringLiteral("online"), true}};
    panel.openSession(owner);
    const QJsonObject parent{
        {QStringLiteral("id"), QStringLiteral("msg_parent_interleave")},
        {QStringLiteral("role"), QStringLiteral("assistant")},
        {QStringLiteral("created"), 1000},
        {QStringLiteral("parts"),
         QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("part_before")},
                                {QStringLiteral("created"), 1001},
                                {QStringLiteral("kind"), QStringLiteral("text")},
                                {QStringLiteral("text"), QStringLiteral("Parent before question")}},
                    QJsonObject{{QStringLiteral("id"), QStringLiteral("part_after")},
                                {QStringLiteral("created"), 1003},
                                {QStringLiteral("kind"), QStringLiteral("text")},
                                {QStringLiteral("text"), QStringLiteral("Parent after question")}}}}};
    const QString input = QString::fromUtf8(
        QJsonDocument(
            QJsonObject{{QStringLiteral("question"), QStringLiteral("Child asks in the middle")},
                        {QStringLiteral("options"), QJsonArray{QStringLiteral("Keep going"), QStringLiteral("Wait")}}})
            .toJson(QJsonDocument::Compact));
    const QJsonObject child{
        {QStringLiteral("id"), QStringLiteral("msg_child_interleave")},
        {QStringLiteral("sessionId"), QStringLiteral("ses_child_interleave")},
        {QStringLiteral("questionProjection"), true},
        {QStringLiteral("role"), QStringLiteral("assistant")},
        {QStringLiteral("created"), 1000},
        {QStringLiteral("parts"),
         QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("part_middle")},
                                {QStringLiteral("created"), 1002},
                                {QStringLiteral("kind"), QStringLiteral("tool")},
                                {QStringLiteral("tool"), QStringLiteral("question")},
                                {QStringLiteral("input"), input},
                                {QStringLiteral("state"), QStringLiteral("completed")},
                                {QStringLiteral("output"), QStringLiteral("Answer stays with its question")}}}}};
    panel.receivePayload(owner, QStringLiteral("history_page"),
                         {{QStringLiteral("ok"), true},
                          {QStringLiteral("messages"), QJsonArray{parent}},
                          {QStringLiteral("questionMessages"), QJsonArray{child}},
                          {QStringLiteral("questions"), QJsonArray{}}});
    panel.show();
    auto* view = panel.findChild<MarkdownView*>();
    QVERIFY(view);
    view->selectAll();
    const QString text = view->selectedText();
    const int before = text.indexOf(QStringLiteral("Parent before question"));
    const int question = text.indexOf(QStringLiteral("Child asks in the middle"));
    const int answer = text.indexOf(QStringLiteral("Answer stays with its question"));
    const int after = text.indexOf(QStringLiteral("Parent after question"));
    QVERIFY(before >= 0 && question > before && answer > question && after > answer);
    QVERIFY(!text.contains(QStringLiteral("正在思考")));
    view->clearSelection();
    QCoreApplication::processEvents();
    if (qEnvironmentVariableIsSet("MAICHAT_REMOTE_QUESTION_ORDER_CAPTURE"))
        QVERIFY(panel.grab().save(qEnvironmentVariable("MAICHAT_REMOTE_QUESTION_ORDER_CAPTURE")));
}

void DesktopSessionSyncHostTest::remoteOldOwnerUpdatesStayOutsideRootHistoryCursor() {
    QTemporaryDir spool;
    RemoteAgentPanel panel(nullptr, spool.filePath(QStringLiteral("outbox.json")));
    const QJsonObject owner{{QStringLiteral("id"), QStringLiteral("ses_window_update")},
                            {QStringLiteral("device_slot"), QStringLiteral("ios_window_update")},
                            {QStringLiteral("login_session_id"), QStringLiteral("window_login")},
                            {QStringLiteral("online"), true}};
    QStringList cursors;
    panel.setHistoryHandler(
        [&](const QJsonObject&, const QString& before, int, RemoteAgentPanel::Completion completion) {
            cursors.append(before);
            completion({});
        });
    panel.openSession(owner);
    QJsonArray messages;
    for (int index = 9; index >= 0; --index)
        messages.append(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("msg_window_%1").arg(index, 2, 10, QLatin1Char('0'))},
            {QStringLiteral("role"), QStringLiteral("user")},
            {QStringLiteral("created"), 2000 + index},
            {QStringLiteral("parts"),
             QJsonArray{QJsonObject{{QStringLiteral("kind"), QStringLiteral("text")},
                                    {QStringLiteral("text"), QStringLiteral("Recent user %1").arg(index)}}}}});
    panel.receivePayload(owner, QStringLiteral("history_page"),
                         {{QStringLiteral("ok"), true}, {QStringLiteral("messages"), messages}});
    const QJsonObject oldOwner{
        {QStringLiteral("id"), QStringLiteral("msg_aaa_old_owner")},
        {QStringLiteral("role"), QStringLiteral("assistant")},
        {QStringLiteral("created"), 500},
        {QStringLiteral("parts"),
         QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("part_old")},
                                {QStringLiteral("created"), 501},
                                {QStringLiteral("kind"), QStringLiteral("text")},
                                {QStringLiteral("text"), QStringLiteral("Older half stays paged out")}},
                    QJsonObject{{QStringLiteral("id"), QStringLiteral("part_new")},
                                {QStringLiteral("created"), 2012},
                                {QStringLiteral("kind"), QStringLiteral("text")},
                                {QStringLiteral("text"), QStringLiteral("New progress from old owner")}}}}};
    panel.receivePayload(owner, QStringLiteral("event"),
                         {{QStringLiteral("ok"), true}, {QStringLiteral("messages"), QJsonArray{oldOwner}}});
    auto* view = panel.findChild<MarkdownView*>();
    QVERIFY(view);
    view->selectAll();
    QVERIFY(view->selectedText().contains(QStringLiteral("New progress from old owner")));
    QVERIFY(!view->selectedText().contains(QStringLiteral("Older half stays paged out")));
    QPushButton* older = nullptr;
    for (auto* button : panel.findChildren<QPushButton*>())
        if (button->text() == QStringLiteral("查看更早消息")) older = button;
    QVERIFY(older && !older->isHidden());
    older->click();
    QCOMPARE(cursors.last(), QStringLiteral("msg_window_00"));
    panel.receivePayload(owner, QStringLiteral("history_page"),
                         {{QStringLiteral("ok"), true}, {QStringLiteral("messages"), QJsonArray{oldOwner}}});
    view->selectAll();
    QCOMPARE(view->selectedText().count(QStringLiteral("New progress from old owner")), 1);
    QCOMPARE(view->selectedText().count(QStringLiteral("Older half stays paged out")), 1);
}

void DesktopSessionSyncHostTest::remoteToolGroupsKeepMetadataOutsideOrdinaryText() {
    // Exercise the real remote transcript renderer using only an owner snapshot;
    // tools have no locally available arguments/results and must stay separate
    // from selectable answer text, with the same compact disclosure as local runs.
    QTemporaryDir spool;
    RemoteAgentPanel panel(nullptr, spool.filePath(QStringLiteral("outbox.json")));
    panel.resize(1000, 740);
    const QJsonObject owner{{QStringLiteral("id"), QStringLiteral("ses_remote_tool_style")},
                           {QStringLiteral("device_slot"), QStringLiteral("remote_tool_owner")},
                           {QStringLiteral("login_session_id"), QStringLiteral("tool_style_login")},
                           {QStringLiteral("online"), true}};
    panel.openSession(owner);
    const auto textPart = [](const QString& id, const QString& text, int created) {
        return QJsonObject{{QStringLiteral("id"), id},
                           {QStringLiteral("kind"), QStringLiteral("text")},
                           {QStringLiteral("created"), created},
                           {QStringLiteral("text"), text}};
    };
    const auto toolPart = [](const QString& id, const QString& tool, const QString& state, int created) {
        return QJsonObject{{QStringLiteral("id"), id},
                           {QStringLiteral("kind"), QStringLiteral("tool")},
                           {QStringLiteral("created"), created},
                           {QStringLiteral("tool"), tool},
                           {QStringLiteral("state"), state}};
    };
    const QJsonObject message{
        {QStringLiteral("id"), QStringLiteral("msg_remote_tool_style")},
        {QStringLiteral("role"), QStringLiteral("assistant")},
        {QStringLiteral("created"), 1000},
        {QStringLiteral("parts"),
         QJsonArray{textPart(QStringLiteral("part_style_a"), QStringLiteral("开始检查工作区。"), 1001),
                    toolPart(QStringLiteral("part_style_shell"), QStringLiteral("shell"), QStringLiteral("running"),
                             1002),
                    QJsonObject{{QStringLiteral("id"), QStringLiteral("part_private_reasoning")},
                                {QStringLiteral("kind"), QStringLiteral("reasoning")},
                                {QStringLiteral("created"), 1003},
                                {QStringLiteral("text"), QStringLiteral("Private reasoning must not become a row")}},
                    toolPart(QStringLiteral("part_style_search"), QStringLiteral("file_grep"),
                             QStringLiteral("completed"), 1004),
                    textPart(QStringLiteral("part_style_b"), QStringLiteral("工作区干净，继续核对分支。"), 1005),
                    toolPart(QStringLiteral("part_style_last"), QStringLiteral("shell"), QStringLiteral("completed"),
                             1006),
                    textPart(QStringLiteral("part_style_c"), QStringLiteral("分支已核对完成。"), 1007)}}};
    panel.receivePayload(owner, QStringLiteral("history_page"),
                         {{QStringLiteral("ok"), true}, {QStringLiteral("messages"), QJsonArray{message}}});
    panel.show();
    auto* view = panel.findChild<MarkdownView*>();
    QVERIFY(view);
    const auto groups = panel.findChildren<QFrame*>(QStringLiteral("remoteAgentToolGroup"));
    QCOMPARE(groups.size(), 2);
    QVERIFY(view->contains(QStringLiteral("remote-tool-group:part_style_shell")));
    QVERIFY(view->contains(QStringLiteral("remote-tool-group:part_style_last")));
    auto* first = groups.first()->findChild<QAbstractButton*>(QStringLiteral("remoteAgentToolGroupDisclosure"));
    auto* second = groups.last()->findChild<QAbstractButton*>(QStringLiteral("remoteAgentToolGroupDisclosure"));
    QVERIFY(first && second);
    QVERIFY(first->text().contains(QStringLiteral("工具调用 2 次")));
    QVERIFY(first->text().contains(QStringLiteral("运行中")));
    QVERIFY(second->text().contains(QStringLiteral("工具调用 1 次")));
    QVERIFY(second->text().contains(QStringLiteral("已完成")));
    QCOMPARE(first->font().pixelSize(), UiZoom::textPixels(UiZoom::TextRole::Metadata));
    QVERIFY(first->font().pixelSize() < UiZoom::textPixels(UiZoom::TextRole::Body));
    QTRY_VERIFY(first->palette().color(QPalette::ButtonText).lightness() > 100);
    const auto details = groups.first()->findChildren<QLabel*>(QStringLiteral("remoteAgentToolDetail"));
    QCOMPARE(details.size(), 2);
    for (QLabel* detail : details) QVERIFY(!detail->isVisibleTo(groups.first()));
    const QString capture = qEnvironmentVariable("MAICHAT_REMOTE_TOOLS_CAPTURE_DIR");
    if (!capture.isEmpty()) {
        QCoreApplication::processEvents();
        QVERIFY(QDir().mkpath(capture));
        QVERIFY(panel.grab().save(QDir(capture).filePath(QStringLiteral("remote-tool-groups-collapsed.png"))));
    }
    first->click();
    for (QLabel* detail : details) QVERIFY(detail->isVisibleTo(groups.first()));
    QVERIFY(details.first()->text().contains(QStringLiteral("shell")));
    QVERIFY(details.last()->text().contains(QStringLiteral("file_grep")));
    const QStringList rows{QStringLiteral("msg_remote_tool_style"),
                           QStringLiteral("remote-tool-group:part_style_shell"),
                           QStringLiteral("msg_remote_tool_style:segment:part_style_b"),
                           QStringLiteral("remote-tool-group:part_style_last"),
                           QStringLiteral("msg_remote_tool_style:segment:part_style_c")};
    for (int index = 0; index < rows.size(); ++index) {
        QVERIFY2(view->contains(rows[index]), qPrintable(rows[index]));
        if (index > 0) QVERIFY(view->itemRect(rows[index - 1]).bottom() <= view->itemRect(rows[index]).top());
    }
    view->selectAll();
    const QString ordinaryText = view->selectedText();
    const int firstText = ordinaryText.indexOf(QStringLiteral("开始检查工作区。"));
    const int secondText = ordinaryText.indexOf(QStringLiteral("工作区干净，继续核对分支。"));
    const int finalText = ordinaryText.indexOf(QStringLiteral("分支已核对完成。"));
    QVERIFY(firstText >= 0 && secondText > firstText && finalText > secondText);
    QVERIFY(!ordinaryText.contains(QStringLiteral("工具调用")));
    QVERIFY(!ordinaryText.contains(QStringLiteral("running")));
    QVERIFY(!ordinaryText.contains(QStringLiteral("completed")));
    QVERIFY(!ordinaryText.contains(QStringLiteral("Private reasoning")));
    view->clearSelection();
    QCoreApplication::processEvents();
    if (!capture.isEmpty()) {
        QVERIFY(QDir().mkpath(capture));
        QVERIFY(panel.grab().save(QDir(capture).filePath(QStringLiteral("remote-tool-groups-expanded.png"))));
    }
}

void DesktopSessionSyncHostTest::remoteToolGroupsRetainExpandedObjectsDuringUpdatesAndPrepend() {
    // Streaming state and pagination may change geometry but cannot replace an
    // expanded tool group, discard its disclosure choice or move it to the tail.
    QTemporaryDir spool;
    RemoteAgentPanel panel(nullptr, spool.filePath(QStringLiteral("outbox.json")));
    panel.resize(1000, 740);
    const QJsonObject owner{{QStringLiteral("id"), QStringLiteral("ses_tool_update")},
                           {QStringLiteral("device_slot"), QStringLiteral("tool_update_owner")},
                           {QStringLiteral("login_session_id"), QStringLiteral("tool_update_login")},
                           {QStringLiteral("online"), true}};
    panel.openSession(owner);
    QJsonObject tool{{QStringLiteral("id"), QStringLiteral("part_stable_tool")},
                     {QStringLiteral("kind"), QStringLiteral("tool")},
                     {QStringLiteral("created"), 1002},
                     {QStringLiteral("tool"), QStringLiteral("shell")},
                     {QStringLiteral("state"), QStringLiteral("running")}};
    const QJsonObject before{{QStringLiteral("id"), QStringLiteral("part_stable_before")},
                             {QStringLiteral("kind"), QStringLiteral("text")},
                             {QStringLiteral("created"), 1001},
                             {QStringLiteral("text"), QStringLiteral("准备检查工作区。")}};
    const QJsonObject after{{QStringLiteral("id"), QStringLiteral("part_stable_after")},
                            {QStringLiteral("kind"), QStringLiteral("text")},
                            {QStringLiteral("created"), 1003},
                            {QStringLiteral("text"), QStringLiteral("工作区检查完成。")}};
    QJsonObject message{{QStringLiteral("id"), QStringLiteral("msg_stable_tool")},
                         {QStringLiteral("role"), QStringLiteral("assistant")},
                         {QStringLiteral("created"), 1000},
                         {QStringLiteral("parts"), QJsonArray{before, tool, after}}};
    panel.receivePayload(owner, QStringLiteral("history_page"),
                         {{QStringLiteral("ok"), true}, {QStringLiteral("messages"), QJsonArray{message}}});
    panel.show();
    auto* view = panel.findChild<MarkdownView*>();
    QVERIFY(view);
    const auto groups = panel.findChildren<QFrame*>(QStringLiteral("remoteAgentToolGroup"));
    QCOMPARE(groups.size(), 1);
    QFrame* group = groups.first();
    auto* disclosure = group->findChild<QAbstractButton*>(QStringLiteral("remoteAgentToolGroupDisclosure"));
    QVERIFY(disclosure);
    disclosure->click();
    auto* detail = group->findChild<QLabel*>(QStringLiteral("remoteAgentToolDetail"));
    QVERIFY(detail && detail->isVisibleTo(group));
    tool.insert(QStringLiteral("state"), QStringLiteral("completed"));
    message.insert(QStringLiteral("parts"), QJsonArray{before, tool, after});
    const QJsonObject update{{QStringLiteral("ok"), true}, {QStringLiteral("messages"), QJsonArray{message}}};
    panel.receivePayload(owner, QStringLiteral("event"), update);
    QCOMPARE(panel.findChildren<QFrame*>(QStringLiteral("remoteAgentToolGroup")), groups);
    QCOMPARE(group->findChild<QAbstractButton*>(QStringLiteral("remoteAgentToolGroupDisclosure")), disclosure);
    QVERIFY(disclosure->text().contains(QStringLiteral("已完成")));
    QVERIFY(detail->isVisibleTo(group));
    // Replayed state is a no-op for the selected answer and embedded widgets.
    view->selectAll();
    const QString selected = view->selectedText();
    panel.receivePayload(owner, QStringLiteral("event"), update);
    QCOMPARE(view->selectedText(), selected);
    QCOMPARE(panel.findChildren<QFrame*>(QStringLiteral("remoteAgentToolGroup")), groups);
    const QJsonObject older{{QStringLiteral("id"), QStringLiteral("msg_older_tool_history")},
                            {QStringLiteral("role"), QStringLiteral("assistant")},
                            {QStringLiteral("created"), 500},
                            {QStringLiteral("parts"),
                             QJsonArray{QJsonObject{{QStringLiteral("kind"), QStringLiteral("text")},
                                                    {QStringLiteral("text"), QStringLiteral("这是更早的会话记录。")}}}}};
    panel.receivePayload(owner, QStringLiteral("history_page"),
                         {{QStringLiteral("ok"), true}, {QStringLiteral("messages"), QJsonArray{older}}});
    QCOMPARE(panel.findChildren<QFrame*>(QStringLiteral("remoteAgentToolGroup")), groups);
    QCOMPARE(group->findChild<QAbstractButton*>(QStringLiteral("remoteAgentToolGroupDisclosure")), disclosure);
    QVERIFY(detail->isVisibleTo(group));
    const QStringList rows{QStringLiteral("msg_older_tool_history"), QStringLiteral("msg_stable_tool"),
                           QStringLiteral("remote-tool-group:part_stable_tool"),
                           QStringLiteral("msg_stable_tool:segment:part_stable_after")};
    for (int index = 0; index < rows.size(); ++index) {
        QVERIFY(view->contains(rows[index]));
        if (index > 0) QVERIFY(view->itemRect(rows[index - 1]).bottom() <= view->itemRect(rows[index]).top());
    }
    view->selectAll();
    QCOMPARE(view->selectedText().count(QStringLiteral("准备检查工作区。")), 1);
    QCOMPARE(view->selectedText().count(QStringLiteral("工作区检查完成。")), 1);
    view->clearSelection();
    QCoreApplication::processEvents();
    const QString capture = qEnvironmentVariable("MAICHAT_REMOTE_TOOLS_CAPTURE_DIR");
    if (!capture.isEmpty()) {
        QVERIFY(QDir().mkpath(capture));
        QVERIFY(panel.grab().save(QDir(capture).filePath(QStringLiteral("remote-tool-groups-updated.png"))));
    }
    // A correction can join two formerly separate runs. Removing the obsolete
    // group must hide it in this event, before deferred deletion can run.
    QJsonObject lastTool = tool;
    lastTool.insert(QStringLiteral("id"), QStringLiteral("part_merge_tool"));
    lastTool.insert(QStringLiteral("created"), 1004);
    QJsonObject lastText = after;
    lastText.insert(QStringLiteral("id"), QStringLiteral("part_merge_after"));
    lastText.insert(QStringLiteral("created"), 1005);
    lastText.insert(QStringLiteral("text"), QStringLiteral("两项检查均已完成。"));
    message.insert(QStringLiteral("parts"), QJsonArray{before, tool, after, lastTool, lastText});
    panel.receivePayload(owner, QStringLiteral("event"),
                         {{QStringLiteral("ok"), true}, {QStringLiteral("messages"), QJsonArray{message}}});
    const auto splitGroups = panel.findChildren<QFrame*>(QStringLiteral("remoteAgentToolGroup"));
    QCOMPARE(splitGroups.size(), 2);
    QFrame* obsoleteGroup = splitGroups.last();
    QVERIFY(obsoleteGroup != group);
    QVERIFY(!obsoleteGroup->isHidden());
    message.insert(QStringLiteral("parts"), QJsonArray{before, tool, lastTool, lastText});
    panel.receivePayload(owner, QStringLiteral("event"),
                         {{QStringLiteral("ok"), true}, {QStringLiteral("messages"), QJsonArray{message}}});
    QVERIFY(obsoleteGroup->isHidden());  // Deliberately no processEvents after the merge.
    QVERIFY(!view->contains(QStringLiteral("remote-tool-group:part_merge_tool")));
    QVERIFY(view->contains(QStringLiteral("remote-tool-group:part_stable_tool")));
    QCOMPARE(group->findChild<QAbstractButton*>(QStringLiteral("remoteAgentToolGroupDisclosure")), disclosure);
    QVERIFY(detail->isVisibleTo(group));
}

void DesktopSessionSyncHostTest::remoteLegacyToolSnapshotsSplitAtQuestionsAndMediaWithoutLeakingDetails() {
    // Old snapshots do not have part IDs/timestamps. Array order still defines
    // the visible transcript; absent or future enum values cannot imply success,
    // and non-question tool internals must never surface in the remote viewer.
    QTemporaryDir spool;
    RemoteAgentPanel panel(nullptr, spool.filePath(QStringLiteral("outbox.json")));
    panel.resize(1000, 740);
    const QJsonObject owner{{QStringLiteral("id"), QStringLiteral("ses_legacy_tool")},
                           {QStringLiteral("device_slot"), QStringLiteral("legacy_tool_owner")},
                           {QStringLiteral("login_session_id"), QStringLiteral("legacy_tool_login")},
                           {QStringLiteral("online"), true}};
    panel.openSession(owner);
    const QJsonObject first{{QStringLiteral("kind"), QStringLiteral("tool")},
                            {QStringLiteral("tool"), QStringLiteral("shell")},
                            {QStringLiteral("state"), QStringLiteral("future_tool_state")},
                            {QStringLiteral("input"), QStringLiteral("PRIVATE_COMMAND_TOKEN")},
                            {QStringLiteral("output"), QStringLiteral("PRIVATE_RESULT_TOKEN")}};
    const QJsonObject second{{QStringLiteral("kind"), QStringLiteral("tool")},
                             {QStringLiteral("tool"), QStringLiteral("file_grep")}};
    const QJsonObject third{{QStringLiteral("kind"), QStringLiteral("tool")},
                            {QStringLiteral("tool"), QStringLiteral("shell")},
                            {QStringLiteral("state"), QStringLiteral("completed")}};
    const QJsonObject question{
        {QStringLiteral("kind"), QStringLiteral("tool")},
        {QStringLiteral("tool"), QStringLiteral("question")},
        {QStringLiteral("state"), QStringLiteral("running")},
        {QStringLiteral("input"), QJsonObject{{QStringLiteral("question"), QStringLiteral("保留这份检查结果吗？")},
                                            {QStringLiteral("options"), QJsonArray{QStringLiteral("继续")}}}}};
    const QJsonObject message{
        {QStringLiteral("id"), QStringLiteral("msg_legacy_tool")},
        {QStringLiteral("role"), QStringLiteral("assistant")},
        {QStringLiteral("parts"),
         QJsonArray{QJsonObject{{QStringLiteral("id"), QString()},
                                {QStringLiteral("kind"), QStringLiteral("text")},
                                {QStringLiteral("text"), QStringLiteral("开始读取旧版会话记录。")}},
                    first, question, second,
                    QJsonObject{{QStringLiteral("id"), QString()},
                                {QStringLiteral("kind"), QStringLiteral("image")},
                                {QStringLiteral("path"), QStringLiteral("C:/PRIVATE_LOCAL_IMAGE.png")}},
                    third,
                    QJsonObject{{QStringLiteral("id"), QString()},
                                {QStringLiteral("kind"), QStringLiteral("text")},
                                {QStringLiteral("text"), QStringLiteral("旧版会话记录已读取。")}}}}};
    const QJsonObject page{{QStringLiteral("ok"), true}, {QStringLiteral("messages"), QJsonArray{message}}};
    panel.receivePayload(owner, QStringLiteral("history_page"), page);
    panel.show();
    auto* view = panel.findChild<MarkdownView*>();
    QVERIFY(view);
    QCOMPARE(view->itemCount(), 7);  // Three ordinary rows, one question and three tool groups.
    const auto groups = panel.findChildren<QFrame*>(QStringLiteral("remoteAgentToolGroup"));
    QCOMPARE(groups.size(), 3);
    for (int index = 0; index < groups.size(); ++index) {
        auto* disclosure = groups[index]->findChild<QAbstractButton*>(QStringLiteral("remoteAgentToolGroupDisclosure"));
        QVERIFY(disclosure);
        QVERIFY(disclosure->text().contains(index == 2 ? QStringLiteral("已完成") : QStringLiteral("状态未知")));
        QVERIFY(!disclosure->text().contains(QStringLiteral("future_tool_state")));
        disclosure->click();
        const auto details = groups[index]->findChildren<QLabel*>(QStringLiteral("remoteAgentToolDetail"));
        QCOMPARE(details.size(), 1);
        QVERIFY(details.first()->isVisibleTo(groups[index]));
        QVERIFY(!details.first()->text().contains(QStringLiteral("future_tool_state")));
        QVERIFY(!details.first()->text().contains(QStringLiteral("PRIVATE_")));
    }
    view->selectAll();
    const QString selected = view->selectedText();
    const int before = selected.indexOf(QStringLiteral("开始读取旧版会话记录。"));
    const int questionAt = selected.indexOf(QStringLiteral("保留这份检查结果吗？"));
    const int image = selected.indexOf(QStringLiteral("图片在所属设备"));
    const int after = selected.indexOf(QStringLiteral("旧版会话记录已读取。"));
    QVERIFY(before >= 0 && questionAt > before && image > questionAt && after > image);
    QCOMPARE(selected.count(QStringLiteral("开始读取旧版会话记录。")), 1);
    QCOMPARE(selected.count(QStringLiteral("图片在所属设备")), 1);
    QCOMPARE(selected.count(QStringLiteral("旧版会话记录已读取。")), 1);
    QVERIFY(!selected.contains(QStringLiteral("PRIVATE_")));
    QVERIFY(!selected.contains(QStringLiteral("工具调用")));
    QVERIFY(!selected.contains(QStringLiteral("future_tool_state")));
    panel.receivePayload(owner, QStringLiteral("event"), page);
    QCOMPARE(panel.findChildren<QFrame*>(QStringLiteral("remoteAgentToolGroup")), groups);
    QCOMPARE(view->selectedText(), selected);
}

void DesktopSessionSyncHostTest::remoteQuestionAppendRetainsExistingTextSelectionAndToolGroup() {
    // A late question changes neither the identity of already selected prose
    // nor the lifetime/expanded state of the tool row immediately after it.
    QTemporaryDir spool;
    RemoteAgentPanel panel(nullptr, spool.filePath(QStringLiteral("outbox.json")));
    panel.resize(1000, 740);
    const QJsonObject owner{{QStringLiteral("id"), QStringLiteral("ses_append_question")},
                           {QStringLiteral("device_slot"), QStringLiteral("append_question_owner")},
                           {QStringLiteral("login_session_id"), QStringLiteral("append_question_login")},
                           {QStringLiteral("online"), true}};
    panel.openSession(owner);
    const QJsonObject text{{QStringLiteral("id"), QStringLiteral("part_append_text")},
                           {QStringLiteral("kind"), QStringLiteral("text")},
                           {QStringLiteral("created"), 1001},
                           {QStringLiteral("text"), QStringLiteral("工作区检查完成，等待你的补充。")}};
    const QJsonObject tool{{QStringLiteral("id"), QStringLiteral("part_append_tool")},
                           {QStringLiteral("kind"), QStringLiteral("tool")},
                           {QStringLiteral("created"), 1002},
                           {QStringLiteral("tool"), QStringLiteral("shell")},
                           {QStringLiteral("state"), QStringLiteral("completed")}};
    QJsonObject message{{QStringLiteral("id"), QStringLiteral("msg_append_question")},
                         {QStringLiteral("role"), QStringLiteral("assistant")},
                         {QStringLiteral("created"), 1000},
                         {QStringLiteral("parts"), QJsonArray{text, tool}}};
    panel.receivePayload(owner, QStringLiteral("history_page"),
                         {{QStringLiteral("ok"), true}, {QStringLiteral("messages"), QJsonArray{message}}});
    panel.show();
    auto* view = panel.findChild<MarkdownView*>();
    QVERIFY(view);
    QVERIFY(view->contains(QStringLiteral("msg_append_question")));
    const auto groups = panel.findChildren<QFrame*>(QStringLiteral("remoteAgentToolGroup"));
    QCOMPARE(groups.size(), 1);
    QFrame* group = groups.first();
    auto* disclosure = group->findChild<QAbstractButton*>(QStringLiteral("remoteAgentToolGroupDisclosure"));
    QVERIFY(disclosure);
    disclosure->click();
    auto* detail = group->findChild<QLabel*>(QStringLiteral("remoteAgentToolDetail"));
    QVERIFY(detail && detail->isVisibleTo(group));
    view->selectAll();
    const QString selected = view->selectedText();
    QCOMPARE(selected.trimmed(), text.value(QStringLiteral("text")).toString());
    const QJsonObject question{
        {QStringLiteral("id"), QStringLiteral("part_appended_question")},
        {QStringLiteral("kind"), QStringLiteral("tool")},
        {QStringLiteral("created"), 1003},
        {QStringLiteral("tool"), QStringLiteral("question")},
        {QStringLiteral("state"), QStringLiteral("running")},
        {QStringLiteral("input"), QJsonObject{{QStringLiteral("question"), QStringLiteral("接下来检查哪个分支？")},
                                            {QStringLiteral("options"), QJsonArray{QStringLiteral("当前分支")}}}}};
    message.insert(QStringLiteral("parts"), QJsonArray{text, tool, question});
    panel.receivePayload(owner, QStringLiteral("event"),
                         {{QStringLiteral("ok"), true}, {QStringLiteral("messages"), QJsonArray{message}}});
    QVERIFY(view->contains(QStringLiteral("msg_append_question")));
    QCOMPARE(view->selectedText(), selected);
    QCOMPARE(panel.findChildren<QFrame*>(QStringLiteral("remoteAgentToolGroup")), groups);
    QCOMPARE(group->findChild<QAbstractButton*>(QStringLiteral("remoteAgentToolGroupDisclosure")), disclosure);
    QVERIFY(detail->isVisibleTo(group));
    const QStringList rows{QStringLiteral("msg_append_question"),
                           QStringLiteral("remote-tool-group:part_append_tool"),
                           QStringLiteral("remote-question:part_appended_question")};
    for (int index = 0; index < rows.size(); ++index) {
        QVERIFY(view->contains(rows[index]));
        if (index > 0) QVERIFY(view->itemRect(rows[index - 1]).bottom() <= view->itemRect(rows[index]).top());
    }
}

void DesktopSessionSyncHostTest::remoteChildQuestionProjectionRetainsLaterTextIdentityAndToolGroups() {
    // The source assistant becomes two chronological fragments when a child
    // question arrives in its middle. IDs must still refer to the source
    // message/part, rather than nesting an extra ':segment:' prefix on C.
    QTemporaryDir spool;
    RemoteAgentPanel panel(nullptr, spool.filePath(QStringLiteral("outbox.json")));
    panel.resize(1000, 740);
    const QJsonObject owner{{QStringLiteral("id"), QStringLiteral("ses_projected_tool_question")},
                           {QStringLiteral("device_slot"), QStringLiteral("projected_question_owner")},
                           {QStringLiteral("login_session_id"), QStringLiteral("projected_question_login")},
                           {QStringLiteral("online"), true}};
    panel.openSession(owner);
    const auto textPart = [](const QString& id, const QString& text, int created) {
        return QJsonObject{{QStringLiteral("id"), id},
                           {QStringLiteral("kind"), QStringLiteral("text")},
                           {QStringLiteral("created"), created},
                           {QStringLiteral("text"), text}};
    };
    const auto toolPart = [](const QString& id, int created) {
        return QJsonObject{{QStringLiteral("id"), id},
                           {QStringLiteral("kind"), QStringLiteral("tool")},
                           {QStringLiteral("created"), created},
                           {QStringLiteral("tool"), QStringLiteral("shell")},
                           {QStringLiteral("state"), QStringLiteral("completed")}};
    };
    const QString finalText = QStringLiteral("分支与工作区均已核对完成。");
    const QJsonObject message{
        {QStringLiteral("id"), QStringLiteral("msg_parent_projected_question")},
        {QStringLiteral("role"), QStringLiteral("assistant")},
        {QStringLiteral("created"), 1000},
        {QStringLiteral("parts"),
         QJsonArray{textPart(QStringLiteral("part_projected_a"), QStringLiteral("开始检查工作区。"), 1001),
                    toolPart(QStringLiteral("part_projected_tool1"), 1002),
                    textPart(QStringLiteral("part_projected_b"), QStringLiteral("工作区正常，继续检查分支。"), 1004),
                    toolPart(QStringLiteral("part_projected_tool2"), 1005),
                    textPart(QStringLiteral("part_projected_c"), finalText, 1006)}}};
    panel.receivePayload(owner, QStringLiteral("history_page"),
                         {{QStringLiteral("ok"), true}, {QStringLiteral("messages"), QJsonArray{message}}});
    panel.show();
    auto* view = panel.findChild<MarkdownView*>();
    QVERIFY(view);
    const auto groups = panel.findChildren<QFrame*>(QStringLiteral("remoteAgentToolGroup"));
    QCOMPARE(groups.size(), 2);
    auto* disclosure = groups.last()->findChild<QAbstractButton*>(QStringLiteral("remoteAgentToolGroupDisclosure"));
    QVERIFY(disclosure);
    disclosure->click();
    auto* detail = groups.last()->findChild<QLabel*>(QStringLiteral("remoteAgentToolDetail"));
    QVERIFY(detail && detail->isVisibleTo(groups.last()));
    const QString finalId = QStringLiteral("msg_parent_projected_question:segment:part_projected_c");
    QVERIFY(view->contains(finalId));
    QCoreApplication::processEvents();
    const QRectF finalRect = view->itemRect(finalId);
    const QPoint from(qRound(finalRect.left()) + 1,
                      qRound(finalRect.center().y()) - view->verticalScrollBar()->value());
    const QPoint to(qRound(finalRect.right()) - 1, from.y());
    auto* window = panel.windowHandle();
    QVERIFY(window);
    const QPoint windowFrom = window->mapFromGlobal(view->viewport()->mapToGlobal(from));
    const QPoint windowTo = window->mapFromGlobal(view->viewport()->mapToGlobal(to));
    // Use the same QPA press/move/release stream as MarkdownView's selection
    // regressions; the QWidget move overload need not deliver a drag on Qt 5.
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, windowFrom);
    QTest::mouseMove(window, windowTo);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, windowTo);
    QCOMPARE(view->selectedText(), finalText);
    const QJsonObject questionInput{{QStringLiteral("question"), QStringLiteral("子任务需要保留哪些结果？")},
                                    {QStringLiteral("options"), QJsonArray{QStringLiteral("全部保留")}}};
    const QJsonObject questionPart{{QStringLiteral("id"), QStringLiteral("part_child_projected_question")},
                                   {QStringLiteral("kind"), QStringLiteral("tool")},
                                   {QStringLiteral("created"), 1003},
                                   {QStringLiteral("tool"), QStringLiteral("question")},
                                   {QStringLiteral("state"), QStringLiteral("running")},
                                   {QStringLiteral("input"), questionInput}};
    const QJsonObject child{{QStringLiteral("id"), QStringLiteral("msg_child_projected_question")},
                            {QStringLiteral("sessionId"), QStringLiteral("ses_child_projected_question")},
                            {QStringLiteral("questionProjection"), true},
                            {QStringLiteral("role"), QStringLiteral("assistant")},
                            {QStringLiteral("created"), 1000},
                            {QStringLiteral("parts"), QJsonArray{questionPart}}};
    panel.receivePayload(owner, QStringLiteral("event"),
                         {{QStringLiteral("ok"), true},
                          {QStringLiteral("messages"), QJsonArray{message}},
                          {QStringLiteral("questionMessages"), QJsonArray{child}}});
    QVERIFY(view->contains(finalId));
    QCOMPARE(view->selectedText(), finalText);
    QCOMPARE(panel.findChildren<QFrame*>(QStringLiteral("remoteAgentToolGroup")), groups);
    QCOMPARE(groups.last()->findChild<QAbstractButton*>(QStringLiteral("remoteAgentToolGroupDisclosure")), disclosure);
    QVERIFY(detail->isVisibleTo(groups.last()));
    const QStringList rows{QStringLiteral("msg_parent_projected_question"),
                           QStringLiteral("remote-tool-group:part_projected_tool1"),
                           QStringLiteral("remote-question:part_child_projected_question"),
                           QStringLiteral("msg_parent_projected_question:segment:part_projected_b"),
                           QStringLiteral("remote-tool-group:part_projected_tool2"), finalId};
    for (int index = 0; index < rows.size(); ++index) {
        QVERIFY2(view->contains(rows[index]), qPrintable(rows[index]));
        if (index > 0) QVERIFY(view->itemRect(rows[index - 1]).bottom() <= view->itemRect(rows[index]).top());
    }
}

QTEST_MAIN(DesktopSessionSyncHostTest)
#include "DesktopSessionSyncHostTest.moc"
