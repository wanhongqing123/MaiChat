// 文件作用：以 Qt 控件显示普通原生相机的帧；用户快门与 Agent 预览共用同一照片入口。
#include "ui/DesktopCameraDialog.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QPointer>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

#include "ui/DesktopCameraSession.h"

namespace {
struct CameraPreviewState {
    QPointer<DesktopCameraDialog> dialog;
    QString sessionId;
    QString lastPhoto;
    QUuid generation;
};

QHash<QWidget*, CameraPreviewState>& activePreviews() {
    static QHash<QWidget*, CameraPreviewState> previews;
    return previews;
}

// 手动入口和 Agent 入口共用一台摄像头窗口，不能各开一个设备句柄。
QPointer<DesktopCameraDialog>& cameraOwner() {
    static QPointer<DesktopCameraDialog> owner;
    return owner;
}

bool usableDirectory(const QString& directory) {
    const QFileInfo info(directory);
    return info.isDir() && info.isWritable();
}
}  // namespace

QString DesktopCameraDialog::capturePhoto(QWidget* parent, const QString& directory, QString* error,
                                          const std::atomic<bool>* canceled) {
    if (error) error->clear();
    if (canceled && canceled->load(std::memory_order_relaxed)) return {};
    if (cameraOwner() && cameraOwner()->isVisible()) {
        if (error) *error = QStringLiteral("另一个相机窗口正在使用摄像头");
        return {};
    }
    if (!usableDirectory(directory)) {
        if (error) *error = QStringLiteral("当前工作目录不可写，无法保存照片");
        return {};
    }
    DesktopCameraDialog dialog(parent, directory, false, canceled);
    if (dialog.session_->state() == DesktopCameraSession::State::Error) {
        if (error) *error = dialog.error_;
        return {};
    }
    QTimer cancellation;
    if (canceled) {
        cancellation.setInterval(100);
        QObject::connect(&cancellation, &QTimer::timeout, &dialog, [&dialog, canceled] {
            if (canceled->load(std::memory_order_relaxed)) dialog.reject();
        });
        cancellation.start();
    }
    if (dialog.exec() != QDialog::Accepted) {
        if (error && !(canceled && canceled->load(std::memory_order_relaxed))) *error = dialog.error_;
        return {};
    }
    return dialog.savedPath_;
}

bool DesktopCameraDialog::openPreview(QWidget* parent, const QString& sessionId, const QString& directory,
                                      QString* error) {
    if (error) error->clear();
    if (!parent || sessionId.isEmpty()) {
        if (error) *error = QStringLiteral("MaiChat 窗口不可用");
        return false;
    }
    const CameraPreviewState current = activePreviews().value(parent);
    if (current.dialog && current.dialog->isVisible()) {
        if (current.sessionId != sessionId) {
            if (error) *error = QStringLiteral("另一段会话正在使用摄像头");
            return false;
        }
        current.dialog->raise();
        current.dialog->activateWindow();
        return true;
    }
    if (cameraOwner() && cameraOwner()->isVisible()) {
        if (error) *error = QStringLiteral("另一个相机窗口正在使用摄像头");
        return false;
    }
    if (!usableDirectory(directory)) {
        if (error) *error = QStringLiteral("当前工作目录不可写，无法保存照片");
        return false;
    }
    auto* dialog = new DesktopCameraDialog(parent, directory, true);
    if (dialog->session_->state() == DesktopCameraSession::State::Error) {
        if (error) *error = dialog->error_;
        delete dialog;
        return false;
    }
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    const QUuid generation = QUuid::createUuid();
    activePreviews().insert(parent, {dialog, sessionId, {}, generation});
    QObject::connect(dialog, &QObject::destroyed, parent, [parent, generation] {
        auto preview = activePreviews().find(parent);
        if (preview != activePreviews().end() && preview->generation == generation) preview->dialog = nullptr;
    });
    QObject::connect(parent, &QObject::destroyed, qApp, [parent] { activePreviews().remove(parent); });
    dialog->show();
    return true;
}

bool DesktopCameraDialog::closePreview(QWidget* parent, const QString& sessionId) {
    const CameraPreviewState state = activePreviews().value(parent);
    const QPointer<DesktopCameraDialog> dialog = state.dialog;
    if (state.sessionId != sessionId || !dialog || !dialog->isVisible()) return false;
    dialog->reject();
    return true;
}

bool DesktopCameraDialog::isPreviewOpen(QWidget* parent, const QString& sessionId) {
    const CameraPreviewState state = activePreviews().value(parent);
    const QPointer<DesktopCameraDialog> dialog = state.sessionId == sessionId ? state.dialog : nullptr;
    return dialog && dialog->isVisible();
}

QString DesktopCameraDialog::previewError(QWidget* parent, const QString& sessionId) {
    const CameraPreviewState state = activePreviews().value(parent);
    return state.sessionId == sessionId && state.dialog ? state.dialog->error_ : QString();
}

QString DesktopCameraDialog::lastPreviewPhoto(QWidget* parent, const QString& sessionId) {
    const CameraPreviewState state = activePreviews().value(parent);
    const QString path = state.sessionId == sessionId ? state.lastPhoto : QString();
    return QFileInfo(path).isFile() ? path : QString();
}

