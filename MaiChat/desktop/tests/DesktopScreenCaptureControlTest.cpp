// Tests use an owned backend; they never start or stop native screen capture.
#include <QtTest/QtTest>

#include <QElapsedTimer>
#include <QPointer>
#include <QSemaphore>
#include <QSignalSpy>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <memory>
#include <stdexcept>

#include "agent/DesktopScreenCaptureControl.h"
#include "MaiThread.h"

namespace {

MaiScreenshotCaptureState activeState() {
    MaiScreenshotCaptureState state;
    state.workerRunning = true;
    state.capturing = true;
    state.workerStarts = 3;
    state.sessionStarts = 5;
    state.framesReceived = 11;
    return state;
}

struct BlockingStop {
    QSemaphore entered;
    QSemaphore release;
    std::atomic<int> calls{0};
    std::atomic<bool> active{true};
    std::atomic<bool> completed{false};
    std::atomic<bool> ranOnGuiThread{false};
    std::atomic<bool> namedStopThread{false};
};

// A failed assertion must not leave an owned task waiting in the global pool.
struct ReleaseStopOnExit {
    std::shared_ptr<BlockingStop> state;
    ~ReleaseStopOnExit() {
        state->release.release();
    }
};

DesktopScreenCaptureControl::Backend blockingBackend(const std::shared_ptr<BlockingStop>& state) {
    auto* guiThread = QThread::currentThread();
    return {[state] {
                MaiScreenshotCaptureState result;
                result.workerRunning = state->active.load();
                result.capturing = state->active.load();
                return result;
            },
            [state, guiThread] {
                ++state->calls;
                state->ranOnGuiThread = QThread::currentThread() == guiThread;
                state->namedStopThread = MaiThread::currentName() == "mai-screen-stop";
                state->entered.release();
                state->release.acquire();
                state->active = false;
                state->completed = true;
                return MaiError{};
            }};
}

}  // namespace

class DesktopScreenCaptureControlTest final : public QObject {
    Q_OBJECT

private slots:
    void readsStateWithoutStartingOrStopping();
    void pollingFindsResidentWorker();
    void stopRunsOffGuiThreadAndCoalesces();
    void failureKeepsActualStateAndCanRetry();
    void successDoesNotInventStoppedState();
    void stoppingCanBeRetriedAfterExternalRestart();
    void backendExceptionsRemainRecoverable();
    void stateReadFailurePreservesLastKnownState();
    void destructionDoesNotWaitOrReceiveCompletion();
};

void DesktopScreenCaptureControlTest::readsStateWithoutStartingOrStopping() {
    MaiScreenshotCaptureState source = activeState();
    int reads = 0;
    int stops = 0;
    bool readOnGuiThread = true;
    auto* guiThread = QThread::currentThread();
    DesktopScreenCaptureControl control(
        {[&] {
             ++reads;
             readOnGuiThread &= QThread::currentThread() == guiThread;
             return source;
         },
         [&] {
             ++stops;
             return MaiError{};
         }});

    QCOMPARE(reads, 1);
    QCOMPARE(stops, 0);
    QVERIFY(!control.isStopping());
    QVERIFY(control.errorMessage().isEmpty());
    QVERIFY(control.captureState().workerRunning);
    QCOMPARE(control.captureState().sessionStarts, std::uint64_t(5));
    QSignalSpy changed(&control, &DesktopScreenCaptureControl::stateChanged);
    source.framesReceived = 12;
    control.refresh();
    QCOMPARE(reads, 2);
    QCOMPARE(stops, 0);
    QCOMPARE(control.captureState().framesReceived, std::uint64_t(12));
    QCOMPARE(changed.count(), 1);
    QVERIFY(readOnGuiThread);
}

void DesktopScreenCaptureControlTest::pollingFindsResidentWorker() {
    MaiScreenshotCaptureState source;
    DesktopScreenCaptureControl control({[&] { return source; }, [] { return MaiError{}; }});
    QVERIFY(!control.captureState().workerRunning);
    source.workerRunning = true;
    source.capturing = false;
    QTRY_VERIFY_WITH_TIMEOUT(control.captureState().workerRunning, 1500);
    QVERIFY(!control.captureState().capturing);
}

