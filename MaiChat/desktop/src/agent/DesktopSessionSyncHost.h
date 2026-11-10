// 文件作用：把本机 MaiAgent 会话映射为跨设备同步协议，并执行已鉴权的远端操作。
// 调用方必须先由账号服务验证发送方与本机属于同一个 MaiChat 账号。
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <memory>

class AgentController;

class DesktopSessionSyncHost final {
public:
    explicit DesktopSessionSyncHost(AgentController& controller);
    ~DesktopSessionSyncHost();
    void setDefaultWorkspaceDirectory(const QString& directory);
    void setImagePublicationScope(const QString& account, const QString& slot, const QString& login);
    // Enroll only images created after this host subscribed to live events.
    // History serialization reads journal receipts and never creates uploads.
    void enrollLiveImages(const QString& sessionId, const QString& messageId, const QString& partId = {});
    QString imageSource(const QString& sessionId, const QString& messageId, const QString& partId) const;

    QJsonArray rootSessions() const;
    QJsonObject historyPage(const QString& sessionId, const QString& beforeId, int limit) const;
    // 已收到事件的根会话消息精读；不改变普通分页，不读取或转发其它子任务正文。
    // 未完成正文可覆盖为宿主已收到的 live 文本，reasoning 始终不进入正文。
    // 不存在或不属于该根会话时返回空对象。
    QJsonObject messageUpdate(const QString& sessionId, const QString& messageId) const;
    // Exact persisted receipt lookup; never infer the accepted row from later output.
    QJsonObject historyMessage(const QString& sessionId, const QString& messageId) const;
    QJsonObject applyCommand(const QString& sessionId, const QJsonObject& payload);

private:
    AgentController& controller_;
    QString defaultWorkspaceDirectory_;
    QString imageAccount_, imageSlot_, imageLogin_;
    QJsonObject decorateImages(QJsonObject message, const QString& sessionId) const;
    // 事件回调只更新共享 revision，不捕获宿主 this；退订后在途回调也不悬空。
    struct Runtime;
    std::shared_ptr<Runtime> runtime_;
};
