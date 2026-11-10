// 文件作用：桌面端跨设备 Session 的本机读取与操作入口；网络投递由外层负责。
#include "agent/DesktopSessionSyncHost.h"

#include <QDir>
#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <unordered_map>
#include <variant>

#include "MaiQuestionTranscript.h"
#include "agent/AgentController.h"
#include "agent/DesktopSessionAttachments.h"

namespace {

QString text(const std::string& value) {
    return QString::fromUtf8(value.data(), int(value.size()));
}

QJsonObject failure(const QString& message) {
    return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), message}};
}

QJsonObject questionFailure(const QString& message, const QString& code) {
    return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), message}, {QStringLiteral("errorCode"), code}};
}

QString curlFailureSummary(const std::string& error) {
    // curl 原始日志最多 16 KiB；JSON 控制字符转义最多放大六倍，另留固定信封空间。
    // 超限时整条拒绝，不能截断 JSON 后再猜错误码；原始日志始终不进入远端快照。
    constexpr std::size_t kMaximumCurlFailureJsonBytes = 16 * 1024 * 6 + 256;
    if (error.size() > kMaximumCurlFailureJsonBytes) return {};
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(error), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return {};
    const QJsonValue status = document.object().value(QStringLiteral("exit_status"));
    if (!status.isDouble()) return {};
    const double numeric = status.toDouble();
    if (!std::isfinite(numeric) || numeric < 1 || numeric > 255) return {};
    const int code = static_cast<int>(numeric);
    if (double(code) != numeric) return {};

    // 只按可信的整数退出码选择静态文案，不拼接 URL、请求头、响应或 stderr。
    QString reason;
    switch (code) {
        case 6: reason = QStringLiteral("无法解析主机名"); break;
        case 7: reason = QStringLiteral("无法连接服务器"); break;
        case 28: reason = QStringLiteral("请求超时"); break;
        case 60: reason = QStringLiteral("证书验证失败"); break;
        case 77: reason = QStringLiteral("无法加载 CA 证书"); break;
        default: return QStringLiteral("curl 执行失败（错误码 %1）").arg(code);
    }
    return QStringLiteral("curl 失败（错误码 %1）：%2").arg(code).arg(reason);
}

QJsonObject serializePart(const MaiMessagePart& part) {
    QJsonObject value{{QStringLiteral("id"), text(part.id)}, {QStringLiteral("created"), double(part.created)}};
    if (const auto* body = std::get_if<MaiTextPart>(&part.body)) {
        value.insert(QStringLiteral("kind"), QStringLiteral("text"));
        value.insert(QStringLiteral("text"), text(body->text));
    } else if (std::holds_alternative<MaiReasoningPart>(part.body)) {
        value.insert(QStringLiteral("kind"), QStringLiteral("reasoning"));
    } else if (const auto* body = std::get_if<MaiToolPart>(&part.body)) {
        value.insert(QStringLiteral("kind"), QStringLiteral("tool"));
        value.insert(QStringLiteral("tool"), text(body->tool));
        value.insert(QStringLiteral("state"), QString::fromLatin1(maiToolStateToString(body->state)));
        if (body->tool == "question") {
            // 只发送模型明确问用户的话与用户的答案；其它工具仍不外传参数和输出。
            value.insert(QStringLiteral("input"), text(body->input));
            value.insert(QStringLiteral("output"), text(body->output));
            value.insert(QStringLiteral("error"), text(body->error));
        } else if (body->tool == "curl" && body->state == MaiToolState::Error) {
            // 普通工具仍仅发布状态；curl 失败只补静态分类，手机无需读取所属设备日志。
            const QString summary = curlFailureSummary(body->error);
            if (!summary.isEmpty()) value.insert(QStringLiteral("error"), summary);
        }
    } else if (const auto* body = std::get_if<MaiImagePart>(&part.body)) {
        value.insert(QStringLiteral("kind"), QStringLiteral("image"));
        value.insert(QStringLiteral("mimeType"), text(body->mimeType));
        value.insert(QStringLiteral("filename"), QFileInfo(text(body->path)).fileName());
        value.insert(QStringLiteral("remotePartId"), text(part.id));
    } else if (const auto* body = std::get_if<MaiVideoPart>(&part.body)) {
        value.insert(QStringLiteral("kind"), QStringLiteral("video"));
        value.insert(QStringLiteral("mimeType"), text(body->mimeType));
        value.insert(QStringLiteral("filename"), QFileInfo(text(body->path)).fileName());
        value.insert(QStringLiteral("remotePartId"), text(part.id));
    } else if (const auto* body = std::get_if<MaiQuotePart>(&part.body)) {
        value.insert(QStringLiteral("kind"), QStringLiteral("quote"));
        value.insert(QStringLiteral("messageId"), text(body->messageId));
        value.insert(QStringLiteral("preview"), text(body->preview));
    }
    return value;
}

