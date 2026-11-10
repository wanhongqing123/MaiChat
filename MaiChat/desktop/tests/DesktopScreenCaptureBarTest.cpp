// This widget suite uses owned backends and never touches native capture.
#include <QtTest/QtTest>

#include <QLabel>
#include <QPushButton>
#include <QSemaphore>
#include <QVBoxLayout>

#include <atomic>
#include <memory>

#include "agent/DesktopScreenCaptureBar.h"
#include "agent/DesktopScreenCaptureControl.h"

namespace {

struct Capture {
    std::atomic<bool> worker{false};
    std::atomic<bool> capturing{false};
    std::atomic<int> stops{0};
    std::atomic<bool> failNext{false};
    std::atomic<bool> blockStop{false};
    QSemaphore entered;
    QSemaphore release;
};

struct ReleaseOnExit {
    std::shared_ptr<Capture> capture;
    ~ReleaseOnExit() {
        capture->release.release();
    }
};

DesktopScreenCaptureControl::Backend backend(const std::shared_ptr<Capture>& capture) {
    return {[capture] {
                MaiScreenshotCaptureState result;
                result.workerRunning = capture->worker.load();
                result.capturing = capture->capturing.load();
                return result;
            },
            [capture] {
                ++capture->stops;
                if (capture->blockStop.load()) {
                    capture->entered.release();
                    capture->release.acquire();
                }
                if (capture->failNext.exchange(false))
                    return MaiError(MaiErrorCode::Internal, "owned capture stop failure");
                capture->worker = false;
                capture->capturing = false;
                return MaiError{};
            }};
}

DesktopScreenCaptureBar* addBar(QWidget& window, DesktopScreenCaptureControl& control) {
    auto* layout = new QVBoxLayout(&window);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* bar = new DesktopScreenCaptureBar(control, &window);
    layout->addWidget(bar);
    layout->addStretch();
    window.resize(640, 180);
    window.show();
    return bar;
}

}  // namespace

class DesktopScreenCaptureBarTest final : public QObject {
    Q_OBJECT

private slots:
    void inactiveHiddenAndLocalCaptureStateIsExplicit();
    void residentWorkerShowsWaitingState();
    void stopButtonProjectsAsyncStopping();
    void failureKeepsRetryUntilActualStopAndRestart();
    void narrowLayoutKeepsStopReachableAndPaintsAmber();
    void replacingBarDoesNotReplaceSharedControl();
};

void DesktopScreenCaptureBarTest::inactiveHiddenAndLocalCaptureStateIsExplicit() {
    auto capture = std::make_shared<Capture>();
    DesktopScreenCaptureControl control(backend(capture));
    QWidget window;
    auto* bar = addBar(window, control);
    auto* label = bar->findChild<QLabel*>(QStringLiteral("agentScreenCaptureLabel"));
    auto* stop = bar->findChild<QPushButton*>(QStringLiteral("agentStopScreenCapture"));
    QVERIFY(label && stop);
    QCOMPARE(bar->objectName(), QStringLiteral("agentScreenCaptureStatus"));
    QVERIFY(bar->isHidden());
    QCOMPARE(capture->stops.load(), 0);

    capture->worker = true;
    capture->capturing = true;
    control.refresh();
    QTRY_VERIFY(bar->isVisible());
    QCOMPARE(label->text(), QStringLiteral("本机屏幕采集中 · 所有会话共用"));
    QCOMPARE(stop->text(), QStringLiteral("停止采集"));
    QVERIFY(stop->isEnabled());
    QVERIFY(stop->toolTip().contains(QStringLiteral("新的截图请求可再次开启")));
    QCOMPARE(capture->stops.load(), 0);
}

void DesktopScreenCaptureBarTest::residentWorkerShowsWaitingState() {
    auto capture = std::make_shared<Capture>();
    capture->worker = true;
    DesktopScreenCaptureControl control(backend(capture));
    QWidget window;
    auto* bar = addBar(window, control);
    auto* label = bar->findChild<QLabel*>(QStringLiteral("agentScreenCaptureLabel"));
    QVERIFY(label);
    QVERIFY(bar->isVisible());
    QCOMPARE(label->text(), QStringLiteral("本机屏幕采集待命 · 所有会话共用"));
}

