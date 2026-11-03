// 文件作用：桌面端共用界面；MaiFfplayVideoDialog 负责对话框呈现、确认交互和结果回传，本文件给出其实现。
// 主要入口：setForwardAction、forwardVideo、saveCopy、activeDialog。
// 直接依赖：MaiGraphicsPresenter.h；实现与这些接口协作。
#include "ui/MaiFfplayVideoDialog.h"
#include "MaiCloudVod.h"
#include "ui/DesktopMusicPlayerDialog.h"

#include <QCloseEvent>
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

#if defined(Q_OS_MAC)
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
#endif

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

#if defined(Q_OS_MAC)
class MaiVideoSurface final : public QWidget {
public:
    explicit MaiVideoSurface(QWidget* parent) : QWidget(parent) {}
    QPaintEngine* paintEngine() const override {
        return nullptr;
    }
};
#endif

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
    setWindowTitle(QFileInfo(path).fileName());
    setMinimumSize(640, 420);
#if defined(Q_OS_MAC)
    setStyleSheet(QStringLiteral("QDialog { background: #080c14; }"));
#else
    setStyleSheet(QStringLiteral("QDialog { background: #10151f; }"));
#endif
    auto* layout = new QVBoxLayout(this);
#if defined(Q_OS_MAC)
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
#endif
#if defined(Q_OS_MAC)
    surface_ = new MaiVideoSurface(this);
#else
    surface_ = new QWidget(this);
#endif
    surface_->setObjectName(QStringLiteral("ffplayVideoSurface"));
    surface_->setAttribute(Qt::WA_NativeWindow);
#if defined(Q_OS_WIN) || defined(Q_OS_MAC)
    surface_->setAttribute(Qt::WA_PaintOnScreen);
    surface_->setAttribute(Qt::WA_NoSystemBackground);
#else
    surface_->setStyleSheet(QStringLiteral("background: black"));
#endif
    layout->addWidget(surface_, 1);
    QWidget* controlParent = this;
    QVBoxLayout* panelLayout = nullptr;
#if defined(Q_OS_MAC)
    overlay_ = new QWidget(surface_);
    overlay_->setObjectName(QStringLiteral("ffplayOverlay"));
    overlay_->setAttribute(Qt::WA_NativeWindow);
    overlay_->setAttribute(Qt::WA_TranslucentBackground);
    overlay_->setStyleSheet(QStringLiteral("QWidget#ffplayOverlay { background: transparent; }"));
    overlay_->setGeometry(surface_->rect());
    surface_->installEventFilter(this);
    auto* overlayLayout = new QVBoxLayout(overlay_);
    overlayLayout->setContentsMargins(18, 18, 18, 18);
    errorLabel_ = new QLabel(overlay_);
    errorLabel_->setAlignment(Qt::AlignCenter);
    overlayLayout->addStretch(1);
    overlayLayout->addWidget(errorLabel_);
    auto* panel = new QWidget(overlay_);
    panel->setObjectName(QStringLiteral("ffplayControlPanel"));
    panel->setStyleSheet(
        QStringLiteral("QWidget#ffplayControlPanel { background: rgba(16, 20, 28, 190);"
                       " border: 1px solid rgba(255,255,255,26); border-radius: 12px; }"));
    panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(10, 8, 10, 8);
    panelLayout->setSpacing(0);
    // 不设固定上限或水平对齐标志，否则布局按 sizeHint 收缩，放大窗口后滑块仍很短。
    panel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    overlayLayout->addWidget(panel);
    controlParent = panel;
#else
    errorLabel_ = new QLabel(this);
    layout->addWidget(errorLabel_);
#endif
    errorLabel_->setStyleSheet(QStringLiteral("color: #ffb4b4"));
    errorLabel_->hide();
    auto* controls = new QHBoxLayout();
    controls->setSpacing(6);
#if defined(Q_OS_MAC)
    pauseButton_ = new QPushButton(QStringLiteral("⏸"), controlParent);
#else
    pauseButton_ = new QPushButton(QStringLiteral("Ⅱ"), controlParent);
