// 文件作用：验证桌面端视频弹窗控制、分享保存、倍速音频及关闭释放。
// 运行边界：由测试进程驱动实际组件或替身，不作为正式 App 的用户入口。
#include <QElapsedTimer>
#include <QAbstractButton>
#include <QAction>
#include <QComboBox>
#include "MaiCameraCapture.h"
#include <QDir>
#include <QHBoxLayout>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFileDialog>
#include <QPointer>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QSlider>
#include <QLabel>
#include <QMouseEvent>
#include <QMenu>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QCursor>
#include <QScreen>
#include <QScopeGuard>
#include <QSplitter>
#include <QSplitterHandle>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QWidget>
#include <QWindow>
#include <QtTest>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "MaiChatTools/DesktopMediaTools.h"
#include "MaiTool.h"
#include "ui/MaiFfplayVideoDialog.h"
#include "ui/DesktopCameraDialog.h"
#include "ui/MainWindow.h"
#include "ui/UiZoom.h"
#include "app/RemoteIMApplication.h"
#include "im/FakeRemoteIMClient.h"

#if defined(__APPLE__)
#include "ui/MaiMacVideoWindow.h"
extern "C" uint64_t maiFfplayAppleNonSilentBufferCount(void);
void activateFfplayTestApplication();
void activateFfplayTestWindow(QWidget* widget);
bool ffplayTestWindowInteractive(QWidget* widget);
QString ffplayTestNativeState(QWidget* widget);
QJsonObject ffplayTestVideoLayerGeometry(QWidget* widget);
bool cancelFfplayTestNativePanel();
void observeFfplayTestNativePanel();
bool clickFfplayTestNativeButton(QWidget* widget);
void* scheduleFfplayTestSaveCancel(bool* observed);
void stopFfplayTestSaveCancel(void* token);
#endif

namespace {

std::atomic<bool> sSawFiniteSeek{false};

// Hold the real playback event thread at a known seek boundary so an accepted
// audio command is still queued when the GUI begins a playback restart. The
// release worker also runs while stopPlayback joins, without pumping GUI events.
class PlaybackAudioQueueGate final {
public:
    explicit PlaybackAudioQueueGate(const char* pattern = "Seek to ") : pattern_(pattern) {
        maiFfplaySetDiagnosticSink(
            [](void* context, int, const char* line) {
                auto* gate = static_cast<PlaybackAudioQueueGate*>(context);
                if (!line || !std::strstr(line, gate->pattern_) || gate->entered.exchange(true)) return;
                while (!gate->released.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                gate->exited = true;
            },
            this);
    }
    ~PlaybackAudioQueueGate() {
        released = true;
        if (releaseWorker.joinable()) releaseWorker.join();
        while (entered.load() && !exited.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        maiFfplaySetDiagnosticSink(nullptr, nullptr);
    }
    void releaseDuringRestart() {
        releaseWorker = std::thread([this] {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            released = true;
        });
    }
    std::atomic<bool> entered{false};
    std::atomic<bool> released{false};

private:
    const char* pattern_;
    std::atomic<bool> exited{false};
    std::thread releaseWorker;
};

// 激活和检查都限于此测试进程自己的窗口；没有前台资格时不采样其他应用的像素。
void activateOwnTestWindow(QWidget* widget) {
    widget->raise();
    widget->activateWindow();
#if defined(Q_OS_MAC)
    activateFfplayTestWindow(widget);
#elif defined(Q_OS_WIN)
    SetForegroundWindow(reinterpret_cast<HWND>(widget->window()->winId()));
#endif
}

void showOwnTestWindow(QWidget* widget) {
#if defined(Q_OS_WIN)
    // 只提高独立测试窗口的层级；不改现装 App、系统前台策略或其他进程。
    widget->setWindowFlag(Qt::WindowStaysOnTopHint);
#endif
    widget->show();
    activateOwnTestWindow(widget);
}

bool ownTestWindowIsForeground(QWidget* widget) {
    if (!widget || !widget->isVisible()) return false;
#if defined(Q_OS_WIN)
    const HWND expected = reinterpret_cast<HWND>(widget->window()->winId());
    const HWND foreground = GetForegroundWindow();
    DWORD process = 0;
    GetWindowThreadProcessId(foreground, &process);
    return process == GetCurrentProcessId() &&
           (foreground == expected || GetAncestor(foreground, GA_ROOTOWNER) == GetAncestor(expected, GA_ROOTOWNER));
#elif defined(Q_OS_MAC)
    return ffplayTestWindowInteractive(widget);
#else
    return widget->isActiveWindow();
#endif
}

#if defined(Q_OS_WIN)
bool nativeWindowHasVisiblePixels(HWND window) {
    if (!IsWindowVisible(window) || IsIconic(window)) return false;
    // Explorer 的 1px ThumbnailDeviceHelperWnd 会保留 WS_VISIBLE，但被 DWM cloak；
    // 它不参与实际合成，不能把全屏截图误判成有其它应用的像素。
    using DwmGetWindowAttributeFunction = HRESULT(WINAPI*)(HWND, DWORD, PVOID, DWORD);
    static const auto getAttribute = reinterpret_cast<DwmGetWindowAttributeFunction>(
        GetProcAddress(GetModuleHandleW(L"dwmapi.dll"), "DwmGetWindowAttribute"));
    DWORD cloaked = 0;
    if (getAttribute && SUCCEEDED(getAttribute(window, 14, &cloaked, sizeof(cloaked))) && cloaked) return false;
    return true;
}
#endif

bool ownTestWindowIsUnobscured(QWidget* widget) {
    if (!ownTestWindowIsForeground(widget)) return false;
#if defined(Q_OS_WIN)
    // Win32 在原生坐标检查整个客户区，避免 Qt 高 DPI 逻辑坐标与物理坐标混用。
    const HWND window = reinterpret_cast<HWND>(widget->window()->winId());
    RECT bounds{};
    if (!GetClientRect(window, &bounds) || bounds.right <= 2 || bounds.bottom <= 2) return false;
    POINT origin{0, 0};
    if (!ClientToScreen(window, &origin)) return false;
    const RECT client{origin.x, origin.y, origin.x + bounds.right, origin.y + bounds.bottom};
    // 顶层遮挡按原生 Z 序检查完整矩形，细小提示窗也不能混入验收截图。
    for (HWND above = GetWindow(window, GW_HWNDPREV); above; above = GetWindow(above, GW_HWNDPREV)) {
        if (!nativeWindowHasVisiblePixels(above)) continue;
        DWORD process = 0;
        GetWindowThreadProcessId(above, &process);
        if (process == GetCurrentProcessId()) continue;
        RECT rectangle{}, overlap{};
        if (GetWindowRect(above, &rectangle) && IntersectRect(&overlap, &client, &rectangle)) return false;
    }
    for (LONG y = 1; y < bounds.bottom; y += 24) {
        for (LONG x = 1; x < bounds.right; x += 24) {
            DWORD process = 0;
            GetWindowThreadProcessId(WindowFromPoint(POINT{origin.x + x, origin.y + y}), &process);
            if (process != GetCurrentProcessId()) return false;
        }
    }
#endif
    return true;
}

QPixmap captureOwnTestWindow(QWidget* widget) {
    if (!ownTestWindowIsUnobscured(widget) || !widget->screen()) {
        qInfo() << "capture rejected" << widget->geometry() << widget->frameGeometry()
                << ownTestWindowIsForeground(widget) << ownTestWindowIsUnobscured(widget);
#if defined(Q_OS_WIN)
        const HWND window = reinterpret_cast<HWND>(widget->window()->winId());
        RECT client{};
        GetWindowRect(window, &client);
        for (HWND above = GetWindow(window, GW_HWNDPREV); above; above = GetWindow(above, GW_HWNDPREV)) {
            if (!nativeWindowHasVisiblePixels(above)) continue;
            RECT rectangle{}, overlap{};
            if (!GetWindowRect(above, &rectangle) || !IntersectRect(&overlap, &client, &rectangle)) continue;
            DWORD process = 0;
            GetWindowThreadProcessId(above, &process);
            wchar_t name[128]{};
            GetClassNameW(above, name, 128);
            qInfo() << "capture overlapping window" << process << QString::fromWCharArray(name)
                    << QRect(rectangle.left, rectangle.top, rectangle.right - rectangle.left,
                             rectangle.bottom - rectangle.top);
        }
#endif
        return {};
    }
    const QRect frame = widget->window()->frameGeometry();
    if (!widget->screen()->geometry().contains(frame)) {
        qInfo() << "capture frame outside screen" << frame << widget->screen()->geometry();
        return {};
    }
#if defined(Q_OS_WIN)
    // 分数 DPI 的逻辑全屏尺寸会四舍五入多出 1px；按已核验 HWND 的物理边界抓取，
    // 不把相邻显示器或窗口外的像素混进自有窗口证据。
    RECT native{};
    if (!GetWindowRect(reinterpret_cast<HWND>(widget->window()->winId()), &native)) return {};
    const int width = native.right - native.left;
    const int height = native.bottom - native.top;
    HDC display = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(display);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(display, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    QImage image;
    if (display && memory && bitmap && bits) {
        const HGDIOBJ previous = SelectObject(memory, bitmap);
        if (BitBlt(memory, 0, 0, width, height, display, native.left, native.top, SRCCOPY | CAPTUREBLT))
            image = QImage(static_cast<uchar*>(bits), width, height, width * 4, QImage::Format_RGB32)
                        .convertToFormat(QImage::Format_RGB888);
        SelectObject(memory, previous);
    }
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    if (display) ReleaseDC(nullptr, display);
    QPixmap result = QPixmap::fromImage(image);
    result.setDevicePixelRatio(widget->devicePixelRatioF());
    return result;
#else
    return widget->screen()->grabWindow(0, frame.x(), frame.y(), frame.width(), frame.height());
#endif
}

bool popupHasVisibleHitTarget(QWidget* popup) {
    QWidget* hit = QApplication::widgetAt(popup->mapToGlobal(popup->rect().center()));
    return hit == popup || (hit && popup->isAncestorOf(hit));
}

// QWidget::isAncestorOf 在独立 Qt::Tool 的 window 边界停止；owned HUD 的所有权由 QObject 链证明。
bool belongsToTestPlayer(QObject* candidate, const MaiFfplayVideoDialog* player) {
    for (QObject* object = candidate; object; object = object->parent())
        if (object == player) return true;
    return false;
}

void dragOwnTestWidget(QWidget* widget, const QPoint& local, const QPoint& delta) {
    const QPoint global = widget->mapToGlobal(local);
    QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(widget, &press);
    QMouseEvent move(QEvent::MouseMove, local + delta, global + delta, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(widget, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, local + delta, global + delta, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(widget, &release);
    QCoreApplication::processEvents();
}

class VideoControlMoveProbe final : public QObject {
public:
    VideoControlMoveProbe(MaiFfplayVideoDialog& player, QWidget& panel) : player_(player), panel_(panel) {}
    int received = 0;
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() != QEvent::MouseMove) return false;
        auto* widget = qobject_cast<QWidget*>(watched);
        if ((!widget || (widget != &player_ && !player_.isAncestorOf(widget))) && watched != player_.windowHandle())
            return false;
        ++received;
        const auto* mouse = static_cast<QMouseEvent*>(event);
        QTimer* hideTimer = nullptr;
        for (auto* timer : player_.findChildren<QTimer*>())
            if (timer->interval() == 2500) hideTimer = timer;
        qInfo() << "[controls-move-event]" << watched->metaObject()->className() << watched->objectName() << "local"
                << mouse->localPos() << "global" << mouse->globalPos() << "cursor" << QCursor::pos() << "spontaneous"
                << event->spontaneous() << "hidden-before" << panel_.isHidden() << "playing" << player_.isPlaying()
                << "timer-active" << bool(hideTimer && hideTimer->isActive()) << "remaining"
                << (hideTimer ? hideTimer->remainingTime() : -1);
        return false;
    }

private:
    MaiFfplayVideoDialog& player_;
    QWidget& panel_;
};

void captureDiagnostic(void*, int, const char* line) {
    if (!line) return;
    const char* seek = std::strstr(line, "Seek to ");
    if (!seek) return;
    seek += std::strlen("Seek to ");
    char* end = nullptr;
    const double percent = std::strtod(seek, &end);
    if (end != seek && *end == '%' && std::isfinite(percent) && std::abs(percent - 50.0) < 0.01) {
        sSawFiniteSeek = true;
    }
}

}  // namespace

class MaiFfplayVideoDialogTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void usesSharedFramelessPlayerChrome();
    void titleBarControlsMoveResizeAndRestoreThePlayer();
    void macInitialWindowRestoreDoesNotReenterFullscreen_data();
    void macInitialWindowRestoreDoesNotReenterFullscreen();
    void windowedVideoFillsViewportAndKeepsUserSize();
    void windowEdgesResizeThroughNativeVideoAndHud();
    void ownedHudTracksOwnerAndForwardsKeys();
    void closingOwnerWithOpenMoreMenuIsSafe();
    void volumeControlsTrackCoreAndSurvivePlaybackChanges();
    void volumePopupKeepsVerticalReadoutAndKeyboardControls();
    void closingOwnerWithOpenVolumePopupIsSafe();
    void opensCompactMenusWithoutChangingPlayback();
    void pausedSpeedChangeKeepsSourceTimeline();
    void volumeCommandsSurviveImmediateRestart_data();
    void volumeCommandsSurviveImmediateRestart();
    void volumeIntentSurvivesDelayedStartup();
    void appliedVolumeIntentDoesNotOverrideLaterAgentCommands();
    void startupAudioIntentDoesNotReplayOverAgentCommands();
    void recordsOwnedPlayerInteractionEvidence();
    void exposesSaveAndForwardActions();
    void shareAndSaveIconsInvokeExistingDialogs();
    void opensControlsAndClosesVideoPopup_data();
    void opensControlsAndClosesVideoPopup();
    void opensAtRequestedSourceTime();
    void agentVideoUsesIndependentWindow();
    void imVideoUsesIndependentWindowAcrossNavigationAndFullscreen_data();
    void imVideoUsesIndependentWindowAcrossNavigationAndFullscreen();
    void fullscreenControlsHideAndWake();
    void macFullscreenKeepsNativeVideoGeometry();
    void controlsHideAndKeepVisibleForPause();
    void agentRepeatedTargetDoesNotReplayFrames();
    void seeksToDecodedTargetFrame_data();
    void seeksToDecodedTargetFrame();
    void sliderSeeksAtPointerPosition();
    void sliderKeepsTargetUntilSeekIsApplied_data();
    void sliderKeepsTargetUntilSeekIsApplied();
    void endedVideoShowsPlayAndSeeksWithoutRunning();
    void cameraDoesNotOpenAfterAgentCancellation();
    void cameraActivePreviewClosesOnAgentCancellation();
    void agentCameraOpensAndClosesWithoutWaitingForShutter();
    void slowedPlaybackExtendsAudibleOutput();
    void localVideoShowsPixels();
    void hdrPlaybackKeepsTransferMetadata_data();
    void hdrPlaybackKeepsTransferMetadata();
};

void MaiFfplayVideoDialogTest::initTestCase() {
    // 可视验收使用正常 App 的随包字体栈，避免 QtTest 的系统回退字体冒充产品外观。
    const QDir fonts(QFileInfo(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE))
                         .dir()
                         .filePath(QStringLiteral("../../../desktop/resources/fonts")));
    for (const auto& file : fonts.entryList({QStringLiteral("*.ttf"), QStringLiteral("*.otf")}, QDir::Files))
        QFontDatabase::addApplicationFont(fonts.filePath(file));
    QFont font;
    font.setFamilies({QStringLiteral("Inter"), QStringLiteral("Noto Sans SC"), QStringLiteral("Microsoft YaHei UI"),
                      QStringLiteral("Microsoft YaHei"), QStringLiteral("PingFang SC")});
    font.setHintingPreference(QFont::PreferNoHinting);
    font.setPixelSize(13);
    QApplication::setFont(font);
#if defined(Q_OS_MAC)
    activateFfplayTestApplication();
    QTest::qWait(250);
#endif
}

// 两种桌面平台共用播放器的窗口外壳和控件；不启动解码即可定位旧系统边框和默认控件回归。
void MaiFfplayVideoDialogTest::usesSharedFramelessPlayerChrome() {
    const QString path = QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE);
    MaiFfplayVideoDialog player(path);
    QVERIFY(player.isWindow());
    QVERIFY2(player.windowFlags().testFlag(Qt::FramelessWindowHint), "video preview still has a native title bar");
    auto* header = player.findChild<QWidget*>(QStringLiteral("ffplayTitleBar"));
    auto* title = player.findChild<QLabel*>(QStringLiteral("ffplayTitleLabel"));
    auto* panel = player.findChild<QWidget*>(QStringLiteral("ffplayControlPanel"));
    auto* pause = player.findChild<QPushButton*>(QStringLiteral("ffplayPauseButton"));
    auto* timeline = player.findChild<QSlider*>(QStringLiteral("ffplaySeekSlider"));
    auto* elapsed = player.findChild<QLabel*>(QStringLiteral("ffplayTimeLabel"));
    auto* duration = player.findChild<QLabel*>(QStringLiteral("ffplayDurationLabel"));
    auto* rates = player.findChild<QComboBox*>(QStringLiteral("ffplaySpeedSelector"));
    QVERIFY(header && title && panel && pause && timeline && elapsed && duration && rates);
    QCOMPARE(title->text(), QFileInfo(path).fileName());
    QVERIFY(header->isAncestorOf(title));
    QVERIFY(panel->isAncestorOf(pause) && panel->isAncestorOf(timeline));
    QVERIFY(!pause->icon().isNull() && pause->text().isEmpty());
    QCOMPARE(rates->count(), 5);
    QVERIFY(!elapsed->text().contains(QLatin1Char('/')));
    for (const char* name : {"ffplayMinimizeButton", "ffplayMaximizeButton", "ffplayCloseButton"}) {
        auto* button = player.findChild<QPushButton*>(QString::fromLatin1(name));
        QVERIFY2(button && header->isAncestorOf(button), name);
        QVERIFY(!button->icon().isNull() && button->text().isEmpty());
        QVERIFY(!button->accessibleName().isEmpty());
    }
    auto* more = player.findChild<QToolButton*>(QStringLiteral("videoMoreButton"));
    auto* divider = player.findChild<QWidget*>(QStringLiteral("ffplayTitleDivider"));
    auto* moreMenu = player.findChild<QMenu*>(QStringLiteral("ffplayMoreMenu"));
    QVERIFY(more && divider && moreMenu);
    QVERIFY(header->isAncestorOf(more) && header->isAncestorOf(divider));
    QCOMPARE(header->findChildren<QToolButton*>().size(), 1);
    QVERIFY(!player.findChild<QToolButton*>(QStringLiteral("videoSaveButton")));
    QVERIFY(!player.findChild<QToolButton*>(QStringLiteral("videoForwardButton")));
    QVERIFY(!player.findChild<QToolButton*>(QStringLiteral("videoShareButton")));
    // Assert the visual grouping through layout order, independent of the
    // chosen margins, font metrics and platform scaling factor.
    auto* headerLayout = qobject_cast<QHBoxLayout*>(header->layout());
    auto* minimize = player.findChild<QPushButton*>(QStringLiteral("ffplayMinimizeButton"));
    QVERIFY(headerLayout && minimize);
    QVERIFY(headerLayout->indexOf(title) < headerLayout->indexOf(more));
    QVERIFY(headerLayout->indexOf(more) < headerLayout->indexOf(divider));
    QVERIFY(headerLayout->indexOf(divider) < headerLayout->indexOf(minimize));
    auto* fullscreen = player.findChild<QPushButton*>(QStringLiteral("ffplayFullscreenButton"));
    auto* volume = player.findChild<QToolButton*>(QStringLiteral("ffplayVolumeButton"));
    auto* volumeSlider = player.findChild<QSlider*>(QStringLiteral("ffplayVolumeSlider"));
    QVERIFY(fullscreen && volume && volumeSlider);
    auto* surface = player.findChild<QWidget*>(QStringLiteral("ffplayVideoSurface"));
    QVERIFY(surface);
    QVERIFY2(surface->testAttribute(Qt::WA_PaintOnScreen),
             "The native video surface must not be overwritten by Qt's backing store.");
    QVERIFY(panel->isAncestorOf(fullscreen) && panel->isAncestorOf(volume));
    QVERIFY(!fullscreen->icon().isNull() && !volume->icon().isNull());
    QCOMPARE(volumeSlider->minimum(), 0);
    QCOMPARE(volumeSlider->maximum(), 100);
    const QString previousPreset = UiZoom::textPreset();
    const auto restorePreset = qScopeGuard([&] {
        UiZoom::setTextPreset(previousPreset);
        UiZoom::refreshText();
    });
    auto* readout = player.findChild<QToolButton*>(QStringLiteral("ffplayVolumeReadout"));
    QVERIFY(readout);
    struct TextPreset {
        const char* name;
        int detail;
        int metadata;
    };
    for (const auto& preset :
         {TextPreset{"standard", 14, 12}, TextPreset{"large", 16, 14}, TextPreset{"extraLarge", 18, 15}}) {
        UiZoom::setTextPreset(QString::fromLatin1(preset.name));
        UiZoom::refreshText();
        for (auto* label : {title, elapsed, duration}) label->ensurePolished();
        rates->ensurePolished();
        readout->ensurePolished();
        QCOMPARE(title->font().pixelSize(), preset.detail);
        QCOMPARE(rates->font().pixelSize(), preset.detail);
        QCOMPARE(elapsed->font().pixelSize(), preset.metadata);
        QCOMPARE(duration->font().pixelSize(), preset.metadata);
        QCOMPARE(readout->font().pixelSize(), preset.metadata);
        QVERIFY(readout->fontMetrics().horizontalAdvance(QStringLiteral("100%")) <= readout->width());
        QVERIFY(rates->maximumWidth() >= rates->fontMetrics().horizontalAdvance(QStringLiteral("0.25×")) + 16);
    }
}

