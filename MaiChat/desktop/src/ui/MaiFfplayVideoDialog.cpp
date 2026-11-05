// 文件作用：桌面端共用界面；MaiFfplayVideoDialog 负责对话框呈现、确认交互和结果回传，本文件给出其实现。
// 主要入口：setForwardAction、forwardVideo、saveCopy、activeDialog。
// 直接依赖：MaiGraphicsPresenter.h；实现与这些接口协作。
#include "ui/MaiFfplayVideoDialog.h"
#include "MaiCloudVod.h"
#include "ui/DesktopMusicPlayerDialog.h"

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
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMetaObject>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
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

// 与应用现有 24px 线条图标保持一致，避免用字体符号造成粗细和基线不齐。
QIcon videoControlIcon(const char* name) {
    QByteArray path;
    const QByteArray key(name);
    if (key == "play")
        path = "<path d='M8 5l11 7-11 7z' fill='#f4f6fa' stroke='none'/>";
    else if (key == "pause")
        path = "<path d='M8 6v12m8-12v12' stroke-width='3'/>";
    else if (key == "replay")
        path = "<path d='M3 10a9 9 0 1 1 2.8 8.5M3 4v6h6'/>";
    else if (key == "fullscreen")
        path = "<path d='M8 4H4v4m12-4h4v4M4 16v4h4m12-4v4h-4'/>";
    else if (key == "minimize")
        path = "<path d='M5 12h14'/>";
    else if (key == "maximize")
        path = "<rect x='5' y='5' width='14' height='14' rx='2'/>";
    else if (key == "restore")
        path = "<path d='M8 7V4h12v12h-3'/><rect x='4' y='8' width='12' height='12' rx='1'/>";
    else if (key == "fullscreen-exit")
        path = "<path d='M4 8h4V4m8 0v4h4M4 16h4v4m8 0v-4h4'/>";
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
        path = "<path d='M14 6l-6 6 6 6'/>";
    else if (key == "next")
        path = "<path d='M10 6l6 6-6 6'/>";
    else
        path =
            "<circle cx='5' cy='12' r='1' fill='#f4f6fa'/><circle cx='12' cy='12' r='1' fill='#f4f6fa'/><circle "
            "cx='19' cy='12' r='1' fill='#f4f6fa'/>";
    QSvgRenderer renderer(
        QByteArray("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='#f4f6fa' "
                   "stroke-width='1.7' stroke-linecap='round' stroke-linejoin='round'>") +
        path + "</svg>");
    QPixmap pixels(40, 40);
    pixels.setDevicePixelRatio(2);
    pixels.fill(Qt::transparent);
    QPainter painter(&pixels);
    renderer.render(&painter, QRectF(0, 0, 20, 20));
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
    // 所有桌面入口共用自绘窗口；原生 surface 只负责画面，不再决定播放器外观。
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setWindowTitle(QFileInfo(path).fileName());
    setMinimumSize(640, 420);
    setStyleSheet(QStringLiteral("QDialog { background:#101010; }"));
    titleBar_ = new QWidget(this);
    titleBar_->setObjectName(QStringLiteral("ffplayTitleBar"));
    titleBar_->setStyleSheet(QStringLiteral("QWidget#ffplayTitleBar { background:#111111; }"));
    auto* header = new QHBoxLayout(titleBar_);
    header->setContentsMargins(18, 0, 10, 0);
    header->setSpacing(4);
    titleLabel_ = new MaiVideoTitleLabel(titleBar_);
    titleLabel_->setObjectName(QStringLiteral("ffplayTitleLabel"));
    titleLabel_->setStyleSheet(QStringLiteral("color:#f4f4f4;font-size:13px;"));
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
        button->setIconSize(QSize(20, 20));
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setFixedSize(34, 34);
        button->setStyleSheet(flatButton);
        header->addWidget(button);
        return button;
    };
#if defined(Q_OS_MAC)
    auto* forward = tool("videoShareButton", "share", QStringLiteral("分享视频"));
    connect(forward, &QToolButton::clicked, this, [this, forward] {
        const QByteArray utf8 = path_.toUtf8();
        maiGraphicsShareMacVideo(forward, utf8.constData());
    });
#else
    auto* forward = tool("videoForwardButton", "forward", QStringLiteral("转发视频"));
    connect(forward, &QToolButton::clicked, this, &MaiFfplayVideoDialog::forwardVideo);