#endif
    pauseButton_->setObjectName(QStringLiteral("ffplayPauseButton"));
    pauseButton_->setToolTip(QStringLiteral("暂停视频"));
    seekSlider_ = new MaiVideoSeekSlider(controlParent);
    seekSlider_->setObjectName(QStringLiteral("ffplaySeekSlider"));
    seekSlider_->setRange(0, 1000);
    timeLabel_ = new QLabel(QStringLiteral("0:00 / 0:00"), controlParent);
    timeLabel_->setObjectName(QStringLiteral("ffplayTimeLabel"));
    timeLabel_->setStyleSheet(QStringLiteral("color: white"));
    controls->addWidget(pauseButton_);
    controls->addWidget(seekSlider_, 1);
    controls->addWidget(timeLabel_);
    speedSelector_ = new QComboBox(controlParent);
    speedSelector_->setObjectName(QStringLiteral("ffplaySpeedSelector"));
    speedSelector_->setToolTip(QStringLiteral("播放倍速"));
    for (double rate : {0.25, 0.5, 1.0, 1.5, 2.0}) speedSelector_->addItem(QStringLiteral("%1×").arg(rate), rate);
    speedSelector_->setCurrentIndex(2);
    controls->addWidget(speedSelector_);
#if defined(Q_OS_MAC)
    panelLayout->addLayout(controls);
    pauseButton_->setFixedSize(32, 32);
    pauseButton_->setStyleSheet(
        QStringLiteral("QPushButton#ffplayPauseButton { background: rgba(255,255,255,18);"
                       " border: none; border-radius: 16px; padding:0; }"
                       "QPushButton#ffplayPauseButton:hover { background: rgba(255,255,255,45); }"));
    updatePlayButton();
    seekSlider_->setStyleSheet(
        QStringLiteral("QSlider::groove:horizontal { height: 4px; background: rgba(255,255,255,75);"
                       " border-radius: 2px; }"
                       "QSlider::sub-page:horizontal { background: #f5f8ff; border-radius: 2px; }"
                       "QSlider::handle:horizontal { width: 10px; margin: -3px 0;"
                       " background: white; border: none; border-radius: 5px; }"));
    seekSlider_->setMinimumWidth(80);
    timeLabel_->setStyleSheet(QStringLiteral("color: #e2e6ed; font-size: 11px;"));
    QFont clockFont = timeLabel_->font();
    clockFont.setStyleHint(QFont::Monospace);
    timeLabel_->setFont(clockFont);
    speedSelector_->setStyleSheet(
        QStringLiteral("QComboBox { color: #e2e6ed; background: transparent; border: none;"
                       " border-radius: 6px; padding: 4px 6px; min-width: 30px; font-size:11px; }"
                       "QComboBox::drop-down { border: none; width: 0; }"
                       "QComboBox QAbstractItemView { color: white; background: #182132;"
                       " border: 1px solid #536173; selection-background-color: #35557a; }"));
#else
    layout->addLayout(controls);
