// 文件作用：把普通 FFplay 音频会话绑定到桌面控件，并统一音乐、inline 音频与视频的所有权。
#pragma once

#include <QObject>
#include <QString>

#include <functional>
#include <memory>

// 所有方法在 GUI 线程调用。媒体探测和解码使用普通媒体接口，Qt 只接收状态及绑定控件。
// 一个进程只有一个活动音频 owner；切换 owner 先停止并收回旧 handle，旧对象不能控制新会话。
// 析构仅停止自己仍拥有的播放；setSource 不自动播放，切换会话后 pause 不会隐式恢复。
class DesktopAudioPlayback final : public QObject {
    Q_OBJECT
public:
    enum class State { Stopped, Starting, Playing, Paused, Ended, Error };
    Q_ENUM(State)

    explicit DesktopAudioPlayback(QObject* parent = nullptr);
    ~DesktopAudioPlayback() override;

    void setSource(const QString& path);
    QString source() const;
    State state() const;
    qint64 positionUs() const;
    qint64 durationUs() const;
    QString errorString() const;

    // play 从 Stopped/Ended 开始或从 Paused 恢复；显式点击旧 owner 可重新取得播放所有权。
    // pause/seek 对已经丢失所有权的对象返回 false，不能把命令发送到其他会话。
    bool play();
    bool pause();
    bool seek(qint64 positionUs);
    void stop();

    static void stopAll();
    // 视频启动前收回全部音频 handle；重复调用无副作用，不启动或操作视频算法。
    static void prepareVideoPlayback();
    // 宿主安装无对象借用的停止活动视频回调；播放音频前执行，清空回调传空函数。
    static void setVideoStopHandler(std::function<void()> handler);

signals:
    void stateChanged(DesktopAudioPlayback::State state);
    void positionChanged(qint64 positionUs);
    void durationChanged(qint64 durationUs);
    void errorOccurred(const QString& error);

private:
    bool ownsSession() const;
    bool ownsPendingStart() const;
    bool startPlayback();
    void releaseSession();
    void refresh();
    void setState(State state);
    void setPosition(qint64 positionUs);
    void setDuration(qint64 durationUs);
    void reportError(const QString& error);
    struct Runtime;
    std::unique_ptr<Runtime> runtime_;
};
