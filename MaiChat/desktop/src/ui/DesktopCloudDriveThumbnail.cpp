#include "ui/DesktopCloudDriveThumbnail.h"

#include "ui/DesktopRasterImage.h"

#include <QBuffer>
#include <QColorSpace>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageWriter>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QSaveFile>

#include <atomic>
#include <cstring>
#include <utility>

namespace {

std::atomic_uint activeImageSnapshots{0};

}  // namespace

class DesktopCloudDriveImageReservation final {
public:
    DesktopCloudDriveImageReservation() noexcept = default;
    ~DesktopCloudDriveImageReservation() {
        activeImageSnapshots.fetch_sub(1, std::memory_order_release);
    }
};

namespace {

constexpr qint64 kMaximumThumbnailBytes = 2'000'000;
constexpr int kMaximumThumbnailAxis = 320;
constexpr qint64 kMaximumMetadataBytes = 16 * 1024;

std::shared_ptr<const DesktopCloudDriveImageReservation> reserveImageSnapshot() {
    unsigned active = activeImageSnapshots.load(std::memory_order_relaxed);
    while (active < 2) {
        if (!activeImageSnapshots.compare_exchange_weak(active, active + 1, std::memory_order_acq_rel)) continue;
        try {
            return std::make_shared<DesktopCloudDriveImageReservation>();
        } catch (...) {
            activeImageSnapshots.fetch_sub(1, std::memory_order_release);
            throw;
        }
    }
    return {};
}

struct ImageIdentity {
    quint64 sourceSize = 0;
    QByteArray sourceSha;
    quint64 thumbnailSize = 0;
    QByteArray thumbnailSha;
    int width = 0;
    int height = 0;
};

QString jpegPath(const QString& checkpoint) {
    return checkpoint + QStringLiteral(".thumbnail.jpg");
}
QString metadataPath(const QString& checkpoint) {
    return checkpoint + QStringLiteral(".thumbnail.meta");
}

bool validCheckpointPath(const QString& path) {
    const QFileInfo info(path);
    return !path.contains(QChar::Null) && QDir::isAbsolutePath(path) && path.toUtf8().size() <= 4096 &&
           info.fileName().endsWith(QStringLiteral(".maicloud.json")) && !info.isSymLink() &&
           info.absoluteDir().exists();
}

bool readBounded(const QString& path, qint64 maximum, QByteArray* bytes, bool selectedSource = false) {
    const QFileInfo info(path);
    if (!info.isFile() || (!selectedSource && info.isSymLink()) || info.size() < 1 || info.size() > maximum)
        return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    // Allocate for the observed file, not the entire policy ceiling; one
    // extra byte still detects a source that grew after the metadata check.
    *bytes = file.read(info.size() + 1);
    return file.error() == QFileDevice::NoError && file.atEnd() && bytes->size() == info.size() &&
           bytes->size() <= maximum;
}

bool atomicWriteNew(const QString& path, const QByteArray& bytes) {
    if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) return false;
    QSaveFile output(path);
    output.setDirectWriteFallback(false);
    return output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.commit();
}

bool validSha(const QByteArray& value) {
    if (value.size() != 64) return false;
    for (const char character : value)
        if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) return false;
    return true;
}

bool positiveInteger(const QJsonObject& object, const QString& name, quint64* result) {
    const QJsonValue value = object.value(name);
    const qint64 integer = value.toInteger(-1);
    if (!value.isDouble() || integer < 1 || double(integer) != value.toDouble()) return false;
    *result = quint64(integer);
    return true;
}

bool readIdentity(const QString& checkpoint, ImageIdentity* identity) {
    QByteArray bytes;
    if (!readBounded(metadataPath(checkpoint), kMaximumMetadataBytes, &bytes)) return false;
    const QJsonDocument document = QJsonDocument::fromJson(bytes);
    if (!document.isObject()) return false;
    const QJsonObject object = document.object();
    quint64 width = 0;
    quint64 height = 0;
    if (object.size() != 7 || object.value(QStringLiteral("version")) != QJsonValue(1) ||
        !positiveInteger(object, QStringLiteral("source_size"), &identity->sourceSize) ||
        !positiveInteger(object, QStringLiteral("thumbnail_size"), &identity->thumbnailSize) ||
        identity->thumbnailSize > quint64(kMaximumThumbnailBytes) ||
        !positiveInteger(object, QStringLiteral("width"), &width) ||
        !positiveInteger(object, QStringLiteral("height"), &height) || width > kMaximumThumbnailAxis ||
        height > kMaximumThumbnailAxis)
        return false;
    identity->sourceSha = object.value(QStringLiteral("source_sha256")).toString().toLatin1();
    identity->thumbnailSha = object.value(QStringLiteral("thumbnail_sha256")).toString().toLatin1();
    identity->width = int(width);
    identity->height = int(height);
    return validSha(identity->sourceSha) && validSha(identity->thumbnailSha);
}

