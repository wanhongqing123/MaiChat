// 文件作用：桌面端共用界面；MaiFfplayVideoDialog 负责对话框呈现、确认交互和结果回传，本文件给出其实现。
// 主要入口：setForwardAction、forwardVideo、saveCopy、activeDialog。
// 直接依赖：MaiGraphicsPresenter.h；实现与这些接口协作。
#include "ui/UiZoom.h"
#include "ui/MaiFfplayVideoDialog.h"
#include "ui/MaiVideoSpeedSelector.h"
#include "ui/MaiVideoHud.h"
#include "ui/MaiVideoBufferingIndicator.h"
#include "ui/MaiVideoVolumePopup.h"
#include "MaiCloudVod.h"
#include "ui/DesktopMusicPlayerDialog.h"
#include "ui/DesktopAudioPlayback.h"

#include <QCloseEvent>
#include <QWidgetAction>
#include <QWindow>
#include <QComboBox>
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMetaObject>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QHideEvent>
#include <QMenu>
#include <QPushButton>
#include <QPainter>
#include <QResizeEvent>
#include <QSaveFile>
#include <QScreen>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QSvgRenderer>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

#include "MaiGraphicsPresenter.h"

#if defined(Q_OS_MAC)
#include "ui/MaiMacVideoPlayback.h"
#include "ui/MaiMacVideoWindow.h"
extern "C" void* maiGraphicsCreateMacLayer(QWidget* widget);
extern "C" void maiGraphicsResizeMacLayer(void* nativeLayer, QWidget* widget);
extern "C" void maiGraphicsRetainMacLayer(void* nativeLayer);
extern "C" void maiGraphicsReleaseMacLayer(void* nativeLayer);
extern "C" void maiGraphicsStyleMacVideoWindow(QWidget* widget);
extern "C" void maiGraphicsShareMacVideo(QWidget* anchor, const char* utf8Path);
#endif

namespace {

// 保留完整文件名供无障碍读取；省略只发生在绘制阶段，不依赖 show 前的布局宽度。
class MaiVideoTitleLabel final : public QLabel {
public:
    explicit MaiVideoTitleLabel(QWidget* parent) : QLabel(parent) {}

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setPen(palette().color(QPalette::WindowText));
        painter.setFont(font());
        painter.drawText(contentsRect(), Qt::AlignLeft | Qt::AlignVCenter,
                         fontMetrics().elidedText(text(), Qt::ElideMiddle, contentsRect().width()));
    }
};

// 统一使用 24px 图标画布，播放组保留参考的实心轮廓，其他动作用线条图形。
QIcon videoControlIcon(const char* name) {
    QByteArray path;
    const QByteArray key(name);
    // 参考播放器的播放控件采用圆润实心轮廓和两角全屏标记；使用 SVG 引擎
    // 按实际设备比例绘制，避免固定 2 倍位图在 1.25/1.5 倍屏幕上再次缩放。
    if (key == "play" || key == "pause" || key == "next")
        return QIcon(QStringLiteral(":/maichat/icons/player-%1.svg").arg(QString::fromLatin1(key)));
    if (key == "fullscreen" || key == "fullscreen-exit")
        return QIcon(QStringLiteral(":/maichat/icons/player-fullscreen.svg"));
    if (key == "replay")
        path = "<path d='M3 10a9 9 0 1 1 2.8 8.5M3 4v6h6'/>";
    else if (key == "minimize")
        path = "<path d='M5 12h14'/>";
    else if (key == "maximize")
        path = "<rect x='5' y='5' width='14' height='14' rx='2'/>";
    else if (key == "restore")
        path = "<path d='M8 7V4h12v12h-3'/><rect x='4' y='8' width='12' height='12' rx='1'/>";
    else if (key == "volume")
        path = "<path d='M11 5L6 9H3v6h3l5 4zM15 8a6 6 0 0 1 0 8m3-11a10 10 0 0 1 0 14'/>";
    else if (key == "muted")
        path = "<path d='M11 5L6 9H3v6h3l5 4zM16 9l5 6m0-6l-5 6'/>";
    else if (key == "close")
        path = "<path d='M6 6l12 12M6 18L18 6'/>";
    else if (key == "forward")
        path = "<path d='M15 4l6 6-6 6M21 10H9a6 6 0 0 0-6 6v4'/>";
    else if (key == "share")
        path = "<path d='M12 15V3m-4 4l4-4 4 4M6 11H4v10h16V11h-2'/>";
    else if (key == "copy")
        path = "<rect x='8' y='8' width='12' height='13' rx='2'/><path d='M16 8V3H4v13h4'/>";
    else if (key == "folder")
        path = "<path d='M3 7V4h7l2 3h9v13H3z'/>";
    else if (key == "save")
        path = "<path d='M12 3v12m-4-4l4 4 4-4M4 15v6h16v-6'/>";
    else if (key == "previous")
        path = "<path d='M18 4L6 12l12 8z' fill='#f4f6fa' stroke='none'/><path d='M4 4v16' stroke-width='3'/>";
    else
        path =
            "<circle cx='5' cy='12' r='1' fill='#f4f6fa'/><circle cx='12' cy='12' r='1' fill='#f4f6fa'/><circle "
            "cx='19' cy='12' r='1' fill='#f4f6fa'/>";
    QSvgRenderer renderer(
        QByteArray("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='#f4f6fa' "
                   "stroke-width='1.7' stroke-linecap='round' stroke-linejoin='round'>") +
        path + "</svg>");
    QPixmap pixels(48, 48);
    pixels.setDevicePixelRatio(2);
    pixels.fill(Qt::transparent);
    QPainter painter(&pixels);
    renderer.render(&painter, QRectF(0, 0, 24, 24));
    return QIcon(pixels);
}

// 进度条按点击/拖动位置定位，避免 QSlider 默认的整页跳动和整数百分比量化。
class MaiVideoSeekSlider final : public QSlider {
public:
    explicit MaiVideoSeekSlider(QWidget* parent) : QSlider(Qt::Horizontal, parent) {}

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            QSlider::mousePressEvent(event);
            return;
        }
        QStyleOptionSlider option;
        initStyleOption(&option);
        const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
        dragOffset_ = handle.contains(event->pos()) ? event->pos().x() - handle.center().x() : 0;
        setSliderDown(true);
        moveTo(event->pos().x());
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (isSliderDown()) {
            moveTo(event->pos().x());
            event->accept();
        } else
            QSlider::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && isSliderDown()) {
            moveTo(event->pos().x());
            setSliderDown(false);
            event->accept();
        } else
            QSlider::mouseReleaseEvent(event);
    }

private:
    void moveTo(int x) {
        QStyleOptionSlider option;
        initStyleOption(&option);
        const QRect groove = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
        const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
        const int span = std::max(1, groove.width() - handle.width());
        setSliderPosition(QStyle::sliderValueFromPosition(
            minimum(), maximum(), x - dragOffset_ - groove.x() - handle.width() / 2, span, option.upsideDown));
    }

    int dragOffset_ = 0;
};

// Qt 在 Windows 上仅为覆盖 paintEngine() 的原生绘制控件保留 WA_PaintOnScreen。
// 普通 QWidget 会忽略该属性，窗口恢复时 backingstore 可用背景覆盖刚提交的 D3D 帧。
// 两个平台都让 Graphics 独占同一个原生 surface，尺寸变化后仍重绘真实的保留视频帧。
class MaiVideoSurface final : public QWidget {
public:
    explicit MaiVideoSurface(QWidget* parent) : QWidget(parent) {}
    QPaintEngine* paintEngine() const override {
        return nullptr;
    }
};

QPointer<MaiFfplayVideoDialog> sActiveDialog;
int sPresenterUsers = 0;
std::atomic<uint64_t> sPresentedFrames{0};

void onFramePresented(uint64_t, bool success, void*) {
    if (success) sPresentedFrames.fetch_add(1);
}

QString bundledResource(const QString& relative) {
    const QString base = QCoreApplication::applicationDirPath();
#if defined(Q_OS_WIN)
    if (relative.endsWith(QStringLiteral("libmaiagent_obs_metal.so")))
        return base + QStringLiteral("/maiagent_obs_d3d11.dll");
    return base + QStringLiteral("/MaiAgentGraphics");
#else
    const QString bundled = QFileInfo(base + relative).absoluteFilePath();
    if (QFileInfo::exists(bundled)) return bundled;
    const QString development =
        QDir(base).filePath(relative.endsWith(QStringLiteral("libmaiagent_obs_metal.so"))
                                ? QStringLiteral("../maiagent/Graphics/libmaiagent_obs_metal.so")
                                : QStringLiteral("../maiagent/Graphics/data"));
    return QFileInfo(development).absoluteFilePath();
#endif
}

}  // namespace

