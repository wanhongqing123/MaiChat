// 文件作用：桌面端共用界面；ImagePreviewDialog 负责对话框呈现、确认交互和结果回传，本文件给出其实现。
// 主要入口：keyPressEvent、mousePressEvent、resizeEvent、updateImage。
// 实现边界：与 ImagePreviewDialog 的公开声明配套，调用方通过相应接口进入。
#include "ui/ImagePreviewDialog.h"
#include "ui/DesktopRasterImage.h"

#include <QGuiApplication>
#include <QCloseEvent>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScreen>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QWindow>
#include <utility>

namespace {

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
    // 保留尺寸拖柄、图片点击和 Esc/Enter 关闭，不把媒体嵌回原会话。
    setWindowFlag(Qt::FramelessWindowHint);
    setModal(true);
    setSizeGripEnabled(true);
    // 只改变显示衬底，原图和其透明像素保持不变；不透明图片继续使用原暗色画布。
    const bool transparent = hasTransparentPixels(image_);
    canvasColor_ = transparent ? QColor(Qt::white) : QColor(8, 9, 11);
    setStyleSheet(QStringLiteral("QDialog { background: %1; } QLabel { color: %2; }")
                      .arg(canvasColor_.name(), transparent ? QStringLiteral("#0f172a") : QStringLiteral("#ffffff")));

    imageLabel_ = new QLabel(this);
    imageLabel_->setAlignment(Qt::AlignCenter);
    imageLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    // 图片只是展示内容；让点击直接落到对话框，由对话框统一关闭预览。
    imageLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);

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

void ImagePreviewDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (closeHandler_ && closeHandler_()) return;
        accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

void ImagePreviewDialog::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        if (closeHandler_ && closeHandler_()) return;
        accept();
        return;
    }
    QDialog::mousePressEvent(event);
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
    updateImage();
}

void ImagePreviewDialog::updateImage() {
    if (image_.isNull()) {
        imageLabel_->setText(QStringLiteral("图片无法加载"));
        return;
    }
    // 高分屏（DPR>1）按物理分辨率缩放并声明 DPR，否则预览图被绘制层二次放大而发虚。
    const qreal dpr = devicePixelRatioF();
    QPixmap scaled =
        image_.scaled((QSizeF(imageLabel_->size()) * dpr).toSize(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    scaled.setDevicePixelRatio(dpr);
    imageLabel_->setPixmap(scaled);
}
