// 文件作用：桌面端应用启动和窗口装配；RemoteIMApplication 负责应用生命周期和全局服务装配，本文件给出其实现。
// 主要入口：selfProfile、createContactGroup、renameContactGroup、deleteContactGroup。
// 直接依赖：app/VideoCoverImage.h、im/VideoFileMetadata.h、remote/RemoteDesktopSignal.h；实现与这些接口协作。
#include "app/RemoteIMApplication.h"
#include "ui/DesktopRasterImage.h"

#include <QSet>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QPointer>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTimer>
#include <QThreadPool>
#include <QUuid>
#include <QVariant>
#include <memory>

#include <QDir>
#include <QBuffer>
#include <QColorSpace>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMimeDatabase>
#include <QPainter>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrentRun>
#include <stdexcept>
#include <utility>
#include <algorithm>
#include <limits>

#include "app/VideoCoverImage.h"
#include "im/VideoFileMetadata.h"
#include "remote/RemoteDesktopSignal.h"
#include "MaiAvatarClient.h"
#include "MaiSystemProxy.h"

namespace {

constexpr int kMachineVideoPreparationBudgetMs = 5000;

std::atomic<int>& machineVideoPreparationAdmissions() {
    static auto* count = new std::atomic<int>(0);
    return *count;
}

QThreadPool* machineVideoPreparationPool() {
    // Process-lifetime bounded service; QObject/window destruction must never
    // join a native shell thumbnail call. Jobs own values/cancel/deadline only.
    static QThreadPool* pool = [] {
        auto* value = new QThreadPool;
        value->setMaxThreadCount(2);
        value->setObjectName(QStringLiteral("sessionImVideoPreparationPool"));
        QCoreApplication::instance()->setProperty("sessionImVideoPreparationPool", QVariant::fromValue<QObject*>(value));
        return value;
    }();
    return pool;
}

struct MachineVideoOutcome {
    RemoteIMVideoPayload payload;
    QString error;
};

MachineVideoOutcome prepareMachineVideo(const QString& path, const QString& coverDirectory,
                                        const std::shared_ptr<std::atomic<bool>>& canceled,
                                        const QDeadlineTimer& deadline) {
    MachineVideoOutcome result;
    if (canceled->load() || deadline.hasExpired()) return result;
    const QFileInfo file(path);
    if (!file.isFile() || !file.isReadable() || file.size() <= 0 || file.size() > 1024ll * 1024 * 1024 ||
        !isSupportedVideoFile(path)) {
        result.error = QStringLiteral("当前 IM 无法发送这种会话视频格式或大小");
        return result;
    }
    VideoFileMetadata metadata = readVideoFileMetadata(path);
    if (metadata.durationSeconds <= 0) metadata.durationSeconds = 1;
    if (canceled->load() || deadline.hasExpired()) return result;
    const VideoCoverImage cover = createVideoCoverImage(path, metadata, coverDirectory);
    if (canceled->load() || deadline.hasExpired()) return result;
    if (!cover.valid) { result.error = QStringLiteral("视频封面生成失败，无法发送会话视频"); return result; }
    result.payload.videoPath = file.absoluteFilePath();
    result.payload.videoType = metadata.containerType;
    result.payload.videoSizeBytes = file.size();
    result.payload.durationSeconds = metadata.durationSeconds;
    result.payload.coverPath = cover.path;
    result.payload.coverType = cover.type;
    result.payload.coverSizeBytes = cover.sizeBytes;
    result.payload.coverWidth = cover.width;
    result.payload.coverHeight = cover.height;
    return result;
}

struct AvatarUploadOutcome {
    QString url;
    QString error;
};

struct AvatarProxy {
    std::string url;
};

// 只适配系统代理；共享上传核心在工作线程逐跳调用，失败时拒绝请求，不隐式直连。
int resolveAvatarProxy(const char* location, void* context, MaiDownloadProxy* result) {
    if (!location || !context || !result) return 0;
    auto* storage = static_cast<AvatarProxy*>(context);
    const auto proxy = maiSystemProxyForUrl(location);
    if (proxy.mode != MaiSystemProxyMode::Direct && proxy.mode != MaiSystemProxyMode::Http &&
        proxy.mode != MaiSystemProxyMode::Socks5)
        return 0;
    storage->url = proxy.url;
    result->url_utf8 = storage->url.c_str();
    // curl 从 URL 解码代理凭据；空指针避免覆盖 URL 内已有凭据。
    result->username_utf8 = nullptr;
    result->password_utf8 = nullptr;
    return 1;
}

AvatarUploadOutcome uploadAvatar(const QString& path, const RemoteIMApplication::AvatarAccess& access,
                                 const QByteArray& caPem, const std::shared_ptr<MaiAvatarCancel>& canceled) {
    const QByteArray bytes = RemoteIMApplication::prepareAvatarJpeg(path);
    if (bytes.isEmpty()) return {{}, QStringLiteral("invalid_image")};
    const QByteArray base = access.mediaBaseUrl.toUtf8();
    const QByteArray token = access.accountToken.toUtf8();
    const QByteArray account = access.accountId.toUtf8();
    MaiAvatarOptions options{};
    options.media_base_url_utf8 = base.constData();
    options.account_token_utf8 = token.constData();
    options.account_id_utf8 = account.constData();
    options.ca_pem = caPem.isEmpty() ? nullptr : caPem.constData();
    options.ca_pem_length = static_cast<size_t>(caPem.size());
    options.connect_timeout_ms = 10000;
    options.transfer_timeout_ms = 60000;
    AvatarProxy proxy;
    options.proxy_resolver = resolveAvatarProxy;
    options.proxy_context = &proxy;
    MaiAvatarResult response{};
    mai_avatar_upload(&options, reinterpret_cast<const unsigned char *>(bytes.constData()),
                      static_cast<size_t>(bytes.size()), "image/jpeg", canceled.get(), &response);
    AvatarUploadOutcome result;
    if (response.status == MAI_AVATAR_OK && response.avatar_url_utf8)
        result.url = QString::fromUtf8(response.avatar_url_utf8);
    else
        result.error = response.status == MAI_AVATAR_CANCELED ? QStringLiteral("canceled")
                                                              : QStringLiteral("upload_failed");
    mai_avatar_result_free(&response);
    return result;
}

// 登录先恢复会话预览；打开会话后才读取最近一页，更早正文按需分页。
constexpr int kConversationPreviewSize = 1;
constexpr int kMessagesPageSize = 40;
constexpr int kGroupPageSize = 20;

bool groupMessageLess(const RemoteIMGroupMessage& lhs, const RemoteIMGroupMessage& rhs) {
    if (lhs.createdAtMillis != rhs.createdAtMillis) return lhs.createdAtMillis < rhs.createdAtMillis;
    const bool leftHasSequence = lhs.sequence > 0;
    const bool rightHasSequence = rhs.sequence > 0;
    if (leftHasSequence != rightHasSequence) return !leftHasSequence;
    if (lhs.sequence != rhs.sequence) return lhs.sequence < rhs.sequence;
    return lhs.id < rhs.id;
}

QString peerOf(const RemoteIMMessage& message) {
    return message.direction == RemoteIMMessageDirection::Outgoing ? message.toUserId : message.fromUserId;
}

QString forwardedCaption(const RemoteIMMessage& message) {
    const QString text = message.text.trimmed();
    if (text.startsWith(QStringLiteral("[图片消息]"))
            || text.startsWith(QStringLiteral("[文件消息]"))
            || text.startsWith(QStringLiteral("[视频消息]"))
            || text.startsWith(QStringLiteral("[语音消息]"))) {
        return QString();
    }
    return text;
}

}  // namespace

QByteArray RemoteIMApplication::prepareAvatarJpeg(const QString &path) {
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly) || source.size() <= 0 ||
        source.size() > 20LL * 1024 * 1024)
        return {};
    QByteArray input = source.read(20LL * 1024 * 1024 + 1);
    if (input.isEmpty() || input.size() > 20LL * 1024 * 1024) return {};
    // Keep the avatar's original 100MP source policy; thumbnail dimensions
    // constrain output only and do not silently inherit the generic 64MP default.
    MaiImageReadOptions options{};
    options.max_encoded_bytes = 20LL * 1024 * 1024;
    options.max_source_pixels = 100LL * 1000 * 1000;
    options.max_output_width = options.max_output_height = 2048;
    QImage decoded = desktopReadRasterBytes(input, options).image;
    if (decoded.isNull()) return {};
    if (decoded.colorSpace().isValid()) decoded.convertToColorSpace(QColorSpace::SRgb);
    QImage image(QSize(512, 512), QImage::Format_RGB32);
    image.setColorSpace(QColorSpace::SRgb);
    image.fill(Qt::white);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        // 先裁切再缩放，极宽/极高图片也不会产生庞大的中间放大图。
        const int side = qMin(decoded.width(), decoded.height());
        const QRect crop((decoded.width() - side) / 2, (decoded.height() - side) / 2, side, side);
        painter.drawImage(image.rect(), decoded, crop);
    }
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "JPEG", 88) || bytes.isEmpty() ||
        bytes.size() > 2000000)
        return {};
    return bytes;
}

RemoteIMContact RemoteIMApplication::selfProfile() const {
    if (selfProfile_.userId == state_.ownerUserId()) return selfProfile_;
    return RemoteIMContact{state_.ownerUserId(), state_.ownerUserId()};
}

RemoteIMApplication::RemoteIMApplication(QString ownerUserId,
                                         std::unique_ptr<RemoteIMClient> client,
                                         std::unique_ptr<LocalMessageDatabase> database,
                                         QObject* parent)
    : QObject(parent), state_(std::move(ownerUserId)), client_(std::move(client)), database_(std::move(database)) {
    if (!client_) {
        throw std::invalid_argument("RemoteIMClient is required");
    }
    client_->setParent(this);
    connect(this, &QObject::destroyed, [generation = messageQueryGeneration_] {
        generation->fetch_add(1);
    });
    messageQueryPool_ = new QThreadPool(this);
    messageQueryPool_->setMaxThreadCount(2);
    typingHeartbeat_ = new QTimer(this);
    typingHeartbeat_->setInterval(3000);
    typingIdle_ = new QTimer(this);
    typingIdle_->setSingleShot(true);
    typingIdle_->setInterval(4000);
    connect(typingIdle_, &QTimer::timeout, this, [this] { setHumanTypingActive(false); });
    connect(typingHeartbeat_, &QTimer::timeout, this, [this] {
        if (typingPeerId_.isEmpty() || typingActivityId_.isEmpty()) return;
        client_->sendActivity(typingPeerId_,
                              RemoteIMActivitySignal{typingActivityId_,
                                                     RemoteIMActivityKind::HumanTyping,
                                                     true, 12000, ++typingSequence_}, {});
    });
    bindClientSignals();
    // 本地库提供完整历史，但启动只取列表所需的一条预览，SDK 漫游按 id 去重合并。
    // 首次打开会话再取最近一页，避免扫码登录同步解析所有会话正文。
    if (database_ && database_->isOpen()) {
        // 分组要在联系人之前恢复：联系人带着分组名进来，分组列表先就位，
        // 内存里才不会出现「指向还不存在的分组」这种中间态。
        state_.setContactGroups(database_->contactGroups());
        hasEarlierMessages_ = database_->loadRecentInto(state_, kConversationPreviewSize);
        groups_ = database_->groups();
        for (const RemoteIMGroup& group : groups_) {
            groupMessages_[group.groupId] = database_->groupMessagesBefore(
                group.groupId, std::numeric_limits<qint64>::max(), QStringLiteral("~"), kGroupPageSize);
            groupHasEarlierLocal_[group.groupId] = database_->groupMessageCount(group.groupId) >
                groupMessages_.value(group.groupId).size();
        }
    }
}

struct RemoteIMApplication::MetadataReceipt {
    RemoteDiagnostics::AccountTag account;
    quint64 generation = 0;
    QString id, sender, recipient;
};

struct RemoteIMApplication::IMHistoryFence {
    IMHistorySource source;
    quint64 sequence = 0;
    std::shared_ptr<std::atomic_bool> canceled;
    IMHistoryFenceCompletion completion;
    QPointer<QTimer> timer;
    QDeadlineTimer deadline{30000};
    bool done = false;
};

std::optional<RemoteIMApplication::IMHistorySource> RemoteIMApplication::imHistorySource(
    const QString& peer, QString* error) const {
    const auto account = client_->currentAccount();
    const auto reject = [error](const QString& value) -> std::optional<IMHistorySource> {
        if (error) *error = value;
        return std::nullopt;
    };
    if (!connectionReady() || !account.isValid() || account.ownerUserId != state_.ownerUserId() ||
        !acceptedFriendsReady() || peer.isEmpty() || peer == account.ownerUserId || !isAcceptedFriend(peer))
        return reject(QStringLiteral("当前账号或好友资料尚未准备好，不能导入历史。"));
    const QString path = database_ ? database_->historyDatabasePath() : QString();
    if (path.isEmpty()) return reject(QStringLiteral("当前账号的本地消息库不可用，无法完整导入聊天历史。"));
    if (error) error->clear();
    RemoteIMClient::LiveMetadataBoundary boundary;
    client_->withLiveMetadataBoundary([&](auto value) { boundary = value; });
    return IMHistorySource{path, account, connectionGeneration_, boundary.generation};
}

