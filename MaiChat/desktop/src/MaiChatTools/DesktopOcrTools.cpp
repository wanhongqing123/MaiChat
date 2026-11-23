// The tool adapts path authorization and JSON; the ordinary service owns local OCR.
#include "MaiChatTools/DesktopOcrTools.h"

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiOcr.h"
#include "MaiTool.h"
#include "ui/DesktopRasterImage.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <utility>

namespace {

constexpr std::uint64_t kMaxImageBytes = 50u * 1024 * 1024;
constexpr std::uint64_t kMaxImagePixels = 12'000'000;
constexpr int kMaxImageAxis = 16'384;
constexpr std::size_t kMaxArgumentsBytes = 64u * 1024;

bool isCanceled(const DesktopOcrReadOptions& options) {
    return options.isCanceled && options.isCanceled();
}

MaiError canceledError() {
    return {MaiErrorCode::Canceled, "image OCR was canceled"};
}

int pollCancel(void* opaque) {
    return isCanceled(*static_cast<const DesktopOcrReadOptions*>(opaque)) ? 1 : 0;
}

QString runtimePath() {
    const QDir executable(QCoreApplication::applicationDirPath());
#if defined(Q_OS_MAC)
    return executable.filePath(QStringLiteral("../Frameworks/libonnxruntime.1.26.0.dylib"));
#elif defined(Q_OS_WIN)
    return executable.filePath(QStringLiteral("onnxruntime.dll"));
#else
    return {};
#endif
}

MaiError recognitionError(MaiOcrStatus status, const char* error) {
    if (status == MAI_OCR_CANCELED) return canceledError();
    const MaiErrorCode code = status == MAI_OCR_INVALID_INPUT || status == MAI_OCR_LIMIT_EXCEEDED
        ? MaiErrorCode::InvalidInput
        : status == MAI_OCR_NOT_CONFIGURED ? MaiErrorCode::NotConfigured : MaiErrorCode::Internal;
    return {code, error[0] != '\0' ? error : "local image OCR failed"};
}

struct NativeResult {
    MaiOcrResult value{};
    ~NativeResult() { maiOcrResultFree(&value); }
};

QJsonObject encodeResult(const DesktopOcrResult& result, const QString& sourcePath) {
    QJsonArray blocks;
    for (const DesktopOcrBlock& block : result.blocks) {
        QJsonArray quad;
        float left = block.quad.front().x;
        float top = block.quad.front().y;
        float right = left;
        float bottom = top;
        for (const DesktopOcrPoint& point : block.quad) {
            quad.append(QJsonObject{{QStringLiteral("x"), point.x}, {QStringLiteral("y"), point.y}});
            left = std::min(left, point.x);
            top = std::min(top, point.y);
            right = std::max(right, point.x);
            bottom = std::max(bottom, point.y);
        }
        blocks.append(QJsonObject{
            {QStringLiteral("text"), QString::fromUtf8(block.text.data(), block.text.size())},
            {QStringLiteral("confidence"), block.confidence},
            {QStringLiteral("quad"), quad},
            {QStringLiteral("bounds"), QJsonObject{
                {QStringLiteral("x"), left}, {QStringLiteral("y"), top},
                {QStringLiteral("width"), right - left}, {QStringLiteral("height"), bottom - top}}}});
    }
    return {{QStringLiteral("image_width"), result.imageWidth},
            {QStringLiteral("image_height"), result.imageHeight},
            {QStringLiteral("partial"), result.partial},
            {QStringLiteral("skipped_lines"), static_cast<qint64>(result.skippedLines)},
            {QStringLiteral("segmented_lines"), static_cast<qint64>(result.segmentedLines)},
            {QStringLiteral("recognition_height"), MAI_OCR_RECOGNITION_HEIGHT},
            {QStringLiteral("max_recognition_width"), MAI_OCR_MAX_RECOGNITION_WIDTH},
            {QStringLiteral("source_path"), sourcePath},
            {QStringLiteral("space"), QStringLiteral("image")},
            {QStringLiteral("space_id"), sourcePath},
            {QStringLiteral("coordinate_origin"), QStringLiteral("top_left")},
            {QStringLiteral("unit"), QStringLiteral("pixel")},
            {QStringLiteral("orientation_applied"), true},
            {QStringLiteral("blocks"), blocks}};
}

class DesktopOcrTool final : public MaiTool {
public:
    explicit DesktopOcrTool(std::shared_ptr<DesktopOcrService> service) : mService(std::move(service)) {}

    std::string name() const override { return "ocr_image"; }

