#pragma once

#include "auth/DesktopVideoCircleAccess.h"

#include <QJsonObject>
#include <QString>
#include <functional>
struct MaiCloudDriveCancel;

struct DesktopSessionRoom {
    QString slot;
    QString login;
    QString session;
    QString key() const { return slot + QLatin1Char(':') + session; }
};

struct DesktopSessionAttachmentResult {
    QJsonObject metadata;
    QString path;
    QString error;
    qint64 generation = 0;
    QString errorCode;
    // Optional local lifetime/source lease; never serialized or retained by Core.
    std::function<bool()> canCommit;
    bool requiresSettledAck = false;
    bool ok() const { return error.isEmpty(); }
};

// Synchronous adapter; callers run upload/download on a worker thread.
class DesktopSessionAttachments final {
public:
    static DesktopSessionAttachmentResult upload(const DesktopVideoCircleAccess& access,
        const DesktopSessionRoom& room, const QString& source);
    static DesktopSessionAttachmentResult download(const DesktopVideoCircleAccess& access,
        const DesktopSessionRoom& room, const QString& attachmentId);
    // Journal reads never enroll old history. A live image event must durably
    // enroll pending before the coordinator publishes it and starts an upload.
    static bool enrollTemporaryImage(const QString& account, const DesktopSessionRoom& room,
        const QString& messageId, const QString& partId);
    static QJsonObject imagePublication(const QString& account, const DesktopSessionRoom& room,
        const QString& messageId, const QString& partId);
    // Explicit user preparation only. Reuse healthy ready/pending; a new request
    // renews a settled/expired publication by exactly one durable generation.
    // Generation 0 omits the wire field for old Sync servers; no history scan.
    static DesktopSessionAttachmentResult prepareTemporaryImage(const QString& account, const DesktopSessionRoom& room,
                                                                const QString& messageId, const QString& partId,
                                                                const QString& requestId,
                                                                bool settledAcknowledged = false,
                                                                qint64 expectedGeneration = -1);
    static DesktopSessionAttachmentResult uploadTemporaryImage(const DesktopVideoCircleAccess& access,
                                                               const DesktopSessionRoom& room, const QString& messageId,
                                                               const QString& partId, const QString& source,
                                                               qint64 expectedGeneration = -1,
                                                               const MaiCloudDriveCancel* cancel = nullptr,
                                                               std::function<bool()> canCommit = {});
    static DesktopSessionAttachmentResult downloadTemporaryImage(const DesktopVideoCircleAccess& access,
        const DesktopSessionRoom& room, const QString& messageId, const QString& partId, const QJsonObject& image,
        const MaiCloudDriveCancel* cancel = nullptr);
    static DesktopSessionAttachmentResult cleanupRoom(const DesktopVideoCircleAccess& access,
        const DesktopSessionRoom& room);
};