// 构造时一次性连接播放器、进度条和窗口关闭行为；关闭需停止解码与音频后再销毁界面。
MaiFfplayVideoDialog::MaiFfplayVideoDialog(const QString& path, QWidget* parent, int64_t startPositionUs)
    : QDialog(parent), path_(path), startPositionUs_(std::max<int64_t>(0, startPositionUs)) {
    // 音频卡通过普通播放桥停止当前视频；回调不借用本窗口，关闭后仍不会留下悬空 owner。
    DesktopAudioPlayback::setVideoStopHandler([] {
        if (auto video = MaiFfplayVideoDialog::activeDialog()) video->close();
    });
    // 所有桌面入口共用自绘窗口；原生 surface 只负责画面，不再决定播放器外观。
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setWindowTitle(QFileInfo(path).fileName());
    // 最小尺寸只保证控件可操作，不锁定宽高比；自由拉伸后仍由 Presenter 等比完整显示。
    setMinimumSize(480, 270);
    setStyleSheet(QStringLiteral("QDialog { background:#000000; }"));
    titleBar_ = new MaiVideoHud(this);
    titleBar_->setObjectName(QStringLiteral("ffplayTitleBar"));
    titleBar_->setStyleSheet(QStringLiteral("QWidget#ffplayTitleBar {background:#101010;}"));
    auto* header = new QHBoxLayout(titleBar_);
    header->setContentsMargins(18, 0, 10, 0);
    header->setSpacing(4);
    titleLabel_ = new MaiVideoTitleLabel(titleBar_);
    titleLabel_->setObjectName(QStringLiteral("ffplayTitleLabel"));
    UiZoom::setStyle(titleLabel_, QStringLiteral("color:#f4f4f4;font-size:@detail;"));
    titleLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    titleLabel_->setMinimumWidth(0);
    header->addWidget(titleLabel_, 1);
    const QString flatButton = QStringLiteral(
        "QPushButton,QToolButton { color:#f4f4f4;background:transparent;border:0;"
        "border-radius:5px;padding:0; }"
        "QPushButton:hover,QToolButton:hover { background:#2b2b2b; }"
        "QPushButton:pressed,QToolButton:pressed { background:#3a3a3a; }"
        "QToolButton::menu-indicator { image:none;width:0; }");
    const auto tool = [&](const char* name, const char* icon, const QString& tip) {
        auto* button = new QToolButton(titleBar_);
        button->setObjectName(QString::fromLatin1(name));
        button->setToolTip(tip);
        button->setAccessibleName(tip);
        button->setIcon(videoControlIcon(icon));
        button->setIconSize(QSize(24, 24));
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setFixedSize(34, 34);
        button->setStyleSheet(flatButton);
        header->addWidget(button);
        return button;
    };
    auto* more = tool("videoMoreButton", "more", QStringLiteral("更多操作"));
    auto* moreMenu = new QMenu(this);
    moreMenu->setObjectName(QStringLiteral("ffplayMoreMenu"));
    moreMenu->setWindowFlags(Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    moreMenu->setAttribute(Qt::WA_TranslucentBackground);
    moreMenu->setMinimumWidth(112);
    moreMenu->setStyleSheet(QStringLiteral(
        "QMenu {color:#eeeeee;background:#202020;border:1px solid #383838;border-radius:8px;padding:3px;}"
        "QMenu::item {padding:7px 12px 7px 10px;border-radius:5px;}"
        "QMenu::item:selected {background:#373737;} QMenu::item:disabled {color:#777777;}"
        "QMenu::separator {height:1px;background:#393939;margin:5px;}"));
    auto* save = moreMenu->addAction(videoControlIcon("save"), QStringLiteral("下载"));
    save->setObjectName(QStringLiteral("videoSaveAction"));
    save->setToolTip(QStringLiteral("保存视频副本"));
    connect(save, &QAction::triggered, this, &MaiFfplayVideoDialog::saveCopy);
    auto* forward = moreMenu->addAction(videoControlIcon("share"), QStringLiteral("分享"));
#if defined(Q_OS_MAC)
    forward->setObjectName(QStringLiteral("videoShareAction"));
    connect(forward, &QAction::triggered, this, [this, more] {
        const QByteArray utf8 = path_.toUtf8();
        maiGraphicsShareMacVideo(more, utf8.constData());
    });
#else
    forward->setObjectName(QStringLiteral("videoForwardAction"));
    connect(forward, &QAction::triggered, this, &MaiFfplayVideoDialog::forwardVideo);
#endif
    auto* previousAction = moreMenu->addAction(videoControlIcon("previous"), QStringLiteral("上一段"));
    previousItemAction_ = previousAction;
    previousAction->setObjectName(QStringLiteral("ffplayPreviousItemAction"));
    previousAction->setVisible(false);
    connect(previousAction, &QAction::triggered, this, [this] { navigateVideo(-1); });
    connect(more, &QToolButton::clicked, this, [this, more, moreMenu] {
        // 与参考播放器一样把文件操作收进同一弹层，右边缘对齐省略号按钮。
        const QSize size = moreMenu->sizeHint();
        const QPointer<MaiFfplayVideoDialog> alive(this);
        moreMenu->exec(more->mapToGlobal(QPoint(more->width() - size.width(), more->height() + 6)));
        if (alive) alive->revealControls();
    });
    auto* divider = new QFrame(titleBar_);
    divider->setObjectName(QStringLiteral("ffplayTitleDivider"));
    divider->setFixedSize(1, 18);
    divider->setStyleSheet(QStringLiteral("background:#555555;border:0;"));
    header->addSpacing(8);
    header->addWidget(divider);
    header->addSpacing(8);
    const auto button = [&](QWidget* parentWidget, const char* name, const char* icon, const QString& tip) {
        auto* value = new QPushButton(parentWidget);
        value->setObjectName(QString::fromLatin1(name));
        value->setToolTip(tip);
        value->setAccessibleName(tip);
        value->setIcon(videoControlIcon(icon));
        value->setIconSize(QSize(24, 24));
        value->setFixedSize(36, 36);
        value->setStyleSheet(flatButton);
        value->setAutoDefault(false);
        value->setDefault(false);
        return value;
    };
    auto* minimize = button(titleBar_, "ffplayMinimizeButton", "minimize", QStringLiteral("最小化"));
    maximizeButton_ = button(titleBar_, "ffplayMaximizeButton", "maximize", QStringLiteral("最大化"));
    auto* close = button(titleBar_, "ffplayCloseButton", "close", QStringLiteral("关闭视频"));
    close->setStyleSheet(flatButton + QStringLiteral("QPushButton:hover {background:#c42b1c;}"));
    header->addWidget(minimize);
    header->addWidget(maximizeButton_);
    header->addWidget(close);
    connect(minimize, &QPushButton::clicked, this, &MaiFfplayVideoDialog::minimizeWindow);
    connect(maximizeButton_, &QPushButton::clicked, this, &MaiFfplayVideoDialog::toggleMaximized);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    surface_ = new MaiVideoSurface(this);
    surface_->setObjectName(QStringLiteral("ffplayVideoSurface"));
    surface_->setAttribute(Qt::WA_NativeWindow);
    surface_->setAttribute(Qt::WA_PaintOnScreen);
    surface_->setAttribute(Qt::WA_NoSystemBackground);
    // 持久的 owner HUD 使用 Qt 顶层透明合成；不重建控件，也不让 Qt 擦写 D3D 视频。
    // 窗口态和全屏共用同一真实渐变浮层，不能为栏位从视频视口扣除高度。
    overlay_ = new MaiVideoHud(this);
    overlay_->setObjectName(QStringLiteral("ffplayOverlay"));
    overlay_->setStyleSheet(QStringLiteral("QWidget#ffplayOverlay {background:#000000;}"));
    auto* overlayLayout = new QVBoxLayout(overlay_);
    overlayLayout->setContentsMargins(0, 0, 0, 0);
    auto* panel = new QWidget(overlay_);
    panel->setObjectName(QStringLiteral("ffplayControlPanel"));
    panel->setStyleSheet(QStringLiteral("QWidget#ffplayControlPanel {background:#000000;}"));
    overlayLayout->addWidget(panel);
    auto* panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(16, 7, 16, 9);
    panelLayout->setSpacing(4);
    auto* timeline = new QHBoxLayout();
    timeline->setObjectName(QStringLiteral("ffplayTimelineRow"));
    timeline->setSpacing(8);
    timeLabel_ = new QLabel(QStringLiteral("0:00"), panel);
    timeLabel_->setObjectName(QStringLiteral("ffplayTimeLabel"));
    durationLabel_ = new QLabel(QStringLiteral("0:00"), panel);
    durationLabel_->setObjectName(QStringLiteral("ffplayDurationLabel"));
    for (auto* label : {timeLabel_, durationLabel_}) {
        UiZoom::setStyle(label, QStringLiteral("color:#eeeeee;font-size:@metadata;"));
        label->setMinimumWidth(30);
    }
    seekSlider_ = new MaiVideoSeekSlider(panel);
    seekSlider_->setObjectName(QStringLiteral("ffplaySeekSlider"));
    seekSlider_->setRange(0, 1000);
    seekSlider_->setMinimumWidth(80);
    seekSlider_->setStyleSheet(QStringLiteral(
        "QSlider::groove:horizontal {height:2px;background:#4b4b4b;border-radius:1px;}"
        "QSlider::sub-page:horizontal {background:#f4f4f4;border-radius:2px;}"
        "QSlider::handle:horizontal {width:8px;margin:-3px 0;background:transparent;border:0;border-radius:4px;}"
        "QSlider::handle:horizontal:hover,QSlider::handle:horizontal:pressed {background:white;}"));
    timeline->addWidget(timeLabel_);
    timeline->addWidget(seekSlider_, 1);
    timeline->addWidget(durationLabel_);
    panelLayout->addLayout(timeline);
    auto* row = new QHBoxLayout();
    row->setObjectName(QStringLiteral("ffplayButtonRow"));
    row->setSpacing(14);
    pauseButton_ = button(panel, "ffplayPauseButton", "pause", QStringLiteral("暂停视频"));
    nextButton_ = button(panel, "ffplayNextItemButton", "next", QStringLiteral("下一段视频"));
    pauseButton_->setFixedSize(36, 36);
    // 上一段留在更多菜单及原快捷键中，底栏与参考一致保留播放/下一段一组。
    row->addWidget(pauseButton_);
    row->addWidget(nextButton_);
    nextButton_->hide();
    row->addStretch(1);
    speedSelector_ = new MaiVideoSpeedSelector(panel);
    speedSelector_->setObjectName(QStringLiteral("ffplaySpeedSelector"));
    speedSelector_->setToolTip(QStringLiteral("播放倍速"));
    speedSelector_->setAccessibleName(QStringLiteral("播放倍速"));
    for (double rate : {0.25, 0.5, 1.0, 1.5, 2.0}) speedSelector_->addItem(QStringLiteral("%1×").arg(rate), rate);
    speedSelector_->setCurrentIndex(2);
    speedSelector_->setFixedHeight(36);
    speedSelector_->setMinimumWidth(44);
    // Leave room for the longest rate at the selected text size, including 0.25×.
    speedSelector_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    UiZoom::setStyle(
        speedSelector_,
        QStringLiteral("QComboBox {color:#f4f4f4;background:transparent;border:0;border-radius:5px;padding:4px "
                       "8px;font-size:@detail;}"
                       "QComboBox:hover {background:#2b2b2b;} QComboBox::drop-down {border:0;width:0;}"
                       "QComboBox QAbstractItemView {color:#f4f4f4;background:#202020;border:1px solid #414141;"
                       "selection-background-color:#3a3a3a;padding:4px;}"));
    row->addWidget(speedSelector_);
    volumeButton_ = new QToolButton(panel);
    volumeButton_->setObjectName(QStringLiteral("ffplayVolumeButton"));
    volumeButton_->setFixedSize(36, 36);
    volumeButton_->setStyleSheet(flatButton);
    volumeButton_->setIconSize(QSize(24, 24));
    volumeButton_->setPopupMode(QToolButton::InstantPopup);
    auto* volumeMenu = new MaiVideoVolumePopup(volumeButton_);
    volumeSlider_ = volumeMenu->slider();
    muteAction_ = volumeMenu->muteAction();
    volumeButton_->setMenu(volumeMenu);
    row->addWidget(volumeButton_);
    auto* fullscreen = button(panel, "ffplayFullscreenButton", "fullscreen", QStringLiteral("进入全屏"));
    row->addWidget(fullscreen);
    panelLayout->addLayout(row);
    errorLabel_ = new QLabel(this);
    errorLabel_->setObjectName(QStringLiteral("ffplayErrorLabel"));
    errorLabel_->setAttribute(Qt::WA_NativeWindow);
    errorLabel_->setAlignment(Qt::AlignCenter);
    errorLabel_->setWordWrap(true);
    errorLabel_->setStyleSheet(QStringLiteral("color:#ffb4b4;background:#191919;padding:12px;"));
    errorLabel_->hide();
    bufferingIndicator_ = new MaiVideoBufferingIndicator(this);
    progressTimer_ = new QTimer(this);
    progressTimer_->setInterval(250);
    connect(progressTimer_, &QTimer::timeout, this, [this] {
#if defined(Q_OS_MAC)
        if (macPlayback_ && macPlayback_->failed()) {
            showPlaybackError(QStringLiteral("无法播放该视频，请检查文件或网络连接。"));
            playing_ = false;
            updatePlayButton();
            return;
        }
        if (macPlayback_ && macPlayback_->ready() && !initialVideoSizeQueued_.load()) {
            const QSize videoSize = macPlayback_->videoSize();
            if (videoSize.isValid() && !initialVideoSizeQueued_.exchange(true))
                setSize(this, videoSize.width(), videoSize.height());
        }
#endif
        // 两种正式播放路径共用 HUD；音量状态必须来自当前实际使用的播放器。
        syncAudioControls();
        if (cloudSource_ && hasPresentedFrame()) setBuffering(false);
        MaiFfplayPlaybackStatus status{};
        if (!playbackStatus(&status) || status.duration_us <= 0) return;
        if (pauseAfterFirstFrame_) {
            // Graphics 的成功回执来自本次启动（startPlayback 会清零计数）。
            // 让 FFplay 先建立真实视频时钟，旧表面像素不算首帧就绪证据。
            if (status.paused) {
                pauseAfterFirstFrame_ = false;
                firstFramePauseQueued_ = false;
            } else if (!firstFramePauseQueued_ && sPresentedFrames.load() > 0 && !status.seeking &&
                       maiFfplaySendCommand("pause") == 1) {
                // 入队后仍保持暂停意图，直到核心回执；不能把未消费命令当作已生效。
                firstFramePauseQueued_ = true;
            }
            playing_ = false;
        } else if (!status.seeking) {
            playing_ = status.paused == 0;
        }
        // 松手后 seek 仍可能在等待云盘读取/解码，旧时钟不能拉回用户选中的滑块。
        const auto position =
            std::clamp(status.seeking ? status.seek_target_us : status.position_us, int64_t{0}, status.duration_us);
        if (!seekSlider_->isSliderDown()) {
            const int range =
                static_cast<int>(std::clamp<int64_t>(status.duration_us / 1000, 1, std::numeric_limits<int>::max()));
            seekSlider_->setRange(0, range);
            seekSlider_->setSingleStep(std::max(1, static_cast<int>(range * 1'000'000.0 / status.duration_us)));
            seekSlider_->setPageStep(std::max(1, static_cast<int>(range * 5'000'000.0 / status.duration_us)));
            seekSlider_->setValue(static_cast<int>(position * double(range) / status.duration_us));
        }
        const auto clockText = [](int64_t microseconds) {
            const int64_t seconds = microseconds / 1000000;
            return QStringLiteral("%1:%2")
                .arg(static_cast<qlonglong>(seconds / 60))
                .arg(static_cast<qlonglong>(seconds % 60), 2, 10, QLatin1Char('0'));
        };
        timeLabel_->setText(clockText(position));
        durationLabel_->setText(clockText(status.duration_us));
        timeLabel_->setToolTip(clockText(position) + QStringLiteral(" / ") + clockText(status.duration_us));
        if (controlsSeeking_ != (status.seeking != 0)) {
            controlsSeeking_ = status.seeking != 0;
            revealControls();
        }
        updatePlayButton();
    });
    connect(pauseButton_, &QPushButton::clicked, this, [this] { sendCommand(QStringLiteral("pause")); });
    connect(seekSlider_, &QSlider::sliderReleased, this, [this] {
        if (seekSlider_->maximum() > 0) seekPercent(seekSlider_->sliderPosition() / double(seekSlider_->maximum()));
    });
    connect(seekSlider_, &QSlider::actionTriggered, this, [this](int) {
        // 键盘步进也进入同一精确 seek；鼠标拖动只在释放时提交最终落点。
        if (!seekSlider_->isSliderDown() && seekSlider_->maximum() > 0)
            seekPercent(seekSlider_->sliderPosition() / double(seekSlider_->maximum()));
    });
    connect(speedSelector_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index >= 0) setSpeed(speedSelector_->itemData(index).toDouble());
    });

    connect(nextButton_, &QPushButton::clicked, this, [this] { navigateVideo(1); });
    connect(fullscreen, &QPushButton::clicked, this, &MaiFfplayVideoDialog::toggleFullscreen);
    connect(volumeSlider_, &QSlider::valueChanged, this, [this](int value) {
        volumePercent_ = value;
        volumeIntentRevision_ = ++audioIntentRevision_;
        volumeRequestPending_ = true;
        volumeRequestReceipt_ = 0;
        if (value > 0 && audioMuted_) {
            audioMuted_ = false;
            muteIntentRevision_ = audioIntentRevision_;
            muteRequestPending_ = true;
            muteRequestReceipt_ = 0;
        }
        syncAudioControls();
    });
    connect(muteAction_, &QAction::triggered, this, [this](bool muted) {
        audioMuted_ = muted;
        muteIntentRevision_ = ++audioIntentRevision_;
        muteRequestPending_ = true;
        muteRequestReceipt_ = 0;
        syncAudioControls();
    });
    connect(this, &QWidget::windowTitleChanged, this, [this] { updateWindowChrome(); });
    controlsHideTimer_ = new QTimer(this);
    controlsHideTimer_->setSingleShot(true);
    controlsHideTimer_->setInterval(2500);
    connect(controlsHideTimer_, &QTimer::timeout, this, [this, panel] {
        if (!isFullScreen() || !playing_) return;
        if (seekSlider_->isSliderDown() || controlsSeeking_ || panel->underMouse() || titleBar_->underMouse() ||
            QApplication::activePopupWidget() || errorLabel_->isVisible() || !hasPresentedFrame()) {
            controlsHideTimer_->start();
            return;
        }
        panel->hide();
        controlsVisible_ = false;
        overlay_->hide();
        titleBar_->hide();
        setCursor(Qt::BlankCursor);
        surface_->setCursor(Qt::BlankCursor);
    });
    setMouseTracking(true);
    installEventFilter(this);
    for (auto* child : findChildren<QWidget*>()) {
        child->setMouseTracking(true);
        child->installEventFilter(this);
    }
    connect(qApp, &QGuiApplication::focusWindowChanged, this, [this] {
        QTimer::singleShot(0, this, [this] {
            // Mac Tool 面板可在同一 App 内浮在其它窗口上方；只跟随本播放器窗口组。
            // 临时无 focus 的全屏切换不改变状态，避免原生状态变更过程中的一帧显隐。
            QWidget* active = QApplication::activeWindow();
            if (!active) return;
            bool owned = false;
            for (QObject* item = active; item; item = item->parent())
                if (item == this) {
                    owned = true;
                    break;
                }
            const bool wasOwned = hudOwnerActive_;
            hudOwnerActive_ = owned;
            if (owned && !wasOwned)
                revealControls();
            else if (!owned) {
                titleBar_->hide();
                overlay_->hide();
            }
        });
    });
    updateControlLayout();
    updatePlayButton();
    syncAudioControls();
}

