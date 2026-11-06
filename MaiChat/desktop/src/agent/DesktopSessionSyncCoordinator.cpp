// 文件作用：桌面端跨设备 Session 的心跳、长轮询与消息转发；模型仍由本机 Agent 调用。
#include "agent/DesktopSessionSyncCoordinator.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSettings>
#include <QSysInfo>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

#include "agent/AgentController.h"
#include "agent/DesktopSessionSyncHost.h"
#include "MaiQuestionTranscript.h"
#include "agent/DesktopSessionAttachments.h"

namespace {

QString newId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces).remove(QLatin1Char('-'));
}

QJsonObject roomState(const QJsonObject& source) {
    return {{QStringLiteral("permissions"), source.value(QStringLiteral("permissions")).toArray()},
            {QStringLiteral("questions"), source.value(QStringLiteral("questions")).toArray()},
            {QStringLiteral("questionMessages"), source.value(QStringLiteral("questionMessages")).toArray()},
            {QStringLiteral("busy"), source.value(QStringLiteral("busy"))},
            {QStringLiteral("turnId"), source.value(QStringLiteral("turnId"))}};
}

QByteArray roomStateDigest(const QJsonObject& source) {
    return QCryptographicHash::hash(QJsonDocument(roomState(source)).toJson(QJsonDocument::Compact),
                                    QCryptographicHash::Sha256);
}

QJsonObject boundedRoomPublish(const QString& sessionId, const QJsonArray& messages, const QJsonObject& state,
                               bool& trimmed) {
    QJsonObject body = state;
    body.insert(QStringLiteral("session_id"), sessionId);
    body.insert(QStringLiteral("messages"), messages);
    QJsonArray projection = body.value(QStringLiteral("questionMessages")).toArray();
    trimmed = false;
    // 为 WSS 的 id/op/body 包装留出 1 KiB；只能裁旧问答投影，不能改根正文或待回答问题。
    while (!projection.isEmpty() && QJsonDocument(body).toJson(QJsonDocument::Compact).size() > 95 * 1024) {
        projection.removeAt(0);
        body.insert(QStringLiteral("questionMessages"), projection);
        trimmed = true;
    }
    return body;
}

}  // namespace

DesktopSessionSyncCoordinator::DesktopSessionSyncCoordinator(Requester requester, AgentProvider agent, QObject* parent,
                                                             QString deviceId, QString deviceName)
    : QObject(parent), requester_(std::move(requester)), agentProvider_(std::move(agent)) {
    deviceId_ = std::move(deviceId);
    if (deviceId_.isEmpty()) {
        QSettings settings;
        deviceId_ = settings.value(QStringLiteral("session-sync/device-id")).toString();
        static const QRegularExpression validId(QStringLiteral("^[a-f0-9]{32}$"));
        if (!validId.match(deviceId_).hasMatch()) {
            deviceId_ = newId();
            settings.setValue(QStringLiteral("session-sync/device-id"), deviceId_);
        }
    }
    deviceName_ = deviceName.isEmpty() ? QSysInfo::machineHostName().left(40) : std::move(deviceName);
    if (deviceName_.isEmpty()) deviceName_ = QStringLiteral("Desktop");
    presenceTimer_.setInterval(15'000);
    connect(&presenceTimer_, &QTimer::timeout, this, [this] { publishPresence(); });
    snapshotTimer_.setSingleShot(true);
    snapshotTimer_.setInterval(300);
    connect(&snapshotTimer_, &QTimer::timeout, this, [this] {
        const QSet<QString> sessions = pendingSnapshotSessions_;
        pendingSnapshotSessions_.clear();
        for (const QString& session : sessions) publishSnapshot(session);
    });
    backfillTimer_.setSingleShot(true);
    backfillTimer_.setInterval(5000);
    connect(&backfillTimer_, &QTimer::timeout, this, [this] { processBackfillPage(); });
}

DesktopSessionSyncCoordinator::~DesktopSessionSyncCoordinator() = default;

void DesktopSessionSyncCoordinator::start() {
    if (started_ || !requester_ || !agentProvider_) return;
    started_ = true;
    presenceTimer_.start();
}

void DesktopSessionSyncCoordinator::connectionReady() {
    if (started_) publishPresence();
}

void DesktopSessionSyncCoordinator::connectionLost() {
    localLoginSession_.clear();
    pendingAttachmentCleanupRooms_.clear();
    initiallyPublishedSessions_.clear();
    publishedMessageDigests_.clear();
    publishedStateDigests_.clear();
    ++publishEpoch_;
    desiredPublishedStates_.clear();
    pendingMessagePublishes_.clear();
    messagePublishRevisions_.clear();
    roomStateRestoresPending_.clear();
    roomStateRestoresInFlight_.clear();
    backfillTimer_.stop();
    backfillSessions_.clear();
    backfillBefore_.clear();
    const auto pending = pendingLifecycle_;
    pendingLifecycle_.clear();
    for (const PendingLifecycle& command : pending)
        command.completion({}, QStringLiteral("设备连接中断，操作状态未知，请先查看会话列表"));
    QJsonArray offline;
    for (const QJsonValue& value : remoteSessions_) {
        QJsonObject entry = value.toObject();
        entry.insert(QStringLiteral("online"), false);
        offline.append(entry);
    }
    if (offline != remoteSessions_) {
        remoteSessions_ = offline;
        if (catalogHandler_) catalogHandler_(offline);
    }
    QJsonArray offlineDevices;
    for (const QJsonValue& value : remoteDevices_) {
        QJsonObject entry = value.toObject();
        entry.insert(QStringLiteral("online"), false);
        offlineDevices.append(entry);
    }
    if (offlineDevices != remoteDevices_) {
        remoteDevices_ = offlineDevices;
        if (deviceHandler_) deviceHandler_(remoteDevices_);
    }
}

void DesktopSessionSyncCoordinator::setCatalogHandler(CatalogHandler handler) {
    catalogHandler_ = std::move(handler);
    if (catalogHandler_) catalogHandler_(remoteSessions_);
}

void DesktopSessionSyncCoordinator::setDeviceHandler(DeviceHandler handler) {
    deviceHandler_ = std::move(handler);
    if (deviceHandler_) deviceHandler_(remoteDevices_);
}

void DesktopSessionSyncCoordinator::setDefaultWorkspaceDirectory(const QString& directory) {
    defaultWorkspaceDirectory_ = directory;
    if (host_) host_->setDefaultWorkspaceDirectory(directory);
}

void DesktopSessionSyncCoordinator::setPayloadHandler(PayloadHandler handler) {
    payloadHandler_ = std::move(handler);
}

void DesktopSessionSyncCoordinator::setAttachmentAccessProvider(AttachmentAccessProvider provider) {
    attachmentAccessProvider_ = std::move(provider);
}

DesktopSessionRoom DesktopSessionSyncCoordinator::localRoom(const QString& sessionId) const {
#if defined(Q_OS_MAC)
    return {QStringLiteral("mac"), localLoginSession_, sessionId};
#else
    return {QStringLiteral("windows"), localLoginSession_, sessionId};
#endif
}

void DesktopSessionSyncCoordinator::acceptCatalog(const QJsonArray& sessions) {
    QJsonArray remote;
    for (const QJsonValue& value : sessions) {
        const QJsonObject entry = value.toObject();
#if defined(Q_OS_MAC)
        constexpr auto ownSlot = "mac";
#else
        constexpr auto ownSlot = "windows";
#endif
        if (entry.value(QStringLiteral("device_slot")).toString() == QLatin1String(ownSlot) &&
            entry.value(QStringLiteral("device_id")).toString() == deviceId_)
            continue;
        remote.append(entry);
    }
    if (remote == remoteSessions_) return;
    remoteSessions_ = remote;
    if (catalogHandler_) catalogHandler_(remote);
}

void DesktopSessionSyncCoordinator::acceptDevices(const QJsonArray& devices) {
    QJsonArray remote;
    for (const QJsonValue& value : devices) {
        const QJsonObject entry = value.toObject();
#if defined(Q_OS_MAC)
        constexpr auto ownSlot = "mac";
#else
        constexpr auto ownSlot = "windows";
#endif
        if (entry.value(QStringLiteral("device_slot")).toString() == QLatin1String(ownSlot) &&
            entry.value(QStringLiteral("device_id")).toString() == deviceId_)
            continue;
        remote.append(entry);
    }
    if (remote == remoteDevices_) return;
    remoteDevices_ = remote;
    if (deviceHandler_) deviceHandler_(remote);
}

void DesktopSessionSyncCoordinator::acceptInbox(const QJsonArray& messages) {
    for (const QJsonValue& message : messages) handleEnvelope(message.toObject());
}

QJsonObject DesktopSessionSyncCoordinator::matchingRemote(const QString& slot, const QString& login,
                                                          const QString& session) const {
    for (const QJsonValue& value : remoteSessions_) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("device_slot")).toString() == slot &&
            entry.value(QStringLiteral("login_session_id")).toString() == login &&
            entry.value(QStringLiteral("id")).toString() == session)
            return entry;
    }
    return {};
}

