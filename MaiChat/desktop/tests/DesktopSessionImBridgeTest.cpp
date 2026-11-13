// Owned IM/model fixtures exercise the ordinary binding and reply path without
// accounts, network requests, OS clipboard access or a running user application.
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFuture>
#include <QFutureWatcher>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSemaphore>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QThreadPool>
#include <QUuid>
#include <QtTest>
#include <QtConcurrent/QtConcurrentRun>

#include <atomic>
#include <algorithm>
#include <memory>
#include <mutex>

#include "agent/AgentController.h"
#include "agent/AgentSessionList.h"
#include "agent/DesktopSessionImBridge.h"
#include "app/RemoteIMApplication.h"
#include "im/RemoteIMClient.h"
#include "../../../MaiAgent/third_party/sqlite/sqlite3.h"

namespace {
struct OwnedCoreWriteLock {
    sqlite3* database = nullptr;
    bool acquire(const QString& path) {
        const auto file = path.toUtf8();
        return sqlite3_open_v2(file.constData(), &database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr) == SQLITE_OK &&
            sqlite3_exec(database, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK;
    }
    void release() {
        if (!database) return;
        sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr);
        sqlite3_close_v2(database); database = nullptr;
    }
    ~OwnedCoreWriteLock() { release(); }
};
qint64 ownedCoreCount(const QString& path, const char* sql) {
    sqlite3* database = nullptr;
    const auto filename = path.toUtf8();
    if (sqlite3_open_v2(filename.constData(), &database, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
        if (database) sqlite3_close_v2(database);
        return -1;
    }
    sqlite3_stmt* statement = nullptr;
    qint64 value = -1;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) == SQLITE_OK && sqlite3_step(statement) == SQLITE_ROW)
        value = sqlite3_column_int64(statement, 0);
    if (statement) sqlite3_finalize(statement);
    sqlite3_close_v2(database);
    return value;
}

QString ownedCoreText(const QString& path, const char* sql) {
    sqlite3* database = nullptr;
    const auto filename = path.toUtf8();
    if (sqlite3_open_v2(filename.constData(), &database, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
        if (database) sqlite3_close_v2(database);
        return {};
    }
    sqlite3_stmt* statement = nullptr;
    QString value;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) == SQLITE_OK && sqlite3_step(statement) == SQLITE_ROW) {
        const auto* bytes = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
        value = QString::fromUtf8(bytes, sqlite3_column_bytes(statement, 0));
    }
    if (statement) sqlite3_finalize(statement);
    sqlite3_close_v2(database);
    return value;
}

class OwnedIm final : public RemoteIMClient {
public:
    struct Sent {
        QString peer;
        QString text;
        RemoteIMMessageOrigin origin;
        QString kind = QStringLiteral("text");
        QString path;
        RemoteIMQuote quote;
        bool hasQuote = false;
    };
    QList<Sent> sent;
    RemoteDiagnostics::AccountTag account;
    RemoteIMSendCompletion deferred;
    bool defer = false;
    bool phasedLive = false;
    std::mutex rawPosting;
    std::atomic<quint64> rawSequence{0};
    quint64 rawGeneration = 0;
    quint64 liveMetadataSequence() const override { return rawSequence.load(); }
    void withLiveMetadataBoundary(const std::function<void(LiveMetadataBoundary)>& publish) override {
        std::lock_guard<std::mutex> lock(rawPosting);
        publish({rawGeneration, rawSequence.load()});
    }
    bool hasExplicitLiveReadiness() const override { return phasedLive; }
    bool deferDisconnect = false;
    RemoteIMCompletion deferredDisconnect;
    RemoteDiagnostics::AccountTag currentAccount() const override { return account; }
    void connectToService(int app, const QString& owner, const QString&, RemoteIMCompletion done) override {
        ++rawGeneration;
        rawSequence = 0;
        account = {quint64(app), owner};
        if (done) done(true, {});
    }
    void disconnectFromService(RemoteIMCompletion done) override {
        if (deferDisconnect) { deferredDisconnect = std::move(done); return; }
        account = {};
        emit disconnected();
        if (done) done(true, {});
    }
    void deleteContact(const QString&, RemoteIMCompletion done) override { if (done) done(true, {}); }
    void sendText(const QString& peer, const QString& text, RemoteIMSendCompletion done) override {
        send(peer, text, RemoteIMMessageOrigin::Human, std::move(done));
    }
    void sendMachineText(const QString& peer, const QString& text, RemoteIMSendCompletion done) override {
        send(peer, text, RemoteIMMessageOrigin::Machine, std::move(done));
    }
    void sendMachineTextWithQuote(const QString& peer, const QString& text, const RemoteIMQuote& quote,
                                  bool hasQuote, RemoteIMSendCompletion done) override {
        send(peer, text, RemoteIMMessageOrigin::Machine, std::move(done), QStringLiteral("text"), {}, quote, hasQuote);
    }
    void sendMachineImageWithText(const QString& peer, const QString& path, const QString& text, bool,
                                   const RemoteIMQuote& quote, bool hasQuote, RemoteIMSendCompletion done) override {
        send(peer, text, RemoteIMMessageOrigin::Machine, std::move(done), QStringLiteral("image"), path, quote, hasQuote);
    }
    void sendMachineFileWithText(const QString& peer, const QString& path, const QString&, const QString& text, bool,
                                  const RemoteIMQuote& quote, bool hasQuote, RemoteIMSendCompletion done) override {
        send(peer, text, RemoteIMMessageOrigin::Machine, std::move(done), QStringLiteral("file"), path, quote, hasQuote);
    }
    void sendMachineVideoWithText(const QString& peer, const RemoteIMVideoPayload& video, const QString& text, bool,
                                   const RemoteIMQuote& quote, bool hasQuote, RemoteIMSendCompletion done) override {
        QVERIFY(video.isValid());
        send(peer, text, RemoteIMMessageOrigin::Machine, std::move(done), QStringLiteral("video"), video.videoPath, quote, hasQuote);
    }
    void sendImage(const QString&, const QString&, RemoteIMSendCompletion done) override {
        if (done) done(false, QStringLiteral("owned unsupported image"), {});
    }
    void sendVoice(const QString&, const QString&, int, RemoteIMCompletion done) override {
        if (done) done(false, QStringLiteral("owned unsupported voice"));
    }
    void incoming(QString id, QString text, RemoteIMMessageOrigin origin = RemoteIMMessageOrigin::Human,
                  QString peer = QStringLiteral("friend"), bool live = true) {
        RemoteIMMessage message;
        message.id = std::move(id);
        message.fromUserId = peer;
        message.toUserId = account.ownerUserId;
        message.text = std::move(text);
        message.origin = origin;
        if (live) emit liveMessagesReceived({message});
        else emit messagesReceived({message});
    }
    quint64 queueRaw(RemoteIMMessage message, bool ready) {
        phasedLive = true;
        std::lock_guard<std::mutex> lock(rawPosting);
        const auto sequence = rawSequence.fetch_add(1) + 1;
        const auto generation = rawGeneration;
        const auto owner = account;
        QTimer::singleShot(0, this, [this, message, ready, sequence, generation, owner] {
            if (generation != rawGeneration || owner != account) return;
            emit liveMessageAnnounced(10000 + sequence, message, sequence, generation);
            if (ready) { emit liveMessageReady(10000 + sequence, message); emit liveMessagesReceived({message}); }
            emit liveMetadataBatchFinished(sequence, true, generation);
        });
        return 10000 + sequence;
    }
private:
    void send(const QString& peer, const QString& text, RemoteIMMessageOrigin origin, RemoteIMSendCompletion done,
              const QString& kind = QStringLiteral("text"), const QString& path = {},
              const RemoteIMQuote& quote = {}, bool hasQuote = false) {
        sent.append({peer, text, origin, kind, path, quote, hasQuote});
        if (defer) { defer = false; deferred = std::move(done); }
        else if (done) done(true, {}, {QStringLiteral("owned-receipt-%1").arg(sent.size()), 1000 + sent.size()});
    }
};

struct ModelState {
    std::atomic<int> calls{0};
    std::atomic<bool> blockFirst{false};
    std::atomic<bool> failFirst{false};
    std::atomic<bool> emptyReply{false};
    QSemaphore release;
    QSemaphore entered;
    std::mutex mutex;
    std::vector<MaiModelRequest> requests;
    QString mediaPath;
    QString mediaType;
    QString observationPath;
};

class OwnedModel final : public MaiModelClient {
public:
    explicit OwnedModel(std::shared_ptr<ModelState> state) : state_(std::move(state)) {}
    MaiWireApi wireApi() const override { return MaiWireApi::ChatCompletions; }
    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>& canceled) override {
        const int call = ++state_->calls;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->requests.push_back(request);
        }
        state_->entered.release();
        if (call == 1 && state_->blockFirst) state_->release.tryAcquire(1, 5000);
        if (canceled) return {};
        if (state_->emptyReply) return {};
        if (call == 1 && (!state_->mediaPath.isEmpty() || !state_->observationPath.isEmpty())) {
            QJsonObject input;
            QString tool;
            if (!state_->mediaPath.isEmpty()) {
                tool = QStringLiteral("agent_send_media");
                input = {{QStringLiteral("file_path"), state_->mediaPath},
                         {QStringLiteral("type"), state_->mediaType},
                         {QStringLiteral("caption"), QStringLiteral("owned media caption")}};
            } else {
                tool = QStringLiteral("view_image");
                input = {{QStringLiteral("path"), state_->observationPath}};
            }
            if (sink.onToolCall)
                sink.onToolCall({"owned-media-call", tool.toStdString(),
                                 QJsonDocument(input).toJson(QJsonDocument::Compact).toStdString()});
            return {};
        }
        if (sink.onReasoning) sink.onReasoning("private reasoning must stay local");
        if (sink.onText) sink.onText(!state_->mediaPath.isEmpty() || !state_->observationPath.isEmpty()
                                        ? "owned completed response" : "reply: " + request.messages.back().content);
        if (call == 1 && state_->failFirst) return {MaiErrorCode::Network, "owned private failure detail"};
        return {};
    }