#endif
    auto* save = tool("videoSaveButton", "save", QStringLiteral("保存视频副本"));
    connect(save, &QToolButton::clicked, this, &MaiFfplayVideoDialog::saveCopy);
    auto* more = tool("videoMoreButton", "more", QStringLiteral("更多操作"));
    connect(more, &QToolButton::clicked, this, [this, more] {
        QMenu menu(this);
        menu.setObjectName(QStringLiteral("ffplayMoreMenu"));
        menu.setStyleSheet(
            QStringLiteral("QMenu {color:#f4f4f4;background:#202020;border:1px solid #414141;"
                           "padding:5px;} QMenu::item {padding:8px 18px;}"
                           "QMenu::item:selected {background:#373737;}"));
        auto* copy = menu.addAction(QStringLiteral("复制文件路径"));
        auto* folder = menu.addAction(QStringLiteral("打开所在文件夹"));
        const auto chosen = menu.exec(more->mapToGlobal(more->rect().bottomLeft()));
        if (chosen == copy)
            QApplication::clipboard()->setText(path_);
        else if (chosen == folder)
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path_).absolutePath()));
        revealControls();
    });
    header->addSpacing(8);
    const auto button = [&](QWidget* parentWidget, const char* name, const char* icon, const QString& tip) {
        auto* value = new QPushButton(parentWidget);
        value->setObjectName(QString::fromLatin1(name));
        value->setToolTip(tip);
        value->setAccessibleName(tip);
        value->setIcon(videoControlIcon(icon));
        value->setIconSize(QSize(20, 20));
        value->setFixedSize(34, 34);
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
    connect(minimize, &QPushButton::clicked, this, &QWidget::showMinimized);
    connect(maximizeButton_, &QPushButton::clicked, this, [this] { isMaximized() ? showNormal() : showMaximized(); });
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    surface_ = new MaiVideoSurface(this);
    surface_->setObjectName(QStringLiteral("ffplayVideoSurface"));
    surface_->setAttribute(Qt::WA_NativeWindow);
    surface_->setAttribute(Qt::WA_PaintOnScreen);
    surface_->setAttribute(Qt::WA_NoSystemBackground);
    // 常态为视频外的黑色 footer；全屏为同级原生叠层，收起时不挤压/重建视频视口。
    overlay_ = new QWidget(this);
    overlay_->setObjectName(QStringLiteral("ffplayOverlay"));
    overlay_->setAttribute(Qt::WA_NativeWindow);
    overlay_->setStyleSheet(QStringLiteral("QWidget#ffplayOverlay {background:#101010;}"));
    auto* overlayLayout = new QVBoxLayout(overlay_);
    overlayLayout->setContentsMargins(0, 0, 0, 0);
    auto* panel = new QWidget(overlay_);
    panel->setObjectName(QStringLiteral("ffplayControlPanel"));
    panel->setStyleSheet(QStringLiteral("QWidget#ffplayControlPanel {background:#101010;}"));
    overlayLayout->addWidget(panel);
    auto* panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(20, 7, 20, 9);
    panelLayout->setSpacing(4);
    auto* timeline = new QHBoxLayout();
    timeline->setObjectName(QStringLiteral("ffplayTimelineRow"));
    timeline->setSpacing(12);
    timeLabel_ = new QLabel(QStringLiteral("0:00"), panel);
    timeLabel_->setObjectName(QStringLiteral("ffplayTimeLabel"));
    durationLabel_ = new QLabel(QStringLiteral("0:00"), panel);
    durationLabel_->setObjectName(QStringLiteral("ffplayDurationLabel"));
    for (auto* label : {timeLabel_, durationLabel_}) {
        label->setStyleSheet(QStringLiteral("color:#eeeeee;font-size:12px;"));
        label->setMinimumWidth(34);
    }
    seekSlider_ = new MaiVideoSeekSlider(panel);
    seekSlider_->setObjectName(QStringLiteral("ffplaySeekSlider"));
    seekSlider_->setRange(0, 1000);
    seekSlider_->setMinimumWidth(80);
    seekSlider_->setStyleSheet(QStringLiteral(
        "QSlider::groove:horizontal {height:4px;background:#4b4b4b;border-radius:2px;}"
        "QSlider::sub-page:horizontal {background:#f4f4f4;border-radius:2px;}"
        "QSlider::handle:horizontal {width:10px;margin:-3px 0;background:white;border:0;border-radius:5px;}"));
    timeline->addWidget(timeLabel_);
    timeline->addWidget(seekSlider_, 1);
    timeline->addWidget(durationLabel_);
    panelLayout->addLayout(timeline);
    auto* row = new QHBoxLayout();
    row->setObjectName(QStringLiteral("ffplayButtonRow"));
    row->setSpacing(10);
    previousButton_ = button(panel, "ffplayPreviousItemButton", "previous", QStringLiteral("上一段视频"));
    pauseButton_ = button(panel, "ffplayPauseButton", "pause", QStringLiteral("暂停视频"));
    nextButton_ = button(panel, "ffplayNextItemButton", "next", QStringLiteral("下一段视频"));
    pauseButton_->setFixedSize(32, 32);
    row->addWidget(previousButton_);
    row->addWidget(pauseButton_);
    row->addWidget(nextButton_);
    previousButton_->hide();
    nextButton_->hide();
    row->addStretch(1);
    speedSelector_ = new QComboBox(panel);
    speedSelector_->setObjectName(QStringLiteral("ffplaySpeedSelector"));
    speedSelector_->setToolTip(QStringLiteral("播放倍速"));
    speedSelector_->setAccessibleName(QStringLiteral("播放倍速"));
    for (double rate : {0.25, 0.5, 1.0, 1.5, 2.0}) speedSelector_->addItem(QStringLiteral("%1×").arg(rate), rate);
    speedSelector_->setCurrentIndex(2);
    speedSelector_->setFixedSize(56, 32);
    speedSelector_->setStyleSheet(QStringLiteral(
        "QComboBox {color:#f4f4f4;background:transparent;border:0;border-radius:5px;padding:4px 8px;font-size:13px;}"
        "QComboBox:hover {background:#2b2b2b;} QComboBox::drop-down {border:0;width:0;}"
        "QComboBox QAbstractItemView {color:#f4f4f4;background:#202020;border:1px solid #414141;"
        "selection-background-color:#3a3a3a;padding:4px;}"));
    row->addWidget(speedSelector_);
    volumeButton_ = new QToolButton(panel);
    volumeButton_->setObjectName(QStringLiteral("ffplayVolumeButton"));
    volumeButton_->setFixedSize(34, 34);
    volumeButton_->setStyleSheet(flatButton);
    volumeButton_->setIconSize(QSize(21, 21));
    volumeButton_->setPopupMode(QToolButton::InstantPopup);
    auto* volumeMenu = new QMenu(volumeButton_);
    volumeMenu->setObjectName(QStringLiteral("ffplayVolumeMenu"));
    volumeMenu->setStyleSheet(
        QStringLiteral("QMenu {color:#eeeeee;background:#202020;border:1px solid #414141;padding:8px;}"
                       "QMenu::item {padding:7px 12px;} QMenu::item:selected {background:#373737;}"));
    auto* volumeWidget = new QWidget(volumeMenu);
    auto* volumeLayout = new QHBoxLayout(volumeWidget);
    volumeLayout->setContentsMargins(8, 6, 8, 6);
    volumeSlider_ = new QSlider(Qt::Horizontal, volumeWidget);
    volumeSlider_->setObjectName(QStringLiteral("ffplayVolumeSlider"));
    volumeSlider_->setAccessibleName(QStringLiteral("音量"));
    volumeSlider_->setRange(0, 100);
    volumeSlider_->setValue(100);
    volumeSlider_->setFixedWidth(128);
    volumeSlider_->setStyleSheet(seekSlider_->styleSheet());
    volumeLayout->addWidget(volumeSlider_);
    auto* widgetAction = new QWidgetAction(volumeMenu);
    widgetAction->setDefaultWidget(volumeWidget);
    volumeMenu->addAction(widgetAction);
    muteAction_ = volumeMenu->addAction(QStringLiteral("静音"));
    muteAction_->setObjectName(QStringLiteral("ffplayMuteAction"));
    muteAction_->setCheckable(true);
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
    progressTimer_ = new QTimer(this);
    progressTimer_->setInterval(250);
    connect(progressTimer_, &QTimer::timeout, this, [this] {
        // 音量队列可能先于媒体时钟就绪，准备期也需要补交尚未入队的用户意图。
        syncAudioControls();
        if (cloudSource_ && sPresentedFrames.load() > 0 && errorLabel_->text() == QStringLiteral("正在缓冲视频…"))
            errorLabel_->hide();
        MaiFfplayPlaybackStatus status{};
        if (!maiFfplayGetPlaybackStatus(&status) || status.duration_us <= 0) return;
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
        if (!status.seeking) playing_ = status.paused == 0;
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

    connect(previousButton_, &QPushButton::clicked, this, [this] { navigateVideo(-1); });
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
        if (seekSlider_->isSliderDown() || controlsSeeking_ || panel->underMouse() ||
            QApplication::activePopupWidget() || errorLabel_->isVisible() || sPresentedFrames.load() == 0) {
            controlsHideTimer_->start();
            return;
        }
        panel->hide();
        overlay_->hide();
        setCursor(Qt::BlankCursor);
        surface_->setCursor(Qt::BlankCursor);
    });
    setMouseTracking(true);
    installEventFilter(this);
    for (auto* child : findChildren<QWidget*>()) {
        child->setMouseTracking(true);
        child->installEventFilter(this);
    }
    updateControlLayout();
    updatePlayButton();
    syncAudioControls();
}

