#include "agent/DesktopSessionImBridge.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDateTime>
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
#include <QMap>
#include <QPointer>
#include <QScopeGuard>
#include <QSaveFile>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QThreadPool>
#include <QVariant>
#include <QUuid>

#include <atomic>
#include <algorithm>
#include <mutex>
#include <thread>
#include <deque>
#include <map>
#include <set>
#include <QtConcurrent/QtConcurrentRun>

#include "agent/AgentController.h"
#include "app/RemoteIMApplication.h"
#include "MaiAgentSendMediaTool.h"
#include "MaiAgentSendFileTool.h"
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
        QCoreApplication::instance()->setProperty("sessionImAttachmentPreparationPool",
                                                  QVariant::fromValue<QObject*>(value));
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
    std::string turnId;
    bool capturing = true;
    bool admitting = false;
    std::thread::id admissionThread;
    std::vector<std::string> admissionIds;
    struct Text {
        std::string messageId, partId, turnId;
    };
    struct Continuation {
        std::string messageId, turnId;
        std::vector<std::string> inputs;
    };
    std::vector<Text> committed;
    std::vector<Continuation> continuations;
    std::set<std::string> provisionalAssistants;
    std::atomic<bool> valid{true};
    std::atomic<bool> failed{false};
};

// Cross-thread Stop touches only this weak ownership registry and a receipt's
// mutex/atomic flag. GUI ledgers, storage and SDK calls stay on their owner.
struct InterruptionWatches {
    std::mutex mutex;
    std::map<std::string, std::weak_ptr<InputReceipt>> sessions;
};

void observeInput(const std::shared_ptr<InputReceipt>& observed, const MaiEvent& event) {
    if (event.sessionId != observed->session) return;
    if (event.type == MaiEventType::SessionUpdated && event.field != "title" && event.field != "input") {
        observed->valid = false;
        return;
    }
    if (event.type == MaiEventType::SessionError) {
        observed->failed = true;
        return;
    }
    if (event.type == MaiEventType::SessionDeleted || event.type == MaiEventType::MessageRemoved) {
        observed->valid = false;
        return;
    }
    std::lock_guard<std::mutex> lock(observed->mutex);
    if (event.type == MaiEventType::MessageTextCommitted) {
        if ((!observed->turnId.empty() && event.turnId != observed->turnId) ||
            (std::find(observed->ids.begin(), observed->ids.end(), event.messageId) == observed->ids.end() &&
             !observed->provisionalAssistants.count(event.messageId))) {
            observed->valid = false;
            return;
        }
        observed->committed.push_back({event.messageId, event.partId, event.turnId});
        return;
    }
    if (event.type == MaiEventType::TurnSteeringApplied) {
        if (event.turnId != observed->turnId || event.inputMessageIds.empty()) {
            observed->valid = false;
            return;
        }
        // Core 可在 submit 返回前消费补充；新段先隔离，GUI/普通 worker 用接收回执验证后才交付。
        observed->provisionalAssistants.insert(event.messageId);
        observed->continuations.push_back({event.messageId, event.turnId, event.inputMessageIds});
        return;
    }
    if (event.type != MaiEventType::MessageUpdated) return;
    if (observed->capturing && std::this_thread::get_id() == observed->submitThread && observed->ids.size() < 2) {
        if (std::find(observed->ids.begin(), observed->ids.end(), event.messageId) == observed->ids.end())
            observed->ids.push_back(event.messageId);
        return;
    }
    if (std::find(observed->ids.begin(), observed->ids.end(), event.messageId) != observed->ids.end() ||
        observed->provisionalAssistants.count(event.messageId))
        return;
    if (observed->admitting && observed->admissionThread == std::this_thread::get_id() &&
        observed->admissionIds.empty()) {
        observed->admissionIds.push_back(event.messageId);
        observed->ids.push_back(event.messageId);
        return;
    }
    observed->valid = false;
}

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
PreparedInput stageInput(const QString& workspace, const QString& identity, const RemoteIMMessage& source, QString text,
                         const std::shared_ptr<std::atomic<bool>>& canceled, const QDeadlineTimer& deadline) {
    PreparedInput result;
    result.text = std::move(text);
    struct Source {
        QString path;
        QString kind;
        qint64 limit;
    };
    QList<Source> sources;
    if (source.hasImage) sources.append({source.image.localPath, QStringLiteral("image"), 32ll * 1024 * 1024});
    if (source.hasVideo) sources.append({source.video.localPath, QStringLiteral("video"), 1024ll * 1024 * 1024});
    if (source.hasFile) sources.append({source.file.localPath, QStringLiteral("file"), 512ll * 1024 * 1024});
    if (source.hasVoice) sources.append({source.voice.localPath, QStringLiteral("audio"), 64ll * 1024 * 1024});
    const QString relativeDirectory = QStringLiteral(".maichat/session-attachments");
    const QString directory =
        QString::fromStdString(maiResolvePathWithinRoot(workspace.toStdString(), relativeDirectory.toStdString()));
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
            !original.isAbsolute() || !original.isFile() || !original.isReadable() || original.size() <= 0 ||
            original.size() > item.limit) {
            result.error = QStringLiteral("收到的附件尚未下载完成、不可读取或超过现有大小限制，没有提交给创作中心。");
            return result;
        }
        const auto mime = mimeDatabase.mimeTypeForFile(original, QMimeDatabase::MatchContent);
        QString suffix = original.suffix();
        if (suffix.isEmpty() || suffix.size() > 16) suffix = mime.preferredSuffix();
        if (suffix.isEmpty()) suffix = QStringLiteral("bin");
        const QString relative =
            relativeDirectory + QLatin1Char('/') + identity + QLatin1Char('-') + item.kind + QLatin1Char('.') + suffix;
        const QString target =
            QString::fromStdString(maiResolvePathWithinRoot(workspace.toStdString(), relative.toStdString()));
        QFile input(original.canonicalFilePath());
        QSaveFile output(target);
        if (target.isEmpty() || !input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
            result.error = QStringLiteral("无法将收到的附件保存到会话工作目录。");
            return result;
        }
        qint64 copied = 0;
        while (!input.atEnd()) {
            if (canceled->load() || deadline.hasExpired()) {
                output.cancelWriting();
                return result;
            }
            const QByteArray chunk = input.read(1024 * 1024);
            if (chunk.isEmpty() || output.write(chunk) != chunk.size()) {
                result.error = QStringLiteral("会话附件复制失败，没有提交给创作中心。");
                return result;
            }
            copied += chunk.size();
            if (copied > item.limit) {
                result.error = QStringLiteral("会话附件超过现有大小限制。");
                return result;
            }
        }
        if (copied != original.size() || canceled->load() || deadline.hasExpired() || !output.commit()) {
            result.error = QStringLiteral("会话附件未能完整保存，没有提交给创作中心。");
            return result;
        }
        const QString contentType = mime.name();
        if (item.kind == QStringLiteral("image") &&
            QStringList{QStringLiteral("image/jpeg"), QStringLiteral("image/png"), QStringLiteral("image/webp"),
                        QStringLiteral("image/gif")}
                .contains(contentType)) {
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
        result.text +=
            QStringLiteral("\n\n会话附件已保存到本机工作目录：\n- ") + documents.join(QStringLiteral("\n- "));
    return result;
}

struct OutputMedia {
    QString path;
    QString type;
    QString caption;
    QString partId;
};

QList<OutputMedia> deliveredMedia(const MaiMessage& reply, const MaiSession& owner, QString* error) {
    QList<OutputMedia> result;
    QSet<QString> paths;
    MaiToolContext context;
    context.root = owner.directory;
    context.sessionId = owner.id;
    for (const auto& part : reply.parts) {
        const auto* tool = std::get_if<MaiToolPart>(&part.body);
        if (!tool || (tool->tool != "agent_send_media" && tool->tool != "agent_send_file") ||
            tool->state != MaiToolState::Completed)
            continue;
        const auto receipt = QJsonDocument::fromJson(QByteArray::fromStdString(tool->output)).object();
        const QString path = receipt.value(QStringLiteral("path")).toString();
        const QString type = receipt.value(QStringLiteral("type")).toString();
        if (tool->tool == "agent_send_file") {
            const auto prepared = maiPrepareAgentFile(path.toStdString(), context);
            if (type != QStringLiteral("file") ||
                receipt.value(QStringLiteral("delivery")).toString() != QStringLiteral("current_ai_session") ||
                !prepared ||
                receipt.value(QStringLiteral("mime_type")).toString().toStdString() != prepared.value().mimeType ||
                receipt.value(QStringLiteral("name")).toString().toStdString() != prepared.value().name ||
                receipt.value(QStringLiteral("bytes")).toVariant().toULongLong() != prepared.value().bytes) {
                *error = QStringLiteral("一项会话文件已不可读取或与明确交付凭据不符，没有自动发送。");
                continue;
            }
            const QString absolute =
                QString::fromStdString(maiResolvePathWithinRoot(owner.directory, prepared.value().relativePath));
            if (!paths.contains(absolute)) {
                paths.insert(absolute);
                result.append({absolute, type, receipt.value(QStringLiteral("caption")).toString(),
                               QString::fromStdString(part.id)});
            }
            continue;
        }
        const auto prepared = maiPrepareAgentMedia(path.toStdString(), type.toStdString(), context);
        if (receipt.value(QStringLiteral("delivery")).toString() != QStringLiteral("current_ai_session") || !prepared ||
            receipt.value(QStringLiteral("mime_type")).toString().toStdString() != prepared.value().mimeType ||
            receipt.value(QStringLiteral("bytes")).toVariant().toULongLong() != prepared.value().bytes) {
            *error = QStringLiteral("一项会话媒体结果已不可读取或与交付凭据不符，没有自动发送。请在创作中心查看。");
            continue;
        }
        const QString absolute =
            QString::fromStdString(maiResolvePathWithinRoot(owner.directory, prepared.value().relativePath));
        if (!paths.contains(absolute)) {
            paths.insert(absolute);
            result.append(
                {absolute, type, receipt.value(QStringLiteral("caption")).toString(), QString::fromStdString(part.id)});
        }
    }
    // MaiImagePart can be a private tool observation (for example a screenshot),
    // and arbitrary tool JSON/Markdown paths are not delivery authority.
    return result;
}

}  // namespace