MaiFfplayVideoDialog::MaiFfplayVideoDialog(const QString& displayName, std::shared_ptr<MaiCloudVod> source,
                                           QWidget* parent)
    : MaiFfplayVideoDialog(displayName, parent, 0) {
    cloudSource_ = std::move(source);
    setWindowTitle(QFileInfo(displayName).fileName());
    if (auto* more = findChild<QWidget*>(QStringLiteral("videoMoreButton"))) more->hide();
    updateControlLayout();
}

void MaiFfplayVideoDialog::updateControlLayout() {
    if (!overlay_ || !titleBar_ || !surface_) return;
    const int controlsHeight = 84;
    // 普通窗口与全屏均覆盖完整视频，控件显隐不改变画面比例或引入固定黑色 footer。
    const bool shown = isVisible() && !isMinimized() && hudOwnerActive_ && (!isFullScreen() || controlsVisible_);
    const QPoint origin = mapToGlobal(QPoint());
    titleBar_->setGeometry(QRect(origin, QSize(width(), 46)));
    titleBar_->setStyleSheet(
        QStringLiteral("QWidget#ffplayTitleBar {background:qlineargradient(x1:0,y1:0,x2:0,y2:1,"
                       "stop:0 rgba(0,0,0,210),stop:1 rgba(0,0,0,15));}"));
    // 保持 Graphics 自己的 fit 规则：任意窗口比例下只保留必要的 letterbox，不裁剪或拉伸视频。
    surface_->setGeometry(rect());
    overlay_->setGeometry(QRect(origin + QPoint(0, height() - controlsHeight), QSize(width(), controlsHeight)));
    overlay_->setStyleSheet(QStringLiteral("QWidget#ffplayOverlay {background:transparent;}"));
    if (auto* panel = findChild<QWidget*>(QStringLiteral("ffplayControlPanel")))
        panel->setStyleSheet(
            QStringLiteral("QWidget#ffplayControlPanel {background:qlineargradient(x1:0,y1:0,x2:0,y2:1,"
                           "stop:0 rgba(0,0,0,0),stop:1 rgba(0,0,0,190));}"));
    titleBar_->setVisible(shown);
    overlay_->setVisible(shown);
#if defined(Q_OS_MAC)
    // 保留两个 Tool 窗口和焦点关系；只在 AppKit 转场中令其不可见，避免旧尺寸
    // HUD 叠在窗口转场快照上。不能 orderOut，否则会打断原生 Space 的窗口组转场。
    const qreal opacity = nativeFullscreenTransition_ ? 0.0 : 1.0;
    titleBar_->setWindowOpacity(opacity);
    overlay_->setWindowOpacity(opacity);
#endif
    // 原生 owner 关系保证 HUD 位于播放器之上；不反复 raise，否则会把 Popup 压回 owner 后面。
    errorLabel_->setGeometry(40, std::max(0, (surface_->height() - 64) / 2), std::max(1, width() - 80), 64);
    if (errorLabel_->isVisible()) errorLabel_->raise();
    bufferingIndicator_->move(surface_->rect().center() - bufferingIndicator_->rect().center());
    if (bufferingIndicator_->isVisible()) bufferingIndicator_->raise();
    if (auto* more = findChild<QWidget*>(QStringLiteral("videoMoreButton"))) more->setVisible(!cloudSource_);
    if (auto* divider = findChild<QWidget*>(QStringLiteral("ffplayTitleDivider"))) divider->setVisible(!cloudSource_);
    updateWindowChrome();
}

