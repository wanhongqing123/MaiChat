#include "agent/DesktopSessionImBridge.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>
#include <QDeadlineTimer>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QElapsedTimer>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QPointer>
#include <QScopeGuard>
#include <QSaveFile>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QThreadPool>
#include <QVariant>

#include <atomic>
#include <algorithm>
#include <mutex>
#include <thread>
#include <QtConcurrent/QtConcurrentRun>

#include "agent/AgentController.h"
#include "app/RemoteIMApplication.h"
#include "model/MessageQuote.h"
#include "MaiAgentSendMediaTool.h"
#include "MaiPathGuard.h"
#include "MaiSessionIMRouter.h"
#include "MaiIMHistorySnapshot.h"
#include "MaiIMHistoryFormat.h"

namespace {

constexpr int kPreparationAdmissionLimit = 32;
constexpr int kMaximumInputBytes = 64 * 1024;
constexpr int kPreparationBudgetMs = 5000;

std::atomic<int>& attachmentPreparationAdmissions() {
    static auto* count = new std::atomic<int>(0);
    return *count;
}

QThreadPool* attachmentPreparationPool() {
    // Process-lifetime bounded service: destroying a window never joins file
    // I/O. Each operation owns cancellation/deadline state and no UI pointers.
    static QThreadPool* pool = [] {
        auto* value = new QThreadPool;
        value->setMaxThreadCount(2);
        value->setObjectName(QStringLiteral("sessionImAttachmentPreparationPool"));
        QCoreApplication::instance()->setProperty("sessionImAttachmentPreparationPool", QVariant::fromValue<QObject*>(value));
        return value;
    }();
    return pool;
}

// The synchronous MaiSendPrompt broadcasts its exact user/assistant IDs before
// returning. Capture those receipts on the submit thread without querying the
// store in an event callback. Thereafter, an unexpected message update revokes
// this route immediately, before a queued Qt local-takeover event can arrive.
struct InputReceipt {
    std::mutex mutex;
    std::thread::id submitThread = std::this_thread::get_id();
    std::string session;
    std::vector<std::string> ids;
    bool capturing = true;
    std::atomic<bool> valid{true};
    std::atomic<bool> failed{false};
};

QString attachmentIdentity(const RemoteDiagnostics::AccountTag& account, const QString& peer, const QString& id) {
    QByteArray bytes;
    for (const auto& value : {QString::number(account.sdkAppId), account.ownerUserId, peer, id}) {
        bytes += QByteArray::number(value.toUtf8().size()) + ':' + value.toUtf8();
    }
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

struct PreparedInput {
    QString text;
    QString error;
    std::vector<MaiModelImage> images;
    std::vector<MaiVideoPart> videos;
};

// The SDK has already downloaded these files. Reuse the Session attachment
// contract with an ordinary cancellable local copy; never fetch a new URL or
// upload the IM cache through the cross-device cloud attachment service.
PreparedInput stageInput(const QString& workspace, const QString& identity, const RemoteIMMessage& source,
                         QString text, const std::shared_ptr<std::atomic<bool>>& canceled,
                         const QDeadlineTimer& deadline) {
    PreparedInput result;
    result.text = std::move(text);
    struct Source { QString path; QString kind; qint64 limit; };
    QList<Source> sources;
    if (source.hasImage) sources.append({source.image.localPath, QStringLiteral("image"), 32ll * 1024 * 1024});
    if (source.hasVideo) sources.append({source.video.localPath, QStringLiteral("video"), 1024ll * 1024 * 1024});
    if (source.hasFile) sources.append({source.file.localPath, QStringLiteral("file"), 512ll * 1024 * 1024});
    if (source.hasVoice) sources.append({source.voice.localPath, QStringLiteral("audio"), 64ll * 1024 * 1024});
    const QString relativeDirectory = QStringLiteral(".maichat/session-attachments");
    const QString directory = QString::fromStdString(
        maiResolvePathWithinRoot(workspace.toStdString(), relativeDirectory.toStdString()));
    if (directory.isEmpty() || !QDir().mkpath(directory)) {
        result.error = QStringLiteral("工作目录无法保存收到的会话附件。");
        return result;
    }
    QStringList documents;
    QMimeDatabase mimeDatabase;
    for (const Source& item : sources) {
        if (canceled->load() || deadline.hasExpired()) return result;
        const QFileInfo original(item.path);
        if (item.path.startsWith(QStringLiteral("//")) || item.path.startsWith(QStringLiteral("\\\\")) ||
            !original.isAbsolute() || !original.isFile() || !original.isReadable() ||
            original.size() <= 0 || original.size() > item.limit) {
            result.error = QStringLiteral("收到的附件尚未下载完成、不可读取或超过现有大小限制，没有提交给创作中心。");
            return result;
        }
        const auto mime = mimeDatabase.mimeTypeForFile(original, QMimeDatabase::MatchContent);
        QString suffix = original.suffix();
        if (suffix.isEmpty() || suffix.size() > 16) suffix = mime.preferredSuffix();
        if (suffix.isEmpty()) suffix = QStringLiteral("bin");
        const QString relative = relativeDirectory + QLatin1Char('/') + identity + QLatin1Char('-') +
            item.kind + QLatin1Char('.') + suffix;
        const QString target = QString::fromStdString(
            maiResolvePathWithinRoot(workspace.toStdString(), relative.toStdString()));
        QFile input(original.canonicalFilePath());
        QSaveFile output(target);
        if (target.isEmpty() || !input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
            result.error = QStringLiteral("无法将收到的附件保存到会话工作目录。");
            return result;
        }
        qint64 copied = 0;
        while (!input.atEnd()) {
            if (canceled->load() || deadline.hasExpired()) { output.cancelWriting(); return result; }
            const QByteArray chunk = input.read(1024 * 1024);
            if (chunk.isEmpty() || output.write(chunk) != chunk.size()) {
                result.error = QStringLiteral("会话附件复制失败，没有提交给创作中心。");
                return result;
            }
            copied += chunk.size();
            if (copied > item.limit) { result.error = QStringLiteral("会话附件超过现有大小限制。"); return result; }
        }
        if (copied != original.size() || canceled->load() || deadline.hasExpired() || !output.commit()) {
            result.error = QStringLiteral("会话附件未能完整保存，没有提交给创作中心。");
            return result;
        }
        const QString contentType = mime.name();
        if (item.kind == QStringLiteral("image") &&
            QStringList{QStringLiteral("image/jpeg"), QStringLiteral("image/png"),
                        QStringLiteral("image/webp"), QStringLiteral("image/gif")}.contains(contentType)) {
            result.images.push_back({target.toStdString(), contentType.toStdString()});
        } else if (item.kind == QStringLiteral("video") && contentType.startsWith(QStringLiteral("video/"))) {
            result.videos.push_back({target.toStdString(), contentType.toStdString()});
        } else {
            // Generic documents/audio and image formats without native model
            // support use the same path-reference input as SessionSyncHost.
            documents.append(relative);
        }
    }
    if (!documents.isEmpty())
        result.text += QStringLiteral("\n\n会话附件已保存到本机工作目录：\n- ") + documents.join(QStringLiteral("\n- "));
    return result;
}

struct OutputMedia { QString path; QString type; QString caption; };

QList<OutputMedia> deliveredMedia(const MaiMessage& reply, const MaiSession& owner, QString* error) {
    QList<OutputMedia> result;
    QSet<QString> paths;
    MaiToolContext context;
    context.root = owner.directory;
    context.sessionId = owner.id;
    for (const auto& part : reply.parts) {
        const auto* tool = std::get_if<MaiToolPart>(&part.body);
        if (!tool || tool->tool != "agent_send_media" || tool->state != MaiToolState::Completed) continue;
        const auto receipt = QJsonDocument::fromJson(QByteArray::fromStdString(tool->output)).object();
        const QString path = receipt.value(QStringLiteral("path")).toString();
        const QString type = receipt.value(QStringLiteral("type")).toString();
        const auto prepared = maiPrepareAgentMedia(path.toStdString(), type.toStdString(), context);
        if (receipt.value(QStringLiteral("delivery")).toString() != QStringLiteral("current_ai_session") ||
            !prepared || receipt.value(QStringLiteral("mime_type")).toString().toStdString() != prepared.value().mimeType ||
            receipt.value(QStringLiteral("bytes")).toVariant().toULongLong() != prepared.value().bytes) {
            *error = QStringLiteral("一项会话媒体结果已不可读取或与交付凭据不符，没有自动发送。请在创作中心查看。");
            continue;
        }
        const QString absolute = QString::fromStdString(maiResolvePathWithinRoot(owner.directory, prepared.value().relativePath));
        if (!paths.contains(absolute)) {
            paths.insert(absolute);
            result.append({absolute, type, receipt.value(QStringLiteral("caption")).toString()});
        }
    }
    // MaiImagePart can be a private tool observation (for example a screenshot),
    // and arbitrary tool JSON/Markdown paths are not delivery authority.
    return result;
}

}  // namespace

using Router = MaiSessionIMRouter;

namespace {
std::atomic<int>& historyReaderAdmissions() { static auto* count = new std::atomic<int>(0); return *count; }

struct HistoryMaintenance {
    MaiContextImportWorker worker;
    QStringList roots;
    qsizetype offset = 0;
    std::string abortId;
    unsigned retries = 0;
    QPointer<QObject> retryContext;
};

void continueHistoryMaintenance(const std::shared_ptr<HistoryMaintenance>& maintenance);

void retryHistoryMaintenance(const std::shared_ptr<HistoryMaintenance>& maintenance,
                             const MaiError& error, const char* operation) {
    const unsigned attempt = std::min(maintenance->retries++, 6u);
    const int delay = std::min(100 * (1 << attempt), 5000);
    qWarning() << "[session-im] context maintenance deferred" << operation
               << maiErrorCodeToString(error.code()) << "retry_ms=" << delay;
    // Only metadata waits in the ordinary Qt loop. No preparation thread
    // sleeps, and the weak Core capability stops when its owner is revoked.
    if (maintenance->retryContext)
        QTimer::singleShot(delay, maintenance->retryContext, [maintenance] { continueHistoryMaintenance(maintenance); });
}

void continueHistoryMaintenance(const std::shared_ptr<HistoryMaintenance>& maintenance) {
    if (maintenance->offset >= maintenance->roots.size()) return;
    attachmentPreparationPool()->start([maintenance] {
        if (!maintenance->abortId.empty()) {
            const auto error = maintenance->worker.abortContextImport(maintenance->abortId);
            if (error && error.code() != MaiErrorCode::NotFound) {
                if (error.code() == MaiErrorCode::Canceled) return;
                retryHistoryMaintenance(maintenance, error, "abort");
                return;
            }
            maintenance->abortId.clear();
        }
        // An already-aborted stage is NotFound to abort, but its hidden payload
        // still needs the SAME session's bounded purge on this worker.
        const auto result = maintenance->worker.purgeContextImports(
            maintenance->roots.at(maintenance->offset).toStdString(), 32);
        if (!result) {
            if (result.error().code() == MaiErrorCode::Canceled) return;
            if (result.error().code() == MaiErrorCode::NotFound) ++maintenance->offset;
            else { retryHistoryMaintenance(maintenance, result.error(), "purge"); return; }
        } else {
            maintenance->retries = 0;
            if (!result.value().hasMore) ++maintenance->offset;
        }
        continueHistoryMaintenance(maintenance);
    });
}

void purgeHistoryLater(MaiContextImportWorker worker, std::string session, QPointer<QObject> context) {
    auto maintenance = std::make_shared<HistoryMaintenance>();
    maintenance->worker = std::move(worker);
    maintenance->roots.append(QString::fromStdString(session));
    maintenance->retryContext = context;
    continueHistoryMaintenance(maintenance);
}

struct HistoryJob {
    Router::Binding binding;
    RemoteIMApplication::IMHistorySource source;
    RemoteIMClient::LiveMetadataBoundary boundary;
    MaiContextImportWorker worker;
    std::shared_ptr<std::atomic<bool>> canceled = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<MaiIMHistorySnapshot> reader;
    std::optional<MaiIMHistoryPage> page;
    MaiIMHistorySource descriptor;
    std::vector<MaiContextImportRecord> formatted;
    std::size_t offset = 0, formattedOffset = 0;
    std::uint64_t sequence = 0, imported = 0;
    QString snapshotId;
    QPointer<QTimer> deadline;
    bool fenceReady = false, inFlight = false, eof = false;
    std::atomic<bool> committed{false};
    std::mutex stageMutex;
    std::string importId;
    qint64 maximumStepMillis = 0;
    QPointer<QObject> maintenanceContext;