#endif
    progressTimer_ = new QTimer(this);
    progressTimer_->setInterval(250);
    connect(progressTimer_, &QTimer::timeout, this, [this] {
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
        timeLabel_->setText(clockText(position) + QStringLiteral(" / ") + clockText(status.duration_us));
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
    auto* actions = new QHBoxLayout();
    actions->addStretch(1);
#if defined(Q_OS_MAC)
    auto* forward = new QPushButton(QStringLiteral("↗  转发"), controlParent);
#else
    auto* forward = new QPushButton(QStringLiteral("↗"), controlParent);
#endif
    forward->setObjectName(QStringLiteral("videoForwardButton"));
#if defined(Q_OS_MAC)
    forward->setStyleSheet(
        QStringLiteral("QPushButton { color:white;background:rgba(255,255,255,25);border:0;border-radius:10px;"
                       "padding:6px 12px; }"
                       "QPushButton:hover { background: rgba(255,255,255,55); }"));
#else
    forward->setStyleSheet(
        QStringLiteral("color:white;background:#363636;border:0;border-radius:20px;"
                       "min-width:40px;min-height:40px;"));
#endif
    forward->setToolTip(QStringLiteral("转发视频"));
    connect(forward, &QPushButton::clicked, this, &MaiFfplayVideoDialog::forwardVideo);
    actions->addWidget(forward);
#if defined(Q_OS_MAC)
    auto* save = new QPushButton(QStringLiteral("↓  保存"), controlParent);
#else
    auto* save = new QPushButton(QStringLiteral("↓"), controlParent);
#endif
    save->setObjectName(QStringLiteral("videoSaveButton"));
    save->setStyleSheet(forward->styleSheet());
    save->setToolTip(QStringLiteral("保存视频副本"));
    connect(save, &QPushButton::clicked, this, &MaiFfplayVideoDialog::saveCopy);
    actions->addWidget(save);
    auto* more = new QPushButton(QStringLiteral("⋯"), controlParent);
    more->setObjectName(QStringLiteral("videoMoreButton"));
    more->setToolTip(QStringLiteral("更多操作"));
    more->setStyleSheet(forward->styleSheet());
    connect(more, &QPushButton::clicked, this, [this, more] {
        QMenu menu(this);
        menu.setObjectName(QStringLiteral("ffplayMoreMenu"));
#if defined(Q_OS_MAC)
        // 局部样式仅影响播放器菜单，不覆盖主聊天界面或系统分享面板。
        menu.setAttribute(Qt::WA_TranslucentBackground);
        menu.setStyleSheet(
            QStringLiteral("QMenu { background:#20242d; color:#f3f5f9; border:1px solid #454b57;"
                           " border-radius:10px; padding:6px; font-size:13px; }"
                           "QMenu::item { padding:9px 16px 9px 10px; min-width:180px; border-radius:6px; }"
                           "QMenu::item:selected { background:#3d475a; }"
                           "QMenu::icon { margin-left:10px; }"
                           "QMenu::separator { height:1px; background:#424752; margin:5px 10px; }"));
        QAction* replayAction = nullptr;
        QAction* forwardAction = nullptr;
        QAction* saveAction = nullptr;
        if (auto* replay = findChild<QPushButton*>(QStringLiteral("ffplayReplayButton"));
            replay && !replay->isVisible()) {
            replayAction = menu.addAction(videoControlIcon("replay"), QStringLiteral("从头播放"));
            forwardAction = menu.addAction(videoControlIcon("forward"), QStringLiteral("转发到会话…"));
            saveAction = menu.addAction(videoControlIcon("save"), QStringLiteral("保存视频副本…"));
            menu.addSeparator();
        }
        QAction* systemShare = menu.addAction(videoControlIcon("share"), QStringLiteral("系统分享…"));
        QAction* copyPath = menu.addAction(videoControlIcon("copy"), QStringLiteral("复制文件路径"));
        QAction* openFolder = menu.addAction(videoControlIcon("folder"), QStringLiteral("在访达中显示"));
        menu.ensurePolished();
        // 菜单在控制条上方展开，避免覆盖底部按钮或被屏幕下沿挤压。
        // 按钮位于面板内边距中，以面板上沿作基准，不能只从按钮上沿减去菜单高度。
        const auto* panel = findChild<QWidget*>(QStringLiteral("ffplayControlPanel"));
        QPoint anchor = more->mapToGlobal(QPoint());
        if (panel) anchor.setY(panel->mapToGlobal(QPoint()).y());
        anchor.ry() -= menu.sizeHint().height() + 8;
        QAction* chosen = menu.exec(anchor);
        if (chosen && chosen == replayAction) {
            replay();
        } else if (chosen && chosen == forwardAction) {
            forwardVideo();
        } else if (chosen && chosen == saveAction) {
            saveCopy();
        } else if (chosen == systemShare) {
            const QByteArray utf8 = path_.toUtf8();
            maiGraphicsShareMacVideo(more, utf8.constData());
        } else
#else
        QAction* copyPath = menu.addAction(QStringLiteral("复制文件路径"));
        QAction* openFolder = menu.addAction(QStringLiteral("打开所在文件夹"));
        QAction* chosen = menu.exec(more->mapToGlobal(more->rect().bottomLeft()));
#endif
            if (chosen == copyPath)
            QApplication::clipboard()->setText(path_);
        else if (chosen == openFolder)
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path_).absolutePath()));
        revealControls();
    });
    actions->addWidget(more);