private:
    std::shared_ptr<ModelState> state_;
};

struct Fixture {
    QTemporaryDir workspace;
    QTemporaryDir historyDirectory;
    bool sqliteCore = false;
    std::shared_ptr<ModelState> model = std::make_shared<ModelState>();
    OwnedIm* im = new OwnedIm;
    LocalMessageDatabase* history = new LocalMessageDatabase(historyDirectory.filePath(QStringLiteral("owned-im.db")));
    RemoteIMApplication app{QStringLiteral("owner"), std::unique_ptr<RemoteIMClient>(im), std::unique_ptr<LocalMessageDatabase>(history)};
    AgentController controller{std::make_unique<OwnedModel>(model), sqliteCore ?
        historyDirectory.filePath(QStringLiteral("owned-session-core.db")) : QString()};
    DesktopSessionImBridge bridge{app, controller};
    QString session;
    explicit Fixture(bool useSqliteCore = false) : sqliteCore(useSqliteCore) {
        app.addContact(QStringLiteral("friend"), QStringLiteral("Owned Friend"));
        app.addContact(QStringLiteral("other"), QStringLiteral("Other Friend"));
        app.connectToService(123, QStringLiteral("owned-unused-signature"));
        session = controller.createSession(workspace.path(), QStringLiteral("Owned Creation Session"));
    }
    void incomingFile(const QString& id, const QString& path) {
        RemoteIMMessage message;
        message.id = id;
        message.fromUserId = QStringLiteral("friend");
        message.toUserId = QStringLiteral("owner");
        message.origin = RemoteIMMessageOrigin::Human;
        message.hasFile = true;
        message.file.localPath = path;
        emit im->liveMessagesReceived({message});
    }
    bool bind(const QString& peer, const QString& target, QString* error = nullptr) {
        if (!bridge.bind(peer, target, error)) return false;
        if (controller.agent().isBusy(target.toStdString())) return true;
        return QTest::qWaitFor([&] { return bridge.boundSession(peer) == target && !bridge.historyPending(peer); }, 10000);
    }
    ~Fixture() { model->release.release(); }
};

class OwnedPoolBlocker {
public:
    explicit OwnedPoolBlocker(QThreadPool* pool) : pool_(pool), previousMax_(pool->maxThreadCount()) {
        pool_->setMaxThreadCount(1);
        future_ = QtConcurrent::run(pool_, [state = state_] {
            state->entered.release();
            state->released.acquire();
        });
    }
    ~OwnedPoolBlocker() { release(); }
    bool entered() { return state_->entered.tryAcquire(1, 1000); }
    void release() {
        if (released_) return;
        released_ = true;
        state_->released.release();
        pool_->setMaxThreadCount(previousMax_);
        future_.waitForFinished();  // Only this test's released blocker, never the pool.
    }
private:
    struct State { QSemaphore entered; QSemaphore released; };
    QThreadPool* pool_;
    int previousMax_;
    bool released_ = false;
    std::shared_ptr<State> state_ = std::make_shared<State>();
    QFuture<void> future_;
};

}  // namespace

class DesktopSessionImBridgeTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();
    void machineSendUsesMachineOriginAndRejectsLateAccountReceipt();
    void machineSendRevocationLeavesNoPendingRow();
    void logoutBeginningRejectsInputBeforeSdkDisconnectCompletes();
    void bindingIsExplicitAndExclusive();
    void liveHumanMessageRepliesOnlyToItsOriginalPeer();
    void cachedImageVideoAndFileUseExistingSessionInputs_data();
    void cachedImageVideoAndFileUseExistingSessionInputs();
    void explicitOwnedMediaUsesMachineDelivery_data();
    void explicitOwnedMediaUsesMachineDelivery();
    void toolObservationImageIsNeverAutomaticallyDelivered();
    void suspendRejectsNewIngressBeforeDisconnectStarts();
    void unbindDuringAttachmentPreparationNeverSubmitsLateInput();
    void workspaceChangeDuringPreparationRejectsTheStagedInput();
    void workspaceChangeBeforeIdleDoesNotSendAnotherDirectoriesMedia();
    void busyLocalTurnIsNotSteeredAndPendingInputStaysOrdered();
    void priorQueuedIdleCannotRetireAnUnfinishedOwnedReply();
    void delayedPreBindingUserReceiptDoesNotCountAsTakeover();
    void synchronousTakeoverBeforeSdkDispatchRevokesTheReply();
    void failedModelDoesNotForwardPartialOutputOrRetry();
    void emptyAssistantReportsReadableLocalErrorWithoutFakeDelivery();
    void unbindAndLocalTakeoverDiscardLateReplies();
    void unsupportedPayloadAndPendingLimitFailExplicitly();
    void attachmentPreparationIgnoresGlobalWorkAndExpiresWithoutBlockingLaterText();
    void asynchronousVideoPreparationKeepsHeartbeatAndReplyFifo();
    void lateSessionErrorDuringVideoPreparationPreventsSdkMediaAndAdvancesFifo();
    void unbindDuringVideoPreparationPreventsLateSdkDispatch();
    void localTakeoverRevokesEverySdkScopedBindingForTheSession();
    void metadataAdmissionKeepsMediaAheadOfLaterReadyText();
    void readinessCannotAdoptReboundSessionAndFailureReleasesHead();
    void allPersistedHistoryImportsBeforeOneLaterLivePromptWithoutUsingChatWindow();
    void unavailableHistorySourceDoesNotEnableAutomaticForwarding();
    void queuedPrebindRawMediaStaysContextAndPostbindFrameExecutesOnce();
    void liveOverlapIsExcludedFromSnapshotUsingConfirmedPersistence();
    void overflowDuringHistoryAbortsBindingAndRetainsImRows();
    void partialHistoryFailureNeverPublishesStagedRows();
    void partialHistoryFailureNeverPublishesStagedRows_data();
    void metadataFailureBeforeFenceCaptureAbortsBootstrap();
    void reopenedStartupProjectionReclaimsAbandonedContextPayload();
private:
    QString previousOrganization_, previousApplication_;
    QSettings::Format previousFormat_ = QSettings::NativeFormat;
    bool previousTestMode_ = false;
    QTemporaryDir settings_;
};

void DesktopSessionImBridgeTest::initTestCase() {
    previousOrganization_ = QCoreApplication::organizationName();
    previousApplication_ = QCoreApplication::applicationName();
    previousFormat_ = QSettings::defaultFormat();
    previousTestMode_ = QStandardPaths::isTestModeEnabled();
    QCoreApplication::setOrganizationName(QStringLiteral("MaiChatSessionImOwned"));
    QCoreApplication::setApplicationName(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
    QStandardPaths::setTestModeEnabled(true);
}

void DesktopSessionImBridgeTest::cleanupTestCase() {
    QSettings::setDefaultFormat(previousFormat_);
    QStandardPaths::setTestModeEnabled(previousTestMode_);
    QCoreApplication::setOrganizationName(previousOrganization_);
    QCoreApplication::setApplicationName(previousApplication_);
}

void DesktopSessionImBridgeTest::machineSendUsesMachineOriginAndRejectsLateAccountReceipt() {
    Fixture f;
    QTRY_VERIFY(f.app.isConnected());
    f.im->defer = true;
    int callbacks = 0;
    bool delivered = true;
    QVERIFY(f.app.sendMachineTextTo(QStringLiteral("friend"), QStringLiteral("owned response"),
        [&](bool ok, const QString&, const RemoteIMSendReceipt&) { ++callbacks; delivered = ok; }));
    QCOMPARE(f.im->sent.size(), 1);
    QCOMPARE(f.im->sent.first().origin, RemoteIMMessageOrigin::Machine);
    QCOMPARE(f.app.chatState().messagesWith(QStringLiteral("friend")).last().origin, RemoteIMMessageOrigin::Machine);
    f.app.disconnectFromService();
    QVERIFY(bool(f.im->deferred));
    f.im->deferred(true, {}, {QStringLiteral("late-old-account"), 9999});
    QTRY_COMPARE(callbacks, 1);
    QVERIFY(!delivered);
    for (const auto& message : f.app.chatState().messagesWith(QStringLiteral("friend")))
        QVERIFY(message.id != QStringLiteral("late-old-account"));
}

void DesktopSessionImBridgeTest::machineSendRevocationLeavesNoPendingRow() {
    Fixture f;
    QTRY_VERIFY(f.app.isConnected());
    bool allowed = true;
    connect(&f.app, &RemoteIMApplication::stateChanged, &f.app, [&] { allowed = false; });
    bool delivered = true;
    int completed = 0;
    QVERIFY(!f.app.sendMachineTextTo(QStringLiteral("friend"), QStringLiteral("revoked before dispatch"),
        [&](bool ok, const QString&, const RemoteIMSendReceipt&) { delivered = ok; ++completed; },
        [&] { return allowed; }));
    QCOMPARE(completed, 1);
    QVERIFY(!delivered);
    QVERIFY(f.im->sent.isEmpty());
    const auto messages = f.app.chatState().messagesWith(QStringLiteral("friend"));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages.last().status, RemoteIMMessageStatus::Failed);
}