    std::string stageId() { std::lock_guard<std::mutex> lock(stageMutex); return importId; }
    ~HistoryJob() {
        canceled->store(true);
        const auto id = stageId();
        if (!committed.load() && !id.empty()) {
            auto maintenance = std::make_shared<HistoryMaintenance>();
            maintenance->worker = worker;
            maintenance->roots.append(QString::fromStdString(binding.sessionId));
            maintenance->abortId = id;
            maintenance->retryContext = maintenanceContext;
            continueHistoryMaintenance(maintenance);
        }
        // Reader uses an I/O-thread deleter; no SQLite close or join occurs here.
    }
};

struct HistoryOpenResult { std::shared_ptr<MaiIMHistorySnapshot> reader; MaiError error; bool capacity = false; };
struct HistoryCoreResult { MaiResult<MaiContextImportProgress> value; qint64 milliseconds; };

template<class Result, class Work, class Done>
void historyWork(QObject* owner, const QString& name, Work work, Done done) {
    auto* watcher = new QFutureWatcher<Result>(owner);
    watcher->setObjectName(name);
    QObject::connect(watcher, &QFutureWatcher<Result>::finished, owner, [watcher, done = std::move(done)]() mutable {
        auto result = watcher->result();
        watcher->deleteLater();
        done(std::move(result));
    });
    watcher->setFuture(QtConcurrent::run(attachmentPreparationPool(), std::move(work)));
}
}

struct DesktopSessionImBridge::Runtime {
    struct Payload {
        Router::QueuedInput input;
        quint64 ingressId = 0;
        bool ready = false;
        QPointer<QTimer> readinessDeadline;
        QString text;
        QString stagingIdentity;
        RemoteIMMessage source;
        bool prepared = true;
        std::vector<MaiModelImage> images;
        std::vector<MaiVideoPart> videos;
    };
    struct CoreLease {
        std::shared_ptr<InputReceipt> observed;
        MaiEventBus::Token subscription = 0;
    };
    RemoteIMApplication& app;
    AgentController& controller;
    Router router;
    std::optional<Router::AccountScope> scope;
    const std::uint64_t controllerGeneration;
    // These ledgers own only platform values/jobs. Queue order, epochs, dedupe,
    // phases and dispatch accounting exist exclusively in MaiSessionIMRouter.
    QHash<quint64, Payload> payloads;
    QHash<quint64, CoreLease> coreLeases;
    QHash<quint64, std::shared_ptr<std::atomic<bool>>> preparing;
    QHash<QString, QSet<QString>> priorCoreUserReceipts;
    QHash<QString, std::shared_ptr<HistoryJob>> historyJobs;

    static std::uint64_t nextControllerGeneration() {
        static std::atomic<std::uint64_t> next{1};
        return next.fetch_add(1);
    }
    Runtime(RemoteIMApplication& application, AgentController& agent)
        : app(application), controller(agent), controllerGeneration(nextControllerGeneration()) {}

