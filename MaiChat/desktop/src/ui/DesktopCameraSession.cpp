// 文件作用：原生相机到 Qt UI 的有界交付桥；原生 JPEG 的写入与撤销均在普通工作线程完成。
#include "ui/DesktopCameraSession.h"

#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QUuid>

#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

#include "MaiFileSystem.h"

namespace {
struct CameraDispatch {
    std::mutex mutex;
    DesktopCameraSession* receiver = nullptr;
    quint64 generation = 0;
    bool active = false;
    bool terminal = false;
    bool frameQueued = false;
    MaiCameraCapture::Frame latest;
};

bool accepts(const CameraDispatch& dispatch, quint64 generation, bool terminal = false) {
    return dispatch.receiver && dispatch.active && dispatch.generation == generation &&
           (terminal || !dispatch.terminal);
}

// 调用方持锁；与析构的失效操作互斥，invokeMethod 不会使用已销毁的 QObject。
template <typename Task>
bool postLocked(const std::shared_ptr<CameraDispatch>& dispatch, quint64 generation, Task task, bool terminal = false) {
    if (!accepts(*dispatch, generation, terminal)) return false;
    return QMetaObject::invokeMethod(
        dispatch->receiver,
        [dispatch, generation, task = std::move(task), terminal] {
            DesktopCameraSession* receiver;
            {
                std::lock_guard<std::mutex> lock(dispatch->mutex);
                if (!accepts(*dispatch, generation, terminal)) return;
                receiver = dispatch->receiver;
            }
            task(receiver);
        },
        Qt::QueuedConnection);
}

template <typename Task>
bool post(const std::shared_ptr<CameraDispatch>& dispatch, quint64 generation, Task task) {
    std::lock_guard<std::mutex> lock(dispatch->mutex);
    return postLocked(dispatch, generation, std::move(task));
}

bool current(const std::shared_ptr<CameraDispatch>& dispatch, quint64 generation) {
    std::lock_guard<std::mutex> lock(dispatch->mutex);
    return accepts(*dispatch, generation);
}

bool validFrame(const MaiCameraCapture::Frame& frame) {
    return frame.sequence && frame.width && frame.height &&
           frame.width <= static_cast<unsigned>(std::numeric_limits<int>::max() / 4) &&
           frame.height <= static_cast<unsigned>(std::numeric_limits<int>::max()) &&
           frame.stride >= static_cast<std::uint64_t>(frame.width) * 4 &&
           frame.stride <= static_cast<unsigned>(std::numeric_limits<int>::max()) &&
           static_cast<std::uint64_t>(frame.stride) * frame.height <= frame.pixels.size();
}

MaiFilePath nativePath(const QString& path) {
#ifdef _WIN32
    return MaiFilePath(path.toStdWString());
#else
    return MaiFilePath::fromUtf8(path.toUtf8().toStdString());
#endif
}

QString uiPath(const MaiFilePath& path) {
#ifdef _WIN32
    return QString::fromStdWString(path.value());
#else
    const auto utf8 = path.toUtf8();
    return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
#endif
}

MaiFilePath stagingPath(const MaiFilePath& path) {
#ifdef _WIN32
    return MaiFilePath(path.value() + L".part");
#else
    return MaiFilePath(path.value() + ".part");
#endif
}

struct PhotoJob {
    std::mutex mutex;
    std::condition_variable changed;
    bool canceled = false;
    bool claimed = false;
    bool ready = false;
    std::string error;
    MaiFilePath destination;
    std::vector<std::uint8_t> jpeg;
};

struct NewCameraFile {
    MaiFilePath path;
    bool owned = false;
    ~NewCameraFile() {
        remove();
    }
    void remove() {
        if (owned) MaiFileSystem::removeFile(path);
        owned = false;
    }
};

bool canceled(const std::shared_ptr<PhotoJob>& job) {
    std::lock_guard<std::mutex> lock(job->mutex);
    return job->canceled;
}

// 只操作本次独占创建的两个路径；publishNewFile 拒绝覆盖任何已有文件。
template <typename Completion>
void savePhoto(const std::shared_ptr<PhotoJob>& job, const std::shared_ptr<CameraDispatch>& dispatch,
               quint64 generation, Completion completion) {
    NewCameraFile staging{stagingPath(job->destination)};
    NewCameraFile destination{job->destination};
    std::string error;
    try {
        if (canceled(job)) return;
        auto result = MaiFileSystem::createEmptyFile(staging.path);
        if (result) {
            error = result.message();
        } else {
            staging.owned = true;
            if (canceled(job)) return;
            const std::string bytes(reinterpret_cast<const char*>(job->jpeg.data()), job->jpeg.size());
            result = MaiFileSystem::writeFile(staging.path, bytes);
            if (result) {
                error = result.message();
            } else {
                if (canceled(job)) return;
                result = MaiFileSystem::publishNewFile(staging.path, destination.path);
                if (result) {
                    error = result.message();
                } else {
                    staging.owned = false;
                    destination.owned = true;
                }
            }
        }
    } catch (...) {
        error = "Unable to save the native camera photo";
    }
    staging.remove();
    if (!destination.owned && error.empty()) error = "Unable to save the native camera photo";
    {
        std::lock_guard<std::mutex> lock(job->mutex);
        if (job->canceled) return;
        job->error = std::move(error);
        job->ready = true;
    }
    if (!post(dispatch, generation, std::move(completion))) return;
    if (!destination.owned) return;

    // 文件仍属于 worker；UI 显式领取才移交所有权。stop 唤醒并等待删除完成。
    std::unique_lock<std::mutex> lock(job->mutex);
    job->changed.wait(lock, [&] { return job->claimed || job->canceled; });
    if (job->claimed) destination.owned = false;
}
}  // namespace