DesktopCameraDialog::DesktopCameraDialog(QWidget* parent, const QString& destination, bool persistent,
                                         const std::atomic<bool>* canceled)
    : QDialog(parent), persistent_(persistent), canceled_(canceled) {
    cameraOwner() = this;
    setObjectName(QStringLiteral("desktopCameraDialog"));
    setWindowTitle(QStringLiteral("拍照"));
    setMinimumSize(640, 520);
    auto* layout = new QVBoxLayout(this);
    viewfinder_ = new QLabel(this);
    viewfinder_->setObjectName(QStringLiteral("desktopCameraPreview"));
    viewfinder_->setAlignment(Qt::AlignCenter);
    viewfinder_->setMinimumSize(1, 1);
    viewfinder_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    layout->addWidget(viewfinder_, 1);
    status_ = new QLabel(QStringLiteral("正在打开摄像头…"), this);
    status_->setObjectName(QStringLiteral("desktopCameraStatus"));
    layout->addWidget(status_);
    auto* actions = new QHBoxLayout;
    actions->addStretch();
    auto* cancel = new QPushButton(persistent ? QStringLiteral("关闭") : QStringLiteral("取消"), this);
    shutter_ = new QPushButton(QStringLiteral("拍照"), this);
    shutter_->setObjectName(QStringLiteral("desktopCameraShutter"));
    shutter_->setEnabled(false);
    actions->addWidget(cancel);
    actions->addWidget(shutter_);
    layout->addLayout(actions);
    connect(cancel, &QPushButton::clicked, this, &DesktopCameraDialog::reject);
    connect(shutter_, &QPushButton::clicked, this, &DesktopCameraDialog::takePhoto);

    session_ = new DesktopCameraSession(this);
    connect(session_, &DesktopCameraSession::stateChanged, this, [this](DesktopCameraSession::State state) {
        if (closing_) return;
        const bool ready = state == DesktopCameraSession::State::Running && session_->isReady();
        shutter_->setEnabled(ready && !pendingCapture_);
        if (state == DesktopCameraSession::State::Starting) status_->setText(QStringLiteral("正在打开摄像头…"));
        if (ready && !pendingCapture_) status_->setText(QStringLiteral("准备就绪"));
        if (state == DesktopCameraSession::State::Error || state == DesktopCameraSession::State::Stopped) {
            previewFrame_ = {};
            viewfinder_->clear();
            pendingCapture_ = 0;
        }
    });
    connect(session_, &DesktopCameraSession::frameReady, this, [this](const QImage& image) {
        if (closing_) return;
        previewFrame_ = image;
        updatePreview();
    });
    connect(session_, &DesktopCameraSession::errorOccurred, this, [this](const QString& error) {
        if (closing_) return;
        error_ = error;
        status_->setText(error_);
        shutter_->setEnabled(false);
    });
    connect(session_, &DesktopCameraSession::photoFailed, this, [this](quint64 request, const QString& error) {
        if (closing_ || request != pendingCapture_) return;
        pendingCapture_ = 0;
        error_ = error;
        status_->setText(error_);
        shutter_->setEnabled(session_->isReady());
    });
    connect(session_, &DesktopCameraSession::photoReady, this, [this](quint64 request) {
        if (closing_ || request != pendingCapture_) return;
        if (canceled_ && canceled_->load(std::memory_order_relaxed)) {
            reject();
            return;
        }
        const QString path = session_->takePhoto(request);
        if (path.isEmpty()) return;
        savedPath_ = path;
        error_.clear();
        pendingCapture_ = 0;
        if (persistent_) {
            auto preview = activePreviews().find(parentWidget());
            if (preview != activePreviews().end() && preview->dialog == this) preview->lastPhoto = path;
        }
        accept();
    });
    session_->start(destination);
}

DesktopCameraDialog::~DesktopCameraDialog() {
    closing_ = true;
    if (session_) session_->stop();
    if (cameraOwner() == this) cameraOwner().clear();
}

void DesktopCameraDialog::done(int result) {
    closing_ = true;
    pendingCapture_ = 0;
    if (session_) session_->stop();
    QDialog::done(result);
}

void DesktopCameraDialog::reject() {
    done(QDialog::Rejected);
}

void DesktopCameraDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    updatePreview();
}

void DesktopCameraDialog::updatePreview() {
    if (!viewfinder_ || previewFrame_.isNull()) return;
    viewfinder_->setPixmap(
        QPixmap::fromImage(previewFrame_).scaled(viewfinder_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void DesktopCameraDialog::takePhoto() {
    if (closing_ || !session_->isReady() || session_->photoPending()) return;
    if (canceled_ && canceled_->load(std::memory_order_relaxed)) {
        reject();
        return;
    }
    pendingCapture_ = session_->capturePhoto();
    if (pendingCapture_) {
        error_.clear();
        shutter_->setEnabled(false);
        status_->setText(QStringLiteral("正在保存照片…"));
    } else {
        error_ = session_->errorString();
        status_->setText(error_);
        shutter_->setEnabled(session_->isReady());
    }
}