void MaiFfplayVideoDialog::updateWindowChrome() {
    if (titleLabel_) {
        titleLabel_->setText(windowTitle());
        titleLabel_->setToolTip(windowTitle());
    }
    if (maximizeButton_) {
        bool expanded = isMaximized();
#if defined(Q_OS_MAC)
        // AppKit 的全屏与最大化互斥；全屏下右上角仍提供“还原窗口”。
        expanded = expanded || isFullScreen();
#endif
        maximizeButton_->setIcon(videoControlIcon(expanded ? "restore" : "maximize"));
        maximizeButton_->setToolTip(expanded ? QStringLiteral("还原窗口") : QStringLiteral("最大化"));
        maximizeButton_->setAccessibleName(maximizeButton_->toolTip());
    }
    if (auto* fullscreen = findChild<QPushButton*>(QStringLiteral("ffplayFullscreenButton"))) {
        fullscreen->setIcon(videoControlIcon(isFullScreen() ? "fullscreen-exit" : "fullscreen"));
        fullscreen->setToolTip(isFullScreen() ? QStringLiteral("退出全屏") : QStringLiteral("进入全屏"));
        fullscreen->setAccessibleName(fullscreen->toolTip());
    }
}

void MaiFfplayVideoDialog::setBuffering(bool buffering) {
    // 等待首帧是正常加载状态；只显示无障碍可识别的图标，不占据一条错误横幅。
    bufferingIndicator_->setVisible(buffering);
    if (buffering) bufferingIndicator_->raise();
}

void MaiFfplayVideoDialog::showPlaybackError(const QString& message) {
    setBuffering(false);
    errorLabel_->setText(message);
    errorLabel_->show();
    errorLabel_->raise();
}

void MaiFfplayVideoDialog::syncAudioControls() {
    MaiFfplayAudioStatus status{sizeof(MaiFfplayAudioStatus), 100, 0};
#if defined(Q_OS_MAC)
    if (macPlayback_) {
        if (volumeRequestPending_) macPlayback_->setVolumePercent(volumePercent_);
        if (muteRequestPending_) macPlayback_->setMuted(audioMuted_);
        if (macPlayback_->audioStatus(&volumePercent_, &audioMuted_)) {
            volumeRequestPending_ = muteRequestPending_ = false;
            volumeRequestReceipt_ = muteRequestReceipt_ = 0;
        }
    } else if (!macPlaybackRequested_)
#endif
        if (started_ && maiFfplayGetAudioStatus(&status) == 1) {
            const uint64_t consumed = audioStartupConsumedRevision_.load();
            if (volumeRequestPending_ && consumed >= volumeIntentRevision_) volumeRequestPending_ = false;
            if (muteRequestPending_ && consumed >= muteIntentRevision_) muteRequestPending_ = false;
            if (volumeRequestPending_ && !volumeRequestReceipt_)
                maiFfplaySetVolumePercentWithReceipt(volumePercent_, &volumeRequestReceipt_);
            if (muteRequestPending_ && !muteRequestReceipt_)
                maiFfplaySetMutedWithReceipt(audioMuted_ ? 1 : 0, &muteRequestReceipt_);
            if (volumeRequestReceipt_ && status.applied_serial >= volumeRequestReceipt_) volumeRequestPending_ = false;
            if (muteRequestReceipt_ && status.applied_serial >= muteRequestReceipt_) muteRequestPending_ = false;
            if (!volumeRequestPending_) volumePercent_ = status.volume_percent;
            if (!muteRequestPending_) audioMuted_ = status.muted != 0;
        }
    audioStartupSettings_ = (audioIntentRevision_ << 8) | (volumePercent_ * 2 + (audioMuted_ ? 1 : 0));
    if (volumeSlider_ && !volumeSlider_->isSliderDown()) {
        const QSignalBlocker blocker(volumeSlider_);
        volumeSlider_->setValue(volumePercent_);
    }
    if (muteAction_) {
        const QSignalBlocker blocker(muteAction_);
        muteAction_->setChecked(audioMuted_);
    }
    if (volumeButton_) {
        // 核心回显在 QSignalBlocker 内同步 slider/action，百分比和可达静音状态也需显式刷新。
        if (auto* menu = volumeButton_->menu()) static_cast<MaiVideoVolumePopup*>(menu)->refreshReadout();
        volumeButton_->setIcon(videoControlIcon(audioMuted_ || volumePercent_ == 0 ? "muted" : "volume"));
        volumeButton_->setToolTip(audioMuted_ ? QStringLiteral("已静音")
                                              : QStringLiteral("音量 %1%").arg(volumePercent_));
        volumeButton_->setAccessibleName(volumeButton_->toolTip());
    }
}

void MaiFfplayVideoDialog::setForwardAction(std::function<void(const QString&)> action) {
    forwardAction_ = std::move(action);
}

void MaiFfplayVideoDialog::updatePlayButton() {
    if (controlsPlaying_ != playing_) {
        controlsPlaying_ = playing_;
        revealControls();
    }
    pauseButton_->setText({});
    pauseButton_->setIcon(videoControlIcon(playing_ ? "pause" : "play"));
    pauseButton_->setIconSize(QSize(24, 24));
    pauseButton_->setToolTip(playing_ ? QStringLiteral("暂停视频") : QStringLiteral("播放视频"));
    pauseButton_->setAccessibleName(pauseButton_->toolTip());
}

void MaiFfplayVideoDialog::revealControls() {
    controlsVisible_ = true;
    const bool shown = isVisible() && !isMinimized() && hudOwnerActive_;
    if (titleBar_) {
        titleBar_->setVisible(shown);
    }
    if (auto* panel = findChild<QWidget*>(QStringLiteral("ffplayControlPanel"))) panel->show();
    if (overlay_) {
        overlay_->setVisible(shown);
    }
    for (QWidget* widget : {static_cast<QWidget*>(this), surface_, titleBar_, overlay_})
        if (widget) widget->unsetCursor();
    scheduleControlHide();
}

void MaiFfplayVideoDialog::scheduleControlHide() {
    if (!controlsHideTimer_) return;
    controlsHideTimer_->stop();
    if (isFullScreen() && playing_) controlsHideTimer_->start();
}

void MaiFfplayVideoDialog::toggleFullscreen() {
#if defined(Q_OS_MAC)
    if (closeAfterFullscreen_) return;
    if (fullscreenObserver_ && (fullscreenRequestPending_ || nativeFullscreenTransition_)) {
        applyFullscreen(!requestedFullscreen_);
        return;
    }
#endif
    applyFullscreen(!isFullScreen());
}

void MaiFfplayVideoDialog::applyFullscreen(bool enabled) {
#if defined(Q_OS_MAC)
    if (closeAfterFullscreen_ && enabled) return;
    if (fullscreenObserver_) {
        const bool previous = fullscreenRequestPending_ || nativeFullscreenTransition_ ? requestedFullscreen_ : isFullScreen();
        if (enabled && !previous)
            windowWasMaximized_ = hasPendingWindowedState_ ? pendingWindowedMaximized_ : isMaximized();
        if (enabled) {
            hasPendingWindowedState_ = false;
            minimizeAfterFullscreen_ = false;
        }
        requestedFullscreen_ = enabled;
        fullscreenRequestPending_ = true;
        applyMacFullscreenRequest();
        return;
    }
#endif
    if (enabled == isFullScreen()) return;
    // GUI、键盘、工具及底层宿主回调共同维护进入前的窗口状态。
    if (!enabled) {
        if (windowWasMaximized_)
            showMaximized();
        else
            showNormal();
    } else {
        windowWasMaximized_ = isMaximized();
        showFullScreen();
    }
    updateControlLayout();
    revealControls();
}

void MaiFfplayVideoDialog::toggleMaximized() {
#if defined(Q_OS_MAC)
    if (closeAfterFullscreen_) return;
    minimizeAfterFullscreen_ = false;
    if (fullscreenObserver_ && (isFullScreen() || fullscreenRequestPending_ || nativeFullscreenTransition_)) {
        // 全屏时 isMaximized() 为 false，不能据此排队“最大化”，否则 DidExit
        // 后会再次铺满。若 Esc 等已请求退出，则这次点击仍是窗口态的正常最大化操作。
        const bool fullscreen = fullscreenRequestPending_ || nativeFullscreenTransition_
                                    ? requestedFullscreen_ : isFullScreen();
        pendingWindowedMaximized_ = hasPendingWindowedState_ ? !pendingWindowedMaximized_
                                                           : (!fullscreen && !isMaximized());
        hasPendingWindowedState_ = true;
        applyFullscreen(false);
        return;
    }
#endif
    isMaximized() ? showNormal() : showMaximized();
}

void MaiFfplayVideoDialog::minimizeWindow() {
#if defined(Q_OS_MAC)
    if (closeAfterFullscreen_) return;
    if (fullscreenObserver_ && (isFullScreen() || nativeFullscreenTransition_ || fullscreenRequestPending_)) {
        // 最小化也必须排到 DidExit 之后，不能反向打断 AppKit 的退出动画。
        minimizeAfterFullscreen_ = true;
        hasPendingWindowedState_ = false;
        applyFullscreen(false);
        return;
    }
#endif
    showMinimized();
}