struct DesktopCameraSession::Runtime {
    std::shared_ptr<CameraDispatch> dispatch = std::make_shared<CameraDispatch>();
    std::unique_ptr<MaiCameraCapture> camera;
    std::shared_ptr<PhotoJob> job;
    std::thread saver;
    MaiFilePath directory;
    State state = State::Stopped;
    bool ready = false;
    quint64 nextRequest = 0;
    quint64 pending = 0;
    QString error;
};

DesktopCameraSession::DesktopCameraSession(QObject* parent) : QObject(parent), runtime_(std::make_unique<Runtime>()) {
    runtime_->dispatch->receiver = this;
}

DesktopCameraSession::~DesktopCameraSession() {
    release();
    std::lock_guard<std::mutex> lock(runtime_->dispatch->mutex);
    runtime_->dispatch->receiver = nullptr;
}

bool DesktopCameraSession::start(const QString& directory) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (runtime_->state == State::Starting || runtime_->state == State::Running) return false;
    release();
    runtime_->error.clear();
    const QFileInfo info(directory);
    const QString canonical = info.canonicalFilePath();
    if (!info.isDir() || !info.isWritable() || canonical.isEmpty()) {
        nativeError(QStringLiteral("当前工作目录不可写，无法保存照片"));
        return false;
    }
    if (!MaiCameraCapture::isAvailable()) {
        nativeError(QStringLiteral("没有可用的摄像头"));
        return false;
    }
    runtime_->directory = nativePath(canonical);
    const auto dispatch = runtime_->dispatch;
    quint64 generation;
    {
        std::lock_guard<std::mutex> lock(dispatch->mutex);
        generation = ++dispatch->generation;
        dispatch->active = true;
        dispatch->terminal = false;
    }
    const auto reportError = [dispatch, generation](const std::string& error) {
        std::lock_guard<std::mutex> lock(dispatch->mutex);
        if (!accepts(*dispatch, generation)) return;
        dispatch->terminal = true;
        dispatch->latest = {};
        const QString message = error.empty() ? QStringLiteral("摄像头无法打开，请检查系统权限")
                                              : QString::fromUtf8(error.data(), static_cast<qsizetype>(error.size()));
        postLocked(dispatch, generation, [message](DesktopCameraSession* owner) { owner->nativeError(message); }, true);
    };
    MaiCameraCapture::Callbacks callbacks;
    callbacks.state = [dispatch, generation, reportError](MaiCameraCapture::State state, const std::string& error) {
        if (state == MaiCameraCapture::State::Error)
            reportError(error);
        else if (state == MaiCameraCapture::State::Stopped)
            post(dispatch, generation, [](DesktopCameraSession* owner) { owner->stop(); });
        // 原生的受理或 Running 状态不代替首个有效帧。
    };
    callbacks.frame = [dispatch, generation, reportError](const MaiCameraCapture::Frame& frame) {
        if (!validFrame(frame)) return;
        try {
            std::lock_guard<std::mutex> lock(dispatch->mutex);
            if (!accepts(*dispatch, generation)) return;
            dispatch->latest = frame;
            if (dispatch->frameQueued) return;
            dispatch->frameQueued = true;
            if (!postLocked(dispatch, generation,
                            [generation](DesktopCameraSession* owner) { owner->displayFrame(generation); }))
                dispatch->frameQueued = false;
        } catch (...) {
            reportError("Unable to copy the native camera preview frame");
        }
    };
    callbacks.photo = [dispatch, generation, reportError](const MaiCameraCapture::Photo& photo) {
        try {
            post(dispatch, generation,
                 [generation, photo](DesktopCameraSession* owner) { owner->receivePhoto(generation, photo); });
        } catch (...) {
            reportError("Unable to copy the native camera photo");
        }
    };
    runtime_->camera = std::make_unique<MaiCameraCapture>(std::move(callbacks));
    runtime_->state = State::Starting;
    if (!runtime_->camera->start()) {
        nativeError(QStringLiteral("摄像头无法打开，请检查系统权限或设备占用"));
        return false;
    }
    emit stateChanged(State::Starting);
    return true;
}