void DesktopScreenCaptureControlTest::stopRunsOffGuiThreadAndCoalesces() {
    auto state = std::make_shared<BlockingStop>();
    ReleaseStopOnExit release{state};
    DesktopScreenCaptureControl control(blockingBackend(state));
    QSignalSpy changed(&control, &DesktopScreenCaptureControl::stateChanged);
    QElapsedTimer callTime;
    callTime.start();
    control.requestStop();
    QVERIFY(callTime.elapsed() < 500);
    QVERIFY(control.isStopping());
    QTRY_VERIFY_WITH_TIMEOUT(state->entered.available() == 1, 1500);
    QVERIFY(!state->ranOnGuiThread.load());
    QVERIFY(state->namedStopThread.load());
    control.requestStop();
    control.requestStop();
    QCOMPARE(state->calls.load(), 1);

    int heartbeats = 0;
    QTimer heartbeat;
    heartbeat.setInterval(10);
    connect(&heartbeat, &QTimer::timeout, &control, [&] { ++heartbeats; });
    heartbeat.start();
    QTRY_VERIFY_WITH_TIMEOUT(heartbeats >= 3, 1000);
    QVERIFY(control.isStopping());
    QVERIFY(control.captureState().capturing);
    state->release.release();
    QTRY_VERIFY_WITH_TIMEOUT(!control.isStopping(), 1500);
    QVERIFY(!control.captureState().workerRunning);
    QVERIFY(!control.captureState().capturing);
    QVERIFY(control.errorMessage().isEmpty());
    QVERIFY(changed.count() >= 2);
}

void DesktopScreenCaptureControlTest::failureKeepsActualStateAndCanRetry() {
    auto active = std::make_shared<std::atomic<bool>>(true);
    auto calls = std::make_shared<std::atomic<int>>(0);
    bool stateReadFails = false;
    DesktopScreenCaptureControl control(
        {[active, &stateReadFails] {
             if (stateReadFails) throw std::runtime_error("owned temporary state failure");
             auto result = activeState();
             result.workerRunning = active->load();
             result.capturing = active->load();
             return result;
         },
         [active, calls] {
             if (++*calls == 1) return MaiError(MaiErrorCode::Internal, "owned stop failure");
             *active = false;
             return MaiError{};
         }});
    control.requestStop();
    QTRY_VERIFY_WITH_TIMEOUT(!control.isStopping(), 1500);
    QCOMPARE(calls->load(), 1);
    QVERIFY(control.captureState().capturing);
    QVERIFY(control.errorMessage().contains(QStringLiteral("owned stop failure")));
    control.refresh();
    QVERIFY(control.errorMessage().contains(QStringLiteral("owned stop failure")));
    stateReadFails = true;
    control.refresh();
    QVERIFY(control.errorMessage().contains(QStringLiteral("owned temporary state failure")));
    stateReadFails = false;
    control.refresh();
    QVERIFY(control.errorMessage().contains(QStringLiteral("owned stop failure")));

    control.requestStop();
    QTRY_VERIFY_WITH_TIMEOUT(!control.isStopping(), 1500);
    QCOMPARE(calls->load(), 2);
    QVERIFY(!control.captureState().capturing);
    QVERIFY(control.errorMessage().isEmpty());
}

void DesktopScreenCaptureControlTest::successDoesNotInventStoppedState() {
    DesktopScreenCaptureControl control({[] { return activeState(); }, [] { return MaiError{}; }});
    control.requestStop();
    QTRY_VERIFY_WITH_TIMEOUT(!control.isStopping(), 1500);
    QVERIFY(control.captureState().workerRunning);
    QVERIFY(control.captureState().capturing);
    QVERIFY(!control.errorMessage().isEmpty());
}

