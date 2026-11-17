// 文件作用：为 Agent 文件卡生成真实文档首屏；读取、解析及 PDF 解码在后台，Markdown 排版沿用 GUI 线程契约。
// 对外边界：不打开窗口、不修改文件；失败返回空图，由调用方保留原文件卡和完整预览入口。
#pragma once

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>
#include <functional>
#include <memory>

enum class AgentDocumentThumbnailKind { Unsupported, Pdf, Markdown };

struct AgentDocumentThumbnailRequest {
    QString path;
    // 文件卡提供所属工作区；每次后台读前重新检查解析后的路径。普通可信文件入口可留空。
    QString allowedRoot;
    QString mimeType;
    QSize logicalSize{312, 192};
    qreal devicePixelRatio = 1;
    qreal markdownScale = 1;
    QString bodyFamily = QStringLiteral("Inter");
};

struct AgentDocumentThumbnailResult {
    QImage image;
    QString canonicalPath;
    QByteArray contentDigest;
    QByteArray cacheKey;
    QString error;
    bool fromCache = false;
};

class AgentDocumentThumbnailPrepared {
public:
    struct Data;
    std::shared_ptr<const Data> data;
};

AgentDocumentThumbnailKind agentDocumentThumbnailKind(const QString& path, const QString& mimeType);
// 后台普通入口；读取有总字节上限。每次读取内容摘要，避免同大小/时间戳的改写命中旧图。
AgentDocumentThumbnailPrepared prepareAgentDocumentThumbnail(const AgentDocumentThumbnailRequest& request);
// GUI 普通入口；只排版已解析的有界 Markdown 子树，或裁剪后台解码的 PDF 第一页。
AgentDocumentThumbnailResult renderAgentDocumentThumbnail(const AgentDocumentThumbnailPrepared& prepared);

// 每个视图持有一个绑定；新请求或 clear() 撤销旧回调，销毁后不再回调。
// 只在 GUI 线程调用；共享两线程池，每个绑定最多一个在途任务和一个最新待处理请求。
// accessGuard 在每次读取前和回填前检查所属视图/会话，图像只缓存在内存 LRU 中。
class AgentDocumentThumbnailBinding final : public QObject {
public:
    using Completion = std::function<void(const AgentDocumentThumbnailResult&)>;
    using AccessGuard = std::function<bool()>;
    explicit AgentDocumentThumbnailBinding(QObject* parent, Completion completion, AccessGuard accessGuard = {});
    ~AgentDocumentThumbnailBinding() override;
    void request(const AgentDocumentThumbnailRequest& request);
    void clear();

private:
    struct Runtime;
    void startPending();
    std::unique_ptr<Runtime> runtime_;
};
