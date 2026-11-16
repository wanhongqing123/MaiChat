// 文件作用：桌面端共用界面；ImagePreviewDialog 负责对话框呈现、确认交互和结果回传，本文件给出其实现。
// 主要入口：原生双指/滚轮缩放、鼠标平移、适应窗口复位及原有关闭交接。
// 实现边界：与 ImagePreviewDialog 的公开声明配套，调用方通过相应接口进入。
#include "ui/ImagePreviewDialog.h"
#include "ui/DesktopRasterImage.h"

#include <QGuiApplication>
#include <QCloseEvent>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPointingDevice>
#include <QApplication>
#include <QScreen>
#include <QShowEvent>
#include <QLayout>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QPointer>
#include <QPropertyAnimation>
#include <QWindow>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <utility>

namespace {

// QVariantAnimation evaluates updateCurrentValue even when its integer QRect
// has stopped changing. Update geometry first, then advance the image using the
// same clock and easing; valueChanged alone would lose those remaining frames.
class ImagePreviewCloseAnimation final : public QPropertyAnimation {
public:
    ImagePreviewCloseAnimation(QWidget* target, std::function<void(qreal)> progress)
        : QPropertyAnimation(target, "geometry", target), progress_(std::move(progress)) {}

protected:
    void updateCurrentValue(const QVariant& value) override {
        const QPointer<ImagePreviewCloseAnimation> guard(this);
        QPropertyAnimation::updateCurrentValue(value);
        if (!guard || state() == QAbstractAnimation::Stopped) return;
        const qreal time = duration() > 0 ? qreal(currentLoopTime()) / duration() : 1;
        progress_(std::clamp(easingCurve().valueForProgress(time), 0.0, 1.0));
    }

private:
    std::function<void(qreal)> progress_;
};

// QPainter 只绘制当前视口。放大图片仍共享原始 QPixmap，避免每个手势增量
// 分配一张巨大缩放图或替换窗口/控件而造成闪动。
class ImagePreviewCanvas final : public QLabel {
public:
    ImagePreviewCanvas(const QPixmap& image, QWidget* parent) : QLabel(parent), image_(image) {}