#if defined(Q_OS_MAC)
    previousButton_ = new QPushButton(QStringLiteral("‹"), controlParent);
    nextButton_ = new QPushButton(QStringLiteral("›"), controlParent);
    previousButton_->setObjectName(QStringLiteral("ffplayPreviousItemButton"));
    nextButton_->setObjectName(QStringLiteral("ffplayNextItemButton"));
    for (QPushButton* button : {previousButton_, nextButton_}) {
        button->setFixedSize(34, 32);
        button->setStyleSheet(
            QStringLiteral("color:white;background:rgba(255,255,255,25);border:0;border-radius:10px;"
                           "font-size:22px;"));
        button->hide();
    }
    previousButton_->setToolTip(QStringLiteral("上一段视频"));
    nextButton_->setToolTip(QStringLiteral("下一段视频"));
    connect(previousButton_, &QPushButton::clicked, this, [this] { navigateVideo(-1); });
    connect(nextButton_, &QPushButton::clicked, this, [this] { navigateVideo(1); });
    actions->insertWidget(0, previousButton_);
    actions->insertWidget(1, nextButton_);
    auto* replayButton = new QPushButton(QStringLiteral("↺  重播"), controlParent);
    replayButton->setObjectName(QStringLiteral("ffplayReplayButton"));
    replayButton->setStyleSheet(forward->styleSheet());
    replayButton->setToolTip(QStringLiteral("从头播放"));
    connect(replayButton, &QPushButton::clicked, this, [this] { replay(); });
    actions->insertWidget(3, replayButton);
    auto* copyPath = new QPushButton(QStringLiteral("⧉  复制路径"), controlParent);
    copyPath->setObjectName(QStringLiteral("ffplayCopyPathButton"));
    copyPath->setStyleSheet(forward->styleSheet());
    connect(copyPath, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(path_); });
    actions->insertWidget(actions->count() - 1, copyPath);
    auto* fullscreenButton = new QPushButton(QStringLiteral("⛶"), controlParent);
    fullscreenButton->setObjectName(QStringLiteral("ffplayFullscreenButton"));
    fullscreenButton->setToolTip(QStringLiteral("切换全屏"));
    fullscreenButton->setStyleSheet(forward->styleSheet());
    connect(fullscreenButton, &QPushButton::clicked, this, [this] {
        if (isFullScreen())
            showNormal();
        else
            showFullScreen();
    });
    actions->insertWidget(actions->count() - 1, fullscreenButton);
    auto* closeButton = new QPushButton(QStringLiteral("×"), controlParent);
    closeButton->setObjectName(QStringLiteral("ffplayCloseButton"));
    closeButton->setToolTip(QStringLiteral("关闭视频"));
    closeButton->setStyleSheet(forward->styleSheet());
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
    actions->addWidget(closeButton);
    // 次要动作保留图标和提示，合入同一行；复制路径留在更多菜单，不重复占据画面。
    copyPath->hide();
    for (auto entry : {std::make_pair(previousButton_, "previous"), std::make_pair(nextButton_, "next"),
                       std::make_pair(replayButton, "replay"), std::make_pair(forward, "forward"),
                       std::make_pair(save, "save"), std::make_pair(more, "more"),
                       std::make_pair(fullscreenButton, "fullscreen"), std::make_pair(closeButton, "close")}) {
        auto* button = entry.first;
        button->setText({});
        button->setIcon(videoControlIcon(entry.second));
        button->setIconSize(QSize(18, 18));
        button->setFixedSize(28, 28);
        button->setAccessibleName(button->toolTip());
        button->setStyleSheet(
            QStringLiteral("QPushButton { background:transparent; border:0; border-radius:6px; padding:0; }"
                           "QPushButton:hover { background:rgba(255,255,255,28); }"
                           "QPushButton:pressed { background:rgba(255,255,255,45); }"));
    }
    while (auto* item = actions->takeAt(0)) {
        if (auto* widget = item->widget()) controls->addWidget(widget);
        delete item;
    }
    delete actions;
    controlsHideTimer_ = new QTimer(this);
    controlsHideTimer_->setSingleShot(true);
    controlsHideTimer_->setInterval(2500);
    connect(controlsHideTimer_, &QTimer::timeout, this, [this] {
        auto* panel = findChild<QWidget*>(QStringLiteral("ffplayControlPanel"));
        if (!panel || !isFullScreen() || !playing_) return;
        // 操作中不收起；隐藏只影响控件，不销毁视频、改变进度或截断菜单选择。
        if (seekSlider_->isSliderDown() || controlsSeeking_ || panel->underMouse() ||
            QApplication::activePopupWidget() || errorLabel_->isVisible() || sPresentedFrames.load() == 0) {
            controlsHideTimer_->start();
            return;
        }
        panel->hide();
        setCursor(Qt::BlankCursor);
        overlay_->setCursor(Qt::BlankCursor);
        surface_->setCursor(Qt::BlankCursor);
    });
    setMouseTracking(true);
    installEventFilter(this);
    for (auto* widget : findChildren<QWidget*>()) {
        widget->setMouseTracking(true);
        widget->installEventFilter(this);
    }
    updateControlLayout();
    overlay_->raise();