// 自绘标题栏补齐系统边框原有的移动、缩放和窗口操作，且不会替换正在播放的源。
void MaiFfplayVideoDialogTest::titleBarControlsMoveResizeAndRestoreThePlayer() {
    MaiFfplayVideoDialog player(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE));
    player.resize(820, 520);
    showOwnTestWindow(&player);
    QTRY_VERIFY_WITH_TIMEOUT(player.presentedFrames() > 0, 5000);
    MaiFfplayPlaybackStatus status{};
    QVERIFY(player.sendCommand(QStringLiteral("pause")));
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&status) && status.paused, 2000);
    const int64_t pausedPosition = status.position_us;
    auto* header = player.findChild<QWidget*>(QStringLiteral("ffplayTitleBar"));
    auto* title = player.findChild<QLabel*>(QStringLiteral("ffplayTitleLabel"));
    auto* minimize = player.findChild<QPushButton*>(QStringLiteral("ffplayMinimizeButton"));
    auto* maximize = player.findChild<QPushButton*>(QStringLiteral("ffplayMaximizeButton"));
    auto* fullscreen = player.findChild<QPushButton*>(QStringLiteral("ffplayFullscreenButton"));
    auto* close = player.findChild<QPushButton*>(QStringLiteral("ffplayCloseButton"));
    QVERIFY(header && title && minimize && maximize && fullscreen && close);
    const QPoint position = player.pos();
    const QPoint delta(28, 18);
    dragOwnTestWidget(header, QPoint(24, header->height() / 2), delta);
    QTRY_COMPARE(player.pos(), position + delta);
    const QSize originalSize = player.size();
    dragOwnTestWidget(&player, QPoint(player.width() - 2, player.height() - 2), QPoint(70, 40));
    QTRY_COMPARE(player.size(), originalSize + QSize(70, 40));
    const QRect normal = player.geometry();
    maximize->click();
    QTRY_VERIFY(player.isMaximized());
    maximize->click();
    QTRY_VERIFY(!player.isMaximized());
    QTRY_COMPARE(player.geometry(), normal);
    QTest::mouseDClick(header, Qt::LeftButton, Qt::NoModifier, QPoint(24, header->height() / 2));
    QTRY_VERIFY(player.isMaximized());
    QTest::mouseDClick(header, Qt::LeftButton, Qt::NoModifier, QPoint(24, header->height() / 2));
    QTRY_VERIFY(!player.isMaximized());
    minimize->click();
    QTRY_VERIFY(player.isMinimized());
    player.showNormal();
    activateOwnTestWindow(&player);
    QTRY_VERIFY(!player.isMinimized());
    fullscreen->click();
    QTRY_VERIFY(player.isFullScreen());
    QTest::keyClick(&player, Qt::Key_Escape);
    QTRY_VERIFY(!player.isFullScreen());
    QVERIFY(player.isVisible() && player.isStarted());
    // 按钮、键盘和工具调用共用窗口状态，退出全屏要恢复原先的最大化状态。
    maximize->click();
    QTRY_VERIFY(player.isMaximized());
    QVERIFY(player.sendCommand(QStringLiteral("fullscreen")));
    QTRY_VERIFY(player.isFullScreen());
    QTest::keyClick(&player, Qt::Key_Escape);
    QTRY_VERIFY(!player.isFullScreen() && player.isMaximized());
    fullscreen->click();
    QTRY_VERIFY(player.isFullScreen());
    QVERIFY(player.sendCommand(QStringLiteral("fullscreen")));
    QTRY_VERIFY(!player.isFullScreen() && player.isMaximized());
    maximize->click();
    QTRY_VERIFY(!player.isMaximized());
    QVERIFY(maiFfplayGetPlaybackStatus(&status) && status.paused);
    QVERIFY(std::abs(status.position_us - pausedPosition) < 100'000);
    QCOMPARE(title->text(), QFileInfo(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE)).fileName());
    close->click();
    QTRY_VERIFY(!player.isVisible());
    QVERIFY(!player.isStarted());
}

void MaiFfplayVideoDialogTest::macInitialWindowRestoreDoesNotReenterFullscreen_data() {
    QTest::addColumn<bool>("initialFullscreen");
    QTest::newRow("initial-maximized") << false;
    QTest::newRow("initial-native-fullscreen") << true;
}

// 初次显示就已铺满屏幕时，右上角还原应稳定回到普通窗口；持续检查原生状态，
// 避免只看到 Qt 目标标志暂时清除就通过，漏掉后续 AppKit 回调再次铺满的情况。
void MaiFfplayVideoDialogTest::macInitialWindowRestoreDoesNotReenterFullscreen() {
#if defined(Q_OS_MAC)
    if (QGuiApplication::platformName() != QStringLiteral("cocoa")) QSKIP("Native Cocoa window required");
    QFETCH(bool, initialFullscreen);
    MaiFfplayVideoDialog player(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE));
    initialFullscreen ? player.showFullScreen() : player.showMaximized();
    activateOwnTestWindow(&player);
    QTRY_VERIFY_WITH_TIMEOUT(player.presentedFrames() > 0, 5000);
    QTest::qWait(800);
    QVERIFY2(ownTestWindowIsForeground(&player), qPrintable(ffplayTestNativeState(&player)));
    qInfo() << "initial-restore" << player.windowState() << player.geometry()
            << ffplayTestVideoLayerGeometry(&player);
    QVERIFY(player.sendCommand(QStringLiteral("pause")));
    MaiFfplayPlaybackStatus before{};
    QTRY_VERIFY(player.playbackStatus(&before) && before.paused);
    auto* maximize = player.findChild<QPushButton*>(QStringLiteral("ffplayMaximizeButton"));
    QVERIFY(maximize && maximize->isVisible());
    int nativeExits = 0;
    void* observer = maiObserveMacVideoFullscreen(&player, &nativeExits,
        [](void* context, MaiMacFullscreenPhase phase) {
            if (phase == MaiMacFullscreenPhase::DidExit) ++*static_cast<int*>(context);
        });
    QVERIFY(observer);
    const auto releaseObserver = qScopeGuard([&] { maiReleaseMacVideoFullscreenObserver(observer); });
    const QString captureDirectory = qEnvironmentVariable("MAICHAT_VIDEO_RESTORE_CAPTURE_DIR");
    const auto capture = [&](const QString& stage) {
        if (captureDirectory.isEmpty()) return true;
        if (!QDir().mkpath(captureDirectory)) return false;
        const QPixmap pixels = captureOwnTestWindow(&player);
        return !pixels.isNull() && pixels.save(QDir(captureDirectory).filePath(
            QString::fromLatin1(QTest::currentDataTag()) + QLatin1Char('-') + stage + QStringLiteral(".png")));
    };
    QVERIFY(capture(QStringLiteral("before")));
    QSignalSpy clicks(maximize, &QPushButton::clicked);
    QVERIFY(clickFfplayTestNativeButton(maximize));
    QTRY_COMPARE(clicks.count(), 1);
    QString previousState;
    const auto windowed = [&] {
        const bool nativeFullscreen = ffplayTestVideoLayerGeometry(&player).value("nativeFullscreen").toBool();
        const QString state = QStringLiteral("qt=%1 nativeFull=%2 geometry=%3,%4 %5x%6 didExit=%7")
            .arg(int(player.windowState())).arg(nativeFullscreen).arg(player.x()).arg(player.y())
            .arg(player.width()).arg(player.height()).arg(nativeExits);
        if (state != previousState) {
            qInfo().noquote() << "restore-state" << state;
            previousState = state;
        }
        return !player.isFullScreen() && !player.isMaximized() && !nativeFullscreen &&
               (!initialFullscreen || nativeExits > 0);
    };
    QTRY_VERIFY_WITH_TIMEOUT(windowed(), 5000);
    const QRect restored = player.geometry();
    const QRect available = player.screen()->availableGeometry();
    QVERIFY(restored.width() < available.width() || restored.height() < available.height());
    QElapsedTimer stable;
    stable.start();
    while (stable.elapsed() < 1200) {
        QTest::qWait(16);
        QVERIFY(windowed());
        QCOMPARE(player.geometry(), restored);
    }
    QVERIFY(capture(QStringLiteral("restored")));
    MaiFfplayPlaybackStatus after{};
    QVERIFY(player.playbackStatus(&after) && after.paused);
    QVERIFY(std::abs(after.position_us - before.position_us) < 100'000);
    QCOMPARE(player.speed(), 1.0);
    player.close();
    QTRY_VERIFY_WITH_TIMEOUT(!player.isVisible(), 3000);
#else
    QSKIP("Native macOS initial window restore regression");
#endif
}

// 窗口态也应把栏位叠在完整视频视口上；首次尺寸采用源比例，切速不覆盖用户尺寸。
void MaiFfplayVideoDialogTest::windowedVideoFillsViewportAndKeepsUserSize() {
    const QString file = QFileInfo(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE))
                             .dir()
                             .filePath(QStringLiteral("../../../desktop/tests/fixtures/video-circle-61s.mp4"));
    MaiFfplayVideoDialog player(file);
    showOwnTestWindow(&player);
    QTRY_VERIFY_WITH_TIMEOUT(player.presentedFrames() > 0, 5000);
    auto* surface = player.findChild<QWidget*>(QStringLiteral("ffplayVideoSurface"));
    auto* header = player.findChild<QWidget*>(QStringLiteral("ffplayTitleBar"));
    auto* footer = player.findChild<QWidget*>(QStringLiteral("ffplayOverlay"));
    QVERIFY(surface && header && footer);
    QTRY_COMPARE(surface->geometry(), player.rect());
    // 160x90 方形像素素材的整个窗口应是 16:9，不另加固定标题/控制区域。
    QTRY_VERIFY(std::abs(player.width() * 9 - player.height() * 16) <= 16);
    QCOMPARE(header->mapToGlobal(QPoint()), surface->mapToGlobal(QPoint()));
    QCOMPARE(footer->mapToGlobal(QPoint(0, footer->height())), surface->mapToGlobal(QPoint(0, surface->height())));
    QVERIFY(player.sendCommand(QStringLiteral("pause")));
    MaiFfplayPlaybackStatus status{};
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&status) && status.paused, 2000);
    player.resize(790, 470);
    const QRect userGeometry = player.geometry();
    QVERIFY(player.setSpeed(1.5));
    QTRY_VERIFY_WITH_TIMEOUT(player.presentedFrames() > 0 && maiFfplayGetPlaybackStatus(&status) && status.paused &&
                                 status.speed_percent == 150,
                             5000);
    QCOMPARE(player.geometry(), userGeometry);
    QCOMPARE(surface->geometry(), player.rect());
    player.close();
}

// 先命中实际 QWidget，再经所在原生窗口发送，覆盖 native surface 和 owned HUD。
void MaiFfplayVideoDialogTest::windowEdgesResizeThroughNativeVideoAndHud() {
    const QString file = QFileInfo(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE))
                             .dir()
                             .filePath(QStringLiteral("../../../desktop/tests/fixtures/video-circle-61s.mp4"));
    MaiFfplayVideoDialog player(file);
    showOwnTestWindow(&player);
    QTRY_VERIFY_WITH_TIMEOUT(player.presentedFrames() > 0, 5000);
    QTRY_VERIFY(std::abs(player.width() * 9 - player.height() * 16) <= 16);
    QVERIFY(player.sendCommand(QStringLiteral("pause")));
    MaiFfplayPlaybackStatus status{};
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&status) && status.paused, 2000);
    const int64_t position = status.position_us;
    auto* surface = player.findChild<QWidget*>(QStringLiteral("ffplayVideoSurface"));
    auto* header = player.findChild<QWidget*>(QStringLiteral("ffplayTitleBar"));
    auto* footer = player.findChild<QWidget*>(QStringLiteral("ffplayOverlay"));
    QVERIFY(surface && header && footer);
    struct ResizeCase {
        Qt::Edges edges;
        QPoint delta;
        Qt::CursorShape cursor;
    };
    const ResizeCase cases[] = {{Qt::LeftEdge, QPoint(-20, 0), Qt::SizeHorCursor},
                                {Qt::RightEdge, QPoint(20, 0), Qt::SizeHorCursor},
                                {Qt::TopEdge, QPoint(0, -20), Qt::SizeVerCursor},
                                {Qt::BottomEdge, QPoint(0, 20), Qt::SizeVerCursor},
                                {Qt::TopEdge | Qt::LeftEdge, QPoint(-20, -20), Qt::SizeFDiagCursor},
                                {Qt::TopEdge | Qt::RightEdge, QPoint(20, -20), Qt::SizeBDiagCursor},
                                {Qt::BottomEdge | Qt::LeftEdge, QPoint(-20, 20), Qt::SizeBDiagCursor},
                                {Qt::BottomEdge | Qt::RightEdge, QPoint(20, 20), Qt::SizeFDiagCursor}};
    const QRect available = player.screen()->availableGeometry();
    const QRect baseline(available.topLeft() + QPoint(70, 70), QSize(660, 430));
    for (const auto& sample : cases) {
        player.setGeometry(baseline);
        QTRY_COMPARE(player.geometry(), baseline);
        QTRY_COMPARE(player.windowHandle()->geometry(), baseline);
        QCoreApplication::processEvents();
        const QPoint local(sample.edges & Qt::LeftEdge    ? 2
                           : sample.edges & Qt::RightEdge ? player.width() - 3
                                                          : player.width() / 2,
                           sample.edges & Qt::TopEdge      ? 2
                           : sample.edges & Qt::BottomEdge ? player.height() - 3
                                                           : player.height() / 2);
        const QPoint global = player.mapToGlobal(local);
        QWidget* hit = QApplication::widgetAt(global);
        qInfo() << "[player-edge]" << int(sample.edges) << "widget" << player.geometry() << "native-window"
                << player.windowHandle()->geometry() << "point" << global << "hit" << hit << "hit-window"
                << (hit ? hit->window() : nullptr);
#if defined(Q_OS_WIN)
        const HWND nativePlayer = reinterpret_cast<HWND>(player.winId());
        RECT nativeRect{};
        GetWindowRect(nativePlayer, &nativeRect);
        // 原生点由 owner 客户区映射，避免把 Qt 逻辑全局坐标直接作为 Win32 物理坐标。
        POINT nativePoint{qRound(local.x() * player.devicePixelRatioF()),
                          qRound(local.y() * player.devicePixelRatioF())};
        ClientToScreen(nativePlayer, &nativePoint);
        const HWND nativeHit = WindowFromPoint(nativePoint);
        DWORD hitProcess = 0;
        GetWindowThreadProcessId(nativeHit, &hitProcess);
        qInfo() << "[player-edge-native]" << nativeRect.left << nativeRect.top << nativeRect.right << nativeRect.bottom
                << "point" << nativePoint.x << nativePoint.y << "hit" << quintptr(nativeHit) << "pid" << hitProcess
                << "own" << GetCurrentProcessId();
        QCOMPARE(hitProcess, GetCurrentProcessId());
#endif
        QVERIFY2(hit && belongsToTestPlayer(hit, &player), "resize point was covered by another application");
        if (sample.edges & Qt::TopEdge)
            QVERIFY(hit == header || header->isAncestorOf(hit));
        else if (sample.edges & Qt::BottomEdge)
            QVERIFY(hit == footer || footer->isAncestorOf(hit));
        else
            QVERIFY(hit == surface || surface->isAncestorOf(hit));
        QWidget* native = hit->window();
        QVERIFY(native->windowHandle());
        QTest::mouseMove(native->windowHandle(), native->mapFromGlobal(global));
        QTRY_COMPARE(hit->cursor().shape(), sample.cursor);
        QTest::mousePress(native->windowHandle(), Qt::LeftButton, Qt::NoModifier, native->mapFromGlobal(global));
        // 按下后鼠标由播放器捕获；后续移动/松开也须沿真实 grabber 分发。
        QWidget* grabber = QWidget::mouseGrabber();
        QVERIFY(grabber && (grabber == &player || player.isAncestorOf(grabber)));
        QTest::mouseMove(grabber->windowHandle(), grabber->mapFromGlobal(global + sample.delta));
        // QWidget 几何同步更新，原生 QWindow 的 resize 回执还可能排在事件队列里。
        // 松开前等窗口几何一致，不把下一轮 widgetAt 建在尚未完成的原生尺寸之上。
        QTRY_COMPARE(player.windowHandle()->geometry(), player.geometry());
        QTest::mouseRelease(grabber->windowHandle(), Qt::LeftButton, Qt::NoModifier,
                            grabber->mapFromGlobal(global + sample.delta));
        QRect expected = baseline;
        if (sample.edges & Qt::LeftEdge) expected.setLeft(expected.left() + sample.delta.x());
        if (sample.edges & Qt::RightEdge) expected.setRight(expected.right() + sample.delta.x());
        if (sample.edges & Qt::TopEdge) expected.setTop(expected.top() + sample.delta.y());
        if (sample.edges & Qt::BottomEdge) expected.setBottom(expected.bottom() + sample.delta.y());
        QTRY_COMPARE(player.geometry(), expected);
        QCOMPARE(surface->geometry(), player.rect());
    }
    QVERIFY(maiFfplayGetPlaybackStatus(&status) && status.paused);
    QVERIFY(std::abs(status.position_us - position) < 100'000);
    player.close();
}

// 真正的透明 owner 窗口保留输入路径，最小化/切到同 App 其它窗口时不能遗留浮层。
void MaiFfplayVideoDialogTest::ownedHudTracksOwnerAndForwardsKeys() {
    const QString path = QFileInfo(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE))
                             .dir()
                             .filePath(QStringLiteral("../../../desktop/tests/fixtures/video-circle-61s.mp4"));
    MaiFfplayVideoDialog player(path);
    showOwnTestWindow(&player);
    QTRY_VERIFY_WITH_TIMEOUT(player.presentedFrames() > 0, 5000);
    auto* header = player.findChild<QWidget*>(QStringLiteral("ffplayTitleBar"));
    auto* footer = player.findChild<QWidget*>(QStringLiteral("ffplayOverlay"));
    QVERIFY(header && footer && header->isWindow() && footer->isWindow());
    QVERIFY(header->testAttribute(Qt::WA_TranslucentBackground));
    QVERIFY(footer->testAttribute(Qt::WA_TranslucentBackground));
    QTRY_VERIFY(header->isVisible() && footer->isVisible());
    QCOMPARE(header->mapToGlobal(QPoint()), player.mapToGlobal(QPoint()));
    const QPoint delta(30, 20);
    player.move(player.pos() + delta);
    QTRY_COMPARE(header->mapToGlobal(QPoint()), player.mapToGlobal(QPoint()));
    QTest::keyClick(header, Qt::Key_Space);
    MaiFfplayPlaybackStatus status{};
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&status) && status.paused, 2000);
    QTest::keyClick(footer, Qt::Key_F);
    QTRY_VERIFY(player.isFullScreen());
    QTest::keyClick(header, Qt::Key_Escape);
    QTRY_VERIFY(!player.isFullScreen() && player.isVisible());
    QVERIFY(maiFfplayGetPlaybackStatus(&status) && status.paused);
    auto* minimize = player.findChild<QPushButton*>(QStringLiteral("ffplayMinimizeButton"));
    QVERIFY(minimize);
    minimize->click();
    QTRY_VERIFY(player.isMinimized());
    QTRY_VERIFY(!header->isVisible() && !footer->isVisible());
    player.showNormal();
    activateOwnTestWindow(&player);
    QTRY_VERIFY(header->isVisible() && footer->isVisible());
    QWidget other(nullptr, Qt::Window);
    other.resize(200, 120);
    showOwnTestWindow(&other);
    QTRY_VERIFY(QApplication::activeWindow() == &other);
    QTRY_VERIFY(!header->isVisible() && !footer->isVisible());
    QVERIFY(player.isVisible() && player.isStarted());
    other.hide();
    activateOwnTestWindow(&player);
    QTRY_VERIFY(header->isVisible() && footer->isVisible());
    footer->close();
    QTRY_VERIFY(!player.isVisible());
    QTRY_VERIFY(!header->isVisible() && !footer->isVisible());
}

// 账号守卫能在菜单的嵌套循环内删除 owner；返回后的菜单回调不能访问旧播放器。
void MaiFfplayVideoDialogTest::closingOwnerWithOpenMoreMenuIsSafe() {
    QPointer<MaiFfplayVideoDialog> player = new MaiFfplayVideoDialog(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE));
    player->setAttribute(Qt::WA_DeleteOnClose);
    showOwnTestWindow(player);
    QTRY_VERIFY_WITH_TIMEOUT(player->presentedFrames() > 0, 5000);
    auto* more = player->findChild<QToolButton*>(QStringLiteral("videoMoreButton"));
    auto* menu = player->findChild<QMenu*>(QStringLiteral("ffplayMoreMenu"));
    QVERIFY(more && menu);
    QTimer::singleShot(100, menu, [&] {
        if (player) player->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    });
    more->click();
    QTRY_VERIFY(player.isNull());
}