QJsonObject DesktopSessionSyncCoordinator::matchingDevice(const QJsonObject& device) const {
    const QString slot = device.value(QStringLiteral("device_slot")).toString();
    const QString login = device.value(QStringLiteral("login_session_id")).toString();
    const QString id = device.value(QStringLiteral("device_id")).toString();
    for (const QJsonValue& value : remoteDevices_) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("device_slot")).toString() == slot &&
            entry.value(QStringLiteral("login_session_id")).toString() == login &&
            entry.value(QStringLiteral("device_id")).toString() == id)
            return entry;
    }
    return {};
}

void DesktopSessionSyncCoordinator::requestRemoteHistory(const QJsonObject& session, const QString& before, int limit,
                                                         Completion completion) {
    if (limit < 1 || limit > 20) {
        completion({}, QStringLiteral("会话页大小无效"));
        return;
    }
    const QString slot = session.value(QStringLiteral("device_slot")).toString();
    const QString login = session.value(QStringLiteral("login_session_id")).toString();
    const QString id = session.value(QStringLiteral("id")).toString();
    if (matchingRemote(slot, login, id).isEmpty()) {
        completion({}, QStringLiteral("所属设备登录已变化，请刷新列表"));
        return;
    }
    QPointer<DesktopSessionSyncCoordinator> guard(this);
    requester_(QStringLiteral("session-sync/history"), "GET",
               {{QStringLiteral("owner_slot"), slot},
                {QStringLiteral("session_id"), id},
                {QStringLiteral("before"), before},
                {QStringLiteral("limit"), limit}},
               [guard, session, completion = std::move(completion)](const QJsonObject& page, const QString& error) {
                   if (guard && error.isEmpty() && guard->payloadHandler_)
                       guard->payloadHandler_(session, QStringLiteral("history_page"), page);
                   completion(page, error);
               });
}

void DesktopSessionSyncCoordinator::sendRemoteCommand(const QJsonObject& session, const QJsonObject& payload,
                                                      Completion completion) {
    if (!session.value(QStringLiteral("online")).toBool()) {
        completion({}, QStringLiteral("所属设备离线，不能执行操作"));
        return;
    }
    const QString slot = session.value(QStringLiteral("device_slot")).toString();
    const QString login = session.value(QStringLiteral("login_session_id")).toString();
    const QString id = session.value(QStringLiteral("id")).toString();
    if (matchingRemote(slot, login, id).isEmpty()) {
        completion({}, QStringLiteral("所属设备登录已变化，请刷新列表"));
        return;
    }
    const QByteArray fingerprintData = slot.toUtf8() + '\n' + login.toUtf8() + '\n' + id.toUtf8() + '\n' +
                                       QJsonDocument(payload).toJson(QJsonDocument::Compact);
    const QString fingerprint =
        QString::fromLatin1(QCryptographicHash::hash(fingerprintData, QCryptographicHash::Sha256).toHex());
    const QString requestId = payload.value(QStringLiteral("requestId")).toString();
    const QRegularExpression validRequest(QStringLiteral("^[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}$"));
    if (!requestId.isEmpty() && !validRequest.match(requestId).hasMatch()) {
        completion({}, QStringLiteral("Invalid command request identity"));
        return;
    }
    // A transport attempt may be retried after the owner's mailbox receipt was
    // lost. Keep the input identity stable, while using a new envelope to deliver it.
    const bool input =
        payload.value(QStringLiteral("action")).toString() == QStringLiteral("prompt") && !requestId.isEmpty();
    QString commandId = input ? newId() : (requestId.isEmpty() ? retryIdByFingerprint_.value(fingerprint) : requestId);
    if (commandId.isEmpty()) commandId = newId();
    const QString previousFingerprint = fingerprintByCommandId_.value(commandId);
    if (!previousFingerprint.isEmpty() && previousFingerprint != fingerprint) {
        completion({}, QStringLiteral("Command request identity was reused with different content"));
        return;
    }
    if (input) {
        if (inputRequestByCommandId_.size() >= 512) inputRequestByCommandId_.erase(inputRequestByCommandId_.begin());
        inputRequestByCommandId_.insert(commandId, requestId);
    } else {
        retryIdByFingerprint_.insert(fingerprint, commandId);
        fingerprintByCommandId_.insert(commandId, fingerprint);
    }
    requester_(QStringLiteral("session-sync/send"), "POST",
               {{QStringLiteral("to_slot"), slot},
                {QStringLiteral("to_login_session_id"), login},
                {QStringLiteral("client_message_id"), commandId},
                {QStringLiteral("session_id"), id},
                {QStringLiteral("kind"), QStringLiteral("command")},
                {QStringLiteral("payload"), payload}},
               std::move(completion));
}