bool sameThumbnail(const QString& checkpoint, const ImageIdentity& identity, QByteArray* bytes = nullptr) {
    QByteArray actual;
    if (!readBounded(jpegPath(checkpoint), kMaximumThumbnailBytes, &actual) ||
        quint64(actual.size()) != identity.thumbnailSize ||
        QCryptographicHash::hash(actual, QCryptographicHash::Sha256).toHex() != identity.thumbnailSha)
        return false;
    if (bytes) *bytes = std::move(actual);
    return true;
}

bool isBoundedJpeg(const QByteArray& bytes, int width, int height) {
    MaiImageReadOptions options{};
    options.max_encoded_bytes = kMaximumThumbnailBytes;
    options.max_source_pixels = kMaximumThumbnailAxis * kMaximumThumbnailAxis;
    options.max_source_width = options.max_source_height = kMaximumThumbnailAxis;
    const MaiImageInfo info = desktopInspectRasterBytes(bytes, options);
    return info.error_code == 0 && info.format == MAI_IMAGE_FORMAT_JPEG && info.display_width == width &&
           info.display_height == height;
}

QByteArray identityRecord(const ImageIdentity& identity) {
    return QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                     {QStringLiteral("source_size"), qint64(identity.sourceSize)},
                                     {QStringLiteral("source_sha256"), QString::fromLatin1(identity.sourceSha)},
                                     {QStringLiteral("thumbnail_size"), qint64(identity.thumbnailSize)},
                                     {QStringLiteral("thumbnail_sha256"), QString::fromLatin1(identity.thumbnailSha)},
                                     {QStringLiteral("width"), identity.width},
                                     {QStringLiteral("height"), identity.height}})
        .toJson(QJsonDocument::Compact);
}

int readSnapshot(void* context, uint64_t offset, unsigned char* output, size_t requested) {
    const auto* bytes = static_cast<const QByteArray*>(context);
    if (offset > quint64(bytes->size()) || requested > quint64(bytes->size()) - offset) return 0;
    if (requested) std::memcpy(output, bytes->constData() + qsizetype(offset), requested);
    return 1;
}

}  // namespace

