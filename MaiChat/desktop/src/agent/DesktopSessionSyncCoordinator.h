// 文件作用：保持桌面端会话在线，接收同账号设备的操作并回传历史与状态。
#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>

#include "auth/DesktopVideoCircleAccess.h"

class AgentController;
class DesktopSessionSyncHost;
struct DesktopSessionRoom;

class DesktopSessionSyncCoordinator final : public QObject {
public:
    using Completion = std::function<void(const QJsonObject&, const QString&)>;
    using Requester = std::function<void(const QString&, const QByteArray&, const QJsonObject&, Completion)>;
    using AgentProvider = std::function<AgentController*()>;
    using CatalogHandler = std::function<void(const QJsonArray&)>;
    using DeviceHandler = std::function<void(const QJsonArray&)>;
    using PayloadHandler = std::function<void(const QJsonObject&, const QString&, const QJsonObject&)>;
    using AttachmentAccessProvider = std::function<DesktopVideoCircleAccess()>;

    DesktopSessionSyncCoordinator(Requester requester, AgentProvider agent, QObject* parent = nullptr,
                                  QString deviceId = {}, QString deviceName = {});
    ~DesktopSessionSyncCoordinator() override;

    void start();
    void connectionReady();
    void connectionLost();
    void acceptCatalog(const QJsonArray& sessions);
    void acceptDevices(const QJsonArray& devices);
    void acceptInbox(const QJsonArray& messages);
    void setCatalogHandler(CatalogHandler handler);
    void setDeviceHandler(DeviceHandler handler);
    void setPayloadHandler(PayloadHandler handler);
    void setAttachmentAccessProvider(AttachmentAccessProvider provider);
    void requestRemoteHistory(const QJsonObject& session, const QString& before, int limit, Completion completion);
    void sendRemoteCommand(const QJsonObject& session, const QJsonObject& payload, Completion completion);
    void sendRemoteLifecycle(const QJsonObject& device, const QJsonObject& payload, Completion completion);
    void setDefaultWorkspaceDirectory(const QString& directory);
    QJsonArray remoteDevices() const {
        return remoteDevices_;
    }
    QJsonArray remoteSessions() const {
        return remoteSessions_;
    }
    DesktopSessionRoom localRoom(const QString& sessionId) const;

private:
    struct Outgoing {
        QString recipientSlot;
        QString recipientLogin;
        QString sessionId;
        QString kind;
        QJsonObject payload;
        QString id;
    };
    struct Incoming {
        QString ownerSlot;
        QString ownerLogin;
        QString sessionId;
        QString kind;
        QString digest;
        int count = 0;
        qint64 started = 0;
        QMap<int, QByteArray> chunks;
    };
    struct PendingLifecycle {
        QString slot;
        QString login;
        Completion completion;
    };

    DesktopSessionSyncHost* host();
    void publishPresence();
    void handleEnvelope(const QJsonObject& envelope);
    void respondToCommand(const QJsonObject& envelope, QJsonObject result);
    void dispatchQueuedInput(const QString& sessionId);
    void cancelQueuedInputs(const QString& sessionId, const QString& reason);
    bool isCurrentInput(const QJsonObject& envelope, quint64 generation) const;
    void startAttachmentCommand(const QJsonObject& envelope);
    void publishAttachmentHistory(const QJsonObject& body, int attempts);
    void finishDeletedRoom(const QString& sessionId);
    void acknowledge(int id);
    void handleRemoteChunk(const QJsonObject& envelope);
    QJsonObject matchingRemote(const QString& slot, const QString& login, const QString& session) const;
    QJsonObject matchingDevice(const QJsonObject& device) const;
    void queuePayload(const QString& slot, const QString& login, const QString& sessionId, const QString& kind,
                      const QJsonObject& payload);
    void flushOutbound();
    void scheduleSnapshot(const QString& sessionId, const QString& messageId = QString());
    void publishSnapshot(const QString& sessionId);
    void publishPage(const QString& sessionId, const QJsonObject& page);
    void restorePublishedRoomState(const QString& sessionId);
    void processBackfillPage();

    Requester requester_;
    AgentProvider agentProvider_;
    QPointer<AgentController> agent_;
    std::unique_ptr<DesktopSessionSyncHost> host_;
    QTimer presenceTimer_;
    QTimer snapshotTimer_;
    // parentId 创建后不变；每个安装实例内缓存变更来源的根房间，逐字 delta 不重复查库。
    QHash<QString, QString> snapshotRootOwners_;
    QTimer backfillTimer_;
    QString deviceId_;
    QString deviceName_;
    QString localLoginSession_;
    QSet<QString> pendingSnapshotSessions_;
    // 已接收事件的原消息可能被 steer 挤出最新页；每次最多点取二十条，不全量重扫。
    QHash<QString, QSet<QString>> pendingMessageUpdates_;
    QSet<QString> initiallyPublishedSessions_;
    QSet<QString> pendingAttachmentCleanupRooms_;
    QQueue<QString> backfillSessions_;
    QHash<QString, QString> backfillBefore_;
    QHash<QString, QByteArray> publishedMessageDigests_;
    QHash<QString, QByteArray> publishedStateDigests_;
    // 裁过显示投影的消息帧全部确认后，再补最新完整小状态；旧回执不覆盖新代次。
    QHash<QString, QJsonObject> desiredPublishedStates_;
    QHash<QString, int> pendingMessagePublishes_;
    QHash<QString, quint64> messagePublishRevisions_;
    QSet<QString> roomStateRestoresPending_;
    QSet<QString> roomStateRestoresInFlight_;
    quint64 publishEpoch_ = 0;
    QHash<QString, Incoming> incoming_;
    // Preserve wire order through asynchronous attachment preparation. Controls
    // bypass this FIFO; accepted input execution remains in the shared Agent.
    QHash<QString, QQueue<QJsonObject>> inputCommands_;
    QSet<QString> preparingInputSessions_;
    QSet<int> pendingInputEnvelopeIds_;
    quint64 inputCommandGeneration_ = 0;
    QHash<QString, QString> retryIdByFingerprint_;
    QHash<QString, QString> fingerprintByCommandId_;
    QHash<QString, QString> inputRequestByCommandId_;
    QHash<QString, PendingLifecycle> pendingLifecycle_;
    QJsonArray remoteSessions_;
    QJsonArray remoteDevices_;
    QString defaultWorkspaceDirectory_;
    CatalogHandler catalogHandler_;
    DeviceHandler deviceHandler_;
    PayloadHandler payloadHandler_;
    AttachmentAccessProvider attachmentAccessProvider_;
    QQueue<Outgoing> outbound_;
    bool started_ = false;
    bool sending_ = false;
};
