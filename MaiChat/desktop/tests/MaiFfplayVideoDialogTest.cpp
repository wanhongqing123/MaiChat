// 文件作用：验证桌面端视频弹窗控制、分享保存、倍速音频及关闭释放。
// 运行边界：由测试进程驱动实际组件或替身，不作为正式 App 的用户入口。
#include <QElapsedTimer>
#include <QAction>
#include <QComboBox>
#include <QCameraInfo>
#include <QDir>
#include <QHBoxLayout>
#include <QFileInfo>
#include <QFileDialog>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSlider>
#include <QLabel>
#include <QMouseEvent>
#include <QMenu>
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

#include "MaiChatTools/DesktopMediaTools.h"
#include "MaiTool.h"
#include "ui/MaiFfplayVideoDialog.h"
#include "ui/DesktopCameraDialog.h"
#include "ui/MainWindow.h"
#include "app/RemoteIMApplication.h"
#include "im/FakeRemoteIMClient.h"

#if defined(__APPLE__)
extern "C" uint64_t maiFfplayAppleNonSilentBufferCount(void);
void activateFfplayTestApplication();
void activateFfplayTestWindow(QWidget* widget);
bool ffplayTestWindowInteractive(QWidget* widget);
QString ffplayTestNativeState(QWidget* widget);
bool cancelFfplayTestNativePanel();
void observeFfplayTestNativePanel();
bool clickFfplayTestNativeButton(QWidget* widget);
void* scheduleFfplayTestSaveCancel(bool* observed);
void stopFfplayTestSaveCancel(void* token);
#endif