DesktopCloudDrivePreparedImage prepareDesktopCloudDriveImage(const QString& source, const QString& checkpoint,
                                                             const QString& displayName) {
    DesktopCloudDrivePreparedImage prepared;
    const QByteArray name = displayName.toUtf8();
    MaiCloudDriveMediaInfo media{};
    if (mai_cloud_drive_media_info(name.constData(), &media) != 1) {
        prepared.error = QStringLiteral("不支持这个云盘文件格式。");
        return prepared;
    }
    prepared.image = QByteArray(media.media_type_utf8) == QByteArray("image");
    const QFileInfo sourceInfo(source);
    if (!validCheckpointPath(checkpoint) || source.contains(QChar::Null) || !QDir::isAbsolutePath(source) ||
        !sourceInfo.isFile() || !sourceInfo.isReadable() || sourceInfo.size() < 1) {
        prepared.error = QStringLiteral("无法读取所选文件或上传任务目录。");
        return prepared;
    }
    if (quint64(sourceInfo.size()) > media.max_size_bytes) {
        const quint64 unit = prepared.image ? 1'000'000 : 1'000'000'000;
        prepared.error = QStringLiteral("%1最多支持 %2 %3。")
                             .arg(prepared.image ? QStringLiteral("图片") : QStringLiteral("视频"),
                                  QString::number(double(media.max_size_bytes) / double(unit), 'g', 10),
                                  prepared.image ? QStringLiteral("MB") : QStringLiteral("GB"));
        return prepared;
    }
    if (!prepared.image) return prepared;
    // Bound the whole snapshot/decode/upload lifetime, not just the decoder
    // call. UI and Agent copies share one permit and never queue source bytes.
    prepared.reservation = reserveImageSnapshot();
    if (!prepared.reservation) {
        prepared.resourceBusy = true;
        prepared.error = QStringLiteral("已有图片正在处理或上传，请稍后重试。");
        return prepared;
    }
    // A source link was explicitly selected by the user. Auxiliary records,
    // unlike that source, must always be ordinary files at the derived path.
    if (!readBounded(source, qint64(media.max_size_bytes), &prepared.encodedOriginal, true)) {
        prepared.error = QStringLiteral("无法读取所选图片或上传任务目录。");
        return prepared;
    }
    // Use one bounded immutable snapshot for decoding and the shared original
    // upload. A source changing after preparation cannot mix new original bytes
    // with an old thumbnail. Videos keep the existing streaming file source.
    const QByteArray sourceSha = QCryptographicHash::hash(prepared.encodedOriginal, QCryptographicHash::Sha256).toHex();
    bool declaredThumbnail = false;
    ImageIdentity checkpointIdentity;
    if (QFileInfo::exists(checkpoint)) {
        QByteArray record;
        if (!readBounded(checkpoint, kMaximumMetadataBytes, &record)) {
            prepared.error = QStringLiteral("无法读取续传任务。");
            return prepared;
        }
        const QJsonDocument document = QJsonDocument::fromJson(record);
        if (!document.isObject()) {
            prepared.error = QStringLiteral("续传任务记录无效。");
            return prepared;
        }
        if (!document.object().contains(QStringLiteral("thumbnail"))) return prepared;
        if (!document.object().value(QStringLiteral("thumbnail")).isObject()) {
            prepared.error = QStringLiteral("续传缩略图记录无效。");
            return prepared;
        }
        const QJsonObject original = document.object();
        const QJsonObject thumbnail = original.value(QStringLiteral("thumbnail")).toObject();
        checkpointIdentity.sourceSha = original.value(QStringLiteral("file_sha256")).toString().toLatin1();
        checkpointIdentity.thumbnailSha = thumbnail.value(QStringLiteral("file_sha256")).toString().toLatin1();
        if (!positiveInteger(original, QStringLiteral("file_size"), &checkpointIdentity.sourceSize) ||
            checkpointIdentity.sourceSize != quint64(prepared.encodedOriginal.size()) ||
            checkpointIdentity.sourceSha != sourceSha || !validSha(checkpointIdentity.sourceSha) ||
            original.value(QStringLiteral("name")).toString() != displayName ||
            original.value(QStringLiteral("content_type")).toString().toUtf8() != media.content_type_utf8 ||
            !positiveInteger(thumbnail, QStringLiteral("size_bytes"), &checkpointIdentity.thumbnailSize) ||
            checkpointIdentity.thumbnailSize > quint64(kMaximumThumbnailBytes) ||
            !validSha(checkpointIdentity.thumbnailSha)) {
            prepared.error = QStringLiteral("原文件或续传缩略图绑定已改变，请确认原文件后重试。");
            return prepared;
        }
        declaredThumbnail = true;
    }
    const QString jpeg = jpegPath(checkpoint);
    const QString metadata = metadataPath(checkpoint);
    const bool hasJpeg = QFileInfo::exists(jpeg) || QFileInfo(jpeg).isSymLink();
    const bool hasMetadata = QFileInfo::exists(metadata) || QFileInfo(metadata).isSymLink();
    if (declaredThumbnail && hasJpeg && !hasMetadata) {
        QByteArray contents;
        MaiImageReadOptions budget{};
        budget.max_encoded_bytes = kMaximumThumbnailBytes;
        budget.max_source_pixels = kMaximumThumbnailAxis * kMaximumThumbnailAxis;
        budget.max_source_width = budget.max_source_height = kMaximumThumbnailAxis;
        if (!sameThumbnail(checkpoint, checkpointIdentity, &contents)) {
            prepared.error = QStringLiteral("续传缩略图内容与任务记录不符。");
            return prepared;
        }
        const MaiImageInfo info = desktopInspectRasterBytes(contents, budget);
        if (info.error_code != 0 || info.format != MAI_IMAGE_FORMAT_JPEG) {
            prepared.error = QStringLiteral("续传缩略图格式或尺寸无效。");
            return prepared;
        }
        checkpointIdentity.width = info.display_width;
        checkpointIdentity.height = info.display_height;
        if (!atomicWriteNew(metadata, identityRecord(checkpointIdentity))) {
            prepared.error = QStringLiteral("无法恢复缩略图的本地续传记录。");
            return prepared;
        }
        prepared.thumbnailPath = jpeg;
        return prepared;
    }
    if (hasJpeg || hasMetadata) {
        ImageIdentity identity;
        QByteArray contents;
        if (!readIdentity(checkpoint, &identity) || identity.sourceSize != quint64(prepared.encodedOriginal.size()) ||
            identity.sourceSha != sourceSha ||
            (declaredThumbnail && (identity.thumbnailSize != checkpointIdentity.thumbnailSize ||
                                   identity.thumbnailSha != checkpointIdentity.thumbnailSha))) {
            prepared.error = QStringLiteral("图片或续传缩略图已改变，请确认原文件后重试。");
            return prepared;
        }
        if (declaredThumbnail && !hasJpeg) return prepared;
        if (!sameThumbnail(checkpoint, identity, &contents) ||
            !isBoundedJpeg(contents, identity.width, identity.height)) {
            prepared.error = QStringLiteral("图片或续传缩略图已改变，请确认原文件后重试。");
            return prepared;
        }
        prepared.thumbnailPath = jpeg;
        return prepared;
    }
    if (declaredThumbnail) {
        // Missing auxiliary bytes are not proof that OSS is missing them.
        // The shared core owns authoritative-ready recovery; if it still needs
        // an unfinished JPEG part it reports the missing source explicitly.
        return prepared;
    }
    MaiImageReadOptions options{};
    options.max_output_width = options.max_output_height = kMaximumThumbnailAxis;
    const DesktopRasterImageResult decoded = desktopReadRasterBytes(prepared.encodedOriginal, options);
    if (decoded.image.isNull() || decoded.info.error_code != 0) {
        prepared.error = QStringLiteral("无法生成图片缩略图，请确认图片内容和尺寸。");
        return prepared;
    }
    QImage pixels = decoded.image;
    if (pixels.colorSpace().isValid()) pixels = pixels.convertedToColorSpace(QColorSpace::SRgb);
    QImage opaque(pixels.size(), QImage::Format_RGB32);
    if (opaque.isNull()) {
        prepared.error = QStringLiteral("无法为图片缩略图分配内存。");
        return prepared;
    }
    opaque.setColorSpace(QColorSpace::SRgb);
    opaque.fill(Qt::white);
    {
        QPainter painter(&opaque);
        painter.drawImage(0, 0, pixels);
    }
    QByteArray contents;
    QBuffer buffer(&contents);
    QImageWriter writer(&buffer, "jpeg");
    writer.setQuality(85);
    if (!buffer.open(QIODevice::WriteOnly) || !writer.write(opaque) || contents.isEmpty() ||
        contents.size() > kMaximumThumbnailBytes || !isBoundedJpeg(contents, opaque.width(), opaque.height())) {
        prepared.error = QStringLiteral("无法编码图片缩略图。");
        return prepared;
    }
    const QByteArray thumbnailSha = QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex();
    const ImageIdentity identity{quint64(prepared.encodedOriginal.size()),
                                 sourceSha,
                                 quint64(contents.size()),
                                 thumbnailSha,
                                 opaque.width(),
                                 opaque.height()};
    const QByteArray record = identityRecord(identity);
    if (!atomicWriteNew(jpeg, contents)) {
        prepared.error = QStringLiteral("无法保存图片缩略图。");
        return prepared;
    }
    if (!atomicWriteNew(metadata, record)) {
        ImageIdentity own;
        own.thumbnailSize = quint64(contents.size());
        own.thumbnailSha = thumbnailSha;
        if (sameThumbnail(checkpoint, own)) QFile::remove(jpeg);
        prepared.error = QStringLiteral("无法保存图片缩略图的续传记录。");
        return prepared;
    }
    prepared.thumbnailPath = jpeg;
    return prepared;
}

