// 文件作用：桌面端显示同账号其他设备的 Session；所有执行请求都回到所属设备。
#pragma once

#include <QJsonObject>
#include <QWidget>

#include <functional>
#include <memory>

class RemoteAgentPanel final : public QWidget {
public:
    using Completion = std::function<void(const QString&)>;
    using CommandHandler = std::function<void(const QJsonObject&, const QJsonObject&, Completion)>;
    using HistoryHandler = std::function<void(const QJsonObject&, const QString&, int, Completion)>;
    using DownloadCompletion = std::function<void(const QString&, const QString&)>;
    using DownloadHandler = std::function<void(const QJsonObject&, const QString&, DownloadCompletion)>;

    // outboxFile may select a private embedding/test store; empty uses app-local data.
    explicit RemoteAgentPanel(QWidget* parent = nullptr, const QString& outboxFile = {});
    ~RemoteAgentPanel() override;

    void setCommandHandler(CommandHandler handler);
    void setHistoryHandler(HistoryHandler handler);
    void setDownloadHandler(DownloadHandler handler);
    void openSession(const QJsonObject& remote);
    void updateSession(const QJsonObject& remote);
    void receivePayload(const QJsonObject& remote, const QString& kind, const QJsonObject& payload);
    QJsonObject session() const;
    // 上传完成后保留原服务器附件引用，未知回执重试不得重新导入同一内容。
    void rememberPreparedPrompt(const QString& requestId, const QJsonObject& payload);

private:
    struct Runtime;
    std::unique_ptr<Runtime> runtime_;
    void refreshHistory();
    void requestHistory(const QString& before, int limit);
    void sendOrStop();
    bool persistOutbox();
    void dispatchQueued(const QString& ownerKey = {});
    void finishQueued(const QString& requestId, const QJsonObject& result, bool unknown = false);
    void recoverQueuedDraft();
    void refreshAttachments();
    void sendAction(const QJsonObject& action, bool clearDraft, const QJsonObject& question = {});
    void sendPrompt(const QJsonObject& action, const QString& retryId = {});
    void removeOptimisticMessage(const QString& id);
    void renderMessages();
    void updateThinking();
    void renderRequests(const QJsonObject& payload);
    void updateControls();
    void showStatus(const QString& message, bool failure = false);
};
