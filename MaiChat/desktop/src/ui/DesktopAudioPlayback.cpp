// 文件作用：为桌面控件绑定普通 FFplay 音频会话，并在音乐、inline 音频和视频之间转交所有权。
#include "ui/DesktopAudioPlayback.h"

#include <QByteArray>
#include <QFileInfo>
#include <QPointer>
#include <QScopedValueRollback>
#include <QThreadPool>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

#include "MaiFfplayAudioSession.h"
#include "MaiMediaProbe.h"

namespace {

struct ProbeResult {
    MaiMediaProbeResult media{};
    std::atomic<bool> ready{false};
};

struct PlaybackOwner {
    QPointer<DesktopAudioPlayback> active;
    QPointer<DesktopAudioPlayback> pending;
    uint64_t activeGeneration = 0;
    uint64_t pendingGeneration = 0;
    uint64_t nextGeneration = 0;
    bool handoff = false;
    std::function<void()> closeVideo;
};

PlaybackOwner& playbackOwner() {
    static PlaybackOwner owner;
    return owner;
}

bool readableSource(const QString& source) {
    const QFileInfo file(source);
    return !source.isEmpty() && file.isFile() && file.isReadable() && file.size() > 0;
}

}  // namespace

struct DesktopAudioPlayback::Runtime {
    QString source;
    QString error;
    State state = State::Stopped;
    qint64 position = 0;
    qint64 duration = 0;
    QTimer* timer = nullptr;
    MaiFfplayAudioSession* session = nullptr;
    std::shared_ptr<ProbeResult> probe;
    MaiMediaProbeResult media{};
    bool probed = false;
    bool playRequested = false;
    uint64_t ownerGeneration = 0;
    uint64_t requestGeneration = 0;
};

DesktopAudioPlayback::DesktopAudioPlayback(QObject* parent) : QObject(parent), runtime_(std::make_unique<Runtime>()) {
    runtime_->timer = new QTimer(this);
    runtime_->timer->setInterval(25);
    runtime_->timer->setTimerType(Qt::PreciseTimer);
    connect(runtime_->timer, &QTimer::timeout, this, &DesktopAudioPlayback::refresh);
}

DesktopAudioPlayback::~DesktopAudioPlayback() {
    runtime_->timer->stop();
    if (ownsPendingStart()) playbackOwner().pending.clear();
    runtime_->playRequested = false;
    // 探测任务只持有独立结果和路径，不持有 QObject；销毁后没有晚到的 UI 回调。
    runtime_->probe.reset();
    releaseSession();
}

void DesktopAudioPlayback::setSource(const QString& path) {
    const QString source = path.isEmpty() ? QString() : QFileInfo(path).absoluteFilePath();
    if (source == runtime_->source && runtime_->state != State::Error) return;
    QPointer<DesktopAudioPlayback> alive(this);
    stop();
    if (!alive) return;
    runtime_->source = source;
    runtime_->error.clear();
    runtime_->probed = false;
    runtime_->media = {};
    runtime_->probe.reset();
    setDuration(0);
    if (!alive || source.isEmpty()) return;
    if (!readableSource(source)) {
        reportError(QStringLiteral("音频或视频文件不存在、为空或不可读取。"));
        return;
    }
    const auto result = std::make_shared<ProbeResult>();
    runtime_->probe = result;
    const std::string utf8 = source.toUtf8().toStdString();
    // Qt 只负责分发和接收结果；容器探测完全使用普通媒体接口，不启动播放或音频设备。
    QThreadPool::globalInstance()->start([result, utf8] {
        result->media = maiMediaProbeFile(utf8.c_str());
        result->ready.store(true, std::memory_order_release);
    });
    runtime_->timer->start();
}

QString DesktopAudioPlayback::source() const {
    return runtime_->source;
}
DesktopAudioPlayback::State DesktopAudioPlayback::state() const {
    return runtime_->state;
}
qint64 DesktopAudioPlayback::positionUs() const {
    return runtime_->position;
}
qint64 DesktopAudioPlayback::durationUs() const {
    return runtime_->duration;
}
QString DesktopAudioPlayback::errorString() const {
    return runtime_->error;
}

bool DesktopAudioPlayback::ownsSession() const {
    const auto& owner = playbackOwner();
    return runtime_->session && owner.active == this && owner.activeGeneration == runtime_->ownerGeneration;
}