namespace {

std::atomic<bool> sSawFiniteSeek{false};

class VideoControlMoveProbe final : public QObject {
public:
    VideoControlMoveProbe(MaiFfplayVideoDialog& player, QWidget& panel)
        : player_(player), panel_(panel) {}
    int received = 0;
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() != QEvent::MouseMove) return false;
        auto* widget = qobject_cast<QWidget*>(watched);
        if ((!widget || (widget != &player_ && !player_.isAncestorOf(widget))) &&
            watched != player_.windowHandle()) return false;
        ++received;
        const auto* mouse = static_cast<QMouseEvent*>(event);
        QTimer* hideTimer = nullptr;
        for (auto* timer : player_.findChildren<QTimer*>())
            if (timer->interval() == 2500) hideTimer = timer;
        qInfo() << "[controls-move-event]" << watched->metaObject()->className() << watched->objectName()
                << "local" << mouse->localPos() << "global" << mouse->globalPos()
                << "cursor" << QCursor::pos() << "spontaneous" << event->spontaneous()
                << "hidden-before" << panel_.isHidden() << "playing" << player_.isPlaying()
                << "timer-active" << bool(hideTimer && hideTimer->isActive())
                << "remaining" << (hideTimer ? hideTimer->remainingTime() : -1);
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
    void exposesSaveAndForwardActions();
    void shareAndSaveIconsInvokeExistingDialogs();
    void opensControlsAndClosesVideoPopup_data();
    void opensControlsAndClosesVideoPopup();
    void opensAtRequestedSourceTime();
    void agentVideoUsesIndependentWindow();
    void imVideoUsesIndependentWindowAcrossNavigationAndFullscreen_data();
    void imVideoUsesIndependentWindowAcrossNavigationAndFullscreen();
    void fullscreenControlsHideAndWake();
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
#if defined(Q_OS_MAC)
    activateFfplayTestApplication();
    QTest::qWait(250);
#endif
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
    player.show();
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
    QTRY_VERIFY_WITH_TIMEOUT(time && time->text().startsWith(QStringLiteral("0:01 /")), 2000);
    QVERIFY(player.grab().save(QStringLiteral("/private/tmp/maichat-accurate-seek-slider-20261031.png")));
    const QRect frame = player.frameGeometry();
    const QPixmap screen = player.screen()->grabWindow(0, frame.x(), frame.y(), frame.width(), frame.height());
    if (!screen.isNull()) screen.save(QStringLiteral("/private/tmp/maichat-accurate-seek-window-20261031.png"));
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
    player.show();
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
    player.show();
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
    // 合成素材只验证正式播放器交给 Graphics 的帧契约，不使用账号或用户视频。
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
    // 使用正式 Agent 独立播放器相同的 Window 类型；裸 QDialog 不代表 macOS 原生全屏窗口。
    player.setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint | Qt::WindowMinMaxButtonsHint | Qt::WindowTitleHint);
    player.show();
    player.raise();
    player.activateWindow();
    QVERIFY(player.isStarted());
    QTRY_VERIFY_WITH_TIMEOUT(player.hasPresentedHdrFrame(), 15000);
    // PQ 首次编译 OBS 完整转换 effect 会明显慢于 P010 HLG 的精简 effect。
    QTRY_VERIFY_WITH_TIMEOUT(player.presentedFrames() > 0, 30000);
    player.close();
    QVERIFY(!player.isStarted());
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
    window.resize(1500, 900);
    window.show();
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
    QVERIFY(player);
    QVERIFY(player->isWindow());
    QVERIFY(!player->windowFlags().testFlag(Qt::FramelessWindowHint));
    QVERIFY(player->windowFlags().testFlag(Qt::WindowTitleHint));
    QTRY_COMPARE(stack->size(), previousSize);
    auto* slot = window.findChild<QWidget*>(QStringLiteral("videoDockSlot"));
    QVERIFY(slot && !slot->isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(player->presentedFrames() > 0, 10000);
    const QPoint position = player->pos();
    window.resize(1200, 800);
    QTest::qWait(100);
    QCOMPARE(player->pos(), position);
    player->resize(1400, 850);
    // 默认小窗居中点不适合放大后继续沿用；保持菜单在屏幕内，避免验收截图被系统裁剪。
    player->move(player->screen()->availableGeometry().topLeft() + QPoint(12, 36));
#if defined(Q_OS_MAC)
    activateFfplayTestWindow(player);
    QTest::qWait(500);
    qInfo() << "[controls-window-native]" << ffplayTestNativeState(player);
#endif
    auto* slider = player->findChild<QSlider*>(QStringLiteral("ffplaySeekSlider"));
    auto* more = player->findChild<QPushButton*>(QStringLiteral("videoMoreButton"));
#if defined(Q_OS_MAC)
    auto* share = player->findChild<QToolButton*>(QStringLiteral("videoShareButton"));
    auto* save = player->findChild<QToolButton*>(QStringLiteral("videoSaveButton"));
    QVERIFY(slider && share && save && !more);
#else
    QVERIFY(slider && more);
#endif
    QHBoxLayout* controlRow = nullptr;
    for (auto* row : player->findChildren<QHBoxLayout*>())
        if (row->indexOf(slider) >= 0) controlRow = row;
    QVERIFY(controlRow && controlRow->parentWidget());
    auto* controlsHost = controlRow->parentWidget();
#if defined(Q_OS_MAC)
    QTRY_VERIFY(slider->isVisible() && share->isVisible() && save->isVisible());
#else
    QTRY_VERIFY(slider->isVisible() && more->isVisible());
#endif
    // macOS overlays a panel on video; Windows lays the same real controls below the surface.
#if defined(Q_OS_MAC)
    auto* panel = player->findChild<QWidget*>(QStringLiteral("ffplayControlPanel"));
    QVERIFY(panel && panel->isVisible());
    QTRY_VERIFY(panel->width() > player->width() * 0.85);
    QTRY_VERIFY(slider->width() > panel->width() * 0.90);
    QVERIFY(slider->mapToGlobal(slider->rect().bottomLeft()).y() < share->mapToGlobal(QPoint()).y());
#endif
    QTRY_VERIFY(controlRow->geometry().width() > player->width() * 0.85);
    QTRY_VERIFY(slider->width() > player->width() * 0.5);
    const QString screenshot = qEnvironmentVariable("MAICHAT_AGENT_VIDEO_SCREENSHOT");
    if (!screenshot.isEmpty()) {
        QVERIFY(controlsHost->grab().save(screenshot + QStringLiteral("-panel.png")));
        // 原生视频层不参与 QWidget::grab；按窗口边框取屏幕区域，验收标题栏和实际画面。
        QTest::qWait(200);
        const QRect frame = player->frameGeometry();
#if defined(Q_OS_MAC)
        if (ffplayTestWindowInteractive(player))
#endif
            QVERIFY(
                player->screen()->grabWindow(0, frame.x(), frame.y(), frame.width(), frame.height()).save(screenshot));
    }
#if !defined(Q_OS_MAC)
    bool menuFitsPlatform = false;
    QTimer dismissMenu;
    dismissMenu.setSingleShot(true);
    connect(&dismissMenu, &QTimer::timeout, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!menu) return;
#if defined(Q_OS_MAC)
        const auto actions = menu->actions();
        menuFitsPlatform = menu->isVisible() && menu->geometry().bottom() <= panel->mapToGlobal(QPoint()).y() &&
            actions.size() == 2 && actions.at(0)->text() == QStringLiteral("分享…") &&
            actions.at(1)->text() == QStringLiteral("保存副本…") &&
            !actions.at(0)->isSeparator() && !actions.at(1)->isSeparator() &&
            !actions.at(0)->menu() && !actions.at(1)->menu();
#else
        // The Windows menu uses the native below-button anchor with Qt's screen-edge adjustment.
        const auto actions = menu->actions();
        menuFitsPlatform = menu->isVisible() && menu->screen() &&
            menu->screen()->availableGeometry().contains(menu->geometry()) && actions.size() == 2 &&
            actions.at(0)->text() == QStringLiteral("复制文件路径") &&
            actions.at(1)->text() == QStringLiteral("打开所在文件夹");
#endif
        qInfo() << "[controls-menu]" << menu->geometry() << "controls-origin"
                << controlsHost->mapToGlobal(controlRow->geometry().topLeft());
        if (!screenshot.isEmpty()) {
            menu->grab().save(screenshot + QStringLiteral("-menu-widget.png"));
            const QRect frame = player->frameGeometry();
#if defined(Q_OS_MAC)
            if (ffplayTestWindowInteractive(player))
#endif
                player->screen()
                    ->grabWindow(0, frame.x(), frame.y(), frame.width(), frame.height())
                    .save(screenshot + QStringLiteral("-menu.png"));
        }
        menu->close();
    });
    dismissMenu.start(200);
    QTest::mouseClick(more, Qt::LeftButton);
    dismissMenu.stop();
    QVERIFY(menuFitsPlatform);