// 断言播放线程的真实音量与静音状态，同时验证旧工具命令能够回显到新控件。
void MaiFfplayVideoDialogTest::volumeControlsTrackCoreAndSurvivePlaybackChanges() {
    const QString source = QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString next = directory.filePath(QStringLiteral("next-fixture.mkv"));
    QVERIFY(QFile::copy(source, next));
    MaiFfplayVideoDialog player(source);
    player.setPlaylist({source, next});
    showOwnTestWindow(&player);
    QTRY_VERIFY_WITH_TIMEOUT(player.presentedFrames() > 0, 5000);
    MaiFfplayPlaybackStatus playback{};
    QVERIFY(player.sendCommand(QStringLiteral("pause")));
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&playback) && playback.paused, 2000);
    auto* volume = player.findChild<QSlider*>(QStringLiteral("ffplayVolumeSlider"));
    auto* volumeButton = player.findChild<QToolButton*>(QStringLiteral("ffplayVolumeButton"));
    auto* volumeMenu = player.findChild<QMenu*>(QStringLiteral("ffplayVolumeMenu"));
    auto* mute = player.findChild<QAction*>(QStringLiteral("ffplayMuteAction"));
    auto* rates = player.findChild<QComboBox*>(QStringLiteral("ffplaySpeedSelector"));
    auto* nextButton = player.findChild<QPushButton*>(QStringLiteral("ffplayNextItemButton"));
    QVERIFY(volume && volumeButton && volumeMenu && mute && rates && nextButton);
    bool volumePopupOpened = false;
    QTimer::singleShot(150, volumeMenu, [&] {
        volumePopupOpened = volumeMenu->isVisible() && volume->isVisible();
        volumeMenu->close();
    });
    QTest::mouseClick(volumeButton, Qt::LeftButton);
    QVERIFY(volumePopupOpened);
    MaiFfplayAudioStatus audio{sizeof(MaiFfplayAudioStatus), -1, -1};
    volume->setValue(37);
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetAudioStatus(&audio) == 1 && audio.volume_percent == 37, 2000);
    mute->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetAudioStatus(&audio) == 1 && audio.muted, 2000);
    QTRY_VERIFY(mute->isChecked());
    QVERIFY(player.sendCommand(QStringLiteral("mute")));
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetAudioStatus(&audio) == 1 && !audio.muted, 2000);
    QTRY_VERIFY(!mute->isChecked());
    QVERIFY(player.sendCommand(QStringLiteral("volume_up")));
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetAudioStatus(&audio) == 1 && audio.volume_percent > 37, 2000);
    QTRY_COMPARE(volume->value(), audio.volume_percent);
    volume->setValue(37);
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetAudioStatus(&audio) == 1 && audio.volume_percent == 37, 2000);
    mute->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetAudioStatus(&audio) == 1 && audio.muted, 2000);
    rates->setCurrentIndex(3);
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&playback) && playback.speed_percent == 150, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetAudioStatus(&audio) == 1 && audio.volume_percent == 37 && audio.muted, 2000);
    QTRY_COMPARE(volume->value(), 37);
    nextButton->click();
    QTRY_COMPARE(player.windowTitle(), QFileInfo(next).fileName());
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&playback) && playback.speed_percent == 150, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetAudioStatus(&audio) == 1 && audio.volume_percent == 37 && audio.muted, 2000);
    QTRY_COMPARE(volume->value(), 37);
    // The compact header exposes previous-item navigation through More while
    // keeping the visible transport row focused on play and next item.
    auto* previous = player.findChild<QAction*>(QStringLiteral("ffplayPreviousItemAction"));
    auto* more = player.findChild<QToolButton*>(QStringLiteral("videoMoreButton"));
    auto* moreMenu = player.findChild<QMenu*>(QStringLiteral("ffplayMoreMenu"));
    QVERIFY(previous && more && moreMenu);
    bool previousRowClicked = false;
    QTimer::singleShot(150, moreMenu, [&] {
        const QRect item = moreMenu->actionGeometry(previous);
        previousRowClicked = moreMenu->isVisible() && previous->isVisible() && previous->isEnabled() && !item.isEmpty();
        if (previousRowClicked)
            QTest::mouseClick(moreMenu, Qt::LeftButton, Qt::NoModifier, item.center());
        else
            moreMenu->close();
    });
    QTest::mouseClick(more, Qt::LeftButton);
    QVERIFY(previousRowClicked);
    QTRY_COMPARE(player.windowTitle(), QFileInfo(source).fileName());
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&playback) && playback.speed_percent == 150, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetAudioStatus(&audio) == 1 && audio.volume_percent == 37 && audio.muted, 2000);
    QTest::keyClick(&player, Qt::Key_Escape);
    QTRY_VERIFY(!player.isVisible());
    QVERIFY(!player.isStarted());
}

// 音量浮层应窄而向上展开；读数、按键和静音直接控制现有真实音频状态。
void MaiFfplayVideoDialogTest::volumePopupKeepsVerticalReadoutAndKeyboardControls() {
    const QString source = QFileInfo(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE))
                               .dir()
                               .filePath(QStringLiteral("../../../desktop/tests/fixtures/video-circle-61s.mp4"));
    MaiFfplayVideoDialog player(source);
    showOwnTestWindow(&player);
    QTRY_VERIFY_WITH_TIMEOUT(player.presentedFrames() > 0, 5000);
    QVERIFY(player.sendCommand(QStringLiteral("pause")));
    MaiFfplayPlaybackStatus playback{};
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&playback) && playback.paused, 2000);
    const int64_t position = playback.position_us;
    auto* volume = player.findChild<QSlider*>(QStringLiteral("ffplayVolumeSlider"));
    auto* button = player.findChild<QToolButton*>(QStringLiteral("ffplayVolumeButton"));
    auto* menu = player.findChild<QMenu*>(QStringLiteral("ffplayVolumeMenu"));
    auto* mute = player.findChild<QAction*>(QStringLiteral("ffplayMuteAction"));
    QVERIFY(volume && button && menu && mute);
    QCOMPARE(volume->orientation(), Qt::Vertical);
    auto* readout = player.findChild<QToolButton*>(QStringLiteral("ffplayVolumeReadout"));
    QVERIFY(readout && !readout->accessibleName().isEmpty());
    volume->setValue(37);
    MaiFfplayAudioStatus audio{sizeof(MaiFfplayAudioStatus), -1, -1};
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetAudioStatus(&audio) == 1 && audio.volume_percent == 37, 2000);
    QTRY_COMPARE(readout->text(), QStringLiteral("37%"));
    bool interacted = false;
    QTimer::singleShot(150, menu, [&] {
        const auto dismiss = qScopeGuard([&] { menu->close(); });
        QTRY_VERIFY_WITH_TIMEOUT(menu->isVisible() && popupHasVisibleHitTarget(menu), 2000);
        QVERIFY(menu->width() <= 80 && menu->height() >= menu->width() * 2);
        QVERIFY(menu->screen()->availableGeometry().contains(menu->geometry()));
        QVERIFY(menu->geometry().bottom() < button->mapToGlobal(QPoint()).y());
        QVERIFY(readout->mapToGlobal(readout->rect().bottomLeft()).y() < volume->mapToGlobal(QPoint()).y());
        QTest::keyClick(volume, Qt::Key_Up);
        const int raised = 37 + volume->singleStep();
        QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetAudioStatus(&audio) == 1 && audio.volume_percent == raised, 2000);
        QTRY_COMPARE(readout->text(), QString::number(raised) + QLatin1Char('%'));
        QTest::mouseClick(readout, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetAudioStatus(&audio) == 1 && audio.muted, 2000);
        QTRY_VERIFY(mute->isChecked());
        QTest::keyClick(menu, Qt::Key_M);
        QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetAudioStatus(&audio) == 1 && !audio.muted, 2000);
        QTest::keyClick(volume, Qt::Key_Down);
        QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetAudioStatus(&audio) == 1 && audio.volume_percent == 37, 2000);
        QTest::keyClick(menu, Qt::Key_Escape);
        QTRY_VERIFY(!menu->isVisible());
        interacted = true;
    });
    QTest::mouseClick(button, Qt::LeftButton);
    QVERIFY(interacted);
    QVERIFY(player.isVisible() && player.isStarted());
    QVERIFY(maiFfplayGetPlaybackStatus(&playback) && playback.paused);
    QVERIFY(std::abs(playback.position_us - position) < 100'000);
    // owned HUD 隐藏后不能留下独立 popup。
    QTimer::singleShot(150, menu, [&] { player.showMinimized(); });
    QTest::mouseClick(button, Qt::LeftButton);
    QTRY_VERIFY(player.isMinimized() && !menu->isVisible());
    player.close();
}

void MaiFfplayVideoDialogTest::closingOwnerWithOpenVolumePopupIsSafe() {
    QPointer<MaiFfplayVideoDialog> player = new MaiFfplayVideoDialog(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE));
    player->setAttribute(Qt::WA_DeleteOnClose);
    showOwnTestWindow(player);
    QTRY_VERIFY_WITH_TIMEOUT(player->presentedFrames() > 0, 5000);
    auto* button = player->findChild<QToolButton*>(QStringLiteral("ffplayVolumeButton"));
    QPointer<QMenu> menu = player->findChild<QMenu*>(QStringLiteral("ffplayVolumeMenu"));
    QVERIFY(button && menu);
    QTimer::singleShot(150, menu, [&] {
        if (player) player->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    });
    button->showMenu();
    QTRY_VERIFY(player.isNull() && menu.isNull());
}

// Open the controls as a user does, then exercise menu selection and dismissal.
// The source clock is paused so popup focus cannot hide a restart or accidental seek.
void MaiFfplayVideoDialogTest::opensCompactMenusWithoutChangingPlayback() {
    const QString source = QFileInfo(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE))
                               .dir()
                               .filePath(QStringLiteral("../../../desktop/tests/fixtures/video-circle-61s.mp4"));
    QVERIFY(QFileInfo::exists(source));
    MaiFfplayVideoDialog player(source);
    const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
    player.resize(QSize(900, 600).boundedTo(available.size() - QSize(48, 64)));
    player.move(available.center() - QPoint(player.width() / 2, player.height() / 2));
    showOwnTestWindow(&player);
    QTRY_VERIFY_WITH_TIMEOUT(player.presentedFrames() > 0, 5000);
    QVERIFY(player.sendCommand(QStringLiteral("pause")));
    MaiFfplayPlaybackStatus playback{};
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&playback) && playback.paused, 2000);
    const int64_t pausedPosition = playback.position_us;
    const auto generation = player.navigationGeneration();
    auto* more = player.findChild<QToolButton*>(QStringLiteral("videoMoreButton"));
    auto* moreMenu = player.findChild<QMenu*>(QStringLiteral("ffplayMoreMenu"));
    auto* speed = player.findChild<QComboBox*>(QStringLiteral("ffplaySpeedSelector"));
    auto* volume = player.findChild<QSlider*>(QStringLiteral("ffplayVolumeSlider"));
    QVERIFY(more && moreMenu && speed && volume);
    bool moreOpenedBelow = false;
    QTimer menuProbe;
    QElapsedTimer menuWait;
    menuWait.start();
    connect(&menuProbe, &QTimer::timeout, moreMenu, [&] {
        const QPoint anchor = more->mapToGlobal(more->rect().bottomLeft());
        moreOpenedBelow = moreMenu->isVisible() && popupHasVisibleHitTarget(moreMenu) && moreMenu->screen() &&
                          moreMenu->screen()->availableGeometry().contains(moreMenu->geometry()) &&
                          moreMenu->geometry().top() >= anchor.y();
        // Cocoa may have painted a popup before WindowServer registers its hit
        // region. Observe real readiness rather than sampling once after 150 ms.
        if (moreOpenedBelow || menuWait.elapsed() > 3000) {
#if defined(Q_OS_MAC)
            qInfo() << "[owned-more-menu] ready" << moreOpenedBelow << moreMenu->geometry()
                    << ffplayTestNativeState(&player);
#endif
            menuProbe.stop();
            QTest::keyClick(moreMenu, Qt::Key_Escape);
        }
    });
    menuProbe.start(50);
    QTest::mouseClick(more, Qt::LeftButton);
    QTRY_VERIFY(moreOpenedBelow);
    QTRY_VERIFY(!moreMenu->isVisible());
    QCOMPARE(player.windowTitle(), QFileInfo(source).fileName());
    QCOMPARE(player.navigationGeneration(), generation);
    QVERIFY(maiFfplayGetPlaybackStatus(&playback) && playback.paused);
    QVERIFY(std::abs(playback.position_us - pausedPosition) < 100'000);
#if !defined(Q_OS_MAC)
    auto* forward = player.findChild<QAction*>(QStringLiteral("videoForwardAction"));
    QVERIFY(forward && moreMenu->actions().contains(forward));
    QString forwardedPath;
    player.setForwardAction([&](const QString& path) { forwardedPath = path; });
    QTimer::singleShot(150, moreMenu, [&] {
        const QRect item = moreMenu->actionGeometry(forward);
        if (moreMenu->isVisible() && !item.isEmpty())
            QTest::mouseClick(moreMenu, Qt::LeftButton, Qt::NoModifier, item.center());
        else
            moreMenu->close();
    });
    QTest::mouseClick(more, Qt::LeftButton);
    QTRY_COMPARE(forwardedPath, source);
    QTRY_VERIFY(!moreMenu->isVisible());
#endif
    volume->setValue(37);
    MaiFfplayAudioStatus audio{sizeof(MaiFfplayAudioStatus), -1, -1};
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetAudioStatus(&audio) == 1 && audio.volume_percent == 37, 2000);
    bool speedOpenedAbove = false;
    bool speedRowSelected = false;
    QTimer::singleShot(150, speed, [&] {
        auto* menu = player.findChild<QMenu*>(QStringLiteral("ffplaySpeedMenu"));
        if (!menu || !menu->isVisible()) return;
        speedOpenedAbove = menu->screen() && menu->screen()->availableGeometry().contains(menu->geometry()) &&
                           menu->geometry().bottom() <= speed->mapToGlobal(QPoint()).y();
        for (auto* action : menu->actions()) {
            const int index = action->data().toInt();
            if (index < 0 || index >= speed->count() || speed->itemData(index).toDouble() != 1.5) continue;
            const QRect item = menu->actionGeometry(action);
            if (!item.isEmpty()) {
                speedRowSelected = true;
                QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, item.center());
            }
            break;
        }
        if (!speedRowSelected) menu->close();
    });
    QTest::mouseClick(speed, Qt::LeftButton);
    QTRY_VERIFY(speedOpenedAbove && speedRowSelected);
    auto* speedMenu = player.findChild<QMenu*>(QStringLiteral("ffplaySpeedMenu"));
    QVERIFY(speedMenu);
    QTRY_VERIFY(!speedMenu->isVisible());
    QTRY_COMPARE(speed->currentIndex(), 3);
    QTRY_VERIFY_WITH_TIMEOUT(
        maiFfplayGetPlaybackStatus(&playback) && playback.paused && playback.speed_percent == 150 && !playback.seeking,
        4000);
    QVERIFY(std::abs(playback.position_us - pausedPosition) < 500'000);
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetAudioStatus(&audio) == 1 && audio.volume_percent == 37, 2000);
    QCOMPARE(player.windowTitle(), QFileInfo(source).fileName());
    player.close();
}

// A restart must present the new source frame before restoring pause. A fixed
// delay can freeze an uninitialized clock and leave the previous surface visible.
void MaiFfplayVideoDialogTest::pausedSpeedChangeKeepsSourceTimeline() {
    const QString source = QFileInfo(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE))
                               .dir()
                               .filePath(QStringLiteral("../../../desktop/tests/fixtures/video-circle-61s.mp4"));
    MaiFfplayVideoDialog player(source);
    showOwnTestWindow(&player);
    QTRY_VERIFY_WITH_TIMEOUT(player.presentedFrames() > 0, 5000);
    QVERIFY(player.sendCommand(QStringLiteral("pause")));
    MaiFfplayPlaybackStatus status{};
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&status) && status.paused, 2000);
    QVERIFY(player.seekSeconds(20));
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&status) && !status.seeking && status.paused &&
                                 status.position_us >= 20'000'000 && status.position_us < 21'000'000,
                             5000);
    const auto sourcePosition = status.position_us;
    QVERIFY(player.setSpeed(1.5));
    QTRY_VERIFY_WITH_TIMEOUT(
        maiFfplayGetPlaybackStatus(&status) && status.paused && status.speed_percent == 150 && !status.seeking, 5000);
    qInfo() << "paused speed restart" << "presentedFrames" << player.presentedFrames() << "position"
            << status.position_us << "paused" << status.paused << "seeking" << status.seeking << "speed"
            << status.speed_percent;
    QVERIFY2(player.presentedFrames() > 0, "the new paused session must present its own frame");
    QVERIFY2(std::abs(status.position_us - sourcePosition) < 500'000,
             "changing speed while paused must preserve the displayed source time");
    auto* elapsed = player.findChild<QLabel*>(QStringLiteral("ffplayTimeLabel"));
    QVERIFY(elapsed);
    QTRY_COMPARE(elapsed->text(), QStringLiteral("0:20"));
    // A user can resume while the new decoder is still preparing. Holding the
    // banner precedes SDL initialization and proves no late timer can re-pause it.
    {
        PlaybackAudioQueueGate gate("ffplay version");
        QVERIFY(player.setSpeed(2.0));
        QTRY_VERIFY_WITH_TIMEOUT(gate.entered.load(), 3000);
        QTest::qWait(400);
        QVERIFY(!player.isPlaying());
        QVERIFY(player.sendCommand(QStringLiteral("pause")));
        QVERIFY(player.isPlaying());
        gate.released = true;
        QTRY_VERIFY_WITH_TIMEOUT(player.presentedFrames() > 0 && maiFfplayGetPlaybackStatus(&status) &&
                                     !status.paused && status.speed_percent == 200 && status.position_us >= 20'000'000,
                                 5000);
        QTest::qWait(400);
        QVERIFY(maiFfplayGetPlaybackStatus(&status) && !status.paused);
    }
    player.close();
}

void MaiFfplayVideoDialogTest::volumeCommandsSurviveImmediateRestart_data() {
    QTest::addColumn<QString>("command");
    QTest::addColumn<int>("initialVolume");
    QTest::addColumn<int>("initialMuted");
    QTest::addColumn<QString>("restart");
    QTest::newRow("mute-on-speed") << QStringLiteral("mute") << 37 << 0 << QStringLiteral("speed");
    QTest::newRow("mute-off-replay") << QStringLiteral("mute") << 37 << 1 << QStringLiteral("replay");
    QTest::newRow("volume-up-next") << QStringLiteral("volume_up") << 37 << 0 << QStringLiteral("next");
    QTest::newRow("volume-down-muted-speed") << QStringLiteral("volume_down") << 37 << 1 << QStringLiteral("speed");
    QTest::newRow("upper-bound-replay") << QStringLiteral("volume_up") << 100 << 0 << QStringLiteral("replay");
    QTest::newRow("lower-bound-next") << QStringLiteral("volume_down") << 0 << 1 << QStringLiteral("next");
    QTest::newRow("zero-up-speed") << QStringLiteral("volume_up") << 0 << 0 << QStringLiteral("speed");
    QTest::newRow("full-down-speed") << QStringLiteral("volume_down") << 100 << 0 << QStringLiteral("speed");
    QTest::newRow("direct-core-settings-next") << QString() << 37 << 0 << QStringLiteral("next");
}