    void setImageRect(const QRectF& rect) {
        imageRect_ = rect;
        update();
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        if (image_.isNull()) {
            QLabel::paintEvent(event);
            return;
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawPixmap(imageRect_, image_, QRectF(image_.rect()));
    }

private:
    QPixmap image_;
    QRectF imageRect_;
};

bool hasTransparentPixels(const QPixmap& pixmap) {
    if (!pixmap.hasAlphaChannel()) return false;
    const QImage pixels = pixmap.toImage();
    // 常规解码与平台 QPixmap 都使用 32 位像素，直接只读逐行检查，不复制整幅图。
    // 只有 Alpha 通道不代表真正透明：JPEG 等不透明内容也可能装在 RGBA 容器中。
    if (pixels.format() == QImage::Format_ARGB32 || pixels.format() == QImage::Format_ARGB32_Premultiplied) {
        for (int y = 0; y < pixels.height(); ++y) {
            const auto* row = reinterpret_cast<const QRgb*>(pixels.constScanLine(y));
            for (int x = 0; x < pixels.width(); ++x)
                if (qAlpha(row[x]) != 255) return true;
        }
        return false;
    }
    if (pixels.format() == QImage::Format_RGBA8888 || pixels.format() == QImage::Format_RGBA8888_Premultiplied) {
        for (int y = 0; y < pixels.height(); ++y) {
            const auto* row = pixels.constScanLine(y);
            for (int x = 0; x < pixels.width(); ++x)
                if (row[x * 4 + 3] != 255) return true;
        }
        return false;
    }
    // 罕见像素格式保留其原生精度读取 Alpha；检查只发生在加载时，不随缩放或绘制重做。
    for (int y = 0; y < pixels.height(); ++y)
        for (int x = 0; x < pixels.width(); ++x)
            if (pixels.pixelColor(x, y).alphaF() < 1.0f) return true;
    return false;
}

}  // namespace

ImagePreviewDialog::ImagePreviewDialog(const QString& imagePath, QWidget* parent)
    : ImagePreviewDialog(QPixmap::fromImage(desktopReadRasterFile(imagePath).image), parent) {}

ImagePreviewDialog::ImagePreviewDialog(const QPixmap& image, QWidget* parent) : QDialog(parent), image_(image) {
    setWindowTitle(QStringLiteral("图片预览"));
    // 图片使用独立的内容窗口；两种桌面平台都不展示系统标题栏及窗口按钮。
    // 保留尺寸拖柄、适应窗口时点击和 Esc/Enter 关闭。放大时点击用于平移，
    // 不会关闭同一个预览；双击恢复适应窗口。
    setWindowFlag(Qt::FramelessWindowHint);
    setModal(true);
    setSizeGripEnabled(true);
    // 只改变显示衬底，原图和其透明像素保持不变；不透明图片继续使用原暗色画布。
    const bool transparent = hasTransparentPixels(image_);
    canvasColor_ = transparent ? QColor(Qt::white) : QColor(8, 9, 11);
    setStyleSheet(QStringLiteral("QDialog { background: %1; } QLabel { color: %2; }")
                      .arg(canvasColor_.name(), transparent ? QStringLiteral("#0f172a") : QStringLiteral("#ffffff")));

    imageLabel_ = new ImagePreviewCanvas(image_, this);
    imageLabel_->setObjectName(QStringLiteral("imagePreviewCanvas"));
    imageLabel_->setAlignment(Qt::AlignCenter);
    imageLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    // 鼠标交互由对话框统一处理；原生手势/滚轮若发给子视口也走同一入口。
    imageLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
    imageLabel_->installEventFilter(this);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(imageLabel_);

    if (!image_.isNull()) {
        QSize initialSize = (QSizeF(image_.size()) / image_.devicePixelRatioF()).toSize();
        QScreen* targetScreen =
            parent && parent->windowHandle() ? parent->windowHandle()->screen() : QGuiApplication::primaryScreen();
        if (targetScreen) {
            const QSize available = targetScreen->availableGeometry().size() - QSize(80, 100);
            if (initialSize.width() > available.width() || initialSize.height() > available.height()) {
                initialSize.scale(available, Qt::KeepAspectRatio);
            }
        }
        resize(initialSize);
    } else {
        resize(320, 240);
    }
    updateImage();
}

void ImagePreviewDialog::setCloseHandler(std::function<bool()> handler) {
    closeHandler_ = std::move(handler);
}

QColor ImagePreviewDialog::canvasColor() const {
    return canvasColor_;
}

QPropertyAnimation* ImagePreviewDialog::createCloseAnimation() {
    closingImageView_ = true;
    mousePressed_ = dragging_ = false;
    updatePointerCursor();
    const qreal initialZoom = zoom_;
    const QPointF initialPan = pan_;
    const qreal initialFitWidth = fittedImageSize(imageLabel_->size()).width();
    const QPointer<ImagePreviewDialog> preview(this);
    // The animation owns the only clock. Its per-evaluation callback runs after
    // the viewport geometry update, including constant or quantized geometry.
    return new ImagePreviewCloseAnimation(this, [preview, initialZoom, initialPan, initialFitWidth](qreal progress) {
        if (!preview) return;
        const qreal fitWidth = preview->fittedImageSize(preview->imageLabel_->size()).width();
        preview->zoom_ = 1 + (initialZoom - 1) * (1 - progress);
        preview->pan_ = initialPan * ((initialFitWidth > 0 ? fitWidth / initialFitWidth : 1) * (1 - progress));
        preview->previousViewport_ = preview->imageLabel_->size();
        preview->updateImage();
    });
}

bool ImagePreviewDialog::event(QEvent* event) {
    if (closingImageView_ &&
        (event->type() == QEvent::NativeGesture || event->type() == QEvent::Wheel ||
         event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease ||
         event->type() == QEvent::MouseButtonDblClick || event->type() == QEvent::MouseMove)) {
        event->accept();
        return true;
    }
    if (event->type() == QEvent::NativeGesture) {
        auto* gesture = static_cast<QNativeGestureEvent*>(event);
        if (gesture->gestureType() == Qt::BeginNativeGesture || gesture->gestureType() == Qt::EndNativeGesture) {
            mousePressed_ = dragging_ = false;
            updatePointerCursor();
            event->accept();
            return true;
        }
        if (gesture->gestureType() == Qt::ZoomNativeGesture) {
            const QPointF origin(imageLabel_->mapToGlobal(QPoint()));
            zoomImage(1 + gesture->value(), gesture->globalPosition() - origin);
            event->accept();
            return true;
        }
        if (gesture->gestureType() == Qt::PanNativeGesture && zoom_ > 1) {
            pan_ += gesture->delta();
            updateImage();
            event->accept();
            return true;
        }
    } else if (event->type() == QEvent::WindowDeactivate) {
        mousePressed_ = dragging_ = false;
        updatePointerCursor();
    }
    return QDialog::event(event);
}

bool ImagePreviewDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched == imageLabel_) {
        if (event->type() == QEvent::NativeGesture) return this->event(event);
        if (event->type() == QEvent::Wheel) {
            wheelEvent(static_cast<QWheelEvent*>(event));
            return event->isAccepted();
        }
    }
    return QDialog::eventFilter(watched, event);
}

void ImagePreviewDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (layout()) layout()->setGeometry(contentsRect());
    updateImage();
}

void ImagePreviewDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        requestClose();
        return;
    }
    QDialog::keyPressEvent(event);
}

void ImagePreviewDialog::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        mousePressed_ = true;
        pressWasZoomed_ = zoom_ > 1;
        dragging_ = false;
        pressPosition_ = lastDragPosition_ = event->position();
        updatePointerCursor();
        event->accept();
        return;
    }
    QDialog::mousePressEvent(event);
}

void ImagePreviewDialog::mouseMoveEvent(QMouseEvent* event) {
    if (mousePressed_ && event->buttons().testFlag(Qt::LeftButton)) {
        if ((event->position() - pressPosition_).manhattanLength() >= QApplication::startDragDistance())
            dragging_ = true;
        if (dragging_ && pressWasZoomed_) {
            pan_ += event->position() - lastDragPosition_;
            lastDragPosition_ = event->position();
            updateImage();
        }
        event->accept();
        return;
    }
    QDialog::mouseMoveEvent(event);
}

void ImagePreviewDialog::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && mousePressed_) {
        const bool close = !pressWasZoomed_ && !dragging_ && zoom_ == 1 &&
                           (event->position() - pressPosition_).manhattanLength() < QApplication::startDragDistance();
        mousePressed_ = dragging_ = false;
        updatePointerCursor();
        event->accept();
        if (close) requestClose();
        return;
    }
    QDialog::mouseReleaseEvent(event);
}

void ImagePreviewDialog::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && zoom_ > 1) {
        mousePressed_ = dragging_ = false;
        resetImageView();
        event->accept();
        return;
    }
    QDialog::mouseDoubleClickEvent(event);
}

