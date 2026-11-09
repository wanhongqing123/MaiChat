// 文件作用：桌面端 Agent 工具；DesktopPdfRenderer 负责内容渲染和资源生命周期，本文件给出其实现。
// 实现边界：与 DesktopPdfRenderer 的公开声明配套，调用方通过相应接口进入。
#include "MaiChatTools/DesktopPdfRenderer.h"

#include <QFile>
#include <QFont>
#include <QMarginsF>
#include <QPdfWriter>
#include <QPageSize>
#include <QTextDocument>
#include <QUrl>
#include <QVariant>

#include <string>

#include "ui/DesktopDocumentImage.h"

namespace {

class SelfContainedPdfDocument final : public QTextDocument {
public:
    explicit SelfContainedPdfDocument(const std::atomic<bool>* cancel = nullptr) : cancel_(cancel) {}

protected:
    QVariant loadResource(int type, const QUrl& name) override {
        QImage image;
        // PDF remains data-only. A typed null also blocks global providers and
        // QTextImageHandler's attempt to load a rejected file name itself.
        if (type == ImageResource && name.scheme() == QStringLiteral("data") &&
            (!cancel_ || !cancel_->load(std::memory_order_relaxed))) {
            MaiImageReadOptions options{};
            if (cancel_) {
                options.context = const_cast<std::atomic<bool>*>(cancel_);
                options.is_canceled = [](void* context) {
                    return static_cast<std::atomic<bool>*>(context)->load(std::memory_order_relaxed) ? 1 : 0;
                };
            }
            image = desktopDocumentDataImage(name, options);
        }
        const QVariant result = QVariant::fromValue(image);
        addResource(type, name, result);
        return result;
    }

private:
    const std::atomic<bool>* cancel_ = nullptr;
};

}  // namespace

MaiToolResult renderDesktopPdf(const std::string& htmlUtf8, const std::string& absoluteOutputPath,
                               const std::atomic<bool>* cancel) {
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed))
        return MaiToolResult::failure(MaiErrorCode::Canceled, "PDF generation was canceled");
    SelfContainedPdfDocument document(cancel);
    QFont font;
    font.setPointSize(10);
    document.setDefaultFont(font);
    document.setHtml(QString::fromUtf8(htmlUtf8.data(), static_cast<int>(htmlUtf8.size())));

    QFile output(QString::fromUtf8(absoluteOutputPath.c_str()));
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return MaiToolResult::failure(MaiErrorCode::InvalidInput, "PDF output could not be created as a new file");
    QPdfWriter writer(&output);
    writer.setPageSize(QPageSize(QPageSize::A4));
    writer.setPageMargins(QMarginsF(18, 18, 18, 18));
    writer.setResolution(144);
    document.print(&writer);
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed))
        return MaiToolResult::failure(MaiErrorCode::Canceled, "PDF generation was canceled");
    return MaiToolResult::success("PDF rendering completed");
}
