// 文件作用：为 macOS 的 MP4/MOV HDR 视频保留系统的逐帧显示映射，复用现有播放控件。
// 普通代码直接调用；云盘仍由 MaiCloudVod 提供字节，不把媒体交给账号服务转发。
#pragma once

#include <memory>
#include <QString>
#include <QSize>
#include "MaiFfplayEntry.h"

class QWidget;
struct MaiCloudVod;

// 所有方法在 GUI 线程调用。对象独占本次 AVPlayer/显示层；销毁先停止网络读取，
// 再移除显示层。异步 seek 回调只保留内部状态，并检查关闭标记和请求代际。
class MaiMacVideoPlayback {
public:
    MaiMacVideoPlayback(QWidget* surface, const QString& path, std::shared_ptr<MaiCloudVod> cloudSource);
    ~MaiMacVideoPlayback();
    bool open(int64_t positionUs, double speed, bool playing);
    bool command(const QString& command);
    bool seek(int64_t positionUs);
    bool setSpeed(double speed);
    bool status(MaiFfplayPlaybackStatus* output) const;
    // GUI 线程同步设置/读取当前 AVPlayer 音量；不借用全局 FFplay 的旧音频快照。
    bool setVolumePercent(int volume);
    bool setMuted(bool muted);
    bool audioStatus(int* volume, bool* muted) const;
    QSize videoSize() const;
    bool ready() const;
    bool failed() const;
    void resize();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