#if defined(Q_OS_MAC)
void MaiFfplayVideoDialog::applyMacFullscreenRequest() {
    if (!fullscreenRequestPending_ || nativeFullscreenTransition_ || !isVisible()) return;
    if (requestedFullscreen_ == isFullScreen()) {
        fullscreenRequestPending_ = false;
        if (!requestedFullscreen_ && closeAfterFullscreen_) {
            // 直到 DidExit 之后才允许 Qt 真正关闭/删除窗口，保留转场期间的原生宿主。
            closeAfterFullscreen_ = false;
            QMetaObject::invokeMethod(this, [this] { close(); }, Qt::QueuedConnection);
            return;
        }
        if (!requestedFullscreen_ && minimizeAfterFullscreen_) {
            minimizeAfterFullscreen_ = false;
            showMinimized();
            return;
        }
        if (!requestedFullscreen_ && hasPendingWindowedState_) {
            const bool maximized = pendingWindowedMaximized_;
            hasPendingWindowedState_ = false;
            maximized ? showMaximized() : showNormal();
        }
        updateControlLayout();
        revealControls();
        return;
    }
    // Qt 的逻辑状态可能先改变；完成条件来自原生 DidEnter/DidExit 通知。
    nativeFullscreenTransition_ = true;
    movingFrame_ = false;
    resizingFrame_ = {};
    if (QWidget::mouseGrabber() == this) releaseMouse();
    updateControlLayout();
    if (requestedFullscreen_) showFullScreen();
    else if (hasPendingWindowedState_ ? pendingWindowedMaximized_ : windowWasMaximized_) showMaximized();
    else showNormal();
}

void MaiFfplayVideoDialog::onMacFullscreenPhase(MaiMacFullscreenPhase phase) {
    if (qEnvironmentVariableIntValue("MAICHAT_TRACE_VIDEO_CONTROLS") == 1)
        qInfo() << "[native-fullscreen]" << int(phase) << "requested" << requestedFullscreen_
                << "pending" << fullscreenRequestPending_ << "qt-fullscreen" << isFullScreen();
    const bool entering = phase == MaiMacFullscreenPhase::WillEnter || phase == MaiMacFullscreenPhase::DidEnter;
    if (phase == MaiMacFullscreenPhase::WillEnter || phase == MaiMacFullscreenPhase::WillExit) {
        nativeFullscreenTransition_ = true;
        // 系统快捷键等外部切换也由 Qt 处理，不能被上一轮已完成的意图撤销。
        if (!fullscreenRequestPending_) requestedFullscreen_ = entering;
        updateControlLayout();
        return;
    }
    nativeFullscreenTransition_ = false;
    if (!fullscreenRequestPending_) requestedFullscreen_ = entering;
    // 等 Qt 的原生 delegate 也完成本次状态同步，再处理最后的排队意图。
    QMetaObject::invokeMethod(this, [this] {
        if (!isVisible()) return;
        applyMacFullscreenRequest();
        updateControlLayout();
        revealControls();
    }, Qt::QueuedConnection);
}
#endif

void MaiFfplayVideoDialog::forwardVideo() {
    if (cloudSource_) {
        QMessageBox::information(this, QStringLiteral("云盘视频"), QStringLiteral("请在云盘列表下载视频后转发。"));
        return;
    }
    if (forwardAction_) {
        // 接收方选择器可运行嵌套事件循环，先持有调用数据，避免 owner 被关闭后悬空。
        const auto action = forwardAction_;
        const QString path = path_;
        action(path);
        return;
    }
    auto* media = new QMimeData();
    media->setUrls({QUrl::fromLocalFile(path_)});
    QApplication::clipboard()->setMimeData(media);
    QMessageBox::information(this, QStringLiteral("视频已复制"), QStringLiteral("可在支持粘贴文件的应用中转发。"));
}

void MaiFfplayVideoDialog::saveCopy() {
    if (cloudSource_) {
        QMessageBox::information(this, QStringLiteral("云盘视频"), QStringLiteral("请使用云盘列表的视频下载按钮。"));
        return;
    }
    const QPointer<MaiFfplayVideoDialog> alive(this);
    const QString sourcePath = path_;
    QPointer<QFileDialog> picker = new QFileDialog(this, QStringLiteral("保存视频"), QString(),
                                                   QStringLiteral("视频文件 (*.mp4 *.mov *.m4v);;所有文件 (*)"));
    picker->setAcceptMode(QFileDialog::AcceptSave);
    picker->setFileMode(QFileDialog::AnyFile);
    picker->selectFile(QFileInfo(sourcePath).fileName());
    const int result = picker->exec();
    if (!picker) return;
    const QString target = result == QDialog::Accepted ? picker->selectedFiles().value(0) : QString();
    picker->deleteLater();
    // 账号守卫可能在选择器内关闭播放器；取消后不继续访问成员或写入文件。
    if (!alive || !alive->isVisible()) return;
    if (target.isEmpty()) return;
    if (QFileInfo(target).absoluteFilePath() == QFileInfo(sourcePath).absoluteFilePath()) return;
    QFile source(sourcePath);
    QSaveFile output(target);
    if (!source.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), QStringLiteral("无法读取或写入视频文件。"));
        return;
    }
    while (!source.atEnd()) {
        const QByteArray bytes = source.read(1024 * 1024);
        if (bytes.isEmpty() || output.write(bytes) != bytes.size()) {
            output.cancelWriting();
            QMessageBox::warning(this, QStringLiteral("保存失败"), QStringLiteral("复制视频时发生错误。"));
            return;
        }
    }
    if (!output.commit()) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), QStringLiteral("无法完成视频保存。"));
        return;
    }
    QMessageBox::information(this, QStringLiteral("已保存"), QStringLiteral("视频副本已保存。"));
}

MaiFfplayVideoDialog::~MaiFfplayVideoDialog() {
#if defined(Q_OS_MAC)
    maiReleaseMacVideoFullscreenObserver(fullscreenObserver_);
    fullscreenObserver_ = nullptr;
#endif
    stopPlayback(true);
}

QPointer<MaiFfplayVideoDialog> MaiFfplayVideoDialog::activeDialog() {
    return sActiveDialog;
}

uint64_t MaiFfplayVideoDialog::presentedFrames() const {
    return sPresentedFrames.load();
}

bool MaiFfplayVideoDialog::hasPresentedFrame() const {
#if defined(Q_OS_MAC)
    if (macPlaybackRequested_) return macPlayback_ && macPlayback_->ready();
#endif
    return presentedFrames() > 0;
}

bool MaiFfplayVideoDialog::playbackStatus(MaiFfplayPlaybackStatus* output) const {
#if defined(Q_OS_MAC)
    if (macPlaybackRequested_) return macPlayback_ && macPlayback_->status(output);
#endif
    return maiFfplayGetPlaybackStatus(output) != 0;
}

bool MaiFfplayVideoDialog::hasPresentedHdrFrame() const {
#if defined(Q_OS_MAC)
    if (macPlaybackRequested_) return macPlayback_ && macPlayback_->ready();
#endif
    return presentedHdrFrame_.load();
}

bool MaiFfplayVideoDialog::presentVideo(void* userData, const MaiVideoFrame* frame, const MaiVideoSubtitle* subtitle) {
    auto* dialog = static_cast<MaiFfplayVideoDialog*>(userData);
    // 与 iOS 共用系统 HLG 显示：保留拍摄环境 AMVE，让系统按实际显示器应用 OOTF。
    const bool hlg = frame && dialog->sourceInfo_.hlg && frame->format == MAI_VIDEO_PIXEL_P010 &&
                     frame->color_space == MAI_VIDEO_COLOR_BT2020 && frame->color_transfer == MAI_VIDEO_TRANSFER_HLG;
    if (hlg && !dialog->hlgOutputRequested_.exchange(true)) {
        const bool ambient = dialog->sourceInfo_.has_ambient != 0;
        if (!maiGraphicsPresenterSetHlgOutput(dialog->viewId_, true, ambient ? dialog->sourceInfo_.ambient : nullptr,
                                              ambient ? 8 : 0))
            dialog->hlgOutputRequested_ = false;
    } else if (!hlg && dialog->hlgOutputRequested_.exchange(false)) {
        maiGraphicsPresenterSetHlgOutput(dialog->viewId_, false, nullptr, 0);
    }
    const bool shown = maiGraphicsPresenterShowVideoFrameWithSubtitle(dialog->viewId_, frame, subtitle, false);
    // SDL 的初始 SIZE_CHANGED 可能使 video_open 不再调用 host.set_size；直接取首个被接受的
    // 视频帧尺寸作为可靠默认比例。只跨线程排队一次，不在每帧或换速时重新调整用户窗口。
    if (shown && frame && frame->width > 0 && frame->height > 0 &&
        frame->width <= uint32_t(std::numeric_limits<int>::max()) &&
        frame->height <= uint32_t(std::numeric_limits<int>::max()) && !dialog->initialVideoSizeQueued_.exchange(true))
        setSize(dialog, int(frame->width), int(frame->height));
    if (shown && frame && frame->color_transfer != MAI_VIDEO_TRANSFER_SDR) dialog->presentedHdrFrame_ = true;
    return shown;
}

bool MaiFfplayVideoDialog::presentRgba(void* userData, const uint8_t* pixels, uint32_t width, uint32_t height,
                                       uint32_t stride) {
    auto* dialog = static_cast<MaiFfplayVideoDialog*>(userData);
    if (dialog->hlgOutputRequested_.exchange(false))
        maiGraphicsPresenterSetHlgOutput(dialog->viewId_, false, nullptr, 0);
    return maiGraphicsPresenterShowFrame(dialog->viewId_, pixels, width, height, stride, false);
}

int MaiFfplayVideoDialog::readCloudInput(void* userData, uint64_t offset, uint8_t* output, int capacity) {
    auto* dialog = static_cast<MaiFfplayVideoDialog*>(userData);
    if (!dialog || !dialog->cloudSource_ || !output || capacity <= 0) return -1;
    size_t bytes = 0;
    MaiCloudVodResult result{};
    return mai_cloud_vod_read_at(dialog->cloudSource_.get(), offset, output, size_t(capacity), &bytes, &result)
               ? int(bytes)
               : -1;
}

