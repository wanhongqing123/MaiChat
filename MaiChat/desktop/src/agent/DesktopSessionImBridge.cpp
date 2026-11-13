#include "agent/DesktopSessionImBridge.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>
#include <QDeadlineTimer>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QPointer>
#include <QQueue>
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

namespace {

constexpr int kPendingLimit = 32;
constexpr int kRememberedInputs = 512;
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

QString inputIdentity(const RemoteDiagnostics::AccountTag& account, const QString& peer, const QString& id) {
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

struct DesktopSessionImBridge::Runtime {
    struct Binding {
        QString session;
        RemoteDiagnostics::AccountTag account;
        quint64 generation = 0;
        QSet<QString> priorUserReceipts;
    };
    struct Input {
        QString peer;
        QString session;
        QString identity;
        QString text;
        QString workspace;
        RemoteDiagnostics::AccountTag account;
        quint64 bindingGeneration = 0;
        quint64 ownerGeneration = 0;
        quint64 connectionGeneration = 0;
        RemoteIMMessage source;
        bool prepared = true;
        std::vector<MaiModelImage> images;
        std::vector<MaiVideoPart> videos;
    };
    struct Route {
        Input input;
        QString user;
        QString assistant;
        std::shared_ptr<InputReceipt> receipt;
        MaiEventBus::Token subscription = 0;
        bool finishing = false;
        int pendingMediaDispatches = 0;
    };
    RemoteIMApplication& app;
    AgentController& controller;
    QHash<QString, Binding> bindings;
    QHash<QString, QQueue<Input>> pending;
    QHash<QString, Route> active;
    QHash<QString, std::shared_ptr<std::atomic<bool>>> preparing;
    QSet<QString> seen;
    QQueue<QString> seenOrder;
    quint64 nextBindingGeneration = 0;
    quint64 ownerGeneration = 0;
    int pendingCount = 0;
    bool suspended = false;

    Runtime(RemoteIMApplication& application, AgentController& agent) : app(application), controller(agent) {}

    bool currentIdentity(const Input& input) const {
        const auto binding = bindings.constFind(input.peer);
        return !suspended && app.connectionReady() && app.connectionGeneration() == input.connectionGeneration &&
            app.client().currentAccount() == input.account &&
            app.chatState().ownerUserId() == input.account.ownerUserId &&
            app.isAcceptedFriend(input.peer) && ownerGeneration == input.ownerGeneration &&
            binding != bindings.constEnd() && binding->session == input.session &&
            binding->account == input.account && binding->generation == input.bindingGeneration;
    }
    bool currentWorkspace(const Input& input) const {
        MaiSession owner;
        return controller.agent().getSession(input.session.toStdString(), owner) && owner.isRoot() &&
            QFileInfo(QString::fromStdString(owner.directory)).canonicalFilePath() == input.workspace;
    }
    bool current(const Input& input) const { return currentIdentity(input) && currentWorkspace(input); }
    void release(Route& route) {
        route.receipt->valid = false;
        controller.agent().eventBus().unsubscribe(route.subscription);
    }
};

DesktopSessionImBridge::DesktopSessionImBridge(RemoteIMApplication& app, AgentController& controller, QObject* parent)
    : QObject(parent), runtime_(std::make_unique<Runtime>(app, controller)) {
    connect(&app, &RemoteIMApplication::incomingMessageArrived, this, &DesktopSessionImBridge::acceptLive);
    connect(&app, &RemoteIMApplication::connectionChanged, this, [this](bool connected) {
        if (!connected) suspend();
        else if (runtime_->app.connectionReady()) runtime_->suspended = false;
    });
    connect(&controller, &AgentController::turnFinished, this, &DesktopSessionImBridge::finish);
    connect(&controller, &AgentController::userMessageAdded, this,
            [this](const QString& session, const QString& user) {
        const auto route = runtime_->active.constFind(session);
        if (route != runtime_->active.constEnd() && route->user == user) return;
        for (const auto& binding : runtime_->bindings)
            if (binding.session == session && binding.priorUserReceipts.contains(user)) return;
        localTakeover(session);
    });
    connect(&controller, &AgentController::sessionStructureChanged, this, &DesktopSessionImBridge::pruneSessions);
}

DesktopSessionImBridge::~DesktopSessionImBridge() { suspend(); }

bool DesktopSessionImBridge::bind(const QString& peerId, const QString& sessionId, QString* error) {
    const auto reject = [error](const QString& message) { if (error) *error = message; return false; };
    const QString peer = peerId.trimmed();
    const auto account = runtime_->app.client().currentAccount();
    bool friendExists = false;
    for (const auto& contact : runtime_->app.chatState().contacts())
        if (contact.userId == peer) friendExists = true;
    if (runtime_->suspended || !runtime_->app.connectionReady() || !account.isValid() ||
        account.ownerUserId != runtime_->app.chatState().ownerUserId() ||
        peer.isEmpty() || peer == account.ownerUserId || !friendExists || !runtime_->app.isAcceptedFriend(peer))
        return reject(QStringLiteral("请选择当前账号的好友会话。"));
    MaiSession session;
    if (!runtime_->controller.agent().getSession(sessionId.toStdString(), session) || !session.isRoot())
        return reject(QStringLiteral("请选择已有的本机创作中心会话。"));
    for (auto it = runtime_->bindings.constBegin(); it != runtime_->bindings.constEnd(); ++it)
        if (it.key() != peer && it->account == account && it->session == sessionId)
            return reject(QStringLiteral("这个创作中心会话已绑定其他好友，请先解除原绑定。"));
    if (boundSession(peer) == sessionId) { if (error) error->clear(); return true; }
    QPointer<DesktopSessionImBridge> lifetime(this);
    unbind(peer);
    if (!lifetime) return false;
    QSet<QString> priorInputs;
    // Binding is a user action, not a history replay. Ignore exact user receipts
    // already stored before it, including their delayed Qt delivery. This one
    // bounded snapshot never determines which assistant output may be sent.
    for (const auto& message : runtime_->controller.agent().listMessagesPage(session.id, "", 512))
        if (message.role == MaiRole::User) priorInputs.insert(QString::fromStdString(message.id));
    runtime_->bindings.insert(peer, {sessionId, account, ++runtime_->nextBindingGeneration, priorInputs});
    if (error) error->clear();
    emit bindingChanged(peer);
    return true;
}

QString DesktopSessionImBridge::boundSession(const QString& peer) const {
    const auto binding = runtime_->bindings.constFind(peer);
    return binding != runtime_->bindings.constEnd() &&
        binding->account.ownerUserId == runtime_->app.chatState().ownerUserId() &&
        (!runtime_->app.client().currentAccount().isValid() || binding->account == runtime_->app.client().currentAccount())
        ? binding->session : QString();
}

QHash<QString, QString> DesktopSessionImBridge::bindings() const {
    QHash<QString, QString> result;
    for (auto it = runtime_->bindings.constBegin(); it != runtime_->bindings.constEnd(); ++it)
        if (!boundSession(it.key()).isEmpty()) result.insert(it.key(), it->session);
    return result;
}

void DesktopSessionImBridge::unbind(const QString& peer) {
    const auto binding = runtime_->bindings.find(peer);
    if (binding == runtime_->bindings.end()) return;
    const QString session = binding->session;
    runtime_->bindings.erase(binding);
    runtime_->pendingCount -= runtime_->pending.value(session).size();
    runtime_->pending.remove(session);
    if (runtime_->preparing.contains(session)) {
        runtime_->preparing.take(session)->store(true);
    }
    auto route = runtime_->active.find(session);
    if (route != runtime_->active.end()) {
        runtime_->release(*route);
        runtime_->active.erase(route);
    }
    emit bindingChanged(peer);
}

void DesktopSessionImBridge::localTakeover(const QString& session) {
    QStringList peers;
    for (auto it = runtime_->bindings.constBegin(); it != runtime_->bindings.constEnd(); ++it)
        if (it->session == session) peers.append(it.key());
    QPointer<DesktopSessionImBridge> lifetime(this);
    for (const auto& peer : peers) {
        unbind(peer);
        if (!lifetime) return;
    }
}

void DesktopSessionImBridge::suspend() {
    runtime_->suspended = true;
    ++runtime_->ownerGeneration;
    for (auto& route : runtime_->active) runtime_->release(route);
    runtime_->active.clear();
    for (const auto& canceled : runtime_->preparing) canceled->store(true);
    runtime_->preparing.clear();
    runtime_->pending.clear();
    runtime_->pendingCount = 0;
}

void DesktopSessionImBridge::pruneSessions() {
    QPointer<DesktopSessionImBridge> lifetime(this);
    const auto copy = bindings();
    for (auto it = copy.constBegin(); it != copy.constEnd(); ++it) {
        MaiSession session;
        if (!runtime_->controller.agent().getSession(it.value().toStdString(), session) || !session.isRoot())
            unbind(it.key());
        if (!lifetime) return;
    }
}

void DesktopSessionImBridge::acceptLive(const QString& peer, const RemoteIMMessage& message) {
    const auto account = runtime_->app.client().currentAccount();
    const auto binding = runtime_->bindings.constFind(peer);
    if (runtime_->suspended || !account.isValid() || !runtime_->app.connectionReady() || binding == runtime_->bindings.constEnd() ||
        binding->account != account || message.direction != RemoteIMMessageDirection::Incoming ||
        message.origin != RemoteIMMessageOrigin::Human || message.fromUserId != peer ||
        message.toUserId != account.ownerUserId || peer == account.ownerUserId ||
        !runtime_->app.isAcceptedFriend(peer) || message.hasApprovalRequest || message.hasApprovalDecision)
        return;
    const QString identity = inputIdentity(account, peer, message.id);
    if (message.id.isEmpty() || runtime_->seen.contains(identity)) return;
    runtime_->seen.insert(identity);
    runtime_->seenOrder.enqueue(identity);
    while (runtime_->seenOrder.size() > kRememberedInputs) runtime_->seen.remove(runtime_->seenOrder.dequeue());
    const bool hasAttachment = message.hasImage || message.hasVideo || message.hasVoice || message.hasFile;
    if (!hasAttachment && message.text.trimmed().isEmpty()) {
        emit relayError(peer, QStringLiteral("这条消息没有可提交的文字或附件。"));
        return;
    }
    if ((message.hasImage && message.image.localPath.isEmpty()) ||
        (message.hasVideo && message.video.localPath.isEmpty()) ||
        (message.hasFile && message.file.localPath.isEmpty()) ||
        (message.hasVoice && message.voice.localPath.isEmpty())) {
        emit relayError(peer, QStringLiteral("这条附件还没有可读取的本地缓存，没有提交给创作中心。"));
        return;
    }
    if (message.text.size() > kMaximumInputBytes || message.text.toUtf8().size() > kMaximumInputBytes) {
        emit relayError(peer, QStringLiteral("这条文字超过会话输入上限，没有提交给创作中心，请缩短后手动重发。"));
        return;
    }
    if (runtime_->pendingCount >= kPendingLimit) {
        emit relayError(peer, QStringLiteral("待处理的会话消息已达 32 条；这条消息没有提交，请稍后手动重发。"));
        return;
    }
    QString text = message.text;
    // IM and Agent IDs belong to different namespaces. Preserve an IM quote as
    // its display snapshot, never pretend its SDK ID is a Core quote receipt.
    if (message.hasQuote && !message.quote.digest.isEmpty())
        text = QStringLiteral("引用：%1\n\n%2").arg(message.quote.digest, text);
    Runtime::Input input;
    input.peer = peer;
    input.session = binding->session;
    input.identity = identity;
    input.text = std::move(text);
    MaiSession owner;
    if (!runtime_->controller.agent().getSession(input.session.toStdString(), owner)) {
        pruneSessions();
        return;
    }
    input.workspace = QFileInfo(QString::fromStdString(owner.directory)).canonicalFilePath();
    if (input.workspace.isEmpty()) {
        emit relayError(peer, QStringLiteral("会话工作目录不可读取，这条消息没有提交给创作中心。"));
        return;
    }
    input.account = account;
    input.bindingGeneration = binding->generation;
    input.ownerGeneration = runtime_->ownerGeneration;
    input.connectionGeneration = runtime_->app.connectionGeneration();
    input.source = message;
    input.prepared = !hasAttachment;
    runtime_->pending[binding->session].enqueue(input);
    ++runtime_->pendingCount;
    dispatch(binding->session);
}

void DesktopSessionImBridge::dispatch(const QString& session) {
    if (runtime_->active.contains(session) || runtime_->preparing.contains(session) ||
        runtime_->controller.agent().isBusy(session.toStdString())) return;
    auto queue = runtime_->pending.find(session);
    if (queue == runtime_->pending.end() || queue->isEmpty()) return;
    const Runtime::Input input = queue->head();
    if (!runtime_->current(input)) {
        QPointer<DesktopSessionImBridge> lifetime(this);
        if (runtime_->currentIdentity(input) && !runtime_->currentWorkspace(input))
            emit relayError(input.peer, QStringLiteral("会话工作目录已改变，已停止这条消息的提交与自动回传，请重新确认绑定。"));
        if (lifetime) localTakeover(session);
        return;
    }
    if (!input.prepared) {
        if (attachmentPreparationAdmissions().load() >= kPendingLimit) {
            queue->dequeue();
            --runtime_->pendingCount;
            QPointer<DesktopSessionImBridge> lifetime(this);
            emit relayError(input.peer, QStringLiteral("附件准备队列已满，这条消息没有提交，后续消息会继续处理。"));
            if (lifetime) QTimer::singleShot(0, this, [this, session] { dispatch(session); });
            return;
        }
        attachmentPreparationAdmissions().fetch_add(1);
        auto canceled = std::make_shared<std::atomic<bool>>(false);
        runtime_->preparing.insert(session, canceled);
        auto* watcher = new QFutureWatcher<PreparedInput>(this);
        watcher->setObjectName(QStringLiteral("sessionImAttachmentPreparation"));
        const QDeadlineTimer deadline(kPreparationBudgetMs);
        auto* timer = new QTimer(watcher);
        timer->setObjectName(QStringLiteral("sessionImAttachmentDeadline"));
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, this, [this, input, canceled, session] {
            if (runtime_->preparing.value(session) != canceled) return;
            canceled->store(true);
            runtime_->preparing.remove(session);
            auto pending = runtime_->pending.find(session);
            if (pending != runtime_->pending.end() && !pending->isEmpty() && pending->head().identity == input.identity) {
                pending->dequeue();
                --runtime_->pendingCount;
            }
            QPointer<DesktopSessionImBridge> lifetime(this);
            if (runtime_->currentIdentity(input))
                emit relayError(input.peer, QStringLiteral("附件准备超时，这条消息没有提交，后续消息会继续处理。"));
            if (lifetime) QTimer::singleShot(0, this, [this, session] { dispatch(session); });
        });
        timer->start(kPreparationBudgetMs);
        connect(watcher, &QFutureWatcher<PreparedInput>::finished, this, [this, watcher, input, canceled, session, deadline] {
            if (auto* timer = watcher->findChild<QTimer*>()) timer->stop();
            PreparedInput prepared = watcher->result();
            if (deadline.hasExpired()) prepared.error = QStringLiteral("附件准备超时，这条消息没有提交给创作中心。");
            watcher->deleteLater();
            if (runtime_->preparing.value(session) != canceled) return;
            runtime_->preparing.remove(session);
            auto pending = runtime_->pending.find(session);
            if (canceled->load() || pending == runtime_->pending.end() ||
                pending->isEmpty() || pending->head().identity != input.identity) return;
            if (!runtime_->current(input)) { dispatch(session); return; }
            if (!prepared.error.isEmpty()) {
                pending->dequeue();
                --runtime_->pendingCount;
                QPointer<DesktopSessionImBridge> lifetime(this);
                emit relayError(input.peer, prepared.error);
                if (lifetime) QTimer::singleShot(0, this, [this, session] { dispatch(session); });
                return;
            }
            pending->head().prepared = true;
            pending->head().text = prepared.text;
            pending->head().images = prepared.images;
            pending->head().videos = prepared.videos;
            dispatch(session);
        });
        watcher->setFuture(QtConcurrent::run(attachmentPreparationPool(), [input, canceled, deadline] {
            const auto release = qScopeGuard([] { attachmentPreparationAdmissions().fetch_sub(1); });
            if (canceled->load() || deadline.hasExpired()) return PreparedInput{};
            return stageInput(input.workspace, input.identity, input.source, input.text, canceled, deadline);
        }));
        return;
    }
    auto receipt = std::make_shared<InputReceipt>();
    receipt->session = session.toStdString();
    const auto token = runtime_->controller.agent().eventBus().subscribe([receipt](const MaiEvent& event) {
        if (event.sessionId != receipt->session) return;
        if (event.type == MaiEventType::SessionError) { receipt->failed = true; return; }
        if (event.type == MaiEventType::SessionDeleted || event.type == MaiEventType::MessageRemoved) {
            receipt->valid = false;
            return;
        }
        if (event.type != MaiEventType::MessageUpdated) return;
        std::lock_guard<std::mutex> lock(receipt->mutex);
        if (receipt->capturing && std::this_thread::get_id() == receipt->submitThread && receipt->ids.size() < 2) {
            receipt->ids.push_back(event.messageId);
        } else if (std::find(receipt->ids.begin(), receipt->ids.end(), event.messageId) == receipt->ids.end()) {
            receipt->valid = false;
        }
    });
    const auto accepted = runtime_->controller.agent().submit(
        MaiSendPrompt{session.toStdString(), input.text.toStdString(), input.images, input.videos});
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(receipt->mutex);
        receipt->capturing = false;
        ids = receipt->ids;
    }
    if (!accepted) {
        receipt->valid = false;
        runtime_->controller.agent().eventBus().unsubscribe(token);
        // A concurrent ordinary submission can win the idle check. It was not
        // accepted; leave this routing input waiting for that turn's idle event.
        if (accepted.error().code() == MaiErrorCode::Busy) return;
        queue->dequeue();
        --runtime_->pendingCount;
        QPointer<DesktopSessionImBridge> lifetime(this);
        emit relayError(input.peer, QStringLiteral("这条消息未能提交给创作中心，请在本机查看会话。"));
        if (lifetime) QTimer::singleShot(0, this, [this, session] { dispatch(session); });
        return;
    }
    queue->dequeue();
    --runtime_->pendingCount;
    QString user;
    for (const auto& id : ids) {
        MaiMessage message;
        if (runtime_->controller.agent().getMessage(session.toStdString(), id, message) && message.role == MaiRole::User)
            user = QString::fromStdString(id);
    }
    // These exact broadcasts are part of the current Core submit contract. If
    // it changes, fail closed instead of guessing a newest message or resending.
    if (user.isEmpty() || std::find(ids.begin(), ids.end(), accepted.value()) == ids.end()) {
        receipt->valid = false;
        runtime_->controller.agent().eventBus().unsubscribe(token);
        QPointer<DesktopSessionImBridge> lifetime(this);
        emit relayError(input.peer, QStringLiteral("会话已接收消息，但未取得可核对的回复凭据，已停止自动回传。"));
        if (lifetime) localTakeover(session);
        return;
    }
    runtime_->active.insert(session, {input, user, QString::fromStdString(accepted.value()), receipt, token});
}