void DesktopCameraSession::release() {
    const auto dispatch = runtime_->dispatch;
    {
        std::lock_guard<std::mutex> lock(dispatch->mutex);
        ++dispatch->generation;
        dispatch->active = false;
        dispatch->terminal = true;
        dispatch->latest = {};
        dispatch->frameQueued = false;
    }
    if (runtime_->job) {
        {
            std::lock_guard<std::mutex> lock(runtime_->job->mutex);
            runtime_->job->canceled = true;
        }
        runtime_->job->changed.notify_all();
    }
    if (runtime_->camera) runtime_->camera->stop();
    runtime_->camera.reset();
    if (runtime_->saver.joinable()) runtime_->saver.join();
    runtime_->job.reset();
    runtime_->pending = 0;
    runtime_->ready = false;
}

void DesktopCameraSession::stop() {
    Q_ASSERT(QThread::currentThread() == thread());
    release();
    if (runtime_->state == State::Stopped) return;
    runtime_->state = State::Stopped;
    emit stateChanged(State::Stopped);
}

void DesktopCameraSession::nativeError(const QString& error) {
    release();
    runtime_->state = State::Error;
    runtime_->error = error;
    QPointer<DesktopCameraSession> alive(this);
    const auto dispatch = runtime_->dispatch;
    quint64 generation;
    {
        std::lock_guard<std::mutex> lock(dispatch->mutex);
        generation = dispatch->generation;
    }
    emit stateChanged(State::Error);
    if (!alive) return;
    {
        std::lock_guard<std::mutex> lock(dispatch->mutex);
        if (dispatch->generation != generation) return;
    }
    emit errorOccurred(error);
}

void DesktopCameraSession::displayFrame(quint64 generation) {
    const auto dispatch = runtime_->dispatch;
    MaiCameraCapture::Frame frame;
    {
        std::lock_guard<std::mutex> lock(dispatch->mutex);
        if (!accepts(*dispatch, generation)) return;
        frame = std::move(dispatch->latest);
        dispatch->frameQueued = false;
    }
    if (!validFrame(frame)) return;
    const QImage image = QImage(frame.pixels.data(), static_cast<int>(frame.width), static_cast<int>(frame.height),
                                static_cast<int>(frame.stride), QImage::Format_ARGB32)
                             .copy();
    if (image.isNull()) {
        nativeError(QStringLiteral("摄像头预览帧无法显示"));
        return;
    }
    if (!current(dispatch, generation)) return;
    runtime_->ready = true;
    if (runtime_->state != State::Running) {
        runtime_->state = State::Running;
        QPointer<DesktopCameraSession> alive(this);
        emit stateChanged(State::Running);
        if (!alive || !current(dispatch, generation)) return;
    }
    emit frameReady(image, frame.sequence);
}