bool finishDesktopCloudDriveImage(const QString& checkpoint, bool verifiedCompletionOrDiscard, QString* error) {
    if (error) error->clear();
    if (!validCheckpointPath(checkpoint)) {
        if (error) *error = QStringLiteral("上传任务目录无效。");
        return false;
    }
    if (!verifiedCompletionOrDiscard && QFileInfo::exists(checkpoint)) return true;
    const QString jpeg = jpegPath(checkpoint);
    const QString metadata = metadataPath(checkpoint);
    if (!QFileInfo::exists(jpeg) && !QFileInfo::exists(metadata) && !QFileInfo(jpeg).isSymLink() &&
        !QFileInfo(metadata).isSymLink())
        return true;
    ImageIdentity identity;
    if (!readIdentity(checkpoint, &identity) ||
        ((QFileInfo::exists(jpeg) || QFileInfo(jpeg).isSymLink()) && !sameThumbnail(checkpoint, identity))) {
        if (error) *error = QStringLiteral("缩略图续传记录不匹配，未删除任何文件。");
        return false;
    }
    if ((QFileInfo::exists(jpeg) && !QFile::remove(jpeg)) || !QFile::remove(metadata)) {
        if (error) *error = QStringLiteral("无法清理缩略图续传文件。");
        return false;
    }
    return true;
}