#endif
    player->resize(640, 420);
    QTRY_VERIFY(slider->width() > 160);
#if defined(Q_OS_MAC)
    QVERIFY(share->isVisible() && save->isVisible());
    QTRY_VERIFY(slider->width() > panel->width() * 0.90);
    if (!screenshot.isEmpty()) {
        QTest::qWait(150);
        const QRect frame = player->frameGeometry();
        QVERIFY(player->screen()->grabWindow(0, frame.x(), frame.y(), frame.width(), frame.height())
            .save(screenshot + QStringLiteral("-compact.png")));
    }
    player->showFullScreen();
    QTRY_VERIFY(player->isFullScreen());
    QTRY_VERIFY(slider->width() > player->width() * 0.85);
    QVERIFY(share->isVisible() && save->isVisible());
    if (!screenshot.isEmpty()) {
        QTest::qWait(650);
        const QRect frame = player->frameGeometry();
        QVERIFY(player->screen()->grabWindow(0, frame.x(), frame.y(), frame.width(), frame.height())
            .save(screenshot + QStringLiteral("-fullscreen.png")));
    }
    player->showNormal();
    QTRY_VERIFY(!player->isFullScreen());
#else
    QVERIFY(more->isVisible());
#endif
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
#if defined(Q_OS_MAC)
    QTemporaryDir directory;
    const QString file = directory.filePath(QStringLiteral("controls-state.mp4"));
    QProcess encoder;
    encoder.start(
        QStringLiteral("ffmpeg"),
        {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-f"),
         QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("testsrc2=size=320x180:rate=10"),
         QStringLiteral("-t"), QStringLiteral("20"), QStringLiteral("-c:v"), QStringLiteral("libx264"),
         QStringLiteral("-preset"), QStringLiteral("ultrafast"), file});
    QVERIFY(encoder.waitForFinished(30000));
    QCOMPARE(encoder.exitCode(), 0);
    MaiFfplayVideoDialog player(file);
    player.show();
    QTRY_VERIFY_WITH_TIMEOUT(player.presentedFrames() > 0, 10000);
    player.showFullScreen();
    auto* surface = player.findChild<QWidget*>(QStringLiteral("ffplayVideoSurface"));
    auto* panel = player.findChild<QWidget*>(QStringLiteral("ffplayControlPanel"));
    auto* pause = player.findChild<QPushButton*>(QStringLiteral("ffplayPauseButton"));
    QVERIFY(surface && panel && pause);
    QTRY_VERIFY_WITH_TIMEOUT(panel->isHidden(), 6000);
    // 直接注入 Qt 事件只验证控件状态机，不把它当成真实前台鼠标/Space 验收。
    QMouseEvent activity(QEvent::MouseMove, QPointF(100, 100), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(surface, &activity);
    QVERIFY(panel->isVisible());
    pause->click();
    QTRY_VERIFY(!player.isPlaying());
    QTest::qWait(2800);
    QVERIFY(panel->isVisible());
    player.close();
#else
    QSKIP("macOS overlay controls");
#endif
}