QJsonObject serializeMessage(const MaiMessage& message, bool busy, const std::string& sessionId) {
    QJsonArray parts;
    for (const auto& part : message.parts) parts.append(serializePart(part));
    return {{QStringLiteral("id"), text(message.id)},
            {QStringLiteral("sessionId"), text(sessionId)},
            {QStringLiteral("role"), QString::fromLatin1(maiRoleToString(message.role))},
            {QStringLiteral("created"), double(message.created)},
            {QStringLiteral("completed"), double(message.completed)},
            {QStringLiteral("active"), busy && message.isInProgress()},
            {QStringLiteral("parts"), parts}};
}

}  // namespace

struct DesktopSessionSyncHost::Runtime {
    const qint64 liveImagesSince = QDateTime::currentMSecsSinceEpoch();
    QHash<QString, QHash<QString, QSet<QString>>> pendingLiveImages;
    QPointer<AgentController> subscribedController;
    std::atomic<std::uint64_t> questionRevision{1};
    MaiEventBus::Token eventToken = 0;
    std::mutex cacheMutex;
    std::uint64_t cachedRevision = 0;
    std::unordered_map<std::string, std::vector<MaiQuestionTranscriptMessage>> childQuestionCache;
};

DesktopSessionSyncHost::DesktopSessionSyncHost(AgentController& controller)
    : controller_(controller), runtime_(std::make_shared<Runtime>()) {
    runtime_->subscribedController = &controller;
    const auto state = runtime_;
    runtime_->eventToken = controller_.agent().eventBus().subscribe([state](const MaiEvent& event) {
        // 正文每字 delta 不能反复读后代 SQLite 历史。其它持久化结构/提问/终态变化
        // 仅增加 revision，实际查询留在 historyPage 的宿主线程中完成。
        if (event.type != MaiEventType::MessagePartDelta && event.type != MaiEventType::SessionStatus)
            state->questionRevision.fetch_add(1, std::memory_order_relaxed);
    });
}

DesktopSessionSyncHost::~DesktopSessionSyncHost() {
    if (runtime_->subscribedController)
        runtime_->subscribedController->agent().eventBus().unsubscribe(runtime_->eventToken);
}

void DesktopSessionSyncHost::setDefaultWorkspaceDirectory(const QString& directory) {
    defaultWorkspaceDirectory_ = directory;
}

void DesktopSessionSyncHost::setImagePublicationScope(const QString& account, const QString& slot,
                                                     const QString& login) {
    imageAccount_ = account; imageSlot_ = slot; imageLogin_ = login;
    if (account.isEmpty() || slot.isEmpty() || login.isEmpty()) return;
    const auto pending = runtime_->pendingLiveImages;
    runtime_->pendingLiveImages.clear();
    for (auto session = pending.cbegin(); session != pending.cend(); ++session)
        for (auto message = session->cbegin(); message != session->cend(); ++message)
            for (const auto& part : *message) enrollLiveImages(session.key(), message.key(), part);
}

