// 文件作用：桌面端共用界面；MaiFfplayVideoDialog 负责对话框呈现、确认交互和结果回传，本文件给出其声明。
// 类型关系：MaiFfplayVideoDialog final : public QDialog；审阅时可据此追踪父类、接口或回调契约。
// 对外边界：本文件给出 MaiFfplayVideoDialog 的类型和函数声明；参数、返回值和对象生命周期以此为准。
// 公开入口：activeDialog、setTitle、setSize、setPosition。
// 直接依赖：MaiFfplayEntry.h；实现与这些接口协作。
#pragma once

#include <QDialog>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <thread>
#include <atomic>
#include <functional>
#include <memory>

#include "MaiFfplayEntry.h"

class QPushButton;
class QComboBox;
class QSlider;
class QLabel;
class QTimer;
class QWidget;
class QEvent;
class QAction;
class QToolButton;
struct MaiCloudVod;

class MaiFfplayVideoDialog final : public QDialog {
    Q_OBJECT

public:
    explicit MaiFfplayVideoDialog(const QString& path, QWidget* parent = nullptr, int64_t startPositionUs = 0);
    MaiFfplayVideoDialog(const QString& displayName, std::shared_ptr<MaiCloudVod> source, QWidget* parent = nullptr);
    ~MaiFfplayVideoDialog() override;

    static QPointer<MaiFfplayVideoDialog> activeDialog();
    bool sendCommand(const QString& command);
    bool seekPercent(double fraction);
    bool seekSeconds(double seconds);
    bool setSpeed(double speed);
    bool replay();
    // GUI 线程读取；仅真实定位/换片递增，供工具识别期间发生的用户导航。
    uint64_t navigationGeneration() const;
    void setPlaylist(const QStringList& paths);
    bool navigateVideo(int direction);
    double speed() const {
        return playbackSpeed_;
    }
    bool isStarted() const {
        return started_;
    }
    bool isPlaying() const {
        return playing_;
    }
    uint64_t presentedFrames() const;
    // 仅在 Graphics 接受保留 HLG/PQ 元数据的帧后成立，供宿主诊断实际播放链路。
    bool hasPresentedHdrFrame() const;
    void setForwardAction(std::function<void(const QString&)> action);
    // Ordinary forwarding remains available independently of the compact UI.
    void forwardVideo();
    void setDocked(bool docked) {
        docked_ = docked;
    }

protected:
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    static bool presentVideo(void* userData, const MaiVideoFrame* frame, const MaiVideoSubtitle* subtitle);
    static bool presentRgba(void* userData, const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t stride);
    static int readCloudInput(void* userData, uint64_t offset, uint8_t* output, int capacity);
    static void setTitle(void* userData, const char* title);
    static void setSize(void* userData, int width, int height);
    static void setPosition(void* userData, int x, int y);
    static void setFullscreen(void* userData, int enabled);
    static void showWindow(void* userData);
    bool startPlayback();
    void stopPlayback(bool cancelCloudSource = false);
    void saveCopy();
    void updatePlayButton();
    // 进度条独占上排，图标动作位于下排；两行均随视口宽度伸展。
    void updateControlLayout();
    void revealControls();
    void scheduleControlHide();
    void updateWindowChrome();
    void syncAudioControls();
    void applyFullscreen(bool enabled);
    void toggleFullscreen();
    bool handleFrameMouse(QObject* watched, QEvent* event);

    QString path_;
    std::shared_ptr<MaiCloudVod> cloudSource_;
    QStringList playlist_;
    int playlistIndex_ = 0;
    QWidget* surface_ = nullptr;
    QWidget* overlay_ = nullptr;
    QWidget* titleBar_ = nullptr;
    QLabel* titleLabel_ = nullptr;
    QLabel* durationLabel_ = nullptr;
    QPushButton* maximizeButton_ = nullptr;
    QToolButton* volumeButton_ = nullptr;
    QSlider* volumeSlider_ = nullptr;
    QAction* muteAction_ = nullptr;
    QPushButton* pauseButton_ = nullptr;
    QPushButton* previousButton_ = nullptr;
    QPushButton* nextButton_ = nullptr;
    QComboBox* speedSelector_ = nullptr;
    QSlider* seekSlider_ = nullptr;
    QLabel* timeLabel_ = nullptr;
    QTimer* progressTimer_ = nullptr;
    QTimer* controlsHideTimer_ = nullptr;
    QLabel* errorLabel_ = nullptr;
    MaiFfplayHost host_{};
    std::thread playbackThread_;
    void* nativeView_ = nullptr;
    uint64_t viewId_ = 0;
    bool started_ = false;
    bool playing_ = true;
    bool controlsPlaying_ = true;
    bool controlsSeeking_ = false;
    bool docked_ = false;
    double playbackSpeed_ = 1.0;
    int volumePercent_ = 100;
    bool audioMuted_ = false;
    // 未入队的准备期意图持续重试直到成功；已入队只等 FIFO 序号回执，
    // 不用超时/数值相等猜测完成，避免覆盖后续键盘或 Agent 的音量命令。
    bool volumeRequestPending_ = false;
    bool muteRequestPending_ = false;
    uint64_t volumeRequestReceipt_ = 0;
    uint64_t muteRequestReceipt_ = 0;
    uint64_t audioIntentRevision_ = 0;
    uint64_t volumeIntentRevision_ = 0;
    uint64_t muteIntentRevision_ = 0;
    // 低 8 位为设置，高位为意图版本。启动线程一次读取并回报消费版本，
    // GUI 不重发已经作为初始参数生效的设置，以免覆盖随后的工具命令。
    std::atomic<uint64_t> audioStartupSettings_{200};
    std::atomic<uint64_t> audioStartupConsumedRevision_{0};
    bool movingFrame_ = false;
    bool windowWasMaximized_ = false;
    Qt::Edges resizingFrame_;
    QPoint framePressGlobal_;
    QRect framePressGeometry_;
    int64_t startPositionUs_ = 0;
    uint64_t navigationGeneration_ = 0;
    std::atomic<bool> playbackFinished_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> presentedHdrFrame_{false};
    std::atomic<bool> hlgOutputRequested_{false};
    MaiFfplayHdrSourceInfo sourceInfo_{};
    std::function<void(const QString&)> forwardAction_;
};