void RemoteIMApplication::awaitIMHistoryMetadataFence(const IMHistorySource& source,
    std::shared_ptr<std::atomic_bool> canceled, IMHistoryFenceCompletion completion) {
    auto fence = std::make_shared<IMHistoryFence>();
    fence->source = source;
    client_->withLiveMetadataBoundary([&](auto boundary) { fence->sequence = boundary.sequence; });
    fence->canceled = canceled ? std::move(canceled) : std::make_shared<std::atomic_bool>(false);
    fence->completion = std::move(completion);
    auto* timer = new QTimer(this);
    fence->timer = timer;
    timer->setObjectName(QStringLiteral("sessionImHistoryMetadataFence"));
    timer->setInterval(25);
    connect(timer, &QTimer::timeout, this, &RemoteIMApplication::drainIMHistoryFences);
    historyFences_.append(fence);
    timer->start();
    // Provisional routing is already published. Even an empty immediate fence
    // yields once so reentrant live metadata keeps its original binding epoch.
    QTimer::singleShot(0, this, &RemoteIMApplication::drainIMHistoryFences);
}

void RemoteIMApplication::drainIMHistoryFences() {
    const auto fences = historyFences_;
    QPointer<RemoteIMApplication> lifetime(this);
    for (const auto& fence : fences) {
        if (fence->done) continue;
        QString error;
        RemoteIMClient::LiveMetadataBoundary boundary;
        client_->withLiveMetadataBoundary([&](auto value) { boundary = value; });
        if (fence->canceled->load()) error = QStringLiteral("历史导入已取消。");
        else if (!connectionReady() || client_->currentAccount() != fence->source.account ||
            connectionGeneration_ != fence->source.connectionGeneration || boundary.generation != fence->source.rawGeneration || !database_ ||
            database_->historyDatabasePath() != fence->source.databasePath)
            error = QStringLiteral("账号或本地消息库已改变，历史导入已取消。");
        else if (!metadataPersistenceError_.isEmpty()) error = QStringLiteral("原始消息未能完整保存到本地，历史导入没有完成。");
        else if (fence->deadline.hasExpired()) error = QStringLiteral("等待原始消息保存超时，历史导入没有完成。");
        else if (metadataCompletedSequence_ < fence->sequence) continue;
        fence->done = true;
        if (fence->timer) { fence->timer->stop(); fence->timer->deleteLater(); }
        historyFences_.removeOne(fence);
        auto complete = std::move(fence->completion);
        if (complete) complete(error.isEmpty(), error);
        if (!lifetime) return;
    }
}

void RemoteIMApplication::announceIMMetadata(quint64 ingress, const RemoteIMMessage& message,
    quint64 rawSequence, quint64 rawGeneration) {
    const auto account = client_->currentAccount();
    const quint64 generation = connectionGeneration_;
    RemoteIMClient::LiveMetadataBoundary boundary;
    client_->withLiveMetadataBoundary([&](auto value) { boundary = value; });
    if (!connectionReady() || !account.isValid() || account.ownerUserId != state_.ownerUserId() ||
        message.toUserId != account.ownerUserId || message.direction != RemoteIMMessageDirection::Incoming ||
        message.fromUserId == account.ownerUserId || (rawGeneration && rawGeneration != boundary.generation) ||
        RemoteDesktopSignals::isSignalText(message.text)) return;
    const QPointer<RemoteIMApplication> lifetime(this);
    emit sessionImMessageAnnounced(ingress, message, rawSequence, rawGeneration);
    if (!lifetime || !connectionReady() || generation != connectionGeneration_ || client_->currentAccount() != account) return;
    const auto receipt = database_ ? database_->persistMessageMetadata(message, message.fromUserId) :
        LocalMessagePersistenceResult{false, false, QStringLiteral("Local message database is unavailable")};
    if (receipt.ok) {
        auto metadata = std::make_shared<MetadataReceipt>();
        metadata->account = account;
        metadata->generation = generation;
        metadata->id = message.id;
        metadata->sender = message.fromUserId;
        metadata->recipient = message.toUserId;
        metadataReceipts_.insert(ingress, metadata);
        if (receipt.inserted) firstMetadataRows_.insert(message.id);
    } else metadataPersistenceError_ = receipt.error;
    emit sessionImMessagePersisted(ingress, receipt.ok, receipt.inserted, message.id, receipt.error);
}

void RemoteIMApplication::readyIMMetadata(quint64 ingress, const RemoteIMMessage& message) {
    const auto receipt = metadataReceipts_.value(ingress);
    if (!receipt || !connectionReady() || receipt->generation != connectionGeneration_ ||
        client_->currentAccount() != receipt->account || receipt->id != message.id ||
        receipt->sender != message.fromUserId || receipt->recipient != message.toUserId) return;
    metadataReceipts_.remove(ingress);
    const auto enriched = database_ ? database_->enrichMessageAttachments(message, message.fromUserId) :
        LocalMessagePersistenceResult{false, false, QStringLiteral("Local message database is unavailable")};
    if (!enriched.ok) {
        metadataPersistenceError_ = enriched.error;
        firstMetadataRows_.remove(message.id);
        emit sessionImMessageFailed(ingress, QStringLiteral("收到的附件未能保存到原始消息记录，没有提交给创作中心。"));
        return;
    }
    emit sessionImMessageReady(ingress, message);
}

void RemoteIMApplication::failIMMetadata(quint64 ingress, const QString& error) {
    const auto receipt = metadataReceipts_.take(ingress);
    if (receipt) firstMetadataRows_.remove(receipt->id);
    if (connectionReady()) emit sessionImMessageFailed(ingress, error);
}

void RemoteIMApplication::queryLocalMessages(const LocalMessageSearchRequest& query,
        std::shared_ptr<std::atomic_bool> canceled, MessageSearchCompletion completion,
        QDeadlineTimer deadline) {
    if (!canceled) canceled = std::make_shared<std::atomic_bool>(false);
    auto reject = [&](const QString& error) {
        LocalMessageSearchResult result;
        result.errorCode = error;
        result.limit = query.limit;
        if (completion) completion(result);
    };
    auto stopped = [&] {
        if (canceled->load()) { reject(QStringLiteral("canceled")); return true; }
        if (deadline.hasExpired()) { reject(QStringLiteral("query_timeout")); return true; }
        return false;
    };
    if (stopped()) return;
    if (pendingMessageQueries_ >= 4) {
        LocalMessageSearchResult result;
        result.errorCode = QStringLiteral("query_busy");
        result.limit = query.limit;
        if (completion) completion(result);
        return;
    }
    const quint64 generation = connectionGeneration_;
    const quint64 queryGeneration = messageQueryGeneration_->load();
    const AccountProfile profile = currentAccountProfile();
    if (stopped()) return;
    if (messageQueryGeneration_->load() != queryGeneration) {
        reject(QStringLiteral("identity_changed"));
        return;
    }
    if (accountProfileProvider_ && !profile.loggedIn) {
        LocalMessageSearchResult result;
        result.errorCode = QStringLiteral("account_unavailable");
        result.limit = query.limit;
        if (completion) completion(result);
        return;
    }
    const QString searchSession = QString::number(generation) + QLatin1Char(':') +
        QString::number(queryGeneration) + QLatin1Char(':') +
        profile.accountId + QLatin1Char(':') + profile.imUserId;
    std::shared_ptr<LocalMessageSearchReader> reader;
    if (database_) reader = database_->createSearchReader(searchSession);
    if (!reader) {
        LocalMessageSearchResult result;
        result.errorCode = QStringLiteral("local_storage_unavailable");
        result.limit = query.limit;
        if (completion) completion(result);
        return;
    }
    ++pendingMessageQueries_;
    struct SearchOperation {
        std::shared_ptr<std::atomic_bool> canceled;
        MessageSearchCompletion completion;
        QMetaObject::Connection destructionConnection;
    };
    auto operation = std::make_shared<SearchOperation>();
    operation->canceled = canceled;
    operation->completion = std::move(completion);
    const QPointer<RemoteIMApplication> self(this);
    operation->destructionConnection = connect(this, &QObject::destroyed, [operation] {
        operation->canceled->store(true);
        operation->completion = {};
    });
    auto* watcher = new QFutureWatcher<LocalMessageSearchResult>(this);
    connect(watcher, &QFutureWatcher<LocalMessageSearchResult>::finished, this,
        [self, watcher, operation, generation, queryGeneration, profile, deadline] {
            auto result = watcher->result();
            watcher->deleteLater();
            QObject::disconnect(operation->destructionConnection);
            if (!self) return;
            --self->pendingMessageQueries_;
            if (operation->canceled->load()) {
                result = {};
                result.errorCode = QStringLiteral("canceled");
            } else if (deadline.hasExpired()) {
                result = {};
                result.errorCode = QStringLiteral("query_timeout");
            } else if (self->messageQueryGeneration_->load() != queryGeneration ||
                       self->connectionGeneration_ != generation) {
                result = {};
                result.errorCode = QStringLiteral("identity_changed");
            } else {
                const AccountProfile current = self->currentAccountProfile();
                if (self->messageQueryGeneration_->load() != queryGeneration ||
                    current.loggedIn != profile.loggedIn || current.accountId != profile.accountId ||
                    current.imUserId != profile.imUserId) {
                    result = {};
                    result.errorCode = QStringLiteral("identity_changed");
                }
            }
            auto done = std::move(operation->completion);
            if (done) done(result);
        });
    watcher->setFuture(QtConcurrent::run(messageQueryPool_,
        [reader = std::move(reader), query, canceled, deadline]() mutable {
        auto result = reader->search(query, canceled, deadline);
        reader.reset(); // Release the read lease before publishing completion.
        return result;
    }));
}

QList<RemoteIMGroup> RemoteIMApplication::groups() const { return groups_; }

QList<RemoteIMGroupMessage> RemoteIMApplication::groupMessages(const QString& groupId) const {
    return groupMessages_.value(groupId.trimmed());
}

int RemoteIMApplication::groupUnreadCount(const QString& groupId) const {
    return groupUnreadCounts_.value(groupId.trimmed());
}

bool RemoteIMApplication::hasEarlierGroupMessages(const QString& groupId) const {
    return groupHasEarlierLocal_.value(groupId) || groupHasEarlierRemote_.value(groupId);
}

bool RemoteIMApplication::hasLoadedGroupHistory(const QString& groupId) const {
    return groupInitialHistoryLoaded_.contains(groupId.trimmed());
}

void RemoteIMApplication::ensureGroupHistory(const QString& groupId) {
    const QString id = groupId.trimmed();
    if (!connected_ || id.isEmpty() || groupInitialHistoryLoaded_.contains(id)) return;
    if (std::none_of(groups_.cbegin(), groups_.cend(), [&id](const RemoteIMGroup& group) {
            return group.groupId == id;
        })) return;
    fetchGroupHistoryPage(id, 0);
}

void RemoteIMApplication::mergeGroup(const RemoteIMGroup& group) {
    if (group.groupId.trimmed().isEmpty()) return;
    for (RemoteIMGroup& old : groups_) {
        if (old.groupId != group.groupId) continue;
        if (!group.name.isEmpty() && group.name != group.groupId) old.name = group.name;
        if (group.memberCount > 0) old.memberCount = group.memberCount;
        // A partial create stays recoverable until activation succeeds or the user sends text.
        old.pendingActivation = old.pendingActivation || group.pendingActivation;
        if (group.unreadCount >= 0 && selectedGroupId_ != group.groupId &&
            !groupUnreadFromConversation_.contains(group.groupId))
            groupUnreadCounts_[group.groupId] = group.unreadCount;
        if (database_) database_->upsertGroup(old);
        return;
    }
    groups_.append(group);
    if (group.unreadCount >= 0 && selectedGroupId_ != group.groupId &&
        !groupUnreadFromConversation_.contains(group.groupId))
        groupUnreadCounts_[group.groupId] = group.unreadCount;
    if (database_) database_->upsertGroup(group);
    if (database_ && !groupMessages_.contains(group.groupId)) {
        groupMessages_[group.groupId] = database_->groupMessagesBefore(
            group.groupId, std::numeric_limits<qint64>::max(), QStringLiteral("~"), kGroupPageSize);
        groupHasEarlierLocal_[group.groupId] = database_->groupMessageCount(group.groupId) >
            groupMessages_.value(group.groupId).size();
    }
    if (pendingGroupPreviews_.contains(group.groupId))
        ingestGroupMessages({pendingGroupPreviews_.take(group.groupId)}, false);
}

void RemoteIMApplication::selectGroup(const QString& groupId) {
    const QString id = groupId.trimmed();
    if (std::none_of(groups_.cbegin(), groups_.cend(), [&id](const RemoteIMGroup& g) {
            return g.groupId == id;
        })) return;
    setHumanTypingActive(false);
    selectedGroupId_ = id;
    groupUnreadCounts_.remove(id);
    client_->markGroupRead(id, {});
    emit groupsChanged();
    if (!groupInitialHistoryLoaded_.contains(id)) fetchGroupHistoryPage(id, 0);
}

void RemoteIMApplication::clearSelectedGroup() {
    selectedGroupId_.clear();
}