MaiFfplayVideoDialog::MaiFfplayVideoDialog(const QString& displayName, std::shared_ptr<MaiCloudVod> source,
                                           QWidget* parent)
    : MaiFfplayVideoDialog(displayName, parent, 0) {
    cloudSource_ = std::move(source);
    setWindowTitle(QFileInfo(displayName).fileName());
    for (const char* name : {"videoForwardButton", "videoShareButton", "videoSaveButton", "videoMoreButton"}) {
        if (auto* button = findChild<QWidget*>(QString::fromLatin1(name))) button->hide();
    }
    updateControlLayout();
}

void MaiFfplayVideoDialog::updateControlLayout() {
    if (!overlay_ || !titleBar_ || !surface_) return;
    const int titleHeight = isFullScreen() ? 0 : 46;
    const int controlsHeight = 84;
    titleBar_->setVisible(!isFullScreen());
    titleBar_->setGeometry(0, 0, width(), titleHeight);
    // 控件隐藏不参与几何计算，全屏视频不会跟着控制栏出现/收起而跳动。
    surface_->setGeometry(0, titleHeight, width(),
                          std::max(1, height() - titleHeight - (isFullScreen() ? 0 : controlsHeight)));
    overlay_->setGeometry(0, height() - controlsHeight, width(), controlsHeight);
    // 构造阶段尚未最终设置窗口 flags，提前 raise 原生子窗会迫使顶层 HWND 过早创建。
    if (isVisible()) {
        overlay_->raise();
        titleBar_->raise();
    }
    errorLabel_->setGeometry(40, titleHeight + std::max(0, (surface_->height() - 64) / 2), std::max(1, width() - 80),
                             64);
    if (errorLabel_->isVisible()) errorLabel_->raise();
    for (const char* name : {"videoForwardButton", "videoShareButton", "videoSaveButton", "videoMoreButton"})
        if (auto* value = findChild<QWidget*>(QString::fromLatin1(name))) value->setVisible(!cloudSource_);
    updateWindowChrome();
}

