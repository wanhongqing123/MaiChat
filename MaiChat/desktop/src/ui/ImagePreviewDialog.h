// 文件作用：桌面端共用界面；ImagePreviewDialog 负责对话框呈现、确认交互和结果回传，本文件给出其声明。
// 类型关系：ImagePreviewDialog final : public QDialog；审阅时可据此追踪父类、接口或回调契约。
// 对外边界：本文件给出 ImagePreviewDialog 的类型和函数声明；参数、返回值和对象生命周期以此为准。
#pragma once

#include <QColor>
#include <QDialog>
#include <QLabel>
#include <QPixmap>
#include <QPointF>
#include <QSizeF>
#include <QString>
#include <functional>

class QCloseEvent;
class QShowEvent;
class QWheelEvent;
class QPropertyAnimation;

class ImagePreviewDialog final : public QDialog {
    Q_OBJECT
public:
    explicit ImagePreviewDialog(const QString& imagePath, QWidget* parent = nullptr);
    explicit ImagePreviewDialog(const QPixmap& image, QWidget* parent = nullptr);
    void setCloseHandler(std::function<bool()> handler);
    const QPixmap& sourceImage() const {
        return image_;
    }
    // 根据实际像素 Alpha 在构造时选择一次衬底；只读查询不再扫描图片。
    // 主窗口转场应复用同一颜色，避免透明区域在交接时露出不同背景。
    QColor canvasColor() const;
    // Create the one close animation; the caller sets geometry endpoints,
    // duration and easing before starting it. The dialog owns the animation.
    // Image progress advances on every evaluation, even with unchanged integer
    // geometry. GUI thread only; the close handler creates this once per dialog.
    QPropertyAnimation* createCloseAnimation();

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void updateImage();
    QSizeF fittedImageSize(const QSizeF& viewport) const;
    void zoomImage(qreal factor, const QPointF& anchor);
    void resetImageView();
    void updatePointerCursor();
    void requestClose();

    QLabel* imageLabel_ = nullptr;
    QPixmap image_;
    QColor canvasColor_{8, 9, 11};
    std::function<bool()> closeHandler_;
    // 缩放和平移只改变原图的绘制矩形，不创建放大后的整幅位图。
    qreal zoom_ = 1;
    QPointF pan_;
    QSizeF previousViewport_;
    QPointF pressPosition_;
    QPointF lastDragPosition_;
    bool mousePressed_ = false;
    bool pressWasZoomed_ = false;
    bool dragging_ = false;
    bool closingImageView_ = false;
};