    std::optional<Router::AccountScope> liveScope() const {
        const auto account = app.client().currentAccount();
        if (!scope || !app.connectionReady() || !account.isValid() ||
            account.ownerUserId != app.chatState().ownerUserId() ||
            account.sdkAppId != scope->sdkAppId || account.ownerUserId.toStdString() != scope->ownerUserId ||
            app.connectionGeneration() != scope->authGeneration) return std::nullopt;
        return scope;
    }
    QString workspace(const std::string& session) const {
        MaiSession owner;
        return controller.agent().getSession(session, owner) && owner.isRoot()
            ? QFileInfo(QString::fromStdString(owner.directory)).canonicalFilePath() : QString();
    }
    bool friendCurrent(const std::string& peer) const {
        const QString id = QString::fromStdString(peer);
        if (!app.acceptedFriendsReady() || !app.isAcceptedFriend(id)) return false;
        for (const auto& contact : app.chatState().contacts()) if (contact.userId == id) return true;
        return false;
    }
    std::optional<Router::ActiveRoute> active(quint64 token) const {
        if (!scope) return std::nullopt;
        for (const auto& route : router.snapshot(*scope).active)
            if (route.reservation.token == token) return route;
        return std::nullopt;
    }
    bool current(const Router::Reservation& reservation) const {
        const auto live = liveScope();
        return live && friendCurrent(reservation.input.binding.peerId) &&
            router.isCurrent(reservation.token, *live, workspace(reservation.input.binding.sessionId).toStdString());
    }
    bool bindingCurrent(const Router::QueuedInput& input) const {
        const auto live = liveScope();
        if (!live || *live != input.binding.account || !friendCurrent(input.binding.peerId)) return false;
        const auto binding = router.bindingForPeer(*live, input.binding.peerId);
        return binding && binding->epoch == input.binding.epoch && binding->sessionId == input.binding.sessionId &&
            binding->workspaceKey == input.binding.workspaceKey && workspace(binding->sessionId).toStdString() == binding->workspaceKey;
    }
    bool outputCurrent(const Router::ActiveRoute& route) const {
        const auto live = liveScope();
        const auto lease = coreLeases.constFind(route.reservation.token);
        return live && route.receipt && lease != coreLeases.constEnd() && lease->observed->valid.load() &&
            !lease->observed->failed.load() &&
            friendCurrent(route.reservation.input.binding.peerId) &&
            router.isOutputCurrent(route.reservation.token, *route.receipt, *live,
                workspace(route.reservation.input.binding.sessionId).toStdString());
    }
    bool sameHistory(const std::shared_ptr<HistoryJob>& job) const {
        return historyJobs.value(QString::fromStdString(job->binding.peerId)) == job;
    }
    bool currentHistory(const std::shared_ptr<HistoryJob>& job) const {
        const auto live = liveScope();
        if (!live || *live != job->binding.account || job->canceled->load() || !friendCurrent(job->binding.peerId)) return false;
        const auto binding = router.bindingForPeer(*live, job->binding.peerId);
        if (!binding || binding->epoch != job->binding.epoch || binding->sessionId != job->binding.sessionId ||
            workspace(binding->sessionId).toStdString() != binding->workspaceKey) return false;
        const auto source = app.imHistorySource(QString::fromStdString(binding->peerId));
        return source && source->account == job->source.account && source->databasePath == job->source.databasePath &&
            source->connectionGeneration == job->source.connectionGeneration && source->rawGeneration == job->source.rawGeneration;
    }
    void release(CoreLease& lease) {
        lease.observed->valid = false;
        controller.agent().eventBus().unsubscribe(lease.subscription);
    }
};

DesktopSessionImBridge::DesktopSessionImBridge(RemoteIMApplication& app, AgentController& controller, QObject* parent)
    : QObject(parent), runtime_(std::make_unique<Runtime>(app, controller)) {
    connect(&app, &RemoteIMApplication::sessionImMessageAnnounced, this, &DesktopSessionImBridge::acceptLive);
    connect(&app, &RemoteIMApplication::sessionImMessageReady, this, &DesktopSessionImBridge::readyLive);
    connect(&app, &RemoteIMApplication::sessionImMessageFailed, this, &DesktopSessionImBridge::failLive);
    connect(&app, &RemoteIMApplication::sessionImMessagePersisted, this, &DesktopSessionImBridge::persistedLive);
    connect(&app, &RemoteIMApplication::connectionChanged, this, [this](bool ready) {
        if (ready) syncScope(); else suspend();
    });
    connect(&app, &RemoteIMApplication::stateChanged, this, &DesktopSessionImBridge::pruneSessions);
    connect(&controller, &AgentController::turnFinished, this, [this](const QString& session) {
        const QPointer<DesktopSessionImBridge> lifetime(this);
        finish(session);
        if (!lifetime) return;
        const auto jobs = runtime_->historyJobs;
        for (auto it = jobs.cbegin(); it != jobs.cend(); ++it)
            if (it.value()->binding.sessionId == session.toStdString()) advanceHistory(it.key());
    });
    connect(&controller, &AgentController::userMessageAdded, this,
        [this](const QString& session, const QString& user) {
            if (!runtime_->scope) return;
            for (const auto& route : runtime_->router.snapshot(*runtime_->scope).active)
                if (route.reservation.input.binding.sessionId == session.toStdString() && route.receipt &&
                    route.receipt->userMessageId == user.toStdString()) return;
            if (runtime_->priorCoreUserReceipts.value(session).contains(user)) return;
            localTakeover(session);
        });
    connect(&controller, &AgentController::sessionStructureChanged, this, &DesktopSessionImBridge::pruneSessions);
    syncScope();
}

DesktopSessionImBridge::~DesktopSessionImBridge() { suspend(); }

void DesktopSessionImBridge::syncScope() {
    const auto account = runtime_->app.client().currentAccount();
    if (!runtime_->app.connectionReady() || !account.isValid() ||
        account.ownerUserId != runtime_->app.chatState().ownerUserId()) { suspend(); return; }
    const Router::AccountScope next{account.sdkAppId, account.ownerUserId.toStdString(),
        runtime_->app.connectionGeneration(), runtime_->controllerGeneration};
    const bool changed = !runtime_->scope || *runtime_->scope != next;
    if (runtime_->router.activate(next) != Router::Status::Ok) return;
    runtime_->scope = next;
    if (changed) runtime_->priorCoreUserReceipts.clear();
    prunePlatformWork();
}

void DesktopSessionImBridge::prunePlatformWork() {
    QSet<quint64> inputs;
    QSet<quint64> tokens;
    QSet<QString> sessions;
    if (runtime_->scope) {
        const auto snapshot = runtime_->router.snapshot(*runtime_->scope);
        for (const auto& binding : snapshot.bindings) sessions.insert(QString::fromStdString(binding.sessionId));
        for (const auto& pending : snapshot.pending) inputs.insert(pending.inputId);
        for (const auto& route : snapshot.active) {
            tokens.insert(route.reservation.token);
            inputs.insert(route.reservation.input.inputId);
        }
    }
    for (auto it = runtime_->historyJobs.begin(); it != runtime_->historyJobs.end();) {
        const auto job = it.value();
        const auto binding = runtime_->scope ? runtime_->router.bindingForPeer(*runtime_->scope, job->binding.peerId) : std::nullopt;
        if (binding && binding->account == job->binding.account && binding->epoch == job->binding.epoch) { ++it; continue; }
        job->canceled->store(true);
        if (job->deadline) { job->deadline->stop(); job->deadline->deleteLater(); }
        it = runtime_->historyJobs.erase(it);
    }
    for (auto it = runtime_->preparing.begin(); it != runtime_->preparing.end();) {
        if (inputs.contains(it.key())) { ++it; continue; }
        it.value()->store(true);
        it = runtime_->preparing.erase(it);
    }
    for (auto it = runtime_->coreLeases.begin(); it != runtime_->coreLeases.end();) {
        if (tokens.contains(it.key())) { ++it; continue; }
        runtime_->release(it.value());
        it = runtime_->coreLeases.erase(it);
    }
    for (auto it = runtime_->payloads.begin(); it != runtime_->payloads.end();) {
        if (inputs.contains(it.key())) { ++it; continue; }
        if (it->readinessDeadline) it->readinessDeadline->deleteLater();
        it = runtime_->payloads.erase(it);
    }
    for (auto it = runtime_->priorCoreUserReceipts.begin(); it != runtime_->priorCoreUserReceipts.end();)
        if (!sessions.contains(it.key())) it = runtime_->priorCoreUserReceipts.erase(it); else ++it;
}

bool DesktopSessionImBridge::bind(const QString& peerId, const QString& sessionId, QString* error) {
    const auto reject = [error](const QString& text) { if (error) *error = text; return false; };
    const auto scope = runtime_->liveScope();
    const QString peer = peerId.trimmed();
    if (!scope || !runtime_->friendCurrent(peer.toStdString()))
        return reject(QStringLiteral("\u8bf7\u9009\u62e9\u5f53\u524d\u8d26\u53f7\u7684\u597d\u53cb\u4f1a\u8bdd\u3002"));
    const QString workspace = runtime_->workspace(sessionId.toStdString());
    if (workspace.isEmpty()) return reject(QStringLiteral("\u8bf7\u9009\u62e9\u5df2\u6709\u7684\u672c\u673a\u521b\u4f5c\u4e2d\u5fc3\u4f1a\u8bdd\u3002"));
    const auto before = runtime_->router.bindingForPeer(*scope, peer.toStdString());
    const auto source = runtime_->app.imHistorySource(peer, error);
    if (!source) return false;
    if (before && before->sessionId == sessionId.toStdString() && before->workspaceKey == workspace.toStdString()) {
        if (error) error->clear();
        return true;
    }
    auto job = std::make_shared<HistoryJob>();
    job->source = *source;
    job->maintenanceContext = QCoreApplication::instance();
    job->worker = runtime_->controller.agent().contextImportWorker();
    job->snapshotId = QStringLiteral("desktop-history-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    Router::Status result = Router::Status::StaleScope;
    // Same transport lock as raw receipt+post. Only plain Router values are
    // published inside; no SQL, Qt signal, parsing or file work holds this lock.
    runtime_->app.client().withLiveMetadataBoundary([&](auto boundary) {
        if (boundary.generation != source->rawGeneration) return;
        result = runtime_->router.bind(*scope, peer.toStdString(), sessionId.toStdString(), workspace.toStdString(), true, true);
        if (result != Router::Status::Ok) return;
        const auto binding = runtime_->router.bindingForPeer(*scope, peer.toStdString());
        result = runtime_->router.beginHistory(*scope, peer.toStdString(), binding->epoch);
        if (result == Router::Status::Ok) {
            job->binding = *runtime_->router.bindingForPeer(*scope, peer.toStdString());
            job->boundary = boundary;
        }
    });
    if (result != Router::Status::Ok) return reject(result == Router::Status::Conflict
        ? QStringLiteral("\u8fd9\u4e2a\u521b\u4f5c\u4e2d\u5fc3\u4f1a\u8bdd\u5df2\u7ed1\u5b9a\u5176\u4ed6\u597d\u53cb\uff0c\u8bf7\u5148\u89e3\u9664\u539f\u7ed1\u5b9a\u3002")
        : QStringLiteral("\u5f53\u524d\u8d26\u53f7\u6216\u4f1a\u8bdd\u7ed1\u5b9a\u5df2\u4e0d\u53ef\u7528\u3002"));
    if (error) error->clear();
    if (const auto previous = runtime_->historyJobs.value(peer)) {
        previous->canceled->store(true);
        if (previous->deadline) { previous->deadline->stop(); previous->deadline->deleteLater(); }
    }
    runtime_->historyJobs.insert(peer, job);
    auto* deadline = new QTimer(this);
    job->deadline = deadline;
    deadline->setObjectName(QStringLiteral("sessionImHistoryDeadline"));
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, this, [this, weak = std::weak_ptr<HistoryJob>(job), peer] {
        const auto current = weak.lock();
        if (current && runtime_->sameHistory(current)) abortHistory(peer, QStringLiteral("准备完整聊天历史超时，绑定未完成。"));
    });
    deadline->start(300000);
    QSet<QString> prior;
    for (const auto& message : runtime_->controller.agent().listMessagesPage(sessionId.toStdString(), "", Router::kRememberedInputLimit))
        if (message.role == MaiRole::User) prior.insert(QString::fromStdString(message.id));
    runtime_->priorCoreUserReceipts.insert(sessionId, prior);
    prunePlatformWork();
    const QPointer<DesktopSessionImBridge> lifetime(this);
    emit bindingChanged(peer);
    if (!lifetime || !runtime_->sameHistory(job)) return true;
    runtime_->app.awaitIMHistoryMetadataFence(job->source, job->canceled,
        [lifetime, job, peer](bool ok, const QString& failure) {
            if (!lifetime || !lifetime->runtime_->sameHistory(job)) return;
            if (!ok) { lifetime->abortHistory(peer, failure); return; }
            job->fenceReady = true;
            lifetime->advanceHistory(peer);
        });
    return true;
}

void DesktopSessionImBridge::abortHistory(const QString& peer, const QString& failure) {
    const auto job = runtime_->historyJobs.value(peer);
    if (!job) return;
    job->canceled->store(true);
    const auto scope = runtime_->liveScope();
    const auto binding = scope ? runtime_->router.bindingForPeer(*scope, peer.toStdString()) : std::nullopt;
    if (binding && binding->account == job->binding.account && binding->epoch == job->binding.epoch)
        runtime_->router.unbind(*scope, peer.toStdString());
    if (job->deadline) { job->deadline->stop(); job->deadline->deleteLater(); }
    runtime_->historyJobs.remove(peer);
    prunePlatformWork();
    const QPointer<DesktopSessionImBridge> lifetime(this);
    emit bindingChanged(peer);
    if (lifetime && !failure.isEmpty()) emit relayError(peer, failure);
}

void DesktopSessionImBridge::advanceHistory(const QString& peer) {
    const auto job = runtime_->historyJobs.value(peer);
    if (!job || job->inFlight || !job->fenceReady) return;
    if (!runtime_->currentHistory(job)) { abortHistory(peer, QStringLiteral("账号、会话或工作目录已改变，历史导入没有完成。")); return; }
    const QPointer<DesktopSessionImBridge> lifetime(this);
    const auto current = [lifetime, job] {
        return lifetime && lifetime->runtime_->sameHistory(job);
    };
    const auto again = [lifetime, peer] { if (lifetime) lifetime->advanceHistory(peer); };
    if (!job->reader && !job->eof) {
        job->inFlight = true;
        MaiIMHistorySnapshotOptions options;
        options.databasePath = job->source.databasePath.toStdString();
        options.profile = MaiIMHistoryProfile::Desktop;
        options.ownerUserId = job->binding.account.ownerUserId;
        options.peerUserId = job->binding.peerId;
        options.canceled = job->canceled;
        auto* mainThread = QThread::currentThread();
        historyWork<HistoryOpenResult>(this, QStringLiteral("sessionImHistoryOpen"), [job, options, mainThread] {
            HistoryOpenResult result;
            if (job->canceled->load()) { result.error = {MaiErrorCode::Canceled, "History import canceled"}; return result; }
            int count = historyReaderAdmissions().load();
            while (count < 2 && !historyReaderAdmissions().compare_exchange_weak(count, count + 1)) {}
            if (count >= 2) { result.capacity = true; return result; }
            auto opened = MaiIMHistorySnapshot::open(options);
            if (!opened) { historyReaderAdmissions().fetch_sub(1); result.error = opened.error(); return result; }
            result.reader = std::shared_ptr<MaiIMHistorySnapshot>(opened.value().release(), [mainThread](MaiIMHistorySnapshot* reader) {
                const auto close = [reader] { delete reader; historyReaderAdmissions().fetch_sub(1); };
                if (QThread::currentThread() == mainThread) attachmentPreparationPool()->start(close);
                else close();
            });
            return result;
        }, [this, job, peer, current, again](HistoryOpenResult opened) {
            if (!current()) return;
            job->inFlight = false;
            if (opened.capacity) {
                QTimer::singleShot(100, this, [this, job, peer] { if (runtime_->sameHistory(job)) advanceHistory(peer); });
                return;
            }
            if (opened.error || !opened.reader) { abortHistory(peer, QStringLiteral("无法固定当前账号的完整本地聊天快照，绑定没有完成。")); return; }
            job->reader = std::move(opened.reader);
            job->descriptor = job->reader->source();
            again();
        });
        return;
    }
    const auto importId = job->stageId();
    if (importId.empty()) {
        const auto identity = QJsonDocument(QJsonArray{QStringLiteral("desktop"), QString::number(job->binding.account.sdkAppId),
            QString::fromStdString(job->binding.account.ownerUserId), peer}).toJson(QJsonDocument::Compact).toStdString();
        const auto sourceKey = "im:" + maiContextImportBatchDigest({{"logical-source", identity, 0}});
        const MaiContextImportSpec spec{job->binding.sessionId, sourceKey, job->snapshotId.toStdString(), "Quoted local IM history"};
        job->inFlight = true;
        historyWork<HistoryCoreResult>(this, QStringLiteral("sessionImHistoryBegin"), [job, spec] {
            QElapsedTimer timer; timer.start();
            if (job->canceled->load()) return HistoryCoreResult{{MaiErrorCode::Canceled, "History import canceled"}, timer.elapsed()};
            auto result = job->worker.beginContextImport(spec);
            if (result) { std::lock_guard<std::mutex> lock(job->stageMutex); job->importId = result.value().importId; }
            if (result && job->canceled->load()) job->worker.abortContextImport(result.value().importId);
            return HistoryCoreResult{std::move(result), timer.elapsed()};
        }, [this, job, peer, current, again](HistoryCoreResult result) {
            if (!current()) return;
            job->inFlight = false;
            job->maximumStepMillis = std::max(job->maximumStepMillis, result.milliseconds);
            if (!result.value) { abortHistory(peer, QStringLiteral("无法暂存完整聊天历史，绑定没有完成。")); return; }
            again();
        });
        return;
    }
    if (job->formattedOffset < job->formatted.size()) {
        std::vector<MaiContextImportRecord> batch;
        std::size_t bytes = 0;
        for (auto index = job->formattedOffset; index < job->formatted.size() && batch.size() < 32; ++index) {
            const auto& record = job->formatted[index];
            const auto length = record.sourceId.size() + record.text.size();
            if (!batch.empty() && bytes + length > 64 * 1024) break;
            // A single legal record may exceed the target chunk size; it still
            // stays within Core's256KiB record cap and runs entirely off Main.
            bytes += length; batch.push_back(record);
        }
        const auto count = batch.size();
        const auto sequence = job->sequence;
        job->inFlight = true;
        historyWork<HistoryCoreResult>(this, QStringLiteral("sessionImHistoryAppend"), [job, importId, sequence, batch = std::move(batch)] {
            QElapsedTimer timer; timer.start();
            if (job->canceled->load()) return HistoryCoreResult{{MaiErrorCode::Canceled, "History import canceled"}, timer.elapsed()};
            auto result = job->worker.appendContextImport(importId, sequence, batch);
            if (job->canceled->load()) job->worker.abortContextImport(importId);
            return HistoryCoreResult{std::move(result), timer.elapsed()};
        }, [this, job, peer, count, bytes, current, again](HistoryCoreResult result) {
            if (!current()) return;
            job->inFlight = false;
            job->maximumStepMillis = std::max(job->maximumStepMillis, result.milliseconds);
            if (!result.value) { abortHistory(peer, QStringLiteral("聊天历史暂存失败，未启用自动回复。")); return; }
            job->sequence = result.value.value().nextSequence;
            job->imported = result.value.value().recordCount;
            job->formattedOffset += count;
            qInfo() << "[session-im] history stage rows=" << count << "bytes=" << bytes << "worker_ms=" << result.milliseconds;
            QTimer::singleShot(0, this, again);
        });
        return;
    }
    job->formatted.clear(); job->formattedOffset = 0;
    if (job->page && job->offset < job->page->records.size()) {
        std::vector<MaiIMHistoryRecord> selected;
        std::size_t bytes = 0;
        while (job->offset < job->page->records.size() && selected.size() < 32) {
            const auto& row = job->page->records[job->offset];
            const auto match = runtime_->router.historyMatch(job->binding.account, job->binding.peerId, job->binding.epoch,
                row.rowId, row.remoteId.value_or(""), row.fromUserId, row.toUserId);
            if (match == Router::HistoryMatch::Stale) { abortHistory(peer, QStringLiteral("历史导入的原绑定已改变。")); return; }
            if (match == Router::HistoryMatch::AwaitingPersistence) break;
            if (match == Router::HistoryMatch::LiveInput) { ++job->offset; continue; }
            if (!selected.empty() && bytes + row.text.size() > 64 * 1024) break;
            bytes += row.text.size(); selected.push_back(row); ++job->offset;
        }
        if (selected.empty()) {
            if (job->offset == job->page->records.size()) QTimer::singleShot(0, this, again);
            return;
        }
        job->inFlight = true;
        historyWork<MaiResult<std::vector<MaiContextImportRecord>>>(this, QStringLiteral("sessionImHistoryFormat"),
            [job, selected = std::move(selected)]() -> MaiResult<std::vector<MaiContextImportRecord>> {
                std::vector<MaiContextImportRecord> formatted;
                for (const auto& row : selected) {
                    if (job->canceled->load()) return {MaiErrorCode::Canceled, "History import canceled"};
                    auto record = maiFormatIMHistoryContextRecord(row, job->descriptor);
                    if (!record) return record.error();
                    formatted.push_back(std::move(record.value()));
                }
                return formatted;
            }, [this, job, peer, current, again](MaiResult<std::vector<MaiContextImportRecord>> result) {
                if (!current()) return;
                job->inFlight = false;
                if (!result) { abortHistory(peer, QStringLiteral("一条聊天历史无法完整安全地导入，绑定没有完成。")); return; }
                job->formatted = std::move(result.value());
                again();
            });
        return;
    }
    if (job->page) {
        if (!job->page->hasMore) { job->eof = true; job->reader.reset(); }
        job->page.reset(); job->offset = 0;
    }
    if (!job->eof) {
        const auto reader = job->reader;
        job->inFlight = true;
        historyWork<MaiResult<MaiIMHistoryPage>>(this, QStringLiteral("sessionImHistoryRead"), [job, reader]() -> MaiResult<MaiIMHistoryPage> {
            if (job->canceled->load()) return {MaiErrorCode::Canceled, "History import canceled"};
            return reader->read();
        }, [this, job, peer, current, again](MaiResult<MaiIMHistoryPage> page) {
            if (!current()) return;
            job->inFlight = false;
            if (!page) { abortHistory(peer, QStringLiteral("本地聊天快照读取失败或超时，未发布部分历史。")); return; }
            job->page = std::move(page.value());
            job->offset = 0;
            again();
        });
        return;
    }
    job->inFlight = true;
    historyWork<HistoryCoreResult>(this, QStringLiteral("sessionImHistoryCommit"), [job, importId] {
        QElapsedTimer timer; timer.start();
        if (job->canceled->load()) return HistoryCoreResult{{MaiErrorCode::Canceled, "History import canceled"}, timer.elapsed()};
        auto result = job->worker.commitContextImport(importId);
        if (result) job->committed = true;
        return HistoryCoreResult{std::move(result), timer.elapsed()};
    }, [this, job, peer, current](HistoryCoreResult result) {
        if (!current()) return;
        job->inFlight = false;
        job->maximumStepMillis = std::max(job->maximumStepMillis, result.milliseconds);
        if (!result.value && result.value.error().code() == MaiErrorCode::Busy) {
            if (!runtime_->controller.agent().isBusy(job->binding.sessionId))
                QTimer::singleShot(0, this, [this, job, peer] { if (runtime_->sameHistory(job)) advanceHistory(peer); });
            return;
        }
        if (!result.value || !runtime_->currentHistory(job) ||
            runtime_->router.completeHistory(job->binding.account, job->binding.peerId, job->binding.epoch) != Router::Status::Ok) {
            abortHistory(peer, QStringLiteral("历史提交未确认，未启用自动回复。")); return;
        }
        if (job->deadline) { job->deadline->stop(); job->deadline->deleteLater(); }
        runtime_->historyJobs.remove(peer);
        purgeHistoryLater(job->worker, job->binding.sessionId, job->maintenanceContext);
        const QPointer<DesktopSessionImBridge> lifetime(this);
        emit historyImported(peer, job->imported, job->maximumStepMillis);
        if (!lifetime) return;
        emit bindingChanged(peer);
        if (lifetime) dispatch(QString::fromStdString(job->binding.sessionId));
    });
}

QString DesktopSessionImBridge::boundSession(const QString& peer) const {
    const auto scope = runtime_->liveScope();
    const auto binding = scope ? runtime_->router.bindingForPeer(*scope, peer.toStdString()) : std::nullopt;
    return binding ? QString::fromStdString(binding->sessionId) : QString();
}

QHash<QString, QString> DesktopSessionImBridge::bindings() const {
    QHash<QString, QString> result;
    const auto scope = runtime_->liveScope();
    if (scope) for (const auto& binding : runtime_->router.snapshot(*scope).bindings)
        result.insert(QString::fromStdString(binding.peerId), QString::fromStdString(binding.sessionId));
    return result;
}

QHash<QString, QString> DesktopSessionImBridge::readyBindings() const {
    QHash<QString, QString> result;
    const auto scope = runtime_->liveScope();
    if (scope) for (const auto& binding : runtime_->router.snapshot(*scope).bindings)
        if (!binding.historyPending)
            result.insert(QString::fromStdString(binding.peerId), QString::fromStdString(binding.sessionId));
    return result;
}

bool DesktopSessionImBridge::historyPending(const QString& peer) const {
    const auto scope = runtime_->liveScope();
    const auto binding = scope ? runtime_->router.bindingForPeer(*scope, peer.toStdString()) : std::nullopt;
    return binding && binding->historyPending;
}

void DesktopSessionImBridge::scheduleHistoryMaintenance(const QStringList& roots) {
    auto maintenance = std::make_shared<HistoryMaintenance>();
    maintenance->worker = runtime_->controller.agent().contextImportWorker();
    maintenance->roots = roots;
    maintenance->retryContext = QCoreApplication::instance();
    maintenance->roots.removeDuplicates();
    continueHistoryMaintenance(maintenance);
}

void DesktopSessionImBridge::unbind(const QString& peer) {
    const auto scope = runtime_->liveScope();
    if (!scope || !runtime_->router.bindingForPeer(*scope, peer.toStdString())) return;
    if (runtime_->router.unbind(*scope, peer.toStdString()) != Router::Status::Ok) return;
    prunePlatformWork();
    emit bindingChanged(peer);
}

void DesktopSessionImBridge::localTakeover(const QString& session) {
    const auto scope = runtime_->liveScope();
    if (!scope) return;
    QStringList peers;
    for (const auto& binding : runtime_->router.snapshot(*scope).bindings)
        if (binding.sessionId == session.toStdString()) peers.append(QString::fromStdString(binding.peerId));
    runtime_->router.revokeSession(*scope, session.toStdString());
    prunePlatformWork();
    QPointer<DesktopSessionImBridge> lifetime(this);
    for (const auto& peer : peers) { emit bindingChanged(peer); if (!lifetime) return; }
}

void DesktopSessionImBridge::suspend() {
    if (runtime_->scope) runtime_->router.logout(*runtime_->scope);
    prunePlatformWork();
}

void DesktopSessionImBridge::pruneSessions() {
    const auto scope = runtime_->liveScope();
    if (!scope) return;
    const auto bindings = runtime_->router.snapshot(*scope).bindings;
    QPointer<DesktopSessionImBridge> lifetime(this);
    for (const auto& binding : bindings) {
        if (!runtime_->friendCurrent(binding.peerId)) unbind(QString::fromStdString(binding.peerId));
        else if (runtime_->workspace(binding.sessionId).toStdString() != binding.workspaceKey) {
            emit relayError(QString::fromStdString(binding.peerId), QStringLiteral("会话工作目录已改变，没有自动发送这轮回复，请重新确认绑定。"));
            if (!lifetime) return;
            localTakeover(QString::fromStdString(binding.sessionId));
        }
        if (!lifetime) return;
    }
}

void DesktopSessionImBridge::acceptLive(quint64 ingressId, const RemoteIMMessage& message,
    quint64 rawSequence, quint64 rawGeneration) {
    const auto scope = runtime_->liveScope();
    if (!scope || ingressId == 0) return;
    const QString peer = message.fromUserId;
    const auto binding = runtime_->router.bindingForPeer(*scope, peer.toStdString());
    if (!binding) return;
    const auto history = runtime_->historyJobs.value(peer);
    if (history && rawSequence && rawGeneration == history->boundary.generation && rawSequence <= history->boundary.sequence)
        return;  // Raw pre-publication packets are prior context, never new work.
    const bool attachment = message.hasImage || message.hasVideo || message.hasVoice || message.hasFile;
    QString payloadError;
    if (!attachment && message.text.trimmed().isEmpty())
        payloadError = QStringLiteral("\u8fd9\u6761\u9644\u4ef6\u8fd8\u6ca1\u6709\u53ef\u8bfb\u53d6\u7684\u672c\u5730\u7f13\u5b58\uff0c\u6ca1\u6709\u63d0\u4ea4\u7ed9\u521b\u4f5c\u4e2d\u5fc3\u3002");
    else if (message.text.size() > kMaximumInputBytes || message.text.toUtf8().size() > kMaximumInputBytes)
        payloadError = QStringLiteral("\u8fd9\u6761\u6587\u5b57\u8d85\u8fc7\u4f1a\u8bdd\u8f93\u5165\u4e0a\u9650\uff0c\u6ca1\u6709\u63d0\u4ea4\u7ed9\u521b\u4f5c\u4e2d\u5fc3\u3002");
    // Snapshot the authoritative binding at original live ingress, before any
    // asynchronous import. A late readiness callback never selects a new bind.
    Router::Incoming incoming;
    incoming.account = *scope;
    incoming.peerId = peer.toStdString();
    incoming.messageId = message.id.toStdString();
    incoming.senderUserId = message.fromUserId.toStdString();
    incoming.recipientUserId = message.toUserId.toStdString();
    incoming.expectedBindingEpoch = binding->epoch;
    incoming.expectedSessionId = binding->sessionId;
    incoming.origin = message.origin == RemoteIMMessageOrigin::Human ? Router::Origin::Human :
        message.origin == RemoteIMMessageOrigin::Machine ? Router::Origin::Machine : Router::Origin::Unknown;
    incoming.live = message.direction == RemoteIMMessageDirection::Incoming;
    incoming.controlFrame = message.hasApprovalRequest || message.hasApprovalDecision;
    incoming.hasContent = payloadError.isEmpty();
    incoming.requirePersistence = true;
    const auto admitted = runtime_->router.accept(incoming);
    if (admitted.status != Router::Status::Ok || !admitted.input) {
        if (admitted.status == Router::Status::QueueFull) {
            if (binding->historyPending) abortHistory(peer, QStringLiteral("历史准备期间的新消息已超过32条，绑定没有完成；原始聊天记录仍保留。"));
            else emit relayError(peer, QStringLiteral("\u5f85\u5904\u7406\u7684\u4f1a\u8bdd\u6d88\u606f\u5df2\u8fbe 32 \u6761\uff1b\u8fd9\u6761\u6d88\u606f\u6ca1\u6709\u63d0\u4ea4\u3002"));
        }
        else if (!payloadError.isEmpty() && message.origin == RemoteIMMessageOrigin::Human &&
                 runtime_->router.bindingForPeer(*scope, peer.toStdString())) {
            if (binding->historyPending) abortHistory(peer, payloadError);
            else emit relayError(peer, payloadError);
        }
        return;
    }
    Runtime::Payload payload;
    payload.input = *admitted.input;
    payload.ingressId = ingressId;
    payload.text = message.text;
    if (message.hasQuote && !message.quote.digest.isEmpty())
        payload.text = QStringLiteral("\u5f15\u7528\uff1a%1\n\n%2").arg(message.quote.digest, payload.text);
    payload.stagingIdentity = attachmentIdentity(runtime_->app.client().currentAccount(), peer, message.id);
    payload.source = message;
    payload.prepared = !attachment;
    auto* deadline = new QTimer(this);
    deadline->setObjectName(QStringLiteral("sessionImReadinessDeadline"));
    deadline->setSingleShot(true);
    payload.readinessDeadline = deadline;
    connect(deadline, &QTimer::timeout, this, [this, ingressId] {
        failLive(ingressId, QStringLiteral("附件下载等待超时，这条消息没有提交，后续消息会继续处理。"));
    });
    // Download readiness has a bounded lifetime independent of a worker pool.
    // This timer owns no routing state: discardInput retires the kernel head.
    deadline->start(90000);
    runtime_->payloads.insert(payload.input.inputId, std::move(payload));
    dispatch(QString::fromStdString(admitted.input->binding.sessionId));
}

void DesktopSessionImBridge::persistedLive(quint64 ingress, bool ok, bool inserted,
    const QString& rowId, const QString&) {
    for (const auto& payload : runtime_->payloads) {
        if (payload.ingressId != ingress) continue;
        const auto input = payload.input;
        const auto peer = QString::fromStdString(input.binding.peerId);
        if (!runtime_->bindingCurrent(input)) { prunePlatformWork(); return; }
        if (!ok) {
            if (historyPending(peer)) abortHistory(peer, QStringLiteral("原始消息未能完整保存，历史准备和绑定已取消。"));
            else {
                runtime_->router.discardInput(input.binding.account, input.inputId);
                prunePlatformWork();
                emit relayError(peer, QStringLiteral("原始消息未能保存到本地，没有提交给创作中心。"));
            }
            return;
        }
        const auto result = runtime_->router.confirmPersisted(input.binding.account, input.inputId, inserted, rowId.toStdString());
        if (result != Router::Status::Ok && historyPending(peer)) {
            abortHistory(peer, QStringLiteral("原始消息的持久化回执未确认，绑定没有完成。"));
            return;
        }
        prunePlatformWork();
        advanceHistory(peer);
        return;
    }
}

void DesktopSessionImBridge::readyLive(quint64 ingressId, const RemoteIMMessage& message) {
    for (auto it = runtime_->payloads.begin(); it != runtime_->payloads.end(); ++it) {
        if (it->ingressId != ingressId || it->ready) continue;
        const auto input = it->input;
        if (!runtime_->bindingCurrent(input)) { prunePlatformWork(); return; }
        const auto& original = it->source;
        if (message.id != original.id || message.fromUserId != original.fromUserId ||
            message.toUserId != original.toUserId || message.origin != original.origin ||
            message.direction != original.direction || message.hasImage != original.hasImage ||
            message.hasVideo != original.hasVideo || message.hasFile != original.hasFile ||
            message.hasVoice != original.hasVoice) return;
        if ((message.hasImage && message.image.localPath.isEmpty()) ||
            (message.hasVideo && message.video.localPath.isEmpty()) ||
            (message.hasFile && message.file.localPath.isEmpty()) ||
            (message.hasVoice && message.voice.localPath.isEmpty())) {
            failLive(ingressId, QStringLiteral("这条附件没有可读取的本地缓存，没有提交给创作中心。"));
            return;
        }
        it->source.image = message.image;
        it->source.video = message.video;
        it->source.file = message.file;
        it->source.voice = message.voice;
        it->ready = true;
        if (it->readinessDeadline) it->readinessDeadline->deleteLater();
        it->readinessDeadline.clear();
        if (!it->prepared && historyPending(QString::fromStdString(input.binding.peerId))) {
            preparePayload(input.inputId);
            return;
        }
        for (const auto& route : runtime_->router.snapshot(input.binding.account).active)
            if (route.reservation.input.inputId == input.inputId) {
                processReservation(route.reservation.token);
                return;
            }
        dispatch(QString::fromStdString(input.binding.sessionId));
        return;
    }
}

void DesktopSessionImBridge::failLive(quint64 ingressId, const QString& error) {
    for (const auto& payload : runtime_->payloads) {
        if (payload.ingressId != ingressId || payload.ready) continue;
        const auto input = payload.input;
        if (!runtime_->bindingCurrent(input)) { prunePlatformWork(); return; }
        if (historyPending(QString::fromStdString(input.binding.peerId))) {
            abortHistory(QString::fromStdString(input.binding.peerId), error);
            return;
        }
        if (runtime_->router.discardInput(input.binding.account, input.inputId) != Router::Status::Ok) return;
        prunePlatformWork();
        QPointer<DesktopSessionImBridge> lifetime(this);
        emit relayError(QString::fromStdString(input.binding.peerId), error);
        if (lifetime) QTimer::singleShot(0, this,
            [this, session = QString::fromStdString(input.binding.sessionId)] { dispatch(session); });
        return;
    }
}

void DesktopSessionImBridge::dispatch(const QString& session) {
    QPointer<DesktopSessionImBridge> lifetime(this);
    const auto scope = runtime_->liveScope();
    if (!scope) return;
    const QString workspace = runtime_->workspace(session.toStdString());
    const auto reservation = runtime_->router.reserveNext(*scope, session.toStdString(), workspace.toStdString(),
        !runtime_->controller.agent().isBusy(session.toStdString()));
    if (reservation.status == Router::Status::Ok && reservation.reservation) {
        processReservation(reservation.reservation->token);
    } else if (reservation.status != Router::Status::Busy && reservation.status != Router::Status::NotBound) {
        for (const auto& binding : runtime_->router.snapshot(*scope).bindings)
            if (binding.sessionId == session.toStdString() && binding.workspaceKey != workspace.toStdString()) {
                emit relayError(QString::fromStdString(binding.peerId), QStringLiteral("\u4f1a\u8bdd\u5de5\u4f5c\u76ee\u5f55\u5df2\u6539\u53d8\uff0c\u8bf7\u91cd\u65b0\u786e\u8ba4\u7ed1\u5b9a\u3002"));
                if (lifetime) localTakeover(session);
                return;
            }
    }
    if (lifetime) prunePlatformWork();
}

void DesktopSessionImBridge::preparePayload(quint64 id) {
    auto payload = runtime_->payloads.find(id);
    if (payload == runtime_->payloads.end() || !payload->ready || payload->prepared || runtime_->preparing.contains(id)) return;
    const auto input = payload->input;
    const auto peer = QString::fromStdString(input.binding.peerId);
    const auto session = QString::fromStdString(input.binding.sessionId);
    const auto fail = [this, input, peer, session](const QString& error) {
        const auto scope = runtime_->liveScope();
        const auto binding = scope ? runtime_->router.bindingForPeer(*scope, input.binding.peerId) : std::nullopt;
        if (!binding || binding->account != input.binding.account || binding->epoch != input.binding.epoch) { prunePlatformWork(); return; }
        const QPointer<DesktopSessionImBridge> lifetime(this);
        if (runtime_->workspace(input.binding.sessionId).toStdString() != input.binding.workspaceKey) {
            emit relayError(peer, QStringLiteral("会话工作目录已改变，这条消息没有提交，请重新确认绑定。"));
            if (lifetime) localTakeover(session);
            return;
        }
        if (historyPending(peer)) { abortHistory(peer, error); return; }
        runtime_->router.discardInput(*scope, input.inputId);
        prunePlatformWork();
        emit relayError(peer, error);
        if (lifetime) QTimer::singleShot(0, this, [this, session] { dispatch(session); });
    };
    if (attachmentPreparationAdmissions().load() >= kPreparationAdmissionLimit) {
        fail(QStringLiteral("附件准备队列已满，这条消息没有提交。")); return;
    }
    attachmentPreparationAdmissions().fetch_add(1);
    const Runtime::Payload copied = *payload;
    auto canceled = std::make_shared<std::atomic<bool>>(false);
    runtime_->preparing.insert(id, canceled);
    auto* watcher = new QFutureWatcher<PreparedInput>(this);
    watcher->setObjectName(QStringLiteral("sessionImAttachmentPreparation"));
    auto* timer = new QTimer(watcher);
    timer->setObjectName(QStringLiteral("sessionImAttachmentDeadline"));
    timer->setSingleShot(true);
    const QDeadlineTimer deadline(kPreparationBudgetMs);
    connect(timer, &QTimer::timeout, this, [this, id, canceled, fail] {
        if (runtime_->preparing.value(id) != canceled) return;
        canceled->store(true);
        runtime_->preparing.remove(id);
        fail(QStringLiteral("附件准备超时，这条消息没有提交，后续消息会继续处理。"));
    });
    connect(watcher, &QFutureWatcher<PreparedInput>::finished, this,
        [this, watcher, timer, id, input, canceled, deadline, fail, peer, session] {
            timer->stop();
            auto prepared = watcher->result();
            watcher->deleteLater();
            if (runtime_->preparing.value(id) != canceled) return;
            runtime_->preparing.remove(id);
            if (canceled->load()) return;
            if (deadline.hasExpired()) prepared.error = QStringLiteral("附件准备超时，没有提交给创作中心。");
            if (!runtime_->bindingCurrent(input) || !prepared.error.isEmpty()) {
                fail(prepared.error.isEmpty() ? QStringLiteral("会话或账号已改变，附件没有提交。") : prepared.error);
                return;
            }
            auto payload = runtime_->payloads.find(id);
            if (payload == runtime_->payloads.end()) return;
            payload->prepared = true;
            payload->text = std::move(prepared.text);
            payload->images = std::move(prepared.images);
            payload->videos = std::move(prepared.videos);
            if (historyPending(peer)) { advanceHistory(peer); return; }
            for (const auto& route : runtime_->router.snapshot(input.binding.account).active)
                if (route.reservation.input.inputId == id) { processReservation(route.reservation.token); return; }
            dispatch(session);
        });
    timer->start(kPreparationBudgetMs);
    watcher->setFuture(QtConcurrent::run(attachmentPreparationPool(), [copied, canceled, deadline] {
        const auto release = qScopeGuard([] { attachmentPreparationAdmissions().fetch_sub(1); });
        if (canceled->load() || deadline.hasExpired()) return PreparedInput{};
        return stageInput(QString::fromStdString(copied.input.binding.workspaceKey), copied.stagingIdentity,
            copied.source, copied.text, canceled, deadline);
    }));
}

void DesktopSessionImBridge::processReservation(quint64 token) {
    QPointer<DesktopSessionImBridge> lifetime(this);
    const auto route = runtime_->active(token);
    if (!route || route->phase != Router::Phase::Reserved) return;
    const auto reservation = route->reservation;
    const auto input = reservation.input;
    const QString session = QString::fromStdString(input.binding.sessionId);
    const auto scope = runtime_->liveScope();
    auto payload = runtime_->payloads.find(input.inputId);
    if (!scope || payload == runtime_->payloads.end() || !runtime_->current(reservation)) {
        if (scope && runtime_->workspace(input.binding.sessionId).toStdString() != input.binding.workspaceKey)
            emit relayError(QString::fromStdString(input.binding.peerId), QStringLiteral("会话工作目录已改变，这条消息没有提交，请重新确认绑定。"));
        if (lifetime) localTakeover(session);
        return;
    }
    if (!payload->ready) return;
    if (!payload->prepared) { preparePayload(input.inputId); return; }
    const Runtime::Payload copied = *payload;
    auto observed = std::make_shared<InputReceipt>();
    observed->session = input.binding.sessionId;
    const auto subscription = runtime_->controller.agent().eventBus().subscribe([observed](const MaiEvent& event) {
        if (event.sessionId != observed->session) return;
        if (event.type == MaiEventType::SessionError) { observed->failed = true; return; }
        if (event.type == MaiEventType::SessionDeleted || event.type == MaiEventType::MessageRemoved) { observed->valid = false; return; }
        if (event.type != MaiEventType::MessageUpdated) return;
        std::lock_guard<std::mutex> lock(observed->mutex);
        if (observed->capturing && std::this_thread::get_id() == observed->submitThread && observed->ids.size() < 2)
            observed->ids.push_back(event.messageId);
        else if (std::find(observed->ids.begin(), observed->ids.end(), event.messageId) == observed->ids.end()) observed->valid = false;
    });
    const auto accepted = runtime_->controller.agent().submit(MaiSendPrompt{input.binding.sessionId,
        copied.text.toStdString(), copied.images, copied.videos});
    std::vector<std::string> ids;
    { std::lock_guard<std::mutex> lock(observed->mutex); observed->capturing = false; ids = observed->ids; }
    if (!accepted) {
        observed->valid = false;
        runtime_->controller.agent().eventBus().unsubscribe(subscription);
        runtime_->router.releaseReservation(token, *scope, input.binding.workspaceKey, accepted.error().code() == MaiErrorCode::Busy);
        prunePlatformWork();
        if (accepted.error().code() != MaiErrorCode::Busy) {
            emit relayError(QString::fromStdString(input.binding.peerId), QStringLiteral("\u8fd9\u6761\u6d88\u606f\u672a\u80fd\u63d0\u4ea4\u7ed9\u521b\u4f5c\u4e2d\u5fc3\uff0c\u8bf7\u5728\u672c\u673a\u67e5\u770b\u4f1a\u8bdd\u3002"));
            if (lifetime) QTimer::singleShot(0, this, [this, session] { dispatch(session); });
        }
        return;
    }
    std::string user;
    for (const auto& id : ids) {
        MaiMessage message;
        if (runtime_->controller.agent().getMessage(input.binding.sessionId, id, message) && message.role == MaiRole::User) user = id;
    }
    const Router::TurnReceipt receipt{user, accepted.value(), accepted.value()};
    if (user.empty() || std::find(ids.begin(), ids.end(), accepted.value()) == ids.end() ||
        runtime_->router.bindReceipt(token, receipt, *scope, runtime_->workspace(input.binding.sessionId).toStdString()) != Router::Status::Ok) {
        observed->valid = false;
        runtime_->controller.agent().eventBus().unsubscribe(subscription);
        localTakeover(session);
        return;
    }
    runtime_->coreLeases.insert(token, {observed, subscription});
}

void DesktopSessionImBridge::acknowledgeDispatch(quint64 token, quint32 index) {
    const auto route = runtime_->active(token);
    const auto scope = runtime_->liveScope();
    if (!route || !route->receipt || !scope) return;
    const QString session = QString::fromStdString(route->reservation.input.binding.sessionId);
    runtime_->router.acknowledgeDispatch(token, *route->receipt, *scope, route->reservation.input.binding.workspaceKey, index);
    const bool retired = !runtime_->active(token);
    prunePlatformWork();
    if (retired) QTimer::singleShot(0, this, [this, session] { dispatch(session); });
}

void DesktopSessionImBridge::finish(const QString& session) {
    QPointer<DesktopSessionImBridge> lifetime(this);
    const auto scope = runtime_->liveScope();
    if (!scope) return;
    std::optional<Router::ActiveRoute> route;
    for (const auto& item : runtime_->router.snapshot(*scope).active)
        if (item.reservation.input.binding.sessionId == session.toStdString()) { route = item; break; }
    if (!route) { QTimer::singleShot(0, this, [this, session] { dispatch(session); }); return; }
    if (route->phase != Router::Phase::Running || !route->receipt) return;
    MaiMessage reply;
    if (!runtime_->controller.agent().getMessage(session.toStdString(), route->receipt->assistantMessageId, reply) ||
        reply.role != MaiRole::Assistant || reply.completed == 0) return;
    const quint64 token = route->reservation.token;
    const auto input = route->reservation.input;
    const auto lease = runtime_->coreLeases.constFind(token);
    const auto payload = runtime_->payloads.constFind(input.inputId);
    if (!runtime_->current(route->reservation) || lease == runtime_->coreLeases.constEnd() || payload == runtime_->payloads.constEnd() ||
        !lease->observed->valid.load()) {
        if (runtime_->workspace(input.binding.sessionId).toStdString() != input.binding.workspaceKey)
            emit relayError(QString::fromStdString(input.binding.peerId), QStringLiteral("会话工作目录已改变，没有自动发送这轮回复，请重新确认绑定。"));
        if (lifetime) localTakeover(session);
        return;
    }
    if (lease->observed->failed.load()) {
        runtime_->router.finishWithoutOutput(token, *route->receipt, *scope, input.binding.workspaceKey);
        prunePlatformWork();
        emit relayError(QString::fromStdString(input.binding.peerId), QStringLiteral("\u521b\u4f5c\u4e2d\u5fc3\u672a\u5b8c\u6210\u8fd9\u6761\u6d88\u606f\uff0c\u8bf7\u5728\u672c\u673a\u67e5\u770b\u4f1a\u8bdd\u9519\u8bef\u3002"));
        if (lifetime) QTimer::singleShot(0, this, [this, session] { dispatch(session); });
        return;
    }
    MaiSession owner;
    runtime_->controller.agent().getSession(session.toStdString(), owner);
    owner.directory = input.binding.workspaceKey;
    QString mediaError;
    const auto media = deliveredMedia(reply, owner, &mediaError);
    const QString text = QString::fromStdString(reply.text()).trimmed();
    const auto count = static_cast<std::uint32_t>(media.size() + (!text.isEmpty() ? 1 : 0));
    if (count == 0) {
        runtime_->router.finishWithoutOutput(token, *route->receipt, *scope, input.binding.workspaceKey);
        prunePlatformWork();
        emit relayError(QString::fromStdString(input.binding.peerId), mediaError.isEmpty()
            ? QStringLiteral("\u8fd9\u8f6e\u6ca1\u6709\u53ef\u81ea\u52a8\u56de\u4f20\u7684\u6587\u5b57\u6216\u660e\u786e\u4ea4\u4ed8\u7684\u5a92\u4f53\uff0c\u8bf7\u5728\u521b\u4f5c\u4e2d\u5fc3\u67e5\u770b\u7ed3\u679c\u3002") : mediaError);
        if (lifetime) QTimer::singleShot(0, this, [this, session] { dispatch(session); });
        return;
    }
    if (runtime_->router.beginOutput(token, *route->receipt, *scope, input.binding.workspaceKey, count) != Router::Status::Ok) return;
    const Runtime::Payload copied = *payload;
    const Router::ActiveRoute output = *route;
    const auto canSend = [lifetime, output] { return lifetime && lifetime->runtime_->outputCurrent(output); };
    const auto completion = [lifetime, output](bool ok, const QString&, const RemoteIMSendReceipt&) {
        if (lifetime && !ok && lifetime->runtime_->bindingCurrent(output.reservation.input))
            emit lifetime->relayError(QString::fromStdString(output.reservation.input.binding.peerId),
                QStringLiteral("\u4e00\u9879\u521b\u4f5c\u4e2d\u5fc3\u56de\u590d\u53d1\u9001\u5931\u8d25\uff0c\u8bf7\u5728\u804a\u5929\u8bb0\u5f55\u4e2d\u67e5\u770b\uff0c\u7cfb\u7edf\u4e0d\u4f1a\u81ea\u52a8\u91cd\u53d1\u3002"));
    };
    const RemoteIMQuote quote = MessageQuote::quoteFor(copied.source);
    std::uint32_t index = 0;
    if (!text.isEmpty()) {
        runtime_->app.sendMachineTextTo(QString::fromStdString(input.binding.peerId), text, completion, canSend, quote, !quote.digest.isEmpty());
        if (!lifetime) return;
        acknowledgeDispatch(token, index++);
    }
    for (const auto& item : media) {
        const auto dispatchIndex = index++;
        if (!canSend()) { acknowledgeDispatch(token, dispatchIndex); continue; }
        runtime_->app.sendMachineMediaTo(QString::fromStdString(input.binding.peerId), item.path, item.type, item.caption,
            completion, canSend, quote, !quote.digest.isEmpty(), [lifetime, token, dispatchIndex](bool) {
                if (lifetime) lifetime->acknowledgeDispatch(token, dispatchIndex);
            });
        if (!lifetime) return;
    }
    if (!mediaError.isEmpty()) emit relayError(QString::fromStdString(input.binding.peerId), mediaError);
}
