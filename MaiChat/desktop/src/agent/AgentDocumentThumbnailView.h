// 文件作用：文件卡内的首屏内容视图；点击透传原按钮，尺寸/DPR/字体变化重绑同一后台入口。
#pragma once
#include "agent/AgentDocumentThumbnail.h"
#include <QWidget>
#include <utility>

class QTimer;
class AgentDocumentThumbnailView final : public QWidget {
public:
    using ThemeProvider = std::function<std::pair<qreal, QString>()>;
    // The host can synchronously remeasure its outer document row before paint.
    // A notification may delete the view; it is always the last operation.
    using LayoutChanged = std::function<void()>;
    AgentDocumentThumbnailView(QWidget* parent, AgentDocumentThumbnailBinding::AccessGuard accessGuard,
                               ThemeProvider theme = {}, LayoutChanged layoutChanged = {});
    void setDocument(const AgentDocumentThumbnailRequest& request);
    void clear();
    const QImage& thumbnail() const;
    QString sourcePath() const;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    bool event(QEvent*) override;

private:
    void schedule();
    void notifyLayoutChanged();
    AgentDocumentThumbnailRequest request_;
    QImage image_;
    AgentDocumentThumbnailBinding* binding_ = nullptr;
    QTimer* timer_ = nullptr;
    ThemeProvider theme_;
    LayoutChanged layoutChanged_;
};