void RemoteIMApplication::refreshGroups() {
    if (!connected_ || groupRefreshInFlight_) return;
    groupRefreshInFlight_ = true;
    const QPointer<RemoteIMApplication> self(this);
    client_->joinedGroups([self](bool ok, const QString& error,
                                 const QList<RemoteIMGroup>& joined) {
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, ok, error, joined] {
            if (!self) return;
            self->groupRefreshInFlight_ = false;
            if (!ok) {
                emit self->errorMessage(error.isEmpty() ? QStringLiteral("群列表加载失败") : error);
                return;
            }
            QSet<QString> ids;
            for (const RemoteIMGroup& group : joined) {
                ids.insert(group.groupId);
                self->mergeGroup(group);
            }
            for (int index = self->groups_.size() - 1; index >= 0; --index) {
                const RemoteIMGroup& group = self->groups_[index];
                if (ids.contains(group.groupId) || group.pendingActivation) continue;
                if (self->selectedGroupId_ == group.groupId) self->selectedGroupId_.clear();
                if (self->database_) self->database_->removeGroup(group.groupId);
                self->groupMessages_.remove(group.groupId);
                self->pendingGroupPreviews_.remove(group.groupId);
                self->groupUnreadCounts_.remove(group.groupId);
                self->groupUnreadFromConversation_.remove(group.groupId);
                self->groups_.removeAt(index);
            }
            emit self->groupsChanged();
        }, Qt::AutoConnection);
    });
}

void RemoteIMApplication::createGroup(const QString& name, const QStringList& memberIds,
                                       RemoteIMGroupCompletion completion) {
    if (!connected_) {
        if (completion) completion(false, QStringLiteral("IM 尚未连接"), {});
        return;
    }
    QSet<QString> members;
    const auto contacts = state_.contacts();
    for (const QString& raw : memberIds) {
        const QString id = raw.trimmed();
        if (id.isEmpty() || id == state_.ownerUserId() || members.contains(id) ||
            !isAcceptedFriend(id) ||
            std::none_of(contacts.cbegin(), contacts.cend(), [&id](const RemoteIMContact& contact) {
                return contact.userId == id;
            })) {
            if (completion) completion(false, QStringLiteral("群成员必须是已接受的好友，且不能重复或包含自己"), {});
            return;
        }
        members.insert(id);
    }
    if (members.isEmpty() || members.size() > 19 || name.trimmed().isEmpty() ||
        name.trimmed().toUtf8().size() > 100) {
        if (completion) completion(false, QStringLiteral("群名称需在 100 字节内，且须选择 1 至 19 位好友"), {});
        return;
    }
    const QPointer<RemoteIMApplication> self(this);
    client_->createGroup(name, memberIds,
        [self, completion = std::move(completion)](bool ok, const QString& error,
                                                  const RemoteIMGroup& result) mutable {
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, ok, error, result,
                                         completion = std::move(completion)]() mutable {
            if (!self) return;
            if (!result.groupId.isEmpty()) {
                RemoteIMGroup group = result;
                group.pendingActivation = !ok;
                self->mergeGroup(group);
                if (ok) self->selectGroup(group.groupId);
                emit self->groupsChanged();
            }
            if (completion) completion(ok, error, result);
        }, Qt::AutoConnection);
    });
}

bool RemoteIMApplication::sendGroupText(const QString& groupId, const QString& text) {
    const QString id = groupId.trimmed();
    const QString content = text.trimmed();
    if (!connected_ || id.isEmpty() || content.isEmpty() ||
        std::none_of(groups_.cbegin(), groups_.cend(), [&id](const RemoteIMGroup& g) {
            return g.groupId == id;
        })) return false;
    RemoteIMGroupMessage message;
    message.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    message.groupId = id;
    message.senderId = state_.ownerUserId();
    message.text = content;
    message.createdAtMillis = QDateTime::currentMSecsSinceEpoch();
    message.fromSelf = true;
    message.pending = true;
    groupMessages_[id].append(message);
    if (database_) database_->insertGroupMessageIfAbsent(message);
    emit groupsChanged();
    const QPointer<RemoteIMApplication> self(this);
    client_->sendGroupText(id, content, [self, id, localId = message.id](
        bool ok, const QString& error, const RemoteIMSendReceipt& receipt) {
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, id, localId, ok, error, receipt] {
            if (!self) return;
            auto& messages = self->groupMessages_[id];
            for (RemoteIMGroupMessage& existing : messages) {
                if (existing.id != localId) continue;
                existing.pending = false;
                existing.failed = !ok;
                if (ok && !receipt.remoteMessageId.isEmpty()) existing.id = receipt.remoteMessageId;
                if (ok && receipt.createdAtMillis > 0) existing.createdAtMillis = receipt.createdAtMillis;
                if (self->database_) {
                    self->database_->adoptGroupMessageId(localId,
                        ok ? receipt.remoteMessageId : QString(), existing.createdAtMillis, existing.sequence);
                    self->database_->updateGroupMessageStatus(existing.id, false, !ok);
                }
                break;
            }
            if (ok && !receipt.remoteMessageId.isEmpty()) {
                // A self echo can arrive before the send receipt. If its server time differs
                // from our local clock, the provisional row may not have matched the echo.
                // Both rows then acquire the same SDK ID; retain the echo with its sequence.
                int keeper = -1;
                for (int index = 0; index < messages.size(); ++index) {
                    if (messages.at(index).id != receipt.remoteMessageId) continue;
                    if (keeper < 0) {
                        keeper = index;
                        continue;
                    }
                    if (messages.at(index).sequence > messages.at(keeper).sequence) {
                        messages.removeAt(keeper);
                        keeper = index - 1;
                    } else {
                        messages.removeAt(index);
                    }
                    --index;
                }
                std::stable_sort(messages.begin(), messages.end(), groupMessageLess);
            }
            if (ok) {
                for (RemoteIMGroup& group : self->groups_) {
                    if (group.groupId != id || !group.pendingActivation) continue;
                    group.pendingActivation = false;
                    if (self->database_) self->database_->upsertGroup(group);
                    break;
                }
            } else {
                emit self->errorMessage(error.isEmpty() ? QStringLiteral("群消息发送失败") : error);
            }
            emit self->groupsChanged();
        }, Qt::AutoConnection);
    });
    return true;
}

void RemoteIMApplication::ingestGroupMessages(const QList<RemoteIMGroupMessage>& messages, bool live) {
    bool changed = false;
    for (const RemoteIMGroupMessage& message : messages) {
        if (message.id.isEmpty() || message.groupId.isEmpty() || message.text.trimmed().isEmpty()) continue;
        if (std::none_of(groups_.cbegin(), groups_.cend(), [&message](const RemoteIMGroup& group) {
                return group.groupId == message.groupId;
            })) {
            if (pendingGroupPreviews_.size() < 50 || pendingGroupPreviews_.contains(message.groupId))
                pendingGroupPreviews_[message.groupId] = message;
            refreshGroups();
            continue;
        }
        auto& items = groupMessages_[message.groupId];
        if (std::any_of(items.cbegin(), items.cend(), [&message](const RemoteIMGroupMessage& old) {
                return old.id == message.id;
            })) continue;
        if (message.fromSelf) {
            auto pending = std::find_if(items.begin(), items.end(), [&message](const RemoteIMGroupMessage& old) {
                return old.pending && old.fromSelf && old.text == message.text &&
                    qAbs(old.createdAtMillis - message.createdAtMillis) < 5000;
            });
            if (pending != items.end()) {
                const QString localId = pending->id;
                *pending = message;
                if (database_) {
                    database_->adoptGroupMessageId(localId, message.id,
                        message.createdAtMillis, message.sequence);
                    database_->updateGroupMessageStatus(message.id, false, false);
                }
                changed = true;
                continue;
            }
        }
        const bool fresh = database_ ? database_->insertGroupMessageIfAbsent(message) : true;
        items.append(message);
        changed = true;
        if (live && fresh && !message.fromSelf) {
            if (selectedGroupId_ == message.groupId) client_->markGroupRead(message.groupId, {});
            else groupUnreadCounts_[message.groupId] += 1;
        }
    }
    if (changed) {
        for (auto& items : groupMessages_) std::stable_sort(items.begin(), items.end(), groupMessageLess);
        emit groupsChanged();
    }
}

void RemoteIMApplication::fetchGroupHistoryPage(const QString& groupId, qint64 beforeSequence) {
    if (!connected_ || groupHistoryInFlight_.contains(groupId)) return;
    groupHistoryInFlight_.insert(groupId);
    const QPointer<RemoteIMApplication> self(this);
    client_->groupHistory(groupId, kGroupPageSize, beforeSequence,
        [self, groupId](bool ok, const QString& error, const RemoteIMGroupHistoryPage& page) {
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, groupId, ok, error, page] {
            if (!self) return;
            self->groupHistoryInFlight_.remove(groupId);
            if (!ok) {
                emit self->errorMessage(error.isEmpty() ? QStringLiteral("群历史加载失败") : error);
                emit self->groupHistoryFinished(groupId, false);
                return;
            }
            self->groupInitialHistoryLoaded_.insert(groupId);
            self->groupHasEarlierRemote_[groupId] = page.hasMore;
            self->groupOlderSequence_[groupId] = page.nextSequence;
            self->ingestGroupMessages(page.messages, false);
            emit self->groupsChanged();
            emit self->groupHistoryFinished(groupId, true);
        }, Qt::AutoConnection);
    });
}

void RemoteIMApplication::loadEarlierGroupMessages(const QString& groupId) {
    if (groupHistoryInFlight_.contains(groupId)) return;
    auto& items = groupMessages_[groupId];
    if (database_ && !items.isEmpty() && groupHasEarlierLocal_.value(groupId)) {
        const auto& oldest = items.first();
        const auto page = database_->groupMessagesBefore(groupId, oldest.createdAtMillis,
                                                         oldest.id, kGroupPageSize);
        groupHasEarlierLocal_[groupId] = page.size() == kGroupPageSize;
        for (const auto& message : page) {
            if (std::none_of(items.cbegin(), items.cend(), [&message](const auto& old) {
                    return old.id == message.id;
                })) items.prepend(message);
        }
        if (!page.isEmpty() && page.first().sequence > 1) {
            const qint64 next = page.first().sequence - 1;
            if (groupOlderSequence_.value(groupId) == 0 ||
                groupOlderSequence_.value(groupId) > next) groupOlderSequence_[groupId] = next;
        }
        emit groupsChanged();
        if (!page.isEmpty()) return;
    }
    if (groupHasEarlierRemote_.value(groupId) && groupOlderSequence_.value(groupId) > 0)
        fetchGroupHistoryPage(groupId, groupOlderSequence_.value(groupId));
}

bool RemoteIMApplication::createContactGroup(const QString& name) {
    // 先落库再改内存：库里主键冲突（重名）是权威判据，内存不必自己再查一遍。
    if (database_ && !database_->createContactGroup(name)) return false;
    if (!state_.addContactGroup(name)) return false;
    emit stateChanged();
    return true;
}

bool RemoteIMApplication::renameContactGroup(const QString& from, const QString& to) {
    if (database_ && !database_->renameContactGroup(from, to)) return false;
    if (!state_.renameContactGroup(from, to)) return false;
    emit stateChanged();
    return true;
}

void RemoteIMApplication::deleteContactGroup(const QString& name) {
    if (database_) database_->deleteContactGroup(name);
    state_.removeContactGroup(name);
    emit stateChanged();
}

void RemoteIMApplication::setContactGroup(const QString& userId, const QString& groupName) {
    if (database_) database_->setContactGroup(userId, groupName);
    state_.setContactGroup(userId, groupName);
    emit stateChanged();
}

const ChatState& RemoteIMApplication::chatState() const { return state_; }
ChatState& RemoteIMApplication::chatState() { return state_; }
RemoteIMClient& RemoteIMApplication::client() { return *client_; }
bool RemoteIMApplication::isConnected() const { return connected_; }
RemoteIMActivitySignal RemoteIMApplication::activityForPeer(const QString& peerId) const {
    return activityByPeer_.value(peerId.trimmed());
}

void RemoteIMApplication::rememberClosedActivity(const QString& identity) {
    if (!closedActivities_.contains(identity)) closedActivities_.append(identity);
    if (closedActivities_.size() > 256) closedActivities_.removeFirst();
}