void DesktopSessionImBridgeTest::logoutBeginningRejectsInputBeforeSdkDisconnectCompletes() {
    Fixture f;
    QTRY_VERIFY(f.app.connectionReady());
    QVERIFY(f.bind(QStringLiteral("friend"), f.session));
    f.im->deferDisconnect = true;
    f.app.disconnectFromService();
    QVERIFY(f.app.isConnected());
    QVERIFY(!f.app.connectionReady());
    f.im->incoming(QStringLiteral("during-owned-logout"), QStringLiteral("must not execute"));
    QVERIFY(!f.app.sendMachineTextTo(QStringLiteral("friend"), QStringLiteral("must not send")));
    QTest::qWait(40);
    QCOMPARE(f.model->calls.load(), 0);
    QVERIFY(f.im->sent.isEmpty());
    QVERIFY(bool(f.im->deferredDisconnect));
    f.im->account = {};
    f.im->deferredDisconnect(true, {});
}

void DesktopSessionImBridgeTest::bindingIsExplicitAndExclusive() {
    Fixture f;
    QTRY_VERIFY(f.app.isConnected());
    QVERIFY(f.bridge.boundSession(QStringLiteral("friend")).isEmpty());
    QString error;
    QVERIFY(!f.bind(QStringLiteral("friend"), QStringLiteral("missing-session"), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(f.bind(QStringLiteral("friend"), f.session, &error));
    QCOMPARE(f.bridge.boundSession(QStringLiteral("friend")), f.session);
    QVERIFY(!f.bind(QStringLiteral("other"), f.session, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(f.model->calls.load(), 0);
    QVERIFY(f.im->sent.isEmpty());
    f.bridge.unbind(QStringLiteral("friend"));
    QVERIFY(f.bridge.boundSession(QStringLiteral("friend")).isEmpty());
}

void DesktopSessionImBridgeTest::liveHumanMessageRepliesOnlyToItsOriginalPeer() {
    Fixture f;
    QTRY_VERIFY(f.app.isConnected());
    QVERIFY(f.bind(QStringLiteral("friend"), f.session));
    f.im->incoming(QStringLiteral("history"), QStringLiteral("old history"), RemoteIMMessageOrigin::Human,
                   QStringLiteral("friend"), false);
    f.im->incoming(QStringLiteral("machine"), QStringLiteral("machine echo"), RemoteIMMessageOrigin::Machine);
    f.im->incoming(QStringLiteral("unknown"), QStringLiteral("legacy unknown"), RemoteIMMessageOrigin::Unknown);
    f.im->incoming(QStringLiteral("other"), QStringLiteral("wrong peer"), RemoteIMMessageOrigin::Human,
                   QStringLiteral("other"));
    QCOMPARE(f.model->calls.load(), 0);
    f.app.selectPeer(QStringLiteral("other"));
    f.im->incoming(QStringLiteral("owned-source"), QStringLiteral("owned task"));
    f.im->incoming(QStringLiteral("owned-source"), QStringLiteral("owned task"));
    QTRY_COMPARE(f.im->sent.size(), 1);
    QCOMPARE(f.model->calls.load(), 1);
    QCOMPARE(f.im->sent.first().peer, QStringLiteral("friend"));
    QCOMPARE(f.im->sent.first().origin, RemoteIMMessageOrigin::Machine);
    QCOMPARE(f.im->sent.first().text, QStringLiteral("reply: owned task"));
    QVERIFY(f.im->sent.first().hasQuote);
    QCOMPARE(f.im->sent.first().quote.digest, QStringLiteral("owned task"));
    QVERIFY(!f.im->sent.first().text.contains(QStringLiteral("private reasoning")));
    QCOMPARE(f.app.chatState().selectedPeerId(), QStringLiteral("other"));
}

void DesktopSessionImBridgeTest::cachedImageVideoAndFileUseExistingSessionInputs_data() {
    QTest::addColumn<QString>("kind");
    QTest::newRow("image") << QStringLiteral("image");
    QTest::newRow("video") << QStringLiteral("video");
    QTest::newRow("file") << QStringLiteral("file");
    QTest::newRow("audio-file") << QStringLiteral("audio");
}

void DesktopSessionImBridgeTest::cachedImageVideoAndFileUseExistingSessionInputs() {
    QFETCH(QString, kind);
    Fixture f;
    QTRY_VERIFY(f.app.connectionReady());
    QTemporaryDir cache;
    const QString path = cache.filePath(kind == QStringLiteral("image") ? QStringLiteral("owned.png") :
        kind == QStringLiteral("video") ? QStringLiteral("owned.mp4") : QStringLiteral("owned.txt"));
    if (kind == QStringLiteral("image")) {
        QImage image(24, 18, QImage::Format_RGB32);
        image.fill(Qt::cyan);
        QVERIFY(image.save(path, "PNG"));
    } else if (kind == QStringLiteral("video")) {
        QVERIFY(QFile::copy(QString::fromUtf8(MAICHAT_SESSION_IM_VIDEO_FIXTURE), path));
    } else {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("owned cached contents"), qint64(21));
    }
    QVERIFY(f.bind(QStringLiteral("friend"), f.session));
    RemoteIMMessage incoming;
    incoming.id = QStringLiteral("owned-media-source-%1").arg(kind);
    incoming.fromUserId = QStringLiteral("friend");
    incoming.toUserId = QStringLiteral("owner");
    incoming.origin = RemoteIMMessageOrigin::Human;
    incoming.text = QStringLiteral("inspect owned attachment");
    incoming.hasQuote = true;
    incoming.quote = {{}, QStringLiteral("friend"), QStringLiteral("owned quoted context"), QStringLiteral("text")};
    if (kind == QStringLiteral("image")) { incoming.hasImage = true; incoming.image.localPath = path; }
    else if (kind == QStringLiteral("video")) { incoming.hasVideo = true; incoming.video.localPath = path; }
    else if (kind == QStringLiteral("audio")) { incoming.hasVoice = true; incoming.voice.localPath = path; }
    else { incoming.hasFile = true; incoming.file.localPath = path; incoming.file.fileName = QStringLiteral("owned.txt"); }
    emit f.im->liveMessagesReceived({incoming});
    QTRY_COMPARE(f.im->sent.size(), 1);
    MaiModelRequest request;
    {
        std::lock_guard<std::mutex> lock(f.model->mutex);
        QVERIFY(!f.model->requests.empty());
        request = f.model->requests.back();
    }
    const auto& input = request.messages.back();
    QVERIFY(QString::fromStdString(input.content).contains(QStringLiteral("owned quoted context")));
    if (kind == QStringLiteral("image")) {
        QCOMPARE(input.images.size(), std::size_t(1));
        const QString staged = QString::fromStdString(input.images.front().path);
        QVERIFY(QFileInfo(staged).canonicalFilePath().startsWith(
            QFileInfo(f.workspace.path()).canonicalFilePath() + QLatin1Char('/')));
        QVERIFY(QFileInfo(staged).isFile());
        QVERIFY(staged != path);
    } else if (kind == QStringLiteral("video")) {
        QVERIFY(QString::fromStdString(input.content).contains(QStringLiteral("Attached video in the Agent workspace:")));
        bool found = false;
        for (const auto& message : f.controller.agent().listMessages(f.session.toStdString())) {
            if (message.role != MaiRole::User) continue;
            for (const auto& part : message.parts)
                if (const auto* video = std::get_if<MaiVideoPart>(&part.body)) {
                    found = true;
                    QVERIFY(QFileInfo(QString::fromStdString(video->path)).canonicalFilePath().startsWith(
                        QFileInfo(f.workspace.path()).canonicalFilePath() + QLatin1Char('/')));
                    QVERIFY(QFileInfo(QString::fromStdString(video->path)).isFile());
                }
        }
        QVERIFY(found);
    } else {
        QVERIFY(input.images.empty());
        QVERIFY(QString::fromStdString(input.content).contains(QStringLiteral(".maichat/session-attachments/")));
    }
    QCOMPARE(f.im->sent.first().origin, RemoteIMMessageOrigin::Machine);
}

void DesktopSessionImBridgeTest::explicitOwnedMediaUsesMachineDelivery_data() {
    QTest::addColumn<QString>("kind");
    QTest::newRow("image") << QStringLiteral("image");
    QTest::newRow("video") << QStringLiteral("video");
    QTest::newRow("audio-as-file") << QStringLiteral("audio");
}

void DesktopSessionImBridgeTest::explicitOwnedMediaUsesMachineDelivery() {
    QFETCH(QString, kind);
    Fixture f;
    QTRY_VERIFY(f.app.connectionReady());
    const QString name = kind == QStringLiteral("image") ? QStringLiteral("delivered.png") :
        kind == QStringLiteral("video") ? QStringLiteral("delivered.mp4") : QStringLiteral("delivered.wav");
    const QString path = f.workspace.filePath(name);
    if (kind == QStringLiteral("image")) {
        QImage image(24, 18, QImage::Format_RGB32); image.fill(Qt::green);
        QVERIFY(image.save(path, "PNG"));
    } else if (kind == QStringLiteral("video")) {
        QVERIFY(QFile::copy(QString::fromUtf8(MAICHAT_SESSION_IM_VIDEO_FIXTURE), path));
    } else {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray wave = QByteArray::fromHex("524946462c00000057415645666d74201000000001000100401f0000803e00000200100064617461080000000000000000000000");
        QCOMPARE(file.write(wave), qint64(wave.size()));
    }
    f.model->mediaPath = name;
    f.model->mediaType = kind;
    QVERIFY(f.bind(QStringLiteral("friend"), f.session));
    f.im->incoming(QStringLiteral("owned-delivery-source#0"), QStringLiteral("deliver owned media"));
    QTRY_COMPARE(f.im->sent.size(), 2);
    QCOMPARE(f.im->sent.last().kind, kind == QStringLiteral("audio") ? QStringLiteral("file") : kind);
    QCOMPARE(f.im->sent.last().origin, RemoteIMMessageOrigin::Machine);
    QCOMPARE(f.im->sent.last().text, QStringLiteral("owned media caption"));
    QCOMPARE(QFileInfo(f.im->sent.last().path).canonicalFilePath(), QFileInfo(path).canonicalFilePath());
    QVERIFY(f.im->sent.last().hasQuote);
    QCOMPARE(f.im->sent.last().quote.msgId, QStringLiteral("owned-delivery-source"));
}

void DesktopSessionImBridgeTest::toolObservationImageIsNeverAutomaticallyDelivered() {
    Fixture f;
    QTRY_VERIFY(f.app.connectionReady());
    const QString path = f.workspace.filePath(QStringLiteral("private-observation.png"));
    QImage image(24, 18, QImage::Format_RGB32); image.fill(Qt::red);
    QVERIFY(image.save(path, "PNG"));
    QVERIFY(f.controller.setModel(f.session, QStringLiteral("glm-5.3-flash")));
    f.model->observationPath = path;
    QVERIFY(f.bind(QStringLiteral("friend"), f.session));
    f.im->incoming(QStringLiteral("owned-observation-source"), QStringLiteral("inspect privately"));
    QTRY_COMPARE(f.model->calls.load(), 2);
    QTRY_COMPARE(f.im->sent.size(), 1);
    QCOMPARE(f.im->sent.first().kind, QStringLiteral("text"));
    QCOMPARE(f.im->sent.first().text, QStringLiteral("owned completed response"));
    bool hasObservation = false;
    for (const auto& message : f.controller.agent().listMessages(f.session.toStdString()))
        if (message.role == MaiRole::Assistant)
            for (const auto& part : message.parts)
                hasObservation |= std::holds_alternative<MaiImagePart>(part.body);
    QVERIFY(hasObservation);
}

void DesktopSessionImBridgeTest::suspendRejectsNewIngressBeforeDisconnectStarts() {
    Fixture f;
    QTRY_VERIFY(f.app.connectionReady());
    QVERIFY(f.bind(QStringLiteral("friend"), f.session));
    f.bridge.suspend();
    QVERIFY(f.app.connectionReady());
    f.im->incoming(QStringLiteral("during-logout-preparation"), QStringLiteral("must not execute"));
    QTest::qWait(40);
    QCOMPARE(f.model->calls.load(), 0);
    QVERIFY(f.im->sent.isEmpty());
}

void DesktopSessionImBridgeTest::unbindDuringAttachmentPreparationNeverSubmitsLateInput() {
    Fixture f;
    QTRY_VERIFY(f.app.connectionReady());
    QTemporaryDir cache;
    const QString path = cache.filePath(QStringLiteral("owned-pending-document.txt"));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(QByteArray(3 * 1024 * 1024, 'x')), qint64(3 * 1024 * 1024));
    }
    QVERIFY(f.bind(QStringLiteral("friend"), f.session));
    RemoteIMMessage message;
    message.id = QStringLiteral("owned-preparation-source");
    message.fromUserId = QStringLiteral("friend");
    message.toUserId = QStringLiteral("owner");
    message.origin = RemoteIMMessageOrigin::Human;
    message.hasFile = true;
    message.file.localPath = path;
    emit f.im->liveMessagesReceived({message});
    f.bridge.unbind(QStringLiteral("friend"));
    QTRY_VERIFY(f.bridge.findChildren<QObject*>(QStringLiteral("sessionImAttachmentPreparation")).isEmpty());
    QCOMPARE(f.model->calls.load(), 0);
    QVERIFY(f.im->sent.isEmpty());
}

void DesktopSessionImBridgeTest::workspaceChangeDuringPreparationRejectsTheStagedInput() {
    Fixture f;
    QTRY_VERIFY(f.app.connectionReady());
    QTemporaryDir cache;
    QTemporaryDir replacement;
    const QString path = cache.filePath(QStringLiteral("owned-staging.txt"));
    { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("owned file"), qint64(10)); }
    QVERIFY(f.bind(QStringLiteral("friend"), f.session));
    QSignalSpy errors(&f.bridge, &DesktopSessionImBridge::relayError);
    RemoteIMMessage message;
    message.id = QStringLiteral("owned-workspace-staging-race");
    message.fromUserId = QStringLiteral("friend");
    message.toUserId = QStringLiteral("owner");
    message.origin = RemoteIMMessageOrigin::Human;
    message.hasFile = true;
    message.file.localPath = path;
    emit f.im->liveMessagesReceived({message});
    QVERIFY(f.controller.setSessionDirectory(f.session, replacement.path()));
    QTRY_VERIFY(f.bridge.boundSession(QStringLiteral("friend")).isEmpty());
    QVERIFY(!errors.isEmpty());
    QCOMPARE(f.model->calls.load(), 0);
    QVERIFY(f.im->sent.isEmpty());
}