void ImagePreviewDialog::wheelEvent(QWheelEvent* event) {
    if (closingImageView_) {
        event->accept();
        return;
    }
    // 触控板的像素滚动直接平移；鼠标滚轮（包括 Ctrl+滚轮）围绕指针缩放。
    // Mac 双指张开由 NativeGesture 处理，不把两指平移误当作缩放。
    const bool preciseStream =
        event->phase() != Qt::NoScrollPhase || !event->pixelDelta().isNull() ||
        (event->pointingDevice() && event->pointingDevice()->type() == QInputDevice::DeviceType::TouchPad);
    // 精确滚动可能有非零 angleDelta，但不足一个逻辑像素而得到零 pixelDelta。
    // 用设备/滚动流识别平移；零像素保持 no-op，不改走鼠标滚轮缩放。
    if (preciseStream && !event->modifiers().testFlag(Qt::ControlModifier)) {
        if (zoom_ > 1) {
            pan_ += event->pixelDelta();
            updateImage();
        }
    } else if (event->angleDelta().y() != 0 || event->pixelDelta().y() != 0) {
        const qreal steps =
            event->angleDelta().y() != 0 ? event->angleDelta().y() / 120.0 : event->pixelDelta().y() / 40.0;
        const QPointF origin(imageLabel_->mapToGlobal(QPoint()));
        zoomImage(std::pow(1.2, steps), event->globalPosition() - origin);
    } else if (zoom_ > 1) {
        pan_.rx() += event->angleDelta().x() / 3.0;
        updateImage();
    }
    event->accept();
}

void ImagePreviewDialog::requestClose() {
    if (closeHandler_ && closeHandler_()) return;
    accept();
}

void ImagePreviewDialog::closeEvent(QCloseEvent* event) {
    if (closeHandler_ && closeHandler_()) {
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

void ImagePreviewDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    // 同一个 native window 的每个几何帧先更新真实图像视口，再缩放像素。
    if (layout()) layout()->setGeometry(contentsRect());
    updateImage();
}

void ImagePreviewDialog::updateImage() {
    if (image_.isNull()) {
        imageLabel_->setText(QStringLiteral("图片无法加载"));
        return;
    }
    const QSizeF viewport(imageLabel_->size());
    const QSizeF fitted = fittedImageSize(viewport);
    if (!previousViewport_.isEmpty() && previousViewport_ != viewport) {
        // 调整窗口时保留正位于视口中心的图片位置，而不是把放大内容跳回中央。
        const QSizeF previous = fittedImageSize(previousViewport_);
        if (previous.width() > 0) pan_ *= fitted.width() / previous.width();
    }
    previousViewport_ = viewport;
    const QSizeF scaled = fitted * zoom_;
    const qreal limitX = (std::max)(0.0, (scaled.width() - viewport.width()) / 2);
    const qreal limitY = (std::max)(0.0, (scaled.height() - viewport.height()) / 2);
    pan_.setX(std::clamp(pan_.x(), -limitX, limitX));
    pan_.setY(std::clamp(pan_.y(), -limitY, limitY));
    const QPointF origin((viewport.width() - scaled.width()) / 2, (viewport.height() - scaled.height()) / 2);
    static_cast<ImagePreviewCanvas*>(imageLabel_)->setImageRect(QRectF(origin + pan_, scaled));
    updatePointerCursor();
}

QSizeF ImagePreviewDialog::fittedImageSize(const QSizeF& viewport) const {
    if (image_.isNull() || viewport.isEmpty()) return {};
    QSizeF size = QSizeF(image_.size()) / image_.devicePixelRatioF();
    size.scale(viewport, Qt::KeepAspectRatio);
    return size;
}

void ImagePreviewDialog::zoomImage(qreal factor, const QPointF& anchor) {
    if (!std::isfinite(factor) || factor <= 0 || image_.isNull()) return;
    const qreal next = std::clamp(zoom_ * factor, 1.0, 32.0);
    if (qFuzzyCompare(next, zoom_)) return;
    const QPointF center(imageLabel_->width() / 2.0, imageLabel_->height() / 2.0);
    pan_ = anchor + (center + pan_ - anchor) * (next / zoom_) - center;
    zoom_ = next;
    updateImage();
}

void ImagePreviewDialog::resetImageView() {
    zoom_ = 1;
    pan_ = {};
    updateImage();
}

void ImagePreviewDialog::updatePointerCursor() {
    setCursor(!closingImageView_ && zoom_ > 1 ? (mousePressed_ ? Qt::ClosedHandCursor : Qt::OpenHandCursor)
                                              : Qt::ArrowCursor);
}