#else
    auto* closeButton = new QPushButton(QStringLiteral("×"), controlParent);
    closeButton->setObjectName(QStringLiteral("ffplayCloseButton"));
    closeButton->setToolTip(QStringLiteral("关闭视频"));
    closeButton->setStyleSheet(forward->styleSheet());
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
    actions->addWidget(closeButton);
    layout->addLayout(actions);
#endif
}

MaiFfplayVideoDialog::MaiFfplayVideoDialog(const QString& displayName, std::shared_ptr<MaiCloudVod> source,
                                           QWidget* parent)
    : MaiFfplayVideoDialog(displayName, parent, 0) {
    cloudSource_ = std::move(source);
    setWindowTitle(QFileInfo(displayName).fileName());
    for (const char* name : {"videoForwardButton", "videoSaveButton", "videoMoreButton"}) {
        if (auto* button = findChild<QPushButton*>(QString::fromLatin1(name))) button->hide();
    }
    updateControlLayout();
}

void MaiFfplayVideoDialog::updateControlLayout() {
#if defined(Q_OS_MAC)
    if (!overlay_) return;
    const int margin = std::clamp(overlay_->width() / 40, 14, 36);
    if (auto* layout = overlay_->layout()) layout->setContentsMargins(margin, 18, margin, 18);
    const bool compact = overlay_->width() < 800 && !cloudSource_;
    if (auto* replay = findChild<QPushButton*>(QStringLiteral("ffplayReplayButton"))) replay->setVisible(!compact);
    for (const char* name : {"videoForwardButton", "videoSaveButton"})
        if (auto* button = findChild<QPushButton*>(QString::fromLatin1(name)))
            button->setVisible(!compact && !cloudSource_);
#endif
}

void MaiFfplayVideoDialog::setForwardAction(std::function<void(const QString&)> action) {
    forwardAction_ = std::move(action);
}

void MaiFfplayVideoDialog::updatePlayButton() {
    if (controlsPlaying_ != playing_) {
        controlsPlaying_ = playing_;
        revealControls();
    }
#if defined(Q_OS_MAC)
    pauseButton_->setText({});
    pauseButton_->setIcon(videoControlIcon(playing_ ? "pause" : "play"));
    pauseButton_->setIconSize(QSize(20, 20));
#else
    pauseButton_->setText(playing_ ? QStringLiteral("Ⅱ") : QStringLiteral("▶"));
#endif
    pauseButton_->setToolTip(playing_ ? QStringLiteral("暂停视频") : QStringLiteral("播放视频"));
    pauseButton_->setAccessibleName(pauseButton_->toolTip());
}

void MaiFfplayVideoDialog::revealControls() {
#if defined(Q_OS_MAC)
    if (auto* panel = findChild<QWidget*>(QStringLiteral("ffplayControlPanel"))) panel->show();
    unsetCursor();
    if (overlay_) overlay_->unsetCursor();
    if (surface_) surface_->unsetCursor();
    scheduleControlHide();
#endif
}