void MaiFfplayVideoDialog::setTitle(void* userData, const char* title) {
    auto* dialog = static_cast<MaiFfplayVideoDialog*>(userData);
    Q_UNUSED(title);
    const QString value = QFileInfo(dialog->path_).fileName();
    QMetaObject::invokeMethod(dialog, [dialog, value] { dialog->setWindowTitle(value); }, Qt::QueuedConnection);
}

void MaiFfplayVideoDialog::setSize(void* userData, int width, int height) {
    auto* dialog = static_cast<MaiFfplayVideoDialog*>(userData);
    QMetaObject::invokeMethod(
        dialog,
        [dialog, width, height] {
            if (width <= 0 || height <= 0 || dialog->initialVideoSizeApplied_) return;
            dialog->initialVideoSizeApplied_ = true;
            if (dialog->docked_ || dialog->isFullScreen() || dialog->isMaximized() || dialog->windowGeometryCustomized_)
                return;
#if defined(Q_OS_MAC)
            if (dialog->nativeFullscreenTransition_ || dialog->fullscreenRequestPending_) return;
#endif
            const QScreen* screen = dialog->screen();
            if (!screen) screen = QGuiApplication::primaryScreen();
            const QRect available = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 800);
            const QSize limit = available.size() * 0.85;
            // 用同一个比例因子满足普通素材的最小尺寸与屏幕上限，不分别拉宽/拉高源比例。
            // 极端长宽比受控件最小宽高约束时仍由 Presenter fit，绝不裁剪或变形内容。
            const double minimumScale =
                std::max(double(dialog->minimumWidth()) / width, double(dialog->minimumHeight()) / height);
            const double maximumScale = std::min(double(limit.width()) / width, double(limit.height()) / height);
            const double scale = std::min(std::max(1.0, minimumScale), maximumScale);
            const QSize size(qRound(width * scale), qRound(height * scale));
            dialog->resize(size.expandedTo(dialog->minimumSize()));
            dialog->move(available.center() - QPoint(dialog->width() / 2, dialog->height() / 2));
        },
        Qt::QueuedConnection);
}

void MaiFfplayVideoDialog::setPosition(void* userData, int x, int y) {
    auto* dialog = static_cast<MaiFfplayVideoDialog*>(userData);
    QMetaObject::invokeMethod(
        dialog,
        [dialog, x, y] {
            if (dialog->docked_ || dialog->isFullScreen() || dialog->isMaximized() || dialog->initialPositionApplied_ ||
                dialog->windowGeometryCustomized_)
                return;
#if defined(Q_OS_MAC)
            if (dialog->nativeFullscreenTransition_ || dialog->fullscreenRequestPending_) return;
#endif
            dialog->initialPositionApplied_ = true;
            // 兼容层 SDL_WINDOWPOS_CENTERED/UNDEFINED 都为 (0,0)；默认位置由首次定尺寸居中。
            // 后续重播/切速不能把用户已移动的窗口重新移到屏幕原点。
            if (x != 0 || y != 0) dialog->move(x, y);
        },
        Qt::QueuedConnection);
}

void MaiFfplayVideoDialog::setFullscreen(void* userData, int enabled) {
    auto* dialog = static_cast<MaiFfplayVideoDialog*>(userData);
    QMetaObject::invokeMethod(
        dialog, [dialog, enabled] { dialog->applyFullscreen(enabled != 0); }, Qt::QueuedConnection);
}

void MaiFfplayVideoDialog::showWindow(void* userData) {
    // 宿主在 showEvent 中创建播放会话；解码线程的 SDL_ShowWindow 不能重新排列
    // owner/HUD/Popup 的 Z 序，也不能把用户切走的播放器抢回其它窗口上方。
    Q_UNUSED(userData);
}

// 作用：验证输入并启动对应的正式播放路径，同时同步共用播放控件。
bool MaiFfplayVideoDialog::startPlayback() {
    if (started_ || (cloudSource_ ? mai_cloud_vod_size(cloudSource_.get()) == 0 : !QFileInfo(path_).isFile())) {
        qWarning() << "FFplay popup has no readable video file:" << path_;
        return false;
    }
    errorLabel_->hide();
    setBuffering(bool(cloudSource_));
    DesktopAudioPlayback::prepareVideoPlayback();
    DesktopMusicPlayerDialog::closeActive();
    if (!sActiveDialog.isNull() && sActiveDialog != this) sActiveDialog->close();
    const QString backend = bundledResource(QStringLiteral("/../Frameworks/libmaiagent_obs_metal.so"));
    const QString effects = bundledResource(QStringLiteral("/../Resources/MaiAgentGraphics"));
    if (!QFileInfo::exists(backend) || !QFileInfo::exists(effects + QStringLiteral("/default.effect"))) {
        qWarning() << "FFplay popup is missing Graphics resources:" << backend << effects;
        return false;
    }
    if (sPresenterUsers == 0) {
        const QByteArray modulePath = backend.toUtf8();
        const QByteArray effectPath = effects.toUtf8();
        if (!maiGraphicsPresenterStart(modulePath.constData(), effectPath.constData(), onFramePresented, nullptr)) {
            qWarning() << "FFplay popup could not start Graphics:" << backend << effects;
            return false;
        }
    }
    ++sPresenterUsers;
#if defined(Q_OS_WIN)
    nativeView_ = reinterpret_cast<void*>(surface_->winId());
    if (nativeView_) {
        viewId_ = maiGraphicsPresenterAttach(nativeView_, std::max(surface_->width(), 1),
                                             std::max(surface_->height(), 1), nullptr, nullptr);
    }
#else
    nativeView_ = maiGraphicsCreateMacLayer(surface_);
    if (nativeView_) {
        viewId_ =
            maiGraphicsPresenterAttach(nativeView_, std::max(surface_->width(), 1), std::max(surface_->height(), 1),
                                       maiGraphicsRetainMacLayer, maiGraphicsReleaseMacLayer);
        maiGraphicsReleaseMacLayer(nativeView_);
    }
#endif
    if (!viewId_) {
        qWarning() << "FFplay popup could not attach its video surface:" << path_;
        --sPresenterUsers;
        if (sPresenterUsers == 0) maiGraphicsPresenterStop();
        return false;
    }
    host_.user_data = this;
    host_.graphics_view_id = viewId_;
    host_.present_video = presentVideo;
    host_.present_rgba = presentRgba;
    host_.set_title = setTitle;
    host_.set_size = setSize;
    host_.set_position = setPosition;
    host_.set_fullscreen = setFullscreen;
    host_.show_window = showWindow;
    host_.read_input_at = cloudSource_ ? readCloudInput : nullptr;
    host_.input_size_bytes = cloudSource_ ? mai_cloud_vod_size(cloudSource_.get()) : 0;
    const QByteArray sourcePath = cloudSource_ ? path_.toUtf8() : QFileInfo(path_).absoluteFilePath().toUtf8();
    sPresentedFrames = 0;
    started_ = true;
    progressTimer_->start();
    playbackFinished_ = false;
    stopRequested_ = false;
    presentedHdrFrame_ = false;
    hlgOutputRequested_ = false;
    sActiveDialog = this;
    const double speed = playbackSpeed_;
    const int64_t startPositionUs = startPositionUs_;
    audioStartupSettings_ = (audioIntentRevision_ << 8) | (volumePercent_ * 2 + (audioMuted_ ? 1 : 0));
    audioStartupConsumedRevision_ = 0;
#if defined(Q_OS_MAC)
    macPlaybackRequested_ = false;
    const uint64_t openGeneration = ++playbackOpenGeneration_;
#else
    const uint64_t openGeneration = 0;
#endif
    playbackThread_ = std::thread([this, sourcePath, speed, startPositionUs, openGeneration] {
        char name[] = "ffplay";
        char* arguments[] = {name, const_cast<char*>(sourcePath.constData())};
        int result = 0;
        const auto currentAudioOptions = [this] {
            const uint64_t snapshot = audioStartupSettings_.load();
            const int settings = snapshot & 255;
            audioStartupConsumedRevision_ = snapshot >> 8;
            return MaiFfplayAudioOptions{sizeof(MaiFfplayAudioOptions), settings / 2, settings % 2};
        };
#if defined(Q_OS_MAC)
        // SDL 兼容纹理不能表示 HLG/PQ。复用共享 HDR 探测：MP4/MOV 交给系统的
        // 逐帧显示映射，其余格式保留现有 FFplay 原生 HDR 帧路径。
        // 探测放在工作线程；两条路径的云盘读取都复用有限缓存的 Range 源。
        sourceInfo_ = {};
        const bool probed = cloudSource_ ? maiFfplayProbeHdrSourceFromHost(&host_, &sourceInfo_)
                                         : maiFfplayProbeHdrSource(sourcePath.constData(), &sourceInfo_);
        if (stopRequested_) {
            playbackFinished_ = true;
            return;
        }
        if (probed && sourceInfo_.native_frame_eligible && sourceInfo_.is_iso_base_media) {
            // 系统 HDR 路径保留 AVPlayerItem 的逐帧显示映射（含 Dolby Vision）。
            // 只把创建交给 GUI 线程；停止/换片先失效代际，避免 join 等待 GUI 而死锁。
            macPlaybackRequested_ = true;
            QMetaObject::invokeMethod(
                this,
                [this, openGeneration] {
                    if (openGeneration != playbackOpenGeneration_ || stopRequested_ || !started_) return;
                    macPlayback_.reset(new MaiMacVideoPlayback(surface_, path_, cloudSource_));
                    macPlayback_->setVolumePercent(volumePercent_);
                    macPlayback_->setMuted(audioMuted_);
                    volumeRequestPending_ = muteRequestPending_ = false;
                    volumeRequestReceipt_ = muteRequestReceipt_ = 0;
                    if (!macPlayback_->open(startPositionUs_, playbackSpeed_, playing_)) {
                        showPlaybackError(QStringLiteral("无法打开该 HDR 视频。"));
                    }
                },
                Qt::QueuedConnection);
            playbackFinished_ = true;
            return;
        }
        host_.present_native_frames = probed && sourceInfo_.native_frame_eligible;
        if (host_.present_native_frames) {
            char nativeRenderer[] = "-enable_vulkan";
            char hardware[] = "-hwaccel";
            char videoToolbox[] = "videotoolbox";
            char* nativeArguments[] = {name, nativeRenderer, hardware, videoToolbox,
                                       const_cast<char*>(sourcePath.constData())};
            MaiFfplayAudioOptions audio = currentAudioOptions();
            result = maiFfplayRunAtSpeedWithAudio(&host_, 5, nativeArguments, speed, startPositionUs, &audio);
            if (!stopRequested_ && sPresentedFrames == 0) {
                // 硬件解码失败时仍保留原生 HDR 帧，不回退到会丢色彩信息的 SDL 路径。
                char* softwareArguments[] = {name, nativeRenderer, const_cast<char*>(sourcePath.constData())};
                audio = currentAudioOptions();
                result = maiFfplayRunAtSpeedWithAudio(&host_, 3, softwareArguments, speed, startPositionUs, &audio);
            }
        } else
#endif
        {
            if (!stopRequested_) {
                const MaiFfplayAudioOptions audio = currentAudioOptions();
                result = maiFfplayRunAtSpeedWithAudio(&host_, 2, arguments, speed, startPositionUs, &audio);
            }
        }
        playbackFinished_ = true;
        QMetaObject::invokeMethod(
            this,
            [this, result] {
                if (result != 0) {
                    showPlaybackError(QStringLiteral("无法播放该视频（FFplay 状态 %1）").arg(result));
                }
            },
            Qt::QueuedConnection);
    });
    return true;
}