void MaiFfplayVideoDialogTest::fullscreenControlsHideAndWake() {
#if defined(Q_OS_MAC)
    QTemporaryDir directory;
    const QString file = directory.filePath(QStringLiteral("controls.mp4"));
    QProcess encoder;
    encoder.start(
        QStringLiteral("ffmpeg"),
        {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-f"),
         QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("testsrc2=size=640x360:rate=10"),
         QStringLiteral("-t"), QStringLiteral("20"), QStringLiteral("-c:v"), QStringLiteral("libx264"),
         QStringLiteral("-preset"), QStringLiteral("ultrafast"), file});
    QVERIFY(encoder.waitForFinished(30000));
    QCOMPARE(encoder.exitCode(), 0);
    MaiFfplayVideoDialog player(file);
    player.setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint | Qt::WindowMinMaxButtonsHint | Qt::WindowTitleHint);
    player.show();
    activateFfplayTestWindow(&player);
    QTest::qWait(300);
    if (!ffplayTestWindowInteractive(&player)) QSKIP("Test window is outside the active macOS GUI Space");
    QTRY_VERIFY_WITH_TIMEOUT(player.presentedFrames() > 0, 10000);
    player.showFullScreen();
    QTRY_VERIFY(player.isFullScreen());
    auto* surface = player.findChild<QWidget*>(QStringLiteral("ffplayVideoSurface"));
    auto* panel = player.findChild<QWidget*>(QStringLiteral("ffplayControlPanel"));
    auto* pause = player.findChild<QPushButton*>(QStringLiteral("ffplayPauseButton"));
    QVERIFY(surface && panel && pause);
    VideoControlMoveProbe moveProbe(player, *panel);
    qApp->installEventFilter(&moveProbe);
    // Qt 5's QWidget mouseMove only warps QCursor on macOS. Use the QWindow
    // overload to deliver a real Qt MouseMove to the control event-filter path;
    // this does not claim a physical mouse/CGEvent or change the timeout.
    QTest::mouseMove(player.windowHandle(),
        player.mapFromGlobal(surface->mapToGlobal(surface->rect().center())));
    QTRY_VERIFY_WITH_TIMEOUT(panel->isHidden(), 6000);
    const QString screenshot = qEnvironmentVariable("MAICHAT_AGENT_VIDEO_SCREENSHOT");
    if (!screenshot.isEmpty()) {
        const QRect frame = player.frameGeometry();
        player.screen()
            ->grabWindow(0, frame.x(), frame.y(), frame.width(), frame.height())
            .save(screenshot + QStringLiteral("-fullscreen-hidden.png"));
    }
    const int movesBeforeWake = moveProbe.received;
    QTest::mouseMove(player.windowHandle(),
        player.mapFromGlobal(surface->mapToGlobal(surface->rect().center() + QPoint(20, 0))));
    QTRY_VERIFY(moveProbe.received > movesBeforeWake);
    qInfo() << "[controls-wake]" << "pointer" << QCursor::pos() << "window" << player.frameGeometry() << "full"
            << player.isFullScreen() << "playing" << player.isPlaying() << "hidden" << panel->isHidden();
    qInfo() << "[controls-wake-native]" << ffplayTestNativeState(&player) << "surface" << surface->geometry()
            << "origin" << surface->mapToGlobal(QPoint());
    QTRY_VERIFY(!panel->isHidden());
    qInfo() << "[controls-move-revealed]" << "received" << moveProbe.received
            << "hidden" << panel->isHidden() << "playing" << player.isPlaying();
    QTest::mouseClick(pause, Qt::LeftButton);
    QTRY_VERIFY(!player.isPlaying());
    QTest::qWait(2800);
    QVERIFY(panel->isVisible());
    player.showNormal();
    QTRY_VERIFY(!player.isFullScreen());
    QVERIFY(panel->isVisible());
    player.close();
