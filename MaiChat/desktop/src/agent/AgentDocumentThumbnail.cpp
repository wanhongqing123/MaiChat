// 文件作用：Agent 文档文件卡首屏的有界读取、普通渲染入口和可撤销异步视图绑定。
#include "agent/AgentDocumentThumbnail.h"

#include "markdown/MarkdownDocument.h"
#include "markdown/MarkdownLayout.h"
#include "markdown/MarkdownTheme.h"
#include "ui/PdfPreviewDialog.h"

#include <QCache>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPointer>
#include <QStringDecoder>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrent>
#include <atomic>
#include <cmath>
#include <optional>

#ifdef Q_OS_MACOS
// 无窗口的 CoreGraphics 第一页适配；与完整 PDF 预览的 PDFKit 窗口互不干扰。
QImage renderAgentMacPdfFirstPage(const QByteArray& bytes, QString* error);
#endif

namespace {
constexpr qint64 kPdfBytes = 20LL * 1024 * 1024;
constexpr qint64 kMarkdownBytes = 512LL * 1024;
constexpr int kPreviewBlocks = 64;
constexpr int kPreviewCharacters = 8192;

struct ImageCache {
    QMutex mutex;
    QCache<QByteArray, QImage> images{24 * 1024};  // KiB；只保留最终缩略图，不留整页 PDF。
};
ImageCache& imageCache() {
    static ImageCache cache;
    return cache;
}
QThreadPool* thumbnailPool() {
    static QThreadPool pool;
    static const bool configured = [] {
        pool.setMaxThreadCount(2);
        pool.setExpiryTimeout(10000);
        return true;
    }();
    Q_UNUSED(configured);
    return &pool;
}
bool onGuiThread() {
    return QCoreApplication::instance() && QThread::currentThread() == QCoreApplication::instance()->thread();
}
bool validRequest(const AgentDocumentThumbnailRequest& request) {
    return !request.path.isEmpty() && request.logicalSize.width() >= 64 && request.logicalSize.width() <= 1024 &&
           request.logicalSize.height() >= 32 && request.logicalSize.height() <= 768 &&
           std::isfinite(request.devicePixelRatio) && request.devicePixelRatio >= 1 && request.devicePixelRatio <= 4 &&
           std::isfinite(request.markdownScale) && request.markdownScale >= 0.5 && request.markdownScale <= 3;
}
QByteArray readBounded(const QString& path, qint64 limit, QString* error) {
    QFile file(path);
    if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > limit) {
        *error = QStringLiteral("无法生成此文档的缩略图。");
        return {};
    }
    const QByteArray bytes = file.read(limit + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > limit || !file.atEnd()) {
        *error = QStringLiteral("文档读取不完整或超过缩略图大小限制。");
        return {};
    }
    return bytes;
}
QByteArray imageKey(const AgentDocumentThumbnailRequest& request, AgentDocumentThumbnailKind kind,
                    const QString& canonicalPath, const QByteArray& digest) {
    QByteArray encoded;
    QDataStream stream(&encoded, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << quint32(1) << canonicalPath << digest << qint32(kind) << request.logicalSize << request.devicePixelRatio
           << request.markdownScale << request.bodyFamily;
    return QCryptographicHash::hash(encoded, QCryptographicHash::Sha256);
}
bool sameTable(const MarkdownBlock& full, const MarkdownBlock& prefix) {
    if (full.rows.size() != prefix.rows.size()) return false;
    for (int row = 0; row < full.rows.size(); ++row) {
        if (full.rows[row].size() != prefix.rows[row].size()) return false;
        for (int cell = 0; cell < full.rows[row].size(); ++cell) {
            const auto& first = full.rows[row][cell];
            const auto& second = prefix.rows[row][cell];
            if (first.size() != second.size()) return false;
            for (int span = 0; span < first.size(); ++span)
                if (first[span].text != second[span].text || first[span].styles != second[span].styles ||
                    first[span].href != second[span].href)
                    return false;
        }
    }
    return true;
}
}  // namespace