// A GUI restart must preserve commands from keyboard/Agent and direct core
// callers, including commands accepted before the playback thread consumes them.
void MaiFfplayVideoDialogTest::volumeCommandsSurviveImmediateRestart() {
    QFETCH(QString, command);
    QFETCH(int, initialVolume);
    QFETCH(int, initialMuted);
    QFETCH(QString, restart);
    const QString source = QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString next = directory.filePath(QStringLiteral("next-audio-fixture.mkv"));
    QVERIFY(QFile::copy(source, next));
    MaiFfplayVideoDialog player(source);
    player.setPlaylist({source, next});
    showOwnTestWindow(&player);
    MaiFfplayPlaybackStatus playback{};
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&playback) == 1 && playback.position_us > 100'000, 5000);
    MaiFfplayAudioStatus audio{sizeof(MaiFfplayAudioStatus), -1, -1};
    QCOMPARE(maiFfplaySetVolumePercent(initialVolume), 1);
    QCOMPARE(maiFfplaySetMuted(initialMuted), 1);
    QTRY_VERIFY_WITH_TIMEOUT(
        maiFfplayGetAudioStatus(&audio) == 1 && audio.volume_percent == initialVolume && audio.muted == initialMuted,
        2000);

    // Obtain the expected logarithmic volume step from the real core before
    // restarting, so the GUI test does not duplicate FFplay's gain formula.
    int expectedVolume = 63;
    const int expectedMuted = command.isEmpty() ? 1 : command == QStringLiteral("mute") ? !initialMuted : initialMuted;
    if (!command.isEmpty()) {
        QVERIFY(player.sendCommand(command));
        QCOMPARE(maiFfplaySetMuted(!initialMuted), 1);
        QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetAudioStatus(&audio) == 1 && audio.muted == !initialMuted, 2000);
        expectedVolume = audio.volume_percent;
        QCOMPARE(maiFfplaySetVolumePercent(initialVolume), 1);
        QCOMPARE(maiFfplaySetMuted(initialMuted), 1);
        QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetAudioStatus(&audio) == 1 && audio.volume_percent == initialVolume &&
                                     audio.muted == initialMuted,
                                 2000);
    }
    auto* volume = player.findChild<QSlider*>(QStringLiteral("ffplayVolumeSlider"));
    auto* mute = player.findChild<QAction*>(QStringLiteral("ffplayMuteAction"));
    QVERIFY(volume && mute);
    QTRY_COMPARE(volume->value(), initialVolume);
    QTRY_COMPARE(mute->isChecked(), bool(initialMuted));

    PlaybackAudioQueueGate gate;
    QVERIFY(player.seekSeconds(1));
    QTRY_VERIFY_WITH_TIMEOUT(gate.entered.load(), 2000);
    if (command.isEmpty()) {
        QCOMPARE(maiFfplaySetVolumePercent(expectedVolume), 1);
        QCOMPARE(maiFfplaySetMuted(expectedMuted), 1);
    } else {
        QVERIFY(player.sendCommand(command));
    }
    QVERIFY(maiFfplayGetAudioStatus(&audio) == 1);
    QCOMPARE(audio.volume_percent, initialVolume);
    QCOMPARE(audio.muted, initialMuted);
    gate.releaseDuringRestart();
    if (restart == QStringLiteral("speed"))
        QVERIFY(player.setSpeed(1.5));
    else if (restart == QStringLiteral("replay"))
        QVERIFY(player.replay());
    else
        QVERIFY(player.navigateVideo(1));
    QTRY_VERIFY_WITH_TIMEOUT(
        maiFfplayGetAudioStatus(&audio) == 1 && audio.volume_percent == expectedVolume && audio.muted == expectedMuted,
        5000);
    QTRY_COMPARE(volume->value(), expectedVolume);
    QTRY_COMPARE(mute->isChecked(), bool(expectedMuted));
    player.close();
}

// The banner is emitted before SDL_Init: setters cannot enqueue yet. Waiting
// longer than the former 1.5-second timeout must not discard the user's intent.
void MaiFfplayVideoDialogTest::volumeIntentSurvivesDelayedStartup() {
    MaiFfplayVideoDialog player(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE));
    PlaybackAudioQueueGate gate("ffplay version");
    showOwnTestWindow(&player);
    QTRY_VERIFY_WITH_TIMEOUT(gate.entered.load(), 3000);
    QCOMPARE(maiFfplaySetVolumePercent(37), 0);
    auto* volume = player.findChild<QSlider*>(QStringLiteral("ffplayVolumeSlider"));
    auto* mute = player.findChild<QAction*>(QStringLiteral("ffplayMuteAction"));
    QVERIFY(volume && mute);
    volume->setValue(37);
    mute->trigger();
    QTest::qWait(1900);
    QCOMPARE(volume->value(), 37);
    QVERIFY(mute->isChecked());
    gate.released = true;
    MaiFfplayAudioStatus audio{sizeof(MaiFfplayAudioStatus), -1, -1};
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetAudioStatus(&audio) == 1 && audio.volume_percent == 37 && audio.muted, 5000);
    QVERIFY(player.setSpeed(1.5));
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetAudioStatus(&audio) == 1 && audio.volume_percent == 37 && audio.muted, 5000);
    player.close();
}

// A later Agent command can already supersede a GUI request before the GUI sees
// the requested value. A receipt acknowledges FIFO execution without re-sending
// the obsolete GUI value or waiting forever for value equality.
void MaiFfplayVideoDialogTest::appliedVolumeIntentDoesNotOverrideLaterAgentCommands() {
    MaiFfplayVideoDialog player(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE));
    showOwnTestWindow(&player);
    MaiFfplayPlaybackStatus playback{};
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&playback) == 1 && playback.position_us > 100'000, 5000);
    auto* volume = player.findChild<QSlider*>(QStringLiteral("ffplayVolumeSlider"));
    auto* mute = player.findChild<QAction*>(QStringLiteral("ffplayMuteAction"));
    QVERIFY(volume && mute);
    PlaybackAudioQueueGate gate;
    QVERIFY(player.seekSeconds(1));
    QTRY_VERIFY_WITH_TIMEOUT(gate.entered.load(), 2000);
    volume->setValue(37);
    mute->trigger();
    QCOMPARE(maiFfplaySetVolumePercent(63), 1);
    QVERIFY(player.sendCommand(QStringLiteral("mute")));
    QTest::qWait(1900);
    QCOMPARE(volume->value(), 37);
    QVERIFY(mute->isChecked());
    gate.released = true;
    MaiFfplayAudioStatus audio{sizeof(MaiFfplayAudioStatus), -1, -1};
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetAudioStatus(&audio) == 1 && audio.volume_percent == 63 && !audio.muted, 5000);
    QTRY_COMPARE(volume->value(), 63);
    QTRY_VERIFY(!mute->isChecked());
    QVERIFY(player.replay());
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetAudioStatus(&audio) == 1 && audio.volume_percent == 63 && !audio.muted, 5000);
    player.close();
}

// Settings consumed by startup parameters already took effect before the first
// GUI poll. Do not submit them again over an Agent update received meanwhile.
void MaiFfplayVideoDialogTest::startupAudioIntentDoesNotReplayOverAgentCommands() {
    MaiFfplayVideoDialog player(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE));
    auto* volume = player.findChild<QSlider*>(QStringLiteral("ffplayVolumeSlider"));
    auto* mute = player.findChild<QAction*>(QStringLiteral("ffplayMuteAction"));
    QVERIFY(volume && mute);
    volume->setValue(37);
    mute->trigger();
    showOwnTestWindow(&player);
    QTimer* progress = nullptr;
    for (auto* timer : player.findChildren<QTimer*>())
        if (timer->interval() == 250) progress = timer;
    QVERIFY(progress);
    progress->stop();
    MaiFfplayPlaybackStatus playback{};
    MaiFfplayAudioStatus audio{sizeof(MaiFfplayAudioStatus), -1, -1};
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&playback) && playback.position_us > 100'000 &&
                                 maiFfplayGetAudioStatus(&audio) == 1 && audio.volume_percent == 37 && audio.muted,
                             5000);
    QCOMPARE(maiFfplaySetVolumePercent(63), 1);
    QVERIFY(player.sendCommand(QStringLiteral("mute")));
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetAudioStatus(&audio) == 1 && audio.volume_percent == 63 && !audio.muted, 2000);
    progress->start();
    QTRY_COMPARE(volume->value(), 63);
    QTRY_VERIFY(!mute->isChecked());
    QTest::qWait(300);
    QVERIFY(maiFfplayGetAudioStatus(&audio) == 1 && audio.volume_percent == 63 && !audio.muted);
    player.close();
}

// 显式开启的人工验收产物：只录测试自身的窗口矩形，覆盖暂停、seek、倍速、音量及全屏过渡。
// 不满足真实前台和无遮挡条件时停止采样并失败，不能把其他应用的像素保存为测试证据。
void MaiFfplayVideoDialogTest::recordsOwnedPlayerInteractionEvidence() {
    const QString output = qEnvironmentVariable("MAICHAT_PLAYER_EVIDENCE_DIR");
    if (output.isEmpty()) QSKIP("Set MAICHAT_PLAYER_EVIDENCE_DIR to record the isolated player fixture");
    QVERIFY(QDir().mkpath(output));
    const QString framesPath = QDir(output).filePath(QStringLiteral("frames"));
    QVERIFY(QDir().mkpath(framesPath));
    QVERIFY2(QDir(framesPath).entryList({QStringLiteral("frame-*.png")}, QDir::Files).isEmpty(),
             "Use a fresh evidence directory; previous frames must not enter this recording");
    const QString source =
        qEnvironmentVariable("MAICHAT_PLAYER_EVIDENCE_VIDEO",
                             QFileInfo(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE))
                                 .dir()
                                 .filePath(QStringLiteral("../../../desktop/tests/fixtures/video-circle-61s.mp4")));
    QVERIFY(QFileInfo::exists(source));
    MaiFfplayVideoDialog player(source);
    const QString nextSource = qEnvironmentVariable("MAICHAT_FFPLAY_NEXT_VIDEO");
    if (QFileInfo(nextSource).isFile()) player.setPlaylist({source, nextSource});
    const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
    const int requestedWidth = qEnvironmentVariableIntValue("MAICHAT_PLAYER_EVIDENCE_WIDTH");
    const int requestedHeight = qEnvironmentVariableIntValue("MAICHAT_PLAYER_EVIDENCE_HEIGHT");
    player.resize(QSize(requestedWidth > 0 ? requestedWidth : 1000, requestedHeight > 0 ? requestedHeight : 640)
                      .boundedTo(available.size() - QSize(48, 64)));
    player.move(available.center() - QPoint(player.width() / 2, player.height() / 2));
    showOwnTestWindow(&player);
    QTest::qWait(200);
    qInfo() << "recording window identity" << player.geometry() << player.frameGeometry()
            << ownTestWindowIsForeground(&player) << ownTestWindowIsUnobscured(&player);
#if defined(Q_OS_WIN)
    if (!ownTestWindowIsUnobscured(&player)) {
        const HWND own = reinterpret_cast<HWND>(player.winId());
        RECT bounds{};
        GetWindowRect(own, &bounds);
        for (POINT point : {POINT{bounds.left + 1, bounds.top + 1},
                            POINT{(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2}}) {
            const HWND found = WindowFromPoint(point);
            DWORD process = 0;
            GetWindowThreadProcessId(found, &process);
            wchar_t name[128]{};
            GetClassNameW(found, name, 128);
            qInfo() << "recording point owner" << point.x << point.y << process << QString::fromWCharArray(name);
        }
    }
#endif
    QTRY_VERIFY_WITH_TIMEOUT(ownTestWindowIsUnobscured(&player), 2500);
    QTRY_VERIFY_WITH_TIMEOUT(player.presentedFrames() > 0, 5000);
    QCoreApplication::processEvents();
    // 等首帧默认比例定尺寸完成后，再应用人工验收明确指定的窗口大小；
    // 这样既测试默认打开行为，也不会让异步初始回调覆盖录制时的自然/自由比例。
    if (requestedWidth > 0 || requestedHeight > 0) {
        player.resize(QSize(requestedWidth > 0 ? requestedWidth : player.width(),
                            requestedHeight > 0 ? requestedHeight : player.height())
                          .boundedTo(available.size() - QSize(48, 64)));
        player.move(available.center() - QPoint(player.width() / 2, player.height() / 2));
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(ownTestWindowIsUnobscured(&player), 2500);
    }
    auto* pause = player.findChild<QPushButton*>(QStringLiteral("ffplayPauseButton"));
    auto* seek = player.findChild<QSlider*>(QStringLiteral("ffplaySeekSlider"));
    auto* speed = player.findChild<QComboBox*>(QStringLiteral("ffplaySpeedSelector"));
    auto* volume = player.findChild<QSlider*>(QStringLiteral("ffplayVolumeSlider"));
    auto* mute = player.findChild<QAction*>(QStringLiteral("ffplayMuteAction"));
    auto* fullscreen = player.findChild<QPushButton*>(QStringLiteral("ffplayFullscreenButton"));
    auto* surface = player.findChild<QWidget*>(QStringLiteral("ffplayVideoSurface"));
    auto* panel = player.findChild<QWidget*>(QStringLiteral("ffplayControlPanel"));
    auto* header = player.findChild<QWidget*>(QStringLiteral("ffplayTitleBar"));
    auto* more = player.findChild<QToolButton*>(QStringLiteral("videoMoreButton"));
    auto* moreMenu = player.findChild<QMenu*>(QStringLiteral("ffplayMoreMenu"));
    QVERIFY(pause && seek && speed && volume && mute && fullscreen && surface && panel && header && more && moreMenu);
    bool captureFailed = false;
    bool capturedBlankFrame = false;
    const bool requireNonblack = qEnvironmentVariableIntValue("MAICHAT_PLAYER_REQUIRE_NONBLACK") != 0;
    int frameNumber = 0;
    const QSize canvasSize(1000, 700);
    const auto captureFrame = [&] {
        if (captureFailed) return;
        const QPixmap pixels = captureOwnTestWindow(&player);
        if (pixels.isNull()) {
            captureFailed = true;
            return;
        }
        if (requireNonblack) {
            // 仅供已知始终明亮的彩条素材；真实录像允许黑场，不能套用这个验收条件。
            const QImage source = pixels.toImage();
            const qreal scale = qreal(source.width()) / player.width();
            const QPoint origin = surface->mapTo(&player, QPoint());
            bool visibleSourcePixel = false;
            for (int y = 1; y <= 3; ++y)
                for (int x = 1; x <= 3; ++x) {
                    const QColor sample = source.pixelColor(qRound((origin.x() + surface->width() * x / 4) * scale),
                                                            qRound((origin.y() + surface->height() * y / 4) * scale));
                    visibleSourcePixel |= std::max({sample.red(), sample.green(), sample.blue()}) > 64;
                }
            capturedBlankFrame |= !visibleSourcePixel;
        }
        QImage frame(canvasSize, QImage::Format_RGB32);
        frame.fill(QColor(QStringLiteral("#07090c")));
        QPainter painter(&frame);
        QPixmap fitted = pixels.scaled(canvasSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        fitted.setDevicePixelRatio(1);
        painter.drawPixmap((canvasSize.width() - fitted.width()) / 2, (canvasSize.height() - fitted.height()) / 2,
                           fitted);
        painter.end();
        const QString name = QStringLiteral("frame-%1.png").arg(frameNumber++, 5, 10, QLatin1Char('0'));
        if (!frame.save(QDir(framesPath).filePath(name))) captureFailed = true;
    };
    const auto captureStage = [&](const QString& stage) {
        const QPixmap pixels = captureOwnTestWindow(&player);
        return !pixels.isNull() && pixels.save(QDir(output).filePath(stage + QStringLiteral(".png")));
    };
    QTimer capture;
    capture.setInterval(100);
    connect(&capture, &QTimer::timeout, this, captureFrame);
    captureFrame();
    capture.start();
    const auto stopCapture = qScopeGuard([&] { capture.stop(); });
    QVERIFY(captureStage(QStringLiteral("01-playing")));
    QTest::qWait(600);
    pause->click();
    QTRY_VERIFY(!player.isPlaying());
    QTest::mouseClick(seek, Qt::LeftButton, Qt::NoModifier, QPoint(seek->width() / 3, seek->height() / 2));
    MaiFfplayPlaybackStatus playback{};
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&playback) && !playback.seeking && playback.paused, 3000);
    QVERIFY(captureStage(QStringLiteral("02-paused-seek")));
    speed->setCurrentIndex(3);
    volume->setValue(37);
    mute->trigger();
    MaiFfplayAudioStatus audio{sizeof(MaiFfplayAudioStatus), -1, -1};
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&playback) && playback.speed_percent == 150, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetAudioStatus(&audio) == 1 && audio.volume_percent == 37 && audio.muted, 2000);
    QTRY_VERIFY(!player.isPlaying());
    QVERIFY(captureStage(QStringLiteral("03-speed-volume")));
    // Record actual open popup windows while the source stays paused. The
    // capture guard only accepts this process's foreground player and menus.
    bool moreCaptured = false;
    QTimer::singleShot(200, moreMenu, [&] {
        moreCaptured = moreMenu->isVisible() && popupHasVisibleHitTarget(moreMenu) &&
                       captureStage(QStringLiteral("03a-more-menu"));
        QTest::keyClick(moreMenu, Qt::Key_Escape);
    });
    QTest::mouseClick(more, Qt::LeftButton);
    QTRY_VERIFY(moreCaptured);
    QTRY_VERIFY(!moreMenu->isVisible());
    bool speedCaptured = false;
    QTimer::singleShot(200, speed, [&] {
        auto* menu = player.findChild<QMenu*>(QStringLiteral("ffplaySpeedMenu"));
        if (!menu) return;
        speedCaptured =
            menu->isVisible() && popupHasVisibleHitTarget(menu) && captureStage(QStringLiteral("03b-speed-menu"));
        QTest::keyClick(menu, Qt::Key_Escape);
    });
    QTest::mouseClick(speed, Qt::LeftButton);
    QTRY_VERIFY(speedCaptured);
    auto* volumeButton = player.findChild<QToolButton*>(QStringLiteral("ffplayVolumeButton"));
    auto* volumeMenu = player.findChild<QMenu*>(QStringLiteral("ffplayVolumeMenu"));
    QVERIFY(volumeButton && volumeMenu);
    bool volumeCaptured = false;
    QTimer::singleShot(200, volumeMenu, [&] {
        volumeCaptured = volumeMenu->isVisible() && popupHasVisibleHitTarget(volumeMenu) &&
                         captureStage(QStringLiteral("03c-volume-menu"));
        QTest::keyClick(volumeMenu, Qt::Key_Escape);
    });
    QTest::mouseClick(volumeButton, Qt::LeftButton);
    QTRY_VERIFY(volumeCaptured);
    QCOMPARE(speed->currentIndex(), 3);
    QTRY_VERIFY(!player.isPlaying());
    QTest::qWait(600);
    pause->click();
    QTRY_VERIFY(player.isPlaying());
    fullscreen->click();
    QTRY_VERIFY(player.isFullScreen());
    activateOwnTestWindow(&player);
    QTRY_VERIFY(header->isVisible() && panel->isVisible());
    QTest::qWait(350);
    QVERIFY(captureStage(QStringLiteral("04-fullscreen")));
    const bool requireHudBlend = qEnvironmentVariableIntValue("MAICHAT_PLAYER_REQUIRE_HUD_BLEND") != 0;
    const auto headerBackgroundValue = [&] {
        const QImage image = captureOwnTestWindow(&player).toImage();
        if (image.isNull()) return -1;
        const qreal scale = qreal(image.width()) / player.width();
        const QColor pixel =
            image.pixelColor(qRound(player.width() * 0.4 * scale), qRound(header->height() * 0.5 * scale));
        return std::max({pixel.red(), pixel.green(), pixel.blue()});
    };
    const int blendedHeader = requireHudBlend ? headerBackgroundValue() : -1;
    if (requireHudBlend) QVERIFY(blendedHeader > 80 && blendedHeader < 210);
    const QRect videoGeometry = surface->geometry();
    QTRY_VERIFY_WITH_TIMEOUT(panel->isHidden(), 6000);
    QVERIFY(header->isHidden());
    QCOMPARE(surface->geometry(), videoGeometry);
    // QWidget 隐藏状态先于 DWM 合成帧发布；给录屏留出真正无控件的可见片段。
    QTest::qWait(400);
    QVERIFY(captureStage(QStringLiteral("05-fullscreen-hidden")));
    if (requireHudBlend) QVERIFY(headerBackgroundValue() > blendedHeader + 15);
    QTest::mouseMove(player.windowHandle(),
                     player.mapFromGlobal(surface->mapToGlobal(surface->rect().center() + QPoint(20, 0))));
    QTRY_VERIFY(panel->isVisible());
    QVERIFY(header->isVisible());
    QTest::qWait(500);
    QTest::keyClick(&player, Qt::Key_Escape);
    QTRY_VERIFY(!player.isFullScreen());
    QTest::qWait(400);
    QVERIFY(captureStage(QStringLiteral("06-restored")));
    capture.stop();
    captureFrame();
    QVERIFY2(!captureFailed, "Recording stopped because the fixture was obscured, backgrounded, or outside its screen");
    QVERIFY2(!capturedBlankFrame, "A visible video frame was replaced by an empty window background.");
    QVERIFY(frameNumber >= 25);
    player.close();
    QProcess encoder;
    encoder.start(QStringLiteral("ffmpeg"),
                  {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
                   QStringLiteral("-framerate"), QStringLiteral("10"), QStringLiteral("-i"),
                   QDir(framesPath).filePath(QStringLiteral("frame-%05d.png")), QStringLiteral("-c:v"),
                   QStringLiteral("libx264"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
                   QStringLiteral("-movflags"), QStringLiteral("+faststart"), QStringLiteral("-y"),
                   QDir(output).filePath(QStringLiteral("player-interactions.mp4"))});
    QVERIFY2(encoder.waitForFinished(30000), qPrintable(encoder.errorString()));
    QVERIFY2(encoder.exitCode() == 0, encoder.readAllStandardError().constData());
}