void DesktopSessionImBridge::completeMediaDispatch(const QString& session, const QString& assistant,
                                                      bool abortRemaining) {
    auto found = runtime_->active.find(session);
    if (found == runtime_->active.end() || found->assistant != assistant || !found->finishing) return;
    if (abortRemaining) found->pendingMediaDispatches = 0;
    else if (found->pendingMediaDispatches > 0) --found->pendingMediaDispatches;
    if (found->pendingMediaDispatches != 0) return;
    runtime_->release(*found);
    runtime_->active.erase(found);
    QTimer::singleShot(0, this, [this, session] { dispatch(session); });
}

void DesktopSessionImBridge::finish(const QString& session) {
    QPointer<DesktopSessionImBridge> lifetime(this);
    auto found = runtime_->active.find(session);
    if (found == runtime_->active.end()) {
        QTimer::singleShot(0, this, [this, session] { dispatch(session); });
        return;
    }
    if (found->finishing) return;
    MaiMessage reply;
    if (!runtime_->controller.agent().getMessage(session.toStdString(), found->assistant.toStdString(), reply) ||
        reply.role != MaiRole::Assistant || reply.completed == 0) return;
    // SessionIdle has no turn ID. Only this exact completed assistant starts
    // output delivery, and duplicate idle events cannot start it a second time.
    found->finishing = true;
    const Runtime::Route route = *found;
    if (runtime_->currentIdentity(route.input) && !runtime_->currentWorkspace(route.input)) {
        emit relayError(route.input.peer, QStringLiteral("会话工作目录已改变，没有自动发送这轮回复，请重新确认绑定。"));
        if (lifetime) localTakeover(session);
        return;
    }
    const bool current = runtime_->current(route.input) && route.receipt->valid.load();
    if (!current || route.receipt->failed.load()) {
        if (current) emit relayError(route.input.peer, QStringLiteral("创作中心未完成这条消息，请在本机查看会话错误。"));
        if (lifetime) completeMediaDispatch(session, route.assistant, true);
        return;
    }
    const QString text = QString::fromStdString(reply.text()).trimmed();
    MaiSession owner;
    QString mediaError;
    const bool hasOwner = runtime_->controller.agent().getSession(session.toStdString(), owner);
    owner.directory = route.input.workspace.toStdString();
    const auto media = hasOwner ? deliveredMedia(reply, owner, &mediaError) : QList<OutputMedia>();
    runtime_->active[session].pendingMediaDispatches = media.size();
    const auto canSend = [lifetime, input = route.input, receipt = route.receipt] {
        return lifetime && receipt->valid.load() && lifetime->runtime_->current(input);
    };
    const auto completion = [lifetime, input = route.input](bool ok, const QString&, const RemoteIMSendReceipt&) {
        if (lifetime && !ok && lifetime->runtime_->current(input))
            emit lifetime->relayError(input.peer, QStringLiteral("一项创作中心回复发送失败，请在聊天记录中查看，系统不会自动重发。"));
    };
    const RemoteIMQuote quote = MessageQuote::quoteFor(route.input.source);
    const bool hasQuote = !quote.digest.isEmpty();
    if (!text.isEmpty()) runtime_->app.sendMachineTextTo(route.input.peer, text, completion, canSend, quote, hasQuote);
    if (!lifetime) return;
    for (const OutputMedia& item : media) {
        if (!canSend()) { completeMediaDispatch(session, route.assistant, true); break; }
        runtime_->app.sendMachineMediaTo(route.input.peer, item.path, item.type, item.caption,
            completion, canSend, quote, hasQuote,
            [lifetime, session, assistant = route.assistant](bool) {
                if (lifetime) lifetime->completeMediaDispatch(session, assistant);
            });
        if (!lifetime) return;
    }
    if (!mediaError.isEmpty()) emit relayError(route.input.peer, mediaError);
    else if (text.isEmpty() && media.isEmpty())
        emit relayError(route.input.peer, QStringLiteral("这轮没有可自动回传的文字或明确交付的媒体，请在创作中心查看结果。"));
    // Keep the exact receipt alive while asynchronous video preparation is in
    // flight. FIFO advances after its final SDK-dispatch guard, not after its
    // network receipt; SDK delivery failures keep their ordinary failed row.
    if (lifetime && media.isEmpty()) completeMediaDispatch(session, route.assistant);
}