void DesktopSessionSyncCoordinator::sendRemoteLifecycle(const QJsonObject& device, const QJsonObject& payload,
                                                        Completion completion) {
    const QJsonObject current = matchingDevice(device);
    if (current.isEmpty() || !current.value(QStringLiteral("online")).toBool()) {
        completion({}, QStringLiteral("所属设备离线或登录已变化"));
        return;
    }
    const QString action = payload.value(QStringLiteral("action")).toString();
    if (action != QStringLiteral("session_create") && action != QStringLiteral("session_delete") &&
        action != QStringLiteral("session_set_workspace")) {
        completion({}, QStringLiteral("不支持的会话操作"));
        return;
    }
    const QString id = newId();
    const QString slot = current.value(QStringLiteral("device_slot")).toString();
    const QString login = current.value(QStringLiteral("login_session_id")).toString();
    pendingLifecycle_.insert(id, {slot, login, std::move(completion)});
    QPointer<DesktopSessionSyncCoordinator> guard(this);
    requester_(QStringLiteral("session-sync/send"), "POST",
               {{QStringLiteral("to_slot"), slot},
                {QStringLiteral("to_login_session_id"), login},
                {QStringLiteral("client_message_id"), id},
                {QStringLiteral("session_id"), QStringLiteral("session_lifecycle")},
                {QStringLiteral("kind"), QStringLiteral("command")},
                {QStringLiteral("payload"), payload}},
               [guard, id](const QJsonObject&, const QString& error) {
                   if (!guard || error.isEmpty() || !guard->pendingLifecycle_.contains(id)) return;
                   const PendingLifecycle command = guard->pendingLifecycle_.take(id);
                   command.completion({}, error);
               });
    QTimer::singleShot(30000, this, [guard, id] {
        if (!guard || !guard->pendingLifecycle_.contains(id)) return;
        const PendingLifecycle command = guard->pendingLifecycle_.take(id);
        command.completion({}, QStringLiteral("操作状态未知，请先刷新所属设备会话列表"));
    });
}

DesktopSessionSyncHost* DesktopSessionSyncCoordinator::host() {
    AgentController* current = agentProvider_();
    if (current && current == agent_) return host_.get();
    if (!current && !host_) return nullptr;
    // An account/controller replacement must invalidate old file completions.
    ++inputCommandGeneration_;
    inputCommands_.clear();
    preparingInputSessions_.clear();
    pendingInputEnvelopeIds_.clear();
    if (agent_) disconnect(agent_, nullptr, this, nullptr);
    snapshotTimer_.stop();
    backfillTimer_.stop();
    pendingSnapshotSessions_.clear();
    pendingMessageUpdates_.clear();
    snapshotRootOwners_.clear();
    initiallyPublishedSessions_.clear();
    backfillSessions_.clear();
    backfillBefore_.clear();
    publishedMessageDigests_.clear();
    publishedStateDigests_.clear();
    ++publishEpoch_;
    desiredPublishedStates_.clear();
    pendingMessagePublishes_.clear();
    messagePublishRevisions_.clear();
    roomStateRestoresPending_.clear();
    roomStateRestoresInFlight_.clear();
    host_.reset();
    agent_ = current;
    if (!current) return nullptr;
    host_ = std::make_unique<DesktopSessionSyncHost>(*current);
    host_->setDefaultWorkspaceDirectory(defaultWorkspaceDirectory_);
    connect(current, &AgentController::textDelta, this,
            [this](const QString& session, const QString& message, const QString&, const QString&) {
                scheduleSnapshot(session, message);
            });
    connect(current, &AgentController::reasoningDelta, this,
            [this](const QString& session, const QString& message, const QString&, const QString&) {
                scheduleSnapshot(session, message);
            });
    connect(
        current, &AgentController::toolPartChanged, this,
        [this](const QString& session, const QString& message, const QString&) { scheduleSnapshot(session, message); });
    // 控制器已将 Core 事件排到 Qt 线程；最终完成也按原 ID 更新，不暴露内部事件桥。
    connect(current, &AgentController::storedMessageUpdated, this,
            [this](const QString& session, const QString& message) { scheduleSnapshot(session, message); });
    connect(current, &AgentController::turnFinished, this,
            [this](const QString& session) { scheduleSnapshot(session); });
    connect(current, &AgentController::turnFailed, this,
            [this](const QString& session, const QString&) { scheduleSnapshot(session); });
    connect(current, &AgentController::permissionAsked, this,
            [this](const QString&, const QString& session) { scheduleSnapshot(session); });
    connect(current, &AgentController::questionAsked, this,
            [this](const QString&, const QString& session, const QString&) { scheduleSnapshot(session); });
    connect(current, &AgentController::sessionStructureChanged, this, [this] {
        snapshotRootOwners_.clear();
        publishPresence();
    });
    connect(current, &AgentController::sessionTitleChanged, this,
            [this](const QString&, const QString&) { publishPresence(); });
    return host_.get();
}

void DesktopSessionSyncCoordinator::publishPresence() {
    DesktopSessionSyncHost* current = host();
    if (!current) return;
    const QJsonArray sessions = current->rootSessions();
    requester_(QStringLiteral("session-sync/presence"), "POST",
               {{QStringLiteral("device_id"), deviceId_},
                {QStringLiteral("name"), deviceName_},
                {QStringLiteral("sessions"), sessions}},
               [this, sessions](const QJsonObject& response, const QString& error) {
                   if (!error.isEmpty()) return;
                   localLoginSession_ = response.value(QStringLiteral("login_session_id")).toString();
                   QSet<QString> currentIds;
                   for (const QJsonValue& value : sessions) {
                       const QString id = value.toObject().value(QStringLiteral("id")).toString();
                       currentIds.insert(id);
                       if (initiallyPublishedSessions_.contains(id)) continue;
                       initiallyPublishedSessions_.insert(id);
                       scheduleSnapshot(id);
                       backfillSessions_.enqueue(id);
                   }
                   for (const QString& oldId : initiallyPublishedSessions_ - currentIds) {
                       if (pendingAttachmentCleanupRooms_.contains(oldId)) continue;
                       pendingAttachmentCleanupRooms_.insert(oldId);
                       QPointer<DesktopSessionSyncCoordinator> guard(this);
                       requester_(QStringLiteral("session-sync/room-delete"), "POST",
                                  {{QStringLiteral("session_id"), oldId}},
                                  [guard, oldId](const QJsonObject&, const QString& error) {
                                      if (!guard) return;
                                      if (!error.isEmpty()) {
                                          guard->pendingAttachmentCleanupRooms_.remove(oldId);
                                          return;
                                      }
                                      if (!guard->attachmentAccessProvider_ || guard->localLoginSession_.isEmpty()) {
                                          guard->pendingAttachmentCleanupRooms_.remove(oldId);
                                          guard->finishDeletedRoom(oldId);
                                          return;
                                      }
                                      const DesktopVideoCircleAccess access = guard->attachmentAccessProvider_();
#if defined(Q_OS_MAC)
                                      const QString slot = QStringLiteral("mac");
#else
                                      const QString slot = QStringLiteral("windows");
#endif
                                      const DesktopSessionRoom room{slot, guard->localLoginSession_, oldId};
                                      auto* watcher = new QFutureWatcher<DesktopSessionAttachmentResult>(guard);
                                      connect(watcher, &QFutureWatcher<DesktopSessionAttachmentResult>::finished, guard,
                                              [guard, watcher, oldId, access] {
                                                  const auto result = watcher->result();
                                                  watcher->deleteLater();
                                                  if (!guard) return;
                                                  guard->pendingAttachmentCleanupRooms_.remove(oldId);
                                                  if (result.ok() && guard->attachmentAccessProvider_ &&
                                                      guard->attachmentAccessProvider_().accountId == access.accountId)
                                                      guard->finishDeletedRoom(oldId);
                                              });
                                      watcher->setFuture(QtConcurrent::run([access, room] {
                                          return DesktopSessionAttachments::cleanupRoom(access, room);
                                      }));
                                  });
                   }
                   if (!backfillTimer_.isActive() && !backfillSessions_.isEmpty()) backfillTimer_.start(2000);
                   for (const auto& session : inputCommands_.keys()) dispatchQueuedInput(session);
               });
}