void MaiFfplayVideoDialog::stopPlayback(bool cancelCloudSource) {
    setBuffering(false);
    stopRequested_ = true;
    syncAudioControls();
    bool wasNativePlayback = false;
#if defined(Q_OS_MAC)
    wasNativePlayback = macPlaybackRequested_.load();
#endif
    pauseAfterFirstFrame_ = false;
    firstFramePauseQueued_ = false;
#if defined(Q_OS_MAC)
    ++playbackOpenGeneration_;
    macPlayback_.reset();
    macPlaybackRequested_ = false;
#endif
    if (!started_) return;
    syncAudioControls();
    MaiFfplayAudioStatus finalAudio{sizeof(MaiFfplayAudioStatus), 100, 0};
    // 仅在本窗口已运行核心时采集最终快照，避免 HDR 准备取消时借用上个会话。
    const bool hadRunningAudio = !wasNativePlayback && maiFfplayGetAudioStatus(&finalAudio) == 1;
    progressTimer_->stop();
    if (cloudSource_) {
        if (cancelCloudSource)
            mai_cloud_vod_cancel(cloudSource_.get());
        else
            mai_cloud_vod_interrupt_read(cloudSource_.get());
    }
    for (int attempt = 0; attempt < 100 && !playbackFinished_; ++attempt) {
        if (maiFfplaySendCommand("close") == 1) break;
        QThread::msleep(10);
    }
    if (playbackThread_.joinable()) playbackThread_.join();
    if (hadRunningAudio && maiFfplayGetAudioStatus(&finalAudio) >= 0) {
        // 音量/静音命令与 close 共用 FIFO；join 后所有先入队的设置已执行。
        // API 停止后仍保留最后快照，必须在此读取，不能用停止前的旧 GUI 缓存
        // 覆盖键盘、Agent 或普通核心调用方刚刚提交的实际设置。
        // 未入队的准备期意图仍需交给下一次启动；已入队的设置以最终真实状态为准。
        if (!volumeRequestPending_ || volumeRequestReceipt_) {
            volumePercent_ = finalAudio.volume_percent;
            volumeRequestPending_ = false;
            volumeRequestReceipt_ = 0;
        }
        if (!muteRequestPending_ || muteRequestReceipt_) {
            audioMuted_ = finalAudio.muted != 0;
            muteRequestPending_ = false;
            muteRequestReceipt_ = 0;
        }
        syncAudioControls();
    }
    if (viewId_) maiGraphicsPresenterDetach(viewId_);
    viewId_ = 0;
    if (--sPresenterUsers == 0) maiGraphicsPresenterStop();
    if (sActiveDialog == this) sActiveDialog = nullptr;
    started_ = false;
}

bool MaiFfplayVideoDialog::sendCommand(const QString& command) {
    // 暂停意图等待首帧期间，播放按钮的再次点击意味着恢复播放；
    // 核心此时已在准备首帧，取消待办即可，不能再切换成暂停。
    if (command == QStringLiteral("pause") && pauseAfterFirstFrame_ && !firstFramePauseQueued_) {
        pauseAfterFirstFrame_ = false;
        playing_ = true;
        updatePlayButton();
        return true;
    }
    // 窗口状态由 Qt 维护；GUI 与工具不能分别切换两个不同的全屏布尔值。
    if (command == QStringLiteral("fullscreen")) {
        if (!started_) return false;
        toggleFullscreen();
        return true;
    }
    const QByteArray utf8 = command.toUtf8();
    bool accepted = false;
#if defined(Q_OS_MAC)
    if (macPlayback_)
        accepted = macPlayback_->command(command);
    else
#endif
        accepted = maiFfplaySendCommand(utf8.constData()) == 1;
    if (accepted && command == QStringLiteral("pause")) {
        // 若恢复暂停的命令已经入队，再次点击需追加同一 FIFO 中的抵消切换。
        pauseAfterFirstFrame_ = false;
        firstFramePauseQueued_ = false;
#if defined(Q_OS_MAC)
        // 系统播放器立即更新意图；到尾后 GUI 轮询可能还没更新 playing_。
        // 使用同一状态源，避免此时点击重播把暂停状态翻转错。
        if (macPlayback_) {
            MaiFfplayPlaybackStatus status{};
            if (macPlayback_->status(&status)) playing_ = status.paused == 0;
        } else
#endif
            playing_ = !playing_;
        updatePlayButton();
    }
    if (accepted && (command.startsWith(QStringLiteral("seek_")) || command == QStringLiteral("step") ||
                     command == QStringLiteral("next_chapter") || command == QStringLiteral("previous_chapter")))
        ++navigationGeneration_;
    return accepted;
}

bool MaiFfplayVideoDialog::seekPercent(double fraction) {
    bool accepted = false;
#if defined(Q_OS_MAC)
    if (macPlayback_) {
        MaiFfplayPlaybackStatus status{};
        accepted = std::isfinite(fraction) && fraction >= 0 && fraction <= 1 && playbackStatus(&status) &&
                   status.duration_us > 0 && macPlayback_->seek(int64_t(std::llround(fraction * status.duration_us)));
    } else
#endif
        accepted = maiFfplaySeekPercent(fraction) == 1;
    if (accepted) ++navigationGeneration_;
    return accepted;
}

bool MaiFfplayVideoDialog::seekSeconds(double seconds) {
    MaiFfplayPlaybackStatus status{};
    if (!std::isfinite(seconds) || seconds < 0 || !playbackStatus(&status) || status.duration_us <= 0 ||
        seconds * 1'000'000 > status.duration_us)
        return false;
    const int64_t target = static_cast<int64_t>(std::llround(seconds * 1'000'000));
    bool accepted = false;
#if defined(Q_OS_MAC)
    if (macPlayback_)
        accepted = macPlayback_->seek(target);
    else
#endif
        accepted = maiFfplaySeekAbsoluteUs(target) == 1;
    if (accepted) ++navigationGeneration_;
    return accepted;
}

uint64_t MaiFfplayVideoDialog::navigationGeneration() const {
    return navigationGeneration_;
}

bool MaiFfplayVideoDialog::setSpeed(double speed) {
    if (speed != 0.25 && speed != 0.5 && speed != 1.0 && speed != 1.5 && speed != 2.0) return false;
    if (!started_) return false;
    if (speed == playbackSpeed_) return true;
#if defined(Q_OS_MAC)
    if (macPlayback_) {
        if (!macPlayback_->setSpeed(speed)) return false;
        playbackSpeed_ = speed;
        if (speedSelector_) {
            const QSignalBlocker blocked(speedSelector_);
            for (int index = 0; index < speedSelector_->count(); ++index)
                if (speedSelector_->itemData(index).toDouble() == speed) speedSelector_->setCurrentIndex(index);
        }
        return true;
    }
#endif
    MaiFfplayPlaybackStatus status{};
    // 首次 HDR 探测尚未启动 FFplay 时也接受倍速，按请求的源时间重建即可；
    // 不从尚不存在的播放时钟推断位置，也不把准备阶段当成播放失败。
    if (!playbackStatus(&status)) {
        if (playbackFinished_) return false;
        status.position_us = startPositionUs_;
    }
    const bool wasPlaying = playing_;
    const int64_t sourcePositionUs = std::max<int64_t>(
        0, status.seeking
               ? status.seek_target_us
               : (pauseAfterFirstFrame_ && sPresentedFrames.load() == 0 ? startPositionUs_ : status.position_us));
    stopPlayback();
    playbackSpeed_ = speed;
    startPositionUs_ = sourcePositionUs;
    playing_ = true;
    if (!startPlayback()) return false;
    if (speedSelector_) {
        const QSignalBlocker blocked(speedSelector_);
        for (int index = 0; index < speedSelector_->count(); ++index) {
            if (speedSelector_->itemData(index).toDouble() == speed) {
                speedSelector_->setCurrentIndex(index);
                break;
            }
        }
    }
    if (!wasPlaying) {
        pauseAfterFirstFrame_ = true;
        playing_ = false;
        updatePlayButton();
    }
    return true;
}

bool MaiFfplayVideoDialog::replay() {
    if (!started_) return false;
    ++navigationGeneration_;
#if defined(Q_OS_MAC)
    if (macPlayback_) {
        MaiFfplayPlaybackStatus status{};
        if (!macPlayback_->status(&status)) return false;
        const bool accepted = macPlayback_->seek(0);
        if (accepted && status.paused) sendCommand(QStringLiteral("pause"));
        return accepted;
    }
#endif
    stopPlayback();
    startPositionUs_ = 0;
    playing_ = true;
    return startPlayback();
}

void MaiFfplayVideoDialog::setPlaylist(const QStringList& paths) {
    playlist_.clear();
    for (const QString& path : paths) {
        if (QFileInfo(path).isFile() && !playlist_.contains(path)) playlist_.append(path);
    }
    if (!playlist_.contains(path_)) playlist_.prepend(path_);
    playlistIndex_ = playlist_.indexOf(path_);
    const bool hasAdjacent = playlist_.size() > 1;
    if (previousItemAction_) previousItemAction_->setVisible(hasAdjacent && playlistIndex_ > 0);
    if (nextButton_) nextButton_->setVisible(hasAdjacent);
    if (previousItemAction_) previousItemAction_->setEnabled(playlistIndex_ > 0);
    if (nextButton_) nextButton_->setEnabled(playlistIndex_ + 1 < playlist_.size());
}