void MaiFfplayVideoDialogTest::seeksToDecodedTargetFrame_data() {
    QTest::addColumn<double>("speed");
    QTest::addColumn<bool>("customInput");
    QTest::addColumn<bool>("hdr");
    QTest::addColumn<bool>("audio");
    QTest::addColumn<int64_t>("initialUs");
    QTest::addColumn<int64_t>("sourceOffsetUs");
    QTest::newRow("sdr") << 1.0 << false << false << false << int64_t{0} << int64_t{0};
    QTest::newRow("sdr-audio-half-speed") << 0.5 << false << false << true << int64_t{0} << int64_t{0};
    QTest::newRow("sdr-audio-double-speed") << 2.0 << false << false << true << int64_t{0} << int64_t{0};
    QTest::newRow("cloud-byte-source") << 1.0 << true << false << true << int64_t{0} << int64_t{0};
    QTest::newRow("initial-source-time") << 1.5 << false << false << true << int64_t{1'360'000} << int64_t{0};
    QTest::newRow("hlg-native") << 1.0 << false << true << true << int64_t{0} << int64_t{0};
    QTest::newRow("hlg-cloud-half-speed") << 0.5 << true << true << true << int64_t{0} << int64_t{0};
    QTest::newRow("nonzero-start-time") << 1.5 << true << false << false << int64_t{0} << int64_t{1'280'000};
}

void MaiFfplayVideoDialogTest::seeksToDecodedTargetFrame() {
    QFETCH(double, speed);
    QFETCH(bool, customInput);
    QFETCH(bool, hdr);
    QFETCH(bool, audio);
    QFETCH(int64_t, initialUs);
    QFETCH(int64_t, sourceOffsetUs);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString file = directory.filePath(QStringLiteral("long-gop.mp4"));
    QProcess generate;
    // 每帧亮度编码源帧号，整个八秒 GOP 只有第一帧是关键帧。
    // 验证最终提交的画面，而不是命令入队或被主时钟推算出来的位置。
    QStringList arguments{
        QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-f"), QStringLiteral("lavfi"),
        QStringLiteral("-i"), QStringLiteral("nullsrc=s=160x90:r=25:d=8,geq=lum='16+N':cb=128:cr=128")};
    if (audio)
        arguments << QStringLiteral("-f") << QStringLiteral("lavfi") << QStringLiteral("-i")
                  << QStringLiteral("sine=frequency=440:sample_rate=48000:duration=8") << QStringLiteral("-c:a")
                  << QStringLiteral("aac");
    else
        arguments << QStringLiteral("-an");
    if (hdr)
        arguments << QStringLiteral("-c:v") << QStringLiteral("libx265") << QStringLiteral("-x265-params")
                  << QStringLiteral(
                         "log-level=error:pools=1:frame-threads=1:lossless=1:keyint=200:min-keyint=200:scenecut=0:"
                         "colorprim=9:transfer=18:colormatrix=9")
                  << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p10le") << QStringLiteral("-tag:v")
                  << QStringLiteral("hvc1");
    else
        arguments << QStringLiteral("-c:v") << QStringLiteral("libx264") << QStringLiteral("-crf")
                  << QStringLiteral("1") << QStringLiteral("-g") << QStringLiteral("200")
                  << QStringLiteral("-keyint_min") << QStringLiteral("200") << QStringLiteral("-sc_threshold")
                  << QStringLiteral("0") << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p");
    if (sourceOffsetUs)
        arguments << QStringLiteral("-output_ts_offset") << QString::number(sourceOffsetUs / 1'000'000.0);
    arguments << QStringLiteral("-y") << file;
    generate.start(QStringLiteral("ffmpeg"), arguments);
    QVERIFY(generate.waitForFinished(15000));
    QCOMPARE(generate.exitCode(), 0);
    struct Observation {
        std::atomic<int> luma{-1};
        std::atomic<int> firstLuma{-1};
        std::atomic<int> expectedLuma{-1};
        std::atomic<bool> matched{false};
        std::atomic<unsigned> frames{0};
        QFile input;
    } observation;
    observation.input.setFileName(file);
    QVERIFY(observation.input.open(QIODevice::ReadOnly));
    MaiFfplayHost host{};
    host.graphics_view_id = 1;
    host.user_data = &observation;
    host.present_native_frames = hdr ? 1 : 0;
    if (customInput) {
        host.input_size_bytes = observation.input.size();
        host.read_input_at = [](void* context, uint64_t offset, uint8_t* output, int capacity) {
            auto& input = static_cast<Observation*>(context)->input;
            if (!input.seek(static_cast<qint64>(offset))) return -1;
            return static_cast<int>(input.read(reinterpret_cast<char*>(output), capacity));
        };
    }
    host.present_video = [](void* context, const MaiVideoFrame* frame, const MaiVideoSubtitle*) {
        auto* output = static_cast<Observation*>(context);
        if (!frame) return false;
        int luma = -1;
        if (frame->format == MAI_VIDEO_PIXEL_I420 || frame->format == MAI_VIDEO_PIXEL_NV12)
            luma = frame->data[0][0];
        else if (frame->format == MAI_VIDEO_PIXEL_P010 || frame->format == MAI_VIDEO_PIXEL_I010) {
            uint16_t value = 0;
            std::memcpy(&value, frame->data[0], sizeof(value));
            luma = value >> (frame->format == MAI_VIDEO_PIXEL_P010 ? 8 : 2);
        } else
            return false;
        output->luma = luma;
        if (output->frames.load() == 0) output->firstLuma = luma;
        if (std::abs(luma - output->expectedLuma.load()) <= 1) output->matched = true;
        ++output->frames;
        return true;
    };
    const QByteArray path = file.toUtf8();
    std::thread playback([&] {
        char name[] = "ffplay";
        char native[] = "-enable_vulkan";
        char hardware[] = "-hwaccel";
        char videotoolbox[] = "videotoolbox";
        char* arguments[] = {name, const_cast<char*>(path.constData())};
        char* nativeArguments[] = {name, native, hardware, videotoolbox, const_cast<char*>(path.constData())};
        maiFfplayRunAtSpeed(&host, hdr ? 5 : 2, hdr ? nativeArguments : arguments, speed, initialUs);
    });
    // 即使断言失败也先关闭工作线程，避免测试因 std::thread 析构而中止。
    const auto cleanup = qScopeGuard([&] {
        maiFfplayRequestQuit();
        playback.join();
    });
    MaiFfplayPlaybackStatus status{};
    QTRY_VERIFY_WITH_TIMEOUT(observation.frames.load() > 0, 5000);
    QVERIFY(std::abs(observation.firstLuma.load() - (16 + initialUs / 40'000)) <= 1);
    QCOMPARE(maiFfplayRequestPauseToggle(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&status) && status.paused, 2000);
    for (int64_t target : {5'240'000, 1'160'000, 6'655'000, 8'000'000, 0}) {
        const int64_t expectedFrame = std::min<int64_t>(target / 40'000, 199);
        const int expected = 16 + static_cast<int>(expectedFrame);
        observation.expectedLuma = expected;
        observation.matched = false;
        QCOMPARE(maiFfplaySeekAbsoluteUs(target), 1);
        QTRY_VERIFY_WITH_TIMEOUT(observation.matched.load(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&status) && status.paused, 2000);
        qInfo() << "paused seek target us" << target << "decoded frame" << observation.luma.load() - 16 << "status us"
                << status.position_us;
        QVERIFY(std::abs(observation.luma.load() - expected) <= 1);
        // setpts 以滤镜 time_base 量化，1.5 倍速可能有不到 0.1 ms 的舍入；
        // 状态必须属于已呈现的源帧，而不是显示请求值掩盖关键帧落点。
        if (target == status.duration_us) {
            // EOF 进度在时长端点，最后呈现的仍须是上面验证的末帧。
            QTRY_VERIFY_WITH_TIMEOUT(
                maiFfplayGetPlaybackStatus(&status) && status.paused && status.position_us == status.duration_us, 2000);
        } else {
            QCOMPARE(std::llround(status.position_us / 40'000.0), expectedFrame);
        }
    }
    observation.expectedLuma = 16 + 131;
    observation.matched = false;
    for (int64_t target : {6'000'000, 2'000'000, 7'000'000, 5'240'000}) QCOMPARE(maiFfplaySeekAbsoluteUs(target), 1);
    QTRY_VERIFY_WITH_TIMEOUT(observation.matched.load(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&status) && status.paused, 2000);
    observation.expectedLuma = 16;
    observation.matched = false;
    QCOMPARE(maiFfplaySendCommand("seek_backward"), 1);
    QTRY_VERIFY_WITH_TIMEOUT(observation.matched.load(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&status) && status.paused && status.position_us < 1000, 2000);
    observation.expectedLuma = 16 + 199;
    observation.matched = false;
    QCOMPARE(maiFfplaySendCommand("seek_forward"), 1);
    QTRY_VERIFY_WITH_TIMEOUT(observation.matched.load(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&status) && status.paused, 2000);
    observation.expectedLuma = 16 + 100;
    observation.matched = false;
    QCOMPARE(maiFfplaySeekPercent(0.5), 1);
    QTRY_VERIFY_WITH_TIMEOUT(observation.matched.load(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&status) && status.paused, 2000);
    QCOMPARE(maiFfplayRequestPauseToggle(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&status) && !status.paused, 2000);
    observation.expectedLuma = 16 + 53;
    observation.matched = false;
    QCOMPARE(maiFfplaySeekAbsoluteUs(2'120'000), 1);
    QTRY_VERIFY_WITH_TIMEOUT(observation.matched.load(), 5000);
    QVERIFY(maiFfplayGetPlaybackStatus(&status) && !status.paused);
}

void MaiFfplayVideoDialogTest::sliderSeeksAtPointerPosition() {
    MaiFfplayVideoDialog player(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE));
    showOwnTestWindow(&player);
#if defined(Q_OS_MAC)
    activateFfplayTestWindow(&player);
    QTest::qWait(300);
    if (!ffplayTestWindowInteractive(&player)) QSKIP("Test window cannot receive foreground native pointer events");
#endif
    QTRY_VERIFY_WITH_TIMEOUT(player.presentedFrames() > 0, 5000);
    auto* slider = player.findChild<QSlider*>(QStringLiteral("ffplaySeekSlider"));
    QVERIFY(slider);
    MaiFfplayPlaybackStatus status{};
    QVERIFY(player.sendCommand(QStringLiteral("pause")));
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&status) && status.paused, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(slider->maximum() >= 2000, 2000);
    const auto nearThreeQuarters = [&] {
        return maiFfplayGetPlaybackStatus(&status) && status.paused &&
               std::abs(status.position_us - status.duration_us * 0.75) < 120'000;
    };
    QTest::mouseClick(slider, Qt::LeftButton, Qt::NoModifier, QPoint(slider->width() * 3 / 4, slider->height() / 2));
    QTRY_VERIFY_WITH_TIMEOUT(nearThreeQuarters(), 5000);
    QVERIFY(player.seekSeconds(0));
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&status) && status.position_us < 100'000, 3000);
    // 拖动期间保持滑块位置，释放后提交最终目标；复用云盘独立窗和本地视频控件。
    QTest::mousePress(slider, Qt::LeftButton, Qt::NoModifier, QPoint(slider->width() / 4, slider->height() / 2));
    QTest::mouseMove(slider, QPoint(slider->width() * 3 / 4, slider->height() / 2));
    QTest::mouseRelease(slider, Qt::LeftButton, Qt::NoModifier, QPoint(slider->width() * 3 / 4, slider->height() / 2));
    QTRY_VERIFY_WITH_TIMEOUT(nearThreeQuarters(), 5000);
    const auto* time = player.findChild<QLabel*>(QStringLiteral("ffplayTimeLabel"));
    QTRY_VERIFY_WITH_TIMEOUT(time && time->text() == QStringLiteral("0:01"), 2000);
    const QString screenshot = qEnvironmentVariable("MAICHAT_FFPLAY_SEEK_CAPTURE");
    if (!screenshot.isEmpty()) {
        activateOwnTestWindow(&player);
        QTRY_VERIFY_WITH_TIMEOUT(ownTestWindowIsUnobscured(&player), 2500);
        const QPixmap pixels = captureOwnTestWindow(&player);
        QVERIFY(!pixels.isNull() && pixels.save(screenshot));
    }
    player.close();
    QVERIFY(!player.isStarted());
}

void MaiFfplayVideoDialogTest::sliderKeepsTargetUntilSeekIsApplied_data() {
    QTest::addColumn<bool>("paused");
    QTest::addColumn<bool>("backward");
    QTest::addColumn<bool>("replacePending");
    QTest::newRow("paused-forward") << true << false << false;
    QTest::newRow("playing-forward") << false << false << false;
    QTest::newRow("paused-backward") << true << true << false;
    QTest::newRow("playing-backward") << false << true << false;
    QTest::newRow("paused-latest-request") << true << false << true;
    QTest::newRow("playing-latest-request") << false << false << true;
}

void MaiFfplayVideoDialogTest::sliderKeepsTargetUntilSeekIsApplied() {
    QFETCH(bool, paused);
    QFETCH(bool, backward);
    QFETCH(bool, replacePending);
    struct SeekBarrier {
        std::atomic<bool> entered{false};
        std::atomic<bool> release{false};
    } barrier;
    MaiFfplayVideoDialog player(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE), nullptr, backward ? 1'000'000 : 0);
    // 暂停事件线程以确定性覆盖请求已提交、解码 seek 尚未执行的异步窗口。
    // 使用现有诊断回调，正式播放器不添加延迟或测试入口。
    maiFfplaySetDiagnosticSink(
        [](void* context, int, const char* line) {
            if (!line || !std::strstr(line, "Seek to ")) return;
            auto* state = static_cast<SeekBarrier*>(context);
            state->entered = true;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!state->release && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        },
        &barrier);
    const auto cleanup = qScopeGuard([&] {
        barrier.release = true;
        player.close();
        maiFfplaySetDiagnosticSink(nullptr, nullptr);
    });
    showOwnTestWindow(&player);
    QTRY_VERIFY_WITH_TIMEOUT(player.presentedFrames() > 0, 5000);
    MaiFfplayPlaybackStatus status{};
    if (paused) {
        QVERIFY(player.sendCommand(QStringLiteral("pause")));
        QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&status) && status.paused, 2000);
    }
    auto* slider = player.findChild<QSlider*>(QStringLiteral("ffplaySeekSlider"));
    QVERIFY(slider);
    QTRY_VERIFY_WITH_TIMEOUT(slider->maximum() >= 2000, 2000);
    QTest::mouseClick(slider, Qt::LeftButton, Qt::NoModifier,
                      QPoint(slider->width() * (backward ? 1 : 3) / 4, slider->height() / 2));
    int requested = slider->value();
    QTRY_VERIFY_WITH_TIMEOUT(barrier.entered.load(), 1000);
    QTest::qWait(400);  // 至少经过一次正式 250 ms 进度刷新。
    qInfo() << "pending seek slider requested" << requested << "displayed" << slider->value();
    QVERIFY2(std::abs(slider->value() - requested) <= 1,
             "old playback status moved the thumb away from the requested seek position");
    QVERIFY(maiFfplayGetPlaybackStatus(&status) && status.seeking);
    if (replacePending) {
        QTest::mouseClick(slider, Qt::LeftButton, Qt::NoModifier, QPoint(slider->width() / 4, slider->height() / 2));
        requested = slider->value();
        QTest::qWait(400);
        QVERIFY(std::abs(slider->value() - requested) <= 1);
        QVERIFY(maiFfplayGetPlaybackStatus(&status) && status.seeking);
        QVERIFY(std::abs(status.seek_target_us - requested * 1000) < 2000);
    }
    barrier.release = true;
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&status) && !status.seeking &&
                                 std::abs(status.position_us - requested * 1000) < 100'000,
                             3000);
}