    std::string description() const override {
        return "Read text from one accessible local raster using local OCR. Returns image-space "
               "quadrilaterals in the original decoded raster pixels after EXIF orientation. "
               "Wide text can return separate fragment blocks; partial/skipped_lines report incomplete "
               "wide-line recovery even when no blocks remain. Recognition limits are normalized dimensions. "
               "Confidence and boxes do not establish clickable controls; inspect repeated labels "
               "before choosing a target. This tool does not create a computer snapshot. For "
               "screenshot input use the same actual snapshot and its frame mapping separately; "
               "never infer a snapshot or mapping from a file path.";
    }

    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"path":{"type":"string","minLength":1,)"
               R"("description":"One accessible local image path, absolute or relative to the workspace"}},)"
               R"("required":["path"],"additionalProperties":false})";
    }

    bool requiresApproval(const std::string&) const override { return false; }

    MaiToolResult execute(const std::string& argumentsJson, const MaiToolContext& context) override {
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "image OCR was canceled");
        if (argumentsJson.size() > kMaxArgumentsBytes)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "a single image path string is required");
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(argumentsJson), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
            document.object().size() != 1 || !document.object().value(QStringLiteral("path")).isString()) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "a single image path string is required");
        }
        const QString path = document.object().value(QStringLiteral("path")).toString();
        if (path.isEmpty() || path.contains(QChar::Null))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "image path must be nonempty and contain no NUL");
        const std::string resolved = context.resolvePath(path.toUtf8().toStdString());
        if (resolved.empty())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "image path is outside the accessible area");
        DesktopOcrReadOptions options;
        options.isCanceled = [&context] { return context.isCanceled(); };
        const auto result = mService->recognizeImageFile(resolved, options);
        if (!result.isOk())
            return MaiToolResult::failure(result.error().code(), result.error().message());
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "image OCR was canceled");
        return MaiToolResult::success(QJsonDocument(encodeResult(result.value(), path))
            .toJson(QJsonDocument::Compact).toStdString());
    }

private:
    std::shared_ptr<DesktopOcrService> mService;
};

}  // namespace

struct DesktopOcrService::State {
    std::timed_mutex mutex;
    bool initialized = false;
    MaiOcrEngine* engine = nullptr;
    MaiError initializationError;

    ~State() {
        std::lock_guard<std::timed_mutex> lock(mutex);
        maiOcrDestroy(engine);
    }

    void initialize() {
        if (initialized) return;
        initialized = true;
        QFile detection(QStringLiteral(":/maichat/models/ocr/det.onnx"));
        QFile recognition(QStringLiteral(":/maichat/models/ocr/rec.onnx"));
        QFile dictionary(QStringLiteral(":/maichat/models/ocr/dictionary.txt"));
        if (!detection.open(QIODevice::ReadOnly) || !recognition.open(QIODevice::ReadOnly) ||
            !dictionary.open(QIODevice::ReadOnly)) {
            initializationError = {MaiErrorCode::NotConfigured, "bundled local OCR models or dictionary are unavailable"};
            return;
        }
        const QByteArray detectionBytes = detection.readAll();
        const QByteArray recognitionBytes = recognition.readAll();
        const QByteArray dictionaryBytes = dictionary.readAll();
        const QByteArray runtime = runtimePath().toUtf8();
        if (runtime.isEmpty() || !QFile::exists(QString::fromUtf8(runtime))) {
            initializationError = {MaiErrorCode::NotConfigured, "bundled ONNX Runtime for local OCR is unavailable"};
            return;
        }
        MaiOcrModelConfig config{};
        config.runtime_path = runtime.constData();
        config.detection_model = reinterpret_cast<const unsigned char*>(detectionBytes.constData());
        config.detection_model_size = static_cast<std::size_t>(detectionBytes.size());
        config.recognition_model = reinterpret_cast<const unsigned char*>(recognitionBytes.constData());
        config.recognition_model_size = static_cast<std::size_t>(recognitionBytes.size());
        config.dictionary_utf8 = dictionaryBytes.constData();
        config.dictionary_size = static_cast<std::size_t>(dictionaryBytes.size());
        char error[1024]{};
        const MaiOcrStatus status = maiOcrCreate(&config, &engine, error, sizeof(error));
        if (status != MAI_OCR_OK || engine == nullptr)
            initializationError = {MaiErrorCode::NotConfigured,
                                   error[0] != '\0' ? error : "local OCR engine could not be initialized"};
    }
};

DesktopOcrService::DesktopOcrService() : mState(std::make_unique<State>()) {}
DesktopOcrService::~DesktopOcrService() = default;