void DesktopSessionSyncCoordinator::finishDeletedRoom(const QString& oldId) {
    cancelQueuedInputs(oldId, QStringLiteral("所属会话已删除，准备中的内容未受理"));
    initiallyPublishedSessions_.remove(oldId);
    backfillSessions_.removeAll(oldId);
    backfillBefore_.remove(oldId);
    publishedStateDigests_.remove(oldId);
    pendingMessageUpdates_.remove(oldId);
    desiredPublishedStates_.remove(oldId);
    pendingMessagePublishes_.remove(oldId);
    messagePublishRevisions_.remove(oldId);
    roomStateRestoresPending_.remove(oldId);
    roomStateRestoresInFlight_.remove(oldId);
    for (auto it = publishedMessageDigests_.begin(); it != publishedMessageDigests_.end();) {
        if (it.key().startsWith(oldId + QLatin1Char(':')))
            it = publishedMessageDigests_.erase(it);
        else
            ++it;
    }
}

void DesktopSessionSyncCoordinator::handleEnvelope(const QJsonObject& received) {
    QJsonObject envelope = received;
    if (envelope.value(QStringLiteral("kind")).toString() == QStringLiteral("command")) {
        QJsonObject payload = envelope.value(QStringLiteral("payload")).toObject();
        if (payload.value(QStringLiteral("action")).toString() == QStringLiteral("prompt") &&
            (!payload.contains(QStringLiteral("requestId")) ||
             (payload.value(QStringLiteral("requestId")).isString() &&
              payload.value(QStringLiteral("requestId")).toString().isEmpty()))) {
            payload.insert(QStringLiteral("requestId"), envelope.value(QStringLiteral("client_message_id")));
            envelope.insert(QStringLiteral("payload"), payload);
        }
    }
    const int id = envelope.value(QStringLiteral("id")).toInt();
    const QString kind = envelope.value(QStringLiteral("kind")).toString();
    const QString session = envelope.value(QStringLiteral("session_id")).toString();
    const QString fromSlot = envelope.value(QStringLiteral("from_slot")).toString();
    const QString fromLogin = envelope.value(QStringLiteral("from_login_session_id")).toString();
    if (id <= 0 || session.isEmpty() || fromSlot.isEmpty() || fromLogin.isEmpty()) return;
    DesktopSessionSyncHost* current = host();
    if (!current && kind == QStringLiteral("command")) return;
    if (kind == QStringLiteral("command")) {
        const QJsonObject payload = envelope.value(QStringLiteral("payload")).toObject();
        const QString action = payload.value(QStringLiteral("action")).toString();
        if (action == QStringLiteral("prompt") || action == QStringLiteral("steer")) {
            if (pendingInputEnvelopeIds_.contains(id)) return;
            auto& queue = inputCommands_[session];
            if (queue.size() >= 256) {
                respondToCommand(envelope,
                                 {{QStringLiteral("ok"), false},
                                  {QStringLiteral("error"), QStringLiteral("所属设备输入队列已满，内容未受理")}});
                return;
            }
            envelope.insert(QStringLiteral("_input_owner_login"), localLoginSession_);
            queue.enqueue(envelope);
            pendingInputEnvelopeIds_.insert(id);
            dispatchQueuedInput(session);
            return;
        }
        const QJsonObject result = current->applyCommand(session, payload);
        if (action == QStringLiteral("stop") && result.value(QStringLiteral("ok")).toBool())
            cancelQueuedInputs(session, QStringLiteral("任务已停止，准备中的内容未受理，已保留在发送端"));
        respondToCommand(envelope, result);
        return;
    } else if (kind == QStringLiteral("event")) {
        if (session == QStringLiteral("session_lifecycle")) {
            const QJsonObject payload = envelope.value(QStringLiteral("payload")).toObject();
            if (payload.value(QStringLiteral("type")).toString() == QStringLiteral("command_result")) {
                const QString commandId = payload.value(QStringLiteral("command_id")).toString();
                auto it = pendingLifecycle_.find(commandId);
                if (it != pendingLifecycle_.end() && it->slot == fromSlot && it->login == fromLogin) {
                    const PendingLifecycle command = it.value();
                    pendingLifecycle_.erase(it);
                    const QJsonObject result = payload.value(QStringLiteral("result")).toObject();
                    command.completion(
                        result, result.value(QStringLiteral("ok")).toBool()
                                    ? QString()
                                    : result.value(QStringLiteral("error")).toString(QStringLiteral("会话操作失败")));
                }
            }
        } else {
            handleRemoteChunk(envelope);
        }
    }
    acknowledge(id);
}

void DesktopSessionSyncCoordinator::respondToCommand(const QJsonObject& envelope, QJsonObject result) {
    const QString session = envelope.value(QStringLiteral("session_id")).toString();
    if (!result.value(QStringLiteral("ok")).toBool()) {
        QString error = result.value(QStringLiteral("error")).toString().left(120);
        error.replace(QRegularExpression(QStringLiteral("[\\x00-\\x1f\\x7f-\\x9f]")), QStringLiteral(" "));
        result.insert(QStringLiteral("error"), error.trimmed().isEmpty() ? QStringLiteral("会话操作失败") : error);
    }
    const QJsonObject response{
        {QStringLiteral("type"), QStringLiteral("command_result")},
        {QStringLiteral("command_id"), envelope.value(QStringLiteral("client_message_id"))},
        {QStringLiteral("result"), result},
        {QStringLiteral("request_id"),
         envelope.value(QStringLiteral("payload")).toObject().value(QStringLiteral("requestId"))}};
    if (session == QStringLiteral("session_lifecycle")) {
        requester_(QStringLiteral("session-sync/send"), "POST",
                   {{QStringLiteral("to_slot"), envelope.value(QStringLiteral("from_slot"))},
                    {QStringLiteral("to_login_session_id"), envelope.value(QStringLiteral("from_login_session_id"))},
                    {QStringLiteral("client_message_id"), newId()},
                    {QStringLiteral("session_id"), session},
                    {QStringLiteral("kind"), QStringLiteral("event")},
                    {QStringLiteral("payload"), response}},
                   [](const QJsonObject&, const QString&) {});
        publishPresence();
    } else {
        queuePayload(envelope.value(QStringLiteral("from_slot")).toString(),
                     envelope.value(QStringLiteral("from_login_session_id")).toString(), session,
                     QStringLiteral("event"), response);
        scheduleSnapshot(session);
    }
    acknowledge(envelope.value(QStringLiteral("id")).toInt());
    if (isCurrentInput(envelope, inputCommandGeneration_)) {
        pendingInputEnvelopeIds_.remove(envelope.value(QStringLiteral("id")).toInt());
        auto& queue = inputCommands_[session];
        queue.dequeue();
        preparingInputSessions_.remove(session);
        if (queue.isEmpty()) inputCommands_.remove(session);
        const quint64 generation = inputCommandGeneration_;
        QTimer::singleShot(0, this, [this, session, generation] {
            if (generation == inputCommandGeneration_) dispatchQueuedInput(session);
        });
    }
}

