// 文件作用：用原生接口替身验证 Qt 相机桥、帧合并与新照片生命周期；不打开真实设备。
#include "ui/DesktopCameraSession.h"

#include "DesktopCameraNativeFake.h"

#include <QBuffer>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <algorithm>
#include <memory>

namespace Native = DesktopCameraNativeFake;

static MaiCameraCapture::Frame frame(quint64 sequence, unsigned char red = 71) {
    MaiCameraCapture::Frame result;
    result.width = result.height = 2;
    result.stride = 12;  // 行尾填充不能变成下一行像素。
    result.sequence = sequence;
    result.pixels.assign(result.stride * result.height, 0xEE);
    for (unsigned row = 0; row < result.height; ++row) {
        for (unsigned column = 0; column < result.width; ++column) {
            const auto offset = row * result.stride + column * 4;
            result.pixels[offset] = 19;
            result.pixels[offset + 1] = 43;
            result.pixels[offset + 2] = red;
            result.pixels[offset + 3] = 255;
        }
    }
    return result;
}

static void firstFrame(const Native::Handle& handle) {
    Native::deliverState(handle, MaiCameraCapture::State::Running);
    Native::deliverFrame(handle, frame(1));
}

static MaiCameraCapture::Photo photo(quint64 request, const QByteArray& jpeg) {
    MaiCameraCapture::Photo result;
    result.requestId = request;
    result.jpeg.assign(jpeg.begin(), jpeg.end());
    return result;
}

static QStringList files(const QString& directory) {
    return QDir(directory).entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
}

class DesktopCameraSessionTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void init() {
        Native::reset();
    }
    void startWaitsForFirstValidFrameAndShutter();
    void frameBurstKeepsOnlyLatestOwningImage();
    void nativeJpegIsSavedExactlyAndClaimSurvivesStop();
    void unclaimedPhotoIsRemovedWithoutTouchingOtherFiles();
    void nativePhotoFailureAllowsRetry();
    void fileWriteFailureDoesNotReportSuccess();
    void stopDropsQueuedPhotoBeforeAnyFileWrite();
    void stopAfterWorkerWriteBeforeQtDeliveryRemovesNewFile();
    void previousGenerationCannotAffectRestart();
    void terminalErrorInvalidatesPreviewAndPendingPhoto();
    void rejectedNativeStartReportsError();
    void destructionCleansUnclaimedPhotoAndRejectsLateCallbacks();
    void photoCanBeClaimedAndStoppedInsideReadySignal();

private:
    QByteArray jpeg_;
};

void DesktopCameraSessionTest::initTestCase() {
    // 仅测试素材在 Qt 编码；生产照片必须逐字节保存原生 JPEG，不从预览帧重编码。
    QImage source(3, 2, QImage::Format_RGB32);
    source.fill(QColor(83, 29, 137));
    QBuffer buffer(&jpeg_);
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY2(source.save(&buffer, "JPEG"), "Qt test fixture JPEG encoder is required");
    QVERIFY(!jpeg_.isEmpty());
}

