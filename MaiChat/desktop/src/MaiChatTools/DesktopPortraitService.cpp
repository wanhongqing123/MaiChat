// 文件作用：授权、转正 sRGB 解码、有界 tile 协调、完整新文件发布和当前账号登记。
#include "MaiChatTools/DesktopPortraitService.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <QColorSpace>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QThread>
#include <QUuid>
#include "MaiPortraitBridge.h"
#include "MaiChatTools/DesktopPortraitFaces.h"

namespace {
std::mutex portraitProcessing;
using Skin = std::unique_ptr<MaiPortraitSkinContext, decltype(&maiPortraitFreeSkinContext)>;
using Buffer = std::unique_ptr<void, decltype(&maiPortraitFree)>;
QByteArray encoded(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
void checkCanceled(const DesktopPortraitRequest& request) {
    if (request.isCanceled && request.isCanceled()) throw std::runtime_error("Portrait operation was canceled");
}
QJsonObject takeOutput(char* value) {
    Buffer owner(value, maiPortraitFree);
    if (!value) throw std::runtime_error("Portrait service returned no response");
    const auto document = QJsonDocument::fromJson(QByteArray(value));
    if (!document.isObject()) throw std::runtime_error("Portrait service returned invalid metadata");
    const auto object = document.object();
    if (!object.value(QStringLiteral("ok")).toBool()) throw std::runtime_error(encoded(object).toStdString());
    return object.value(QStringLiteral("output")).toObject();
}
QRect checkedRect(const QJsonArray& values, const QImage& source, int maximumSide) {
    if (values.size() != 4) throw std::runtime_error("Portrait region has invalid coordinates");
    const QRect rectangle(values[0].toInt(-1), values[1].toInt(-1), values[2].toInt(), values[3].toInt());
    if (rectangle.x() < 0 || rectangle.y() < 0 || rectangle.width() <= 0 || rectangle.height() <= 0 ||
        rectangle.width() > maximumSide || rectangle.height() > maximumSide || !source.rect().contains(rectangle))
        throw std::runtime_error("Portrait region exceeds source bounds");
    return rectangle;
}
QRect checkedRect(const QJsonObject& object, const QImage& source) {
    return checkedRect(QJsonArray{object.value(QStringLiteral("x")), object.value(QStringLiteral("y")),
                                  object.value(QStringLiteral("width")), object.value(QStringLiteral("height"))},
                       source, 2048);
}
void applyPatch(QImage& target, const QRect& sample, const QRect& write, const unsigned char* pixels, int channels) {
    if (!pixels || !sample.contains(write) || !target.rect().contains(write))
        throw std::runtime_error("Portrait patch is invalid");
    for (int row = 0; row < write.height(); ++row) {
        const size_t sourceOffset =
            (static_cast<size_t>(write.y() - sample.y() + row) * sample.width() + write.x() - sample.x()) * channels;
        std::memcpy(target.scanLine(write.y() + row) + write.x() * channels, pixels + sourceOffset,
                    static_cast<size_t>(write.width()) * channels);
    }
}
struct NewFiles {
    QStringList paths;
    bool committed = false;
    ~NewFiles() {
        if (!committed)
            for (const auto& path : paths) QFile::remove(path);
    }
};
void savePng(const QImage& image, const QString& temporary, const QString& output,
             const DesktopPortraitRequest& request, NewFiles& files) {
    checkCanceled(request);
    // Qt 读写器析构时才关闭文件；写入和验证各自结束后释放句柄，
    // Windows 才能对完整的同目录临时文件执行不覆盖的原子发布。
    {
        QImageWriter writer(temporary, "png");
        if (!writer.write(image)) throw std::runtime_error("Cannot encode complete portrait PNG");
    }
    // Decoder validation precedes no-replace publication; no partial file becomes a result.
    {
        QImageReader verify(temporary, "png");
        if (!verify.canRead() || verify.size() != image.size() || verify.read().isNull())
            throw std::runtime_error("Encoded portrait PNG failed validation");
    }
    checkCanceled(request);
    takeOutput(maiPortraitPublishOutput(temporary.toUtf8().constData(), output.toUtf8().constData()));
    files.paths.append(output);
}
}  // namespace

DesktopPortraitService::DesktopPortraitService(QString profileDirectory, QString modelDirectory, QString runtimePath)
    : mProfileDirectory(std::move(profileDirectory)),
      mModelDirectory(std::move(modelDirectory)),
      mRuntimePath(std::move(runtimePath)) {}

DesktopPortraitService::~DesktopPortraitService() {
    // ORT 是延迟加载的动态库，其内部静态状态会早于 Bridge 全局引擎析构。
    // 在账号服务仍有确定生命周期时释放会话，避免退出进程才销毁晚存活的 ORT 资源。
    std::lock_guard<std::mutex> lock(portraitProcessing);
    maiPortraitConfigure(nullptr, nullptr, nullptr);
}

QJsonObject DesktopPortraitService::execute(const DesktopPortraitRequest& request) const {
    try {
        if (QCoreApplication::instance() && QThread::currentThread() == QCoreApplication::instance()->thread())
            throw std::runtime_error("Portrait processing must run on a background thread");
        std::unique_lock<std::mutex> lock(portraitProcessing, std::defer_lock);
        while (!lock.try_lock()) {
            checkCanceled(request);
            QThread::msleep(10);
        }
        checkCanceled(request);
        if (mProfileDirectory.isEmpty() || !QFileInfo(mProfileDirectory).isAbsolute())
            throw std::runtime_error("Current account portrait profile is unavailable");
        if (request.operation == QStringLiteral("record_feedback") ||
            request.operation == QStringLiteral("personalization")) {
            auto arguments = request.arguments;
            if (request.operation == QStringLiteral("record_feedback"))
                arguments.insert(QStringLiteral("action"), QStringLiteral("record_feedback"));
            const auto response = takeOutput(maiPortraitPersonalizationRequest(mProfileDirectory.toUtf8().constData(),
                                                                               encoded(arguments).constData()));
            return {{QStringLiteral("ok"), true}, {QStringLiteral("output"), response}};
        }
        if (request.operation != QStringLiteral("beautify") && request.operation != QStringLiteral("reshape_body"))
            throw std::runtime_error("Unknown portrait operation");
        const QByteArray arguments = encoded(request.arguments);
        Buffer invalid(maiPortraitValidateArguments(request.operation.toUtf8().constData(), arguments.constData(), 0),
                       maiPortraitFree);
        if (invalid) throw std::runtime_error(static_cast<const char*>(invalid.get()));
        if (!QFileInfo(request.workspace).isAbsolute()) throw std::runtime_error("Current workspace must be absolute");
        const QString workspace = QFileInfo(request.workspace).canonicalFilePath();
        if (!QFileInfo(workspace).isDir() || !request.resolvePath)
            throw std::runtime_error("Current workspace and path authorization are required");
        QString sourcePath = request.resolvePath(request.arguments.value(QStringLiteral("path")).toString());
        if (sourcePath.isEmpty()) throw std::runtime_error("Source is outside the authorized file domain");
        if (QFileInfo::exists(QDir(mProfileDirectory).filePath(QStringLiteral("portrait.sqlite3")))) {
            Buffer restored(
                maiPortraitOriginalSource(mProfileDirectory.toUtf8().constData(), sourcePath.toUtf8().constData()),
                maiPortraitFree);
            if (!restored) throw std::runtime_error("Cannot resolve the original portrait");
            sourcePath = request.resolvePath(QString::fromUtf8(static_cast<const char*>(restored.get())));
            if (sourcePath.isEmpty()) throw std::runtime_error("Original is outside the authorized file domain");
        }
        const QFileInfo sourceFile(sourcePath);
        if (!sourceFile.isFile() || sourceFile.size() <= 0 || sourceFile.size() > 128ll * 1024 * 1024)
            throw std::runtime_error("Source image is missing or exceeds 128 MB");
        QImageReader reader(sourcePath);
        reader.setAutoTransform(true);
        const QSize size = reader.size();
        if (!size.isValid() || size.width() > 20000 || size.height() > 20000 ||
            static_cast<qint64>(size.width()) * size.height() > 32000000)
            throw std::runtime_error("Source exceeds the 32 megapixel portrait budget");
        QImage source = reader.read();
        if (source.isNull()) throw std::runtime_error("Cannot decode the original image");
        if (!source.colorSpace().isValid()) source.setColorSpace(QColorSpace::SRgb);
        source.convertToColorSpace(QColorSpace::SRgb);
        source = source.convertToFormat(QImage::Format_RGBA8888);
        if (source.isNull()) throw std::runtime_error("Cannot convert the source to sRGB RGBA");
        checkCanceled(request);
        // The authorized source stays immutable; a fingerprint also prevents a changed file being silently committed.
        QCryptographicHash fileHash(QCryptographicHash::Sha256);
        QFile sourceBytes(sourcePath);
        if (!sourceBytes.open(QIODevice::ReadOnly) || !fileHash.addData(&sourceBytes))
            throw std::runtime_error("Cannot fingerprint the source file");
        sourceBytes.close();
        const QByteArray originalFileHash = fileHash.result();
        if (!QDir().mkpath(mProfileDirectory)) throw std::runtime_error("Cannot create the account portrait profile");
        maiPortraitConfigure(mModelDirectory.toUtf8().constData(), mRuntimePath.toUtf8().constData(), nullptr);
        const QByteArray hints =
            encoded({{QStringLiteral("_face_hints"), desktopPortraitFaces(source, request.isCanceled)}});
        QImage output, mask(source.size(), QImage::Format_Grayscale8);
        if (mask.isNull()) throw std::runtime_error("Cannot allocate the portrait diagnostic mask");
        mask.fill(0);
        QJsonObject metadata;
        if (request.operation == QStringLiteral("reshape_body")) {
            // Body geometry has its own bounded full-raster contract; skin beautification below is tiled to 32 MP.
            if (static_cast<qint64>(source.width()) * source.height() > 12000000)
                throw std::runtime_error("Body reshaping supports up to 12 megapixels");
            auto hostArguments = request.arguments;
            hostArguments.insert(QStringLiteral("operation"), request.operation);
            hostArguments.insert(QStringLiteral("_face_hints"),
                                 QJsonDocument::fromJson(hints).object().value(QStringLiteral("_face_hints")));
            auto result =
                maiPortraitProcessRgba(source.constBits(), source.width(), source.height(), source.bytesPerLine(),
                                       encoded(hostArguments).constData(), mProfileDirectory.toUtf8().constData(),
                                       sourcePath.toUtf8().constData());
            Buffer pixels(result.rgba, maiPortraitFree), detail(result.metadata, maiPortraitFree),
                error(result.error, maiPortraitFree);
            if (result.error || !result.rgba || !result.metadata)
                throw std::runtime_error(result.error ? result.error : "Body processing failed");
            if (result.width != source.width() || result.height != source.height())
                throw std::runtime_error("Body output dimensions changed unexpectedly");
            output = QImage(result.rgba, result.width, result.height, QImage::Format_RGBA8888).copy();
            metadata = QJsonDocument::fromJson(QByteArray(result.metadata)).object();
        } else {
            const QImage proxy = source.scaled(512, 512, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            const QImage global = source.scaled(64, 64, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            QCryptographicHash hash(QCryptographicHash::Sha256);
            hash.addData(QByteArray::number(source.width()) + 'x' + QByteArray::number(source.height()) + ':');
            for (int row = 0; row < source.height(); ++row) {
                checkCanceled(request);
                hash.addData(reinterpret_cast<const char*>(source.constScanLine(row)), source.width() * 4);
            }
            const auto analysis = maiPortraitAnalyzeSkinRgba(
                proxy.constBits(), proxy.width(), proxy.height(), proxy.bytesPerLine(), source.width(), source.height(),
                hints.constData(), arguments.constData(), mProfileDirectory.toUtf8().constData(),
                sourcePath.toUtf8().constData(), hash.result().toHex().constData(), global.constBits(),
                global.bytesPerLine());
            Skin skin(analysis.context, maiPortraitFreeSkinContext);
            Buffer planBytes(analysis.metadata, maiPortraitFree), error(analysis.error, maiPortraitFree);
            if (analysis.error || !skin || !analysis.metadata)
                throw std::runtime_error(analysis.error ? analysis.error : "Skin analysis failed");
            const QJsonObject plan = QJsonDocument::fromJson(QByteArray(analysis.metadata))
                                         .object()
                                         .value(QStringLiteral("output"))
                                         .toObject();
            const auto refinement = plan.value(QStringLiteral("skin_refinement_request")).toObject();
            if (!refinement.isEmpty()) {
                checkCanceled(request);
                const QRect region =
                    checkedRect(refinement.value(QStringLiteral("source_rect")).toArray(), source, 20000);
                const QSize target(refinement.value(QStringLiteral("proxy_width")).toInt(),
                                   refinement.value(QStringLiteral("proxy_height")).toInt());
                if (target.width() <= 0 || target.height() <= 0 || target.width() > 1024 || target.height() > 1024)
                    throw std::runtime_error("Skin refinement proxy is invalid");
                QImage refined(target, QImage::Format_RGBA8888);
                {
                    QPainter painter(&refined);
                    painter.setCompositionMode(QPainter::CompositionMode_Source);
                    painter.setRenderHint(QPainter::SmoothPixmapTransform);
                    painter.drawImage(refined.rect(), source, region);
                }
                takeOutput(maiPortraitRefineSkinRegionRgba(skin.get(), refined.constBits(), refined.width(),
                                                           refined.height(), refined.bytesPerLine()));
            }
            checkCanceled(request);
            const auto faceObject = plan.value(QStringLiteral("face_roi")).toObject();
            if (faceObject.isEmpty())
                takeOutput(maiPortraitProtectSkinFaceRegion(skin.get(), nullptr, 0, 0, 0));
            else {
                const QImage face = source.copy(checkedRect(faceObject, source));
                takeOutput(maiPortraitProtectSkinFaceRegion(skin.get(), face.constBits(), face.width(), face.height(),
                                                            face.bytesPerLine()));
            }
            output = source.copy();
            const auto tiles = plan.value(QStringLiteral("tiles")).toArray();
            for (int index = 0; index < tiles.size(); ++index) {
                checkCanceled(request);
                const auto tile = tiles[index].toObject();
                const QRect sample = checkedRect(tile.value(QStringLiteral("sample_rect")).toArray(), source, 1040);
                const QRect write = checkedRect(tile.value(QStringLiteral("write_rect")).toArray(), source, 1024);
                const QImage pixels = source.copy(sample);
                const auto result = maiPortraitProcessSkinTile(skin.get(), index, pixels.constBits(), pixels.width(),
                                                               pixels.height(), pixels.bytesPerLine());
                Buffer rgba(result.rgba, maiPortraitFree), alpha(result.mask, maiPortraitFree),
                    detail(result.metadata, maiPortraitFree), failure(result.error, maiPortraitFree);
                if (result.error || !result.rgba || !result.mask || result.width != sample.width() ||
                    result.height != sample.height())
                    throw std::runtime_error(result.error ? result.error : "Portrait tile failed");
                applyPatch(output, sample, write, result.rgba, 4);
                applyPatch(mask, sample, write, result.mask, 1);
            }
            metadata = takeOutput(maiPortraitFinishSkin(skin.get()));
            const auto geometry = plan.value(QStringLiteral("geometry_parameters")).toObject();
            const bool hasGeometry = geometry.value(QStringLiteral("slim_face")).toDouble() > 0 ||
                                     geometry.value(QStringLiteral("enlarge_eyes")).toDouble() > 0;
            metadata.insert(QStringLiteral("geometry_applied"), hasGeometry);
            if (hasGeometry) {
                checkCanceled(request);
                const QRect region = checkedRect(faceObject, source);
                const QImage pixels = output.copy(region);
                const auto result = maiPortraitProcessRegionRgba(
                    pixels.constBits(), pixels.width(), pixels.height(), pixels.bytesPerLine(),
                    encoded(geometry).constData(), mProfileDirectory.toUtf8().constData(),
                    sourcePath.toUtf8().constData(),
                    encoded(plan.value(QStringLiteral("face_plan")).toObject()).constData());
                Buffer rgba(result.rgba, maiPortraitFree), alpha(result.mask, maiPortraitFree),
                    detail(result.metadata, maiPortraitFree), failure(result.error, maiPortraitFree);
                if (result.error || !result.rgba || result.width != region.width() || result.height != region.height())
                    throw std::runtime_error(result.error ? result.error : "Face geometry failed");
                applyPatch(output, region, region, result.rgba, 4);
                metadata.insert(QStringLiteral("geometry_processing_bbox"),
                                QJsonArray{region.x(), region.y(), region.width(), region.height()});
                metadata.insert(QStringLiteral("metrics_scope"),
                                QStringLiteral("skin_color_and_smoothing_before_optional_face_geometry"));
            }
        }
        checkCanceled(request);
        fileHash.reset();
        if (!sourceBytes.open(QIODevice::ReadOnly) || !fileHash.addData(&sourceBytes) ||
            fileHash.result() != originalFileHash)
            throw std::runtime_error("Source changed during portrait processing; no output was committed");
        sourceBytes.close();
        const QString stem = QStringLiteral("portrait-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString imageName = stem + QStringLiteral(".png"), maskName = stem + QStringLiteral("-mask.png");
        const QString imagePath = QDir(workspace).filePath(imageName), maskPath = QDir(workspace).filePath(maskName);
        const QString imageTemporary = imagePath + QStringLiteral(".partial"),
                      maskTemporary = maskPath + QStringLiteral(".partial");
        if (QFileInfo::exists(imageTemporary) || QFileInfo::exists(maskTemporary) || QFileInfo::exists(imagePath) ||
            QFileInfo::exists(maskPath))
            throw std::runtime_error("Portrait output name is already in use");
        NewFiles files{{imageTemporary, maskTemporary}};
        output.setColorSpace(QColorSpace::SRgb);
        savePng(output, imageTemporary, imagePath, request, files);
        savePng(mask, maskTemporary, maskPath, request, files);
        checkCanceled(request);
        auto result = takeOutput(maiPortraitCommitResult(
            mProfileDirectory.toUtf8().constData(), encoded(metadata).constData(), imagePath.toUtf8().constData()));
        result.insert(QStringLiteral("path"), imageName);
        result.insert(QStringLiteral("mask_path"), maskName);
        result.insert(QStringLiteral("width"), output.width());
        result.insert(QStringLiteral("height"), output.height());
        result.insert(QStringLiteral("orientation_normalized"), true);
        result.insert(QStringLiteral("color_space"), QStringLiteral("sRGB"));
        files.committed = true;
        return {{QStringLiteral("ok"), true}, {QStringLiteral("output"), result}};
    } catch (const std::exception& error) {
        return {
            {QStringLiteral("ok"), false},
            {QStringLiteral("errorCode"),
             request.isCanceled && request.isCanceled() ? QStringLiteral("canceled") : QStringLiteral("invalid_input")},
            {QStringLiteral("error"), QString::fromUtf8(error.what())}};
    }
}