struct AgentDocumentThumbnailPrepared::Data {
    AgentDocumentThumbnailRequest request;
    AgentDocumentThumbnailKind kind = AgentDocumentThumbnailKind::Unsupported;
    AgentDocumentThumbnailResult result;
    MarkdownDocument markdown;
    QImage pdfPage;
};

AgentDocumentThumbnailKind agentDocumentThumbnailKind(const QString& path, const QString& mimeType) {
    const QString mime = mimeType.section(QLatin1Char(';'), 0, 0).trimmed().toLower();
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (mime == QStringLiteral("application/pdf") || suffix == QStringLiteral("pdf"))
        return AgentDocumentThumbnailKind::Pdf;
    if (mime == QStringLiteral("text/markdown") || mime == QStringLiteral("text/x-markdown") ||
        suffix == QStringLiteral("md") || suffix == QStringLiteral("markdown"))
        return AgentDocumentThumbnailKind::Markdown;
    return AgentDocumentThumbnailKind::Unsupported;
}

AgentDocumentThumbnailPrepared prepareAgentDocumentThumbnail(const AgentDocumentThumbnailRequest& request) {
    auto data = std::make_shared<AgentDocumentThumbnailPrepared::Data>();
    AgentDocumentThumbnailPrepared prepared{data};
    data->request = request;
    auto& result = data->result;
    data->kind = agentDocumentThumbnailKind(request.path, request.mimeType);
    if (!validRequest(request) || data->kind == AgentDocumentThumbnailKind::Unsupported) {
        result.error = QStringLiteral("此文件不提供文档缩略图。");
        return prepared;
    }
    result.canonicalPath = QFileInfo(request.path).canonicalFilePath();
    if (!request.allowedRoot.isEmpty()) {
        const QString root = QFileInfo(request.allowedRoot).canonicalFilePath();
        const QString relative = QDir(root).relativeFilePath(result.canonicalPath);
        if (root.isEmpty() || result.canonicalPath.isEmpty() || QDir::isAbsolutePath(relative) ||
            relative == QStringLiteral("..") || relative.startsWith(QStringLiteral("../"))) {
            result.error = QStringLiteral("文件已离开所属工作区。");
            return prepared;
        }
    }
    const auto limit = data->kind == AgentDocumentThumbnailKind::Pdf ? kPdfBytes : kMarkdownBytes;
    const QByteArray bytes = readBounded(result.canonicalPath, limit, &result.error);
    if (!result.error.isEmpty()) return prepared;
    result.contentDigest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    result.cacheKey = imageKey(request, data->kind, result.canonicalPath, result.contentDigest);
    {
        auto& cache = imageCache();
        QMutexLocker lock(&cache.mutex);
        if (const QImage* cached = cache.images.object(result.cacheKey)) {
            result.image = *cached;
            result.fromCache = true;
            return prepared;
        }
    }
    if (data->kind == AgentDocumentThumbnailKind::Markdown) {
        // bytes is the complete file; incomplete EOF sequences are errors, not a pending stream chunk.
        QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
        const QString text = decoder(bytes);
        if (decoder.hasError()) {
            result.error = QStringLiteral("Markdown 文档不是有效的 UTF-8 文本。");
            return prepared;
        }
        // 先完整解析后取树前缀：后置链接定义、围栏、引用的语义与完整预览一致。
        const auto full = MarkdownDocument::parse(text);
        data->markdown = full.prefixForPreview(kPreviewBlocks, kPreviewCharacters);
        // 极深引用并无可读首屏；保持普通卡，避免 GUI 排版遍历异常深的结构。
        for (int i = 0; i < data->markdown.blocks().size(); ++i) {
            const auto& block = data->markdown.blocks()[i];
            // 表格列宽由整表各行共同决定。部分表格不能冒充真正首屏；保留可点文件卡。
            if ((block.kind == MarkdownBlockKind::Table && !sameTable(full.blocks()[i], block)) ||
                block.quoteIds.size() > 16 || (!block.rows.isEmpty() && block.rows.first().size() > 32)) {
                result.error = QStringLiteral("此文档结构不适合生成缩略图。");
                break;
            }
        }
    } else {
#ifdef Q_OS_WIN
        const auto page = renderWindowsPdfPageBytes(bytes, 0);
        data->pdfPage = page.image;
        result.error = page.error;
#elif defined(Q_OS_MACOS)
        data->pdfPage = renderAgentMacPdfFirstPage(bytes, &result.error);
#else
        result.error = QStringLiteral("当前平台不提供 PDF 缩略图。");
#endif
        if (data->pdfPage.isNull() && result.error.isEmpty()) result.error = QStringLiteral("PDF 页面无法解码。");
    }
    return prepared;
}