void MaiFfplayVideoDialog::scheduleControlHide() {
#if defined(Q_OS_MAC)
    if (!controlsHideTimer_) return;
    controlsHideTimer_->stop();
    if (isFullScreen() && playing_) controlsHideTimer_->start();
#endif
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
#if defined(Q_OS_MAC)
    const QString value = QFileInfo(dialog->path_).fileName();
#else
    const QString value = QString::fromUtf8(title ? title : "Video");
#endif
    QMetaObject::invokeMethod(dialog, [dialog, value] { dialog->setWindowTitle(value); }, Qt::QueuedConnection);
}

void MaiFfplayVideoDialog::setSize(void* userData, int width, int height) {
    auto* dialog = static_cast<MaiFfplayVideoDialog*>(userData);
    QMetaObject::invokeMethod(
        dialog,
        [dialog, width, height] {
            if (dialog->docked_ || dialog->isFullScreen()) return;
#if defined(Q_OS_MAC)
            QSize size(std::max(width, 640), std::max(height, 420));
#else
            QSize size(std::max(width, 640), std::max(height + 60, 420));
#endif
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
            if (enabled)
                dialog->showFullScreen();
            else
                dialog->showNormal();
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
#if defined(Q_OS_MAC)
    if (overlay_) overlay_->raise();
#endif
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
    playbackThread_ = std::thread([this, sourcePath, speed, startPositionUs] {
        char name[] = "ffplay";
        char* arguments[] = {name, const_cast<char*>(sourcePath.constData())};
        int result = 0;
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
            result = maiFfplayRunAtSpeed(&host_, 5, nativeArguments, speed, startPositionUs);
            if (!stopRequested_ && sPresentedFrames == 0) {
                // 硬件解码失败时仍保留原生 HDR 帧，不回退到会丢色彩信息的 SDL 路径。
                char* softwareArguments[] = {name, nativeRenderer, const_cast<char*>(sourcePath.constData())};
                result = maiFfplayRunAtSpeed(&host_, 3, softwareArguments, speed, startPositionUs);
            }
        } else
#endif
        {
            if (!stopRequested_) result = maiFfplayRunAtSpeed(&host_, 2, arguments, speed, startPositionUs);
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
    if (viewId_) maiGraphicsPresenterDetach(viewId_);
    viewId_ = 0;
    if (--sPresenterUsers == 0) maiGraphicsPresenterStop();
    if (sActiveDialog == this) sActiveDialog = nullptr;
    started_ = false;
}

bool MaiFfplayVideoDialog::sendCommand(const QString& command) {
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
    if (!viewId_) return;
#if defined(Q_OS_MAC)
    maiGraphicsResizeMacLayer(nativeView_, surface_);
#endif
    maiGraphicsPresenterResize(viewId_, std::max(surface_->width(), 1), std::max(surface_->height(), 1));
}

bool MaiFfplayVideoDialog::eventFilter(QObject* watched, QEvent* event) {
#if defined(Q_OS_MAC)
    if (watched == surface_ && event->type() == QEvent::Resize && overlay_) {
        overlay_->setGeometry(surface_->rect());
        updateControlLayout();
    }
    if (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::KeyPress ||
        (watched == this && event->type() == QEvent::WindowStateChange))
        revealControls();
#endif
    return QDialog::eventFilter(watched, event);
}

void MaiFfplayVideoDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape)
        close();
    else if (event->key() == Qt::Key_Space)
        sendCommand(QStringLiteral("pause"));
    else if (event->key() == Qt::Key_Left && event->modifiers() & Qt::ShiftModifier)
        navigateVideo(-1);
    else if (event->key() == Qt::Key_Right && event->modifiers() & Qt::ShiftModifier)
        navigateVideo(1);
    else if (event->key() == Qt::Key_Left)
        sendCommand(QStringLiteral("seek_backward"));
    else if (event->key() == Qt::Key_Right)
        sendCommand(QStringLiteral("seek_forward"));
    else if (event->key() == Qt::Key_PageUp)
        sendCommand(QStringLiteral("next_chapter"));
    else if (event->key() == Qt::Key_PageDown)
        sendCommand(QStringLiteral("previous_chapter"));
    else
        QDialog::keyPressEvent(event);
}