using Router = MaiSessionIMRouter;

namespace {
std::atomic<int>& historyReaderAdmissions() {
    static auto* count = new std::atomic<int>(0);
    return *count;
}

struct HistoryMaintenance {
    MaiContextImportWorker worker;
    QStringList roots;
    qsizetype offset = 0;
    std::string abortId;
    unsigned retries = 0;
    QPointer<QObject> retryContext;
};

void continueHistoryMaintenance(const std::shared_ptr<HistoryMaintenance>& maintenance);

void retryHistoryMaintenance(const std::shared_ptr<HistoryMaintenance>& maintenance, const MaiError& error,
                             const char* operation) {
    const unsigned attempt = std::min(maintenance->retries++, 6u);
    const int delay = std::min(100 * (1 << attempt), 5000);
    qWarning() << "[session-im] context maintenance deferred" << operation << maiErrorCodeToString(error.code())
               << "retry_ms=" << delay;
    // Only metadata waits in the ordinary Qt loop. No preparation thread
    // sleeps, and the weak Core capability stops when its owner is revoked.
    if (maintenance->retryContext)
        QTimer::singleShot(delay, maintenance->retryContext,
                           [maintenance] { continueHistoryMaintenance(maintenance); });
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
        const auto result =
            maintenance->worker.purgeContextImports(maintenance->roots.at(maintenance->offset).toStdString(), 32);
        if (!result) {
            if (result.error().code() == MaiErrorCode::Canceled) return;
            if (result.error().code() == MaiErrorCode::NotFound)
                ++maintenance->offset;
            else {
                retryHistoryMaintenance(maintenance, result.error(), "purge");
                return;
            }
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

    std::string stageId() {
        std::lock_guard<std::mutex> lock(stageMutex);
        return importId;
    }
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

struct HistoryOpenResult {
    std::shared_ptr<MaiIMHistorySnapshot> reader;
    MaiError error;
    bool capacity = false;
};
struct HistoryCoreResult {
    MaiResult<MaiContextImportProgress> value;
    qint64 milliseconds;
};

template <class Result, class Work, class Done>
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
}  // namespace

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
        std::string questionId;
    };
    struct CoreLease {
        struct Output {
            Router::TurnReceipt receipt;
            QString text;
            std::optional<OutputMedia> media;
            bool committedText = false;
            std::optional<RemoteIMApprovalRequest> approval;
            std::optional<RemoteIMApprovalDecision> resolution;
        };
        struct Approval {
            MaiPermissionRequest permission;
            Router::TurnReceipt receipt;
            QString publicToken;
            bool resolutionQueued = false;
            bool terminalDispatched = false;
        };
        std::shared_ptr<InputReceipt> observed;
        MaiEventBus::Token subscription = 0;
        Router::QueuedInput origin;
        std::string directory;
        std::map<std::string, Router::TurnReceipt> segments;
        std::set<std::string> sentParts;
        QSet<QString> sentMedia;
        QMap<quint32, Output> outputs;
        bool dispatching = false;
        std::optional<MaiQuestionRequest> question;
        QHash<QString, Approval> approvals;
        RemoteIMActivitySignal activity;
        qint64 lastActivityAt = 0;
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
    std::shared_ptr<InterruptionWatches> interruptionWatches = std::make_shared<InterruptionWatches>();
    QHash<quint64, std::shared_ptr<std::atomic<bool>>> preparing;
    QHash<QString, QSet<QString>> priorCoreUserReceipts;
    std::deque<std::pair<QString, QString>> recentUserReceipts;
    QHash<QString, std::shared_ptr<HistoryJob>> historyJobs;

    void rememberUser(const QString& session, const QString& user) {
        recentUserReceipts.emplace_back(session, user);
        while (recentUserReceipts.size() > Router::kRememberedInputLimit) recentUserReceipts.pop_front();
    }

    static std::uint64_t nextControllerGeneration() {
        static std::atomic<std::uint64_t> next{1};
        return next.fetch_add(1);
    }
    Runtime(RemoteIMApplication& application, AgentController& agent)
        : app(application), controller(agent), controllerGeneration(nextControllerGeneration()) {}

    std::optional<Router::AccountScope> liveScope() const {
        const auto account = app.client().currentAccount();
        if (!scope || !app.connectionReady() || !account.isValid() ||
            account.ownerUserId != app.chatState().ownerUserId() || account.sdkAppId != scope->sdkAppId ||
            account.ownerUserId.toStdString() != scope->ownerUserId ||
            app.connectionGeneration() != scope->authGeneration)
            return std::nullopt;
        return scope;
    }
    QString workspace(const std::string& session) const {
        MaiSession owner;
        return controller.agent().getSession(session, owner) && owner.isRoot()
                   ? QFileInfo(QString::fromStdString(owner.directory)).canonicalFilePath()
                   : QString();
    }
    bool friendCurrent(const std::string& peer) const {
        const QString id = QString::fromStdString(peer);
        if (!app.acceptedFriendsReady() || !app.isAcceptedFriend(id)) return false;
        for (const auto& contact : app.chatState().contacts())
            if (contact.userId == id) return true;
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
               binding->workspaceKey == input.binding.workspaceKey &&
               workspace(binding->sessionId).toStdString() == binding->workspaceKey;
    }
    bool outputCurrent(const Router::ActiveRoute& route, bool committed = false) const {
        const auto live = liveScope();
        const auto lease = coreLeases.constFind(route.reservation.token);
        return live && route.receipt && lease != coreLeases.constEnd() && lease->observed->valid.load() &&
               (committed || !lease->observed->failed.load()) &&
               friendCurrent(route.reservation.input.binding.peerId) &&
               router.isOutputCurrent(route.reservation.token, *route.receipt, *live,
                                      workspace(route.reservation.input.binding.sessionId).toStdString());
    }
    bool sameHistory(const std::shared_ptr<HistoryJob>& job) const {
        return historyJobs.value(QString::fromStdString(job->binding.peerId)) == job;
    }
    bool currentHistory(const std::shared_ptr<HistoryJob>& job) const {
        const auto live = liveScope();
        if (!live || *live != job->binding.account || job->canceled->load() || !friendCurrent(job->binding.peerId))
            return false;
        const auto binding = router.bindingForPeer(*live, job->binding.peerId);
        if (!binding || binding->epoch != job->binding.epoch || binding->sessionId != job->binding.sessionId ||
            workspace(binding->sessionId).toStdString() != binding->workspaceKey)
            return false;
        const auto source = app.imHistorySource(QString::fromStdString(binding->peerId));
        return source && source->account == job->source.account && source->databasePath == job->source.databasePath &&
               source->connectionGeneration == job->source.connectionGeneration &&
               source->rawGeneration == job->source.rawGeneration;
    }
    void release(CoreLease& lease) {
        {
            std::lock_guard<std::mutex> lock(interruptionWatches->mutex);
            auto found = interruptionWatches->sessions.find(lease.origin.binding.sessionId);
            if (found != interruptionWatches->sessions.end() && found->second.lock() == lease.observed)
                interruptionWatches->sessions.erase(found);
        }
        const QPointer<RemoteIMApplication> approvalHost(&app);
        const auto approvalScope = lease.origin.binding.account;
        const auto approvalPeer = QString::fromStdString(lease.origin.binding.peerId);
        for (const auto& approval : lease.approvals) {
            if (approval.terminalDispatched) continue;
            const RemoteIMApprovalDecision resolution{approval.publicToken, RemoteIMApprovalAction::Resolved};
            QTimer::singleShot(0, &app, [approvalHost, approvalScope, approvalPeer, resolution] {
                const auto permitted = [approvalHost, approvalScope, approvalPeer] {
                    if (!approvalHost || !approvalHost->connectionReady() ||
                        approvalHost->connectionGeneration() != approvalScope.authGeneration ||
                        !approvalHost->isAcceptedFriend(approvalPeer))
                        return false;
                    const auto account = approvalHost->client().currentAccount();
                    return account.sdkAppId == approvalScope.sdkAppId &&
                           account.ownerUserId.toStdString() == approvalScope.ownerUserId;
                };
                if (permitted())
                    approvalHost->sendMachineApprovalResolvedTo(approvalPeer, QStringLiteral("审批已处理。"),
                                                                resolution, {}, permitted);
            });
        }
        if (lease.activity.active) {
            lease.activity.active = false;
            ++lease.activity.sequence;
            const QPointer<RemoteIMApplication> host(&app);
            const auto scope = lease.origin.binding.account;
            const auto peer = QString::fromStdString(lease.origin.binding.peerId);
            const auto signal = lease.activity;
            // 清理不能在迭代 ledger 时进入 SDK/Qt 回调；终止仅清同一 activity ID，不能清掉后来的任务。
            QTimer::singleShot(0, &app, [host, scope, peer, signal] {
                if (!host || !host->connectionReady() || host->connectionGeneration() != scope.authGeneration ||
                    !host->isAcceptedFriend(peer))
                    return;
                const auto account = host->client().currentAccount();
                if (account.sdkAppId == scope.sdkAppId && account.ownerUserId.toStdString() == scope.ownerUserId)
                    host->client().sendActivity(peer, signal, {});
            });
        }
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
        if (ready)
            syncScope();
        else
            suspend();
    });
    connect(&app, &RemoteIMApplication::stateChanged, this, &DesktopSessionImBridge::pruneSessions);
    connect(
        &controller, &AgentController::turnInterrupted, this,
        [watches = runtime_->interruptionWatches](const QString& session, const QString& turn) {
            std::shared_ptr<InputReceipt> observed;
            {
                std::lock_guard<std::mutex> lock(watches->mutex);
                const auto found = watches->sessions.find(session.toStdString());
                if (found != watches->sessions.end()) observed = found->second.lock();
            }
            if (!observed) return;
            std::lock_guard<std::mutex> lock(observed->mutex);
            const auto exact = turn.toStdString();
            if (observed->turnId == exact ||
                (observed->turnId.empty() &&
                 std::find(observed->ids.begin(), observed->ids.end(), exact) != observed->ids.end()))
                observed->valid = false;
        },
        Qt::DirectConnection);
    connect(
        &controller, &AgentController::turnInterrupted, this,
        [this](const QString& session, const QString& turn) {
            const auto scope = runtime_->liveScope();
            if (!scope) return;
            for (const auto& route : runtime_->router.snapshot(*scope).active)
                if (route.reservation.input.binding.sessionId == session.toStdString() && route.receipt &&
                    route.receipt->turnId == turn.toStdString()) {
                    localTakeover(session);
                    return;
                }
        },
        Qt::QueuedConnection);
    connect(&controller, &AgentController::turnFinished, this, [this](const QString& session) {
        const QPointer<DesktopSessionImBridge> lifetime(this);
        publishCommittedOutput(session);
        if (!lifetime) return;
        finish(session);
        if (!lifetime) return;
        const auto jobs = runtime_->historyJobs;
        for (auto it = jobs.cbegin(); it != jobs.cend(); ++it)
            if (it.value()->binding.sessionId == session.toStdString()) advanceHistory(it.key());
    });
    connect(&controller, &AgentController::userMessageAdded, this, [this](const QString& session, const QString& user) {
        if (!runtime_->scope) return;
        for (const auto& route : runtime_->router.snapshot(*runtime_->scope).active)
            if (route.reservation.input.binding.sessionId == session.toStdString() && route.receipt &&
                (route.receipt->userMessageId == user.toStdString() ||
                 std::any_of(route.followups.begin(), route.followups.end(), [&](const Router::AcceptedInput& input) {
                     return input.userMessageId == user.toStdString();
                 })))
                return;
        if (runtime_->priorCoreUserReceipts.value(session).contains(user)) return;
        if (std::find(runtime_->recentUserReceipts.begin(), runtime_->recentUserReceipts.end(),
                      std::make_pair(session, user)) != runtime_->recentUserReceipts.end())
            return;
        localTakeover(session);
    });
    connect(&controller, &AgentController::textCommitted, this,
            [this](const QString& session, const QString&, const QString&, const QString&) {
                publishCommittedOutput(session);
            });
    connect(&controller, &AgentController::steeringApplied, this,
            [this](const QString& session, const QString&, const QString&, const QStringList&) {
                publishCommittedOutput(session);
            });
    connect(&controller, &AgentController::textDelta, this,
            [this](const QString& session, const QString&, const QString&, const QString&) {
                setActivity(session, int(RemoteIMActivityKind::MachineWorking));
            });
    connect(&controller, &AgentController::reasoningDelta, this,
            [this](const QString& session, const QString&, const QString&, const QString&) {
                setActivity(session, int(RemoteIMActivityKind::MachineThinking));
            });
    connect(&controller, &AgentController::toolPartChanged, this,
            [this](const QString& session, const QString& messageId, const QString& partId) {
                MaiMessage message;
                if (!runtime_->controller.agent().getMessage(session.toStdString(), messageId.toStdString(), message))
                    return;
                for (const auto& part : message.parts)
                    if (part.id == partId.toStdString())
                        if (const auto* tool = std::get_if<MaiToolPart>(&part.body)) {
                            setActivity(
                                session,
                                int(tool->state == MaiToolState::Pending   ? RemoteIMActivityKind::MachineWaiting
                                    : tool->state == MaiToolState::Running ? RemoteIMActivityKind::MachineTool
                                                                           : RemoteIMActivityKind::MachineWorking));
                            break;
                        }
            });
    connect(&controller, &AgentController::questionAsked, this,
            [this](const QString& question, const QString& session, const QString&) {
                publishQuestion(question, session);
            });
    connect(&controller, &AgentController::permissionAsked, this, &DesktopSessionImBridge::publishApproval);
    connect(&controller, &AgentController::permissionReplied, this, &DesktopSessionImBridge::resolveApproval);
    connect(&controller, &AgentController::questionAnswered, this,
            [this](const QString& question, const QString& session) {
                for (auto it = runtime_->coreLeases.begin(); it != runtime_->coreLeases.end(); ++it)
                    if (it->origin.binding.sessionId == session.toStdString() && it->question &&
                        it->question->id == question.toStdString())
                        it->question.reset();
                setActivity(session, int(RemoteIMActivityKind::MachineWorking));
            });
    auto* activity = new QTimer(this);
    activity->setObjectName(QStringLiteral("sessionImActivityHeartbeat"));
    activity->setInterval(2000);
    connect(activity, &QTimer::timeout, this, &DesktopSessionImBridge::heartbeatActivities);
    activity->start();
    connect(&controller, &AgentController::sessionStructureChanged, this, &DesktopSessionImBridge::pruneSessions);
    syncScope();
}