void uploadDesktopCloudDriveMedia(const MaiCloudDriveOptions& supplied, const QString& source,
                                  const QString& checkpoint, const QString& name, const MaiCloudDriveCancel* cancel,
                                  MaiCloudDriveResult* result, QString* localError,
                                  const DesktopCloudDrivePreparedImage* preparedImage) {
    if (!result) return;
    *result = {};
    result->status = MAI_CLOUD_DRIVE_FILE_ERROR;
    if (localError) localError->clear();
    try {
        DesktopCloudDrivePreparedImage prepared =
            preparedImage ? *preparedImage : prepareDesktopCloudDriveImage(source, checkpoint, name);
        if (!prepared.error.isEmpty()) {
            if (localError) *localError = prepared.error;
        } else {
            const QByteArray original = source.toUtf8();
            const QByteArray task = checkpoint.toUtf8();
            const QByteArray display = name.toUtf8();
            const QByteArray thumbnail = prepared.thumbnailPath.toUtf8();
            MaiCloudDriveOptions options = supplied;
            options.cloud_drive_thumbnail_path_utf8 = thumbnail.isEmpty() ? nullptr : thumbnail.constData();
            MaiCloudDriveMediaInfo media{};
            if (mai_cloud_drive_media_info(display.constData(), &media) == 1 &&
                (prepared.thumbnailPath.isEmpty() || prepared.thumbnailPath == jpegPath(checkpoint))) {
                if (prepared.image && prepared.reservation && !prepared.encodedOriginal.isEmpty() &&
                    quint64(prepared.encodedOriginal.size()) <= media.max_size_bytes) {
                    MaiCloudDriveSource snapshot{original.constData(), quint64(prepared.encodedOriginal.size()),
                                                 readSnapshot, &prepared.encodedOriginal};
                    mai_cloud_drive_upload_resumable_source(&options, &snapshot, task.constData(), display.constData(),
                                                            media.content_type_utf8, cancel, nullptr, nullptr, result);
                } else if (!prepared.image) {
                    mai_cloud_drive_upload_resumable(&options, original.constData(), task.constData(),
                                                     display.constData(), media.content_type_utf8, cancel, nullptr,
                                                     nullptr, result);
                }
            } else if (localError) {
                *localError = QStringLiteral("云盘文件或缩略图绑定无效。");
            }
        }
    } catch (...) {
        result->status = MAI_CLOUD_DRIVE_FILE_ERROR;
        if (localError) *localError = QStringLiteral("无法准备图片上传资源。");
    }
    QString cleanupError;
    if (!finishDesktopCloudDriveImage(checkpoint, result->status == MAI_CLOUD_DRIVE_OK, &cleanupError))
        qWarning("[cloud] auxiliary thumbnail cleanup remains pending");
}

quint64 desktopCloudDriveMaximumBytes(bool image) {
    MaiCloudDriveMediaInfo media{};
    return mai_cloud_drive_media_info(image ? "image.jpg" : "video.mp4", &media) == 1 ? media.max_size_bytes : 0;
}

int desktopCloudDriveDownloadTimeoutMs(bool image) {
    // Images may now reach 100 MB; full-video saves can reach 20 GB. These
    // bounded whole-file budgets do not alter the Range source's chunk timeout.
    return image ? 10 * 60 * 1000 : 6 * 60 * 60 * 1000;
}
