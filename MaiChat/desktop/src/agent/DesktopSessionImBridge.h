// Explicit friend-to-local-Session bindings reuse the existing IM origin
// protocol. This adapter owns routing receipts, not another Agent executor.
#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <memory>

class AgentController;
class RemoteIMApplication;
struct RemoteIMMessage;

class DesktopSessionImBridge final : public QObject {
    Q_OBJECT
public:
    DesktopSessionImBridge(RemoteIMApplication& app, AgentController& controller, QObject* parent = nullptr);
    ~DesktopSessionImBridge() override;

    bool bind(const QString& peerId, const QString& sessionId, QString* error = nullptr);
    void unbind(const QString& peerId);
    QString boundSession(const QString& peerId) const;
    QHash<QString, QString> bindings() const;
    // Local input/stop takes ownership of the Session. Unbinding never stops
    // its model or tools; it only revokes pending/late automatic IM delivery.
    void localTakeover(const QString& sessionId);
    // Latches new input off immediately; a later ready IM connection may resume
    // new live input. Pending work is never replayed after this boundary.
    void suspend();

signals:
    void bindingChanged(const QString& peerId);
    void relayError(const QString& peerId, const QString& message);

private:
    void acceptLive(const QString& peerId, const RemoteIMMessage& message);
    void dispatch(const QString& sessionId);
    void finish(const QString& sessionId);
    void completeMediaDispatch(const QString& sessionId, const QString& assistantId, bool abortRemaining = false);
    void pruneSessions();
    struct Runtime;
    std::unique_ptr<Runtime> runtime_;
};