void RemoteIMApplication::applyActivity(const QString& peerId,
                                        const RemoteIMActivitySignal& signal) {
    const QString cleanPeerId = peerId.trimmed();
    bool knownPeer = false;
    for (const auto& contact : state_.contacts()) {
        if (contact.userId == cleanPeerId) knownPeer = true;
    }
    if (!knownPeer || cleanPeerId == state_.ownerUserId()) return;
    const QString identity = cleanPeerId + QLatin1Char('|') + signal.activityId;
    if (closedActivities_.contains(identity)) return;
    if (signal.sequence <= activitySequences_.value(identity, -1)) return;
    if (activitySequences_.size() > 512) activitySequences_.clear();
    activitySequences_.insert(identity, signal.sequence);
    const auto old = activityByPeer_.value(cleanPeerId);
    if (!signal.active) {
        rememberClosedActivity(identity);
        if (old.activityId == signal.activityId) clearActivity(cleanPeerId);
        return;
    }
    if (!old.activityId.isEmpty() && old.activityId != signal.activityId) {
        rememberClosedActivity(cleanPeerId + QLatin1Char('|') + old.activityId);
    }
    if (QTimer* timer = activityExpiryTimers_.take(cleanPeerId)) { timer->stop(); timer->deleteLater(); }
    activityByPeer_.insert(cleanPeerId, signal);
    auto* expiry = new QTimer(this);
    expiry->setSingleShot(true);
    expiry->setInterval(qBound(1000, signal.ttlMs, 30000));
    connect(expiry, &QTimer::timeout, this, [this, cleanPeerId, expiry] {
        if (activityExpiryTimers_.value(cleanPeerId) != expiry) return;
        activityExpiryTimers_.remove(cleanPeerId);
        activityByPeer_.remove(cleanPeerId);
        emit activityChanged(cleanPeerId);
        expiry->deleteLater();
    });
    activityExpiryTimers_.insert(cleanPeerId, expiry);
    expiry->start();
    if (old.activityId != signal.activityId || old.kind != signal.kind) emit activityChanged(cleanPeerId);
}

void RemoteIMApplication::clearActivity(const QString& peerId) {
    const QString cleanPeerId = peerId.trimmed();
    if (QTimer* timer = activityExpiryTimers_.take(cleanPeerId)) { timer->stop(); timer->deleteLater(); }
    const auto old = activityByPeer_.take(cleanPeerId);
    if (!old.activityId.isEmpty()) {
        rememberClosedActivity(cleanPeerId + QLatin1Char('|') + old.activityId);
        emit activityChanged(cleanPeerId);
    }
}

void RemoteIMApplication::setHumanTypingActive(bool active) {
    const QString peerId = state_.selectedPeerId().trimmed();
    if (!active || peerId.isEmpty()) {
        typingHeartbeat_->stop();
        typingIdle_->stop();
        if (!typingPeerId_.isEmpty() && !typingActivityId_.isEmpty()) {
            client_->sendActivity(typingPeerId_,
                                  RemoteIMActivitySignal{typingActivityId_,
                                                         RemoteIMActivityKind::HumanTyping,
                                                         false, 1000, ++typingSequence_}, {});
        }
        typingPeerId_.clear();
        typingActivityId_.clear();
        return;
    }
    if (!connected_) return;
    if (typingHeartbeat_->isActive() && typingPeerId_ == peerId) {
        typingIdle_->start();
        return;
    }
    setHumanTypingActive(false);
    typingPeerId_ = peerId;
    typingSequence_ = 0;
    typingActivityId_ = QStringLiteral("typing:") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    client_->sendActivity(typingPeerId_,
                          RemoteIMActivitySignal{typingActivityId_,
                                                 RemoteIMActivityKind::HumanTyping,
                                                 true, 12000, ++typingSequence_}, {});
    typingHeartbeat_->start();
    typingIdle_->start();
}

bool RemoteIMApplication::hasEarlierMessages(const QString& peerId) const {
    return hasEarlierMessages_.value(peerId.trimmed(), false);
}

bool RemoteIMApplication::loadRecentPeerHistory(const QString& peerId) {
    if (recentHistoryLoadedPeers_.contains(peerId) || !database_ || !database_->isOpen()) return false;
    recentHistoryLoadedPeers_.insert(peerId);
    QList<RemoteIMMessage> recent = database_->loadRecentMessages(peerId, kMessagesPageSize + 1);
    const bool hasEarlier = recent.size() > kMessagesPageSize;
    if (hasEarlier) recent.removeFirst();
    const int previousCount = state_.messagesWith(peerId).size();
    state_.appendDatabaseHistoryPage(recent);
    if (!recent.isEmpty())
        historyPageCursors_.insert(peerId, {recent.first().createdAtMillis, recent.first().id});
    hasEarlierMessages_.insert(peerId, hasEarlier);
    return state_.messagesWith(peerId).size() != previousCount;
}

int RemoteIMApplication::loadEarlierMessages(const QString& peerId) {
    const QString peer = peerId.trimmed();
    if (peer.isEmpty() || !database_) return 0;
    const int previousCount = state_.messagesWith(peer).size();
    if (loadRecentPeerHistory(peer)) {
        // Direct callers may open a preview-only conversation through this
        // entry point. Publish that first page even when no older page exists.
        emit stateChanged();
        return state_.messagesWith(peer).size() - previousCount;
    }
    if (!hasEarlierMessages_.value(peer, false) || !historyPageCursors_.contains(peer)) return 0;
    const auto cursor = historyPageCursors_.value(peer);
    QList<RemoteIMMessage> earlier =
        database_->loadMessagesBefore(peer, cursor.first, cursor.second, kMessagesPageSize + 1);
    const bool hasEarlier = earlier.size() > kMessagesPageSize;
    if (hasEarlier) earlier.removeFirst();
    state_.appendDatabaseHistoryPage(earlier);
    if (!earlier.isEmpty())
        historyPageCursors_.insert(peer, {earlier.first().createdAtMillis, earlier.first().id});
    hasEarlierMessages_.insert(peer, hasEarlier);
    if (!earlier.isEmpty()) emit stateChanged();
    return earlier.size();
}

void RemoteIMApplication::connectToService(int sdkAppId, const QString& userSig) {
    const quint64 generation = ++connectionGeneration_;
    messageQueryGeneration_->fetch_add(1);
    connected_ = false;
    acceptedFriendsLoaded_ = false;
    metadataReceipts_.clear();
    firstMetadataRows_.clear();
    metadataCompletedSequence_ = 0;
    completedMetadataBatches_.clear();
    metadataPersistenceError_.clear();
    const QPointer<RemoteIMApplication> self(this);
    emit connectionChanged(false);
    if (!self) return;
    drainIMHistoryFences();
    if (!self) return;
    client_->connectToService(sdkAppId, state_.ownerUserId(), userSig, [self, generation](bool ok, const QString& error) {
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, generation, ok, error] {
            if (!self || self->connectionGeneration_ != generation || self->disconnecting_) return;
            self->connected_ = ok;
            emit self->connectionChanged(ok);
            if (!self) return;
            if (ok) { self->refreshGroups(); self->refreshFriendList(); }
            if (self && !ok) emit self->errorMessage(error.isEmpty() ? QStringLiteral("IM 连接失败") : error);
        }, Qt::AutoConnection);
    });
}

// SDK 退出实行单飞：签名轮换与会话撤销撞在同一时刻时，只向 SDK 发一次 logout。
// 回调切回 Qt 所属线程后更新连接状态，再交给调用方决定重连或退出。
void RemoteIMApplication::disconnectFromService(RemoteIMCompletion completion) {
    if (completion) disconnectCallbacks_.push_back(std::move(completion));
    if (disconnecting_) return;
    disconnecting_ = true;
    ++connectionGeneration_;
    messageQueryGeneration_->fetch_add(1);
    const QPointer<RemoteIMApplication> self(this);
    acceptedFriendsLoaded_ = false;
    metadataReceipts_.clear();
    firstMetadataRows_.clear();
    drainIMHistoryFences();
    if (!self) return;
    client_->disconnectFromService([self](bool ok, const QString& error) {
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, ok, error] {
            if (!self) return;
            self->connected_ = false;
            self->disconnecting_ = false;
            emit self->connectionChanged(false);
            auto callbacks = std::move(self->disconnectCallbacks_);
            self->disconnectCallbacks_.clear();
            for (auto& callback : callbacks) callback(ok, error);
        }, Qt::AutoConnection);
    });
}

void RemoteIMApplication::addContact(const QString& userId, const QString& displayName) {
    state_.upsertContact(RemoteIMContact{userId, displayName});
    loadRecentPeerHistory(userId);
    state_.selectPeer(userId);
    if (database_) database_->upsertContact(RemoteIMContact{userId, displayName});
    emit stateChanged();
}

void RemoteIMApplication::requestFriend(const QString& userId, const QString& displayName) {
    if (accountFriendOperations_.send) {
        Q_UNUSED(displayName);
        QPointer<RemoteIMApplication> self(this);
        accountFriendOperations_.send(userId.trimmed(), [self](bool ok, const QString& error) {
            if (!self) return;
            if (ok) emit self->friendRequestSent();
            else emit self->errorMessage(error.isEmpty() ?
                QStringLiteral("发送好友申请失败") : error);
        });
        return;
    }
    QString phone = userId.trimmed();
    phone.remove(QRegularExpression(QStringLiteral("[ -]")));
    static const QRegularExpression phonePattern(QStringLiteral("^(\\+86)?1[3-9][0-9]{9}$"));
    if (phonePattern.match(phone).hasMatch()) {
        if (!friendAccountResolver_) {
            emit errorMessage(QStringLiteral("当前账号无法通过手机号查找好友"));
            return;
        }
        QPointer<RemoteIMApplication> self(this);
        friendAccountResolver_(phone, [self, displayName](const QString& peerId,
                                                         const QString& error) {
            if (!self) return;
            QMetaObject::invokeMethod(self, [self, peerId, displayName, error] {
                if (!self) return;
                if (!error.isEmpty()) { emit self->errorMessage(error); return; }
                self->requestFriendById(peerId, displayName);
            }, Qt::AutoConnection);
        });
        return;
    }
    requestFriendById(userId.trimmed(), displayName);
}

void RemoteIMApplication::setFriendAccountResolver(FriendAccountResolver resolver) {
    friendAccountResolver_ = std::move(resolver);
}

void RemoteIMApplication::setAccountFriendOperations(AccountFriendOperations operations) {
    accountFriendOperations_ = std::move(operations);
    acceptedFriendsLoaded_ = false;
    refreshFriendList();
}

void RemoteIMApplication::setNicknameUpdater(
    std::function<void(const QString&, RemoteIMCompletion)> updater) {
    nicknameUpdater_ = std::move(updater);
}

void RemoteIMApplication::setAccountProfileProvider(std::function<AccountProfile()> provider) {
    messageQueryGeneration_->fetch_add(1);
    accountProfileProvider_ = std::move(provider);
}

RemoteIMApplication::AccountProfile RemoteIMApplication::currentAccountProfile() const {
    AccountProfile result = accountProfileProvider_ ? accountProfileProvider_() : AccountProfile{};
    if (!result.loggedIn || result.accountId.isEmpty() || result.imUserId != state_.ownerUserId())
        return {};
    // SDK 自资料推送同时覆盖本机修改与其它设备修改；不从鉴权响应臆造 faceURL。
    result.avatarUrl = selfProfile_.avatarUrl;
    return result;
}

void RemoteIMApplication::setAvatarAccessProvider(std::function<AvatarAccess()> provider) {
    avatarAccessProvider_ = std::move(provider);
}