bool DesktopSessionSyncCoordinator::isCurrentInput(const QJsonObject& envelope, quint64 generation) const {
    if (generation != inputCommandGeneration_) return false;
    const auto found = inputCommands_.constFind(envelope.value(QStringLiteral("session_id")).toString());
    return found != inputCommands_.cend() && !found->isEmpty() &&
           found->head().value(QStringLiteral("id")) == envelope.value(QStringLiteral("id"));
}

void DesktopSessionSyncCoordinator::dispatchQueuedInput(const QString& sessionId) {
    // host() may invalidate queues when the account's controller has changed.
    DesktopSessionSyncHost* current = host();
    const auto found = inputCommands_.constFind(sessionId);
    if (!current || found == inputCommands_.cend() || found->isEmpty() || preparingInputSessions_.contains(sessionId))
        return;
    const QJsonObject envelope = found->head();
    const QString ownerLogin = envelope.value(QStringLiteral("_input_owner_login")).toString();
    if (!ownerLogin.isEmpty() && localLoginSession_.isEmpty()) return;
    if (!ownerLogin.isEmpty() && ownerLogin != localLoginSession_) {
        respondToCommand(envelope,
                         {{QStringLiteral("ok"), false},
                          {QStringLiteral("error"), QStringLiteral("所属设备登录已变化，准备中的内容未受理")}});
        return;
    }
    preparingInputSessions_.insert(sessionId);
    const QJsonObject payload = envelope.value(QStringLiteral("payload")).toObject();
    if (!payload.value(QStringLiteral("attachments")).toArray().isEmpty())
        startAttachmentCommand(envelope);
    else
        respondToCommand(envelope, current->applyCommand(sessionId, payload));
}

void DesktopSessionSyncCoordinator::cancelQueuedInputs(const QString& sessionId, const QString& reason) {
    const auto queue = inputCommands_.take(sessionId);
    preparingInputSessions_.remove(sessionId);
    for (const auto& envelope : queue) pendingInputEnvelopeIds_.remove(envelope.value(QStringLiteral("id")).toInt());
    for (const auto& envelope : queue)
        respondToCommand(envelope, {{QStringLiteral("ok"), false}, {QStringLiteral("error"), reason}});
}