void DesktopScreenCaptureControlTest::stoppingCanBeRetriedAfterExternalRestart() {
    auto active = std::make_shared<std::atomic<bool>>(true);
    auto calls = std::make_shared<std::atomic<int>>(0);
    DesktopScreenCaptureControl control(
        {[active] {
             MaiScreenshotCaptureState result;
             result.workerRunning = active->load();
             result.capturing = active->load();
             return result;
         },
         [active, calls] {
             ++*calls;
             *active = false;
             return MaiError{};
         }});
    control.requestStop();
    QTRY_VERIFY_WITH_TIMEOUT(!control.isStopping(), 1500);
    QVERIFY(!control.captureState().capturing);
    *active = true;
    QTRY_VERIFY_WITH_TIMEOUT(control.captureState().capturing, 1500);
    control.requestStop();
    QTRY_VERIFY_WITH_TIMEOUT(!control.isStopping(), 1500);
    QCOMPARE(calls->load(), 2);
    QVERIFY(!control.captureState().capturing);
}

void DesktopScreenCaptureControlTest::backendExceptionsRemainRecoverable() {
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto active = std::make_shared<std::atomic<bool>>(true);
    DesktopScreenCaptureControl control(
        {[active] {
             MaiScreenshotCaptureState result;
             result.capturing = active->load();
             return result;
         },
         [calls, active]() -> MaiError {
             const int call = ++*calls;
             if (call == 1) throw std::runtime_error("owned thrown stop failure");
             if (call == 2) throw 7;
             *active = false;
             return {};
         }});
    for (int attempt = 1; attempt <= 2; ++attempt) {
        control.requestStop();
        QTRY_VERIFY_WITH_TIMEOUT(!control.isStopping(), 1500);
        QCOMPARE(calls->load(), attempt);
        QVERIFY(control.captureState().capturing);
        QVERIFY(!control.errorMessage().isEmpty());
    }
    control.requestStop();
    QTRY_VERIFY_WITH_TIMEOUT(!control.isStopping(), 1500);
    QCOMPARE(calls->load(), 3);
    QVERIFY(!control.captureState().capturing);
    QVERIFY(control.errorMessage().isEmpty());
}

void DesktopScreenCaptureControlTest::stateReadFailurePreservesLastKnownState() {
    bool readFails = false;
    MaiScreenshotCaptureState source = activeState();
    DesktopScreenCaptureControl control(
        {[&] {
             if (readFails) throw std::runtime_error("owned state failure");
             return source;
         },
         [] { return MaiError{}; }});
    readFails = true;
    control.refresh();
    QVERIFY(control.captureState().capturing);
    QVERIFY(control.errorMessage().contains(QStringLiteral("owned state failure")));
    readFails = false;
    source = {};
    control.refresh();
    QVERIFY(!control.captureState().capturing);
    QVERIFY(control.errorMessage().isEmpty());
}

void DesktopScreenCaptureControlTest::destructionDoesNotWaitOrReceiveCompletion() {
    auto state = std::make_shared<BlockingStop>();
    ReleaseStopOnExit release{state};
    auto* control = new DesktopScreenCaptureControl(blockingBackend(state));
    QPointer<DesktopScreenCaptureControl> guard(control);
    int changes = 0;
    connect(control, &DesktopScreenCaptureControl::stateChanged, this, [&] { ++changes; });
    control->requestStop();
    QTRY_VERIFY_WITH_TIMEOUT(state->entered.available() == 1, 1500);
    QElapsedTimer deleteTime;
    deleteTime.start();
    delete control;
    QVERIFY(deleteTime.elapsed() < 500);
    QVERIFY(guard.isNull());
    const int beforeCompletion = changes;
    state->release.release();
    QTRY_VERIFY_WITH_TIMEOUT(state->completed.load(), 1500);
    QCoreApplication::processEvents();
    QCOMPARE(changes, beforeCompletion);
}

QTEST_MAIN(DesktopScreenCaptureControlTest)
#include "DesktopScreenCaptureControlTest.moc"
