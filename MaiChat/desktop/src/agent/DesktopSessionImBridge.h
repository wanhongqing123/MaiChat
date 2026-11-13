// Explicit friend-to-local-Session bindings reuse the existing IM origin
// protocol. MaiSessionIMRouter owns binding/queue/receipt authority; this class
// only adapts Qt, ordinary Core execution, media preparation and native SDK I/O.
#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
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
    QHash<QString, QString> readyBindings() const;
    bool historyPending(const QString& peerId) const;
    // Pass the existing ordinary Session-list projection at startup. Cleanup
    // runs bounded Core maintenance on preparation I/O, never on the UI thread.
    void scheduleHistoryMaintenance(const QStringList& localRootSessionIds);
    // Local input/stop takes ownership of the Session. Unbinding never stops
    // its model or tools; it only revokes pending/late automatic IM delivery.
    void localTakeover(const QString& sessionId);
    // Latches new input off immediately; a later ready IM connection may resume
    // new live input. Pending work is never replayed after this boundary.
    void suspend();

signals:
    void bindingChanged(const QString& peerId);
    void relayError(const QString& peerId, const QString& message);
    void historyImported(const QString& peerId, quint64 records, qint64 maximumStepMillis);

private:
    void acceptLive(quint64 ingressId, const RemoteIMMessage& message,
                    quint64 rawSequence = 0, quint64 rawGeneration = 0);
    void persistedLive(quint64 ingressId, bool ok, bool inserted, const QString& rowId, const QString& error);
    void readyLive(quint64 ingressId, const RemoteIMMessage& message);
    void failLive(quint64 ingressId, const QString& error);
    void advanceHistory(const QString& peerId);
    void abortHistory(const QString& peerId, const QString& error);
    void preparePayload(quint64 inputId);
    void syncScope();
    void dispatch(const QString& sessionId);
    void processReservation(quint64 token);
    void finish(const QString& sessionId);
    void acknowledgeDispatch(quint64 token, quint32 index);
    void prunePlatformWork();
    void pruneSessions();
    struct Runtime;
    std::unique_ptr<Runtime> runtime_;
};