void DesktopCameraSessionTest::startWaitsForFirstValidFrameAndShutter() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    DesktopCameraSession session;
    QSignalSpy frames(&session, &DesktopCameraSession::frameReady);
    QThread* deliveredOn = nullptr;
    connect(&session, &DesktopCameraSession::frameReady, &session, [&] { deliveredOn = QThread::currentThread(); });
    QVERIFY(session.start(directory.path()));
    QCOMPARE(Native::startCount(), 1);
    QCOMPARE(session.state(), DesktopCameraSession::State::Starting);
    QVERIFY(!session.isReady());
    QCOMPARE(session.capturePhoto(), quint64(0));
    QCOMPARE(Native::captureCount(), 0);
    const auto handle = Native::latest();
    Native::deliverState(handle, MaiCameraCapture::State::Running);
    auto invalid = frame(1);
    invalid.stride = 7;
    Native::deliverFrame(handle, invalid);
    QTest::qWait(20);
    QVERIFY(!session.isReady());
    QCOMPARE(frames.size(), 0);
    auto valid = frame(2);
    Native::deliverFrame(handle, valid);
    std::fill(valid.pixels.begin(), valid.pixels.end(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(session.isReady(), 1000);
    QCOMPARE(session.state(), DesktopCameraSession::State::Running);
    QCOMPARE(frames.size(), 1);
    const QImage image = qvariant_cast<QImage>(frames.front().at(0));
    QCOMPARE(image.size(), QSize(2, 2));
    QCOMPARE(image.pixelColor(1, 1), QColor(71, 43, 19));
    QCOMPARE(frames.front().at(1).toULongLong(), quint64(2));
    QCOMPARE(deliveredOn, QThread::currentThread());
    const quint64 request = session.capturePhoto();
    QVERIFY(request != 0);
    QVERIFY(session.photoPending());
    QCOMPARE(Native::pendingRequestId(), request);
    QCOMPARE(session.capturePhoto(), quint64(0));
    QCOMPARE(Native::captureCount(), 1);
}

void DesktopCameraSessionTest::frameBurstKeepsOnlyLatestOwningImage() {
    QTemporaryDir directory;
    DesktopCameraSession session;
    QSignalSpy frames(&session, &DesktopCameraSession::frameReady);
    QVERIFY(session.start(directory.path()));
    const auto handle = Native::latest();
    Native::deliverState(handle, MaiCameraCapture::State::Running);
    std::vector<MaiCameraCapture::Frame> burst;
    for (unsigned i = 1; i <= 100; ++i) burst.push_back(frame(i, static_cast<unsigned char>(i)));
    Native::deliverFrames(handle, burst);
    burst.clear();
    QCOMPARE(frames.size(), 0);  // UI 尚未处理事件，原生线程不能发 UI 信号。
    QTRY_COMPARE_WITH_TIMEOUT(frames.size(), 1, 1000);
    QCOMPARE(frames.front().at(1).toULongLong(), quint64(100));
    QCOMPARE(qvariant_cast<QImage>(frames.front().at(0)).pixelColor(1, 1), QColor(100, 43, 19));
    QTest::qWait(40);
    QCOMPARE(frames.size(), 1);
}

void DesktopCameraSessionTest::nativeJpegIsSavedExactlyAndClaimSurvivesStop() {
    QTemporaryDir directory;
    DesktopCameraSession session;
    QSignalSpy ready(&session, &DesktopCameraSession::photoReady);
    QVERIFY(session.start(directory.path()));
    const auto handle = Native::latest();
    firstFrame(handle);
    QTRY_VERIFY_WITH_TIMEOUT(session.isReady(), 1000);
    const quint64 request = session.capturePhoto();
    QVERIFY(request != 0);
    Native::deliverPhoto(handle, photo(request, jpeg_));
    QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 3000);
    QCOMPARE(ready.front().front().toULongLong(), request);
    QVERIFY(session.photoPending());
    QVERIFY(session.takePhoto(request + 1).isEmpty());
    const QString path = session.takePhoto(request);
    QVERIFY(QFileInfo(path).isAbsolute());
    // 桥在 start 时规范化保存目录；macOS 的 /var 与 /private/var 可指向同一目录。
    QCOMPARE(QFileInfo(path).dir().canonicalPath(), QDir(directory.path()).canonicalPath());
    QCOMPARE(QFileInfo(path).suffix(), QStringLiteral("jpg"));
    QFile saved(path);
    QVERIFY(saved.open(QIODevice::ReadOnly));
    QCOMPARE(saved.readAll(), jpeg_);
    saved.close();
    QVERIFY(!session.photoPending());
    QVERIFY(session.takePhoto(request).isEmpty());
    session.stop();
    QVERIFY(QFileInfo::exists(path));
    QCOMPARE(files(directory.path()).size(), 1);
}