AgentDocumentThumbnailResult renderAgentDocumentThumbnail(const AgentDocumentThumbnailPrepared& prepared) {
    if (!prepared.data) return {};
    const auto& data = *prepared.data;
    AgentDocumentThumbnailResult result = data.result;
    if (!result.error.isEmpty() || !result.image.isNull()) return result;
    if (!onGuiThread()) {
        result.error = QStringLiteral("文档缩略图排版必须在界面线程完成。");
        return result;
    }
    const auto& request = data.request;
    const QSize pixels(qRound(request.logicalSize.width() * request.devicePixelRatio),
                       qRound(request.logicalSize.height() * request.devicePixelRatio));
    QImage image(pixels, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        result.error = QStringLiteral("无法分配缩略图图像。");
        return result;
    }
    image.setDevicePixelRatio(request.devicePixelRatio);
    image.fill(Qt::white);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::TextAntialiasing);
        if (data.kind == AgentDocumentThumbnailKind::Markdown) {
            // 缩小的是正常阅读视口而非整篇文档；底部自然裁切，长文不会缩成不可辨的图标。
            constexpr qreal readingScale = 0.5;
            constexpr qreal margin = 24;
            painter.scale(readingScale, readingScale);
            const QRectF viewport(QPointF(), QSizeF(request.logicalSize) / readingScale);
            MarkdownLayout layout;
            layout.layout(data.markdown, MarkdownTheme::standard(request.markdownScale, request.bodyFamily),
                          viewport.width() - margin * 2);
            layout.paint(&painter, QPointF(margin, margin), viewport);
        } else if (!data.pdfPage.isNull()) {
            const qreal height = qreal(data.pdfPage.height()) * request.logicalSize.width() / data.pdfPage.width();
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawImage(QRectF(0, 0, request.logicalSize.width(), height), data.pdfPage);
        }
    }
    result.image = image;
    auto& cache = imageCache();
    QMutexLocker lock(&cache.mutex);
    cache.images.insert(result.cacheKey, new QImage(image), qMax(1, int((image.sizeInBytes() + 1023) / 1024)));
    return result;
}

struct AgentDocumentThumbnailBinding::Runtime {
    Completion completion;
    AccessGuard accessGuard;
    AgentDocumentThumbnailRequest lastRequest;
    std::optional<AgentDocumentThumbnailRequest> pending;
    bool hasRequest = false;
    bool inFlight = false;
    quint64 activeTicket = 0;
    QFileSystemWatcher* watcher = nullptr;
    QFutureWatcher<AgentDocumentThumbnailPrepared>* future = nullptr;
    std::shared_ptr<std::atomic<quint64>> generation = std::make_shared<std::atomic<quint64>>(0);
};

