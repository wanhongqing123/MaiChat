// 文件作用：Desktop 拍照预览与用户确认；普通界面和 Agent 工具复用同一入口。
#pragma once

#include <QDialog>
#include <QImage>
#include <QString>

#include <atomic>

class DesktopCameraSession;
class QLabel;
class QPushButton;
class QResizeEvent;

class DesktopCameraDialog final : public QDialog {
public:
    // 返回新建 JPEG 绝对路径；取消返回空路径和空错误，无摄像头或失败返回错误。
    static QString capturePhoto(QWidget* parent, const QString& directory, QString* error,
                                const std::atomic<bool>* canceled = nullptr);
    static bool openPreview(QWidget* parent, const QString& sessionId, const QString& directory, QString* error);
    static bool closePreview(QWidget* parent, const QString& sessionId);
    static bool isPreviewOpen(QWidget* parent, const QString& sessionId);
    static QString previewError(QWidget* parent, const QString& sessionId);
    static QString lastPreviewPhoto(QWidget* parent, const QString& sessionId);

    ~DesktopCameraDialog() override;

private:
    DesktopCameraDialog(QWidget* parent, const QString& destination, bool persistent = false,
                        const std::atomic<bool>* canceled = nullptr);
    void done(int result) override;
    void reject() override;
    void resizeEvent(QResizeEvent* event) override;
    void takePhoto();
    void updatePreview();

    DesktopCameraSession* session_ = nullptr;
    QLabel* viewfinder_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* shutter_ = nullptr;
    QImage previewFrame_;
    QString savedPath_;
    QString error_;
    bool persistent_ = false;
    bool closing_ = false;
    quint64 pendingCapture_ = 0;
    const std::atomic<bool>* canceled_ = nullptr;
};