void DesktopCameraSessionTest::unclaimedPhotoIsRemovedWithoutTouchingOtherFiles() {
    QTemporaryDir directory;
    QFile sentinel(directory.filePath(QStringLiteral("keep.jpg")));
    QVERIFY(sentinel.open(QIODevice::WriteOnly));
    QCOMPARE(sentinel.write("owned-before-capture"), qint64(20));
    sentinel.close();
    DesktopCameraSession session;
    QSignalSpy ready(&session, &DesktopCameraSession::photoReady);
    QVERIFY(session.start(directory.path()));
    const auto handle = Native::latest();
    firstFrame(handle);
    QTRY_VERIFY_WITH_TIMEOUT(session.isReady(), 1000);
    const quint64 request = session.capturePhoto();
    QVERIFY(request != 0);
    Native::deliverPhoto(handle, photo(request, jpeg_));
    QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 3000);
    QCOMPARE(files(directory.path()).size(), 2);
    session.stop();
    QTRY_COMPARE_WITH_TIMEOUT(files(directory.path()), QStringList{QStringLiteral("keep.jpg")}, 3000);
    QVERIFY(session.takePhoto(request).isEmpty());
    QVERIFY(sentinel.open(QIODevice::ReadOnly));
    QCOMPARE(sentinel.readAll(), QByteArray("owned-before-capture"));
}

void DesktopCameraSessionTest::nativePhotoFailureAllowsRetry() {
    QTemporaryDir directory;
    DesktopCameraSession session;
    QSignalSpy ready(&session, &DesktopCameraSession::photoReady);
    QSignalSpy failed(&session, &DesktopCameraSession::photoFailed);
    QVERIFY(session.start(directory.path()));
    const auto handle = Native::latest();
    firstFrame(handle);
    QTRY_VERIFY_WITH_TIMEOUT(session.isReady(), 1000);
    const quint64 request = session.capturePhoto();
    QVERIFY(request != 0);
    Native::deliverPhoto(handle, {request, {}, "Native shutter failed"});
    QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 1000);
    QCOMPARE(failed.front().at(0).toULongLong(), request);
    QCOMPARE(failed.front().at(1).toString(), QStringLiteral("Native shutter failed"));
    QCOMPARE(ready.size(), 0);
    QVERIFY(files(directory.path()).isEmpty());
    QVERIFY(!session.photoPending());
    QVERIFY(session.isReady());
    QVERIFY(session.capturePhoto() != 0);
}

void DesktopCameraSessionTest::fileWriteFailureDoesNotReportSuccess() {
    QTemporaryDir directory;
    const QString destination = directory.filePath(QStringLiteral("destination"));
    QVERIFY(QDir().mkdir(destination));
    DesktopCameraSession session;
    QSignalSpy ready(&session, &DesktopCameraSession::photoReady);
    QSignalSpy failed(&session, &DesktopCameraSession::photoFailed);
    QVERIFY(session.start(destination));
    const auto handle = Native::latest();
    firstFrame(handle);
    QTRY_VERIFY_WITH_TIMEOUT(session.isReady(), 1000);
    const quint64 request = session.capturePhoto();
    QVERIFY(request != 0);
    QVERIFY(QDir().rmdir(destination));  // 只移除测试自建的空目录，不修改权限或用户目录。
    Native::deliverPhoto(handle, photo(request, jpeg_));
    QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 3000);
    QVERIFY(!failed.front().at(1).toString().isEmpty());
    QCOMPARE(ready.size(), 0);
    QVERIFY(!session.photoPending());
    QVERIFY(session.takePhoto(request).isEmpty());
    QVERIFY(files(directory.path()).isEmpty());
}