void DesktopSessionSyncHost::enrollLiveImages(const QString& sessionId, const QString& messageId,
                                            const QString& partId) {
    MaiSession session;
    MaiMessage message;
    if (!controller_.agent().getSession(sessionId.toStdString(), session) || !session.isRoot() ||
        !controller_.agent().getMessage(sessionId.toStdString(), messageId.toStdString(), message) ||
        (message.role != MaiRole::User && message.role != MaiRole::Assistant)) return;
    for (const auto& part : message.parts) {
        if ((!partId.isEmpty() && text(part.id) != partId) || part.created < runtime_->liveImagesSince ||
            !std::holds_alternative<MaiImagePart>(part.body)) continue;
        if (imageAccount_.isEmpty() || imageLogin_.isEmpty()) {
            // An image can arrive while the first presence ACK is in flight.
            // Retain only these observed live IDs, never scan history later.
            runtime_->pendingLiveImages[sessionId][messageId].insert(text(part.id));
            continue;
        }
        DesktopSessionAttachments::enrollTemporaryImage(imageAccount_,
            {imageSlot_, imageLogin_, sessionId}, messageId, text(part.id));
    }
}

QString DesktopSessionSyncHost::imageSource(const QString& sessionId, const QString& messageId,
                                           const QString& partId) const {
    MaiMessage message;
    if (!controller_.agent().getMessage(sessionId.toStdString(), messageId.toStdString(), message)) return {};
    for (const auto& part : message.parts)
        if (text(part.id) == partId)
            if (const auto* image = std::get_if<MaiImagePart>(&part.body)) return text(image->path);
    return {};
}

QJsonObject DesktopSessionSyncHost::decorateImages(QJsonObject message, const QString& sessionId) const {
    if (imageAccount_.isEmpty()) return message;
    auto parts = message.value(QStringLiteral("parts")).toArray();
    const auto messageId = message.value(QStringLiteral("id")).toString();
    for (int index = 0; index < parts.size(); ++index) {
        auto part = parts[index].toObject();
        if (part.value(QStringLiteral("kind")).toString() != QStringLiteral("image")) continue;
        const auto image = DesktopSessionAttachments::imagePublication(imageAccount_,
            {imageSlot_, imageLogin_, sessionId}, messageId, part.value(QStringLiteral("id")).toString());
        if (!image.isEmpty()) part.insert(QStringLiteral("remoteImage"), image);
        parts[index] = part;
    }
    message.insert(QStringLiteral("parts"), parts);
    return message;
}

QJsonArray DesktopSessionSyncHost::rootSessions() const {
    QJsonArray result;
    for (const MaiSession& session : controller_.agent().listSessions()) {
        if (!session.isRoot()) continue;
        result.append(QJsonObject{{QStringLiteral("id"), text(session.id)},
                                  {QStringLiteral("title"), text(session.title)},
                                  {QStringLiteral("updated_at"), double(session.updated)},
                                  {QStringLiteral("turnId"), text(controller_.agent().activeTurnId(session.id))},
                                  {QStringLiteral("busy"), controller_.agent().isBusy(session.id)}});
    }
    return result;
}

QJsonObject DesktopSessionSyncHost::historyMessage(const QString& sessionId, const QString& messageId) const {
    MaiSession session;
    MaiMessage message;
    if (!controller_.agent().getSession(sessionId.toStdString(), session) || !session.isRoot() ||
        !controller_.agent().getMessage(sessionId.toStdString(), messageId.toStdString(), message))
        return {};
    return decorateImages(serializeMessage(message, controller_.agent().isBusy(sessionId.toStdString()),
                                          sessionId.toStdString()), sessionId);
}