void DesktopSessionImBridgeTest::workspaceChangeBeforeIdleDoesNotSendAnotherDirectoriesMedia() {
    Fixture f;
    QTRY_VERIFY(f.app.connectionReady());
    QTemporaryDir replacement;
    const QString name = QStringLiteral("owned-same-name.png");
    QImage image(20, 16, QImage::Format_RGB32); image.fill(Qt::cyan);
    QVERIFY(image.save(f.workspace.filePath(name), "PNG"));
    QVERIFY(QFile::copy(f.workspace.filePath(name), replacement.filePath(name)));
    f.model->mediaPath = name;
    f.model->mediaType = QStringLiteral("image");
    QVERIFY(f.bind(QStringLiteral("friend"), f.session));
    QSignalSpy errors(&f.bridge, &DesktopSessionImBridge::relayError);
    f.im->incoming(QStringLiteral("owned-workspace-delivery-race"), QStringLiteral("deliver the original file"));
    // Finish only the owned Core worker; hold queued Qt idle delivery until the
    // ordinary local API changes this idle Session's workspace.
    f.controller.agent().waitIdle();
    QVERIFY(f.controller.setSessionDirectory(f.session, replacement.path()));
    QTRY_VERIFY(f.bridge.boundSession(QStringLiteral("friend")).isEmpty());
    QVERIFY(!errors.isEmpty());
    QVERIFY(errors.first().last().toString().contains(QStringLiteral("工作目录")));
    QVERIFY(f.im->sent.isEmpty());
}

void DesktopSessionImBridgeTest::busyLocalTurnIsNotSteeredAndPendingInputStaysOrdered() {
    Fixture f;
    QTRY_VERIFY(f.app.isConnected());
    f.model->blockFirst = true;
    QVERIFY(f.controller.sendPrompt(f.session, QStringLiteral("local owner task")));
    QTRY_COMPARE(f.model->calls.load(), 1);
    QVERIFY(f.bind(QStringLiteral("friend"), f.session));
    f.im->incoming(QStringLiteral("pending-one"), QStringLiteral("first remote task"));
    f.im->incoming(QStringLiteral("pending-two"), QStringLiteral("second remote task"));
    QTest::qWait(40);
    QCOMPARE(f.model->calls.load(), 1);
    int users = 0;
    for (const auto& message : f.controller.agent().listMessages(f.session.toStdString()))
        if (message.role == MaiRole::User) ++users;
    QCOMPARE(users, 1);
    f.model->release.release();
    QTRY_COMPARE(f.im->sent.size(), 2);
    QCOMPARE(f.model->calls.load(), 3);
    QCOMPARE(f.im->sent[0].text, QStringLiteral("reply: first remote task"));
    QCOMPARE(f.im->sent[1].text, QStringLiteral("reply: second remote task"));
    for (const auto& sent : f.im->sent) QVERIFY(!sent.text.contains(QStringLiteral("local owner task")));
}

void DesktopSessionImBridgeTest::priorQueuedIdleCannotRetireAnUnfinishedOwnedReply() {
    Fixture f;
    QTRY_VERIFY(f.app.connectionReady());
    f.model->blockFirst = true;
    QVERIFY(f.bind(QStringLiteral("friend"), f.session));
    f.im->incoming(QStringLiteral("owned-after-prior-turn"), QStringLiteral("new owned reply"));
    QTRY_COMPARE(f.model->calls.load(), 1);
    MaiEvent oldIdle;
    oldIdle.type = MaiEventType::SessionIdle;
    oldIdle.sessionId = f.session.toStdString();
    f.controller.agent().eventBus().publish(oldIdle);
    QCoreApplication::processEvents();
    QVERIFY(f.im->sent.isEmpty());
    f.model->release.release();
    QTRY_COMPARE(f.im->sent.size(), 1);
    QCOMPARE(f.im->sent.first().text, QStringLiteral("reply: new owned reply"));
}

void DesktopSessionImBridgeTest::delayedPreBindingUserReceiptDoesNotCountAsTakeover() {
    Fixture f;
    QTRY_VERIFY(f.app.connectionReady());
    f.model->blockFirst = true;
    QVERIFY(f.controller.sendPrompt(f.session, QStringLiteral("already accepted local input")));
    // Wait only on this owned worker; do not deliver the old Qt user event yet.
    QVERIFY(f.model->entered.tryAcquire(1, 1000));
    QVERIFY(f.bind(QStringLiteral("friend"), f.session));
    f.im->incoming(QStringLiteral("after-binding"), QStringLiteral("remote after local"));
    QCoreApplication::processEvents();
    QCOMPARE(f.bridge.boundSession(QStringLiteral("friend")), f.session);
    QCOMPARE(f.model->calls.load(), 1);
    f.model->release.release();
    QTRY_COMPARE(f.im->sent.size(), 1);
    QCOMPARE(f.im->sent.first().text, QStringLiteral("reply: remote after local"));
}