AgentDocumentThumbnailBinding::AgentDocumentThumbnailBinding(QObject* parent, Completion completion,
                                                             AccessGuard accessGuard)
    : QObject(parent), runtime_(std::make_unique<Runtime>()) {
    runtime_->completion = std::move(completion);
    runtime_->accessGuard = std::move(accessGuard);
    runtime_->watcher = new QFileSystemWatcher(this);
    runtime_->future = new QFutureWatcher<AgentDocumentThumbnailPrepared>(this);
    connect(runtime_->future, &QFutureWatcher<AgentDocumentThumbnailPrepared>::finished, this, [this] {
        AgentDocumentThumbnailPrepared prepared;
        bool preparationFailed = false;
        try {
            auto future = runtime_->future->future();
            // Consume even stale generations before any reentrant completion:
            // the watcher must not retain a full PDF page or parsed source tree.
            // A canceled/exceptional future can finish without a result.
            if (future.isValid() && future.isResultReadyAt(0))
                prepared = future.takeResult();
            else
                preparationFailed = true;
        } catch (...) {
            preparationFailed = true;
        }
        runtime_->inFlight = false;
        if (runtime_->generation->load() == runtime_->activeTicket) {
            AgentDocumentThumbnailResult result;
            if (runtime_->accessGuard && !runtime_->accessGuard()) {
                clear();
                result.error = QStringLiteral("文件已移动或不可用。");
            } else if (preparationFailed) {
                result.error = QStringLiteral("文档缩略图生成未完成。");
            } else {
                result = renderAgentDocumentThumbnail(prepared);
            }
            // 回调可删除文件卡或重新绑定；返回后只在对象仍存活时启动最新待处理请求。
            const QPointer<AgentDocumentThumbnailBinding> alive(this);
            if (runtime_->completion) runtime_->completion(result);
            if (!alive) return;
        }
        startPending();
    });
    const auto reload = [this] {
        if (runtime_->hasRequest) request(runtime_->lastRequest);
    };
    connect(runtime_->watcher, &QFileSystemWatcher::fileChanged, this, reload);
    connect(runtime_->watcher, &QFileSystemWatcher::directoryChanged, this, reload);
}
AgentDocumentThumbnailBinding::~AgentDocumentThumbnailBinding() {
    ++*runtime_->generation;
}
void AgentDocumentThumbnailBinding::clear() {
    Q_ASSERT(onGuiThread());
    ++*runtime_->generation;
    runtime_->hasRequest = false;
    runtime_->pending.reset();
    const auto paths = runtime_->watcher->files() + runtime_->watcher->directories();
    if (!paths.isEmpty()) runtime_->watcher->removePaths(paths);
}
void AgentDocumentThumbnailBinding::request(const AgentDocumentThumbnailRequest& requested) {
    Q_ASSERT(onGuiThread());
    // requested 可能引用 lastRequest，先复制再 clear，保证重新绑定时没有悬空/旧代际。
    const auto request = requested;
    clear();
    if (runtime_->accessGuard && !runtime_->accessGuard()) {
        AgentDocumentThumbnailResult result;
        result.error = QStringLiteral("文件已移动或不可用。");
        if (runtime_->completion) runtime_->completion(result);
        return;
    }
    runtime_->lastRequest = request;
    runtime_->hasRequest = true;
    const QFileInfo file(request.path);
    if (file.exists()) runtime_->watcher->addPath(file.absoluteFilePath());
    if (file.dir().exists()) runtime_->watcher->addPath(file.absolutePath());
    runtime_->pending = request;
    startPending();
}
void AgentDocumentThumbnailBinding::startPending() {
    if (runtime_->inFlight || !runtime_->pending) return;
    if (runtime_->accessGuard && !runtime_->accessGuard()) {
        clear();
        AgentDocumentThumbnailResult result;
        result.error = QStringLiteral("文件已移动或不可用。");
        if (runtime_->completion) runtime_->completion(result);
        return;
    }
    const auto request = *runtime_->pending;
    runtime_->pending.reset();
    runtime_->inFlight = true;
    const auto generation = runtime_->generation;
    const auto ticket = generation->load();
    runtime_->activeTicket = ticket;
    runtime_->future->setFuture(QtConcurrent::run(thumbnailPool(), [request, generation, ticket] {
        if (generation->load() != ticket) return AgentDocumentThumbnailPrepared{};
        return prepareAgentDocumentThumbnail(request);
    }));
}