void DesktopCameraSessionTest::stopDropsQueuedPhotoBeforeAnyFileWrite() {
    QTemporaryDir directory;
    DesktopCameraSession session;
    QSignalSpy ready(&session, &DesktopCameraSession::photoReady);
    QVERIFY(session.start(directory.path()));
    const auto handle = Native::latest();
    firstFrame(handle);
    QTRY_VERIFY_WITH_TIMEOUT(session.isReady(), 1000);
    const quint64 request = session.capturePhoto();
    QVERIFY(request != 0);
    Native::deliverPhoto(handle, photo(request, jpeg_));
    session.stop();  // 原生已返回，Qt 尚未处理它排入的照片结果。
    session.stop();
    QCOMPARE(session.state(), DesktopCameraSession::State::Stopped);
    QVERIFY(!session.isReady());
    QVERIFY(!session.photoPending());
    QVERIFY(Native::stopCount() >= 1);
    QTest::qWait(100);
    QCOMPARE(ready.size(), 0);
    QVERIFY(files(directory.path()).isEmpty());
}

void DesktopCameraSessionTest::stopAfterWorkerWriteBeforeQtDeliveryRemovesNewFile() {
    QTemporaryDir directory;
    DesktopCameraSession session;
    QSignalSpy ready(&session, &DesktopCameraSession::photoReady);
    QVERIFY(session.start(directory.path()));
    const auto handle = Native::latest();
    firstFrame(handle);
    QTRY_VERIFY_WITH_TIMEOUT(session.isReady(), 1000);
    const quint64 request = session.capturePhoto();
    QVERIFY(request != 0);
    Native::deliverPhoto(handle, photo(request, jpeg_));
    // 只交付原生结果这一轮，让桥启动保存线程；其后不处理 Qt 回执。
    QCoreApplication::sendPostedEvents(&session, QEvent::MetaCall);
    QElapsedTimer deadline;
    deadline.start();
    while (deadline.elapsed() < 3000 &&
           QDir(directory.path()).entryList({QStringLiteral("*.jpg")}, QDir::Files).isEmpty())
        QThread::msleep(2);
    QCOMPARE(QDir(directory.path()).entryList({QStringLiteral("*.jpg")}, QDir::Files).size(), 1);
    QCOMPARE(ready.size(), 0);  // UI 阻塞时仍完成原生 JPEG 写入。
    session.stop();
    QVERIFY(files(directory.path()).isEmpty());
    QTest::qWait(30);
    QCOMPARE(ready.size(), 0);
    QVERIFY(session.takePhoto(request).isEmpty());
}

void DesktopCameraSessionTest::previousGenerationCannotAffectRestart() {
    QTemporaryDir directory;
    DesktopCameraSession session;
    QSignalSpy ready(&session, &DesktopCameraSession::photoReady);
    QSignalSpy frames(&session, &DesktopCameraSession::frameReady);
    QVERIFY(session.start(directory.path()));
    const auto old = Native::latest();
    firstFrame(old);
    QTRY_VERIFY_WITH_TIMEOUT(session.isReady(), 1000);
    const quint64 request = session.capturePhoto();
    QVERIFY(request != 0);
    session.stop();
    frames.clear();
    QVERIFY(session.start(directory.path()));
    const auto current = Native::latest();
    Native::deliverState(old, MaiCameraCapture::State::Running);
    Native::deliverFrame(old, frame(999));
    Native::deliverPhoto(old, photo(request, jpeg_));
    Native::deliverState(old, MaiCameraCapture::State::Error, "Old permission result");
    QTest::qWait(40);
    QCOMPARE(session.state(), DesktopCameraSession::State::Starting);
    QVERIFY(!session.isReady());
    QCOMPARE(ready.size(), 0);
    QCOMPARE(frames.size(), 0);
    QVERIFY(files(directory.path()).isEmpty());
    firstFrame(current);
    QTRY_VERIFY_WITH_TIMEOUT(session.isReady(), 1000);
    QCOMPARE(Native::startCount(), 2);
}