void RemoteIMApplication::updateAvatar(const QString &path,
                                       std::shared_ptr<std::atomic_bool> canceled,
                                       AvatarCompletion completion) {
    const AccountProfile profile = currentAccountProfile();
    const AvatarAccess access = avatarAccessProvider_ ? avatarAccessProvider_() : AvatarAccess{};
    if (!profile.loggedIn || !connected_ || access.accountId != profile.accountId ||
        access.imUserId != profile.imUserId || access.sessionIdentity.isEmpty() ||
        access.mediaBaseUrl.isEmpty() || access.accountToken.isEmpty()) {
        if (completion) completion({false, {}, QStringLiteral("not_authenticated")});
        return;
    }
    if (!canceled) canceled = std::make_shared<std::atomic_bool>(false);
    if (canceled->load()) {
        if (completion) completion({false, {}, QStringLiteral("canceled")});
        return;
    }
    // 一个账号宿主串行发布头像，避免两个已派发的 SDK 写入按相反顺序完成。
    if (avatarUpdateInFlight_) {
        if (completion) completion({false, {}, QStringLiteral("busy")});
        return;
    }
    avatarUpdateInFlight_ = true;
    struct Request {
        std::shared_ptr<MaiAvatarCancel> uploadCancel{mai_avatar_cancel_create(),
                                                      mai_avatar_cancel_destroy};
        std::shared_ptr<std::atomic_bool> canceled;
        AvatarCompletion completion;
        QMetaObject::Connection destructionConnection;
        QElapsedTimer elapsed;
        bool finished = false;
        bool sdkDispatched = false;
    };
    auto request = std::make_shared<Request>();
    request->canceled = std::move(canceled);
    request->completion = std::move(completion);
    request->elapsed.start();
    const quint64 connection = connectionGeneration_;
    const quint64 update = ++avatarUpdateGeneration_;
    const QPointer<RemoteIMApplication> self(this);
    auto *poll = new QTimer(this);
    const QPointer<QTimer> polling(poll);
    auto identityMatches = [self, access, connection, update] {
        if (!self || !self->connected_ || self->disconnecting_ ||
            self->connectionGeneration_ != connection || self->avatarUpdateGeneration_ != update ||
            !self->avatarAccessProvider_)
            return false;
        const AvatarAccess current = self->avatarAccessProvider_();
        return current.accountId == access.accountId && current.imUserId == access.imUserId &&
               current.sessionIdentity == access.sessionIdentity;
    };
    auto finish = [self, request, polling, update](const AvatarUpdateResult &result) {
        if (request->finished) return;
        request->finished = true;
        QObject::disconnect(request->destructionConnection);
        if (self && self->avatarUpdateGeneration_ == update) self->avatarUpdateInFlight_ = false;
        mai_avatar_cancel_request(request->uploadCancel.get());
        if (polling) {
            polling->stop();
            polling->deleteLater();
        }
        auto done = std::move(request->completion);
        if (done) done(result);
    };
    connect(poll, &QTimer::timeout, this, [request, identityMatches, finish] {
        if (!identityMatches()) finish({false, {}, QStringLiteral("identity_changed")});
        else if (request->canceled->load() && !request->sdkDispatched)
            finish({false, {}, QStringLiteral("canceled")});
        else if (request->elapsed.elapsed() >= 90000)
            finish({false, {}, QStringLiteral("timeout")});
    });
    poll->start(50);
    // 句柄活到工作线程结束；App 销毁也先取消，后台任务不能持有 Qt 宿主裸指针。
    request->destructionConnection = connect(this, &QObject::destroyed, [request] {
        request->finished = true;
        request->completion = {};
        mai_avatar_cancel_request(request->uploadCancel.get());
    });
    QFile certificates(QStringLiteral(":/maichat/auth/cacert.pem"));
    const QByteArray ca =
        certificates.open(QIODevice::ReadOnly) ? certificates.readAll() : QByteArray{};
    auto *watcher = new QFutureWatcher<AvatarUploadOutcome>(this);
    connect(watcher, &QFutureWatcher<AvatarUploadOutcome>::finished, this,
            [self, watcher, request, identityMatches, finish] {
                const AvatarUploadOutcome uploaded = watcher->result();
                watcher->deleteLater();
                if (!self || request->finished) return;
                if (!identityMatches()) {
                    finish({false, {}, QStringLiteral("identity_changed")});
                    return;
                }
                if (request->canceled->load()) {
                    finish({false, {}, QStringLiteral("canceled")});
                    return;
                }
                if (uploaded.url.isEmpty()) {
                    finish({false, {}, uploaded.error});
                    return;
                }
                // 最后一刻在 SDK 所属 GUI 线程再次检查原账号；上传成功不等于资料已发布。
                self->client_->setSelfAvatar(
                    uploaded.url,
                    [self, request, identityMatches, finish, url = uploaded.url](bool ok,
                                                                                 const QString &) {
                        if (!self || request->finished) return;
                        QMetaObject::invokeMethod(
                            self,
                            [request, identityMatches, finish, url, ok] {
                                if (request->finished) return;
                                if (!identityMatches())
                                    finish({false, {}, QStringLiteral("identity_changed")});
                                else
                                    finish(ok ? AvatarUpdateResult{true, url, {}}
                                              : AvatarUpdateResult{
                                                    false,
                                                    {},
                                                    QStringLiteral("profile_update_failed")});
                            },
                            Qt::AutoConnection);
                    },
                    [request, identityMatches] {
                        if (request->finished || !identityMatches() ||
                            (!request->sdkDispatched && request->canceled->load()))
                            return false;
                        // SDK 派发后不能撤回远端资料修改；等待实际回执，避免成功后因取消改报失败。
                        request->sdkDispatched = true;
                        return true;
                    });
            });
    watcher->setFuture(QtConcurrent::run([path, access, ca, request] {
        return uploadAvatar(path, access, ca, request->uploadCancel);
    }));
}

bool RemoteIMApplication::updateNickname(const QString& nickname,
                                         RemoteIMCompletion completion) {
    if (!nicknameUpdater_) {
        if (completion) completion(false, QStringLiteral("MaiChat 账号资料不可用"));
        return false;
    }
    nicknameUpdater_(nickname, std::move(completion));
    return true;
}

void RemoteIMApplication::setPasswordChangeHandler(
    std::function<QJsonObject(const QJsonObject&)> handler) {
    passwordChangeHandler_ = std::move(handler);
}

QJsonObject RemoteIMApplication::requestPasswordChange(const QJsonObject& arguments) {
    return passwordChangeHandler_ ? passwordChangeHandler_(arguments) : QJsonObject{};
}

void RemoteIMApplication::requestFriendById(const QString& userId,
                                            const QString& displayName) {
    Q_UNUSED(displayName);
    QPointer<RemoteIMApplication> self(this);
    client_->requestFriend(userId, [self](bool ok, const QString& error) {
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, ok, error] {
            if (!self) return;
            if (!ok) {
                emit self->errorMessage(error.isEmpty() ? QStringLiteral("发送好友申请失败") : error);
                return;
            }
            emit self->friendRequestSent();
        }, Qt::AutoConnection);
    });
}

void RemoteIMApplication::refreshFriendList() {
    if (accountFriendOperations_.list) {
        QPointer<RemoteIMApplication> self(this);
        const auto generation = connectionGeneration_;
        accountFriendOperations_.list([self, generation](const QList<AccountFriend>& friends,
                                             const QString& error) {
            if (!self) return;
            if (!error.isEmpty()) {
                emit self->errorMessage(QStringLiteral("好友列表加载失败：%1").arg(error));
                return;
            }
            QMetaObject::invokeMethod(self, [self, generation, friends] {
                if (!self || self->connectionGeneration_ != generation) return;
                self->acceptedFriendsLoaded_ = true;
                self->acceptedFriendIds_.clear();
                self->friendAccountByImId_.clear();
                self->friendDisplayByImId_.clear();
                for (const AccountFriend& friendAccount : friends) {
                    if (friendAccount.imUserId.isEmpty() || friendAccount.accountId.isEmpty()) continue;
                    self->acceptedFriendIds_.insert(friendAccount.imUserId);
                    self->friendAccountByImId_.insert(friendAccount.imUserId,
                                                     friendAccount.accountId);
                    const QString displayName = friendAccount.nickname.trimmed().isEmpty() ?
                        friendAccount.accountId : friendAccount.nickname.trimmed();
                    self->friendDisplayByImId_.insert(friendAccount.imUserId, displayName);
                    bool exists = false;
                    RemoteIMContact current;
                    for (const RemoteIMContact& contact : self->state_.contacts()) {
                        if (contact.userId == friendAccount.imUserId) {
                            exists = true;
                            current = contact;
                            break;
                        }
                    }
                    if (!exists) current.userId = friendAccount.imUserId;
                    current.displayName = displayName;
                    self->state_.upsertContact(current);
                    self->state_.setContactDisplayName(current.userId, displayName);
                    if (self->database_) self->database_->upsertContact(current);
                }
                emit self->stateChanged();
                if (self) emit self->acceptedFriendsChanged();
                // Account service owns friendship and display names; it has no
                // SDK face URL. Refresh that independent metadata through the
                // ordinary client path after publishing the current relation.
                if (self && self->connectionReady() && self->connectionGeneration_ == generation)
                    self->client_->refreshFriendList();
            }, Qt::AutoConnection);
        });
        return;
    }
    client_->refreshFriendList();
}

void RemoteIMApplication::friendRequests(RemoteIMFriendRequestsCompletion completion) {
    if (accountFriendOperations_.requests) {
        QPointer<RemoteIMApplication> self(this);
        accountFriendOperations_.requests([self, completion = std::move(completion)](
            const QList<AccountFriendRequest>& requests, const QString& error) {
            if (!self) return;
            QMetaObject::invokeMethod(self, [self, completion, requests, error] {
                if (!self) return;
                self->requestIdByImId_.clear();
                QList<RemoteIMFriendRequest> display;
                for (const AccountFriendRequest& request : requests) {
                    self->requestIdByImId_.insert(request.imUserId, request.id);
                    display.append(RemoteIMFriendRequest{request.imUserId,
                        request.nickname.trimmed().isEmpty() ? request.accountId :
                            request.nickname.trimmed(), {}, {}});
                }
                if (completion) completion(error.isEmpty(), error, display);
            }, Qt::AutoConnection);
        });
        return;
    }
    QPointer<RemoteIMApplication> self(this);
    client_->friendRequests([self, completion = std::move(completion)](
                                bool ok, const QString& error,
                                const QList<RemoteIMFriendRequest>& requests) {
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, completion, ok, error, requests] {
            if (self && completion) completion(ok, error, requests);
        }, Qt::AutoConnection);
    });
}

void RemoteIMApplication::decideFriendRequest(const QString& userId, bool accept,
                                              RemoteIMCompletion completion) {
    if (accountFriendOperations_.decide) {
        const QString requestId = requestIdByImId_.value(userId);
        if (requestId.isEmpty()) {
            if (completion) completion(false, QStringLiteral("好友申请已失效，请刷新"));
            return;
        }
        QPointer<RemoteIMApplication> self(this);
        accountFriendOperations_.decide(requestId, accept,
            [self, userId, accept, completion = std::move(completion)](
                bool ok, const QString& error) {
            if (!self) return;
            if (ok) {
                self->requestIdByImId_.remove(userId);
                if (accept) self->refreshFriendList();
            }
            if (completion) completion(ok, error);
        });
        return;
    }
    QPointer<RemoteIMApplication> self(this);
    client_->decideFriendRequest(userId, accept,
                                 [self, userId, accept, completion = std::move(completion)](
                                     bool ok, const QString& error) {
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, userId, accept, completion, ok, error] {
            if (!self) return;
            if (ok && accept) {
                self->state_.upsertContact(RemoteIMContact{userId, userId});
                if (self->database_) self->database_->upsertContact(RemoteIMContact{userId, userId});
                emit self->stateChanged();
            }
            if (completion) completion(ok, error);
        }, Qt::AutoConnection);
    });
}

void RemoteIMApplication::deleteContact(const QString& userId) {
    const QString cleanUserId = userId.trimmed();
    if (cleanUserId.isEmpty()) return;
    if (accountFriendOperations_.remove) {
        const QString accountId = friendAccountByImId_.value(cleanUserId);
        if (accountId.isEmpty()) {
            emit errorMessage(QStringLiteral("好友账号尚未同步"));
            return;
        }
        QPointer<RemoteIMApplication> self(this);
        accountFriendOperations_.remove(accountId,
            [self, cleanUserId](bool ok, const QString& error) {
            if (!self) return;
            if (!ok) { emit self->errorMessage(error); return; }
            self->acceptedFriendIds_.remove(cleanUserId);
            self->friendAccountByImId_.remove(cleanUserId);
            self->friendDisplayByImId_.remove(cleanUserId);
            // 本地会话和历史不属于好友关系，删除好友时保留。
            emit self->stateChanged();
        });
        return;
    }
    client_->deleteContact(cleanUserId, [this, cleanUserId](bool ok, const QString& error) {
        if (!ok) {
            emit errorMessage(error.isEmpty() ? QStringLiteral("删除好友失败") : error);
            return;
        }
        state_.removeContactAndMessages(cleanUserId);
        if (database_) database_->removeContactCascade(cleanUserId);
        recentHistoryLoadedPeers_.remove(cleanUserId);
        historyPageCursors_.remove(cleanUserId);
        hasEarlierMessages_.remove(cleanUserId);
        loadRecentPeerHistory(state_.selectedPeerId());
        emit stateChanged();
    });
}

void RemoteIMApplication::clearMessagesWith(const QString& userId) {
    const QString peerId = userId.trimmed();
    if (peerId.isEmpty()) return;
    state_.removeMessagesWith(peerId);
    if (database_) database_->removeMessagesForPeer(peerId);
    // 本地库已清空，该会话不再有「更早的消息」可翻。
    hasEarlierMessages_[peerId] = false;
    historyPageCursors_.remove(peerId);
    emit stateChanged();
}

void RemoteIMApplication::selectPeer(const QString& userId) {
    const QString peerId = userId.trimmed();
    if (peerId.isEmpty()) return;
    const QString previousPeerId = state_.selectedPeerId();
    if (previousPeerId != peerId) setHumanTypingActive(false);
    const int previousUnreadCount = state_.unreadCount(peerId);
    bool knownContact = false;
    for (const RemoteIMContact& contact : state_.contacts()) {
        if (contact.userId == peerId) {
            knownContact = true;
            break;
        }
    }

    const bool loadedHistory = loadRecentPeerHistory(peerId);
    state_.selectPeer(peerId);
    if (!knownContact) {
        // selectPeer 会为未知账号补联系人，此时仍需刷新列表结构。
        emit stateChanged();
    } else if (previousPeerId != peerId || previousUnreadCount > 0 || loadedHistory) {
        emit selectionChanged(peerId);
    }
}

void RemoteIMApplication::sendText(const QString& text,
                                   const RemoteIMQuote& quote,
                                   bool hasQuote) {
    sendTextTo(state_.selectedPeerId(), text, quote, hasQuote);
}