void MaiFfplayVideoDialog::updateWindowChrome() {
    if (titleLabel_) {
        titleLabel_->setText(windowTitle());
        titleLabel_->setToolTip(windowTitle());
    }
    if (maximizeButton_) {
        maximizeButton_->setIcon(videoControlIcon(isMaximized() ? "restore" : "maximize"));
        maximizeButton_->setToolTip(isMaximized() ? QStringLiteral("还原窗口") : QStringLiteral("最大化"));
        maximizeButton_->setAccessibleName(maximizeButton_->toolTip());
    }
    if (auto* fullscreen = findChild<QPushButton*>(QStringLiteral("ffplayFullscreenButton"))) {
        fullscreen->setIcon(videoControlIcon(isFullScreen() ? "fullscreen-exit" : "fullscreen"));
        fullscreen->setToolTip(isFullScreen() ? QStringLiteral("退出全屏") : QStringLiteral("进入全屏"));
        fullscreen->setAccessibleName(fullscreen->toolTip());
    }
}

void MaiFfplayVideoDialog::syncAudioControls() {
    MaiFfplayAudioStatus status{sizeof(MaiFfplayAudioStatus), 100, 0};
    if (started_ && maiFfplayGetAudioStatus(&status) == 1) {
        const uint64_t consumed = audioStartupConsumedRevision_.load();
        if (volumeRequestPending_ && consumed >= volumeIntentRevision_)
            volumeRequestPending_ = false;
        if (muteRequestPending_ && consumed >= muteIntentRevision_)
            muteRequestPending_ = false;
        if (volumeRequestPending_ && !volumeRequestReceipt_)
            maiFfplaySetVolumePercentWithReceipt(volumePercent_, &volumeRequestReceipt_);
        if (muteRequestPending_ && !muteRequestReceipt_)
            maiFfplaySetMutedWithReceipt(audioMuted_ ? 1 : 0, &muteRequestReceipt_);
        if (volumeRequestReceipt_ && status.applied_serial >= volumeRequestReceipt_)
            volumeRequestPending_ = false;
        if (muteRequestReceipt_ && status.applied_serial >= muteRequestReceipt_)
            muteRequestPending_ = false;
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
    pauseButton_->setIconSize(QSize(20, 20));
    pauseButton_->setToolTip(playing_ ? QStringLiteral("暂停视频") : QStringLiteral("播放视频"));
    pauseButton_->setAccessibleName(pauseButton_->toolTip());
}

void MaiFfplayVideoDialog::revealControls() {
    if (auto* panel = findChild<QWidget*>(QStringLiteral("ffplayControlPanel"))) panel->show();
    if (overlay_) {
        overlay_->show();
        overlay_->raise();
    }
    unsetCursor();
    if (surface_) surface_->unsetCursor();
    scheduleControlHide();
}

void MaiFfplayVideoDialog::scheduleControlHide() {
    if (!controlsHideTimer_) return;
    controlsHideTimer_->stop();
    if (isFullScreen() && playing_) controlsHideTimer_->start();
}

void MaiFfplayVideoDialog::toggleFullscreen() {
    applyFullscreen(!isFullScreen());
}

void MaiFfplayVideoDialog::applyFullscreen(bool enabled) {
    if (enabled == isFullScreen()) return;
    // GUI、键盘、工具及底层宿主回调共同维护进入前的窗口状态。
    if (!enabled) {
        if (windowWasMaximized_) showMaximized();
        else showNormal();
    } else {
        windowWasMaximized_ = isMaximized();
        showFullScreen();
    }
    updateControlLayout();
    revealControls();
}

void MaiFfplayVideoDialog::forwardVideo() {
    if (cloudSource_) {
        QMessageBox::information(this, QStringLiteral("云盘视频"), QStringLiteral("请在云盘列表下载视频后转发。"));
        return;
    }
    if (forwardAction_) {
        forwardAction_(path_);
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
    const QString target = QFileDialog::getSaveFileName(this, QStringLiteral("保存视频"), QFileInfo(path_).fileName(),
                                                        QStringLiteral("视频文件 (*.mp4 *.mov *.m4v);;所有文件 (*)"));
    if (target.isEmpty()) return;
    if (QFileInfo(target).absoluteFilePath() == QFileInfo(path_).absoluteFilePath()) return;
    QFile source(path_);
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
    stopPlayback(true);
}

QPointer<MaiFfplayVideoDialog> MaiFfplayVideoDialog::activeDialog() {
    return sActiveDialog;
}

uint64_t MaiFfplayVideoDialog::presentedFrames() const {
    return sPresentedFrames.load();
}

bool MaiFfplayVideoDialog::hasPresentedHdrFrame() const {
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
            if (dialog->docked_ || dialog->isFullScreen()) return;
            // 自动尺寸按同一标题栏 46px 和控制栏 84px 计算，保留源画面比例。
            QSize size(std::max(width, 640), std::max(height + 130, 420));
            // 原始 4K 或竖屏尺寸不能把独立窗口的关闭/播放控件推到屏幕外。
            if (const auto* screen = dialog->screen()) {
                const QSize available = screen->availableGeometry().size() * 0.85;
                if (size.width() > available.width() || size.height() > available.height())
                    size.scale(available, Qt::KeepAspectRatio);
            }
            dialog->resize(size.expandedTo(dialog->minimumSize()));
        },
        Qt::QueuedConnection);
}