#else
    QSKIP("macOS overlay controls");
#endif
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
        QSKIP("Qt camera preview requires the normal desktop platform plugin");
    if (QCameraInfo::availableCameras().isEmpty()) QSKIP("No camera is connected");
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
        QSKIP("Qt camera preview requires the normal desktop platform plugin");
    if (QCameraInfo::availableCameras().isEmpty()) QSKIP("No camera is connected");
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
    QScreen* captureScreen = nullptr;
    if (!captureDirectory.isEmpty()) {
        QVERIFY(QDir().mkpath(captureDirectory));
        captureScreen = QGuiApplication::primaryScreen();
        QVERIFY(captureScreen);
        // Reserve room for the later main-window move/resize, keeping synthetic UI on screen.
        mainSize = mainSize.boundedTo(captureScreen->availableGeometry().size() - QSize(104, 116));
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
    window.show();
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
    QVERIFY(!player->windowFlags().testFlag(Qt::FramelessWindowHint));
    QVERIFY(player->windowFlags().testFlag(Qt::WindowTitleHint));
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
                const QRect area = window.frameGeometry().intersected(screen->availableGeometry())
                                       .adjusted(24, 48, -24, -24);
                const QSize frame = player->frameGeometry().size() - player->size();
                player->resize(player->size().boundedTo(area.size() - frame));
                player->move(area.center() - QPoint(player->frameGeometry().width() / 2,
                                                    player->frameGeometry().height() / 2));
            }
            player->raise();
            player->activateWindow();
        } else {
            window.activateWindow();
        }
        if (stage == QStringLiteral("closed")) {
            if (player && player->isVisible()) return false;
            // Qt destruction precedes the Windows compositor's closing animation.
            QTest::qWait(350);
        } else {
            QTest::qWait(80);
        }
        QCoreApplication::processEvents();
        QRect region = window.frameGeometry();
        if (player && player->isVisible()) region = region.united(player->frameGeometry());
        region = region.intersected(screen->geometry());
        if (region.isEmpty()) return false;
        const QPixmap pixels = screen->grabWindow(0, region.x(), region.y(), region.width(), region.height());
        const QString filename = QString::fromLatin1(QTest::currentDataTag()) + QLatin1Char('-')
            + stage + QStringLiteral(".png");
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
    player->showFullScreen();
    QTRY_VERIFY(player->isFullScreen());
    QTRY_COMPARE(stack->geometry(), contentBefore);
    QVERIFY(!slot->isVisible() && !pane->isVisible());
    player->showNormal();
    QTRY_VERIFY(!player->isFullScreen());
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
    auto* share = dialog.findChild<QToolButton*>(QStringLiteral("videoShareButton"));
    auto* save = dialog.findChild<QToolButton*>(QStringLiteral("videoSaveButton"));
    QVERIFY(share && save);
    for (auto* button : {share, save}) {
        QCOMPARE(button->toolButtonStyle(), Qt::ToolButtonIconOnly);
        QVERIFY(button->text().isEmpty() && !button->icon().isNull());
        QVERIFY(!button->toolTip().isEmpty() && button->accessibleName() == button->toolTip());
    }
    QVERIFY(!dialog.findChild<QWidget*>(QStringLiteral("videoMoreButton")));