bool RemoteIMApplication::sendTextTo(const QString& peerId, const QString& text,
                                     const RemoteIMQuote& quote, bool hasQuote) {
    const QString target = peerId.trimmed();
    const QString cleanText = text.trimmed();
    if (cleanText.isEmpty() || target.isEmpty() || target == state_.ownerUserId()) return false;
    setHumanTypingActive(false);
    RemoteIMMessage message = state_.queueOutgoingTextTo(target, cleanText);
    message.quote = quote;
    message.hasQuote = hasQuote;
    if (hasQuote) state_.updateMessageQuote(message.id, quote);
    persistMessage(message);
    emit stateChanged();

    client_->sendTextWithQuote(
        message.toUserId, message.text, message.quote, message.hasQuote,
        [this, messageId = message.id](bool ok, const QString& error, const RemoteIMSendReceipt& receipt) {
            // 发送成功且 SDK 给了稳定 id：内存与库同步换 id，漫游重投同一条消息
            // 时按主键去重（否则临时 UUID 与漫游 id 对不上，重启后会重复显示）。
            const QString effectiveId = adoptRemoteMessageId(messageId, ok ? receipt.remoteMessageId : QString());
            if (ok) {
                state_.updateMessageTime(effectiveId, receipt.createdAtMillis);
                if (database_) database_->updateMessageTime(effectiveId, receipt.createdAtMillis);
            }
            markMessage(effectiveId, ok ? RemoteIMMessageStatus::Sent : RemoteIMMessageStatus::Failed);
            if (!ok) emit errorMessage(error.isEmpty() ? QStringLiteral("文本消息发送失败") : error);
        });
    return true;
}

bool RemoteIMApplication::sendMachineTextTo(const QString& peerId, const QString& text,
                                            RemoteIMSendCompletion completion, std::function<bool()> canSend,
                                            const RemoteIMQuote& quote, bool hasQuote) {
    const QString target = peerId.trimmed();
    RemoteIMMessage message;
    message.text = text.trimmed();
    message.quote = quote;
    message.hasQuote = hasQuote && !quote.digest.isEmpty();
    return sendMachineMessageTo(target, message,
        [this, target, text = message.text, quote, hasQuote = message.hasQuote](RemoteIMSendCompletion done) {
            client_->sendMachineTextWithQuote(target, text, quote, hasQuote, std::move(done));
        }, std::move(completion), std::move(canSend));
}

bool RemoteIMApplication::sendMachineMediaTo(const QString& peerId, const QString& path, const QString& type,
                                             const QString& caption, RemoteIMSendCompletion completion,
                                             std::function<bool()> canSend, const RemoteIMQuote& quote, bool hasQuote,
                                             std::function<void(bool)> onDispatched) {
    const QString target = peerId.trimmed();
    if (type == QStringLiteral("video"))
        return prepareMachineVideoTo(target, path, caption, std::move(completion), std::move(canSend),
                                     quote, hasQuote, std::move(onDispatched));
    const QFileInfo file(path);
    const auto reject = [&completion, &onDispatched](const QString& error) {
        if (completion) completion(false, error, {});
        if (onDispatched) onDispatched(false);
        return false;
    };
    if (!file.isFile() || !file.isReadable() || file.size() <= 0)
        return reject(QStringLiteral("待发送的会话附件不存在或不可读取"));
    RemoteIMMessage message;
    message.text = caption.trimmed();
    message.quote = quote;
    message.hasQuote = hasQuote && !quote.digest.isEmpty();
    const QString source = file.absoluteFilePath();
    if (type == QStringLiteral("image")) {
        if (file.size() > 32ll * 1024 * 1024) return reject(QStringLiteral("图片超过 IM 的现有大小限制"));
        message.hasImage = true;
        message.image = {source, 0, 0, file.size()};
        const bool queued = sendMachineMessageTo(target, message,
            [this, target, source, message](RemoteIMSendCompletion done) {
                client_->sendMachineImageWithText(target, source, message.text, false, message.quote,
                                                 message.hasQuote, std::move(done));
            }, std::move(completion), std::move(canSend));
        if (onDispatched) onDispatched(queued);
        return queued;
    }
    if (type == QStringLiteral("audio") || type == QStringLiteral("file")) {
        if (file.size() > 512ll * 1024 * 1024) return reject(QStringLiteral("文件超过 IM 的现有大小限制"));
        message.hasFile = true;
        message.file = {source, file.fileName(), QMimeDatabase().mimeTypeForFile(file).name(), file.size()};
        const bool queued = sendMachineMessageTo(target, message,
            [this, target, source, message](RemoteIMSendCompletion done) {
                client_->sendMachineFileWithText(target, source, message.file.fileName, message.text, false,
                                                message.quote, message.hasQuote, std::move(done));
            }, std::move(completion), std::move(canSend));
        if (onDispatched) onDispatched(queued);
        return queued;
    }
    return reject(QStringLiteral("当前 IM 无法发送这种会话附件格式或大小"));
}

bool RemoteIMApplication::prepareMachineVideoTo(const QString& peerId, const QString& path, const QString& caption,
                                                RemoteIMSendCompletion completion, std::function<bool()> canSend,
                                                const RemoteIMQuote& quote, bool hasQuote,
                                                std::function<void(bool)> onDispatched) {
    const auto account = client_->currentAccount();
    const quint64 generation = connectionGeneration_;
    bool friendExists = false;
    for (const auto& contact : state_.contacts()) if (contact.userId == peerId) friendExists = true;
    if (!connectionReady() || !account.isValid() || account.ownerUserId != state_.ownerUserId() ||
        peerId == account.ownerUserId || !friendExists || !isAcceptedFriend(peerId) ||
        machineVideoPreparationAdmissions().load() >= 32 || (canSend && !canSend())) {
        if (completion) completion(false, QStringLiteral("会话视频准备不可用或已达现有队列上限"), {});
        if (onDispatched) onDispatched(false);
        return false;
    }
    struct Preparation {
        bool settled = false;
        std::shared_ptr<std::atomic<bool>> canceled = std::make_shared<std::atomic<bool>>(false);
    };
    auto state = std::make_shared<Preparation>();
    machineVideoPreparationAdmissions().fetch_add(1);
    const QPointer<RemoteIMApplication> self(this);
    const QDeadlineTimer deadline(kMachineVideoPreparationBudgetMs);
    auto* watcher = new QFutureWatcher<MachineVideoOutcome>(this);
    watcher->setObjectName(QStringLiteral("sessionImVideoPreparation"));
    auto* timer = new QTimer(watcher);
    timer->setObjectName(QStringLiteral("sessionImVideoPreparationDeadline"));
    timer->setSingleShot(true);
    const auto cancelOnDestroy = connect(this, &QObject::destroyed, [canceled = state->canceled] { canceled->store(true); });
    auto fail = [state, completion, onDispatched](const QString& error) {
        if (state->settled) return;
        state->settled = true;
        state->canceled->store(true);
        if (completion) completion(false, error, {});
        if (onDispatched) onDispatched(false);
    };
    connect(timer, &QTimer::timeout, this, [fail] { fail(QStringLiteral("会话视频准备超时，没有派发到 IM")); });
    connect(watcher, &QFutureWatcher<MachineVideoOutcome>::finished, this,
        [self, watcher, timer, state, account, generation, peerId, caption, quote, hasQuote,
         completion, canSend, onDispatched, fail, deadline, cancelOnDestroy] {
            timer->stop();
            watcher->deleteLater();
            QObject::disconnect(cancelOnDestroy);
            if (state->settled) return;
            if (!self || state->canceled->load() || deadline.hasExpired() || !self->connectionReady() ||
                self->connectionGeneration_ != generation || self->client_->currentAccount() != account ||
                (canSend && !canSend())) { fail(QStringLiteral("会话或账号已改变，视频没有派发到 IM")); return; }
            const auto prepared = watcher->result();
            if (!prepared.error.isEmpty() || !prepared.payload.isValid()) {
                fail(prepared.error.isEmpty() ? QStringLiteral("会话视频准备未完成，没有派发到 IM") : prepared.error);
                return;
            }
            state->settled = true;
            const auto payload = prepared.payload;
            RemoteIMMessage message;
            message.text = caption.trimmed();
            message.quote = quote;
            message.hasQuote = hasQuote && !quote.digest.isEmpty();
            message.hasVideo = true;
            message.video = {payload.videoPath, QFileInfo(payload.videoPath).fileName(), payload.coverPath,
                             payload.durationSeconds, payload.videoSizeBytes};
            const bool queued = self->sendMachineMessageTo(peerId, message,
                [self, peerId, payload, message](RemoteIMSendCompletion done) {
                    if (self) self->client_->sendMachineVideoWithText(peerId, payload, message.text, false,
                                                                     message.quote, message.hasQuote, std::move(done));
                    else if (done) done(false, QStringLiteral("IM host was destroyed"), {});
                }, completion, canSend);
            if (onDispatched) onDispatched(queued);
        });
    const QString coverDirectory = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("RemoteIMVideoCovers"));
    timer->start(kMachineVideoPreparationBudgetMs);
    watcher->setFuture(QtConcurrent::run(machineVideoPreparationPool(),
        [path, coverDirectory, canceled = state->canceled, deadline] {
            const auto release = qScopeGuard([] { machineVideoPreparationAdmissions().fetch_sub(1); });
            return prepareMachineVideo(path, coverDirectory, canceled, deadline);
        }));
    // Admission is distinct from SDK dispatch and delivery. The caller receives
    // a separate dispatch callback after preparation's final owner/scope guard.
    return true;
}

bool RemoteIMApplication::sendMachineMessageTo(const QString& peerId, RemoteIMMessage message,
                                               std::function<void(RemoteIMSendCompletion)> dispatch,
                                               RemoteIMSendCompletion completion, std::function<bool()> canSend) {
    const QString target = peerId.trimmed();
    const auto account = client_->currentAccount();
    const quint64 generation = connectionGeneration_;
    bool friendExists = false;
    for (const auto& contact : state_.contacts()) if (contact.userId == target) friendExists = true;
    if (!connectionReady() || !account.isValid() || account.ownerUserId != state_.ownerUserId() ||
        target == account.ownerUserId || target.isEmpty() ||
        (message.text.isEmpty() && !message.hasImage && !message.hasVideo && !message.hasFile) ||
        !friendExists || !isAcceptedFriend(target) || (canSend && !canSend())) {
        if (completion) completion(false, QStringLiteral("当前账号或好友会话已不可用"), {});
        return false;
    }
    message.fromUserId = account.ownerUserId;
    message.toUserId = target;
    message.direction = RemoteIMMessageDirection::Outgoing;
    message.status = RemoteIMMessageStatus::Pending;
    message.origin = RemoteIMMessageOrigin::Machine;
    message.createdAtMillis = (message.createdAtMillis / 1000) * 1000;
    const QPointer<RemoteIMApplication> self(this);
    state_.appendLiveMessage(message);
    persistMessage(message);
    emit stateChanged();
    // stateChanged observers can synchronously begin logout. Recheck immediately
    // before the SDK call as well as when its asynchronous receipt returns.
    if (!self || !connectionReady() || connectionGeneration_ != generation || client_->currentAccount() != account ||
        !isAcceptedFriend(target) || (canSend && !canSend())) {
        if (self && client_->currentAccount() == account && state_.ownerUserId() == account.ownerUserId)
            markMessage(message.id, RemoteIMMessageStatus::Failed);
        if (completion) completion(false, QStringLiteral("账号连接已改变"), {});
        return false;
    }
    dispatch(
        [self, account, generation, localId = message.id, completion = std::move(completion)]
        (bool ok, const QString& error, const RemoteIMSendReceipt& receipt) {
            if (!self) return;
            QTimer::singleShot(0, self, [self, account, generation, localId, ok, error, receipt, completion] {
                if (!self) return;
                if (!self->connectionReady() || self->connectionGeneration_ != generation ||
                    self->client_->currentAccount() != account || self->state_.ownerUserId() != account.ownerUserId) {
                    if (completion) completion(false, QStringLiteral("账号连接已改变"), {});
                    return;
                }
                const QString effective = self->adoptRemoteMessageId(localId, ok ? receipt.remoteMessageId : QString());
                if (ok) {
                    self->state_.updateMessageTime(effective, receipt.createdAtMillis);
                    if (self->database_) self->database_->updateMessageTime(effective, receipt.createdAtMillis);
                }
                self->markMessage(effective, ok ? RemoteIMMessageStatus::Sent : RemoteIMMessageStatus::Failed);
                if (completion) completion(ok, error, receipt);
            });
        });
    return true;
}

void RemoteIMApplication::sendDiagnosticTextTo(const QString& peerId, const QString& text,
                                               std::function<void(bool)> completion) {
    sendDiagnosticTo(peerId, text, {}, std::move(completion));
}

void RemoteIMApplication::sendDiagnosticReportTo(const QString& peerId, const QString& path,
                                                 std::function<void(bool)> completion) {
    sendDiagnosticTo(peerId, {}, path, std::move(completion));
}