bool DesktopAudioPlayback::ownsPendingStart() const {
    const auto& owner = playbackOwner();
    return runtime_->playRequested && owner.pending == this && owner.pendingGeneration == runtime_->requestGeneration;
}

void DesktopAudioPlayback::setState(State state) {
    if (runtime_->state == state) return;
    runtime_->state = state;
    emit stateChanged(state);
}

void DesktopAudioPlayback::setPosition(qint64 positionUs) {
    const qint64 position = std::max<qint64>(0, positionUs);
    if (runtime_->position == position) return;
    runtime_->position = position;
    emit positionChanged(position);
}

void DesktopAudioPlayback::setDuration(qint64 durationUs) {
    const qint64 duration = std::max<qint64>(0, durationUs);
    if (runtime_->duration == duration) return;
    runtime_->duration = duration;
    emit durationChanged(duration);
}

void DesktopAudioPlayback::releaseSession() {
    // handle 在让出 owner 前停止并 join；旧对象之后即使收到排队命令也无法调用全局 FFplay。
    if (runtime_->session) {
        auto* session = std::exchange(runtime_->session, nullptr);
        maiFfplayAudioStop(session);
    }
    auto& owner = playbackOwner();
    if (owner.active == this && owner.activeGeneration == runtime_->ownerGeneration) owner.active.clear();
    runtime_->ownerGeneration = 0;
}

void DesktopAudioPlayback::reportError(const QString& error) {
    if (ownsPendingStart()) playbackOwner().pending.clear();
    runtime_->playRequested = false;
    releaseSession();
    runtime_->error = error;
    QPointer<DesktopAudioPlayback> alive(this);
    setState(State::Error);
    if (alive) emit errorOccurred(error);
}

bool DesktopAudioPlayback::play() {
    auto& owner = playbackOwner();
    if (owner.handoff) return false;
    QPointer<DesktopAudioPlayback> alive(this);
    if (runtime_->state == State::Error) {
        // 用户再次点击时重新验证同一路径，文件恢复后不能沿用旧的探测失败或空任务。
        const QString source = runtime_->source;
        setSource(source);
        if (!alive) return false;
    }
    if (!readableSource(runtime_->source)) {
        reportError(QStringLiteral("音频文件不存在、为空或不可读取。"));
        return false;
    }
    const QPointer<DesktopAudioPlayback> previousPending = owner.pending;
    runtime_->requestGeneration = ++owner.nextGeneration;
    runtime_->playRequested = true;
    owner.pending = this;
    owner.pendingGeneration = runtime_->requestGeneration;
    if (previousPending && previousPending != this) previousPending->stop();
    if (!alive || !ownsPendingStart()) return false;
    if (ownsSession()) {
        int exitCode = 0;
        if (!maiFfplayAudioFinished(runtime_->session, &exitCode)) {
            owner.pending.clear();
            runtime_->playRequested = false;
            MaiFfplayPlaybackStatus status{};
            if (maiFfplayAudioStatus(runtime_->session, &status) && status.paused) {
                if (!maiFfplayAudioCommand(runtime_->session, "pause")) return false;
                setState(State::Playing);
            }
            return true;
        }
        releaseSession();
    }
    if (!runtime_->probed) {
        // 先完成异步本地验证，畸形文件不会抢占正在播放的另一张卡片。
        setState(State::Starting);
        if (alive) runtime_->timer->start();
        return true;
    }
    return startPlayback();
}

bool DesktopAudioPlayback::startPlayback() {
    auto& owner = playbackOwner();
    if (!ownsPendingStart() || owner.handoff) return false;
    if (runtime_->media.error_code < 0 || !runtime_->media.has_audio || !readableSource(runtime_->source)) {
        reportError(QStringLiteral("文件没有可播放的音轨，或文件已不可读取。"));
        return false;
    }
    QScopedValueRollback<bool> handoff(owner.handoff, true);
    QPointer<DesktopAudioPlayback> alive(this);
    if (owner.active && owner.active != this) owner.active->stop();
    if (!alive || !ownsPendingStart()) return false;
    const auto closeVideo = owner.closeVideo;
    if (closeVideo) closeVideo();
    if (!alive || !ownsPendingStart()) return false;
    owner.pending.clear();
    runtime_->playRequested = false;
    runtime_->error.clear();
    owner.active = this;
    owner.activeGeneration = runtime_->ownerGeneration = runtime_->requestGeneration;
    const QByteArray utf8 = runtime_->source.toUtf8();
    runtime_->session = maiFfplayAudioStart(utf8.constData());
    if (!runtime_->session) {
        reportError(QStringLiteral("无法启动 FFplay 音频播放器。"));
        return false;
    }
    setPosition(0);
    if (!alive) return false;
    runtime_->timer->start();
    setState(State::Starting);
    return true;
}