bool MaiFfplayVideoDialog::navigateVideo(int direction) {
    if (direction != -1 && direction != 1) return false;
    const int nextIndex = playlistIndex_ + direction;
    if (nextIndex < 0 || nextIndex >= playlist_.size()) return false;
    const QString nextPath = playlist_.at(nextIndex);
    if (!QFileInfo(nextPath).isFile()) {
        showPlaybackError(QStringLiteral("视频文件已被移动或删除。"));
        return false;
    }
    const QString previousPath = path_;
    const int previousIndex = playlistIndex_;
    // 即使新片打开失败后恢复旧片，也已经重建播放时间线。
    ++navigationGeneration_;
    stopPlayback();
    path_ = nextPath;
    playlistIndex_ = nextIndex;
    startPositionUs_ = 0;
    playing_ = true;
    setWindowTitle(QFileInfo(path_).fileName());
    if (!startPlayback()) {
        path_ = previousPath;
        playlistIndex_ = previousIndex;
        startPlayback();
        showPlaybackError(QStringLiteral("无法打开下一段视频。"));
        return false;
    }
    if (previousItemAction_) previousItemAction_->setEnabled(playlistIndex_ > 0);
    if (previousItemAction_) previousItemAction_->setVisible(playlistIndex_ > 0);
    if (nextButton_) nextButton_->setEnabled(playlistIndex_ + 1 < playlist_.size());
    return true;
}

void MaiFfplayVideoDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    updateControlLayout();
    revealControls();
    if (windowHandle()) windowHandle()->installEventFilter(this);
#if defined(Q_OS_MAC)
    maiGraphicsStyleMacVideoWindow(this);
    if (!fullscreenObserver_)
        fullscreenObserver_ = maiObserveMacVideoFullscreen(this, this, [](void* context, MaiMacFullscreenPhase phase) {
            static_cast<MaiFfplayVideoDialog*>(context)->onMacFullscreenPhase(phase);
        });
#endif
    if (!started_ && !startPlayback()) setWindowTitle(QStringLiteral("无法打开视频"));
}

void MaiFfplayVideoDialog::closeEvent(QCloseEvent* event) {
#if defined(Q_OS_MAC)
    if (fullscreenObserver_ && (nativeFullscreenTransition_ || isFullScreen())) {
        // 进入全屏尚未结束时直接销毁 NSWindow，会使后续窗口也收不到 DidEnter。
        // 合并为退出再关闭；视频图层保持存活，关闭期间不额外重建或隐藏它。
        closeAfterFullscreen_ = true;
        minimizeAfterFullscreen_ = false;
        hasPendingWindowedState_ = false;
        windowWasMaximized_ = false;
        applyFullscreen(false);
        event->ignore();
        return;
    }
    fullscreenRequestPending_ = false;
    nativeFullscreenTransition_ = false;
    maiReleaseMacVideoFullscreenObserver(fullscreenObserver_);
    fullscreenObserver_ = nullptr;
#endif
    if (controlsHideTimer_) controlsHideTimer_->stop();
    stopPlayback(true);
    QDialog::closeEvent(event);
}

void MaiFfplayVideoDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    updateControlLayout();
    if (!viewId_) return;
#if defined(Q_OS_MAC)
    maiGraphicsResizeMacLayer(nativeView_, surface_);
    if (macPlayback_) macPlayback_->resize();
#endif
    maiGraphicsPresenterResize(viewId_, std::max(surface_->width(), 1), std::max(surface_->height(), 1));
}

void MaiFfplayVideoDialog::moveEvent(QMoveEvent* event) {
    QDialog::moveEvent(event);
    updateControlLayout();
}

void MaiFfplayVideoDialog::hideEvent(QHideEvent* event) {
    // owner 的最小化、切换全屏和关闭都不能留下独立浮层；对象继续保留供恢复时复用。
    if (controlsHideTimer_) controlsHideTimer_->stop();
    if (titleBar_) titleBar_->hide();
    if (overlay_) overlay_->hide();
    QDialog::hideEvent(event);
}

bool MaiFfplayVideoDialog::handleFrameMouse(QObject* watched, QEvent* event) {
    const auto type = event->type();
    if (type != QEvent::MouseButtonPress && type != QEvent::MouseMove && type != QEvent::MouseButtonRelease &&
        type != QEvent::MouseButtonDblClick)
        return false;
    auto* mouse = static_cast<QMouseEvent*>(event);
    const QPoint local = mapFromGlobal(mouse->globalPos());
    const bool titleHit = watched == titleBar_ || watched == titleLabel_;
    if (type == QEvent::MouseButtonDblClick && titleHit && mouse->button() == Qt::LeftButton) {
        if (isFullScreen())
            applyFullscreen(false);
        else
            toggleMaximized();
        return true;
    }
#if defined(Q_OS_MAC)
    if (nativeFullscreenTransition_ && titleHit) return true;
#endif
    Qt::Edges edges;
    if (!isMaximized() && !isFullScreen() && rect().contains(local)) {
        if (local.x() < 6) edges |= Qt::LeftEdge;
        if (local.x() >= width() - 6) edges |= Qt::RightEdge;
        if (local.y() < 6) edges |= Qt::TopEdge;
        if (local.y() >= height() - 6) edges |= Qt::BottomEdge;
    }
    if (type == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton && (edges || titleHit)) {
        if (titleHit && isMaximized() && !isFullScreen()) {
            const double fraction = local.x() / double(std::max(1, width()));
            showNormal();
            move(mouse->globalPos() - QPoint(int(width() * fraction), 20));
        }
        resizingFrame_ = edges;
        movingFrame_ = !edges && titleHit && !isFullScreen();
        framePressGlobal_ = mouse->globalPos();
        framePressGeometry_ = geometry();
        if (movingFrame_ || resizingFrame_) {
            windowGeometryCustomized_ = true;
            grabMouse();
            return true;
        }
    }
    if (type == QEvent::MouseMove && (mouse->buttons() & Qt::LeftButton)) {
        const QPoint delta = mouse->globalPos() - framePressGlobal_;
        if (movingFrame_) {
            move(framePressGeometry_.topLeft() + delta);
            return true;
        }
        if (resizingFrame_) {
            QRect target = framePressGeometry_;
            if (resizingFrame_ & Qt::LeftEdge)
                target.setLeft(std::min(target.left() + delta.x(), target.right() - minimumWidth() + 1));
            if (resizingFrame_ & Qt::RightEdge)
                target.setRight(std::max(target.right() + delta.x(), target.left() + minimumWidth() - 1));
            if (resizingFrame_ & Qt::TopEdge)
                target.setTop(std::min(target.top() + delta.y(), target.bottom() - minimumHeight() + 1));
            if (resizingFrame_ & Qt::BottomEdge)
                target.setBottom(std::max(target.bottom() + delta.y(), target.top() + minimumHeight() - 1));
            setGeometry(target);
            return true;
        }
    }
    if (type == QEvent::MouseButtonRelease && (movingFrame_ || resizingFrame_)) {
        movingFrame_ = false;
        resizingFrame_ = {};
        releaseMouse();
        return true;
    }
    if (type == QEvent::MouseMove) {
        Qt::CursorShape cursor = Qt::ArrowCursor;
        if (edges == (Qt::LeftEdge | Qt::TopEdge) || edges == (Qt::RightEdge | Qt::BottomEdge))
            cursor = Qt::SizeFDiagCursor;
        else if (edges == (Qt::RightEdge | Qt::TopEdge) || edges == (Qt::LeftEdge | Qt::BottomEdge))
            cursor = Qt::SizeBDiagCursor;
        else if (edges & (Qt::LeftEdge | Qt::RightEdge))
            cursor = Qt::SizeHorCursor;
        else if (edges & (Qt::TopEdge | Qt::BottomEdge))
            cursor = Qt::SizeVerCursor;
        // native surface 与两个 owned HUD 是不同的原生窗口；仅给 owner 设置不会可靠继承。
        for (QWidget* widget : {static_cast<QWidget*>(this), surface_, titleBar_, overlay_}) {
            if (cursor == Qt::ArrowCursor)
                widget->unsetCursor();
            else
                widget->setCursor(cursor);
        }
    }
    return false;
}

bool MaiFfplayVideoDialog::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::WindowStateChange && watched == this) {
        updateControlLayout();
        revealControls();
    }
    if (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::KeyPress)
        revealControls();
    if (handleFrameMouse(watched, event)) return true;
    return QDialog::eventFilter(watched, event);
}

void MaiFfplayVideoDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
#if defined(Q_OS_MAC)
        if (isFullScreen() || fullscreenRequestPending_ || nativeFullscreenTransition_) {
            applyFullscreen(false);
            return;
        }
#endif
        if (isFullScreen())
            toggleFullscreen();
        else
            close();
    } else if (event->key() == Qt::Key_Space)
        sendCommand(QStringLiteral("pause"));
    else if (event->key() == Qt::Key_Left && event->modifiers() & Qt::ShiftModifier)
        navigateVideo(-1);
    else if (event->key() == Qt::Key_Right && event->modifiers() & Qt::ShiftModifier)
        navigateVideo(1);
    else if (event->key() == Qt::Key_Left)
        sendCommand(QStringLiteral("seek_backward"));
    else if (event->key() == Qt::Key_Right)
        sendCommand(QStringLiteral("seek_forward"));
    else if (event->key() == Qt::Key_Up)
        sendCommand(QStringLiteral("volume_up"));
    else if (event->key() == Qt::Key_Down)
        sendCommand(QStringLiteral("volume_down"));
    else if (event->key() == Qt::Key_M)
        sendCommand(QStringLiteral("mute"));
    else if (event->key() == Qt::Key_F)
        toggleFullscreen();
    else if (event->key() == Qt::Key_PageUp)
        sendCommand(QStringLiteral("next_chapter"));
    else if (event->key() == Qt::Key_PageDown)
        sendCommand(QStringLiteral("previous_chapter"));
    else
        QDialog::keyPressEvent(event);
}