void MaiFfplayVideoDialogTest::endedVideoShowsPlayAndSeeksWithoutRunning() {
    MaiFfplayVideoDialog player(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE));
    showOwnTestWindow(&player);
    MaiFfplayPlaybackStatus status{};
    QTRY_VERIFY_WITH_TIMEOUT(player.presentedFrames() > 0, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&status) && status.duration_us > 0 &&
                                 status.position_us >= status.duration_us - 50'000,
                             5000);
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&status) && status.paused, 1500);
    QTRY_VERIFY_WITH_TIMEOUT(!player.isPlaying(), 1000);
    const uint64_t endedFrames = player.presentedFrames();
    QVERIFY(player.seekSeconds(0.5));
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&status) && !status.seeking && status.paused &&
                                 std::abs(status.position_us - 500'000) < 50'000,
                             3000);
    QTRY_VERIFY_WITH_TIMEOUT(player.presentedFrames() > endedFrames, 3000);
    const auto pausedFrames = player.presentedFrames();
    QTest::qWait(300);
    QCOMPARE(player.presentedFrames(), pausedFrames);
    QVERIFY(player.sendCommand(QStringLiteral("pause")));
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&status) && !status.paused, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(player.presentedFrames() > pausedFrames, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(
        maiFfplayGetPlaybackStatus(&status) && status.paused && status.position_us == status.duration_us, 4000);
    QVERIFY(player.sendCommand(QStringLiteral("pause")));
    QTRY_VERIFY_WITH_TIMEOUT(
        maiFfplayGetPlaybackStatus(&status) && !status.paused && !status.seeking && status.position_us < 500'000, 3000);
    player.close();
}

void MaiFfplayVideoDialogTest::hdrPlaybackKeepsTransferMetadata_data() {
    QTest::addColumn<QString>("transfer");
    QTest::addColumn<QString>("sourceFile");
    QTest::newRow("hlg") << QStringLiteral("arib-std-b67") << QString();
    QTest::newRow("pq") << QStringLiteral("smpte2084") << QString();
    const QString original = qEnvironmentVariable("MAICHAT_TEST_HDR_VIDEO");
    if (!original.isEmpty()) QTest::newRow("original-hdr") << QStringLiteral("arib-std-b67") << original;
}

void MaiFfplayVideoDialogTest::hdrPlaybackKeepsTransferMetadata() {
    QFETCH(QString, transfer);
    QFETCH(QString, sourceFile);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString file = sourceFile.isEmpty() ? directory.filePath(QStringLiteral("hdr.mov")) : sourceFile;
    QProcess generate;
    // 合成素材验证正式 HDR 播放路径；私有现场原片只通过可选环境变量加入。
    if (sourceFile.isEmpty()) {
        generate.start(QStringLiteral("ffmpeg"),
                       {QStringLiteral("-v"),
                        QStringLiteral("error"),
                        QStringLiteral("-f"),
                        QStringLiteral("lavfi"),
                        QStringLiteral("-i"),
                        QStringLiteral("testsrc2=size=192x108:rate=24:duration=2"),
                        QStringLiteral("-vf"),
                        QStringLiteral("format=yuv420p10le"),
                        QStringLiteral("-c:v"),
                        QStringLiteral("libx265"),
                        QStringLiteral("-preset"),
                        QStringLiteral("ultrafast"),
                        QStringLiteral("-x265-params"),
                        QStringLiteral("log-level=error:pools=1:frame-threads=1:colorprim=9:transfer=%1:colormatrix=9")
                            .arg(transfer == QStringLiteral("arib-std-b67") ? 18 : 16),
                        QStringLiteral("-color_primaries"),
                        QStringLiteral("bt2020"),
                        QStringLiteral("-colorspace"),
                        QStringLiteral("bt2020nc"),
                        QStringLiteral("-color_trc"),
                        transfer,
                        QStringLiteral("-tag:v"),
                        QStringLiteral("hvc1"),
                        QStringLiteral("-an"),
                        QStringLiteral("-y"),
                        file});
        QVERIFY(generate.waitForFinished(15000));
        QCOMPARE(generate.exitCode(), 0);
    }
    MaiFfplayHdrSourceInfo source{};
    QVERIFY(maiFfplayProbeHdrSource(file.toUtf8().constData(), &source));
    QVERIFY(source.native_frame_eligible);
    MaiFfplayVideoDialog player(file);
    // 使用组件自己的共享窗口外壳，避免测试重新引入已经移除的系统标题栏。
    showOwnTestWindow(&player);
    player.raise();
    player.activateWindow();
    QVERIFY(player.isStarted());
    QTRY_VERIFY_WITH_TIMEOUT(player.hasPresentedHdrFrame(), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(player.hasPresentedFrame(), 30000);
#if defined(Q_OS_MAC)
    // MOV/MP4 由 AVPlayer 保留逐帧显示映射；原生首帧不能伪装成 FFplay 提交计数。
    QVERIFY(source.is_iso_base_media);
    QCOMPARE(player.presentedFrames(), uint64_t{0});
    MaiFfplayPlaybackStatus status{};
    QTRY_VERIFY_WITH_TIMEOUT(player.playbackStatus(&status) && status.duration_us > 0, 5000);
    if (!status.paused) QVERIFY(player.sendCommand(QStringLiteral("pause")));
    QTRY_VERIFY_WITH_TIMEOUT(player.playbackStatus(&status) && status.paused, 2000);
    QVERIFY(player.setSpeed(1.5));
    QVERIFY(player.playbackStatus(&status));
    QVERIFY(status.paused);
    QCOMPARE(status.speed_percent, 150);
    // The reconstructed HUD must control the actual native HDR player, and
    // subsequent Agent commands must be reflected back without replaying UI intent.
    auto* volume = player.findChild<QSlider*>(QStringLiteral("ffplayVolumeSlider"));
    auto* mute = player.findChild<QAction*>(QStringLiteral("ffplayMuteAction"));
    QVERIFY(volume && mute);
    volume->setValue(37);
    QVERIFY(player.sendCommand(QStringLiteral("volume_up")));
    QTRY_COMPARE(volume->value(), 47);
    mute->trigger();
    QVERIFY(player.sendCommand(QStringLiteral("mute")));
    QTRY_VERIFY(!mute->isChecked());
    const int64_t target = status.duration_us / 2;
    QVERIFY(player.seekSeconds(target / 1'000'000.0));
    QTRY_VERIFY_WITH_TIMEOUT(player.playbackStatus(&status) && !status.seeking && status.paused &&
                                 std::abs(status.position_us - target) < 50'000,
                             5000);
    const int64_t pausedPosition = status.position_us;
    // 使用 GUI/工具共用入口；原生异步转场期间仍应保留暂停、进度和倍速。
    const QRect beforeFullscreen = player.geometry();
    QVERIFY(player.sendCommand(QStringLiteral("fullscreen")));
    QTRY_VERIFY(player.isFullScreen());
    QVERIFY(player.sendCommand(QStringLiteral("fullscreen")));
    QTRY_VERIFY(!player.isFullScreen());
    QTRY_COMPARE(player.geometry(), beforeFullscreen);
    QVERIFY(player.playbackStatus(&status));
    QVERIFY(status.paused);
    QVERIFY(std::abs(status.position_us - pausedPosition) < 1000);
    QCOMPARE(status.speed_percent, 150);

    // 到尾自动暂停，仍允许暂停 seek 和重播；状态读取与 Agent 使用同一个接口。
    QVERIFY(player.seekSeconds((status.duration_us - 250'000) / 1'000'000.0));
    QTRY_VERIFY_WITH_TIMEOUT(player.playbackStatus(&status) && !status.seeking, 5000);
    QVERIFY(player.sendCommand(QStringLiteral("pause")));
    QTRY_VERIFY_WITH_TIMEOUT(
        player.playbackStatus(&status) && status.paused && std::abs(status.position_us - status.duration_us) < 50'000,
        3000);
    QVERIFY(player.replay());
    QTRY_VERIFY_WITH_TIMEOUT(
        player.playbackStatus(&status) && !status.paused && !status.seeking && status.position_us < target, 5000);
#endif
    player.close();
    QTRY_VERIFY(!player.isStarted());
#if defined(Q_OS_MAC)
    // 关闭可先于探测完成和 GUI 打开回调；过期回调不能重新打开播放或保留图层。
    MaiFfplayVideoDialog closing(file);
    closing.show();
    closing.close();
    QTest::qWait(150);
    QVERIFY(!closing.isStarted());
#endif
}

void MaiFfplayVideoDialogTest::opensAtRequestedSourceTime() {
    QWidget host;
    MaiToolRegistry registry;
    registerDesktopMediaTools(registry, &host);
    MaiTool* openTool = registry.find("maichat_play_video");
    QVERIFY(openTool != nullptr);
    MaiTool* cameraTool = registry.find("maichat_camera_command");
    QVERIFY(cameraTool != nullptr);
    QVERIFY(!cameraTool->requiresApproval("{}"));
    QVERIFY(!cameraTool->requiresPerCallApproval("{}"));
    MaiToolContext emptyContext;
    QVERIFY(cameraTool->execute(R"({"action":"open"})", emptyContext).hasError());
    MaiTool* musicTool = registry.find("maichat_play_local_music");
    QVERIFY(musicTool != nullptr);
    QVERIFY(!musicTool->requiresApproval("{}"));
    QVERIFY(!musicTool->requiresPerCallApproval("{}"));
    QVERIFY(musicTool->execute(R"({"path":"song.mp3"})", emptyContext).hasError());
    QVERIFY(QString::fromStdString(openTool->parametersSchema()).contains(QStringLiteral("start_seconds")));
    MaiToolContext context;
    const QFileInfo fixture(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE));
    context.root = fixture.dir().absolutePath().toStdString();
    QVERIFY(openTool->execute(R"({"path":"unused.mp4","start_seconds":-1})", context).hasError());
    QVERIFY(openTool->execute(R"({"path":"unused.mp4","start_seconds":"1"})", context).hasError());
    const MaiToolResult opened = openTool->execute(
        QStringLiteral("{\"path\":\"%1\",\"start_seconds\":1.0}").arg(fixture.fileName()).toStdString(), context);
    QVERIFY2(!opened.hasError(), opened.error().message().c_str());
    const auto result = QJsonDocument::fromJson(QByteArray::fromStdString(opened.output())).object();
    QCOMPARE(result.value(QStringLiteral("start_seconds")).toDouble(), 1.0);
    QPointer<MaiFfplayVideoDialog> dialog = MaiFfplayVideoDialog::activeDialog();
    QVERIFY(dialog);
    MaiFfplayPlaybackStatus status{};
    int64_t firstPosition = -1;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 4000 && firstPosition < 0) {
        QCoreApplication::processEvents();
        if (maiFfplayGetPlaybackStatus(&status) && status.position_us > 0)
            firstPosition = status.position_us;
        else
            QTest::qWait(10);
    }
    QVERIFY2(firstPosition >= 500'000, "playback started before the requested source time");
    dialog->close();
    QTRY_VERIFY(dialog.isNull());
}

void MaiFfplayVideoDialogTest::agentVideoUsesIndependentWindow() {
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("agent-video-window-test"), std::move(client));
    MainWindow window(app);
    window.resize(1200, 800);
    showOwnTestWindow(&window);
    auto* stack = window.findChild<QStackedWidget*>(QStringLiteral("contentStack"));
    QVERIFY(stack);
    QCoreApplication::processEvents();
    const QSize previousSize = stack->size();
    MaiToolRegistry registry;
    registerDesktopMediaTools(registry, &window);
    MaiToolContext context;
    const QFileInfo fixture(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE));
    context.root = fixture.dir().absolutePath().toStdString();
    const auto opened =
        registry.find("maichat_play_video")
            ->execute(QStringLiteral("{\"path\":\"%1\"}").arg(fixture.fileName()).toStdString(), context);
    QVERIFY2(!opened.hasError(), opened.error().message().c_str());
    auto player = MaiFfplayVideoDialog::activeDialog();
    QVERIFY(player && player->isWindow());
    QVERIFY(player->windowFlags().testFlag(Qt::FramelessWindowHint));
    auto* header = player->findChild<QWidget*>(QStringLiteral("ffplayTitleBar"));
    auto* panel = player->findChild<QWidget*>(QStringLiteral("ffplayControlPanel"));
    auto* surface = player->findChild<QWidget*>(QStringLiteral("ffplayVideoSurface"));
    auto* slider = player->findChild<QSlider*>(QStringLiteral("ffplaySeekSlider"));
    auto* more = player->findChild<QToolButton*>(QStringLiteral("videoMoreButton"));
    auto* menu = player->findChild<QMenu*>(QStringLiteral("ffplayMoreMenu"));
    auto* save = player->findChild<QAction*>(QStringLiteral("videoSaveAction"));
#if defined(Q_OS_MAC)
    auto* share = player->findChild<QAction*>(QStringLiteral("videoShareAction"));
#else
    auto* share = player->findChild<QAction*>(QStringLiteral("videoForwardAction"));
#endif
    QVERIFY(header && panel && surface && slider && more && menu && save && share);
    QVERIFY(header->isAncestorOf(more));
    QVERIFY(menu->actions().contains(share) && menu->actions().contains(save));
    QTRY_COMPARE(stack->size(), previousSize);
    auto* slot = window.findChild<QWidget*>(QStringLiteral("videoDockSlot"));
    QVERIFY(slot && !slot->isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(player->presentedFrames() > 0, 10000);
    const QPoint position = player->pos();
    window.resize(1100, 740);
    QTest::qWait(100);
    QCOMPARE(player->pos(), position);
    player->resize(1000, 660);
    player->move(player->screen()->availableGeometry().topLeft() + QPoint(12, 36));
    activateOwnTestWindow(player);
    QTRY_VERIFY(panel->isVisible() && slider->isVisible() && more->isVisible());
    QTRY_VERIFY(slider->width() > panel->width() * 0.65);
    // 窗口态与全屏复用覆盖完整视频视口的持久 HUD，不能再给控制栏预留黑色区域。
    QCOMPARE(surface->geometry(), player->rect());
    QCOMPARE(header->mapToGlobal(QPoint()), surface->mapToGlobal(QPoint()));
    QVERIFY(surface->mapToGlobal(surface->rect().bottomLeft()).y() >= panel->mapToGlobal(QPoint()).y());
    const QString screenshot = qEnvironmentVariable("MAICHAT_AGENT_VIDEO_SCREENSHOT");
    if (!screenshot.isEmpty()) {
        QTRY_VERIFY_WITH_TIMEOUT(ownTestWindowIsUnobscured(player), 2500);
        const QPixmap pixels = captureOwnTestWindow(player);
        QVERIFY2(!pixels.isNull(), "test player is not foreground and unobscured");
        QVERIFY(pixels.save(screenshot));
        QVERIFY(panel->grab().save(screenshot + QStringLiteral("-panel.png")));
    }
    bool menuFitsScreen = false;
    QTimer dismissMenu;
    dismissMenu.setSingleShot(true);
    connect(&dismissMenu, &QTimer::timeout, [&] {
        menuFitsScreen = menu->isVisible() && menu->screen() &&
                         menu->screen()->availableGeometry().contains(menu->geometry()) && !menu->actions().isEmpty();
        if (!screenshot.isEmpty()) menu->grab().save(screenshot + QStringLiteral("-menu-widget.png"));
        menu->close();
    });
    dismissMenu.start(200);
    more->click();
    dismissMenu.stop();
    QVERIFY(menuFitsScreen);
    player->resize(640, 420);
    QTRY_VERIFY(slider->width() > panel->width() * 0.65);
    QVERIFY(more->isVisible() && share->isVisible() && save->isVisible());
    if (!screenshot.isEmpty()) {
        QTest::qWait(150);
        const QPixmap pixels = captureOwnTestWindow(player);
        QVERIFY(!pixels.isNull() && pixels.save(screenshot + QStringLiteral("-compact.png")));
    }
    player->showFullScreen();
    QTRY_VERIFY(player->isFullScreen());
    activateOwnTestWindow(player);
    QTRY_VERIFY(header->isVisible());
    QTRY_VERIFY(slider->width() > player->width() * 0.70);
    if (!screenshot.isEmpty()) {
        QTest::qWait(350);
        const QPixmap pixels = captureOwnTestWindow(player);
        QVERIFY(!pixels.isNull() && pixels.save(screenshot + QStringLiteral("-fullscreen.png")));
    }
    QTest::keyClick(player, Qt::Key_Escape);
    QTRY_VERIFY(!player->isFullScreen());
    QVERIFY(player->isVisible());
    player->close();
    QTRY_VERIFY(!MaiFfplayVideoDialog::activeDialog());
    QVERIFY(window.isVisible());
}

void MaiFfplayVideoDialogTest::agentRepeatedTargetDoesNotReplayFrames() {
    QTemporaryDir directory;
    const QString file = directory.filePath(QStringLiteral("agent-target.mp4"));
    QProcess encoder;
    encoder.start(
        QStringLiteral("ffmpeg"),
        {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-f"),
         QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("testsrc2=size=320x180:rate=10"),
         QStringLiteral("-t"), QStringLiteral("20"), QStringLiteral("-c:v"), QStringLiteral("libx264"),
         QStringLiteral("-preset"), QStringLiteral("ultrafast"), file});
    QVERIFY(encoder.waitForFinished(30000));
    QCOMPARE(encoder.exitCode(), 0);
    QWidget host;
    MaiToolRegistry registry;
    registerDesktopMediaTools(registry, &host);
    MaiToolContext context;
    context.root = directory.path().toStdString();
    context.sessionId = "video-repeat-session";
    context.messageId = "video-repeat-turn";
    const auto opened =
        registry.find("maichat_play_video")->execute(R"({"path":"agent-target.mp4","start_seconds":5})", context);
    QVERIFY2(!opened.hasError(), opened.error().message().c_str());
    auto player = MaiFfplayVideoDialog::activeDialog();
    QVERIFY(player);
    MaiFfplayPlaybackStatus status{};
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&status) && status.position_us >= 5'000'000, 15000);
    QTRY_VERIFY_WITH_TIMEOUT(player->presentedFrames() > 0, 10000);
    auto* command = registry.find("maichat_video_command");
    for (int attempt = 0; attempt < 3; ++attempt) {
        context.partId = "repeat-part-" + std::to_string(attempt);
        QTest::qWait(700);
        QVERIFY(maiFfplayGetPlaybackStatus(&status));
        const auto before = status.position_us;
        const auto result = command->execute(R"({"action":"seek_seconds","seconds":5})", context);
        QVERIFY2(!result.hasError(), result.error().message().c_str());
        const auto output = QJsonDocument::fromJson(QByteArray::fromStdString(result.output())).object();
        QTest::qWait(250);
        QVERIFY(maiFfplayGetPlaybackStatus(&status));
        QVERIFY2(status.position_us + 100'000 >= before, "Duplicate target replayed earlier video frames");
        QVERIFY2(output.value(QStringLiteral("deduplicated")).toBool(), "Same-turn target was executed twice");
    }
    const auto observed = command->execute(R"({"action":"status"})", context);
    QVERIFY(!observed.hasError());
    const auto snapshot = QJsonDocument::fromJson(QByteArray::fromStdString(observed.output())).object();
    QVERIFY(snapshot.value(QStringLiteral("position_seconds")).toDouble() > 6);
    QVERIFY(snapshot.value(QStringLiteral("ready")).toBool());
    QVERIFY(!snapshot.contains(QStringLiteral("accepted")));
    const auto generation = player->navigationGeneration();
    QVERIFY(!command->execute(R"({"action":"status"})", context).hasError());
    QCOMPARE(player->navigationGeneration(), generation);
    // 同轮明确要求再回到该位置仍可执行，不能把去重变成禁止重新观看。
    const auto forced = command->execute(R"({"action":"seek_seconds","seconds":5,"force_seek":true})", context);
    QVERIFY(!forced.hasError());
    QVERIFY(!QJsonDocument::fromJson(QByteArray::fromStdString(forced.output()))
                 .object()
                 .value(QStringLiteral("deduplicated"))
                 .toBool());
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&status) && !status.seeking &&
                                 status.position_us >= 5'000'000 && status.position_us < 6'000'000,
                             3000);
    // 用户手动拖动后，原回合再定位属于新的导航；百分比与绝对秒数按同一目标识别。
    QVERIFY(player->seekSeconds(7));
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&status) && !status.seeking && status.position_us >= 7'000'000,
                             3000);
    const auto afterManual = command->execute(R"({"action":"seek_seconds","seconds":5})", context);
    QVERIFY(!afterManual.hasError());
    QVERIFY(!QJsonDocument::fromJson(QByteArray::fromStdString(afterManual.output()))
                 .object()
                 .value(QStringLiteral("deduplicated"))
                 .toBool());
    const auto percent = command->execute(R"({"action":"seek_percent","percent":25})", context);
    QVERIFY(!percent.hasError());
    QVERIFY(QJsonDocument::fromJson(QByteArray::fromStdString(percent.output()))
                .object()
                .value(QStringLiteral("deduplicated"))
                .toBool());
    QVERIFY(command->execute(R"({"action":"seek_seconds","seconds":5,"force_seek":"true"})", context).hasError());
    // 新用户回合仍可明确回到同一时间；不能用固定时间去抖吞掉真实需求。
    context.messageId = "video-repeat-next-turn";
    const auto again = command->execute(R"({"action":"seek_seconds","seconds":5})", context);
    QVERIFY(!again.hasError());
    QVERIFY(!QJsonDocument::fromJson(QByteArray::fromStdString(again.output()))
                 .object()
                 .value(QStringLiteral("deduplicated"))
                 .toBool());
    player->close();
    const auto closed = command->execute(R"({"action":"status"})", context);
    QVERIFY(!closed.hasError());
    QCOMPARE(QJsonDocument::fromJson(QByteArray::fromStdString(closed.output()))
                 .object()
                 .value(QStringLiteral("state"))
                 .toString(),
             QStringLiteral("closed"));
}

void MaiFfplayVideoDialogTest::controlsHideAndKeepVisibleForPause() {
    const QString file = QFileInfo(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE))
                             .dir()
                             .filePath(QStringLiteral("../../../desktop/tests/fixtures/video-circle-61s.mp4"));
    QVERIFY(QFileInfo::exists(file));
    MaiFfplayVideoDialog player(file);
    showOwnTestWindow(&player);
    QTRY_VERIFY_WITH_TIMEOUT(player.presentedFrames() > 0, 10000);
    player.showFullScreen();
    QTRY_VERIFY(player.isFullScreen());
    auto* surface = player.findChild<QWidget*>(QStringLiteral("ffplayVideoSurface"));
    auto* panel = player.findChild<QWidget*>(QStringLiteral("ffplayControlPanel"));
    auto* header = player.findChild<QWidget*>(QStringLiteral("ffplayTitleBar"));
    auto* pause = player.findChild<QPushButton*>(QStringLiteral("ffplayPauseButton"));
    QVERIFY(surface && panel && header && pause);
    QTRY_VERIFY(header->isVisible());
    const QRect videoGeometry = surface->geometry();
    QTRY_VERIFY_WITH_TIMEOUT(panel->isHidden(), 6000);
    QVERIFY(header->isHidden());
    QCOMPARE(surface->geometry(), videoGeometry);
    // Qt 注入只验证控件状态机；录屏另行检查实际窗口和渲染层。
    QMouseEvent activity(QEvent::MouseMove, QPointF(100, 100), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(surface, &activity);
    QTRY_VERIFY(panel->isVisible());
    QVERIFY(header->isVisible());
    pause->click();
    QTRY_VERIFY(!player.isPlaying());
    QTest::qWait(2800);
    QVERIFY(panel->isVisible() && header->isVisible());
    QCOMPARE(surface->geometry(), videoGeometry);
    QTest::keyClick(&player, Qt::Key_Escape);
    QTRY_VERIFY(!player.isFullScreen());
    QVERIFY(player.isVisible() && panel->isVisible());
    player.close();
    QTRY_VERIFY(!player.isVisible());
}