QJsonObject DesktopSessionSyncHost::historyPage(const QString& sessionId, const QString& beforeId, int limit) const {
    MaiSession session;
    if (limit < 1 || limit > 20 || !controller_.agent().getSession(sessionId.toStdString(), session) ||
        !session.isRoot())
        return failure(QStringLiteral("会话不存在或页大小无效"));
    QJsonArray messages;
    const auto page =
        controller_.agent().listMessagesPage(sessionId.toStdString(), beforeId.toStdString(), size_t(limit));
    for (const MaiMessage& message : page)
        messages.append(decorateImages(serializeMessage(message, controller_.agent().isBusy(session.id), session.id), sessionId));
    QJsonArray permissions;
    for (const MaiPermissionRequest& request : controller_.agent().listPendingPermissions()) {
        if (request.sessionId != session.id) continue;
        permissions.append(QJsonObject{{QStringLiteral("id"), text(request.id)},
                                       {QStringLiteral("tool"), text(request.toolName)},
                                       {QStringLiteral("input"), text(request.arguments)},
                                       {QStringLiteral("allowForSession"), request.allowForSession},
                                       {QStringLiteral("rememberOnApproval"), request.rememberOnApproval},
                                       {QStringLiteral("fileCount"), int(request.approvalKeys.size())}});
    }
    QJsonArray questions;
    // 待回答状态每次权威读取，尤其不能在答案已受理但 worker 尚未广播时复用旧 ID。
    auto transcript = captureMaiQuestionTranscript(controller_.agent(), session.id, false);
    const auto revision = runtime_->questionRevision.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(runtime_->cacheMutex);
        if (runtime_->cachedRevision != revision) {
            runtime_->childQuestionCache.clear();
            runtime_->cachedRevision = revision;
        }
        const auto found = runtime_->childQuestionCache.find(session.id);
        if (found != runtime_->childQuestionCache.end()) {
            transcript.childMessages = found->second;
        } else {
            transcript.childMessages = captureMaiQuestionTranscript(controller_.agent(), session.id).childMessages;
            // 热窗口与桌面会话复用相同，避免远端交替查询让历史投影无界驻留。
            if (runtime_->childQuestionCache.size() >= 3) runtime_->childQuestionCache.clear();
            runtime_->childQuestionCache.emplace(session.id, transcript.childMessages);
        }
    }
    for (const MaiQuestionRequest& request : transcript.pending) {
        QJsonArray options;
        for (const std::string& option : request.options) options.append(text(option));
        questions.append(QJsonObject{{QStringLiteral("id"), text(request.id)},
                                     {QStringLiteral("sessionId"), text(request.sessionId)},
                                     {QStringLiteral("messageId"), text(request.messageId)},
                                     {QStringLiteral("partId"), text(request.partId)},
                                     {QStringLiteral("asked"), double(request.asked)},
                                     {QStringLiteral("question"), text(request.question)},
                                     {QStringLiteral("options"), options}});
    }
    // 单独传子任务问答投影，messages 的页大小、before 游标仍只属于根会话。
    QJsonArray questionMessages;
    for (const auto& projected : transcript.childMessages) {
        QJsonObject message = serializeMessage(projected.message, controller_.agent().isBusy(projected.ownerSessionId),
                                               projected.ownerSessionId);
        message.insert(QStringLiteral("questionProjection"), true);
        questionMessages.append(message);
    }
    QSet<QString> pendingMessages;
    for (const auto& question : transcript.pending) pendingMessages.insert(text(question.messageId));
    while (!questionMessages.isEmpty() &&
           QJsonDocument(questionMessages).toJson(QJsonDocument::Compact).size() > 48 * 1024) {
        int removeIndex = 0;
        for (int index = 0; index < questionMessages.size(); ++index)
            if (!pendingMessages.contains(questionMessages[index].toObject().value(QStringLiteral("id")).toString())) {
                removeIndex = index;
                break;
            }
        questionMessages.removeAt(removeIndex);
    }
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("busy"), controller_.agent().isBusy(session.id)},
            {QStringLiteral("turnId"), text(controller_.agent().activeTurnId(session.id))},
            {QStringLiteral("messages"), messages},
            {QStringLiteral("permissions"), permissions},
            {QStringLiteral("questionMessages"), questionMessages},
            {QStringLiteral("questions"), questions}};
}