void RemoteIMApplication::sendDiagnosticTo(const QString& peerId, const QString& text, const QString& path,
                                           std::function<void(bool)> completion) {
    const auto account = client_->currentAccount();
    bool friendExists = false;
    for (const auto& contact : state_.contacts()) if (contact.userId == peerId) friendExists = true;
    if (!isConnected() || !account.isValid() || account.ownerUserId != state_.ownerUserId() ||
        !friendExists || peerId == account.ownerUserId || (text.isEmpty() && path.isEmpty())) {
        if (completion) completion(false);
        return;
    }
    const QFileInfo info(path);
    if (!path.isEmpty() && (!info.isFile() || !info.isReadable())) {
        if (completion) completion(false);
        return;
    }
    const auto message = path.isEmpty() ? state_.queueOutgoingTextTo(peerId, text) :
        state_.queueOutgoingFileTo(peerId, path, info.fileName(), QStringLiteral("text/markdown"), info.size());
    persistMessage(message);
    emit stateChanged();
    const QPointer<RemoteIMApplication> self(this);
    auto done = [self, account, id = message.id, completion = std::move(completion)]
                (bool ok, const QString&, const RemoteIMSendReceipt& receipt) {
        if (!self) return;
        QTimer::singleShot(0, self, [self, account, id, ok, receipt, completion] {
            if (!self) return;
            if (self->client_->currentAccount() != account || self->state_.ownerUserId() != account.ownerUserId) {
                if (completion) completion(false);
                return;
            }
            const auto effectiveId = self->adoptRemoteMessageId(id, ok ? receipt.remoteMessageId : QString());
            if (ok) {
                self->state_.updateMessageTime(effectiveId, receipt.createdAtMillis);
                if (self->database_) self->database_->updateMessageTime(effectiveId, receipt.createdAtMillis);
            }
            self->markMessage(effectiveId, ok ? RemoteIMMessageStatus::Sent : RemoteIMMessageStatus::Failed);
            if (completion) completion(ok);
        });
    };
    if (path.isEmpty()) client_->sendText(peerId, text, std::move(done));
    else client_->sendFile(peerId, path, info.fileName(), std::move(done));
}

// 作用：对选中的联系人发送同一条文本并汇总成功与失败数。
int RemoteIMApplication::broadcastText(const QStringList& peerIds, const QString& text) {
    if (text.trimmed().isEmpty()) return 0;

    // 去重 + 去空。同一个人在"选中的联系人"和"选中的分组"里各出现一次是很常见的，
    // 不去重他就会收到两条一模一样的消息。
    QStringList recipients;
    QSet<QString> seen;
    for (const QString& peerId : peerIds) {
        const QString clean = peerId.trimmed();
        if (clean.isEmpty() || seen.contains(clean)) continue;
        seen.insert(clean);
        recipients.append(clean);
    }
    if (recipients.isEmpty()) return 0;

    // 先把所有消息一次性入库入内存再发，中途不刷界面：
    // 边发边刷会让会话列表在群发过程中不停跳动。
    QList<RemoteIMMessage> queued;
    for (const QString& peerId : recipients) {
        RemoteIMMessage message = state_.queueOutgoingTextTo(peerId, text);
        persistMessage(message);
        queued.append(message);
    }
    emit stateChanged();

    // 回执是逐条异步回来的，用共享计数器等齐再报结果。
    auto remaining = std::make_shared<int>(queued.size());
    auto failed = std::make_shared<QStringList>();
    const int total = queued.size();

    for (const RemoteIMMessage& message : queued) {
        client_->sendText(message.toUserId, message.text,
                          [this, messageId = message.id, peerId = message.toUserId, remaining, failed, total]
                          (bool ok, const QString& error, const RemoteIMSendReceipt& receipt) {
            Q_UNUSED(error);
            const QString effectiveId =
                adoptRemoteMessageId(messageId, ok ? receipt.remoteMessageId : QString());
            if (ok) {
                state_.updateMessageTime(effectiveId, receipt.createdAtMillis);
                if (database_) database_->updateMessageTime(effectiveId, receipt.createdAtMillis);
            } else {
                failed->append(peerId);
            }
            markMessage(effectiveId, ok ? RemoteIMMessageStatus::Sent : RemoteIMMessageStatus::Failed);

            // 群发失败不逐条弹 errorMessage：发给 20 个人失败 5 个就是 5 个弹窗。
            // 统一在最后报一次，把失败的人列全。
            if (--(*remaining) == 0) emit broadcastFinished(total, *failed);
        });
    }
    return total;
}

// 转发时生成新的本地消息与发送状态，不复用原消息标识或原会话的引用关系。
// 附件仍按源文件类型交给客户端发送；缺失的本地文件要返回失败而非发送空消息。
bool RemoteIMApplication::forwardMessage(const RemoteIMMessage& source,
                                         const QString& peerId) {
    const QString target = peerId.trimmed();
    if (target.isEmpty() || target == state_.ownerUserId()) return false;

    auto finish = [this](const QString& localId, const QString& failureText) {
        return [this, localId, failureText](bool ok, const QString& error,
                                            const RemoteIMSendReceipt& receipt) {
            const QString effectiveId =
                adoptRemoteMessageId(localId, ok ? receipt.remoteMessageId : QString());
            if (ok) {
                state_.updateMessageTime(effectiveId, receipt.createdAtMillis);
                if (database_) database_->updateMessageTime(effectiveId, receipt.createdAtMillis);
            }
            markMessage(effectiveId,
                        ok ? RemoteIMMessageStatus::Sent : RemoteIMMessageStatus::Failed);
            if (!ok) emit errorMessage(error.isEmpty() ? failureText : error);
        };
    };

    if (source.hasImage) {
        const QString path = source.image.localPath.trimmed();
        const QFileInfo info(path);
        if (path.isEmpty() || !info.exists() || !info.isFile()) {
            emit errorMessage(QStringLiteral("图片尚未下载完成或本地缓存已被清理"));
            return false;
        }
        const QString caption = forwardedCaption(source);
        RemoteIMMessage message = state_.queueOutgoingImageTo(
            target, path, source.image.width, source.image.height,
            source.image.sizeBytes > 0 ? source.image.sizeBytes : info.size(),
            caption, source.captionAbove);
        persistMessage(message);
        emit stateChanged();
        RemoteIMSendCompletion onDone = finish(message.id, QStringLiteral("图片转发失败"));
        if (caption.isEmpty()) {
            client_->sendImage(target, path, std::move(onDone));
        } else {
            client_->sendImageWithText(target, path, caption, source.captionAbove,
                                       std::move(onDone));
        }
        return true;
    }

    if (source.hasFile) {
        const QString path = source.file.localPath.trimmed();
        const QFileInfo info(path);
        if (path.isEmpty() || !info.exists() || !info.isFile()) {
            emit errorMessage(QStringLiteral("文件尚未下载完成或本地缓存已被清理"));
            return false;
        }
        const QString fileName = source.file.fileName.trimmed().isEmpty()
            ? info.fileName() : source.file.fileName.trimmed();
        const QString mimeType = source.file.mimeType.trimmed().isEmpty()
            ? QMimeDatabase().mimeTypeForFile(info).name() : source.file.mimeType;
        const QString caption = forwardedCaption(source);
        RemoteIMMessage message = state_.queueOutgoingFileTo(
            target, path, fileName, mimeType,
            source.file.sizeBytes > 0 ? source.file.sizeBytes : info.size(),
            caption, source.captionAbove);
        persistMessage(message);
        emit stateChanged();
        RemoteIMSendCompletion onDone = finish(message.id, QStringLiteral("文件转发失败"));
        if (caption.isEmpty()) {
            client_->sendFile(target, path, fileName, std::move(onDone));
        } else {
            client_->sendFileWithText(target, path, fileName, caption,
                                      source.captionAbove, std::move(onDone));
        }
        return true;
    }

    if (source.hasVideo) {
        return sendVideoTo(target, source.video.localPath,
                           forwardedCaption(source), source.captionAbove);
    }

    if (source.hasVoice) {
        emit errorMessage(QStringLiteral("桌面端暂不支持转发语音消息"));
        return false;
    }

    // 历史中可能存在「附件下载失败，只剩内部占位文本」的退化记录。
    // 这种记录不能伪装成普通文本转发，否则会把内部协议和文件名发给对方。
    const QString text = forwardedCaption(source);
    if (text.isEmpty()) return false;
    RemoteIMMessage message = state_.queueOutgoingTextTo(target, text);
    persistMessage(message);
    emit stateChanged();
    client_->sendText(target, text, finish(message.id, QStringLiteral("消息转发失败")));
    return true;
}

void RemoteIMApplication::sendApprovalDecision(const QString& token,
                                               RemoteIMApprovalAction action,
                                               std::function<void(bool)> completion) {
    const QString cleanToken = token.trimmed();
    if (!isValidRemoteIMApprovalToken(cleanToken) || state_.selectedPeerId().isEmpty()) {
        if (completion) completion(false);
        return;
    }
    RemoteIMMessage message = state_.queueOutgoingApprovalDecision(cleanToken, action);
    persistMessage(message);
    emit stateChanged();

    client_->sendApprovalDecision(
        message.toUserId,
        cleanToken,
        action,
        [this, messageId = message.id, completion = std::move(completion)](
            bool ok, const QString& error, const RemoteIMSendReceipt& receipt) {
            const QString effectiveId = adoptRemoteMessageId(
                messageId, ok ? receipt.remoteMessageId : QString());
            if (ok) {
                state_.updateMessageTime(effectiveId, receipt.createdAtMillis);
                if (database_) database_->updateMessageTime(effectiveId, receipt.createdAtMillis);
            }
            markMessage(
                effectiveId,
                ok ? RemoteIMMessageStatus::Sent : RemoteIMMessageStatus::Failed);
            if (!ok) {
                emit errorMessage(
                    error.isEmpty() ? QStringLiteral("审批决定发送失败") : error);
            }
            if (completion) completion(ok);
        });
}

void RemoteIMApplication::sendImage(const QString& localPath, const QString& text, bool captionAbove) {
    sendImageTo(state_.selectedPeerId(), localPath, text, captionAbove);
}

bool RemoteIMApplication::sendImageTo(const QString& peerId, const QString& localPath,
                                      const QString& text, bool captionAbove) {
    const QString target = peerId.trimmed();
    const QString cleanPath = localPath.trimmed();
    if (cleanPath.isEmpty() || target.isEmpty()) return false;

    QFileInfo info(cleanPath);
    if (!info.exists() || !info.isFile()) return false;
    const QString caption = text.trimmed();
    RemoteIMMessage message = state_.queueOutgoingImageTo(
        target, cleanPath, 0, 0, info.size(), caption, captionAbove);
    persistMessage(message);
    emit stateChanged();

    RemoteIMSendCompletion onDone =
        [this, messageId = message.id](bool ok, const QString& error, const RemoteIMSendReceipt& receipt) {
        const QString effectiveId = adoptRemoteMessageId(messageId, ok ? receipt.remoteMessageId : QString());
        if (ok) {
            state_.updateMessageTime(effectiveId, receipt.createdAtMillis);
            if (database_) database_->updateMessageTime(effectiveId, receipt.createdAtMillis);
        }
        markMessage(effectiveId, ok ? RemoteIMMessageStatus::Sent : RemoteIMMessageStatus::Failed);
        if (!ok) emit errorMessage(error.isEmpty() ? QStringLiteral("图片消息发送失败") : error);
    };
    if (caption.isEmpty()) {
        client_->sendImage(message.toUserId, cleanPath, std::move(onDone));
    } else {
        client_->sendImageWithText(message.toUserId, cleanPath, caption, captionAbove, std::move(onDone));
    }
    return true;
}

void RemoteIMApplication::sendFile(const QString& localPath, const QString& text, bool captionAbove) {
    sendFileTo(state_.selectedPeerId(), localPath, text, captionAbove);
}

bool RemoteIMApplication::sendFileTo(const QString& peerId, const QString& localPath,
                                     const QString& text, bool captionAbove) {
    const QString target = peerId.trimmed();
    const QString cleanPath = localPath.trimmed();
    if (cleanPath.isEmpty() || target.isEmpty()) return false;

    QFileInfo info(cleanPath);
    if (!info.exists() || !info.isFile()) {
        emit errorMessage(QStringLiteral("文件不存在或不可读：%1").arg(cleanPath));
        return false;
    }
    const QString fileName = info.fileName();
    const QString mimeType = QMimeDatabase().mimeTypeForFile(info).name();
    const QString caption = text.trimmed();
    RemoteIMMessage message = state_.queueOutgoingFileTo(
        target, cleanPath, fileName, mimeType, info.size(), caption, captionAbove);
    persistMessage(message);
    emit stateChanged();

    RemoteIMSendCompletion onDone =
        [this, messageId = message.id](bool ok, const QString& error, const RemoteIMSendReceipt& receipt) {
        const QString effectiveId = adoptRemoteMessageId(messageId, ok ? receipt.remoteMessageId : QString());
        if (ok) {
            state_.updateMessageTime(effectiveId, receipt.createdAtMillis);
            if (database_) database_->updateMessageTime(effectiveId, receipt.createdAtMillis);
        }
        markMessage(effectiveId, ok ? RemoteIMMessageStatus::Sent : RemoteIMMessageStatus::Failed);
        if (!ok) emit errorMessage(error.isEmpty() ? QStringLiteral("文件消息发送失败") : error);
    };
    if (caption.isEmpty()) {
        client_->sendFile(message.toUserId, cleanPath, fileName, std::move(onDone));
    } else {
        client_->sendFileWithText(message.toUserId, cleanPath, fileName, caption, captionAbove, std::move(onDone));
    }
    return true;
}

void RemoteIMApplication::sendVideo(const QString& localPath, const QString& text, bool captionAbove) {
    sendVideoTo(state_.selectedPeerId(), localPath, text, captionAbove);
}