// 覆盖真实原生 HLG 显示层；持续采样中间帧，不能只检查最后 isFullScreen 的目标标志。
void MaiFfplayVideoDialogTest::macFullscreenKeepsNativeVideoGeometry() {
#if defined(Q_OS_MAC)
    if (QGuiApplication::platformName() != QStringLiteral("cocoa")) QSKIP("Native Cocoa window required");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString path = qEnvironmentVariable("MAICHAT_FFPLAY_LOCAL_VIDEO");
    if (path.isEmpty()) {
        path = directory.filePath(QStringLiteral("transition-hlg.mov"));
        QProcess generate;
        generate.start(QStringLiteral("ffmpeg"),
            {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-f"), QStringLiteral("lavfi"),
             QStringLiteral("-i"), QStringLiteral("testsrc2=size=192x108:rate=24:duration=2"),
             QStringLiteral("-vf"), QStringLiteral("format=yuv420p10le"), QStringLiteral("-c:v"),
             QStringLiteral("libx265"), QStringLiteral("-preset"), QStringLiteral("ultrafast"),
             QStringLiteral("-x265-params"),
             QStringLiteral("log-level=error:pools=1:frame-threads=1:colorprim=9:transfer=18:colormatrix=9"),
             QStringLiteral("-color_primaries"), QStringLiteral("bt2020"), QStringLiteral("-colorspace"),
             QStringLiteral("bt2020nc"), QStringLiteral("-color_trc"), QStringLiteral("arib-std-b67"),
             QStringLiteral("-tag:v"), QStringLiteral("hvc1"), QStringLiteral("-an"), QStringLiteral("-y"), path});
        QVERIFY(generate.waitForFinished(15000));
        QCOMPARE(generate.exitCode(), 0);
    }
    QVERIFY(QFileInfo(path).isFile());
    QWidget backdrop;
    const QRect captureArea = QGuiApplication::primaryScreen()->availableGeometry();
    backdrop.setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    backdrop.setStyleSheet(QStringLiteral("background:#26313f;"));
    backdrop.setGeometry(captureArea);
    showOwnTestWindow(&backdrop);
    MaiFfplayVideoDialog player(path, &backdrop);
    showOwnTestWindow(&player);
    QTRY_VERIFY_WITH_TIMEOUT(player.hasPresentedHdrFrame(), 10000);
    QVERIFY(player.sendCommand(QStringLiteral("pause")));
    QTest::qWait(300);
    player.resize(740, 440);
    player.move(100, 120);
    QTest::qWait(200);
    // 独立录屏不阻塞 AppKit 事件循环。仅在隔离测试桌面显式启用：原生 Space
    // 动画仍可能显示系统壁纸。退出（含断言失败）前先停止，再释放自有背景。
    QProcess recorder;
    const auto stopRecorder = qScopeGuard([&] {
        if (recorder.state() == QProcess::NotRunning) return;
        recorder.write("q\n");
        if (!recorder.waitForFinished(5000)) { recorder.kill(); recorder.waitForFinished(1000); }
    });
    const QString recording = qEnvironmentVariable("MAICHAT_FULLSCREEN_RECORDING");
    if (!recording.isEmpty()) {
        const qreal scale = player.screen()->devicePixelRatio();
        const QRect area = backdrop.geometry().adjusted(12, 12, -12, -12);
        const QString filter = QStringLiteral("crop=%1:%2:%3:%4,scale=1280:-2")
            .arg(qRound(area.width() * scale) / 2 * 2).arg(qRound(area.height() * scale) / 2 * 2)
            .arg(qRound(area.x() * scale)).arg(qRound(area.y() * scale));
        recorder.setStandardErrorFile(recording + QStringLiteral(".log"));
        recorder.start(QStringLiteral("ffmpeg"),
            {QStringLiteral("-hide_banner"), QStringLiteral("-y"), QStringLiteral("-f"),
             QStringLiteral("avfoundation"), QStringLiteral("-framerate"), QStringLiteral("30"),
             QStringLiteral("-pixel_format"), QStringLiteral("bgr0"), QStringLiteral("-i"),
             QStringLiteral("Capture screen 0:none"), QStringLiteral("-vf"), filter,
             QStringLiteral("-an"), QStringLiteral("-c:v"), QStringLiteral("libx264"),
             QStringLiteral("-preset"), QStringLiteral("ultrafast"), QStringLiteral("-crf"),
             QStringLiteral("22"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"), recording});
        QVERIFY(recorder.waitForStarted(3000));
        QTest::qWait(500);
        QVERIFY(recorder.state() != QProcess::NotRunning);
    }
    auto* surface = player.findChild<QWidget*>(QStringLiteral("ffplayVideoSurface"));
    auto* fullscreen = player.findChild<QPushButton*>(QStringLiteral("ffplayFullscreenButton"));
    QVERIFY(surface && fullscreen);
    bool nativeTransition = false;
    void* observer = maiObserveMacVideoFullscreen(&player, &nativeTransition,
        [](void* state, MaiMacFullscreenPhase phase) {
            *static_cast<bool*>(state) = phase == MaiMacFullscreenPhase::WillEnter ||
                                        phase == MaiMacFullscreenPhase::WillExit;
        });
    QVERIFY(observer);
    const auto releaseObserver = qScopeGuard([&] { maiReleaseMacVideoFullscreenObserver(observer); });
    const QRect windowed = player.geometry();
    MaiFfplayPlaybackStatus before{};
    QVERIFY(player.playbackStatus(&before));
    QVERIFY(before.paused);
    const double speed = player.speed();
    const QJsonObject originalLayer = ffplayTestVideoLayerGeometry(surface);
    QFile trace(qEnvironmentVariable("MAICHAT_FULLSCREEN_TRACE"));
    if (!trace.fileName().isEmpty()) QVERIFY(trace.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QElapsedTimer elapsed;
    elapsed.start();
    double worstVideoMismatch = 0;
    int animatedFrames = 0;
    const auto sample = [&] {
        QJsonObject value = ffplayTestVideoLayerGeometry(surface);
        value.insert("ms", double(elapsed.elapsed()));
        value.insert("qtWidth", player.width());
        value.insert("qtHeight", player.height());
        value.insert("qtFullscreen", player.isFullScreen());
        value.insert("qtMaximized", player.isMaximized());
        value.insert("surfaceWidth", surface->width());
        value.insert("surfaceHeight", surface->height());
        if (trace.isOpen()) trace.write(QJsonDocument(value).toJson(QJsonDocument::Compact) + '\n');
        if (value.contains("videoWidth")) {
            worstVideoMismatch = qMax(worstVideoMismatch,
                std::abs(value.value("shownWidth").toDouble() - value.value("shownViewportWidth").toDouble()));
            if (value.value("animationCount").toInt()) ++animatedFrames;
        }
    };
    const auto observe = [&](int milliseconds) {
        QElapsedTimer interval;
        interval.start();
        while (interval.elapsed() < milliseconds) { QTest::qWait(16); sample(); }
    };
    sample();
    auto* maximize = player.findChild<QPushButton*>(QStringLiteral("ffplayMaximizeButton"));
    QVERIFY(maximize);
    for (int iteration = 0; iteration < 3; ++iteration) {
        maximize->click();
        observe(200);
        maximize->click();
        observe(400);
        qInfo() << "maximize-return" << iteration << player.geometry() << windowed;
        QCOMPARE(player.geometry(), windowed);
        fullscreen->click();
        observe(70);
        fullscreen->click();
        observe(70);
        fullscreen->click();
        observe(450);
        fullscreen->click();
        observe(700);
        // 连续输入保持原来的 70ms 间隔；最终验收等待实际 Did 通知，不把 700ms
        // 当成系统动画的保证时长。录屏会增加 WindowServer 合成负载。
        QElapsedTimer completion;
        completion.start();
        while (completion.elapsed() < 3000 &&
               (nativeTransition || player.isFullScreen() || player.geometry() != windowed)) observe(16);
        QVERIFY(!nativeTransition);
        qInfo() << "fullscreen-return" << iteration << player.geometry() << windowed;
        QCOMPARE(player.geometry(), windowed);
    }
    qInfo() << "native-video-geometry" << "worstPresentationMismatch" << worstVideoMismatch
            << "animatedFrames" << animatedFrames << "restored" << player.geometry() << "expected" << windowed;
    QCOMPARE(player.geometry(), windowed);
    QVERIFY2(worstVideoMismatch <= 2.0, "Native video layer animates independently of its current viewport");
    QCOMPARE(animatedFrames, 0);
    MaiFfplayPlaybackStatus after{};
    QVERIFY(player.playbackStatus(&after));
    QVERIFY(after.paused);
    QVERIFY(std::abs(after.position_us - before.position_us) < 50'000);
    QCOMPARE(player.speed(), speed);
    QCOMPARE(ffplayTestVideoLayerGeometry(surface).value("videoLayerIdentity"), originalLayer.value("videoLayerIdentity"));
    fullscreen->click();
    player.close();
    QTRY_VERIFY_WITH_TIMEOUT(!player.isVisible(), 3000);
    // 再开一个同宿主窗口验证原生 Space 已收敛，不能只检查关闭窗口已隐藏。
    MaiFfplayVideoDialog reopened(path, &backdrop);
    showOwnTestWindow(&reopened);
    QTRY_VERIFY_WITH_TIMEOUT(reopened.hasPresentedHdrFrame(), 10000);
    const QRect reopenedGeometry = reopened.geometry();
    QVERIFY(reopened.sendCommand(QStringLiteral("fullscreen")));
    QTRY_VERIFY(reopened.isFullScreen());
    QVERIFY(reopened.sendCommand(QStringLiteral("fullscreen")));
    QTRY_VERIFY_WITH_TIMEOUT(!reopened.isFullScreen(), 3000);
    QTRY_COMPARE(reopened.geometry(), reopenedGeometry);
    reopened.close();
    QTRY_VERIFY_WITH_TIMEOUT(!reopened.isVisible(), 3000);

#else
    QSKIP("Native AppKit transition regression");
#endif
}

void MaiFfplayVideoDialogTest::fullscreenControlsHideAndWake() {
    const QString file = QFileInfo(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE))
                             .dir()
                             .filePath(QStringLiteral("../../../desktop/tests/fixtures/video-circle-61s.mp4"));
    QVERIFY(QFileInfo::exists(file));
    MaiFfplayVideoDialog player(file);
    showOwnTestWindow(&player);
    QTRY_VERIFY_WITH_TIMEOUT(player.presentedFrames() > 0, 10000);
    player.showFullScreen();
    QTRY_VERIFY(player.isFullScreen());
    activateOwnTestWindow(&player);
    auto* surface = player.findChild<QWidget*>(QStringLiteral("ffplayVideoSurface"));
    auto* panel = player.findChild<QWidget*>(QStringLiteral("ffplayControlPanel"));
    auto* header = player.findChild<QWidget*>(QStringLiteral("ffplayTitleBar"));
    auto* pause = player.findChild<QPushButton*>(QStringLiteral("ffplayPauseButton"));
    QVERIFY(surface && panel && header && pause);
    QTRY_VERIFY(header->isVisible());
    const QRect videoGeometry = surface->geometry();
    VideoControlMoveProbe moveProbe(player, *panel);
    qApp->installEventFilter(&moveProbe);
    const auto removeProbe = qScopeGuard([&] { qApp->removeEventFilter(&moveProbe); });
    // QWindow 重载经过与前台活动相同的 Qt 事件过滤链，不把鼠标位移本身当作唤醒证据。
    QTest::mouseMove(player.windowHandle(), player.mapFromGlobal(surface->mapToGlobal(surface->rect().center())));
    QTRY_VERIFY_WITH_TIMEOUT(panel->isHidden(), 6000);
    QVERIFY(header->isHidden());
    QCOMPARE(surface->geometry(), videoGeometry);
    const QString screenshot = qEnvironmentVariable("MAICHAT_AGENT_VIDEO_SCREENSHOT");
    if (!screenshot.isEmpty()) {
        QTRY_VERIFY_WITH_TIMEOUT(ownTestWindowIsUnobscured(&player), 2500);
        const QPixmap pixels = captureOwnTestWindow(&player);
        QVERIFY(!pixels.isNull() && pixels.save(screenshot + QStringLiteral("-fullscreen-hidden.png")));
    }
    const int movesBeforeWake = moveProbe.received;
    QTest::mouseMove(player.windowHandle(),
                     player.mapFromGlobal(surface->mapToGlobal(surface->rect().center() + QPoint(20, 0))));
    QTRY_VERIFY(moveProbe.received > movesBeforeWake);
    QTRY_VERIFY(!panel->isHidden());
    QVERIFY(!header->isHidden());
    QCOMPARE(surface->geometry(), videoGeometry);
    pause->click();
    QTRY_VERIFY(!player.isPlaying());
    QTest::qWait(2800);
    QVERIFY(panel->isVisible() && header->isVisible());
    QTest::keyClick(&player, Qt::Key_Escape);
    QTRY_VERIFY(!player.isFullScreen());
    QVERIFY(player.isVisible() && panel->isVisible());
    player.close();
    QTRY_VERIFY(!player.isVisible());
}

void MaiFfplayVideoDialogTest::cameraDoesNotOpenAfterAgentCancellation() {
    QWidget host;
    MaiToolRegistry registry;
    registerDesktopMediaTools(registry, &host);
    MaiTool* camera = registry.find("maichat_camera_command");
    QVERIFY(camera != nullptr);
    std::atomic<bool> canceled{true};
    MaiToolContext context;
    context.sessionId = "ses_camera_test";
    context.cancel = &canceled;
    const MaiToolResult result = camera->execute(R"({"action":"open"})", context);
    QCOMPARE(result.error().code(), MaiErrorCode::Canceled);
    QString error;
    QCOMPARE(DesktopCameraDialog::capturePhoto(nullptr, QDir::tempPath(), &error, &canceled), QString());
    QVERIFY(error.isEmpty());
}

void MaiFfplayVideoDialogTest::agentCameraOpensAndClosesWithoutWaitingForShutter() {
    if (qEnvironmentVariable("MAICHAT_TEST_REAL_CAMERA_CANCEL") != QStringLiteral("1"))
        QSKIP("Real camera test must be explicitly enabled");
    if (qEnvironmentVariable("QT_QPA_PLATFORM") == QStringLiteral("offscreen"))
        QSKIP("Native camera preview requires the normal desktop platform plugin");
    if (!MaiCameraCapture::isAvailable()) QSKIP("No camera is connected");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QWidget host;
    host.show();
    MaiToolRegistry registry;
    registerDesktopMediaTools(registry, &host);
    MaiTool* camera = registry.find("maichat_camera_command");
    QVERIFY(camera != nullptr);
    MaiToolContext context;
    context.sessionId = "ses_camera_test";
    context.root = directory.path().toStdString();
    const MaiToolResult closed = camera->execute(R"({"action":"status"})", context);
    QVERIFY(!closed.hasError());
    QVERIFY(QString::fromStdString(closed.output()).contains(QStringLiteral("closed")));
    QElapsedTimer elapsed;
    elapsed.start();
    const MaiToolResult opened = camera->execute(R"({"action":"open"})", context);
    QVERIFY(!opened.hasError());
    QVERIFY(elapsed.elapsed() < 2000);
    QVERIFY(DesktopCameraDialog::isPreviewOpen(&host, QStringLiteral("ses_camera_test")));
    auto* preview = host.findChild<QDialog*>(QStringLiteral("desktopCameraDialog"));
    QVERIFY(preview != nullptr);
    auto* shutter = preview->findChild<QPushButton*>(QStringLiteral("desktopCameraShutter"));
    QVERIFY(shutter != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(shutter->isEnabled(), 7000);
    if (qEnvironmentVariableIsSet("MAICHAT_CAMERA_SCREENSHOT")) {
        const QPixmap picture = preview->grab();
        QVERIFY(!picture.isNull());
        QVERIFY(picture.save(qEnvironmentVariable("MAICHAT_CAMERA_SCREENSHOT")));
    }
    MaiToolContext other = context;
    other.sessionId = "ses_other";
    QVERIFY(camera->execute(R"({"action":"open"})", other).hasError());
    QVERIFY(!camera->execute(R"({"action":"close"})", other).hasError());
    QVERIFY(DesktopCameraDialog::isPreviewOpen(&host, QStringLiteral("ses_camera_test")));
    const MaiToolResult result = camera->execute(R"({"action":"close"})", context);
    QVERIFY(!result.hasError());
    const MaiToolResult reopened = camera->execute(R"({"action":"open"})", context);
    QVERIFY(!reopened.hasError());
    QCoreApplication::processEvents();
    QVERIFY(DesktopCameraDialog::isPreviewOpen(&host, QStringLiteral("ses_camera_test")));
    QVERIFY(!camera->execute(R"({"action":"close"})", context).hasError());
    QTRY_VERIFY_WITH_TIMEOUT(!DesktopCameraDialog::isPreviewOpen(&host, QStringLiteral("ses_camera_test")), 3000);
    QVERIFY(QDir(directory.path()).entryList(QStringList{QStringLiteral("*.jpg")}, QDir::Files).isEmpty());
}

void MaiFfplayVideoDialogTest::cameraActivePreviewClosesOnAgentCancellation() {
    if (qEnvironmentVariable("MAICHAT_TEST_REAL_CAMERA_CANCEL") != QStringLiteral("1"))
        QSKIP("Real camera test must be explicitly enabled");
    if (qEnvironmentVariable("QT_QPA_PLATFORM") == QStringLiteral("offscreen"))
        QSKIP("Native camera preview requires the normal desktop platform plugin");
    if (!MaiCameraCapture::isAvailable()) QSKIP("No camera is connected");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    std::atomic<bool> canceled{false};
    QTimer::singleShot(1200, [&] { canceled.store(true, std::memory_order_relaxed); });
    QElapsedTimer elapsed;
    elapsed.start();
    QString error;
    const QString photo = DesktopCameraDialog::capturePhoto(nullptr, directory.path(), &error, &canceled);
    QVERIFY(photo.isEmpty());
    QVERIFY(error.isEmpty());
    QVERIFY(elapsed.elapsed() < 5000);
    QVERIFY(QDir(directory.path()).entryList(QStringList{QStringLiteral("*.jpg")}, QDir::Files).isEmpty());
}

void MaiFfplayVideoDialogTest::imVideoUsesIndependentWindowAcrossNavigationAndFullscreen_data() {
    QTest::addColumn<QSize>("mainSize");
    QTest::newRow("wide-main-window") << QSize(1500, 900);
    QTest::newRow("narrow-main-window") << QSize(900, 800);
}

void MaiFfplayVideoDialogTest::imVideoUsesIndependentWindowAcrossNavigationAndFullscreen() {
    QFETCH(QSize, mainSize);
    const QString captureDirectory = qEnvironmentVariable("MAICHAT_VIDEO_PREVIEW_CAPTURE_DIR");
    QScreen* screen = QGuiApplication::primaryScreen();
    QVERIFY(screen);
    // 后续显式增加 60×40 并移动窗口，夹具必须留在可用屏幕内，避免 AppKit
    // 的越界约束在关闭播放器之后才修正宿主尺寸，被误判成播放器挤压聊天区。
    mainSize = mainSize.boundedTo(screen->availableGeometry().size() - QSize(104, 116));
    QScreen* captureScreen = nullptr;
    if (!captureDirectory.isEmpty()) {
        QVERIFY(QDir().mkpath(captureDirectory));
        captureScreen = screen;
    }
    auto client = std::make_unique<FakeRemoteIMClient>();
    RemoteIMApplication app(QStringLiteral("im-video-window-test"), std::move(client));
    const QString peer = QStringLiteral("video-fixture-peer");
    app.addContact(peer, QStringLiteral("Video fixture"));
    app.selectPeer(peer);
    RemoteIMMessage message;
    message.id = QStringLiteral("local-video-fixture");
    message.fromUserId = peer;
    message.toUserId = app.chatState().ownerUserId();
    message.hasVideo = true;
    message.video.localPath = QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE);
    app.chatState().appendMessageForRestore(message);
    MainWindow window(app);
    window.resize(mainSize);
    if (captureScreen) window.move(captureScreen->availableGeometry().topLeft() + QPoint(20, 32));
    showOwnTestWindow(&window);
    auto* stack = window.findChild<QStackedWidget*>(QStringLiteral("contentStack"));
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("messageEditor"));
    auto* videoButton = window.findChild<QPushButton*>(QStringLiteral("messageVideoButton"));
    auto* slot = window.findChild<QWidget*>(QStringLiteral("videoDockSlot"));
    auto* pane = window.findChild<QWidget*>(QStringLiteral("videoDockPane"));
    QVERIFY(stack && editor && videoButton && slot && pane);
    editor->setPlainText(QStringLiteral("Unsent local video fixture draft"));
    QCoreApplication::processEvents();
    const QRect contentBefore = stack->geometry();
    const QSize windowBefore = window.size();
    const QString draftBefore = editor->toPlainText();

    // Exercise the actual IM message click, rather than testing only the Agent's already-correct opener.
    videoButton->click();
    QPointer<MaiFfplayVideoDialog> player = MaiFfplayVideoDialog::activeDialog();
    QVERIFY(player);
    QVERIFY(player->isWindow());
    QVERIFY(player->windowFlags().testFlag(Qt::FramelessWindowHint));
    QVERIFY(player->findChild<QWidget*>(QStringLiteral("ffplayTitleBar")));
    QTRY_COMPARE(stack->geometry(), contentBefore);
    QCOMPARE(window.size(), windowBefore);
    QVERIFY(!slot->isVisible() && !pane->isVisible());
    QCOMPARE(editor->toPlainText(), draftBefore);
    QCOMPARE(app.chatState().selectedPeerId(), peer);
    QTRY_VERIFY_WITH_TIMEOUT(player->presentedFrames() > 0, 10000);
    const auto captureStage = [&](const QString& stage) {
        if (captureDirectory.isEmpty()) return true;
        QScreen* screen = player ? player->screen() : window.screen();
        if (!screen) return false;
        window.raise();
        if (player && player->isVisible()) {
            if (!player->isFullScreen()) {
                // Keep the popup within the synthetic MainWindow: its union must not expose other apps.
                const QRect area =
                    window.frameGeometry().intersected(screen->availableGeometry()).adjusted(24, 48, -24, -24);
                const QSize frame = player->frameGeometry().size() - player->size();
                player->resize(player->size().boundedTo(area.size() - frame));
                player->move(area.center() -
                             QPoint(player->frameGeometry().width() / 2, player->frameGeometry().height() / 2));
            }
            activateOwnTestWindow(player);
        } else {
            activateOwnTestWindow(&window);
        }
        if (stage == QStringLiteral("closed")) {
            if (player && player->isVisible()) return false;
            // Qt destruction precedes the Windows compositor's closing animation.
            QTest::qWait(350);
        } else {
            QTest::qWait(80);
        }
        QCoreApplication::processEvents();
        if (!ownTestWindowIsUnobscured(&window)) return false;
        QRect region = window.frameGeometry();
        if (player && player->isVisible()) region = region.united(player->frameGeometry());
        region = region.intersected(screen->geometry());
        if (region.isEmpty()) return false;
        const QPixmap pixels = screen->grabWindow(0, region.x(), region.y(), region.width(), region.height());
        const QString filename =
            QString::fromLatin1(QTest::currentDataTag()) + QLatin1Char('-') + stage + QStringLiteral(".png");
        return !pixels.isNull() && pixels.save(QDir(captureDirectory).filePath(filename));
    };
    QVERIFY(captureStage(QStringLiteral("popup")));

    // Reopening closes the previous player without replacing any chat widgets or draft.
    QPointer<MaiFfplayVideoDialog> previous = player;
    videoButton->click();
    player = MaiFfplayVideoDialog::activeDialog();
    QVERIFY(player && player != previous);
    QTRY_VERIFY(previous.isNull());
    QTRY_COMPARE(stack->geometry(), contentBefore);
    QCOMPARE(window.size(), windowBefore);
    QCOMPARE(editor->toPlainText(), draftBefore);
    QTRY_VERIFY_WITH_TIMEOUT(player->presentedFrames() > 0, 10000);
    const QRect windowedGeometry = player->geometry();
    // 经真实 GUI/工具共用入口切换；目标状态标志先变化，须继续等原窗口恢复。
    QVERIFY(player->sendCommand(QStringLiteral("fullscreen")));
    QTRY_VERIFY(player->isFullScreen());
    QTRY_COMPARE(stack->geometry(), contentBefore);
    QVERIFY(!slot->isVisible() && !pane->isVisible());
    QVERIFY(player->sendCommand(QStringLiteral("fullscreen")));
    QTRY_VERIFY(!player->isFullScreen());
    QTRY_COMPARE(player->geometry(), windowedGeometry);
    QTRY_COMPARE(stack->geometry(), contentBefore);

    auto* contacts = window.findChild<QPushButton*>(QStringLiteral("contactsNavButton"));
    auto* messages = window.findChild<QPushButton*>(QStringLiteral("messagesNavButton"));
    QVERIFY(contacts && messages);
    contacts->click();
    QCOMPARE(MaiFfplayVideoDialog::activeDialog(), player);
    const QPoint playerPosition = player->pos();
    window.resize(mainSize + QSize(60, 40));
    window.move(window.pos() + QPoint(20, 15));
    QCoreApplication::processEvents();
    QCOMPARE(player->pos(), playerPosition);
    QVERIFY(!slot->isVisible() && !pane->isVisible());
    messages->click();
    QCOMPARE(editor->toPlainText(), draftBefore);
    QCOMPARE(app.chatState().selectedPeerId(), peer);
    const QRect contentAfterNavigation = stack->geometry();
    player->close();
    QTRY_VERIFY(player.isNull());
    QTRY_COMPARE(stack->geometry(), contentAfterNavigation);
    QVERIFY(!slot->isVisible() && !pane->isVisible());
    QCOMPARE(editor->toPlainText(), draftBefore);
    QCOMPARE(app.chatState().selectedPeerId(), peer);
    QVERIFY(captureStage(QStringLiteral("closed")));
}

void MaiFfplayVideoDialogTest::exposesSaveAndForwardActions() {
    MaiFfplayVideoDialog dialog(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE));
#if defined(Q_OS_MAC)
    auto* share = dialog.findChild<QAction*>(QStringLiteral("videoShareAction"));
#else
    auto* share = dialog.findChild<QAction*>(QStringLiteral("videoForwardAction"));
#endif
    auto* save = dialog.findChild<QAction*>(QStringLiteral("videoSaveAction"));
    auto* more = dialog.findChild<QToolButton*>(QStringLiteral("videoMoreButton"));
    auto* menu = dialog.findChild<QMenu*>(QStringLiteral("ffplayMoreMenu"));
    QVERIFY(share && save && more && menu);
    QCOMPARE(more->toolButtonStyle(), Qt::ToolButtonIconOnly);
    QVERIFY(more->text().isEmpty() && !more->icon().isNull());
    QVERIFY(!more->toolTip().isEmpty() && more->accessibleName() == more->toolTip());
    for (auto* action : {share, save}) {
        QVERIFY(menu->actions().contains(action));
        QVERIFY(!action->text().isEmpty() && !action->icon().isNull());
    }
    QVERIFY(menu->actions().indexOf(save) < menu->actions().indexOf(share));
    bool forwarded = false;
    dialog.setForwardAction([&forwarded](const QString&) { forwarded = true; });
    dialog.forwardVideo();
    QVERIFY(forwarded);
}

void MaiFfplayVideoDialogTest::shareAndSaveIconsInvokeExistingDialogs() {
    MaiFfplayVideoDialog dialog(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE));
    showOwnTestWindow(&dialog);
#if defined(Q_OS_MAC)
    activateFfplayTestWindow(&dialog);
#endif
    QTRY_VERIFY_WITH_TIMEOUT(dialog.presentedFrames() > 0, 10000);
    auto* more = dialog.findChild<QToolButton*>(QStringLiteral("videoMoreButton"));
    auto* menu = dialog.findChild<QMenu*>(QStringLiteral("ffplayMoreMenu"));
    auto* save = dialog.findChild<QAction*>(QStringLiteral("videoSaveAction"));
    QVERIFY(more && menu && save && more->isVisible());
#if defined(Q_OS_MAC)
    auto* share = dialog.findChild<QAction*>(QStringLiteral("videoShareAction"));
    QVERIFY(share && menu->actions().contains(share));
    QSignalSpy shared(share, &QAction::triggered);
    // Keep the existing AppKit event helper, targeting the actual menu row.
    // The transparent child contributes coordinates, never intercepts input
    // or replaces the production QAction with a test-only action.
    const auto clickNativeMenuAction = [&](QAction* action) {
        const QRect rectangle = menu->actionGeometry(action);
        if (!menu->isVisible() || rectangle.isEmpty()) return false;
        auto* target = new QWidget(menu);
        target->setAttribute(Qt::WA_TransparentForMouseEvents);
        target->setAttribute(Qt::WA_NoSystemBackground);
        target->setGeometry(rectangle);
        target->show();
        const bool sent = clickFfplayTestNativeButton(target);
        QTimer::singleShot(500, target, &QObject::deleteLater);
        return sent;
    };
    observeFfplayTestNativePanel();
    bool sharePickerObserved = false;
    bool shareRowClicked = false;
    QTimer dismissShare;
    connect(&dismissShare, &QTimer::timeout, [&] {
        if (cancelFfplayTestNativePanel()) {
            sharePickerObserved = true;
            dismissShare.stop();
        }
    });
    // AppKit sharing requires the real native input context; QWidget synthetic
    // clicks bypass NSEvent and cannot validate a system picker presentation.
    QTimer::singleShot(150, menu, [&] {
        dismissShare.start(100);
        shareRowClicked = clickNativeMenuAction(share);
        if (!shareRowClicked) menu->close();
    });
    QVERIFY(clickFfplayTestNativeButton(more));
    QTRY_COMPARE(shared.count(), 1);
    QVERIFY(shareRowClicked);
    QTRY_VERIFY(sharePickerObserved);
    dismissShare.stop();
    QVERIFY(dialog.isVisible());
    bool saveOpened = false;
    QTemporaryDir saveDirectory;
    QVERIFY(saveDirectory.isValid());
    const QString originalDirectory = QDir::currentPath();
    const auto restoreDirectory = qScopeGuard([&] { QDir::setCurrent(originalDirectory); });
    QVERIFY(QDir::setCurrent(saveDirectory.path()));
    // NSSavePanel runs its own modal loop; a Qt timer/widget scan does not
    // reliably cancel it. Use a native modal-mode timer without choosing a file.
    void* cancelToken = scheduleFfplayTestSaveCancel(&saveOpened);
    const auto stopCancel = qScopeGuard([&] { stopFfplayTestSaveCancel(cancelToken); });
    bool saveRowClicked = false;
    QTimer::singleShot(150, menu, [&] {
        saveRowClicked = clickNativeMenuAction(save);
        if (!saveRowClicked) menu->close();
    });
    QVERIFY(clickFfplayTestNativeButton(more));
    QTRY_VERIFY(saveOpened);
    QVERIFY(saveRowClicked);
    QVERIFY(saveOpened);
    QVERIFY(dialog.isVisible());
#else
    // Exercise the production saveCopy path without a platform-native modal
    // loop; cancel before selecting a destination or writing any file.
    const bool wasNonNative = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    const auto restoreDialogMode =
        qScopeGuard([&] { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, wasNonNative); });
    bool saveOpened = false;
    bool saveRowClicked = false;
    QTimer dismissSave;
    connect(&dismissSave, &QTimer::timeout, [&] {
        for (auto* widget : QApplication::topLevelWidgets()) {
            auto* picker = qobject_cast<QFileDialog*>(widget);
            if (!picker || !picker->isVisible()) continue;
            saveOpened = true;
            picker->reject();
            dismissSave.stop();
            break;
        }
    });
    dismissSave.start(50);
    const auto stopDismissSave = qScopeGuard([&] { dismissSave.stop(); });
    QTimer::singleShot(150, menu, [&] {
        const QRect rectangle = menu->actionGeometry(save);
        saveRowClicked = menu->isVisible() && !rectangle.isEmpty();
        if (saveRowClicked)
            QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, rectangle.center());
        else
            menu->close();
    });
    QTest::mouseClick(more, Qt::LeftButton);
    QTRY_VERIFY(saveOpened && saveRowClicked);
    QVERIFY(dialog.isVisible());
