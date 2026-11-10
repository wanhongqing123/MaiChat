#include "agent/DesktopScreenCaptureControl.h"

#include <QFutureWatcher>
#include <QThread>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <exception>
#include <utility>

#include "MaiThread.h"

namespace {

class StopWorkerName final {
public:
    StopWorkerName() : previous_(MaiThread::currentName()) {
        MaiThread::setCurrentName("mai-screen-stop");
    }
    ~StopWorkerName() {
        // Global-pool threads are reused; do not label unrelated later work as
        // a screen stop. Retain any known previous role, otherwise name the pool.
        MaiThread::setCurrentName(previous_.empty() ? "mai-qt-pool" : previous_);
    }

private:
    std::string previous_;
};

bool sameState(const MaiScreenshotCaptureState& left, const MaiScreenshotCaptureState& right) {
    return left.workerRunning == right.workerRunning && left.capturing == right.capturing &&
           left.workerStarts == right.workerStarts && left.deviceCreations == right.deviceCreations &&
           left.sessionStarts == right.sessionStarts && left.framesReceived == right.framesReceived;
}

MaiError stopBackend(const DesktopScreenCaptureControl::Backend& backend) {
    StopWorkerName threadName;
    if (!backend.stop)
        return {MaiErrorCode::NotConfigured, "Screen capture stop is unavailable."};
    try {
        return backend.stop();
    } catch (const std::exception& error) {
        return {MaiErrorCode::Internal, std::string("Screen capture stop failed: ") + error.what()};
    } catch (...) {
        return {MaiErrorCode::Internal, "Screen capture stop failed unexpectedly."};
    }
}

}  // namespace

DesktopScreenCaptureControl::DesktopScreenCaptureControl(QObject* parent)
    : DesktopScreenCaptureControl(Backend{maiScreenshotCaptureState, maiStopScreenshotCapture}, parent) {}

DesktopScreenCaptureControl::DesktopScreenCaptureControl(Backend backend, QObject* parent)
    : QObject(parent), backend_(std::move(backend)), pollTimer_(new QTimer(this)) {
    // Polling the cheap snapshot must never acquire a native capture session.
    pollTimer_->setInterval(500);
    connect(pollTimer_, &QTimer::timeout, this, &DesktopScreenCaptureControl::refresh);
    refresh();
    pollTimer_->start();
}

// QFutureWatcher disconnects on destruction without waiting for its future.
// The worker owns only a Backend copy, so it can finish safely after this object.
DesktopScreenCaptureControl::~DesktopScreenCaptureControl() = default;

MaiScreenshotCaptureState DesktopScreenCaptureControl::captureState() const {
    return state_;
}

bool DesktopScreenCaptureControl::isStopping() const {
    return stopping_;
}

QString DesktopScreenCaptureControl::errorMessage() const {
    return error_;
}

void DesktopScreenCaptureControl::readState() {
    try {
        if (!backend_.state) {
            error_ = QStringLiteral("Screen capture state is unavailable.");
            return;
        }
        const MaiScreenshotCaptureState current = backend_.state();
        state_ = current;
        // A recovered snapshot read must not silently erase an earlier failed
        // stop. That failure remains actionable until the user explicitly retries.
        error_ = stopError_;
    } catch (const std::exception& error) {
        // Keep the last known flags: a failed read does not prove capture stopped.
        error_ = QStringLiteral("Unable to read screen capture state: %1")
                     .arg(QString::fromUtf8(error.what()));
    } catch (...) {
        error_ = QStringLiteral("Unable to read screen capture state.");
    }
}

void DesktopScreenCaptureControl::refresh() {
    Q_ASSERT(QThread::currentThread() == thread());
    const MaiScreenshotCaptureState before = state_;
    const QString previousError = error_;
    readState();
    if (!sameState(before, state_) || previousError != error_) emit stateChanged();
}

void DesktopScreenCaptureControl::requestStop() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (stopping_) return;
    stopping_ = true;
    error_.clear();
    stopError_.clear();

    auto* watcher = new QFutureWatcher<MaiError>(this);
    connect(watcher, &QFutureWatcher<MaiError>::finished, this, [this, watcher] {
        MaiError result;
        try {
            result = watcher->result();
        } catch (...) {
            result = {MaiErrorCode::Internal, "Screen capture stop could not complete."};
        }
        watcher->deleteLater();
        completeStop(result);
    });
    try {
        // Never capture this or a widget. In-flight backend resources must obey
        // Backend's documented copied-lifetime contract after control deletion.
        watcher->setFuture(QtConcurrent::run([backend = backend_] { return stopBackend(backend); }));
    } catch (...) {
        delete watcher;
        completeStop({MaiErrorCode::Internal, "Screen capture stop could not start."});
        return;
    }
    emit stateChanged();
}

void DesktopScreenCaptureControl::completeStop(const MaiError& error) {
    Q_ASSERT(QThread::currentThread() == thread());
    stopping_ = false;
    stopError_ = error ? QString::fromUtf8(error.message().c_str()) : QString();
    if (error && stopError_.isEmpty()) stopError_ = QStringLiteral("Screen capture stop failed.");
    readState();
    if (error_.isEmpty() && (state_.workerRunning || state_.capturing)) {
        stopError_ = QStringLiteral("Screen capture is still active. Try stopping it again.");
        error_ = stopError_;
    }
    emit stateChanged();
}