void MaiFfplayVideoDialog::setPosition(void* userData, int x, int y) {
    auto* dialog = static_cast<MaiFfplayVideoDialog*>(userData);
    QMetaObject::invokeMethod(
        dialog,
        [dialog, x, y] {
            if (!dialog->docked_) dialog->move(x, y);
        },
        Qt::QueuedConnection);
}

void MaiFfplayVideoDialog::setFullscreen(void* userData, int enabled) {
    auto* dialog = static_cast<MaiFfplayVideoDialog*>(userData);
    QMetaObject::invokeMethod(
        dialog,
        [dialog, enabled] {
            dialog->applyFullscreen(enabled != 0);
        },
        Qt::QueuedConnection);
}

void MaiFfplayVideoDialog::showWindow(void* userData) {
    auto* dialog = static_cast<MaiFfplayVideoDialog*>(userData);
    QMetaObject::invokeMethod(dialog, [dialog] { dialog->raise(); }, Qt::QueuedConnection);
}

// 作用：验证文件并启动 FFplay，同时同步播放控件状态。
bool MaiFfplayVideoDialog::startPlayback() {
    if (started_ || (cloudSource_ ? mai_cloud_vod_size(cloudSource_.get()) == 0 : !QFileInfo(path_).isFile())) {
        qWarning() << "FFplay popup has no readable video file:" << path_;
        return false;
    }
    if (cloudSource_) {
        errorLabel_->setText(QStringLiteral("正在缓冲视频…"));
        errorLabel_->show();
    } else {
        errorLabel_->hide();
    }
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
    if (overlay_) overlay_->raise();
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
    playbackThread_ = std::thread([this, sourcePath, speed, startPositionUs] {
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
        // SDL 兼容纹理只有 8-bit BT.601/709，不能表示 HLG/PQ。与移动端复用
        // HDR 探测和原生帧桥接，让 10-bit BT.2020 使用每视图 HLG EDR 或 PQ 转换。
        // 探测放在解码线程，云盘继续使用 Range 源，不把整个视频下载到本地。
        sourceInfo_ = {};
        const bool probed = cloudSource_ ? maiFfplayProbeHdrSourceFromHost(&host_, &sourceInfo_)
                                         : maiFfplayProbeHdrSource(sourcePath.constData(), &sourceInfo_);
        if (stopRequested_) {
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
                    errorLabel_->setText(QStringLiteral("无法播放该视频（FFplay 状态 %1）").arg(result));
                    errorLabel_->show();
                }
            },
            Qt::QueuedConnection);
    });
    return true;
}