DesktopSessionImBridge::~DesktopSessionImBridge() {
    suspend();
}

void DesktopSessionImBridge::syncScope() {
    const auto account = runtime_->app.client().currentAccount();
    if (!runtime_->app.connectionReady() || !account.isValid() ||
        account.ownerUserId != runtime_->app.chatState().ownerUserId()) {
        suspend();
        return;
    }
    const Router::AccountScope next{account.sdkAppId, account.ownerUserId.toStdString(),
                                    runtime_->app.connectionGeneration(), runtime_->controllerGeneration};
    const bool changed = !runtime_->scope || *runtime_->scope != next;
    if (runtime_->router.activate(next) != Router::Status::Ok) return;
    runtime_->scope = next;
    if (changed) {
        runtime_->priorCoreUserReceipts.clear();
        runtime_->recentUserReceipts.clear();
    }
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
        const auto binding =
            runtime_->scope ? runtime_->router.bindingForPeer(*runtime_->scope, job->binding.peerId) : std::nullopt;
        if (binding && binding->account == job->binding.account && binding->epoch == job->binding.epoch) {
            ++it;
            continue;
        }
        job->canceled->store(true);
        if (job->deadline) {
            job->deadline->stop();
            job->deadline->deleteLater();
        }
        it = runtime_->historyJobs.erase(it);
    }
    for (auto it = runtime_->preparing.begin(); it != runtime_->preparing.end();) {
        if (inputs.contains(it.key())) {
            ++it;
            continue;
        }
        it.value()->store(true);
        it = runtime_->preparing.erase(it);
    }
    for (auto it = runtime_->coreLeases.begin(); it != runtime_->coreLeases.end();) {
        if (tokens.contains(it.key())) {
            ++it;
            continue;
        }
        runtime_->release(it.value());
        it = runtime_->coreLeases.erase(it);
    }
    for (auto it = runtime_->payloads.begin(); it != runtime_->payloads.end();) {
        if (inputs.contains(it.key())) {
            ++it;
            continue;
        }
        if (it->readinessDeadline) it->readinessDeadline->deleteLater();
        it = runtime_->payloads.erase(it);
    }
    for (auto it = runtime_->priorCoreUserReceipts.begin(); it != runtime_->priorCoreUserReceipts.end();)
        if (!sessions.contains(it.key()))
            it = runtime_->priorCoreUserReceipts.erase(it);
        else
            ++it;
}