#else
    auto* forward = dialog.findChild<QPushButton*>(QStringLiteral("videoForwardButton"));
    auto* save = dialog.findChild<QPushButton*>(QStringLiteral("videoSaveButton"));
    QVERIFY(forward != nullptr);
    QVERIFY(save != nullptr);
#endif
    bool forwarded = false;
    dialog.setForwardAction([&forwarded](const QString&) { forwarded = true; });
    dialog.forwardVideo();
    QVERIFY(forwarded);
}

void MaiFfplayVideoDialogTest::shareAndSaveIconsInvokeExistingDialogs() {
#if defined(Q_OS_MAC)
    MaiFfplayVideoDialog dialog(QStringLiteral(MAICHAT_FFPLAY_VIDEO_FIXTURE));
    dialog.show();
    activateFfplayTestWindow(&dialog);
    QTRY_VERIFY_WITH_TIMEOUT(dialog.presentedFrames() > 0, 10000);
    auto* share = dialog.findChild<QToolButton*>(QStringLiteral("videoShareButton"));
    auto* save = dialog.findChild<QToolButton*>(QStringLiteral("videoSaveButton"));
    QVERIFY(share && save && share->isVisible() && save->isVisible());
    QSignalSpy shared(share, &QToolButton::clicked);
    observeFfplayTestNativePanel();
    bool sharePickerObserved = false;
    QTimer dismissShare;
    connect(&dismissShare, &QTimer::timeout, [&] {
        if (cancelFfplayTestNativePanel()) {
            sharePickerObserved = true;
            dismissShare.stop();
        }
    });
    dismissShare.start(100);
    // AppKit sharing requires the real native input context; QWidget synthetic
    // clicks bypass NSEvent and cannot validate a system picker presentation.
    QVERIFY(clickFfplayTestNativeButton(share));
    QTRY_COMPARE(shared.count(), 1);
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
    QVERIFY(clickFfplayTestNativeButton(save));
    QTRY_VERIFY(saveOpened);
    QVERIFY(saveOpened);
    QVERIFY(dialog.isVisible());
    dialog.close();
#else
    QSKIP("Native macOS sharing and save dialogs");
#endif
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
    QTRY_VERIFY_WITH_TIMEOUT(slider->value() > 0 && timeLabel->text().contains(QStringLiteral(" / 0:02")), 6000);
    QTRY_VERIFY_WITH_TIMEOUT(dialog->presentedFrames() >= 3, 4000);
    QWidget* surface = dialog->findChild<QWidget*>(QStringLiteral("ffplayVideoSurface"));
    QVERIFY(surface != nullptr);
    const auto hasVisibleColor = [surface] {
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
    dialog.resize(1280, 900);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    QVERIFY(dialog.isStarted());
    QTRY_VERIFY_WITH_TIMEOUT(dialog.presentedFrames() > 0, 5000);
    QWidget* surface = dialog.findChild<QWidget*>(QStringLiteral("ffplayVideoSurface"));
    QVERIFY(surface != nullptr);
    QTest::qWait(500);
    const QPoint topLeft = surface->mapToGlobal(QPoint(0, 0));
    const QImage pixels =
        surface->screen()->grabWindow(0, topLeft.x(), topLeft.y(), surface->width(), surface->height()).toImage();
    const QString capturePath = qEnvironmentVariable("MAICHAT_FFPLAY_CAPTURE");
    if (!capturePath.isEmpty()) QVERIFY(pixels.save(capturePath));
    const QString windowCapture = qEnvironmentVariable("MAICHAT_FFPLAY_WINDOW_CAPTURE");
    if (!windowCapture.isEmpty()) {
        const QRect frame = dialog.frameGeometry();
        QVERIFY(
            dialog.screen()->grabWindow(0, frame.x(), frame.y(), frame.width(), frame.height()).save(windowCapture));
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

QTEST_MAIN(MaiFfplayVideoDialogTest)
#include "MaiFfplayVideoDialogTest.moc"