void MaiFfplayVideoDialog::stopPlayback(bool cancelCloudSource) {
    stopRequested_ = true;
    if (!started_) return;
    syncAudioControls();
    MaiFfplayAudioStatus finalAudio{sizeof(MaiFfplayAudioStatus), 100, 0};
    // 仅在本窗口已运行核心时采集最终快照，避免 HDR 准备取消时借用上个会话。
    const bool hadRunningAudio = maiFfplayGetAudioStatus(&finalAudio) == 1;
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
    // 窗口状态由 Qt 维护；GUI 与工具不能分别切换两个不同的全屏布尔值。
    if (command == QStringLiteral("fullscreen")) {
        if (!started_) return false;
        toggleFullscreen();
        return true;
    }
    const QByteArray utf8 = command.toUtf8();
    const bool accepted = maiFfplaySendCommand(utf8.constData()) == 1;
    if (accepted && command == QStringLiteral("pause")) {
        playing_ = !playing_;
        updatePlayButton();
    }
    if (accepted && (command.startsWith(QStringLiteral("seek_")) || command == QStringLiteral("step") ||
                     command == QStringLiteral("next_chapter") || command == QStringLiteral("previous_chapter")))
        ++navigationGeneration_;
    return accepted;
}

bool MaiFfplayVideoDialog::seekPercent(double fraction) {
    const bool accepted = maiFfplaySeekPercent(fraction) == 1;
    if (accepted) ++navigationGeneration_;
    return accepted;
}