MaiResult<DesktopOcrResult> DesktopOcrService::recognizeImageFile(
    const std::string& absolutePath, const DesktopOcrReadOptions& options) {
    if (isCanceled(options)) return canceledError();
    const QString path = QString::fromUtf8(absolutePath.data(), absolutePath.size());
    if (path.isEmpty() || path.contains(QChar::Null) || !QDir::isAbsolutePath(path) ||
        path.startsWith(QStringLiteral(":/")) || path.startsWith(QStringLiteral("//")) ||
        path.startsWith(QStringLiteral("\\\\"))) {
        return {MaiErrorCode::InvalidInput, "OCR requires an authorized absolute local image path"};
    }
    std::uint64_t fileBytes = 0;
    if (!MaiFileSystem::fileSize(MaiFilePath::fromUtf8(absolutePath), fileBytes) ||
        fileBytes == 0 || fileBytes > kMaxImageBytes) {
        return {MaiErrorCode::InvalidInput, "image file is missing or exceeds 50 MiB"};
    }
    // Serialize decoding as well as inference: queued sessions must not each
    // retain a decoded raster plus two full-size compositing/conversion images.
    // Waiting callers still poll cancellation without allocating image pixels.
    // No call context or cancellation callback survives this synchronous scope.
    std::unique_lock<std::timed_mutex> lock(mState->mutex, std::defer_lock);
    while (!lock.try_lock_for(std::chrono::milliseconds(25)))
        if (isCanceled(options)) return canceledError();
    if (isCanceled(options)) return canceledError();

    MaiImageReadOptions readOptions{};
    readOptions.max_encoded_bytes = kMaxImageBytes;
    readOptions.max_source_pixels = kMaxImagePixels;
    readOptions.max_source_width = kMaxImageAxis;
    readOptions.max_source_height = kMaxImageAxis;
    readOptions.is_canceled = pollCancel;
    readOptions.context = const_cast<DesktopOcrReadOptions*>(&options);
    const auto raster = desktopReadRasterFile(path, readOptions);
    if (isCanceled(options) || raster.errorCode == -ECANCELED) return canceledError();
    if (raster.errorCode == -EFBIG)
        return {MaiErrorCode::InvalidInput, "image exceeds the 12 megapixel or 16384 pixel axis limit"};
    if (raster.image.isNull() || raster.errorCode != 0)
        return {MaiErrorCode::InvalidInput, "image format could not be decoded as a complete local raster"};

    // The core deliberately ignores alpha. Flatten onto white so transparent
    // screenshots and documents retain their intended visible text contrast.
    QImage opaque(raster.image.size(), QImage::Format_RGB32);
    if (opaque.isNull()) return {MaiErrorCode::Internal, "image OCR pixel allocation failed"};
    opaque.fill(Qt::white);
    {
        QPainter painter(&opaque);
        painter.drawImage(QPoint(0, 0), raster.image);
    }
    const QImage rgba = opaque.convertToFormat(QImage::Format_RGBA8888);
    if (rgba.isNull()) return {MaiErrorCode::Internal, "image OCR pixel conversion failed"};
    if (isCanceled(options)) return canceledError();

    mState->initialize();
    if (isCanceled(options)) return canceledError();
    if (mState->initializationError.hasError()) return mState->initializationError;

    MaiOcrImage image{rgba.constBits(), static_cast<std::size_t>(rgba.sizeInBytes()),
                      rgba.width(), rgba.height(), static_cast<int>(rgba.bytesPerLine())};
    MaiOcrOptions ocrOptions{pollCancel, const_cast<DesktopOcrReadOptions*>(&options)};
    NativeResult native;
    char error[1024]{};
    const MaiOcrStatus status = maiOcrRecognize(mState->engine, &image, &ocrOptions, &native.value, error, sizeof(error));
    if (isCanceled(options)) return canceledError();
    if (status != MAI_OCR_OK) return recognitionError(status, error);
    if (native.value.width != image.width || native.value.height != image.height ||
        native.value.count > 256 || (native.value.count != 0 && native.value.regions == nullptr)) {
        return {MaiErrorCode::Internal, "local OCR returned inconsistent raster geometry"};
    }
    DesktopOcrResult result;
    result.imageWidth = native.value.width;
    result.imageHeight = native.value.height;
    result.partial = native.value.partial != 0;
    result.skippedLines = native.value.skipped_lines;
    result.segmentedLines = native.value.segmented_lines;
    result.blocks.reserve(native.value.count);
    for (std::size_t index = 0; index < native.value.count; ++index) {
        const MaiOcrRegion& region = native.value.regions[index];
        if (region.text == nullptr) return {MaiErrorCode::Internal, "local OCR returned an invalid text block"};
        DesktopOcrBlock block;
        block.text = region.text;
        block.confidence = region.confidence;
        for (std::size_t point = 0; point < block.quad.size(); ++point)
            block.quad[point] = {region.quad[point].x, region.quad[point].y};
        result.blocks.push_back(std::move(block));
    }
    if (isCanceled(options)) return canceledError();
    return result;
}

void registerDesktopOcrTools(MaiToolRegistry& registry, std::shared_ptr<DesktopOcrService> service) {
    if (!service) service = std::make_shared<DesktopOcrService>();
    registry.add(std::make_unique<DesktopOcrTool>(std::move(service)));
}