QJsonObject DesktopSessionSyncHost::messageUpdate(const QString& sessionId, const QString& messageId) const {
    MaiSession session;
    MaiMessage message;
    if (messageId.isEmpty() || !controller_.agent().getSession(sessionId.toStdString(), session) || !session.isRoot() ||
        !controller_.agent().getMessage(session.id, messageId.toStdString(), message))
        return {};
    const bool busy = controller_.agent().isBusy(session.id);
    if (busy && message.isInProgress())
        for (auto& part : message.parts)
            if (auto* body = std::get_if<MaiTextPart>(&part.body)) {
                const auto live = controller_.livePartText(sessionId, text(part.id)).toUtf8().toStdString();
                if (live.size() > body->text.size()) body->text = live;
            }
    return decorateImages(serializeMessage(message, busy, session.id), sessionId);
}

QJsonObject DesktopSessionSyncHost::applyCommand(const QString& sessionId, const QJsonObject& payload) {
    const QString action = payload.value(QStringLiteral("action")).toString();
    if (sessionId == QStringLiteral("session_lifecycle")) {
        if (action == QStringLiteral("session_create")) {
            QString path = payload.value(QStringLiteral("workspace_path")).toString();
            if (path.isEmpty()) {
                path = defaultWorkspaceDirectory_;
                if (path.isEmpty() || QDir(path).isRoot() || !QDir().mkpath(path))
                    return failure(QStringLiteral("默认工作目录不可用"));
            } else if (path.size() > 4096 || !QFileInfo(path).isAbsolute()) {
                return failure(QStringLiteral("工作目录必须是已存在的绝对路径"));
            }
            const QFileInfo directory(path);
            const QString canonical = directory.canonicalFilePath();
            if (!directory.isDir() || canonical.isEmpty()) return failure(QStringLiteral("工作目录不存在或不可访问"));
            const QString id = controller_.createSession(canonical);
            if (id.isEmpty()) return failure(controller_.lastError());
            return {{QStringLiteral("ok"), true}, {QStringLiteral("session_id"), id}};
        }
        if (action == QStringLiteral("session_delete")) {
            const QString target = payload.value(QStringLiteral("target_session_id")).toString();
            MaiSession targetSession;
            if (target.isEmpty() || !controller_.agent().getSession(target.toStdString(), targetSession) ||
                !targetSession.isRoot())
                return failure(QStringLiteral("会话不存在"));
            const auto deleted = controller_.agent().submit(MaiDeleteSession{target.toStdString()});
            if (!deleted) return failure(text(deleted.error().message()));
            return {{QStringLiteral("ok"), true}, {QStringLiteral("session_id"), target}};
        }
        if (action == QStringLiteral("session_set_workspace")) {
            const QString target = payload.value(QStringLiteral("target_session_id")).toString();
            const QString path = payload.value(QStringLiteral("workspace_path")).toString();
            MaiSession targetSession;
            if (target.isEmpty() || !controller_.agent().getSession(target.toStdString(), targetSession) ||
                !targetSession.isRoot())
                return failure(QStringLiteral("会话不存在"));
            const QFileInfo directory(path);
            const QString canonical = directory.canonicalFilePath();
            if (path.isEmpty() || path.toUtf8().size() > 4096 || !directory.isAbsolute() || !directory.isDir() ||
                canonical.isEmpty())
                return failure(QStringLiteral("工作目录必须是已存在的绝对路径"));
            if (!controller_.setSessionDirectory(target, canonical)) return failure(controller_.lastError());
            return {{QStringLiteral("ok"), true},
                    {QStringLiteral("session_id"), target},
                    {QStringLiteral("workspace_path"), canonical}};
        }
        return failure(QStringLiteral("不支持的会话操作"));
    }
    MaiSession session;
    if (!controller_.agent().getSession(sessionId.toStdString(), session) || !session.isRoot())
        return failure(QStringLiteral("会话不存在"));
    if (action == QStringLiteral("prompt") || action == QStringLiteral("steer")) {
        const bool steering = action == QStringLiteral("steer");
        const QString turn = payload.value(QStringLiteral("turnId")).toString();
        QString request = payload.value(QStringLiteral("requestId")).toString();
        const QString quote = payload.value(QStringLiteral("quoteMessageId")).toString();
        static const QRegularExpression id(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
        if (steering && (!id.match(turn).hasMatch() || !id.match(request).hasMatch() || request.size() > 96 ||
                         (!quote.isEmpty() && !id.match(quote).hasMatch())))
            return failure(QStringLiteral("当前任务标识无效，请重新读取会话"));
        if (!steering && payload.contains(QStringLiteral("requestId")) &&
            (!payload.value(QStringLiteral("requestId")).isString() || !id.match(request).hasMatch() ||
             request.size() > 96))
            return failure(QStringLiteral("消息请求编号无效"));
        const QString message = payload.value(QStringLiteral("text")).toString();
        const QJsonArray attachments = payload.value(QStringLiteral("resolved_attachments")).toArray();
        if ((message.isEmpty() && attachments.isEmpty()) || message.toUtf8().size() > 16 * 1024 ||
            attachments.size() > 10)
            return failure(QStringLiteral("消息为空或过长"));
        QStringList images;
        QStringList videos;
        QStringList documents;
        std::vector<MaiModelImage> steeringImages;
        std::vector<MaiVideoPart> steeringVideos;
        const QString root = QFileInfo(QString::fromUtf8(session.directory.c_str())).canonicalFilePath();
        const QString attachmentRoot =
            QDir::fromNativeSeparators(QDir(root).filePath(QStringLiteral(".maichat/session-attachments"))) +
            QLatin1Char('/');
        for (const QJsonValue& value : attachments) {
            const QJsonObject item = value.toObject();
            const QString path = QDir::fromNativeSeparators(
                QFileInfo(item.value(QStringLiteral("path")).toString()).canonicalFilePath());
            if (root.isEmpty() || path.isEmpty() || !path.startsWith(attachmentRoot) || !QFileInfo(path).isFile())
                return failure(QStringLiteral("会话附件路径无效"));
            const QString kind = item.value(QStringLiteral("kind")).toString();
            const QString mime = item.value(QStringLiteral("content_type")).toString();
            if (kind == QStringLiteral("image") &&
                (mime == QStringLiteral("image/jpeg") || mime == QStringLiteral("image/png") ||
                 mime == QStringLiteral("image/webp") || mime == QStringLiteral("image/gif"))) {
                images.append(path);
                steeringImages.push_back({path.toStdString(), mime.toStdString()});
            } else if (kind == QStringLiteral("image"))
                documents.append(QDir(root).relativeFilePath(path));
            else if (kind == QStringLiteral("video")) {
                videos.append(path);
                steeringVideos.push_back({path.toStdString(), mime.toStdString()});
            } else if (kind == QStringLiteral("audio") || kind == QStringLiteral("file"))
                documents.append(QDir(root).relativeFilePath(path));
            else
                return failure(QStringLiteral("会话附件类型无效"));
        }
        QString prompt = message;
        if (!documents.isEmpty())
            prompt += QStringLiteral("\n\n会话附件已保存到本机工作目录：\n- ") + documents.join(QStringLiteral("\n- "));
        if (steering) {
            // The owner validates the original turn atomically. Never retry this
            // as a new prompt if that turn ended while attachments were copied.
            const auto accepted = controller_.agent().submit(
                MaiSteerPrompt{sessionId.toStdString(), turn.toStdString(), request.toStdString(), prompt.toStdString(),
                               std::move(steeringImages), std::move(steeringVideos), quote.toStdString()});
            if (!accepted) return failure(text(accepted.error().message()));
            return {{QStringLiteral("ok"), true},
                    {QStringLiteral("user_message_id"), text(accepted.value())},
                    {QStringLiteral("turnId"), turn}};
        }
        // 普通远端消息由所属端接收排队，不依赖观察端缓存的 turnId 或忙闲状态。
        // Coordinator 为旧调用补上信箱请求编号；直接普通调用未给编号时视为新消息。
        if (request.isEmpty()) request = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto accepted = controller_.agent().submit(
            MaiEnqueuePrompt{MaiSendPrompt{sessionId.toStdString(), prompt.toStdString(), std::move(steeringImages),
                                           std::move(steeringVideos), quote.toStdString()},
                             request.toStdString()});
        if (!accepted) return failure(text(accepted.error().message()));
        return {{QStringLiteral("ok"), true},
                {QStringLiteral("accepted"), true},
                {QStringLiteral("user_message_id"), text(accepted.value())}};
    } else if (action == QStringLiteral("stop")) {
        if (!controller_.interrupt(sessionId, payload.value(QStringLiteral("turnId")).toString()))
            return failure(controller_.lastError());
    } else if (action == QStringLiteral("permission")) {
        const QString id = payload.value(QStringLiteral("id")).toString();
        const QString decision = payload.value(QStringLiteral("decision")).toString();
        if (id.isEmpty() || id.size() > 128) return failure(QStringLiteral("权限请求无效"));
        if (decision != QStringLiteral("denied") && decision != QStringLiteral("approved") &&
            decision != QStringLiteral("approved_for_session"))
            return failure(QStringLiteral("权限决定无效"));
        const bool ok = decision == QStringLiteral("denied")                 ? controller_.denyPermission(id)
                        : decision == QStringLiteral("approved")             ? controller_.approvePermission(id)
                        : decision == QStringLiteral("approved_for_session") ? controller_.approvePermission(id, true)
                                                                             : false;
        if (!ok) return failure(controller_.lastError());
    } else if (action == QStringLiteral("answer")) {
        const QString id = payload.value(QStringLiteral("id")).toString();
        const QString answer = payload.value(QStringLiteral("text")).toString();
        if (id.isEmpty() || id.size() > 128 || answer.isEmpty() || answer.toUtf8().size() > 16 * 1024)
            return questionFailure(QStringLiteral("回答请求无效"), QStringLiteral("invalid_input"));
        for (const QString& key : {QStringLiteral("images"), QStringLiteral("videos"), QStringLiteral("local_files"),
                                   QStringLiteral("resolved_attachments"), QStringLiteral("attachments")}) {
            const QJsonValue media = payload.value(key);
            if (!media.isUndefined() && !media.isNull() && (!media.isArray() || !media.toArray().isEmpty()))
                return questionFailure(QStringLiteral("问题回答仅接收文字，附件请保留为草稿"),
                                       QStringLiteral("invalid_input"));
        }
        bool owned = false;
        for (const auto& question : controller_.pendingQuestions())
            if (text(question.id) == id &&
                maiQuestionBelongsToSession(controller_.agent(), session.id, question.sessionId)) {
                owned = true;
                break;
            }
        if (!owned)
            return questionFailure(QStringLiteral("此问题已不在当前会话中等待回答"), QStringLiteral("not_found"));
        // 保留核心的陈旧 ID 拒绝语义：校验后仍可能被其它入口回答，不能回退成 prompt。
        const auto answered = controller_.agent().submit(MaiReplyQuestion{id.toStdString(), answer.toStdString()});
        if (!answered)
            return questionFailure(text(answered.error().message()),
                                   QString::fromLatin1(maiErrorCodeToString(answered.error().code())));
        QJsonObject receipt{{QStringLiteral("ok"), true}, {QStringLiteral("question_id"), id}};
        if (payload.value(QStringLiteral("requestId")).isString())
            receipt.insert(QStringLiteral("requestId"), payload.value(QStringLiteral("requestId")));
        return receipt;
    } else if (action == QStringLiteral("rename")) {
        const QString title = payload.value(QStringLiteral("title")).toString();
        if (title.isEmpty() || title.size() > 120) return failure(QStringLiteral("会话标题无效"));
        const auto result =
            controller_.agent().submit(MaiUpdateSession{sessionId.toStdString(), title.toStdString(), "", ""});
        if (!result) return failure(text(result.error().message()));
    } else {
        return failure(QStringLiteral("不支持的远端操作"));
    }
    return {{QStringLiteral("ok"), true}};
}