bool MaiFfplayVideoDialog::seekSeconds(double seconds) {
    MaiFfplayPlaybackStatus status{};
    if (!std::isfinite(seconds) || seconds < 0 || !maiFfplayGetPlaybackStatus(&status) || status.duration_us <= 0 ||
        seconds * 1'000'000 > status.duration_us)
        return false;
    const bool accepted = maiFfplaySeekAbsoluteUs(static_cast<int64_t>(std::llround(seconds * 1'000'000))) == 1;
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
    MaiFfplayPlaybackStatus status{};
    // 首次 HDR 探测尚未启动 FFplay 时也接受倍速，按请求的源时间重建即可；
    // 不从尚不存在的播放时钟推断位置，也不把准备阶段当成播放失败。
    if (!maiFfplayGetPlaybackStatus(&status)) {
        if (playbackFinished_) return false;
        status.position_us = startPositionUs_;
    }
    const bool wasPlaying = playing_;
    const int64_t sourcePositionUs = std::max<int64_t>(0, status.position_us);
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
    if (!wasPlaying)
        QTimer::singleShot(150, this, [this] {
            if (started_ && playing_) sendCommand(QStringLiteral("pause"));
        });
    return true;
}

bool MaiFfplayVideoDialog::replay() {
    if (!started_) return false;
    ++navigationGeneration_;
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
    if (previousButton_) previousButton_->setVisible(hasAdjacent);
    if (nextButton_) nextButton_->setVisible(hasAdjacent);
    if (previousButton_) previousButton_->setEnabled(playlistIndex_ > 0);
    if (nextButton_) nextButton_->setEnabled(playlistIndex_ + 1 < playlist_.size());
}

bool MaiFfplayVideoDialog::navigateVideo(int direction) {
    if (direction != -1 && direction != 1) return false;
    const int nextIndex = playlistIndex_ + direction;
    if (nextIndex < 0 || nextIndex >= playlist_.size()) return false;
    const QString nextPath = playlist_.at(nextIndex);
    if (!QFileInfo(nextPath).isFile()) {
        errorLabel_->setText(QStringLiteral("视频文件已被移动或删除。"));
        errorLabel_->show();
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
        errorLabel_->setText(QStringLiteral("无法打开下一段视频。"));
        errorLabel_->show();
        return false;
    }
    if (previousButton_) previousButton_->setEnabled(playlistIndex_ > 0);
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
#endif
    if (!started_ && !startPlayback()) setWindowTitle(QStringLiteral("无法打开视频"));
}

void MaiFfplayVideoDialog::closeEvent(QCloseEvent* event) {
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
#endif
    maiGraphicsPresenterResize(viewId_, std::max(surface_->width(), 1), std::max(surface_->height(), 1));
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
        isMaximized() ? showNormal() : showMaximized();
        return true;
    }
    Qt::Edges edges;
    if (!isMaximized() && !isFullScreen() && rect().contains(local)) {
        if (local.x() < 6) edges |= Qt::LeftEdge;
        if (local.x() >= width() - 6) edges |= Qt::RightEdge;
        if (local.y() < 6) edges |= Qt::TopEdge;
        if (local.y() >= height() - 6) edges |= Qt::BottomEdge;
    }
    if (type == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton && (edges || titleHit)) {
        if (titleHit && isMaximized()) {
            const double fraction = local.x() / double(std::max(1, width()));
            showNormal();
            move(mouse->globalPos() - QPoint(int(width() * fraction), 20));
        }
        resizingFrame_ = edges;
        movingFrame_ = !edges && titleHit && !isFullScreen();
        framePressGlobal_ = mouse->globalPos();
        framePressGeometry_ = geometry();
        if (movingFrame_ || resizingFrame_) {
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
    if (type == QEvent::MouseMove && !isFullScreen() && !isMaximized()) {
        if (edges == (Qt::LeftEdge | Qt::TopEdge) || edges == (Qt::RightEdge | Qt::BottomEdge))
            setCursor(Qt::SizeFDiagCursor);
        else if (edges == (Qt::RightEdge | Qt::TopEdge) || edges == (Qt::LeftEdge | Qt::BottomEdge))
            setCursor(Qt::SizeBDiagCursor);
        else if (edges & (Qt::LeftEdge | Qt::RightEdge))
            setCursor(Qt::SizeHorCursor);
        else if (edges & (Qt::TopEdge | Qt::BottomEdge))
            setCursor(Qt::SizeVerCursor);
        else
            unsetCursor();
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