void DesktopSessionSyncCoordinator::startAttachmentCommand(const QJsonObject& envelope) {
    const QString sessionId = envelope.value(QStringLiteral("session_id")).toString();
    const QJsonArray references =
        envelope.value(QStringLiteral("payload")).toObject().value(QStringLiteral("attachments")).toArray();
    AgentController* controller = agentProvider_ ? agentProvider_() : nullptr;
    MaiSession session;
    if (!controller || !controller->agent().getSession(sessionId.toStdString(), session) || !session.isRoot() ||
        !attachmentAccessProvider_ || localLoginSession_.isEmpty()) {
        respondToCommand(envelope, {{QStringLiteral("ok"), false},
                                    {QStringLiteral("error"), QStringLiteral("所属设备或会话附件空间不可用")}});
        return;
    }
    const quint64 generation = inputCommandGeneration_;
    const DesktopVideoCircleAccess access = attachmentAccessProvider_();
    if (agentProvider_() != controller) {
        host();
        return;
    }
    if (!isCurrentInput(envelope, generation)) return;
    if (envelope.value(QStringLiteral("_input_owner_login")).toString() != localLoginSession_) {
        respondToCommand(envelope, {{QStringLiteral("ok"), false},
                                    {QStringLiteral("error"), QStringLiteral("所属设备登录已变化，附件未派发")}});
        return;
    }
#if defined(Q_OS_MAC)
    const QString slot = QStringLiteral("mac");
#else
    const QString slot = QStringLiteral("windows");
#endif
    const DesktopSessionRoom room{slot, localLoginSession_, sessionId};
    const QString workspace = QString::fromUtf8(session.directory.c_str());
    const QJsonObject command = envelope.value(QStringLiteral("payload")).toObject();
    // The same core request must retain its imported media paths on a new
    // transport delivery. Bind the namespace to this account/login/turn.
    const QString retryNamespace =
        !command.value(QStringLiteral("requestId")).toString().isEmpty()
            ? QStringList{access.accountId, room.login, sessionId,
                          command.value(QStringLiteral("action")).toString() == QStringLiteral("steer")
                              ? command.value(QStringLiteral("turnId")).toString()
                              : QStringLiteral("input"),
                          command.value(QStringLiteral("requestId")).toString()}
                  .join(QLatin1Char(':'))
            : QString();
    auto* watcher = new QFutureWatcher<QPair<QJsonArray, QString>>(this);
    QPointer<DesktopSessionSyncCoordinator> guard(this);
    connect(
        watcher, &QFutureWatcher<QPair<QJsonArray, QString>>::finished, this,
        [guard, watcher, envelope, access, sessionId, room, generation, owner = QPointer<AgentController>(controller)] {
            const auto resolved = watcher->result();
            watcher->deleteLater();
            if (!guard || !guard->isCurrentInput(envelope, generation)) return;
            if (!owner || guard->agentProvider_() != owner) {
                guard->host();
                return;
            }
            if (!guard->attachmentAccessProvider_ || guard->attachmentAccessProvider_().accountId != access.accountId) {
                // Do not use a replacement account's token to acknowledge old work.
                ++guard->inputCommandGeneration_;
                guard->inputCommands_.clear();
                guard->preparingInputSessions_.clear();
                guard->pendingInputEnvelopeIds_.clear();
                return;
            }
            if (!resolved.second.isEmpty() || guard->localLoginSession_ != room.login) {
                guard->respondToCommand(envelope,
                                        {{QStringLiteral("ok"), false},
                                         {QStringLiteral("error"),
                                          resolved.second.isEmpty() ? QStringLiteral("账号已切换") : resolved.second}});
                return;
            }
            DesktopSessionSyncHost* current = guard->host();
            if (!current) return;
            QJsonObject payload = envelope.value(QStringLiteral("payload")).toObject();
            payload.insert(QStringLiteral("resolved_attachments"), resolved.first);
            const QJsonObject result = current->applyCommand(sessionId, payload);
            if (result.value(QStringLiteral("ok")).toBool()) {
                const QString userId = result.value(QStringLiteral("user_message_id")).toString();
                const QJsonObject page = current->historyPage(sessionId, {}, 1);
                for (const QJsonValue& value : QJsonArray{current->historyMessage(sessionId, userId)}) {
                    QJsonObject message = value.toObject();
                    if (message.value(QStringLiteral("id")).toString() != userId) continue;
                    QJsonArray parts = message.value(QStringLiteral("parts")).toArray();
                    for (const QJsonValue& item : resolved.first) {
                        const QJsonObject attachment = item.toObject();
                        const QString name = QFileInfo(attachment.value(QStringLiteral("path")).toString()).fileName();
                        bool matched = false;
                        for (int index = 0; index < parts.size(); ++index) {
                            QJsonObject part = parts.at(index).toObject();
                            if (part.value(QStringLiteral("kind")) != attachment.value(QStringLiteral("kind")) ||
                                part.value(QStringLiteral("filename")).toString() != name)
                                continue;
                            part.insert(QStringLiteral("attachment_id"),
                                        attachment.value(QStringLiteral("attachment_id")));
                            part.insert(QStringLiteral("name"), attachment.value(QStringLiteral("name")));
                            parts.replace(index, part);
                            matched = true;
                            break;
                        }
                        if (!matched)
                            parts.append(QJsonObject{
                                {QStringLiteral("id"),
                                 QStringLiteral("session_attachment_") +
                                     attachment.value(QStringLiteral("attachment_id")).toString()},
                                {QStringLiteral("kind"), attachment.value(QStringLiteral("kind"))},
                                {QStringLiteral("mimeType"), attachment.value(QStringLiteral("content_type"))},
                                {QStringLiteral("attachment_id"), attachment.value(QStringLiteral("attachment_id"))},
                                {QStringLiteral("name"), attachment.value(QStringLiteral("name"))}});
                    }
                    message.insert(QStringLiteral("parts"), parts);
                    guard->publishAttachmentHistory(
                        {{QStringLiteral("session_id"), sessionId},
                         {QStringLiteral("messages"), QJsonArray{message}},
                         {QStringLiteral("busy"), page.value(QStringLiteral("busy"))},
                         {QStringLiteral("turnId"), page.value(QStringLiteral("turnId"))},
                         {QStringLiteral("permissions"), page.value(QStringLiteral("permissions"))},
                         {QStringLiteral("questions"), page.value(QStringLiteral("questions"))}},
                        5);
                    break;
                }
            }
            guard->respondToCommand(envelope, result);
        });
    watcher->setFuture(QtConcurrent::run([access, room, workspace, references, retryNamespace] {
        QJsonArray resolved;
        const QString directory = QDir(workspace).filePath(QStringLiteral(".maichat/session-attachments"));
        if (!QDir().mkpath(directory))
            return QPair<QJsonArray, QString>{{}, QStringLiteral("工作目录无法保存会话附件")};
        for (const QJsonValue& value : references) {
            const QString id = value.toObject().value(QStringLiteral("attachment_id")).toString();
            const auto downloaded = DesktopSessionAttachments::download(access, room, id);
            if (!downloaded.ok()) return QPair<QJsonArray, QString>{{}, downloaded.error};
            const QString name = downloaded.metadata.value(QStringLiteral("name")).toString();
            const QString prefix =
                retryNamespace.isEmpty()
                    ? QUuid::createUuid().toString(QUuid::WithoutBraces).left(8)
                    : QString::fromLatin1(QCryptographicHash::hash((retryNamespace + QLatin1Char(':') + id).toUtf8(),
                                                                   QCryptographicHash::Sha256)
                                              .toHex()
                                              .left(24));
            const QString target = QDir(directory).filePath(prefix + QLatin1Char('-') + name);
            bool copied = false;
            if (!retryNamespace.isEmpty() && QFileInfo::exists(target)) {
                QFile original(target);
                QFile retry(downloaded.path);
                QCryptographicHash originalHash(QCryptographicHash::Sha256);
                QCryptographicHash retryHash(QCryptographicHash::Sha256);
                copied = !QFileInfo(target).isSymLink() && original.open(QIODevice::ReadOnly) &&
                         retry.open(QIODevice::ReadOnly) && originalHash.addData(&original) &&
                         retryHash.addData(&retry) && originalHash.result() == retryHash.result();
            } else {
                copied = QFile::copy(downloaded.path, target);
            }
            QFile::remove(downloaded.path);
            if (!copied) return QPair<QJsonArray, QString>{{}, QStringLiteral("无法将会话附件保存到工作目录")};
            resolved.append(QJsonObject{
                {QStringLiteral("attachment_id"), id},
                {QStringLiteral("name"), name},
                {QStringLiteral("path"), target},
                {QStringLiteral("kind"), downloaded.metadata.value(QStringLiteral("media_type"))},
                {QStringLiteral("content_type"), downloaded.metadata.value(QStringLiteral("content_type"))}});
        }
        return QPair<QJsonArray, QString>{resolved, {}};
    }));
}

void DesktopSessionSyncCoordinator::publishAttachmentHistory(const QJsonObject& body, int attempts) {
    QPointer<DesktopSessionSyncCoordinator> guard(this);
    requester_(QStringLiteral("session-sync/publish"), "POST", body,
               [guard, body, attempts](const QJsonObject&, const QString& error) {
                   if (!guard || error.isEmpty() || attempts <= 1) return;
                   QTimer::singleShot(2000, guard, [guard, body, attempts] {
                       if (guard) guard->publishAttachmentHistory(body, attempts - 1);
                   });
               });
}