void DesktopSessionImBridgeTest::synchronousTakeoverBeforeSdkDispatchRevokesTheReply() {
    Fixture f;
    QTRY_VERIFY(f.app.connectionReady());
    QVERIFY(f.bind(QStringLiteral("friend"), f.session));
    QSignalSpy errors(&f.bridge, &DesktopSessionImBridge::relayError);
    bool tookOver = false;
    connect(&f.app, &RemoteIMApplication::stateChanged, &f.app, [&] {
        if (tookOver) return;
        const auto rows = f.app.chatState().messagesWith(QStringLiteral("friend"));
        // ChatState sorts by SDK time; the pending reply need not be the final
        // row when its timestamp is rounded to the same second as the input.
        const bool pendingReply = std::any_of(rows.begin(), rows.end(), [](const RemoteIMMessage& row) {
            return row.origin == RemoteIMMessageOrigin::Machine && row.status == RemoteIMMessageStatus::Pending &&
                row.text == QStringLiteral("reply: remote answer is revoked");
        });
        if (!pendingReply) return;
        tookOver = true;
        QVERIFY(f.controller.sendPrompt(f.session, QStringLiteral("local takeover before SDK dispatch")));
    });
    f.im->incoming(QStringLiteral("delivery-race"), QStringLiteral("remote answer is revoked"));
    QTRY_VERIFY(tookOver);
    QTRY_VERIFY(f.bridge.boundSession(QStringLiteral("friend")).isEmpty());
    QTRY_VERIFY(!f.controller.agent().isBusy(f.session.toStdString()));
    QVERIFY(f.im->sent.isEmpty());
    const auto rows = f.app.chatState().messagesWith(QStringLiteral("friend"));
    const auto revoked = std::find_if(rows.begin(), rows.end(), [](const RemoteIMMessage& row) {
        return row.origin == RemoteIMMessageOrigin::Machine && row.text == QStringLiteral("reply: remote answer is revoked");
    });
    QVERIFY(revoked != rows.end());
    QCOMPARE(revoked->status, RemoteIMMessageStatus::Failed);
    QVERIFY(!errors.isEmpty());
    QVERIFY(errors.first().last().toString().contains(QStringLiteral("发送失败")));
}

void DesktopSessionImBridgeTest::failedModelDoesNotForwardPartialOutputOrRetry() {
    Fixture f;
    QTRY_VERIFY(f.app.connectionReady());
    f.model->failFirst = true;
    QVERIFY(f.bind(QStringLiteral("friend"), f.session));
    QSignalSpy errors(&f.bridge, &DesktopSessionImBridge::relayError);
    f.im->incoming(QStringLiteral("owned-failed-source"), QStringLiteral("owned failed task"));
    QTRY_COMPARE(errors.count(), 1);
    QVERIFY(f.im->sent.isEmpty());
    QVERIFY(errors.first().last().toString().contains(QStringLiteral("创作中心")));
    QVERIFY(!errors.first().last().toString().contains(QStringLiteral("owned private failure detail")));
    f.im->incoming(QStringLiteral("owned-failed-source"), QStringLiteral("owned failed task"));
    QTest::qWait(40);
    QCOMPARE(f.model->calls.load(), 1);
}

void DesktopSessionImBridgeTest::emptyAssistantReportsReadableLocalErrorWithoutFakeDelivery() {
    Fixture f;
    QTRY_VERIFY(f.app.connectionReady());
    f.model->emptyReply = true;
    QVERIFY(f.bind(QStringLiteral("friend"), f.session));
    QSignalSpy errors(&f.bridge, &DesktopSessionImBridge::relayError);
    f.im->incoming(QStringLiteral("owned-empty-answer"), QStringLiteral("owned empty response task"));
    QTRY_COMPARE(errors.count(), 1);
    // Core owns its bounded final-answer attempt. An empty model stream is a
    // failed turn, so the bridge reports that failure without inventing output.
    QCOMPARE(errors.first().last().toString(), QStringLiteral("创作中心未完成这条消息，请在本机查看会话错误。"));
    QCOMPARE(f.model->calls.load(), 2);
    QVERIFY(f.im->sent.isEmpty());
}

void DesktopSessionImBridgeTest::unbindAndLocalTakeoverDiscardLateReplies() {
    Fixture f;
    QTRY_VERIFY(f.app.isConnected());
    f.model->blockFirst = true;
    QVERIFY(f.bind(QStringLiteral("friend"), f.session));
    f.im->incoming(QStringLiteral("late-source"), QStringLiteral("late task"));
    QTRY_COMPARE(f.model->calls.load(), 1);
    f.bridge.unbind(QStringLiteral("friend"));
    QVERIFY(f.controller.agent().isBusy(f.session.toStdString()));
    f.model->release.release();
    QTRY_VERIFY(!f.controller.agent().isBusy(f.session.toStdString()));
    QTest::qWait(40);
    QVERIFY(f.im->sent.isEmpty());

    QVERIFY(f.bind(QStringLiteral("friend"), f.session));
    QVERIFY(f.controller.sendPrompt(f.session, QStringLiteral("owner takes over")));
    QTRY_VERIFY(f.bridge.boundSession(QStringLiteral("friend")).isEmpty());
    QTRY_VERIFY(!f.controller.agent().isBusy(f.session.toStdString()));
    QVERIFY(f.im->sent.isEmpty());
}

void DesktopSessionImBridgeTest::unsupportedPayloadAndPendingLimitFailExplicitly() {
    Fixture f;
    QTRY_VERIFY(f.app.isConnected());
    f.model->blockFirst = true;
    QVERIFY(f.bind(QStringLiteral("friend"), f.session));
    f.im->incoming(QStringLiteral("busy-owned-source"), QStringLiteral("busy owned task"));
    QTRY_COMPARE(f.model->calls.load(), 1);
    QSignalSpy errors(&f.bridge, &DesktopSessionImBridge::relayError);
    RemoteIMMessage file;
    file.id = QStringLiteral("unsupported-owned-file");
    file.fromUserId = QStringLiteral("friend");
    file.toUserId = QStringLiteral("owner");
    file.origin = RemoteIMMessageOrigin::Human;
    file.hasFile = true;
    emit f.im->liveMessagesReceived({file});
    QCOMPARE(errors.count(), 1);
    f.im->incoming(QStringLiteral("owned-oversized"), QString(65537, QLatin1Char('x')));
    QCOMPARE(errors.count(), 2);
    for (int index = 0; index < 33; ++index)
        f.im->incoming(QStringLiteral("owned-pending-%1").arg(index), QStringLiteral("pending %1").arg(index));
    QCOMPARE(errors.count(), 3);
    QCOMPARE(f.model->calls.load(), 1);
    f.bridge.unbind(QStringLiteral("friend"));
    f.model->release.release();
    QTRY_VERIFY(!f.controller.agent().isBusy(f.session.toStdString()));
    QTest::qWait(40);
    QCOMPARE(f.model->calls.load(), 1);
    QVERIFY(f.im->sent.isEmpty());
}

void DesktopSessionImBridgeTest::attachmentPreparationIgnoresGlobalWorkAndExpiresWithoutBlockingLaterText() {
    Fixture f;
    QTRY_VERIFY(f.app.connectionReady());
    QTemporaryDir cache;
    const QString path = cache.filePath(QStringLiteral("owned-pool.txt"));
    { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("owned"), qint64(5)); }
    QVERIFY(f.bind(QStringLiteral("friend"), f.session));
    OwnedPoolBlocker global(QThreadPool::globalInstance());
    QVERIFY(global.entered());
    f.incomingFile(QStringLiteral("owned-global-pool-held"), path);
    QTRY_COMPARE(f.im->sent.size(), 1);
    QTRY_VERIFY(f.bridge.findChildren<QObject*>(QStringLiteral("sessionImAttachmentPreparation")).isEmpty());
    auto* pool = qobject_cast<QThreadPool*>(qApp->property("sessionImAttachmentPreparationPool").value<QObject*>());
    QVERIFY(pool && pool != QThreadPool::globalInstance());
    OwnedPoolBlocker dedicated(pool);
    QVERIFY(dedicated.entered());
    QSignalSpy errors(&f.bridge, &DesktopSessionImBridge::relayError);
    int heartbeats = 0;
    QTimer heartbeat;
    connect(&heartbeat, &QTimer::timeout, &heartbeat, [&] { ++heartbeats; });
    heartbeat.start(5);
    f.incomingFile(QStringLiteral("owned-preparation-expiry"), path);
    f.im->incoming(QStringLiteral("owned-text-after-expiry"), QStringLiteral("text after expired attachment"));
    auto* watcher = f.bridge.findChild<QObject*>(QStringLiteral("sessionImAttachmentPreparation"));
    QVERIFY(watcher);
    auto* deadline = watcher->findChild<QTimer*>(QStringLiteral("sessionImAttachmentDeadline"));
    QVERIFY(deadline && deadline->isActive() && deadline->interval() <= 5000);
    deadline->start(30);  // Exercise the real owned watchdog, without a product flag.
    QTRY_COMPARE(f.im->sent.size(), 2);
    QVERIFY(heartbeats > 0);
    QCOMPARE(f.model->calls.load(), 2);
    QVERIFY(!errors.isEmpty());
    QVERIFY(f.im->sent.last().text.contains(QStringLiteral("text after expired attachment")));
    dedicated.release();
    QTRY_VERIFY(f.bridge.findChildren<QObject*>(QStringLiteral("sessionImAttachmentPreparation")).isEmpty());
    QCOMPARE(f.model->calls.load(), 2);
}