#endif
    dialog.close();
}

void MaiFfplayVideoDialogTest::opensControlsAndClosesVideoPopup_data() {
    QTest::addColumn<QString>("fixturePath");
    QTest::newRow("video-audio-subtitles") << QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE);
    QTest::newRow("h264") << QStringLiteral(MAICHAT_FFPLAY_H264_FIXTURE);
    QTest::newRow("av1") << QStringLiteral(MAICHAT_FFPLAY_AV1_FIXTURE);
}

// 不同编码素材共用一套打开、控制、关闭路径；关闭后播放器资源必须退出，不能留后台声音。
void MaiFfplayVideoDialogTest::opensControlsAndClosesVideoPopup() {
    QFETCH(QString, fixturePath);
    sSawFiniteSeek = false;
    maiFfplaySetDiagnosticSink(captureDiagnostic, nullptr);
    QWidget host;
    MaiToolRegistry registry;
    registerDesktopMediaTools(registry, &host);
    MaiTool* openTool = registry.find("maichat_play_video");
    MaiTool* commandTool = registry.find("maichat_video_command");
    QVERIFY(openTool != nullptr);
    QVERIFY(commandTool != nullptr);
    QVERIFY(QString::fromStdString(commandTool->parametersSchema()).contains(QStringLiteral("set_speed")));
    QVERIFY(QString::fromStdString(commandTool->parametersSchema()).contains(QStringLiteral("replay")));
    MaiToolContext context;
    const QFileInfo fixture(fixturePath);
    context.root = fixture.dir().absolutePath().toStdString();
    const MaiToolResult opened =
        openTool->execute(QStringLiteral("{\"path\":\"%1\"}").arg(fixture.fileName()).toStdString(), context);
    QVERIFY2(!opened.hasError(), opened.error().message().c_str());
    QPointer<MaiFfplayVideoDialog> dialog = MaiFfplayVideoDialog::activeDialog();
    QVERIFY(!dialog.isNull());
    QVERIFY(dialog->isStarted());
    QCOMPARE(MaiFfplayVideoDialog::activeDialog(), dialog);
    auto* slider = dialog->findChild<QSlider*>(QStringLiteral("ffplaySeekSlider"));
    auto* timeLabel = dialog->findChild<QLabel*>(QStringLiteral("ffplayTimeLabel"));
    QVERIFY(slider != nullptr);
    QVERIFY(timeLabel != nullptr);
    auto* speedSelector = dialog->findChild<QComboBox*>(QStringLiteral("ffplaySpeedSelector"));
    QVERIFY(speedSelector != nullptr);
    QCOMPARE(speedSelector->count(), 5);
    QVERIFY(!commandTool->execute(R"({"action":"set_speed","speed":0.5})", context).hasError());
    MaiFfplayPlaybackStatus speedStatus{};
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&speedStatus) && speedStatus.speed_percent == 50, 3000);
    auto* durationLabel = dialog->findChild<QLabel*>(QStringLiteral("ffplayDurationLabel"));
    QVERIFY(durationLabel);
    QTRY_VERIFY_WITH_TIMEOUT(slider->value() > 0 && durationLabel->text() == QStringLiteral("0:02"), 6000);
    QVERIFY(!timeLabel->text().contains(QLatin1Char('/')));
    QTRY_VERIFY_WITH_TIMEOUT(dialog->presentedFrames() >= 3, 4000);
    QWidget* surface = dialog->findChild<QWidget*>(QStringLiteral("ffplayVideoSurface"));
    QVERIFY(surface != nullptr);
    activateOwnTestWindow(dialog);
    QTRY_VERIFY_WITH_TIMEOUT(ownTestWindowIsUnobscured(dialog), 2500);
    const auto hasVisibleColor = [surface] {
        if (!ownTestWindowIsUnobscured(surface->window())) return false;
        const QPoint topLeft = surface->mapToGlobal(QPoint(0, 0));
        const QImage pixels =
            surface->screen()->grabWindow(0, topLeft.x(), topLeft.y(), surface->width(), surface->height()).toImage();
        int minimumValue = 255;
        int maximumValue = 0;
        for (int y = 0; y < pixels.height(); y += 8) {
            for (int x = 0; x < pixels.width(); x += 8) {
                const QColor color = pixels.pixelColor(x, y);
                const int value = std::max({color.red(), color.green(), color.blue()});
                minimumValue = std::min(minimumValue, value);
                maximumValue = std::max(maximumValue, value);
            }
        }
        return maximumValue - minimumValue > 32;
    };
    QTRY_VERIFY_WITH_TIMEOUT(hasVisibleColor(), 2000);
    QTRY_VERIFY_WITH_TIMEOUT(!commandTool->execute(R"({"action":"pause"})", context).hasError(), 3000);
    QTest::qWait(200);
    const uint64_t pausedFrames = dialog->presentedFrames();
    QTest::qWait(250);
    QCOMPARE(dialog->presentedFrames(), pausedFrames);
    QVERIFY(!commandTool->execute(R"({"action":"play"})", context).hasError());
    QTRY_VERIFY_WITH_TIMEOUT(dialog->presentedFrames() > pausedFrames, 3000);
    QVERIFY(!commandTool->execute(R"({"action":"seek_percent","percent":50})", context).hasError());
    QTRY_VERIFY_WITH_TIMEOUT(sSawFiniteSeek.load(), 1500);
    QVERIFY(!commandTool->execute(R"({"action":"replay"})", context).hasError());
    QVERIFY(dialog->isStarted());
    QElapsedTimer timer;
    timer.start();
    QVERIFY(!commandTool->execute(R"({"action":"close"})", context).hasError());
    QVERIFY(timer.elapsed() < 5000);
    QTRY_VERIFY(dialog.isNull());
    QVERIFY(MaiFfplayVideoDialog::activeDialog().isNull());
    maiFfplaySetDiagnosticSink(nullptr, nullptr);
}

void MaiFfplayVideoDialogTest::slowedPlaybackExtendsAudibleOutput() {
#if defined(__APPLE__)
    QWidget host;
    MaiToolRegistry registry;
    registerDesktopMediaTools(registry, &host);
    MaiTool* openTool = registry.find("maichat_play_video");
    QVERIFY(openTool != nullptr);
    const QFileInfo fixture(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE));
    MaiToolContext context;
    context.root = fixture.dir().absolutePath().toStdString();
    const MaiToolResult opened =
        openTool->execute(QStringLiteral("{\"path\":\"%1\"}").arg(fixture.fileName()).toStdString(), context);
    QVERIFY2(!opened.hasError(), opened.error().message().c_str());
    QPointer<MaiFfplayVideoDialog> dialog = MaiFfplayVideoDialog::activeDialog();
    QVERIFY(!dialog.isNull());
    QVERIFY(dialog->setSpeed(0.5));
    const uint64_t before = maiFfplayAppleNonSilentBufferCount();
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayAppleNonSilentBufferCount() > before, 3000);
    // 两秒源音轨按 0.5 倍速应持续约四秒；若只改画面，音频此时已结束。
    QTest::qWait(2200);
    const uint64_t midway = maiFfplayAppleNonSilentBufferCount();
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayAppleNonSilentBufferCount() > midway, 700);
    dialog->close();
    QTRY_VERIFY(dialog.isNull());

    const MaiToolResult reopened =
        openTool->execute(QStringLiteral("{\"path\":\"%1\"}").arg(fixture.fileName()).toStdString(), context);
    QVERIFY2(!reopened.hasError(), reopened.error().message().c_str());
    dialog = MaiFfplayVideoDialog::activeDialog();
    QVERIFY(!dialog.isNull());
    QVERIFY(dialog->setSpeed(2.0));
    const uint64_t fastStart = maiFfplayAppleNonSilentBufferCount();
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayAppleNonSilentBufferCount() > fastStart, 3000);
    // 两秒源音轨按 2 倍速应在约一秒后结束，不应继续按原速输出声音。
    QTest::qWait(1500);
    const uint64_t fastEnd = maiFfplayAppleNonSilentBufferCount();
    QTest::qWait(300);
    QCOMPARE(maiFfplayAppleNonSilentBufferCount(), fastEnd);
    dialog->close();
    QTRY_VERIFY(dialog.isNull());
#endif
}

void MaiFfplayVideoDialogTest::localVideoShowsPixels() {
    const QString path = qEnvironmentVariable("MAICHAT_FFPLAY_LOCAL_VIDEO");
    if (!QFileInfo(path).isFile()) QSKIP("No local video fixture");
    const QString nextPath = qEnvironmentVariable("MAICHAT_FFPLAY_NEXT_VIDEO");
    MaiFfplayVideoDialog dialog(path);
    if (QFileInfo(nextPath).isFile()) dialog.setPlaylist({path, nextPath});
    const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
    dialog.resize(QSize(1060, 680).boundedTo(available.size() - QSize(40, 60)));
    dialog.move(available.center() - QPoint(dialog.width() / 2, dialog.height() / 2));
    showOwnTestWindow(&dialog);
#if !defined(Q_OS_WIN)
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
#endif
    // Windows 上原生视频/控制子窗覆盖整个客户区，顶层 QWindow 可能没有暴露区域；
    // 使用完整 Win32 Z 序和窗口归属检查，且实际像素仍须通过下方画面断言。
    QTRY_VERIFY_WITH_TIMEOUT(ownTestWindowIsUnobscured(&dialog), 2500);
    QVERIFY(dialog.isStarted());
    QTRY_VERIFY_WITH_TIMEOUT(dialog.presentedFrames() > 0, 5000);
    QWidget* surface = dialog.findChild<QWidget*>(QStringLiteral("ffplayVideoSurface"));
    QVERIFY(surface != nullptr);
    QTest::qWait(500);
    QVERIFY2(ownTestWindowIsUnobscured(&dialog), "test player lost foreground or was obscured before pixel capture");
    const QPoint topLeft = surface->mapToGlobal(QPoint(0, 0));
    const QImage pixels =
        surface->screen()->grabWindow(0, topLeft.x(), topLeft.y(), surface->width(), surface->height()).toImage();
    const QString capturePath = qEnvironmentVariable("MAICHAT_FFPLAY_CAPTURE");
    if (!capturePath.isEmpty()) QVERIFY(pixels.save(capturePath));
    const QString windowCapture = qEnvironmentVariable("MAICHAT_FFPLAY_WINDOW_CAPTURE");
    if (!windowCapture.isEmpty()) {
        const QPixmap windowPixels = captureOwnTestWindow(&dialog);
        QVERIFY(!windowPixels.isNull() && windowPixels.save(windowCapture));
    }
    int minimumValue = 255;
    int maximumValue = 0;
    for (int y = 0; y < pixels.height(); y += 8)
        for (int x = 0; x < pixels.width(); x += 8) {
            const QColor color = pixels.pixelColor(x, y);
            const int value = std::max({color.red(), color.green(), color.blue()});
            minimumValue = std::min(minimumValue, value);
            maximumValue = std::max(maximumValue, value);
        }
    qInfo() << "local video pixels" << pixels.size() << minimumValue << maximumValue;
    QVERIFY(maximumValue - minimumValue > 32);
    auto* pauseButton = dialog.findChild<QPushButton*>(QStringLiteral("ffplayPauseButton"));
    QVERIFY(pauseButton != nullptr && pauseButton->isVisible());
    QTest::mouseClick(pauseButton, Qt::LeftButton);
    MaiFfplayPlaybackStatus playback{};
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&playback) && playback.paused != 0, 2000);
    QTest::mouseClick(pauseButton, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&playback) && playback.paused == 0, 2000);
    auto* speedSelector = dialog.findChild<QComboBox*>(QStringLiteral("ffplaySpeedSelector"));
    QVERIFY(speedSelector != nullptr && speedSelector->isVisible());
    speedSelector->setCurrentIndex(3);
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&playback) && playback.speed_percent == 150, 3000);
    if (QFileInfo(nextPath).isFile()) {
        QString forwardedPath;
        dialog.setForwardAction([&](const QString& currentPath) { forwardedPath = currentPath; });
        auto* nextButton = dialog.findChild<QPushButton*>(QStringLiteral("ffplayNextItemButton"));
        QVERIFY(nextButton != nullptr);
        QVERIFY(nextButton->isVisible() && nextButton->isEnabled());
        nextButton->click();
        QTRY_COMPARE_WITH_TIMEOUT(dialog.windowTitle(), QFileInfo(nextPath).fileName(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(dialog.presentedFrames() > 0, 5000);
        dialog.forwardVideo();
        QCOMPARE(QFileInfo(forwardedPath).canonicalFilePath(), QFileInfo(nextPath).canonicalFilePath());
        QCOMPARE(dialog.speed(), 1.5);
        QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&playback) && playback.speed_percent == 150, 3000);
        QVERIFY(!dialog.navigateVideo(1));
        QVERIFY(dialog.navigateVideo(-1));
        QTRY_COMPARE_WITH_TIMEOUT(dialog.windowTitle(), QFileInfo(path).fileName(), 3000);
    }
    dialog.close();
}

// 与正常 App 使用相同的分数缩放和矢量图标设置；QtTest 默认 main 不开启
// HighDpiPixmaps，会把 1x 图标放大，不能用于判断 125%/150% 实际产品画面。
int main(int argc, char* argv[]) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif
    QApplication application(argc, argv);
    MaiFfplayVideoDialogTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "MaiFfplayVideoDialogTest.moc"
