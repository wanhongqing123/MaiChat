// 文件作用：把普通原生相机绑定到 Qt UI；Qt 只接收拥有内存的预览帧和照片结果。
#pragma once

#include <QImage>
#include <QObject>
#include <QString>

#include <memory>

#include "MaiCameraCapture.h"

// 所有公共方法在所属 Qt 线程调用；原生回调只投递工作，不能直接访问控件或发 UI 信号。
// start 只表示受理；首个有效 BGRA 帧才就绪。预览只保留最新帧，最多一个待显示任务。
// 照片在工作线程以原生 JPEG 字节保存。photoReady 后须 takePhoto 显式领取：
// 已领取文件由 UI 持有；stop/析构只清理本次未领取或迟到结果，不删除旧照片。
class DesktopCameraSession final : public QObject {
    Q_OBJECT
public:
    enum class State { Stopped, Starting, Running, Error };
    Q_ENUM(State)

    explicit DesktopCameraSession(QObject* parent = nullptr);
    ~DesktopCameraSession() override;

    bool start(const QString& directory);
    void stop();
    State state() const;
    bool isReady() const;
    bool photoPending() const;
    QString errorString() const;

    // 非零 ID 表示快门受理；同一时刻最多一个待拍摄、保存或领取的照片。
    // takePhoto 只接受匹配的已完成 ID，成功后将新文件交给调用方并清除 pending。
    quint64 capturePhoto();
    QString takePhoto(quint64 requestId);

signals:
    void stateChanged(DesktopCameraSession::State state);
    void frameReady(const QImage& image, quint64 sequence);
    void photoReady(quint64 requestId);
    void photoFailed(quint64 requestId, const QString& error);
    void errorOccurred(const QString& error);

private:
    void release();
    void displayFrame(quint64 generation);
    void nativeError(const QString& error);
    void receivePhoto(quint64 generation, const MaiCameraCapture::Photo& photo);
    void finishPhoto(quint64 generation, quint64 requestId);
    struct Runtime;
    std::unique_ptr<Runtime> runtime_;
};