void DesktopSessionImBridgeTest::asynchronousVideoPreparationKeepsHeartbeatAndReplyFifo() {
    Fixture f;
    QTRY_VERIFY(f.app.connectionReady());
    const QString path = f.workspace.filePath(QStringLiteral("owned-async-video.mp4"));
    QVERIFY(QFile::copy(QString::fromUtf8(MAICHAT_SESSION_IM_VIDEO_FIXTURE), path));
    QVERIFY(f.app.sendMachineMediaTo(QStringLiteral("friend"), path, QStringLiteral("video"), QStringLiteral("owned warm-up")));
    QTRY_COMPARE(f.im->sent.size(), 1);
    QTRY_VERIFY(f.app.findChildren<QObject*>(QStringLiteral("sessionImVideoPreparation")).isEmpty());
    auto* pool = qobject_cast<QThreadPool*>(qApp->property("sessionImVideoPreparationPool").value<QObject*>());
    QVERIFY(pool && pool != QThreadPool::globalInstance());
    OwnedPoolBlocker slow(pool);
    QVERIFY(slow.entered());
    f.model->mediaPath = QStringLiteral("owned-async-video.mp4");
    f.model->mediaType = QStringLiteral("video");
    QVERIFY(f.bind(QStringLiteral("friend"), f.session));
    int heartbeats = 0;
    QTimer heartbeat;
    connect(&heartbeat, &QTimer::timeout, &heartbeat, [&] { ++heartbeats; });
    heartbeat.start(5);
    f.im->incoming(QStringLiteral("owned-slow-video-request"), QStringLiteral("deliver video"));
    QTRY_COMPARE(f.im->sent.size(), 2); // warm-up plus the owned text; video prep is held.
    f.im->incoming(QStringLiteral("owned-after-slow-video"), QStringLiteral("next owned input"));
    QTest::qWait(40);
    QVERIFY(heartbeats > 0);
    QCOMPARE(f.model->calls.load(), 2);
    slow.release();
    QTRY_COMPARE(f.im->sent.size(), 4);
    QCOMPARE(f.im->sent[1].kind, QStringLiteral("text"));
    QCOMPARE(f.im->sent[2].kind, QStringLiteral("video"));
    QCOMPARE(f.im->sent[3].kind, QStringLiteral("text"));
    QCOMPARE(f.model->calls.load(), 3);
}

void DesktopSessionImBridgeTest::lateSessionErrorDuringVideoPreparationPreventsSdkMediaAndAdvancesFifo() {
    Fixture f;
    QTRY_VERIFY(f.app.connectionReady());
    const QString path = f.workspace.filePath(QStringLiteral("owned-late-error-video.mp4"));
    QVERIFY(QFile::copy(QString::fromUtf8(MAICHAT_SESSION_IM_VIDEO_FIXTURE), path));
    QVERIFY(f.app.sendMachineMediaTo(QStringLiteral("friend"), path, QStringLiteral("video"), QStringLiteral("owned warm-up")));
    QTRY_COMPARE(f.im->sent.size(), 1);
    QTRY_VERIFY(f.app.findChildren<QObject*>(QStringLiteral("sessionImVideoPreparation")).isEmpty());
    auto* pool = qobject_cast<QThreadPool*>(qApp->property("sessionImVideoPreparationPool").value<QObject*>());
    QVERIFY(pool);
    OwnedPoolBlocker slow(pool);
    QVERIFY(slow.entered());
    f.model->mediaPath = QStringLiteral("owned-late-error-video.mp4");
    f.model->mediaType = QStringLiteral("video");
    QVERIFY(f.bind(QStringLiteral("friend"), f.session));
    QSignalSpy errors(&f.bridge, &DesktopSessionImBridge::relayError);
    f.im->incoming(QStringLiteral("owned-late-error-request"), QStringLiteral("deliver video before late error"));
    QTRY_COMPARE(f.im->sent.size(), 2);  // Warm-up and exact completed text; media preparation is still held.
    QVERIFY(f.app.findChild<QObject*>(QStringLiteral("sessionImVideoPreparation")));
    f.im->incoming(QStringLiteral("owned-after-late-error"), QStringLiteral("next owned input"));
    MaiEvent failure;
    failure.type = MaiEventType::SessionError;
    failure.sessionId = f.session.toStdString();
    failure.detail = "owned late private failure detail";
    f.controller.agent().eventBus().publish(failure);
    slow.release();
    QTRY_VERIFY(f.app.findChildren<QObject*>(QStringLiteral("sessionImVideoPreparation")).isEmpty());
    QTRY_COMPARE(f.model->calls.load(), 3);
    QTRY_COMPARE(std::count_if(f.im->sent.cbegin(), f.im->sent.cend(), [](const OwnedIm::Sent& row) {
        return row.kind == QStringLiteral("text");
    }), 2);
    QCOMPARE(f.im->sent.size(), 3);
    QVERIFY(std::none_of(f.im->sent.cbegin() + 1, f.im->sent.cend(), [](const OwnedIm::Sent& row) {
        return row.kind == QStringLiteral("video");
    }));
    QCOMPARE(errors.size(), 1);
    QVERIFY(!errors.first().last().toString().contains(QStringLiteral("owned late private failure detail")));
    QCOMPARE(f.bridge.boundSession(QStringLiteral("friend")), f.session);
}

void DesktopSessionImBridgeTest::unbindDuringVideoPreparationPreventsLateSdkDispatch() {
    Fixture f;
    QTRY_VERIFY(f.app.connectionReady());
    const QString path = f.workspace.filePath(QStringLiteral("owned-revoked-video.mp4"));
    QVERIFY(QFile::copy(QString::fromUtf8(MAICHAT_SESSION_IM_VIDEO_FIXTURE), path));
    QVERIFY(f.app.sendMachineMediaTo(QStringLiteral("friend"), path, QStringLiteral("video"), QStringLiteral("owned warm-up")));
    QTRY_COMPARE(f.im->sent.size(), 1);
    QTRY_VERIFY(f.app.findChildren<QObject*>(QStringLiteral("sessionImVideoPreparation")).isEmpty());
    auto* pool = qobject_cast<QThreadPool*>(qApp->property("sessionImVideoPreparationPool").value<QObject*>());
    QVERIFY(pool);
    OwnedPoolBlocker slow(pool);
    QVERIFY(slow.entered());
    f.model->mediaPath = QStringLiteral("owned-revoked-video.mp4");
    f.model->mediaType = QStringLiteral("video");
    QVERIFY(f.bind(QStringLiteral("friend"), f.session));
    f.im->incoming(QStringLiteral("owned-revoked-video-request"), QStringLiteral("deliver video then revoke"));
    QTRY_COMPARE(f.im->sent.size(), 2);
    auto* watcher = f.app.findChild<QObject*>(QStringLiteral("sessionImVideoPreparation"));
    QVERIFY(watcher);
    auto* deadline = watcher->findChild<QTimer*>(QStringLiteral("sessionImVideoPreparationDeadline"));
    QVERIFY(deadline && deadline->isActive());
    f.bridge.unbind(QStringLiteral("friend"));
    deadline->start(20);
    QTest::qWait(40);
    slow.release();
    QTRY_VERIFY(f.app.findChildren<QObject*>(QStringLiteral("sessionImVideoPreparation")).isEmpty());
    QCOMPARE(f.im->sent.size(), 2);
    QCOMPARE(f.model->calls.load(), 2);
    QVERIFY(f.bridge.boundSession(QStringLiteral("friend")).isEmpty());
}

void DesktopSessionImBridgeTest::localTakeoverRevokesEverySdkScopedBindingForTheSession() {
    Fixture f;
    QTRY_VERIFY(f.app.connectionReady());
    QVERIFY(f.bind(QStringLiteral("friend"), f.session));
    f.app.disconnectFromService();
    f.app.connectToService(456, QStringLiteral("owned-second-sdk-signature"));
    QTRY_VERIFY(f.app.connectionReady());
    QVERIFY(f.bind(QStringLiteral("other"), f.session));
    f.bridge.localTakeover(f.session);
    QVERIFY(f.bridge.boundSession(QStringLiteral("other")).isEmpty());
    f.app.disconnectFromService();
    f.app.connectToService(123, QStringLiteral("owned-original-sdk-signature"));
    QTRY_VERIFY(f.app.connectionReady());
    QVERIFY(f.bridge.boundSession(QStringLiteral("friend")).isEmpty());
    QCOMPARE(f.model->calls.load(), 0);
    QVERIFY(f.im->sent.isEmpty());
}