bool DesktopAudioPlayback::pause() {
    QPointer<DesktopAudioPlayback> alive(this);
    const bool pending = ownsPendingStart();
    if (pending) {
        playbackOwner().pending.clear();
        runtime_->playRequested = false;
    }
    if (!ownsSession()) {
        if (pending) setState(State::Paused);
        return pending;
    }
    MaiFfplayPlaybackStatus status{};
    if (!maiFfplayAudioStatus(runtime_->session, &status)) {
        // 首帧/设备尚未就绪时直接收回启动任务，切页后不能再出现迟到的声音。
        releaseSession();
        setState(State::Paused);
        return true;
    }
    if (!status.paused && !maiFfplayAudioCommand(runtime_->session, "pause")) return false;
    setPosition(status.position_us);
    if (alive) setState(State::Paused);
    return true;
}

bool DesktopAudioPlayback::seek(qint64 positionUs) {
    return ownsSession() && positionUs >= 0 && (runtime_->duration == 0 || positionUs <= runtime_->duration) &&
           maiFfplayAudioSeekAbsoluteUs(runtime_->session, positionUs) == 1;
}

void DesktopAudioPlayback::stop() {
    if (ownsPendingStart()) playbackOwner().pending.clear();
    runtime_->playRequested = false;
    releaseSession();
    runtime_->error.clear();
    QPointer<DesktopAudioPlayback> alive(this);
    setPosition(0);
    if (alive) setState(State::Stopped);
}

void DesktopAudioPlayback::refresh() {
    QPointer<DesktopAudioPlayback> alive(this);
    if (runtime_->probe && runtime_->probe->ready.load(std::memory_order_acquire)) {
        runtime_->media = runtime_->probe->media;
        runtime_->probe.reset();
        runtime_->probed = true;
        if (runtime_->media.error_code < 0) {
            reportError(QStringLiteral("无法读取媒体信息（%1）。").arg(runtime_->media.error_code));
            return;
        }
        setDuration(runtime_->media.duration_us);
        if (!alive) return;
        if (ownsPendingStart() && !startPlayback()) return;
        if (!alive) return;
    }
    if (!ownsSession()) {
        if (!runtime_->probe) runtime_->timer->stop();
        return;
    }
    int exitCode = 0;
    if (maiFfplayAudioFinished(runtime_->session, &exitCode)) {
        releaseSession();
        if (exitCode != 0) {
            reportError(QStringLiteral("FFplay 无法播放此音频（%1）。").arg(exitCode));
            return;
        }
        setPosition(runtime_->duration);
        if (alive) setState(State::Ended);
        return;
    }
    MaiFfplayPlaybackStatus status{};
    if (!maiFfplayAudioStatus(runtime_->session, &status)) return;
    if (status.duration_us > 0) setDuration(status.duration_us);
    if (!alive) return;
    setPosition(status.position_us);
    if (alive) setState(status.paused ? State::Paused : State::Playing);
}

void DesktopAudioPlayback::stopAll() {
    auto& owner = playbackOwner();
    QScopedValueRollback<bool> handoff(owner.handoff, true);
    const QPointer<DesktopAudioPlayback> pending = owner.pending;
    const QPointer<DesktopAudioPlayback> active = owner.active;
    owner.pending.clear();
    ++owner.nextGeneration;
    if (pending) pending->stop();
    if (active && active != pending) active->stop();
}

void DesktopAudioPlayback::prepareVideoPlayback() {
    stopAll();
}

void DesktopAudioPlayback::setVideoStopHandler(std::function<void()> handler) {
    playbackOwner().closeVideo = std::move(handler);
}