DesktopCameraSession::State DesktopCameraSession::state() const {
    return runtime_->state;
}

bool DesktopCameraSession::isReady() const {
    return runtime_->state == State::Running && runtime_->ready;
}

bool DesktopCameraSession::photoPending() const {
    return runtime_->pending != 0;
}

QString DesktopCameraSession::errorString() const {
    return runtime_->error;
}

quint64 DesktopCameraSession::capturePhoto() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!isReady() || runtime_->pending || !runtime_->camera) return 0;
    if (!++runtime_->nextRequest) ++runtime_->nextRequest;
    const quint64 request = runtime_->nextRequest;
    runtime_->pending = request;
    runtime_->error.clear();
    if (runtime_->camera->capturePhoto(request)) return request;
    runtime_->pending = 0;
    runtime_->error = QStringLiteral("摄像头尚未准备好拍照，请重试");
    return 0;
}

void DesktopCameraSession::receivePhoto(quint64 generation, const MaiCameraCapture::Photo& photo) {
    if (!current(runtime_->dispatch, generation) || !photo.requestId || photo.requestId != runtime_->pending ||
        runtime_->job)
        return;
    if (!photo.error.empty() || photo.jpeg.empty()) {
        runtime_->pending = 0;
        const QString error = photo.error.empty()
                                  ? QStringLiteral("原生相机未返回照片，请重试")
                                  : QString::fromUtf8(photo.error.data(), static_cast<qsizetype>(photo.error.size()));
        runtime_->error = error;
        emit photoFailed(photo.requestId, error);
        return;
    }
    const auto job = std::make_shared<PhotoJob>();
    const std::string name = "camera-" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString() + ".jpg";
    job->destination = runtime_->directory.append(MaiFilePath::fromUtf8(name));
    job->jpeg = photo.jpeg;
    runtime_->job = job;
    try {
        runtime_->saver = std::thread([job, dispatch = runtime_->dispatch, generation, request = photo.requestId] {
            savePhoto(job, dispatch, generation,
                      [generation, request](DesktopCameraSession* owner) { owner->finishPhoto(generation, request); });
        });
    } catch (...) {
        runtime_->job.reset();
        runtime_->pending = 0;
        const QString error = QStringLiteral("照片保存线程无法启动，请重试");
        runtime_->error = error;
        emit photoFailed(photo.requestId, error);
    }
}

void DesktopCameraSession::finishPhoto(quint64 generation, quint64 requestId) {
    if (!current(runtime_->dispatch, generation) || requestId != runtime_->pending || !runtime_->job) return;
    std::string error;
    {
        std::lock_guard<std::mutex> lock(runtime_->job->mutex);
        if (!runtime_->job->ready || runtime_->job->canceled) return;
        error = runtime_->job->error;
    }
    if (error.empty()) {
        emit photoReady(requestId);
        return;
    }
    if (runtime_->saver.joinable()) runtime_->saver.join();
    runtime_->job.reset();
    runtime_->pending = 0;
    const QString message = QString::fromUtf8(error.data(), static_cast<qsizetype>(error.size()));
    runtime_->error = message;
    emit photoFailed(requestId, message);
}

QString DesktopCameraSession::takePhoto(quint64 requestId) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!requestId || requestId != runtime_->pending || !runtime_->job) return {};
    QString path;
    {
        std::lock_guard<std::mutex> lock(runtime_->job->mutex);
        if (!runtime_->job->ready || runtime_->job->canceled || runtime_->job->claimed || !runtime_->job->error.empty())
            return {};
        path = uiPath(runtime_->job->destination);
        runtime_->job->claimed = true;
    }
    runtime_->job->changed.notify_all();
    if (runtime_->saver.joinable()) runtime_->saver.join();
    runtime_->job.reset();
    runtime_->pending = 0;
    return path;
}
