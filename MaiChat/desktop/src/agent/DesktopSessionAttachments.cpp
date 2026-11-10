#include "agent/DesktopSessionAttachments.h"

#include "MaiCloudDriveClient.h"
#include "MaiCurlDownload.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QResource>
#include <QStandardPaths>
#include <QSaveFile>
#include <QMutex>
#include <QMutexLocker>
#include <QUuid>

static QByteArray trustedRoots(const DesktopVideoCircleAccess& access) {
    if (!access.caPem.isEmpty()) return access.caPem;
    Q_INIT_RESOURCE(AuthCertificates);
    QFile resource(QStringLiteral(":/maichat/auth/cacert.pem"));
    return resource.open(QIODevice::ReadOnly) ? resource.readAll() : QByteArray();
}

namespace {

struct Call {
    QByteArray base, token, roots, proxy, host, slot, login, session, message, part;
    MaiCloudDriveOptions options{};
    explicit Call(const DesktopVideoCircleAccess& access, const DesktopSessionRoom& room)
        : base(access.authBaseUrl.toUtf8()), token(access.accountToken.toUtf8()),
          roots(trustedRoots(access)), proxy(access.proxyUrl.toUtf8()),
          host(access.signedUploadHostSuffix.toUtf8()), slot(room.slot.toUtf8()),
          login(room.login.toUtf8()), session(room.session.toUtf8()) {
        options.auth_base_url_utf8 = base.constData();
        options.account_token_utf8 = token.constData();
        options.ca_pem = roots.isEmpty() ? nullptr : roots.constData();
        options.ca_pem_length = size_t(roots.size());
        options.proxy_url_utf8 = access.proxyUrl.isNull() ? nullptr : proxy.constData();
        options.signed_upload_host_suffix_utf8 = host.isEmpty() ? nullptr : host.constData();
        options.owner_device_slot_utf8 = slot.constData();
        options.owner_login_session_id_utf8 = login.constData();
        options.session_id_utf8 = session.constData();
        options.retain_completed_checkpoint = 1;
    }
    void temporary(const QString& messageId, const QString& partId) {
        message = messageId.toUtf8(); part = partId.toUtf8();
        options.temporary_session_image = 1;
        options.message_id_utf8 = message.constData();
        options.part_id_utf8 = part.constData();
    }
};

DesktopSessionAttachmentResult parse(MaiCloudDriveResult* raw) {
    DesktopSessionAttachmentResult reply;
    if (raw->response_json_utf8)
        reply.metadata = QJsonDocument::fromJson(QByteArray(raw->response_json_utf8,
            int(raw->response_bytes))).object();
    if (raw->status != MAI_CLOUD_DRIVE_OK)
        reply.error = raw->http_status == 401 ? QStringLiteral("请重新登录 MaiChat") :
            raw->http_status == 413 ? QStringLiteral("附件超过大小限制或会话空间已满") :
            raw->http_status == 404 ? QStringLiteral("会话附件已不存在") :
            QStringLiteral("会话附件传输失败，请重试（%1）").arg(int(raw->status));
    mai_cloud_drive_result_free(raw);
    return reply;
}

QString directory(const QString& name, const QString& account, bool cache = false) {
    const QString root = QStandardPaths::writableLocation(
        cache ? QStandardPaths::CacheLocation : QStandardPaths::AppDataLocation);
    const QString digest = QString::fromLatin1(QCryptographicHash::hash(
        account.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
    const QString path = QDir(root).filePath(QStringLiteral("session-attachments/") + digest +
                                             QLatin1Char('/') + name);
    return QDir().mkpath(path) ? path : QString();
}

QMutex journalMutex;

QString imageKey(const DesktopSessionRoom& room, const QString& message, const QString& part) {
    // The original login is stored in the immutable journal binding. This
    // lookup survives owner relogin so later text cannot erase a ready receipt.
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(QJsonArray{room.slot, room.session, message, part}).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
}

QString journalPath(const QString& account, const DesktopSessionRoom& room,
                    const QString& message, const QString& part) {
    const auto folder = directory(QStringLiteral("image-publications"), account);
    return folder.isEmpty() ? QString() : QDir(folder).filePath(imageKey(room, message, part) + QStringLiteral(".json"));
}

QJsonObject readJournal(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

bool writeJournal(const QString& path, const QJsonObject& object) {
    QSaveFile file(path);
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return !path.isEmpty() && file.open(QIODevice::WriteOnly) &&
        file.write(bytes) == bytes.size() && file.commit();
}

bool matchesBinding(const QJsonObject& journal, const DesktopSessionRoom& room,
                    const QString& message, const QString& part) {
    return journal.value(QStringLiteral("owner_slot")).toString() == room.slot &&
        journal.value(QStringLiteral("session_id")).toString() == room.session &&
        journal.value(QStringLiteral("message_id")).toString() == message &&
        journal.value(QStringLiteral("part_id")).toString() == part &&
        !journal.value(QStringLiteral("owner_login_session_id")).toString().isEmpty();
}

}  // namespace

bool DesktopSessionAttachments::enrollTemporaryImage(const QString& account, const DesktopSessionRoom& room,
    const QString& messageId, const QString& partId) {
    if (account.isEmpty() || room.slot.isEmpty() || room.login.isEmpty() || room.session.isEmpty() ||
        messageId.isEmpty() || partId.isEmpty()) return false;
    QMutexLocker lock(&journalMutex);
    const auto path = journalPath(account, room, messageId, partId);
    if (QFileInfo::exists(path)) return matchesBinding(readJournal(path), room, messageId, partId);
    return writeJournal(path, {{QStringLiteral("owner_slot"), room.slot},
        {QStringLiteral("owner_login_session_id"), room.login}, {QStringLiteral("session_id"), room.session},
        {QStringLiteral("message_id"), messageId}, {QStringLiteral("part_id"), partId},
        {QStringLiteral("remoteImage"), QJsonObject{{QStringLiteral("state"), QStringLiteral("pending")},
                                                  {QStringLiteral("retention"), QStringLiteral("temporary")}}}});
}

QJsonObject DesktopSessionAttachments::imagePublication(const QString& account, const DesktopSessionRoom& room,
    const QString& messageId, const QString& partId) {
    if (account.isEmpty()) return {};
    QMutexLocker lock(&journalMutex);
    const auto journal = readJournal(journalPath(account, room, messageId, partId));
    return matchesBinding(journal, room, messageId, partId)
        ? journal.value(QStringLiteral("remoteImage")).toObject() : QJsonObject{};
}

DesktopSessionAttachmentResult DesktopSessionAttachments::uploadTemporaryImage(
    const DesktopVideoCircleAccess& access, const DesktopSessionRoom& room,
    const QString& messageId, const QString& partId, const QString& source) {
    const auto path = journalPath(access.accountId, room, messageId, partId);
    QJsonObject journal;
    {
        QMutexLocker lock(&journalMutex);
        journal = readJournal(path);
    }
    if (!matchesBinding(journal, room, messageId, partId))
        return {{}, {}, QStringLiteral("图片尚未发布")};
    const auto previous = journal.value(QStringLiteral("remoteImage")).toObject();
    if (previous.value(QStringLiteral("state")).toString() != QStringLiteral("pending"))
        return {previous, {}, previous.value(QStringLiteral("state")).toString() == QStringLiteral("ready")
            ? QString() : QStringLiteral("图片发布已结束")};
    DesktopSessionAttachmentResult reply;
    const QFileInfo file(source);
    MaiCloudDriveMediaInfo media{};
    const auto name = file.fileName().toUtf8();
    if (!access.isValid() || room.login != journal.value(QStringLiteral("owner_login_session_id")).toString() ||
        !file.isFile() || !mai_session_attachment_media_info(name.constData(), &media) ||
        QByteArray(media.media_type_utf8) != "image" || file.size() < 1 || quint64(file.size()) > media.max_size_bytes) {
        reply.error = QStringLiteral("图片文件或账号不可用");
    } else {
        Call call(access, room);
        call.temporary(messageId, partId);
        const auto checkpoint = (path + QStringLiteral(".maicloud.json")).toUtf8();
        const auto sourcePath = file.absoluteFilePath().toUtf8();
        MaiCloudDriveResult raw{};
        mai_cloud_drive_upload_resumable(&call.options, sourcePath.constData(), checkpoint.constData(),
            name.constData(), media.content_type_utf8, nullptr, nullptr, nullptr, &raw);
        reply = parse(&raw);
    }
    QJsonObject image{{QStringLiteral("retention"), QStringLiteral("temporary")}};
    if (reply.ok()) {
        // Only the fixed receipt enters WS metadata. Paths, signed URLs, OSS
        // keys and multipart state remain confined to the local transfer core.
        for (const auto& key : {"attachment_id", "room_key", "owner_login_session_id", "name", "media_type",
                                "content_type", "size_bytes", "expires_at"})
            image.insert(QLatin1String(key), reply.metadata.value(QLatin1String(key)));
        image.insert(QStringLiteral("state"), QStringLiteral("ready"));
    } else {
        image.insert(QStringLiteral("state"), QStringLiteral("failed"));
        image.insert(QStringLiteral("error_code"), QStringLiteral("upload_failed"));
        // A failed attempt also retains a server-assigned expiry when its init
        // receipt reached the durable checkpoint before the transport failed.
        const auto checkpoint = readJournal(path + QStringLiteral(".maicloud.json"));
        if (checkpoint.value(QStringLiteral("expires_at")).toDouble() > 0)
            image.insert(QStringLiteral("expires_at"), checkpoint.value(QStringLiteral("expires_at")));
    }
    journal.insert(QStringLiteral("remoteImage"), image);
    {
        QMutexLocker lock(&journalMutex);
        if (!writeJournal(path, journal)) reply.error = QStringLiteral("无法保存图片发布回执");
    }
    reply.metadata = image;
    return reply;
}

DesktopSessionAttachmentResult DesktopSessionAttachments::downloadTemporaryImage(
    const DesktopVideoCircleAccess& access, const DesktopSessionRoom& room,
    const QString& messageId, const QString& partId, const QJsonObject& image, const MaiCloudDriveCancel* cancel) {
    if (!access.isValid() || image.value(QStringLiteral("retention")).toString() != QStringLiteral("temporary") ||
        (image.value(QStringLiteral("state")).toString() != QStringLiteral("ready") &&
         image.value(QStringLiteral("state")).toString() != QStringLiteral("expired")) ||
        image.value(QStringLiteral("room_key")).toString() != room.key() ||
        image.value(QStringLiteral("owner_login_session_id")).toString().isEmpty())
        return {{}, {}, QStringLiteral("图片引用无效")};
    const auto folder = directory(QStringLiteral("image-cache"), access.accountId, true);
    const auto id = image.value(QStringLiteral("attachment_id")).toString();
    const auto bytes = image.value(QStringLiteral("size_bytes")).toVariant().toLongLong();
    if (folder.isEmpty() || id.isEmpty() || bytes < 1 || bytes > 20'000'000)
        return {{}, {}, QStringLiteral("图片信息无效")};
    DesktopSessionRoom originalRoom = room;
    originalRoom.login = image.value(QStringLiteral("owner_login_session_id")).toString();
    const auto digest = QCryptographicHash::hash((imageKey(originalRoom, messageId, partId) +
        QLatin1Char(':') + originalRoom.login + QLatin1Char(':') + id).toUtf8(), QCryptographicHash::Sha256).toHex();
    const auto target = QDir(folder).filePath(QString::fromLatin1(digest) + QStringLiteral(".image"));
    // Only the shared new-file downloader publishes this cache path. A complete
    // cached image remains usable after the server's fixed retention expires.
    if (QFileInfo(target).isFile() && QFileInfo(target).size() == bytes) return {image, target, {}};
    if (image.value(QStringLiteral("expires_at")).toDouble() <= QDateTime::currentMSecsSinceEpoch())
        return {image, {}, QStringLiteral("图片已过期")};
    Call call(access, originalRoom);
    call.temporary(messageId, partId);
    QJsonObject request;
    for (const auto& key : {"attachment_id", "name", "content_type", "media_type", "size_bytes", "expires_at"})
        request.insert(QLatin1String(key), image.value(QLatin1String(key)));
    request.insert(QStringLiteral("output_path"), target);
    const auto arguments = QJsonDocument(request).toJson(QJsonDocument::Compact);
    MaiCloudDriveResult raw{};
    mai_cloud_drive_request(&call.options, MAI_SESSION_IMAGE_DOWNLOAD, arguments.constData(), cancel, &raw);
    const bool expired = raw.http_status == 410;
    auto reply = parse(&raw);
    if (reply.ok()) reply.path = target;
    else if (expired) reply.error = QStringLiteral("图片已过期");
    return reply;
}

DesktopSessionAttachmentResult DesktopSessionAttachments::upload(
    const DesktopVideoCircleAccess& access, const DesktopSessionRoom& room,
    const QString& source) {
    if (!access.isValid() || !QFileInfo(source).isFile())
        return {{}, {}, QStringLiteral("会话附件文件或账号不可用")};
    const QFileInfo file(source);
    const QByteArray name = file.fileName().toUtf8();
    MaiCloudDriveMediaInfo media{};
    if (!mai_session_attachment_media_info(name.constData(), &media) ||
        file.size() < 1 || quint64(file.size()) > media.max_size_bytes)
        return {{}, {}, QStringLiteral("会话附件格式或大小无效")};
    const QString folder = directory(QStringLiteral("uploads"), access.accountId);
    if (folder.isEmpty()) return {{}, {}, QStringLiteral("无法保存会话附件上传记录")};
    const QByteArray digest = QCryptographicHash::hash(
        (room.key() + QLatin1Char(':') + file.absoluteFilePath()).toUtf8(),
        QCryptographicHash::Sha256).toHex();
    const QString checkpoint = QDir(folder).filePath(
        QString::fromLatin1(digest) + QStringLiteral(".maicloud.json"));
    Call call(access, room);
    const QByteArray path = file.absoluteFilePath().toUtf8();
    const QByteArray task = checkpoint.toUtf8();
    MaiCloudDriveResult raw{};
    mai_cloud_drive_upload_resumable(&call.options, path.constData(), task.constData(),
        name.constData(), media.content_type_utf8, nullptr, nullptr, nullptr, &raw);
    auto reply = parse(&raw);
    if (reply.ok() &&
        (reply.metadata.value(QStringLiteral("room_key")).toString() != room.key() ||
         reply.metadata.value(QStringLiteral("name")).toString() != file.fileName() ||
         reply.metadata.value(QStringLiteral("content_type")).toString() !=
             QString::fromLatin1(media.content_type_utf8) ||
         reply.metadata.value(QStringLiteral("media_type")).toString() !=
             QString::fromLatin1(media.media_type_utf8) ||
         reply.metadata.value(QStringLiteral("size_bytes")).toVariant().toLongLong() != file.size()))
        reply.error = QStringLiteral("会话附件上传回执不匹配");
    return reply;
}

DesktopSessionAttachmentResult DesktopSessionAttachments::download(
    const DesktopVideoCircleAccess& access, const DesktopSessionRoom& room,
    const QString& attachmentId) {
    if (!access.isValid()) return {{}, {}, QStringLiteral("请先登录 MaiChat")};
    Call call(access, room);
    const QByteArray request = QJsonDocument(QJsonObject{
        {QStringLiteral("video_id"), attachmentId}}).toJson(QJsonDocument::Compact);
    MaiCloudDriveResult raw{};
    mai_cloud_drive_request(&call.options, MAI_CLOUD_DRIVE_PLAY_LINK,
        request.constData(), nullptr, &raw);
    auto reply = parse(&raw);
    if (!reply.ok()) return reply;
    const QString name = reply.metadata.value(QStringLiteral("name")).toString();
    const QByteArray display = name.toUtf8();
    MaiCloudDriveMediaInfo media{};
    const qint64 bytes = reply.metadata.value(QStringLiteral("size_bytes")).toVariant().toLongLong();
    if (!mai_session_attachment_media_info(display.constData(), &media) ||
        bytes < 1 || quint64(bytes) > media.max_size_bytes ||
        reply.metadata.value(QStringLiteral("room_key")).toString() != room.key() ||
        reply.metadata.value(QStringLiteral("content_type")).toString() !=
            QString::fromLatin1(media.content_type_utf8) ||
        reply.metadata.value(QStringLiteral("media_type")).toString() !=
            QString::fromLatin1(media.media_type_utf8)) {
        reply.error = QStringLiteral("会话附件归属或文件信息无效");
        return reply;
    }
    const QString folder = directory(QStringLiteral("downloads"), access.accountId, true);
    if (folder.isEmpty()) return {{}, {}, QStringLiteral("无法准备会话附件目录")};
    const QString target = QDir(folder).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) +
                                               QLatin1Char('.') + QFileInfo(name).suffix());
    const QByteArray signedUrl = reply.metadata.value(QStringLiteral("read_url")).toString().toUtf8();
    const QByteArray path = target.toUtf8();
    MaiDownloadOptions options{};
    options.url_utf8 = signedUrl.constData();
    options.target_path_utf8 = path.constData();
    options.max_bytes = uint64_t(bytes);
    options.ca_pem = call.roots.isEmpty() ? nullptr : call.roots.constData();
    options.ca_pem_length = size_t(call.roots.size());
    options.proxy_url_utf8 = access.proxyUrl.isNull() ? nullptr : call.proxy.constData();
    options.transfer_timeout_ms = 2 * 60 * 60 * 1000;
    MaiDownloadResult downloaded{};
    mai_download_file(&options, nullptr, &downloaded);
    if (downloaded.status != MAI_DOWNLOAD_OK || downloaded.bytes != uint64_t(bytes)) {
        QFile::remove(target);
        reply.error = QStringLiteral("会话附件下载失败，请重试");
        return reply;
    }
    reply.path = target;
    return reply;
}

DesktopSessionAttachmentResult DesktopSessionAttachments::cleanupRoom(
    const DesktopVideoCircleAccess& access, const DesktopSessionRoom& room) {
    if (!access.isValid()) return {{}, {}, QStringLiteral("请先登录 MaiChat")};
    Call call(access, room);
    MaiCloudDriveResult raw{};
    mai_cloud_drive_request(&call.options, MAI_SESSION_ATTACHMENT_CLEANUP_ROOM,
                            "{}", nullptr, &raw);
    return parse(&raw);
}