void DesktopScreenCaptureBarTest::stopButtonProjectsAsyncStopping() {
    auto capture = std::make_shared<Capture>();
    capture->worker = true;
    capture->capturing = true;
    capture->blockStop = true;
    ReleaseOnExit release{capture};
    DesktopScreenCaptureControl control(backend(capture));
    QWidget window;
    auto* bar = addBar(window, control);
    auto* label = bar->findChild<QLabel*>(QStringLiteral("agentScreenCaptureLabel"));
    auto* stop = bar->findChild<QPushButton*>(QStringLiteral("agentStopScreenCapture"));
    QVERIFY(label && stop);
    QTest::mouseClick(stop, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(capture->entered.available() == 1, 1500);
    QVERIFY(control.isStopping());
    QVERIFY(bar->isVisible());
    QVERIFY(!stop->isEnabled());
    QVERIFY(label->text().contains(QStringLiteral("正在停止本机屏幕采集")));
    stop->click();
    QCOMPARE(capture->stops.load(), 1);
    capture->release.release();
    QTRY_VERIFY_WITH_TIMEOUT(!control.isStopping(), 1500);
    QVERIFY(bar->isHidden());
}

void DesktopScreenCaptureBarTest::failureKeepsRetryUntilActualStopAndRestart() {
    auto capture = std::make_shared<Capture>();
    capture->worker = true;
    capture->capturing = true;
    capture->failNext = true;
    DesktopScreenCaptureControl control(backend(capture));
    QWidget window;
    auto* bar = addBar(window, control);
    auto* label = bar->findChild<QLabel*>(QStringLiteral("agentScreenCaptureLabel"));
    auto* stop = bar->findChild<QPushButton*>(QStringLiteral("agentStopScreenCapture"));
    QVERIFY(label && stop);
    QTest::mouseClick(stop, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!control.isStopping(), 1500);
    QCOMPARE(capture->stops.load(), 1);
    QVERIFY(bar->isVisible());
    QVERIFY(label->text().contains(QStringLiteral("本机屏幕采集状态异常")));
    QVERIFY(label->text().contains(QStringLiteral("owned capture stop failure")));
    QCOMPARE(stop->text(), QStringLiteral("重试停止"));
    QVERIFY(stop->isEnabled());
    control.refresh();
    QCOMPARE(stop->text(), QStringLiteral("重试停止"));

    QTest::mouseClick(stop, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!control.isStopping(), 1500);
    QCOMPARE(capture->stops.load(), 2);
    QVERIFY(bar->isHidden());
    capture->worker = true;
    capture->capturing = true;
    control.refresh();
    QVERIFY(bar->isVisible());
    QCOMPARE(stop->text(), QStringLiteral("停止采集"));
    QCOMPARE(label->text(), QStringLiteral("本机屏幕采集中 · 所有会话共用"));
}

void DesktopScreenCaptureBarTest::narrowLayoutKeepsStopReachableAndPaintsAmber() {
    auto capture = std::make_shared<Capture>();
    capture->worker = true;
    capture->capturing = true;
    DesktopScreenCaptureControl control(backend(capture));
    QWidget window;
    auto* bar = addBar(window, control);
    window.resize(280, 180);
    auto* label = bar->findChild<QLabel*>(QStringLiteral("agentScreenCaptureLabel"));
    auto* stop = bar->findChild<QPushButton*>(QStringLiteral("agentStopScreenCapture"));
    QVERIFY(label && stop);
    QTRY_VERIFY(stop->isVisible());
    QTRY_VERIFY(bar->width() <= window.contentsRect().width());
    QVERIFY(bar->rect().contains(stop->geometry()));
    QVERIFY(label->geometry().right() < stop->geometry().left());
    QVERIFY(stop->width() >= stop->fontMetrics().horizontalAdvance(stop->text()));
    const QImage painted = bar->grab().toImage();
    QVERIFY(!painted.isNull());
    QCOMPARE(painted.pixelColor(3, 3), QColor(QStringLiteral("#fff7ed")));
    QTest::mouseClick(stop, Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(capture->stops.load(), 1, 1500);
    QTRY_VERIFY_WITH_TIMEOUT(bar->isHidden(), 1500);
}

void DesktopScreenCaptureBarTest::replacingBarDoesNotReplaceSharedControl() {
    auto capture = std::make_shared<Capture>();
    capture->worker = true;
    capture->capturing = true;
    DesktopScreenCaptureControl control(backend(capture));
    auto first = std::make_unique<DesktopScreenCaptureBar>(control);
    first.reset();
    QCOMPARE(capture->stops.load(), 0);
    QWidget window;
    auto* bar = addBar(window, control);
    QVERIFY(bar->isVisible());
    capture->worker = false;
    capture->capturing = false;
    control.refresh();
    QVERIFY(bar->isHidden());
}

QTEST_MAIN(DesktopScreenCaptureBarTest)
#include "DesktopScreenCaptureBarTest.moc"