bool DesktopSessionImBridge::bind(const QString& peerId, const QString& sessionId, QString* error) {
    const auto reject = [error](const QString& text) {
        if (error) *error = text;
        return false;
    };
    const auto scope = runtime_->liveScope();
    const QString peer = peerId.trimmed();
    if (!scope || !runtime_->friendCurrent(peer.toStdString()))
        return reject(QStringLiteral("\u8bf7\u9009\u62e9\u5f53\u524d\u8d26\u53f7\u7684\u597d\u53cb\u4f1a\u8bdd\u3002"));
    const QString workspace = runtime_->workspace(sessionId.toStdString());
    if (workspace.isEmpty())
        return reject(QStringLiteral(
            "\u8bf7\u9009\u62e9\u5df2\u6709\u7684\u672c\u673a\u521b\u4f5c\u4e2d\u5fc3\u4f1a\u8bdd\u3002"));
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
        result = runtime_->router.bind(*scope, peer.toStdString(), sessionId.toStdString(), workspace.toStdString(),
                                       true, true);
        if (result != Router::Status::Ok) return;
        const auto binding = runtime_->router.bindingForPeer(*scope, peer.toStdString());
        result = runtime_->router.beginHistory(*scope, peer.toStdString(), binding->epoch);
        if (result == Router::Status::Ok) {
            job->binding = *runtime_->router.bindingForPeer(*scope, peer.toStdString());
            job->boundary = boundary;
        }
    });
    if (result != Router::Status::Ok)
        return reject(
            result == Router::Status::Conflict
                ? QStringLiteral("\u8fd9\u4e2a\u521b\u4f5c\u4e2d\u5fc3\u4f1a\u8bdd\u5df2\u7ed1\u5b9a\u5176\u4ed6\u597d"
                                 "\u53cb\uff0c\u8bf7\u5148\u89e3\u9664\u539f\u7ed1\u5b9a\u3002")
                : QStringLiteral(
                      "\u5f53\u524d\u8d26\u53f7\u6216\u4f1a\u8bdd\u7ed1\u5b9a\u5df2\u4e0d\u53ef\u7528\u3002"));
    if (error) error->clear();
    if (const auto previous = runtime_->historyJobs.value(peer)) {
        previous->canceled->store(true);
        if (previous->deadline) {
            previous->deadline->stop();
            previous->deadline->deleteLater();
        }
    }
    runtime_->historyJobs.insert(peer, job);
    auto* deadline = new QTimer(this);
    job->deadline = deadline;
    deadline->setObjectName(QStringLiteral("sessionImHistoryDeadline"));
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, this, [this, weak = std::weak_ptr<HistoryJob>(job), peer] {
        const auto current = weak.lock();
        if (current && runtime_->sameHistory(current))
            abortHistory(peer, QStringLiteral("准备完整聊天历史超时，绑定未完成。"));
    });
    deadline->start(300000);
    QSet<QString> prior;
    for (const auto& message :
         runtime_->controller.agent().listMessagesPage(sessionId.toStdString(), "", Router::kRememberedInputLimit))
        if (message.role == MaiRole::User) prior.insert(QString::fromStdString(message.id));
    runtime_->priorCoreUserReceipts.insert(sessionId, prior);
    prunePlatformWork();
    const QPointer<DesktopSessionImBridge> lifetime(this);
    emit bindingChanged(peer);
    if (!lifetime || !runtime_->sameHistory(job)) return true;
    runtime_->app.awaitIMHistoryMetadataFence(job->source, job->canceled,
                                              [lifetime, job, peer](bool ok, const QString& failure) {
                                                  if (!lifetime || !lifetime->runtime_->sameHistory(job)) return;
                                                  if (!ok) {
                                                      lifetime->abortHistory(peer, failure);
                                                      return;
                                                  }
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
    if (job->deadline) {
        job->deadline->stop();
        job->deadline->deleteLater();
    }
    runtime_->historyJobs.remove(peer);
    prunePlatformWork();
    const QPointer<DesktopSessionImBridge> lifetime(this);
    emit bindingChanged(peer);
    if (lifetime && !failure.isEmpty()) emit relayError(peer, failure);
}

void DesktopSessionImBridge::advanceHistory(const QString& peer) {
    const auto job = runtime_->historyJobs.value(peer);
    if (!job || job->inFlight || !job->fenceReady) return;
    if (!runtime_->currentHistory(job)) {
        abortHistory(peer, QStringLiteral("账号、会话或工作目录已改变，历史导入没有完成。"));
        return;
    }
    const QPointer<DesktopSessionImBridge> lifetime(this);
    const auto current = [lifetime, job] { return lifetime && lifetime->runtime_->sameHistory(job); };
    const auto again = [lifetime, peer] {
        if (lifetime) lifetime->advanceHistory(peer);
    };
    if (!job->reader && !job->eof) {
        job->inFlight = true;
        MaiIMHistorySnapshotOptions options;
        options.databasePath = job->source.databasePath.toStdString();
        options.profile = MaiIMHistoryProfile::Desktop;
        options.ownerUserId = job->binding.account.ownerUserId;
        options.peerUserId = job->binding.peerId;
        options.canceled = job->canceled;
        auto* mainThread = QThread::currentThread();
        historyWork<HistoryOpenResult>(
            this, QStringLiteral("sessionImHistoryOpen"),
            [job, options, mainThread] {
                HistoryOpenResult result;
                if (job->canceled->load()) {
                    result.error = {MaiErrorCode::Canceled, "History import canceled"};
                    return result;
                }
                int count = historyReaderAdmissions().load();
                while (count < 2 && !historyReaderAdmissions().compare_exchange_weak(count, count + 1)) {
                }
                if (count >= 2) {
                    result.capacity = true;
                    return result;
                }
                auto opened = MaiIMHistorySnapshot::open(options);
                if (!opened) {
                    historyReaderAdmissions().fetch_sub(1);
                    result.error = opened.error();
                    return result;
                }
                result.reader = std::shared_ptr<MaiIMHistorySnapshot>(opened.value().release(),
                                                                      [mainThread](MaiIMHistorySnapshot* reader) {
                                                                          const auto close = [reader] {
                                                                              delete reader;
                                                                              historyReaderAdmissions().fetch_sub(1);
                                                                          };
                                                                          if (QThread::currentThread() == mainThread)
                                                                              attachmentPreparationPool()->start(close);
                                                                          else
                                                                              close();
                                                                      });
                return result;
            },
            [this, job, peer, current, again](HistoryOpenResult opened) {
                if (!current()) return;
                job->inFlight = false;
                if (opened.capacity) {
                    QTimer::singleShot(100, this, [this, job, peer] {
                        if (runtime_->sameHistory(job)) advanceHistory(peer);
                    });
                    return;
                }
                if (opened.error || !opened.reader) {
                    abortHistory(peer, QStringLiteral("无法固定当前账号的完整本地聊天快照，绑定没有完成。"));
                    return;
                }
                job->reader = std::move(opened.reader);
                job->descriptor = job->reader->source();
                again();
            });
        return;
    }
    const auto importId = job->stageId();
    if (importId.empty()) {
        const auto identity =
            QJsonDocument(QJsonArray{QStringLiteral("desktop"), QString::number(job->binding.account.sdkAppId),
                                     QString::fromStdString(job->binding.account.ownerUserId), peer})
                .toJson(QJsonDocument::Compact)
                .toStdString();
        const auto sourceKey = "im:" + maiContextImportBatchDigest({{"logical-source", identity, 0}});
        const MaiContextImportSpec spec{job->binding.sessionId, sourceKey, job->snapshotId.toStdString(),
                                        "Quoted local IM history"};
        job->inFlight = true;
        historyWork<HistoryCoreResult>(
            this, QStringLiteral("sessionImHistoryBegin"),
            [job, spec] {
                QElapsedTimer timer;
                timer.start();
                if (job->canceled->load())
                    return HistoryCoreResult{{MaiErrorCode::Canceled, "History import canceled"}, timer.elapsed()};
                auto result = job->worker.beginContextImport(spec);
                if (result) {
                    std::lock_guard<std::mutex> lock(job->stageMutex);
                    job->importId = result.value().importId;
                }
                if (result && job->canceled->load()) job->worker.abortContextImport(result.value().importId);
                return HistoryCoreResult{std::move(result), timer.elapsed()};
            },
            [this, job, peer, current, again](HistoryCoreResult result) {
                if (!current()) return;
                job->inFlight = false;
                job->maximumStepMillis = std::max(job->maximumStepMillis, result.milliseconds);
                if (!result.value) {
                    abortHistory(peer, QStringLiteral("无法暂存完整聊天历史，绑定没有完成。"));
                    return;
                }
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
            bytes += length;
            batch.push_back(record);
        }
        const auto count = batch.size();
        const auto sequence = job->sequence;
        job->inFlight = true;
        historyWork<HistoryCoreResult>(
            this, QStringLiteral("sessionImHistoryAppend"),
            [job, importId, sequence, batch = std::move(batch)] {
                QElapsedTimer timer;
                timer.start();
                if (job->canceled->load())
                    return HistoryCoreResult{{MaiErrorCode::Canceled, "History import canceled"}, timer.elapsed()};
                auto result = job->worker.appendContextImport(importId, sequence, batch);
                if (job->canceled->load()) job->worker.abortContextImport(importId);
                return HistoryCoreResult{std::move(result), timer.elapsed()};
            },
            [this, job, peer, count, bytes, current, again](HistoryCoreResult result) {
                if (!current()) return;
                job->inFlight = false;
                job->maximumStepMillis = std::max(job->maximumStepMillis, result.milliseconds);
                if (!result.value) {
                    abortHistory(peer, QStringLiteral("聊天历史暂存失败，未启用自动回复。"));
                    return;
                }
                job->sequence = result.value.value().nextSequence;
                job->imported = result.value.value().recordCount;
                job->formattedOffset += count;
                qInfo() << "[session-im] history stage rows=" << count << "bytes=" << bytes
                        << "worker_ms=" << result.milliseconds;
                QTimer::singleShot(0, this, again);
            });
        return;
    }
    job->formatted.clear();
    job->formattedOffset = 0;
    if (job->page && job->offset < job->page->records.size()) {
        std::vector<MaiIMHistoryRecord> selected;
        std::size_t bytes = 0;
        while (job->offset < job->page->records.size() && selected.size() < 32) {
            const auto& row = job->page->records[job->offset];
            const auto match =
                runtime_->router.historyMatch(job->binding.account, job->binding.peerId, job->binding.epoch, row.rowId,
                                              row.remoteId.value_or(""), row.fromUserId, row.toUserId);
            if (match == Router::HistoryMatch::Stale) {
                abortHistory(peer, QStringLiteral("历史导入的原绑定已改变。"));
                return;
            }
            if (match == Router::HistoryMatch::AwaitingPersistence) break;
            if (match == Router::HistoryMatch::LiveInput) {
                ++job->offset;
                continue;
            }
            if (!selected.empty() && bytes + row.text.size() > 64 * 1024) break;
            bytes += row.text.size();
            selected.push_back(row);
            ++job->offset;
        }
        if (selected.empty()) {
            if (job->offset == job->page->records.size()) QTimer::singleShot(0, this, again);
            return;
        }
        job->inFlight = true;
        historyWork<MaiResult<std::vector<MaiContextImportRecord>>>(
            this, QStringLiteral("sessionImHistoryFormat"),
            [job, selected = std::move(selected)]() -> MaiResult<std::vector<MaiContextImportRecord>> {
                std::vector<MaiContextImportRecord> formatted;
                for (const auto& row : selected) {
                    if (job->canceled->load()) return {MaiErrorCode::Canceled, "History import canceled"};
                    auto record = maiFormatIMHistoryContextRecord(row, job->descriptor);
                    if (!record) return record.error();
                    formatted.push_back(std::move(record.value()));
                }
                return formatted;
            },
            [this, job, peer, current, again](MaiResult<std::vector<MaiContextImportRecord>> result) {
                if (!current()) return;
                job->inFlight = false;
                if (!result) {
                    abortHistory(peer, QStringLiteral("一条聊天历史无法完整安全地导入，绑定没有完成。"));
                    return;
                }
                job->formatted = std::move(result.value());
                again();
            });
        return;
    }
    if (job->page) {
        if (!job->page->hasMore) {
            job->eof = true;
            job->reader.reset();
        }
        job->page.reset();
        job->offset = 0;
    }
    if (!job->eof) {
        const auto reader = job->reader;
        job->inFlight = true;
        historyWork<MaiResult<MaiIMHistoryPage>>(
            this, QStringLiteral("sessionImHistoryRead"),
            [job, reader]() -> MaiResult<MaiIMHistoryPage> {
                if (job->canceled->load()) return {MaiErrorCode::Canceled, "History import canceled"};
                return reader->read();
            },
            [this, job, peer, current, again](MaiResult<MaiIMHistoryPage> page) {
                if (!current()) return;
                job->inFlight = false;
                if (!page) {
                    abortHistory(peer, QStringLiteral("本地聊天快照读取失败或超时，未发布部分历史。"));
                    return;
                }
                job->page = std::move(page.value());
                job->offset = 0;
                again();
            });
        return;
    }
    job->inFlight = true;
    historyWork<HistoryCoreResult>(
        this, QStringLiteral("sessionImHistoryCommit"),
        [job, importId] {
            QElapsedTimer timer;
            timer.start();
            if (job->canceled->load())
                return HistoryCoreResult{{MaiErrorCode::Canceled, "History import canceled"}, timer.elapsed()};
            auto result = job->worker.commitContextImport(importId);
            if (result) job->committed = true;
            return HistoryCoreResult{std::move(result), timer.elapsed()};
        },
        [this, job, peer, current](HistoryCoreResult result) {
            if (!current()) return;
            job->inFlight = false;
            job->maximumStepMillis = std::max(job->maximumStepMillis, result.milliseconds);
            if (!result.value && result.value.error().code() == MaiErrorCode::Busy) {
                if (!runtime_->controller.agent().isBusy(job->binding.sessionId))
                    QTimer::singleShot(0, this, [this, job, peer] {
                        if (runtime_->sameHistory(job)) advanceHistory(peer);
                    });
                return;
            }
            if (!result.value || !runtime_->currentHistory(job) ||
                runtime_->router.completeHistory(job->binding.account, job->binding.peerId, job->binding.epoch) !=
                    Router::Status::Ok) {
                abortHistory(peer, QStringLiteral("历史提交未确认，未启用自动回复。"));
                return;
            }
            if (job->deadline) {
                job->deadline->stop();
                job->deadline->deleteLater();
            }
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
    if (scope)
        for (const auto& binding : runtime_->router.snapshot(*scope).bindings)
            result.insert(QString::fromStdString(binding.peerId), QString::fromStdString(binding.sessionId));
    return result;
}

QHash<QString, QString> DesktopSessionImBridge::readyBindings() const {
    QHash<QString, QString> result;
    const auto scope = runtime_->liveScope();
    if (scope)
        for (const auto& binding : runtime_->router.snapshot(*scope).bindings)
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
    for (const auto& peer : peers) {
        emit bindingChanged(peer);
        if (!lifetime) return;
    }
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
        if (!runtime_->friendCurrent(binding.peerId))
            unbind(QString::fromStdString(binding.peerId));
        else if (runtime_->workspace(binding.sessionId).toStdString() != binding.workspaceKey) {
            emit relayError(QString::fromStdString(binding.peerId),
                            QStringLiteral("会话工作目录已改变，没有自动发送这轮回复，请重新确认绑定。"));
            if (!lifetime) return;
            localTakeover(QString::fromStdString(binding.sessionId));
        }
        if (!lifetime) return;
    }
}

void DesktopSessionImBridge::acceptLive(quint64 ingressId, const RemoteIMMessage& message, quint64 rawSequence,
                                        quint64 rawGeneration) {
    const auto scope = runtime_->liveScope();
    if (!scope || ingressId == 0) return;
    const QString peer = message.fromUserId;
    const auto binding = runtime_->router.bindingForPeer(*scope, peer.toStdString());
    if (!binding) return;
    if (message.hasApprovalDecision) {
        receiveApprovalDecision(message);
        return;
    }
    const auto history = runtime_->historyJobs.value(peer);
    if (history && rawSequence && rawGeneration == history->boundary.generation &&
        rawSequence <= history->boundary.sequence)
        return;  // Raw pre-publication packets are prior context, never new work.
    const bool attachment = message.hasImage || message.hasVideo || message.hasVoice || message.hasFile;
    QString payloadError;
    if (!attachment && message.text.trimmed().isEmpty())
        payloadError = QStringLiteral(
            "\u8fd9\u6761\u9644\u4ef6\u8fd8\u6ca1\u6709\u53ef\u8bfb\u53d6\u7684\u672c\u5730\u7f13\u5b58\uff0c\u6ca1"
            "\u6709\u63d0\u4ea4\u7ed9\u521b\u4f5c\u4e2d\u5fc3\u3002");
    else if (message.text.size() > kMaximumInputBytes || message.text.toUtf8().size() > kMaximumInputBytes)
        payloadError = QStringLiteral(
            "\u8fd9\u6761\u6587\u5b57\u8d85\u8fc7\u4f1a\u8bdd\u8f93\u5165\u4e0a\u9650\uff0c\u6ca1\u6709\u63d0\u4ea4"
            "\u7ed9\u521b\u4f5c\u4e2d\u5fc3\u3002");
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
    incoming.origin = message.origin == RemoteIMMessageOrigin::Human     ? Router::Origin::Human
                      : message.origin == RemoteIMMessageOrigin::Machine ? Router::Origin::Machine
                                                                         : Router::Origin::Unknown;
    incoming.live = message.direction == RemoteIMMessageDirection::Incoming;
    incoming.controlFrame = message.hasApprovalRequest || message.hasApprovalDecision;
    incoming.hasContent = payloadError.isEmpty();
    incoming.requirePersistence = true;
    const auto admitted = runtime_->router.accept(incoming);
    if (admitted.status != Router::Status::Ok || !admitted.input) {
        if (admitted.status == Router::Status::QueueFull) {
            if (binding->historyPending)
                abortHistory(peer,
                             QStringLiteral("历史准备期间的新消息已超过32条，绑定没有完成；原始聊天记录仍保留。"));
            else
                emit relayError(peer,
                                QStringLiteral("\u5f85\u5904\u7406\u7684\u4f1a\u8bdd\u6d88\u606f\u5df2\u8fbe 32 "
                                               "\u6761\uff1b\u8fd9\u6761\u6d88\u606f\u6ca1\u6709\u63d0\u4ea4\u3002"));
        } else if (!payloadError.isEmpty() && message.origin == RemoteIMMessageOrigin::Human &&
                   runtime_->router.bindingForPeer(*scope, peer.toStdString())) {
            if (binding->historyPending)
                abortHistory(peer, payloadError);
            else
                emit relayError(peer, payloadError);
        }
        return;
    }
    Runtime::Payload payload;
    payload.input = *admitted.input;
    payload.ingressId = ingressId;
    payload.text = message.text;
    for (auto it = runtime_->coreLeases.cbegin(); it != runtime_->coreLeases.cend(); ++it)
        if (it->origin.binding.sessionId == payload.input.binding.sessionId && it->question)
            payload.questionId = it->question->id;
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

void DesktopSessionImBridge::persistedLive(quint64 ingress, bool ok, bool inserted, const QString& rowId,
                                           const QString&) {
    for (const auto& payload : runtime_->payloads) {
        if (payload.ingressId != ingress) continue;
        const auto input = payload.input;
        const auto peer = QString::fromStdString(input.binding.peerId);
        if (!runtime_->bindingCurrent(input)) {
            prunePlatformWork();
            return;
        }
        if (!ok) {
            if (historyPending(peer))
                abortHistory(peer, QStringLiteral("原始消息未能完整保存，历史准备和绑定已取消。"));
            else {
                runtime_->router.discardInput(input.binding.account, input.inputId);
                prunePlatformWork();
                emit relayError(peer, QStringLiteral("原始消息未能保存到本地，没有提交给创作中心。"));
            }
            return;
        }
        const auto result =
            runtime_->router.confirmPersisted(input.binding.account, input.inputId, inserted, rowId.toStdString());
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
        if (!runtime_->bindingCurrent(input)) {
            prunePlatformWork();
            return;
        }
        const auto& original = it->source;
        if (message.id != original.id || message.fromUserId != original.fromUserId ||
            message.toUserId != original.toUserId || message.origin != original.origin ||
            message.direction != original.direction || message.hasImage != original.hasImage ||
            message.hasVideo != original.hasVideo || message.hasFile != original.hasFile ||
            message.hasVoice != original.hasVoice)
            return;
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
        if (!runtime_->bindingCurrent(input)) {
            prunePlatformWork();
            return;
        }
        if (historyPending(QString::fromStdString(input.binding.peerId))) {
            abortHistory(QString::fromStdString(input.binding.peerId), error);
            return;
        }
        if (runtime_->router.discardInput(input.binding.account, input.inputId) != Router::Status::Ok) return;
        prunePlatformWork();
        QPointer<DesktopSessionImBridge> lifetime(this);
        emit relayError(QString::fromStdString(input.binding.peerId), error);
        if (lifetime)
            QTimer::singleShot(
                0, this, [this, session = QString::fromStdString(input.binding.sessionId)] { dispatch(session); });
        return;
    }
}

void DesktopSessionImBridge::dispatch(const QString& session) {
    QPointer<DesktopSessionImBridge> lifetime(this);
    const auto scope = runtime_->liveScope();
    if (!scope) return;
    for (const auto& active : runtime_->router.snapshot(*scope).active)
        if (active.reservation.input.binding.sessionId == session.toStdString()) {
            if (active.phase == Router::Phase::Running)
                processFollowup(active.reservation.token);
            else if (active.phase == Router::Phase::Reserved)
                processReservation(active.reservation.token);
            return;
        }
    const QString workspace = runtime_->workspace(session.toStdString());
    const auto reservation = runtime_->router.reserveNext(*scope, session.toStdString(), workspace.toStdString(),
                                                          !runtime_->controller.agent().isBusy(session.toStdString()));
    if (reservation.status == Router::Status::Ok && reservation.reservation) {
        processReservation(reservation.reservation->token);
    } else if (reservation.status != Router::Status::Busy && reservation.status != Router::Status::NotBound) {
        for (const auto& binding : runtime_->router.snapshot(*scope).bindings)
            if (binding.sessionId == session.toStdString() && binding.workspaceKey != workspace.toStdString()) {
                emit relayError(QString::fromStdString(binding.peerId),
                                QStringLiteral("\u4f1a\u8bdd\u5de5\u4f5c\u76ee\u5f55\u5df2\u6539\u53d8\uff0c\u8bf7"
                                               "\u91cd\u65b0\u786e\u8ba4\u7ed1\u5b9a\u3002"));
                if (lifetime) localTakeover(session);
                return;
            }
    }
    if (lifetime) prunePlatformWork();
}

void DesktopSessionImBridge::preparePayload(quint64 id) {
    auto payload = runtime_->payloads.find(id);
    if (payload == runtime_->payloads.end() || !payload->ready || payload->prepared || runtime_->preparing.contains(id))
        return;
    const auto input = payload->input;
    const auto peer = QString::fromStdString(input.binding.peerId);
    const auto session = QString::fromStdString(input.binding.sessionId);
    const auto fail = [this, input, peer, session](const QString& error) {
        const auto scope = runtime_->liveScope();
        const auto binding = scope ? runtime_->router.bindingForPeer(*scope, input.binding.peerId) : std::nullopt;
        if (!binding || binding->account != input.binding.account || binding->epoch != input.binding.epoch) {
            prunePlatformWork();
            return;
        }
        const QPointer<DesktopSessionImBridge> lifetime(this);
        if (runtime_->workspace(input.binding.sessionId).toStdString() != input.binding.workspaceKey) {
            emit relayError(peer, QStringLiteral("会话工作目录已改变，这条消息没有提交，请重新确认绑定。"));
            if (lifetime) localTakeover(session);
            return;
        }
        if (historyPending(peer)) {
            abortHistory(peer, error);
            return;
        }
        runtime_->router.discardInput(*scope, input.inputId);
        prunePlatformWork();
        emit relayError(peer, error);
        if (lifetime) QTimer::singleShot(0, this, [this, session] { dispatch(session); });
    };
    if (attachmentPreparationAdmissions().load() >= kPreparationAdmissionLimit) {
        fail(QStringLiteral("附件准备队列已满，这条消息没有提交。"));
        return;
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
    connect(
        watcher, &QFutureWatcher<PreparedInput>::finished, this,
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
            if (historyPending(peer)) {
                advanceHistory(peer);
                return;
            }
            for (const auto& route : runtime_->router.snapshot(input.binding.account).active)
                if (route.reservation.input.inputId == id) {
                    processReservation(route.reservation.token);
                    return;
                }
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
            emit relayError(QString::fromStdString(input.binding.peerId),
                            QStringLiteral("会话工作目录已改变，这条消息没有提交，请重新确认绑定。"));
        if (lifetime) localTakeover(session);
        return;
    }
    if (!payload->ready) return;
    if (!payload->prepared) {
        preparePayload(input.inputId);
        return;
    }
    const Runtime::Payload copied = *payload;
    auto observed = std::make_shared<InputReceipt>();
    MaiSession target;
    if (!runtime_->controller.agent().getSession(input.binding.sessionId, target) ||
        QFileInfo(QString::fromStdString(target.directory)).canonicalFilePath().toStdString() !=
            input.binding.workspaceKey) {
        emit relayError(QString::fromStdString(input.binding.peerId),
                        QStringLiteral("会话工作目录已改变，请重新确认绑定。"));
        if (lifetime) localTakeover(session);
        return;
    }
    observed->session = input.binding.sessionId;
    {
        std::lock_guard<std::mutex> lock(runtime_->interruptionWatches->mutex);
        runtime_->interruptionWatches->sessions[input.binding.sessionId] = observed;
    }
    const auto subscription = runtime_->controller.agent().eventBus().subscribe(
        [observed](const MaiEvent& event) { observeInput(observed, event); });
    MaiSendPrompt prompt{input.binding.sessionId, copied.text.toStdString(), copied.images, copied.videos};
    prompt.expectedDirectory = target.directory;
    const auto accepted = runtime_->controller.agent().submit(prompt);
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(observed->mutex);
        observed->capturing = false;
        ids = observed->ids;
    }
    if (!accepted) {
        observed->valid = false;
        runtime_->controller.agent().eventBus().unsubscribe(subscription);
        runtime_->router.releaseReservation(token, *scope, input.binding.workspaceKey,
                                            accepted.error().code() == MaiErrorCode::Busy);
        prunePlatformWork();
        if (accepted.error().code() != MaiErrorCode::Busy) {
            emit relayError(QString::fromStdString(input.binding.peerId),
                            QStringLiteral("\u8fd9\u6761\u6d88\u606f\u672a\u80fd\u63d0\u4ea4\u7ed9\u521b\u4f5c\u4e2d"
                                           "\u5fc3\uff0c\u8bf7\u5728\u672c\u673a\u67e5\u770b\u4f1a\u8bdd\u3002"));
            if (lifetime) QTimer::singleShot(0, this, [this, session] { dispatch(session); });
        }
        return;
    }
    std::string user;
    for (const auto& id : ids) {
        MaiMessage message;
        if (runtime_->controller.agent().getMessage(input.binding.sessionId, id, message) &&
            message.role == MaiRole::User)
            user = id;
    }
    const Router::TurnReceipt receipt{user, accepted.value(), accepted.value()};
    if (!observed->valid || user.empty() || std::find(ids.begin(), ids.end(), accepted.value()) == ids.end() ||
        runtime_->router.bindReceipt(
            token, receipt, *scope, runtime_->workspace(input.binding.sessionId).toStdString()) != Router::Status::Ok) {
        observed->valid = false;
        runtime_->controller.agent().eventBus().unsubscribe(subscription);
        localTakeover(session);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(observed->mutex);
        observed->turnId = receipt.turnId;
    }
    Runtime::CoreLease lease;
    lease.observed = observed;
    lease.subscription = subscription;
    lease.origin = input;
    lease.directory = target.directory;
    lease.segments.emplace(receipt.assistantMessageId, receipt);
    lease.activity.activityId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    lease.activity.kind = RemoteIMActivityKind::MachineWorking;
    lease.activity.taskStartedAtMs = QDateTime::currentMSecsSinceEpoch();
    lease.activity.startedAtMs = lease.activity.taskStartedAtMs;
    runtime_->coreLeases.insert(token, std::move(lease));
    runtime_->rememberUser(session, QString::fromStdString(user));
    setActivity(session, int(RemoteIMActivityKind::MachineWorking));
    if (!lifetime) return;
    publishCommittedOutput(session);
}

void DesktopSessionImBridge::processFollowup(quint64 token) {
    const QPointer<DesktopSessionImBridge> lifetime(this);
    const auto route = runtime_->active(token);
    const auto scope = runtime_->liveScope();
    auto lease = runtime_->coreLeases.find(token);
    if (!route || route->phase != Router::Phase::Running || !route->receipt || !scope ||
        lease == runtime_->coreLeases.end() || !lease->observed->valid || !runtime_->current(route->reservation))
        return;
    const auto& binding = route->reservation.input.binding;
    const auto activeTurn = runtime_->controller.agent().activeTurnId(binding.sessionId);
    if (activeTurn.empty()) return;
    if (activeTurn != route->receipt->turnId) {
        localTakeover(QString::fromStdString(binding.sessionId));
        return;
    }
    const auto reserved = runtime_->router.reserveFollowup(token, *scope, binding.workspaceKey);
    if (!reserved.reservation) return;
    const auto input = reserved.reservation->input;
    const auto release = [&] { runtime_->router.releaseFollowup(token, input.inputId, *scope, binding.workspaceKey); };
    const auto payload = runtime_->payloads.constFind(input.inputId);
    if (payload == runtime_->payloads.constEnd() || !payload->ready || !payload->prepared) {
        release();
        if (payload != runtime_->payloads.constEnd() && payload->ready) preparePayload(input.inputId);
        return;
    }
    const Runtime::Payload copied = *payload;
    const auto observed = lease->observed;
    MaiSession target;
    if (!runtime_->controller.agent().getSession(binding.sessionId, target) || target.directory != lease->directory ||
        QFileInfo(QString::fromStdString(target.directory)).canonicalFilePath().toStdString() != binding.workspaceKey) {
        release();
        localTakeover(QString::fromStdString(binding.sessionId));
        return;
    }
    {
        std::lock_guard<std::mutex> lock(observed->mutex);
        observed->admitting = true;
        observed->admissionThread = std::this_thread::get_id();
        observed->admissionIds.clear();
    }
    const std::string request = "im-" + std::to_string(runtime_->controllerGeneration) + "-" +
                                std::to_string(binding.epoch) + "-" + std::to_string(input.inputId);
    const auto accepted = runtime_->controller.agent().submit(MaiSteerPrompt{binding.sessionId,
                                                                             activeTurn,
                                                                             request,
                                                                             copied.text.toStdString(),
                                                                             copied.images,
                                                                             copied.videos,
                                                                             {},
                                                                             target.directory});
    if (!lifetime) return;
    bool receiptKnown = false;
    {
        std::lock_guard<std::mutex> lock(observed->mutex);
        observed->admitting = false;
        receiptKnown = accepted &&
                       std::find(observed->ids.begin(), observed->ids.end(), accepted.value()) != observed->ids.end() &&
                       (observed->admissionIds.empty() ||
                        (observed->admissionIds.size() == 1 && observed->admissionIds.front() == accepted.value()));
    }
    if (!accepted) {
        release();
        if (accepted.error().code() != MaiErrorCode::Busy && accepted.error().code() != MaiErrorCode::NotFound) {
            runtime_->router.discardInput(*scope, input.inputId);
            emit relayError(QString::fromStdString(binding.peerId),
                            QStringLiteral("补充消息未能提交给当前会话，请在本机查看。"));
        }
        return;
    }
    if (!receiptKnown || runtime_->router.commitFollowup(token, input.inputId, accepted.value(), *scope,
                                                         binding.workspaceKey) != Router::Status::Ok) {
        localTakeover(QString::fromStdString(binding.sessionId));
        return;
    }
    runtime_->rememberUser(QString::fromStdString(binding.sessionId), QString::fromStdString(accepted.value()));
    lease = runtime_->coreLeases.find(token);
    if (lease == runtime_->coreLeases.end()) return;
    // 问答和权限分开。先受理同 turn 补充可使剩余旧工具重规划，再唤醒准确的文字问题。
    if (!copied.questionId.empty() && copied.images.empty() && copied.videos.empty()) {
        for (const auto& question : runtime_->controller.agent().listPendingQuestions())
            if (question.id == copied.questionId && question.sessionId == binding.sessionId &&
                lease->segments.count(question.messageId)) {
                runtime_->controller.agent().submit(
                    MaiReplyQuestion{question.id, copied.text.toStdString(), lease->directory});
                break;
            }
    }
    publishCommittedOutput(QString::fromStdString(binding.sessionId));
    if (!lifetime) return;
    QTimer::singleShot(0, this, [this, token] { processFollowup(token); });
}

void DesktopSessionImBridge::publishCommittedOutput(const QString& session) {
    const QPointer<DesktopSessionImBridge> lifetime(this);
    const auto scope = runtime_->liveScope();
    if (!scope) return;
    for (const auto& route : runtime_->router.snapshot(*scope).active) {
        if (route.reservation.input.binding.sessionId != session.toStdString()) continue;
        if (runtime_->workspace(session.toStdString()).toStdString() != route.reservation.input.binding.workspaceKey) {
            emit relayError(QString::fromStdString(route.reservation.input.binding.peerId),
                            QStringLiteral("会话工作目录已改变，没有自动发送这轮回复，请重新确认绑定。"));
            if (lifetime) localTakeover(session);
            return;
        }
        auto lease = runtime_->coreLeases.find(route.reservation.token);
        if (lease == runtime_->coreLeases.end() || !lease->observed->valid) return;
        std::vector<InputReceipt::Text> texts;
        std::vector<InputReceipt::Continuation> continuations;
        {
            std::lock_guard<std::mutex> lock(lease->observed->mutex);
            if (lease->observed->admitting) return;
            texts.swap(lease->observed->committed);
            continuations.swap(lease->observed->continuations);
        }
        for (const auto& continuation : continuations) {
            const auto status = runtime_->router.bindContinuation(route.reservation.token, continuation.turnId,
                                                                  continuation.messageId, continuation.inputs, *scope,
                                                                  route.reservation.input.binding.workspaceKey);
            if (status != Router::Status::Ok && status != Router::Status::Duplicate) {
                localTakeover(session);
                return;
            }
            lease->segments.emplace(
                continuation.messageId,
                Router::TurnReceipt{continuation.inputs.back(), continuation.messageId, continuation.turnId});
        }
        for (const auto& text : texts) {
            lease = runtime_->coreLeases.find(route.reservation.token);
            if (lease == runtime_->coreLeases.end()) return;
            const auto receipt = lease->segments.find(text.messageId);
            if (receipt == lease->segments.end() || receipt->second.turnId != text.turnId) {
                localTakeover(session);
                return;
            }
            if (lease->sentParts.count(text.partId)) continue;
            MaiMessage message;
            if (!runtime_->controller.agent().getMessage(session.toStdString(), text.messageId, message) ||
                message.role != MaiRole::Assistant) {
                localTakeover(session);
                return;
            }
            for (const auto& part : message.parts)
                if (part.id == text.partId)
                    if (const auto* value = std::get_if<MaiTextPart>(&part.body)) {
                        lease->sentParts.insert(text.partId);
                        if (!QString::fromStdString(value->text).trimmed().isEmpty())
                            sendTextOutput(route.reservation.token, receipt->second,
                                           QString::fromStdString(value->text), true);
                        if (!lifetime) return;
                        break;
                    }
        }
        return;
    }
}

void DesktopSessionImBridge::sendTextOutput(quint64 token, const Router::TurnReceipt& receipt, const QString& text,
                                            bool committed) {
    const auto route = runtime_->active(token);
    const auto scope = runtime_->liveScope();
    if (!route || !scope) return;
    const auto allocation =
        runtime_->router.appendOutput(token, receipt, *scope, route->reservation.input.binding.workspaceKey, 1);
    if (allocation.status != Router::Status::Ok) return;
    auto lease = runtime_->coreLeases.find(token);
    if (lease == runtime_->coreLeases.end()) return;
    lease->outputs.insert(allocation.firstIndex, Runtime::CoreLease::Output{receipt, text, {}, committed});
    dispatchOutput(token);
}

void DesktopSessionImBridge::dispatchOutput(quint64 token) {
    const auto route = runtime_->active(token);
    auto lease = runtime_->coreLeases.find(token);
    if (!route || lease == runtime_->coreLeases.end() || lease->dispatching || lease->outputs.isEmpty()) return;
    const auto index = lease->outputs.firstKey();
    const auto item = lease->outputs.first();
    lease->dispatching = true;
    const QPointer<DesktopSessionImBridge> lifetime(this);
    auto output = *route;
    output.receipt = item.receipt;
    const auto canSend = [lifetime, output, completed = item.committedText || item.resolution.has_value()] {
        // A fixed original-token resolution clears a card even after a later
        // model error. All ownership/account/workspace/route guards still hold.
        return lifetime && lifetime->runtime_->outputCurrent(output, completed);
    };
    const auto completion = [lifetime, output](bool ok, const QString&, const RemoteIMSendReceipt&) {
        if (lifetime && !ok && lifetime->runtime_->bindingCurrent(output.reservation.input))
            emit lifetime->relayError(QString::fromStdString(output.reservation.input.binding.peerId),
                                      QStringLiteral("一项阶段回复发送失败，系统不会自动重发。"));
    };
    const QString peer = QString::fromStdString(route->reservation.input.binding.peerId);
    if (item.media) {
        runtime_->app.sendMachineMediaTo(peer, item.media->path, item.media->type, item.media->caption, completion,
                                         canSend, {}, false, [lifetime, token, index, receipt = item.receipt](bool) {
                                             if (lifetime) lifetime->acknowledgeDispatch(token, index, receipt);
                                         });
    } else if (item.approval) {
        runtime_->app.sendMachineApprovalRequestTo(peer, item.text, *item.approval, completion, canSend);
        if (lifetime) acknowledgeDispatch(token, index, item.receipt);
    } else if (item.resolution) {
        const bool queued =
            runtime_->app.sendMachineApprovalResolvedTo(peer, item.text, *item.resolution, completion, canSend);
        if (lifetime) {
            lease = runtime_->coreLeases.find(token);
            if (queued && lease != runtime_->coreLeases.end())
                for (auto& approval : lease->approvals)
                    if (approval.publicToken == item.resolution->token) approval.terminalDispatched = true;
            acknowledgeDispatch(token, index, item.receipt);
        }
    } else {
        runtime_->app.sendMachineTextTo(peer, item.text, completion, canSend);
        if (lifetime) acknowledgeDispatch(token, index, item.receipt);
    }
}

void DesktopSessionImBridge::acknowledgeDispatch(quint64 token, quint32 index, const Router::TurnReceipt& receipt) {
    const auto route = runtime_->active(token);
    const auto scope = runtime_->liveScope();
    if (!route || !route->receipt || !scope) return;
    const QString session = QString::fromStdString(route->reservation.input.binding.sessionId);
    runtime_->router.acknowledgeDispatch(token, receipt, *scope, route->reservation.input.binding.workspaceKey, index);
    const auto lease = runtime_->coreLeases.find(token);
    if (lease != runtime_->coreLeases.end()) {
        lease->outputs.remove(index);
        lease->dispatching = false;
    }
    const bool retired = !runtime_->active(token);
    prunePlatformWork();
    if (retired)
        QTimer::singleShot(0, this, [this, session] { dispatch(session); });
    else
        QTimer::singleShot(0, this, [this, token] {
            dispatchOutput(token);
            processFollowup(token);
        });
}

void DesktopSessionImBridge::publishMedia(const QString& session, const QString& messageId) {
    const auto scope = runtime_->liveScope();
    if (!scope) return;
    for (const auto& route : runtime_->router.snapshot(*scope).active) {
        if (route.reservation.input.binding.sessionId != session.toStdString()) continue;
        auto lease = runtime_->coreLeases.find(route.reservation.token);
        if (lease == runtime_->coreLeases.end() || !lease->observed->valid || lease->observed->failed) return;
        const auto segment = lease->segments.find(messageId.toStdString());
        if (segment == lease->segments.end()) return;
        const auto receipt = segment->second;
        MaiMessage reply;
        MaiSession owner;
        if (!runtime_->controller.agent().getMessage(session.toStdString(), messageId.toStdString(), reply) ||
            reply.role != MaiRole::Assistant || !runtime_->controller.agent().getSession(session.toStdString(), owner))
            return;
        owner.directory = route.reservation.input.binding.workspaceKey;
        QString error;
        const auto media = deliveredMedia(reply, owner, &error);
        const QPointer<DesktopSessionImBridge> lifetime(this);
        for (const auto& item : media) {
            lease = runtime_->coreLeases.find(route.reservation.token);
            if (lease == runtime_->coreLeases.end()) return;
            if (lease->sentMedia.contains(item.partId)) continue;
            const auto allocated =
                runtime_->router.appendOutput(route.reservation.token, receipt, *scope, owner.directory, 1);
            if (allocated.status != Router::Status::Ok) return;
            lease->sentMedia.insert(item.partId);
            lease->outputs.insert(allocated.firstIndex, Runtime::CoreLease::Output{receipt, {}, item});
            dispatchOutput(route.reservation.token);
            if (!lifetime) return;
        }
        if (!error.isEmpty()) emit relayError(QString::fromStdString(route.reservation.input.binding.peerId), error);
        return;
    }
}

void DesktopSessionImBridge::publishQuestion(const QString& questionId, const QString& session) {
    const QPointer<DesktopSessionImBridge> lifetime(this);
    publishCommittedOutput(session);
    if (!lifetime) return;
    const auto scope = runtime_->liveScope();
    if (!scope) return;
    for (const auto& route : runtime_->router.snapshot(*scope).active) {
        if (route.reservation.input.binding.sessionId != session.toStdString()) continue;
        auto lease = runtime_->coreLeases.find(route.reservation.token);
        if (lease == runtime_->coreLeases.end() || !lease->observed->valid ||
            (lease->question && lease->question->id == questionId.toStdString()))
            return;
        for (const auto& question : runtime_->controller.agent().listPendingQuestions()) {
            if (question.id != questionId.toStdString() || question.sessionId != session.toStdString() ||
                !lease->segments.count(question.messageId))
                continue;
            lease->question = question;
            const auto receipt = lease->segments.at(question.messageId);
            QString body = QString::fromStdString(question.question);
            for (const auto& option : question.options) body += QStringLiteral("\n- ") + QString::fromStdString(option);
            body += QStringLiteral("\n\n可直接回复文字作答。");
            sendTextOutput(route.reservation.token, receipt, body);
            if (!lifetime) return;
            setActivity(session, int(RemoteIMActivityKind::MachineWaiting));
            return;
        }
    }
}

void DesktopSessionImBridge::publishApproval(const QString& permissionId, const QString& session) {
    const QPointer<DesktopSessionImBridge> lifetime(this);
    const auto scope = runtime_->liveScope();
    if (!scope) return;
    publishCommittedOutput(session);
    if (!lifetime) return;
    for (const auto& route : runtime_->router.snapshot(*scope).active) {
        if (route.reservation.input.binding.sessionId != session.toStdString() || !route.receipt) continue;
        auto lease = runtime_->coreLeases.find(route.reservation.token);
        if (lease == runtime_->coreLeases.end() || !runtime_->current(route.reservation) || !lease->observed->valid ||
            lease->observed->failed)
            return;
        if (lease->approvals.contains(permissionId)) return;
        for (const auto& permission : runtime_->controller.agent().listPendingPermissions()) {
            const auto segment = lease->segments.find(permission.messageId);
            if (permission.id != permissionId.toStdString() || permission.sessionId != session.toStdString() ||
                segment == lease->segments.end())
                continue;
            const auto receipt = segment->second;
            RemoteIMApprovalRequest request;
            request.token = QStringLiteral("approval-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
            request.actions = {RemoteIMApprovalAction::ApproveOnce};
            if (permission.allowForSession) request.actions.append(RemoteIMApprovalAction::ApprovePrefix);
            request.actions.append(RemoteIMApprovalAction::Reject);
            const auto allocation = runtime_->router.appendOutput(route.reservation.token, receipt, *scope,
                                                                  route.reservation.input.binding.workspaceKey, 1);
            if (allocation.status != Router::Status::Ok) return;
            lease->approvals.insert(permissionId, {permission, receipt, request.token});
            QString body = QStringLiteral("工具 %1 请求操作授权。").arg(QString::fromStdString(permission.toolName));
            if (permission.allowForSession) body += QStringLiteral("记住授权仅作用于本会话的相同审批目标。");
            Runtime::CoreLease::Output output{receipt, body, {}};
            output.approval = request;
            lease->outputs.insert(allocation.firstIndex, output);
            dispatchOutput(route.reservation.token);
            setActivity(session, int(RemoteIMActivityKind::MachineWaiting));
            return;
        }
    }
}

bool DesktopSessionImBridge::receiveApprovalDecision(const RemoteIMMessage& message) {
    const auto scope = runtime_->liveScope();
    if (!scope || message.id.isEmpty() || !message.hasApprovalDecision || message.hasApprovalRequest ||
        message.direction != RemoteIMMessageDirection::Incoming || message.origin != RemoteIMMessageOrigin::Human ||
        message.toUserId.toStdString() != scope->ownerUserId)
        return false;
    const auto action = message.approvalDecision.action;
    if (action != RemoteIMApprovalAction::ApproveOnce && action != RemoteIMApprovalAction::ApprovePrefix &&
        action != RemoteIMApprovalAction::Reject)
        return false;
    for (const auto& route : runtime_->router.snapshot(*scope).active) {
        if (!route.receipt || route.phase != Router::Phase::Running ||
            route.reservation.input.binding.peerId != message.fromUserId.toStdString() ||
            !runtime_->current(route.reservation))
            continue;
        auto lease = runtime_->coreLeases.find(route.reservation.token);
        if (lease == runtime_->coreLeases.end() || !lease->observed->valid || lease->observed->failed ||
            runtime_->controller.agent().activeTurnId(route.reservation.input.binding.sessionId) !=
                route.receipt->turnId)
            continue;
        for (const auto& approval : lease->approvals) {
            if (approval.publicToken != message.approvalDecision.token || approval.resolutionQueued ||
                (action == RemoteIMApprovalAction::ApprovePrefix && !approval.permission.allowForSession))
                continue;
            for (const auto& permission : runtime_->controller.agent().listPendingPermissions())
                if (permission.id == approval.permission.id && permission.sessionId == approval.permission.sessionId &&
                    permission.messageId == approval.permission.messageId &&
                    permission.partId == approval.permission.partId) {
                    const auto decision =
                        action == RemoteIMApprovalAction::ApproveOnce     ? MaiPermissionDecision::Approved
                        : action == RemoteIMApprovalAction::ApprovePrefix ? MaiPermissionDecision::ApprovedForSession
                                                                          : MaiPermissionDecision::Denied;
                    return bool(runtime_->controller.agent().submit(
                        MaiReplyPermission{permission.id, decision, lease->directory}));
                }
        }
    }
    return false;
}

void DesktopSessionImBridge::resolveApproval(const QString& permissionId, const QString& decision) {
    const auto scope = runtime_->liveScope();
    if (!scope) return;
    const QPointer<DesktopSessionImBridge> lifetime(this);
    for (const auto& route : runtime_->router.snapshot(*scope).active) {
        auto lease = runtime_->coreLeases.find(route.reservation.token);
        if (lease == runtime_->coreLeases.end()) continue;
        auto approval = lease->approvals.find(permissionId);
        if (approval == lease->approvals.end() || approval->resolutionQueued) continue;
        const auto allocation = runtime_->router.appendOutput(route.reservation.token, approval->receipt, *scope,
                                                              route.reservation.input.binding.workspaceKey, 1);
        if (allocation.status != Router::Status::Ok) return;
        approval->resolutionQueued = true;
        Runtime::CoreLease::Output output{approval->receipt, QStringLiteral("审批已处理。"), {}};
        output.resolution = RemoteIMApprovalDecision{approval->publicToken, decision == QStringLiteral("timed_out")
                                                                                ? RemoteIMApprovalAction::AutoDeclined
                                                                                : RemoteIMApprovalAction::Resolved};
        lease->outputs.insert(allocation.firstIndex, output);
        dispatchOutput(route.reservation.token);
        if (lifetime)
            setActivity(QString::fromStdString(route.reservation.input.binding.sessionId),
                        int(RemoteIMActivityKind::MachineWorking));
        return;
    }
}

void DesktopSessionImBridge::setActivity(const QString& session, int value) {
    const auto scope = runtime_->liveScope();
    if (!scope) return;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    for (const auto& route : runtime_->router.snapshot(*scope).active) {
        if (route.reservation.input.binding.sessionId != session.toStdString()) continue;
        auto lease = runtime_->coreLeases.find(route.reservation.token);
        if (lease == runtime_->coreLeases.end() || !lease->observed->valid || lease->observed->failed) return;
        const auto kind = static_cast<RemoteIMActivityKind>(value);
        if (lease->activity.active && lease->activity.kind == kind && now - lease->lastActivityAt < 2000) return;
        if (!runtime_->current(route.reservation)) return;
        if (!lease->activity.active || lease->activity.kind != kind) lease->activity.startedAtMs = now;
        lease->activity.kind = kind;
        lease->activity.active = true;
        ++lease->activity.sequence;
        lease->lastActivityAt = now;
        const auto signal = lease->activity;
        const QString peer = QString::fromStdString(route.reservation.input.binding.peerId);
        // 此调用前没有 await/媒体准备；接收方沿既有 v2 activity 控制通道绘制计时器。
        runtime_->app.client().sendActivity(peer, signal, {});
        return;
    }
}

void DesktopSessionImBridge::heartbeatActivities() {
    const auto scope = runtime_->liveScope();
    if (!scope) return;
    const QPointer<DesktopSessionImBridge> lifetime(this);
    for (const auto& route : runtime_->router.snapshot(*scope).active) {
        const auto lease = runtime_->coreLeases.constFind(route.reservation.token);
        if (lease == runtime_->coreLeases.constEnd()) continue;
        setActivity(QString::fromStdString(route.reservation.input.binding.sessionId), int(lease->activity.kind));
        if (!lifetime) return;
        processFollowup(route.reservation.token);
        if (!lifetime) return;
    }
}

void DesktopSessionImBridge::finish(const QString& session) {
    const QPointer<DesktopSessionImBridge> lifetime(this);
    const auto scope = runtime_->liveScope();
    if (!scope || runtime_->controller.agent().isBusy(session.toStdString())) return;
    publishCommittedOutput(session);
    if (!lifetime) return;
    std::optional<Router::ActiveRoute> route;
    for (const auto& item : runtime_->router.snapshot(*scope).active)
        if (item.reservation.input.binding.sessionId == session.toStdString()) {
            route = item;
            break;
        }
    if (!route) {
        QTimer::singleShot(0, this, [this, session] { dispatch(session); });
        return;
    }
    if (route->phase != Router::Phase::Running || !route->receipt) return;
    const auto token = route->reservation.token;
    const auto receipt = *route->receipt;
    const auto input = route->reservation.input;
    auto lease = runtime_->coreLeases.find(token);
    if (!runtime_->current(route->reservation) || lease == runtime_->coreLeases.end() || !lease->observed->valid) {
        localTakeover(session);
        return;
    }
    if (lease->observed->failed) {
        runtime_->router.finishWithoutOutput(token, receipt, *scope, input.binding.workspaceKey);
        prunePlatformWork();
        emit relayError(QString::fromStdString(input.binding.peerId),
                        QStringLiteral("创作中心未完成这条消息，请在本机查看会话错误。"));
        if (lifetime) QTimer::singleShot(0, this, [this, session] { dispatch(session); });
        return;
    }
    MaiMessage reply;
    if (!runtime_->controller.agent().getMessage(session.toStdString(), receipt.assistantMessageId, reply) ||
        reply.role != MaiRole::Assistant || !reply.completed)
        return;
    // 正文只由完整成功响应的 typed commit 发布。取消或错误留下的部分文本不补发。
    // 明确交付的媒体仍在成功收尾、正文之后排队；覆盖同 turn 已受理的全部分段，
    // 不扫描最新会话，也不会把工具观察图或失败轮次的原始结果当交付。
    lease = runtime_->coreLeases.find(token);
    if (lease == runtime_->coreLeases.end()) return;
    QStringList mediaMessages;
    for (const auto& segment : lease->segments) mediaMessages.append(QString::fromStdString(segment.first));
    for (const auto& message : mediaMessages) {
        publishMedia(session, message);
        if (!lifetime) return;
    }
    lease = runtime_->coreLeases.find(token);
    if (lease == runtime_->coreLeases.end()) return;
    const bool empty = lease->sentParts.empty() && lease->sentMedia.empty();
    const auto sealed = runtime_->router.sealOutput(token, receipt, *scope, input.binding.workspaceKey);
    if (sealed != Router::Status::Ok && sealed != Router::Status::Duplicate) return;
    prunePlatformWork();
    if (empty)
        emit relayError(QString::fromStdString(input.binding.peerId),
                        QStringLiteral("这轮没有可自动回传的文字或明确交付的媒体，请在创作中心查看结果。"));
    if (lifetime && !runtime_->active(token)) QTimer::singleShot(0, this, [this, session] { dispatch(session); });
}