void DesktopCameraSessionTest::terminalErrorInvalidatesPreviewAndPendingPhoto() {
    QTemporaryDir directory;
    DesktopCameraSession session;
    QSignalSpy ready(&session, &DesktopCameraSession::photoReady);
    QSignalSpy frames(&session, &DesktopCameraSession::frameReady);
    QVERIFY(session.start(directory.path()));
    const auto handle = Native::latest();
    firstFrame(handle);
    QTRY_VERIFY_WITH_TIMEOUT(session.isReady(), 1000);
    const quint64 request = session.capturePhoto();
    QVERIFY(request != 0);
    Native::deliverState(handle, MaiCameraCapture::State::Error, "Camera disconnected");
    QTRY_COMPARE_WITH_TIMEOUT(session.state(), DesktopCameraSession::State::Error, 1000);
    QCOMPARE(session.errorString(), QStringLiteral("Camera disconnected"));
    QVERIFY(!session.isReady());
    QVERIFY(!session.photoPending());
    frames.clear();
    Native::deliverFrame(handle, frame(2));
    Native::deliverPhoto(handle, photo(request, jpeg_));
    QTest::qWait(100);
    QCOMPARE(session.state(), DesktopCameraSession::State::Error);
    QCOMPARE(frames.size(), 0);
    QCOMPARE(ready.size(), 0);
    QVERIFY(files(directory.path()).isEmpty());
}

void DesktopCameraSessionTest::rejectedNativeStartReportsError() {
    QTemporaryDir directory;
    Native::setStartAccepted(false);
    DesktopCameraSession session;
    QVERIFY(!session.start(directory.path()));
    QCOMPARE(session.state(), DesktopCameraSession::State::Error);
    QVERIFY(!session.errorString().isEmpty());
    QVERIFY(!session.isReady());
    QCOMPARE(session.capturePhoto(), quint64(0));
    session.stop();
    Native::setStartAccepted(true);
    QVERIFY(session.start(directory.path()));
    firstFrame(Native::latest());
    QTRY_VERIFY_WITH_TIMEOUT(session.isReady(), 1000);
}

void DesktopCameraSessionTest::destructionCleansUnclaimedPhotoAndRejectsLateCallbacks() {
    QTemporaryDir directory;
    auto session = std::make_unique<DesktopCameraSession>();
    int delivered = 0;
    connect(session.get(), &DesktopCameraSession::photoReady, this, [&] { ++delivered; });
    QVERIFY(session->start(directory.path()));
    const auto handle = Native::latest();
    firstFrame(handle);
    QTRY_VERIFY_WITH_TIMEOUT(session->isReady(), 1000);
    const quint64 request = session->capturePhoto();
    QVERIFY(request != 0);
    Native::deliverPhoto(handle, photo(request, jpeg_));
    QTRY_COMPARE_WITH_TIMEOUT(delivered, 1, 3000);
    QCOMPARE(files(directory.path()).size(), 1);
    session.reset();
    Native::deliverFrame(handle, frame(2));
    Native::deliverPhoto(handle, photo(request, jpeg_));
    Native::deliverState(handle, MaiCameraCapture::State::Error, "Late result after destruction");
    QTest::qWait(100);
    QCOMPARE(delivered, 1);
    QVERIFY(files(directory.path()).isEmpty());
}

void DesktopCameraSessionTest::photoCanBeClaimedAndStoppedInsideReadySignal() {
    QTemporaryDir directory;
    DesktopCameraSession session;
    QString claimed;
    connect(&session, &DesktopCameraSession::photoReady, &session, [&](quint64 request) {
        claimed = session.takePhoto(request);
        session.stop();
    });
    QVERIFY(session.start(directory.path()));
    const auto handle = Native::latest();
    firstFrame(handle);
    QTRY_VERIFY_WITH_TIMEOUT(session.isReady(), 1000);
    const quint64 request = session.capturePhoto();
    QVERIFY(request != 0);
    Native::deliverPhoto(handle, photo(request, jpeg_));
    QTRY_VERIFY_WITH_TIMEOUT(!claimed.isEmpty(), 3000);
    QCOMPARE(session.state(), DesktopCameraSession::State::Stopped);
    QVERIFY(QFileInfo::exists(claimed));
}

QTEST_GUILESS_MAIN(DesktopCameraSessionTest)
#include "DesktopCameraSessionTest.moc"