void DesktopSessionImBridgeTest::metadataAdmissionKeepsMediaAheadOfLaterReadyText() {
    Fixture f;
    f.im->phasedLive = true;
    QVERIFY(f.bind(QStringLiteral("friend"), f.session));
    RemoteIMMessage media;
    media.id = QStringLiteral("owned-slow-media-first");
    media.fromUserId = QStringLiteral("friend");
    media.toUserId = QStringLiteral("owner");
    media.origin = RemoteIMMessageOrigin::Human;
    media.hasFile = true;
    media.text = QStringLiteral("first media caption");
    emit f.im->liveMessageAnnounced(1, media);
    RemoteIMMessage text = media;
    text.id = QStringLiteral("owned-ready-text-second");
    text.hasFile = false;
    text.text = QStringLiteral("second ready text");
    emit f.im->liveMessageAnnounced(2, text);
    emit f.im->liveMessageReady(2, text);
    QTest::qWait(60);
    QCOMPARE(f.model->calls.load(), 0);
    const QString path = f.workspace.filePath(QStringLiteral("owned-first.txt"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("owned first media body"), qint64(22));
    file.close();
    media.file = {path, QStringLiteral("owned-first.txt"), QStringLiteral("text/plain"), 22};
    emit f.im->liveMessageReady(1, media);
    QTRY_COMPARE_WITH_TIMEOUT(f.im->sent.size(), 2, 2500);
    QVERIFY(f.im->sent[0].text.contains(QStringLiteral("first media caption")));
    QVERIFY(f.im->sent[1].text.contains(QStringLiteral("second ready text")));
}

void DesktopSessionImBridgeTest::readinessCannotAdoptReboundSessionAndFailureReleasesHead() {
    Fixture f;
    f.im->phasedLive = true;
    QVERIFY(f.bind(QStringLiteral("friend"), f.session));
    RemoteIMMessage media;
    media.id = QStringLiteral("owned-original-media");
    media.fromUserId = QStringLiteral("friend");
    media.toUserId = QStringLiteral("owner");
    media.origin = RemoteIMMessageOrigin::Human;
    media.hasImage = true;
    emit f.im->liveMessageAnnounced(10, media);
    f.bridge.unbind(QStringLiteral("friend"));
    QVERIFY(f.bind(QStringLiteral("friend"), f.session));
    media.image.localPath = f.workspace.filePath(QStringLiteral("late.png"));
    emit f.im->liveMessageReady(10, media);
    media.id = QStringLiteral("owned-failed-media");
    media.image.localPath.clear();
    emit f.im->liveMessageAnnounced(11, media);
    RemoteIMMessage text = media;
    text.id = QStringLiteral("owned-after-failed-media");
    text.hasImage = false;
    text.text = QStringLiteral("after failed media");
    emit f.im->liveMessageAnnounced(12, text);
    emit f.im->liveMessageReady(12, text);
    QTest::qWait(40);
    QCOMPARE(f.model->calls.load(), 0);
    QSignalSpy errors(&f.bridge, &DesktopSessionImBridge::relayError);
    emit f.im->liveMessageFailed(11, QStringLiteral("owned download failed"));
    QTRY_COMPARE_WITH_TIMEOUT(f.im->sent.size(), 1, 2500);
    QVERIFY(f.im->sent.front().text.contains(QStringLiteral("after failed media")));
    QCOMPARE(f.model->calls.load(), 1);
    QCOMPARE(errors.size(), 1);
}

void DesktopSessionImBridgeTest::allPersistedHistoryImportsBeforeOneLaterLivePromptWithoutUsingChatWindow() {
    Fixture f(true);
    QVERIFY2(f.controller.openError().isEmpty(), qPrintable(f.controller.openError()));
    QVERIFY(f.history->isOpen());
    for (int index = 0; index < 451; ++index) {
        RemoteIMMessage row;
        row.id = QStringLiteral("history-%1").arg(index, 5, 10, QLatin1Char('0'));
        const bool outgoing = index % 2;
        row.fromUserId = outgoing ? QStringLiteral("owner") : QStringLiteral("friend");
        row.toUserId = outgoing ? QStringLiteral("friend") : QStringLiteral("owner");
        row.direction = outgoing ? RemoteIMMessageDirection::Outgoing : RemoteIMMessageDirection::Incoming;
        row.status = outgoing ? RemoteIMMessageStatus::Sent : RemoteIMMessageStatus::Received;
        row.createdAtMillis = index / 3;
        row.text = QStringLiteral("persisted-history-owned-%1").arg(index);
        QVERIFY(f.history->insertMessageIfAbsent(row, QStringLiteral("friend")));
    }
    QVERIFY(f.app.chatState().messagesWith(QStringLiteral("friend")).isEmpty());
    int heartbeats = 0;
    QTimer heartbeat;
    connect(&heartbeat, &QTimer::timeout, &heartbeat, [&] { ++heartbeats; });
    heartbeat.start(5);
    QSignalSpy importedStep(&f.bridge, &DesktopSessionImBridge::historyImported);
    QVERIFY(f.bridge.bind(QStringLiteral("friend"), f.session));
    QVERIFY(f.bridge.historyPending(QStringLiteral("friend")));
    QVERIFY(QTest::qWaitFor([&] { return !f.bridge.historyPending(QStringLiteral("friend")); }, 10000));
    QCOMPARE(f.bridge.boundSession(QStringLiteral("friend")), f.session);
    QCOMPARE(f.model->calls.load(), 0);
    QVERIFY(heartbeats > 0);
    QCOMPARE(importedStep.size(), 1);
    QCOMPARE(importedStep.first().at(1).toULongLong(), quint64(451));
    qInfo() << "owned SQLite history import: heartbeat ticks=" << heartbeats
            << "maximum Core worker step ms=" << importedStep.first().at(2).toLongLong();
    const auto imported = f.controller.agent().listMessages(f.session.toStdString());
    QCOMPARE(imported.size(), std::size_t(451));
    for (const auto& row : imported) {
        QCOMPARE(row.role, MaiRole::User);
        QVERIFY(row.completed && row.text().find("persisted-history-owned-") != std::string::npos);
    }
    f.im->incoming(QStringLiteral("after-history"), QStringLiteral("owned one later live prompt"));
    QTRY_COMPARE(f.im->sent.size(), 1);
    QCOMPARE(f.model->calls.load(), 1);
    std::lock_guard<std::mutex> lock(f.model->mutex);
    QCOMPARE(f.model->requests.size(), std::size_t(1));
    const auto& request = f.model->requests.front();
    QCOMPARE(std::count_if(request.messages.cbegin(), request.messages.cend(), [](const MaiModelMessage& row) {
        return row.content.find("persisted-history-owned-") != std::string::npos;
    }), 451);
}

void DesktopSessionImBridgeTest::unavailableHistorySourceDoesNotEnableAutomaticForwarding() {
    OwnedIm* im = new OwnedIm;
    RemoteIMApplication app(QStringLiteral("owner"), std::unique_ptr<RemoteIMClient>(im));
    auto state = std::make_shared<ModelState>();
    AgentController controller(std::make_unique<OwnedModel>(state), QString());
    DesktopSessionImBridge bridge(app, controller);
    QTemporaryDir workspace;
    app.addContact(QStringLiteral("friend"), QStringLiteral("Owned Friend"));
    app.connectToService(123, QStringLiteral("owned-unused-signature"));
    const QString session = controller.createSession(workspace.path(), QStringLiteral("Owned source failure"));
    QString error;
    QVERIFY(!bridge.bind(QStringLiteral("friend"), session, &error));
    QVERIFY(!error.isEmpty());
    im->incoming(QStringLiteral("no-history-store"), QStringLiteral("must not execute"));
    QTest::qWait(30);
    QCOMPARE(state->calls.load(), 0);
    QVERIFY(im->sent.isEmpty());
}

void DesktopSessionImBridgeTest::queuedPrebindRawMediaStaysContextAndPostbindFrameExecutesOnce() {
    Fixture f;
    RemoteIMMessage old;
    old.id = QStringLiteral("queued-before-bind");
    old.fromUserId = QStringLiteral("friend"); old.toUserId = QStringLiteral("owner");
    old.origin = RemoteIMMessageOrigin::Human;
    old.text = QStringLiteral("prebind raw media prior context"); old.hasImage = true;
    const auto ticket = f.im->queueRaw(old, false); // Actual raw receipt, queued Main parse is still pending.
    QVERIFY(f.bridge.bind(QStringLiteral("friend"), f.session));
    QVERIFY(f.bridge.historyPending(QStringLiteral("friend")));
    QVERIFY(QTest::qWaitFor([&] { return !f.bridge.historyPending(QStringLiteral("friend")); }, 10000));
    QCOMPARE(f.controller.agent().listMessages(f.session.toStdString()).size(), std::size_t(1));
    QCOMPARE(f.model->calls.load(), 0);
    const auto image = f.workspace.filePath(QStringLiteral("late-prior-image.png"));
    QVERIFY(QImage(8, 8, QImage::Format_RGB32).save(image));
    old.image = {image, 8, 8, QFileInfo(image).size()};
    emit f.im->liveMessageReady(ticket, old);
    emit f.im->liveMessagesReceived({old});
    QTest::qWait(30);
    QCOMPARE(f.model->calls.load(), 0);
    RemoteIMMessage live = old;
    live.id = QStringLiteral("postbind-raw"); live.hasImage = false; live.image = {};
    live.text = QStringLiteral("postbind raw executes once");
    f.im->queueRaw(live, true);
    QTRY_COMPARE(f.im->sent.size(), 1);
    QCOMPARE(f.model->calls.load(), 1);
}

void DesktopSessionImBridgeTest::liveOverlapIsExcludedFromSnapshotUsingConfirmedPersistence() {
    Fixture f;
    QVERIFY(f.bind(QStringLiteral("friend"), f.session));
    f.bridge.unbind(QStringLiteral("friend"));
    auto* pool = qobject_cast<QThreadPool*>(qApp->property("sessionImAttachmentPreparationPool").value<QObject*>());
    QVERIFY(pool);
    OwnedPoolBlocker held(pool);
    QVERIFY(held.entered());
    RemoteIMMessage prior;
    prior.id = QStringLiteral("persisted-before-overlap"); prior.fromUserId = QStringLiteral("friend");
    prior.toUserId = QStringLiteral("owner"); prior.text = QStringLiteral("unique old history context");
    QVERIFY(f.history->insertMessageIfAbsent(prior, QStringLiteral("friend")));
    QVERIFY(f.bridge.bind(QStringLiteral("friend"), f.session));
    f.im->incoming(QStringLiteral("actual-live-overlap"), QStringLiteral("unique overlapped new prompt"));
    QTest::qWait(30);
    QCOMPARE(f.model->calls.load(), 0);
    held.release();
    QTRY_COMPARE(f.im->sent.size(), 1);
    QCOMPARE(f.model->calls.load(), 1);
    std::lock_guard<std::mutex> lock(f.model->mutex);
    const auto& request = f.model->requests.front();
    QCOMPARE(std::count_if(request.messages.begin(), request.messages.end(), [](const MaiModelMessage& message) {
        return message.content.find("unique overlapped new prompt") != std::string::npos;
    }), 1);
    QCOMPARE(std::count_if(request.messages.begin(), request.messages.end(), [](const MaiModelMessage& message) {
        return message.content.find("unique old history context") != std::string::npos;
    }), 1);
}

void DesktopSessionImBridgeTest::overflowDuringHistoryAbortsBindingAndRetainsImRows() {
    Fixture f;
    f.im->rawSequence = 1; // Owned SDK raw frame has not reached its ordinary metadata writer yet.
    QSignalSpy errors(&f.bridge, &DesktopSessionImBridge::relayError);
    QVERIFY(f.bridge.bind(QStringLiteral("friend"), f.session));
    QVERIFY(f.bridge.historyPending(QStringLiteral("friend")));
    for (int index = 0; index < 33; ++index)
        f.im->incoming(QStringLiteral("during-history-%1").arg(index), QStringLiteral("owned pending live input"));
    QVERIFY(f.bridge.boundSession(QStringLiteral("friend")).isEmpty());
    QCOMPARE(f.model->calls.load(), 0);
    QCOMPARE(errors.size(), 1);
    QCOMPARE(f.history->loadRecentMessages(QStringLiteral("friend"), 100).size(), 33);
    QVERIFY(f.controller.agent().listMessages(f.session.toStdString()).empty());
}

void DesktopSessionImBridgeTest::partialHistoryFailureNeverPublishesStagedRows_data() {
    QTest::addColumn<bool>("lockDuringAbort");
    QTest::addColumn<bool>("abortBeforeDtor");
    QTest::newRow("normal-idle-gc") << false << false;
    QTest::newRow("transient-write-lock") << true << false;
    QTest::newRow("already-aborted-before-dtor") << false << true;
}

void DesktopSessionImBridgeTest::partialHistoryFailureNeverPublishesStagedRows() {
    QFETCH(bool, lockDuringAbort);
    QFETCH(bool, abortBeforeDtor);
    Fixture f(true);
    QVERIFY(f.controller.sendPrompt(f.session, QStringLiteral("owned ordinary conversation must survive GC")));
    QTRY_VERIFY(!f.controller.agent().isBusy(f.session.toStdString()));
    QCOMPARE(f.model->calls.load(), 1);
    const auto ordinaryCount = f.controller.agent().listMessages(f.session.toStdString()).size();
    for (int index = 0; index < 202; ++index) {
        RemoteIMMessage row;
        row.id = QStringLiteral("partial-%1").arg(index, 5, 10, QLatin1Char('0'));
        row.fromUserId = QStringLiteral("friend"); row.toUserId = QStringLiteral("owner");
        row.createdAtMillis = index;
        row.text = index == 201 ? QString(256 * 1024 + 1, QLatin1Char('x')) : QStringLiteral("owned staged prior context");
        QVERIFY(f.history->insertMessageIfAbsent(row, QStringLiteral("friend")));
    }
    QSignalSpy errors(&f.bridge, &DesktopSessionImBridge::relayError);
    const auto core = f.historyDirectory.filePath(QStringLiteral("owned-session-core.db"));
    const auto storageLock = std::make_shared<OwnedCoreWriteLock>();
    if (lockDuringAbort) connect(&f.bridge, &DesktopSessionImBridge::relayError, &f.controller,
        [storageLock, core] {
            QVERIFY(storageLock->acquire(core));
            QTimer::singleShot(6500, qApp, [storageLock] { storageLock->release(); });
        });
    if (abortBeforeDtor) connect(&f.bridge, &DesktopSessionImBridge::relayError, &f.controller,
        [core, worker = f.controller.agent().contextImportWorker()] {
            // The signal still owns the failed HistoryJob. Reproduce the
            // existing canceled begin/append worker's earlier successful abort
            // before that job's destructor schedules its own maintenance.
            const auto id = ownedCoreText(core, "SELECT id FROM context_imports WHERE state=0 LIMIT 1");
            QVERIFY(!id.isEmpty());
            auto aborted = QtConcurrent::run([worker, id] {
                const auto first = worker.abortContextImport(id.toStdString());
                const auto repeated = worker.abortContextImport(id.toStdString());
                return std::make_pair(first, repeated);
            });
            QTRY_VERIFY(aborted.isFinished());
            QVERIFY(!aborted.result().first);
            QCOMPARE(aborted.result().second.code(), MaiErrorCode::NotFound);
            QCOMPARE(ownedCoreCount(core, "SELECT COUNT(*) FROM context_imports WHERE state=2"), qint64(1));
            QCOMPARE(ownedCoreCount(core, "SELECT COUNT(*) FROM messages WHERE context_import_id IS NOT NULL"), qint64(200));
        });
    QVERIFY(f.bridge.bind(QStringLiteral("friend"), f.session));
    QVERIFY(QTest::qWaitFor([&] { return f.bridge.boundSession(QStringLiteral("friend")).isEmpty(); }, 10000));
    QCOMPARE(errors.size(), 1);
    QCOMPARE(f.model->calls.load(), 1);
    QCOMPARE(f.history->loadRecentMessages(QStringLiteral("friend"), 300).size(), 202);
    qInfo() << "owned failed snapshot hidden payload before idle GC="
            << ownedCoreCount(core, "SELECT COUNT(*) FROM messages WHERE context_import_id IS NOT NULL");
    QTRY_COMPARE_WITH_TIMEOUT(ownedCoreCount(core, "SELECT COUNT(*) FROM messages WHERE context_import_id IS NOT NULL"), qint64(0), lockDuringAbort ? 12000 : 3000);
    QTRY_COMPARE_WITH_TIMEOUT(ownedCoreCount(core, "SELECT COUNT(*) FROM context_imports WHERE state=2"), qint64(0), 3000);
    QCOMPARE(f.controller.agent().listMessages(f.session.toStdString()).size(), ordinaryCount);
}

void DesktopSessionImBridgeTest::metadataFailureBeforeFenceCaptureAbortsBootstrap() {
    Fixture f;
    f.im->rawSequence = 1;
    emit f.im->liveMetadataBatchFinished(1, false, f.im->rawGeneration);
    QSignalSpy errors(&f.bridge, &DesktopSessionImBridge::relayError);
    QVERIFY(f.bridge.bind(QStringLiteral("friend"), f.session));
    QVERIFY(QTest::qWaitFor([&] { return f.bridge.boundSession(QStringLiteral("friend")).isEmpty(); }, 3000));
    QCOMPARE(errors.size(), 1);
    QCOMPARE(f.model->calls.load(), 0);
    QVERIFY(f.controller.agent().listMessages(f.session.toStdString()).empty());
    QVERIFY(f.im->sent.isEmpty());
}

void DesktopSessionImBridgeTest::reopenedStartupProjectionReclaimsAbandonedContextPayload() {
    QTemporaryDir directory;
    const auto core = directory.filePath(QStringLiteral("owned-reopened-core.db"));
    QString session;
    {
        auto state = std::make_shared<ModelState>();
        AgentController controller(std::make_unique<OwnedModel>(state), core);
        session = controller.createSession(directory.path(), QStringLiteral("Owned existing root"));
        const auto worker = controller.agent().contextImportWorker();
        auto seeded = QtConcurrent::run([worker, session] {
            const auto begun = worker.beginContextImport({session.toStdString(), "owned-orphan-source", "owned-abandoned-snapshot", "owned prior context"});
            if (!begun) return false;
            std::vector<MaiContextImportRecord> records;
            for (int index = 0; index < 23; ++index) records.push_back({"orphan-" + std::to_string(index), "owned orphan payload", index});
            return bool(worker.appendContextImport(begun.value().importId, 0, records));
        });
        QTRY_VERIFY(seeded.isFinished());
        QVERIFY(seeded.result());
        QCOMPARE(ownedCoreCount(core, "SELECT COUNT(*) FROM messages WHERE context_import_id IS NOT NULL"), qint64(23));
    }
    OwnedIm* im = new OwnedIm;
    RemoteIMApplication app(QStringLiteral("owner"), std::unique_ptr<RemoteIMClient>(im),
        std::make_unique<LocalMessageDatabase>(directory.filePath(QStringLiteral("owned-im.db"))));
    app.connectToService(123, QStringLiteral("owned-unused-signature"));
    auto state = std::make_shared<ModelState>();
    AgentController controller(std::make_unique<OwnedModel>(state), core);
    DesktopSessionImBridge bridge(app, controller);
    AgentSessionList projection(controller);
    connect(&projection, &AgentSessionList::localRootIdsProjected, &bridge, &DesktopSessionImBridge::scheduleHistoryMaintenance);
    bridge.scheduleHistoryMaintenance(projection.projectedLocalRootIds());
    projection.refresh(); // Actual MainWindow first ordinary refresh lifecycle.
    QTRY_COMPARE_WITH_TIMEOUT(ownedCoreCount(core, "SELECT COUNT(*) FROM messages WHERE context_import_id IS NOT NULL"), qint64(0), 3000);
    QVERIFY(controller.agent().listMessages(session.toStdString()).empty());
    QCOMPARE(state->calls.load(), 0);
    QVERIFY(im->sent.isEmpty());
}

QTEST_MAIN(DesktopSessionImBridgeTest)
#include "DesktopSessionImBridgeTest.moc"
