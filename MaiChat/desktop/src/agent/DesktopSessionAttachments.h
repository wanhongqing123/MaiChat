#pragma once

#include "auth/DesktopVideoCircleAccess.h"

#include <QJsonObject>
#include <QString>
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
    static DesktopSessionAttachmentResult uploadTemporaryImage(const DesktopVideoCircleAccess& access,
        const DesktopSessionRoom& room, const QString& messageId, const QString& partId, const QString& source);
    static DesktopSessionAttachmentResult downloadTemporaryImage(const DesktopVideoCircleAccess& access,
        const DesktopSessionRoom& room, const QString& messageId, const QString& partId, const QJsonObject& image,
        const MaiCloudDriveCancel* cancel = nullptr);
    static DesktopSessionAttachmentResult cleanupRoom(const DesktopVideoCircleAccess& access,
        const DesktopSessionRoom& room);
};