void DesktopSessionSyncCoordinator::handleRemoteChunk(const QJsonObject& envelope) {
    const QString slot = envelope.value(QStringLiteral("from_slot")).toString();
    const QString login = envelope.value(QStringLiteral("from_login_session_id")).toString();
    const QString session = envelope.value(QStringLiteral("session_id")).toString();
    const QJsonObject remote = matchingRemote(slot, login, session);
    if (remote.isEmpty()) return;
    const QJsonObject chunk = envelope.value(QStringLiteral("payload")).toObject();
    const QString transferId = chunk.value(QStringLiteral("transfer_id")).toString();
    const int index = chunk.value(QStringLiteral("chunk_index")).toInt(-1);
    const int count = chunk.value(QStringLiteral("chunk_count")).toInt(-1);
    const QString digest = chunk.value(QStringLiteral("sha256")).toString();
    const QString encoded = chunk.value(QStringLiteral("data_base64")).toString();
    if (transferId.isEmpty() || count < 1 || count > 128 || index < 0 || index >= count || digest.size() != 64 ||
        encoded.size() > 32 * 1024)
        return;
    for (auto it = incoming_.begin(); it != incoming_.end();) {
        if (QDateTime::currentMSecsSinceEpoch() - it.value().started > 120'000)
            it = incoming_.erase(it);
        else
            ++it;
    }
    if (incoming_.size() >= 128 && !incoming_.contains(transferId)) return;
    Incoming transfer = incoming_.value(transferId);
    if (transfer.count == 0) {
        transfer = {slot,
                    login,
                    session,
                    envelope.value(QStringLiteral("kind")).toString(),
                    digest,
                    count,
                    QDateTime::currentMSecsSinceEpoch(),
                    {}};
    }
    if (transfer.ownerSlot != slot || transfer.ownerLogin != login || transfer.sessionId != session ||
        transfer.kind != envelope.value(QStringLiteral("kind")).toString() || transfer.digest != digest ||
        transfer.count != count)
        return;
    const QByteArray bytes = QByteArray::fromBase64(encoded.toLatin1());
    if (bytes.size() > 24 * 1024) return;
    transfer.chunks.insert(index, bytes);
    if (transfer.chunks.size() < count) {
        incoming_.insert(transferId, transfer);
        return;
    }
    incoming_.remove(transferId);
    QByteArray complete;
    for (int part = 0; part < count; ++part) {
        if (!transfer.chunks.contains(part)) return;
        complete.append(transfer.chunks.value(part));
        if (complete.size() > 3 * 1024 * 1024) return;
    }
    const QString actual = QString::fromLatin1(QCryptographicHash::hash(complete, QCryptographicHash::Sha256).toHex());
    if (actual != digest) return;
    const QJsonDocument document = QJsonDocument::fromJson(complete);
    if (!document.isObject()) return;
    QJsonObject decoded = document.object();
    if (decoded.value(QStringLiteral("type")).toString() == QStringLiteral("command_result")) {
        const QString commandId = decoded.value(QStringLiteral("command_id")).toString();
        const QString fingerprint = fingerprintByCommandId_.take(commandId);
        if (!fingerprint.isEmpty()) retryIdByFingerprint_.remove(fingerprint);
        const QString request = inputRequestByCommandId_.take(commandId);
        if (!request.isEmpty()) decoded.insert(QStringLiteral("request_id"), request);
    }
    if (payloadHandler_) payloadHandler_(remote, transfer.kind, decoded);
}

void DesktopSessionSyncCoordinator::acknowledge(int id) {
    requester_(QStringLiteral("session-sync/ack"), "POST", {{QStringLiteral("ids"), QJsonArray{id}}},
               [](const QJsonObject&, const QString&) {});
}

void DesktopSessionSyncCoordinator::queuePayload(const QString& slot, const QString& login, const QString& sessionId,
                                                 const QString& kind, const QJsonObject& payload) {
    const QByteArray bytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    constexpr int chunkSize = 24 * 1024;
    const int count = qMax(1, (bytes.size() + chunkSize - 1) / chunkSize);
    if (count > 128) {
        if (kind != QStringLiteral("event"))
            queuePayload(slot, login, sessionId, QStringLiteral("event"),
                         {{QStringLiteral("type"), QStringLiteral("payload_too_large")}});
        return;
    }
    const QString transferId = newId();
    const QString digest = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    for (int index = 0; index < count; ++index) {
        const QByteArray chunk = bytes.mid(index * chunkSize, chunkSize);
        outbound_.enqueue({slot,
                           login,
                           sessionId,
                           kind,
                           {{QStringLiteral("transfer_id"), transferId},
                            {QStringLiteral("chunk_index"), index},
                            {QStringLiteral("chunk_count"), count},
                            {QStringLiteral("sha256"), digest},
                            {QStringLiteral("data_base64"), QString::fromLatin1(chunk.toBase64())}},
                           transferId + QLatin1Char('_') + QString::number(index)});
    }
    flushOutbound();
}

void DesktopSessionSyncCoordinator::flushOutbound() {
    if (sending_ || outbound_.isEmpty()) return;
    sending_ = true;
    const Outgoing next = outbound_.head();
    QPointer<DesktopSessionSyncCoordinator> guard(this);
    requester_(QStringLiteral("session-sync/send"), "POST",
               {{QStringLiteral("to_slot"), next.recipientSlot},
                {QStringLiteral("to_login_session_id"), next.recipientLogin},
                {QStringLiteral("client_message_id"), next.id},
                {QStringLiteral("session_id"), next.sessionId},
                {QStringLiteral("kind"), next.kind},
                {QStringLiteral("payload"), next.payload}},
               [guard, next](const QJsonObject&, const QString& error) {
                   if (!guard) return;
                   guard->sending_ = false;
                   if (!guard->outbound_.isEmpty()) guard->outbound_.dequeue();
                   if (!error.isEmpty()) {
                       QQueue<Outgoing> remaining;
                       while (!guard->outbound_.isEmpty()) {
                           const Outgoing queued = guard->outbound_.dequeue();
                           if (queued.recipientSlot == next.recipientSlot &&
                               queued.recipientLogin == next.recipientLogin && queued.sessionId == next.sessionId)
                               continue;
                           remaining.enqueue(queued);
                       }
                       guard->outbound_ = std::move(remaining);
                   }
                   QTimer::singleShot(0, guard, [guard] {
                       if (guard) guard->flushOutbound();
                   });
               });
}

void DesktopSessionSyncCoordinator::scheduleSnapshot(const QString& sessionId, const QString& messageId) {
    if (sessionId.isEmpty()) return;
    AgentController* current = agentProvider_();
    if (!current) return;
    if (current != agent_) host();
    QString root = snapshotRootOwners_.value(sessionId);
    if (root.isEmpty()) {
        // AgentController 已把 Core 事件排到 Qt 宿主线程；在这里查归属，
        // 不在同步 Core publish 回调中查库。子任务问题必须发布到根会话房间。
        root = QString::fromStdString(maiQuestionRootSessionId(current->agent(), sessionId.toStdString()));
        if (root.isEmpty()) return;
        if (snapshotRootOwners_.size() >= 256) snapshotRootOwners_.clear();
        snapshotRootOwners_.insert(sessionId, root);
    }
    pendingSnapshotSessions_.insert(root);
    if (root == sessionId && !messageId.isEmpty()) pendingMessageUpdates_[root].insert(messageId);
    if (!snapshotTimer_.isActive()) snapshotTimer_.start();
}

void DesktopSessionSyncCoordinator::publishSnapshot(const QString& sessionId) {
    DesktopSessionSyncHost* current = host();
    if (!current || sessionId.isEmpty()) return;
    QJsonObject page = current->historyPage(sessionId, {}, 20);
    if (!page.value(QStringLiteral("ok")).toBool()) return;
    QJsonArray messages = page.value(QStringLiteral("messages")).toArray();
    auto updates = pendingMessageUpdates_.value(sessionId).values();
    std::sort(updates.begin(), updates.end());
    // 根 messages 保持原游标语义；这里仅为 publish 合入已知事件 owner，接收端按 ID 更新。
    for (int index = 0; index < qMin(20, updates.size()); ++index) {
        const auto id = updates[index];
        pendingMessageUpdates_[sessionId].remove(id);
        const auto updated = current->messageUpdate(sessionId, id);
        if (updated.isEmpty()) continue;
        bool replaced = false;
        for (int item = 0; item < messages.size(); ++item)
            if (messages[item].toObject().value(QStringLiteral("id")).toString() == id) {
                messages[item] = updated;
                replaced = true;
                break;
            }
        if (!replaced) messages.append(updated);
    }
    if (pendingMessageUpdates_.value(sessionId).isEmpty())
        pendingMessageUpdates_.remove(sessionId);
    else
        scheduleSnapshot(sessionId);
    page.insert(QStringLiteral("messages"), messages);
    publishPage(sessionId, page);
}

void DesktopSessionSyncCoordinator::processBackfillPage() {
    if (backfillSessions_.isEmpty()) return;
    const QString sessionId = backfillSessions_.head();
    DesktopSessionSyncHost* current = host();
    if (!backfillBefore_.contains(sessionId)) {
        const QJsonObject latest = current ? current->historyPage(sessionId, {}, 20) : QJsonObject{};
        const QJsonArray recent = latest.value(QStringLiteral("messages")).toArray();
        if (!latest.value(QStringLiteral("ok")).toBool() || recent.size() < 20) {
            backfillSessions_.dequeue();
        } else {
            backfillBefore_.insert(sessionId, recent.last().toObject().value(QStringLiteral("id")).toString());
        }
        if (!backfillSessions_.isEmpty()) backfillTimer_.start(5000);
        return;
    }
    const QJsonObject page =
        current ? current->historyPage(sessionId, backfillBefore_.value(sessionId), 20) : QJsonObject{};
    const QJsonArray messages = page.value(QStringLiteral("messages")).toArray();
    if (!page.value(QStringLiteral("ok")).toBool() || messages.isEmpty()) {
        backfillSessions_.dequeue();
        backfillBefore_.remove(sessionId);
    } else {
        publishPage(sessionId, page);
        backfillBefore_.insert(sessionId, messages.last().toObject().value(QStringLiteral("id")).toString());
        if (messages.size() < 20) {
            backfillSessions_.dequeue();
            backfillBefore_.remove(sessionId);
        }
    }
    if (!backfillSessions_.isEmpty()) backfillTimer_.start(5000);
}

void DesktopSessionSyncCoordinator::publishPage(const QString& sessionId, const QJsonObject& page) {
    const QJsonArray messages = page.value(QStringLiteral("messages")).toArray();
    const auto state = roomState(page);
    if (pendingMessagePublishes_.value(sessionId) > 0 &&
        roomStateDigest(desiredPublishedStates_.value(sessionId)) != roomStateDigest(state))
        roomStateRestoresPending_.insert(sessionId);
    desiredPublishedStates_.insert(sessionId, state);
    QList<QJsonObject> changed;
    for (const QJsonValue& message : messages) {
        const QString id = message.toObject().value(QStringLiteral("id")).toString();
        if (id.isEmpty()) continue;
        const QString key = sessionId + QLatin1Char(':') + id;
        const QByteArray digest = QCryptographicHash::hash(
            QJsonDocument(message.toObject()).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
        if (publishedMessageDigests_.value(key) == digest) continue;
        changed.append(message.toObject());
    }
    // 先登记整个批次，再发请求；测试/本地传输的同步回调也不能抢先补状态。
    if (!changed.isEmpty()) ++messagePublishRevisions_[sessionId];
    pendingMessagePublishes_[sessionId] += changed.size();
    const auto epoch = publishEpoch_;
    for (const auto& message : changed) {
        bool trimmed = false;
        const auto body = boundedRoomPublish(sessionId, QJsonArray{message}, state, trimmed);
        if (trimmed) roomStateRestoresPending_.insert(sessionId);
        if (QJsonDocument(body).toJson(QJsonDocument::Compact).size() > 95 * 1024) {
            --pendingMessagePublishes_[sessionId];
            initiallyPublishedSessions_.remove(sessionId);
            continue;
        }
        const QString key = sessionId + QLatin1Char(':') + message.value(QStringLiteral("id")).toString();
        const auto digest =
            QCryptographicHash::hash(QJsonDocument(message).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
        const auto actualStateDigest = roomStateDigest(body);
        publishedMessageDigests_.insert(key, digest);
        publishedStateDigests_.insert(sessionId, actualStateDigest);
        QPointer<DesktopSessionSyncCoordinator> guard(this);
        requester_(
            QStringLiteral("session-sync/publish"), "POST", body,
            [guard, key, digest, sessionId, actualStateDigest, epoch](const QJsonObject&, const QString& error) {
                if (!guard || guard->publishEpoch_ != epoch || !guard->desiredPublishedStates_.contains(sessionId))
                    return;
                if (!error.isEmpty()) {
                    if (guard->publishedMessageDigests_.value(key) == digest)
                        guard->publishedMessageDigests_.remove(key);
                    if (guard->publishedStateDigests_.value(sessionId) == actualStateDigest)
                        guard->publishedStateDigests_.remove(sessionId);
                    guard->initiallyPublishedSessions_.remove(sessionId);
                }
                // 失败也释放计数；旧 ACK 不写新的 digest，只在全批次结束后补最新状态。
                guard->pendingMessagePublishes_[sessionId] =
                    qMax(0, guard->pendingMessagePublishes_.value(sessionId) - 1);
                guard->restorePublishedRoomState(sessionId);
            });
    }
    restorePublishedRoomState(sessionId);
}

void DesktopSessionSyncCoordinator::restorePublishedRoomState(const QString& sessionId) {
    if (!desiredPublishedStates_.contains(sessionId) || pendingMessagePublishes_.value(sessionId) > 0 ||
        roomStateRestoresInFlight_.contains(sessionId))
        return;
    const auto state = desiredPublishedStates_.value(sessionId);
    const auto desiredDigest = roomStateDigest(state);
    if (!roomStateRestoresPending_.contains(sessionId) && publishedStateDigests_.value(sessionId) == desiredDigest)
        return;
    bool trimmed = false;
    const auto body = boundedRoomPublish(sessionId, QJsonArray{}, state, trimmed);
    if (QJsonDocument(body).toJson(QJsonDocument::Compact).size() > 95 * 1024) return;
    const auto actualDigest = roomStateDigest(body);
    const auto revision = messagePublishRevisions_.value(sessionId);
    const auto epoch = publishEpoch_;
    roomStateRestoresPending_.remove(sessionId);
    roomStateRestoresInFlight_.insert(sessionId);
    publishedStateDigests_.insert(sessionId, actualDigest);
    QPointer<DesktopSessionSyncCoordinator> guard(this);
    requester_(
        QStringLiteral("session-sync/publish"), "POST", body,
        [guard, sessionId, actualDigest, desiredDigest, revision, epoch](const QJsonObject&, const QString& error) {
            if (!guard || guard->publishEpoch_ != epoch || !guard->desiredPublishedStates_.contains(sessionId)) return;
            guard->roomStateRestoresInFlight_.remove(sessionId);
            if (!error.isEmpty()) {
                if (guard->publishedStateDigests_.value(sessionId) == actualDigest)
                    guard->publishedStateDigests_.remove(sessionId);
                guard->roomStateRestoresPending_.insert(sessionId);
                guard->initiallyPublishedSessions_.remove(sessionId);
                return;  // 网络失败留给下一次真实快照重试，不在回调中无限重发。
            }
            if (guard->messagePublishRevisions_.value(sessionId) != revision ||
                roomStateDigest(guard->desiredPublishedStates_.value(sessionId)) != desiredDigest) {
                guard->roomStateRestoresPending_.insert(sessionId);
                guard->restorePublishedRoomState(sessionId);
            }
        });
}