// 校验视频文件与收件人后发送附件，并把本地发送状态关联到原会话。
// 文件不可读时直接反馈错误，不留下误导性的已发送消息。
bool RemoteIMApplication::sendVideoTo(const QString& peerId, const QString& localPath,
                                      const QString& text, bool captionAbove) {
    const QString target = peerId.trimmed();
    const QString cleanPath = localPath.trimmed();
    if (cleanPath.isEmpty() || target.isEmpty()) return false;

    const QFileInfo info(cleanPath);
    if (!info.exists() || !info.isFile()) {
        emit errorMessage(QStringLiteral("视频不存在或不可读：%1").arg(cleanPath));
        return false;
    }
    if (!isSupportedVideoFile(cleanPath)) {
        emit errorMessage(QStringLiteral("只支持发送 mp4 / mov 视频"));
        return false;
    }

    VideoFileMetadata metadata = readVideoFileMetadata(cleanPath);
    // 时长是 SDK 的必填项，而加密/畸形容器可能解不出来。给 1 秒兜底也比让整条
    // 消息发不出去强——对端最多是进度条显示得不准。
    if (metadata.durationSeconds <= 0) metadata.durationSeconds = 1;

    const QString coverDirectory =
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
            .filePath(QStringLiteral("RemoteIMVideoCovers"));
    const VideoCoverImage cover = createVideoCoverImage(cleanPath, metadata, coverDirectory);
    if (!cover.valid) {
        emit errorMessage(QStringLiteral("视频封面生成失败，无法发送视频"));
        return false;
    }

    RemoteIMVideoPayload payload;
    payload.videoPath = cleanPath;
    payload.videoType = metadata.containerType;
    payload.videoSizeBytes = info.size();
    payload.durationSeconds = metadata.durationSeconds;
    payload.coverPath = cover.path;
    payload.coverType = cover.type;
    payload.coverSizeBytes = cover.sizeBytes;
    payload.coverWidth = cover.width;
    payload.coverHeight = cover.height;

    const QString fileName = info.fileName();
    const QString caption = text.trimmed();
    // 本地回显用真正的视频附件（messages 表已有视频列）：气泡直接显示封面 + 时长，
    // 点击可播放，不再借道文件卡。
    RemoteIMMessage message = state_.queueOutgoingVideoTo(
        target, cleanPath, fileName, cover.path, metadata.durationSeconds, info.size(),
        caption, captionAbove);
    persistMessage(message);
    emit stateChanged();

    RemoteIMSendCompletion onDone =
        [this, messageId = message.id](bool ok, const QString& error, const RemoteIMSendReceipt& receipt) {
        const QString effectiveId = adoptRemoteMessageId(messageId, ok ? receipt.remoteMessageId : QString());
        if (ok) {
            state_.updateMessageTime(effectiveId, receipt.createdAtMillis);
            if (database_) database_->updateMessageTime(effectiveId, receipt.createdAtMillis);
        }
        markMessage(effectiveId, ok ? RemoteIMMessageStatus::Sent : RemoteIMMessageStatus::Failed);
        if (!ok) emit errorMessage(error.isEmpty() ? QStringLiteral("视频消息发送失败") : error);
    };
    if (caption.isEmpty()) {
        client_->sendVideo(message.toUserId, payload, std::move(onDone));
    } else {
        client_->sendVideoWithText(message.toUserId, payload, caption, captionAbove, std::move(onDone));
    }
    return true;
}

void RemoteIMApplication::sendVoicePlaceholder() {
    emit errorMessage(QStringLiteral("语音消息需要接入桌面端原生录音与 IM SDK 后启用"));
}

void RemoteIMApplication::markMessage(const QString& messageId, RemoteIMMessageStatus status) {
    state_.updateMessageStatus(messageId, status);
    if (database_) database_->updateMessageStatus(messageId, status);
    emit stateChanged();
}

QString RemoteIMApplication::adoptRemoteMessageId(const QString& localId, const QString& remoteMessageId) {
    if (remoteMessageId.isEmpty() || remoteMessageId == localId) return localId;
    state_.adoptMessageId(localId, remoteMessageId);
    if (database_) database_->adoptMessageId(localId, remoteMessageId);
    return remoteMessageId;
}

void RemoteIMApplication::persistMessage(const RemoteIMMessage& message) {
    if (!database_) return;
    database_->upsertContact(RemoteIMContact{peerOf(message), peerOf(message)});
    database_->insertMessageIfAbsent(message, peerOf(message));
}

// 作用：连接 IM 客户端信号与应用状态更新入口。
void RemoteIMApplication::bindClientSignals() {
    connect(client_.get(), &RemoteIMClient::selfProfileReceived, this,
            [this](const QString& userId, const QString& name, const QString& avatarUrl) {
        if (userId != state_.ownerUserId()) return;
        selfProfile_ = RemoteIMContact{userId, name, avatarUrl};
        emit selfProfileChanged();
    });
    connect(client_.get(), &RemoteIMClient::contactsReceived, this, [this](const QList<RemoteIMContact>& contacts) {
        const bool shouldSelectFirstContact = state_.selectedPeerId().isEmpty();
        for (const RemoteIMContact& contact : contacts) {
            RemoteIMContact current = contact;
            const QString accountName = friendDisplayByImId_.value(contact.userId);
            if (!accountName.isEmpty()) current.displayName = accountName;
            state_.upsertContact(current);
            if (!accountName.isEmpty())
                state_.setContactDisplayName(current.userId, accountName);
            if (database_) database_->upsertContact(current);
        }
        if (shouldSelectFirstContact && !contacts.isEmpty()) {
            loadRecentPeerHistory(contacts.first().userId);
            state_.selectPeer(contacts.first().userId);
        }
        emit stateChanged();
    });
    // 漫游/历史与实时推送分两条同构通道（都带稳定 SDK 消息 id、按 id 去重落库），
    // 区别仅在实时通道的新入站消息会累计会话未读红点。
    connect(client_.get(), &RemoteIMClient::messagesReceived, this, [this](const QList<RemoteIMMessage>& messages) {
        ingestMessages(messages, /*live=*/false);
    });
    connect(client_.get(), &RemoteIMClient::groupsReceived, this,
            [this](const QList<RemoteIMGroup>& groups) {
        for (const RemoteIMGroup& group : groups) mergeGroup(group);
        emit groupsChanged();
    });
    connect(client_.get(), &RemoteIMClient::groupUnreadReceived, this,
            [this](const QString& groupId, int count) {
        if (selectedGroupId_ == groupId) return;
        groupUnreadFromConversation_.insert(groupId);
        groupUnreadCounts_[groupId] = qMax(0, count);
        emit groupsChanged();
    });
    connect(client_.get(), &RemoteIMClient::groupMessagesReceived, this,
            [this](const QList<RemoteIMGroupMessage>& messages, bool live) {
        ingestGroupMessages(messages, live);
    });
    connect(client_.get(), &RemoteIMClient::groupMembershipChanged, this,
            [this] { refreshGroups(); });
    connect(client_.get(), &RemoteIMClient::liveMessagesReceived, this, [this](const QList<RemoteIMMessage>& messages) {
        // Immediate transports announce and become ready in this same call.
        // The SDK announces earlier, before media downloads, on its own channel.
        if (!client_->hasExplicitLiveReadiness()) {
            const auto generation = connectionGeneration_;
            const QPointer<RemoteIMApplication> self(this);
            for (const auto& message : messages) {
                if (RemoteDesktopSignals::isSignalText(message.text)) continue;
                const quint64 ingressId = ++immediateLiveIngressId_;
                announceIMMetadata(ingressId, message);
                if (!self || generation != connectionGeneration_) return;
                readyIMMetadata(ingressId, message);
                if (!self || generation != connectionGeneration_) return;
            }
        }
        ingestMessages(messages, /*live=*/true);
    });
    connect(client_.get(), &RemoteIMClient::liveMessageAnnounced, this, &RemoteIMApplication::announceIMMetadata);
    connect(client_.get(), &RemoteIMClient::liveMessageReady, this, &RemoteIMApplication::readyIMMetadata);
    connect(client_.get(), &RemoteIMClient::liveMessageFailed, this, &RemoteIMApplication::failIMMetadata);
    connect(client_.get(), &RemoteIMClient::liveMetadataBatchFinished, this, [this](quint64 sequence, bool parsed, quint64 generation) {
        RemoteIMClient::LiveMetadataBoundary boundary;
        client_->withLiveMetadataBoundary([&](auto value) { boundary = value; });
        if (!connectionReady() || (generation && generation != boundary.generation)) return;
        if (!parsed) metadataPersistenceError_ = QStringLiteral("Raw IM metadata could not be parsed");
        // Nested Qt event loops can finish a later parsed batch first. A fence
        // advances only over a contiguous prefix, never over a missing raw row.
        if (sequence > metadataCompletedSequence_) completedMetadataBatches_.insert(sequence);
        while (completedMetadataBatches_.remove(metadataCompletedSequence_ + 1)) ++metadataCompletedSequence_;
        drainIMHistoryFences();
    });
    connect(client_.get(), &RemoteIMClient::activityReceived, this,
            [this](const QString& fromUserId, const RemoteIMActivitySignal& signal) {
        applyActivity(fromUserId, signal);
    });
    connect(client_.get(), &RemoteIMClient::incomingText, this, [this](const QString& fromUserId, const QString& text) {
        // 远程桌面信令在入库前就分流出去：既不污染聊天记录，也不让房间号留在历史里。
        if (RemoteDesktopSignals::isSignalText(text)) {
            emit remoteDesktopSignalReceived(fromUserId, text);
            return;
        }
        const RemoteIMMessage received = state_.receiveText(fromUserId, text);
        persistMessage(received);
        emit stateChanged();
        emit incomingMessageArrived(fromUserId.trimmed(), received);
    });
    connect(client_.get(), &RemoteIMClient::incomingImage, this, [this](const QString& fromUserId,
                                                                        const QString& localPath,
                                                                        int width,
                                                                        int height,
                                                                        qint64 sizeBytes) {
        const RemoteIMMessage received = state_.receiveImage(fromUserId, localPath, width, height, sizeBytes);
        persistMessage(received);
        emit stateChanged();
        emit incomingMessageArrived(fromUserId.trimmed(), received);
    });
    connect(client_.get(), &RemoteIMClient::incomingVoice, this, [this](const QString& fromUserId,
                                                                        const QString& localPath,
                                                                        int durationSeconds) {
        const RemoteIMMessage received = state_.receiveVoice(fromUserId, localPath, durationSeconds);
        persistMessage(received);
        emit stateChanged();
        emit incomingMessageArrived(fromUserId.trimmed(), received);
    });
    connect(client_.get(), &RemoteIMClient::incomingFile, this, [this](const QString& fromUserId,
                                                                       const QString& localPath,
                                                                       const QString& fileName,
                                                                       const QString& mimeType,
                                                                       qint64 sizeBytes) {
        const RemoteIMMessage received = state_.receiveFile(fromUserId, localPath, fileName, mimeType, sizeBytes);
        persistMessage(received);
        emit stateChanged();
        emit incomingMessageArrived(fromUserId.trimmed(), received);
    });
    connect(client_.get(), &RemoteIMClient::disconnected, this, [this] {
        setHumanTypingActive(false);
        for (QTimer* timer : activityExpiryTimers_) timer->deleteLater();
        activityExpiryTimers_.clear();
        const QStringList peers = activityByPeer_.keys();
        activityByPeer_.clear();
        for (const QString& peer : peers) emit activityChanged(peer);
        connected_ = false;
        emit connectionChanged(false);
    });
}

void RemoteIMApplication::ingestMessages(const QList<RemoteIMMessage>& messages, bool live) {
    const bool shouldSelectFirstPeer = state_.selectedPeerId().isEmpty();
    QString firstPeerId;
    for (const RemoteIMMessage& message : messages) {
        // 实时推送通道同样可能带信令（TimSdk 走的是 liveMessagesReceived），
        // 这里一并拦掉；漫游历史里理应没有信令，拦了也无害。
        if (RemoteDesktopSignals::isSignalText(message.text)) {
            if (live) emit remoteDesktopSignalReceived(message.fromUserId, message.text);
            continue;
        }
        const QString peerId = peerOf(message);
        if (peerId.isEmpty()) continue;
        if (firstPeerId.isEmpty()) firstPeerId = peerId;
        state_.upsertContact(RemoteIMContact{peerId, peerId});
        // 首次入库才算「新消息」。漫游和实时两条路会投递同一条消息，
        // 没有库时退化成「实时的都算新」——那种模式下本来也没有历史可重复。
        bool firstTimeSeen = true;
        const bool metadataInserted = live && firstMetadataRows_.remove(message.id);
        if (database_) {
            database_->upsertContact(RemoteIMContact{peerId, peerId});
            firstTimeSeen = database_->insertMessageIfAbsent(message, peerId);
            firstTimeSeen = firstTimeSeen || metadataInserted;
        }
        if (live) {
            state_.appendLiveMessage(message);
            // 自己发出去的消息不通知；漫游/历史恢复也不通知（live=false 走不到这里）。
            if (firstTimeSeen && message.direction == RemoteIMMessageDirection::Incoming) {
                emit incomingMessageArrived(peerId, message);
            }
        } else {
            state_.appendMessageForRestore(message);
        }
    }
    if (shouldSelectFirstPeer && !firstPeerId.isEmpty()) {
        loadRecentPeerHistory(firstPeerId);
        state_.selectPeer(firstPeerId);
    }
    emit stateChanged();
}
