// 文件作用：通过相机私有替身验证用户快门、会话窗口归属及普通手动入口；不打开设备。
#include "ui/DesktopCameraDialog.h"

#include "DesktopCameraNativeFake.h"

#include <QApplication>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <memory>

namespace Native = DesktopCameraNativeFake;

static QDialog* visibleCamera(QWidget* owner) {
    for (auto* dialog : owner->findChildren<QDialog*>(QStringLiteral("desktopCameraDialog")))
        if (dialog->isVisible()) return dialog;
    return nullptr;
}

static void readyFrame(const Native::Handle& handle) {
    MaiCameraCapture::Frame frame;
    frame.width = frame.height = 2;
    frame.stride = 8;
    frame.sequence = 1;
    frame.pixels = {31, 43, 67, 255, 31, 43, 67, 255, 31, 43, 67, 255, 31, 43, 67, 255};
    Native::deliverState(handle, MaiCameraCapture::State::Running);
    Native::deliverFrame(handle, frame);
}

static MaiCameraCapture::Photo jpegPhoto(const QByteArray& bytes) {
    MaiCameraCapture::Photo result;
    result.requestId = Native::pendingRequestId();
    result.jpeg.assign(bytes.begin(), bytes.end());
    return result;
}

class DesktopCameraDialogTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void init() {
        Native::reset();
    }
    void previewWaitsForUserShutterAndRetainsClaimedPhoto();
    void sameSessionReusesWindowAndOtherSessionCannotTakeIt();
    void closeCancelsQueuedPhotoAndParentDestructionRejectsLateResult();
    void manualCaptureUsesSameNativeJpegPath();

private:
    QByteArray jpeg_;
};

void DesktopCameraDialogTest::initTestCase() {
    QImage image(3, 2, QImage::Format_RGB32);
    image.fill(QColor(77, 99, 121));
    QBuffer buffer(&jpeg_);
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(image.save(&buffer, "JPEG"));
}

void DesktopCameraDialogTest::previewWaitsForUserShutterAndRetainsClaimedPhoto() {
    QTemporaryDir directory;
    QWidget owner;
    QString error;
    QVERIFY(DesktopCameraDialog::openPreview(&owner, QStringLiteral("session-a"), directory.path(), &error));
    QVERIFY(error.isEmpty());
    auto* dialog = visibleCamera(&owner);
    QVERIFY(dialog);
    auto* shutter = dialog->findChild<QPushButton*>(QStringLiteral("desktopCameraShutter"));
    auto* preview = dialog->findChild<QLabel*>(QStringLiteral("desktopCameraPreview"));
    QVERIFY(shutter);
    QVERIFY(preview);
    QVERIFY(!shutter->isEnabled());
    QCOMPARE(Native::captureCount(), 0);
    const auto handle = Native::latest();
    readyFrame(handle);
    QTRY_VERIFY_WITH_TIMEOUT(shutter->isEnabled(), 1000);
    QVERIFY(!preview->pixmap().isNull());
    QCOMPARE(Native::captureCount(), 0);
    QTest::mouseClick(shutter, Qt::LeftButton);
    QCOMPARE(Native::captureCount(), 1);
    QVERIFY(!shutter->isEnabled());
    Native::deliverPhoto(handle, jpegPhoto(jpeg_));
    QTRY_VERIFY_WITH_TIMEOUT(!DesktopCameraDialog::lastPreviewPhoto(&owner, QStringLiteral("session-a")).isEmpty(),
                             3000);
    const QString path = DesktopCameraDialog::lastPreviewPhoto(&owner, QStringLiteral("session-a"));
    QFile saved(path);
    QVERIFY(saved.open(QIODevice::ReadOnly));
    QCOMPARE(saved.readAll(), jpeg_);
    saved.close();
    QVERIFY(!DesktopCameraDialog::isPreviewOpen(&owner, QStringLiteral("session-a")));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(QFileInfo::exists(path));
}

void DesktopCameraDialogTest::sameSessionReusesWindowAndOtherSessionCannotTakeIt() {
    QTemporaryDir directory;
    QWidget owner;
    QString error;
    QVERIFY(DesktopCameraDialog::openPreview(&owner, QStringLiteral("session-a"), directory.path(), &error));
    auto* first = visibleCamera(&owner);
    QVERIFY(first);
    const auto old = Native::latest();
    QVERIFY(DesktopCameraDialog::openPreview(&owner, QStringLiteral("session-a"), directory.path(), &error));
    QCOMPARE(visibleCamera(&owner), first);
    QCOMPARE(Native::startCount(), 1);
    QVERIFY(!DesktopCameraDialog::openPreview(&owner, QStringLiteral("session-b"), directory.path(), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!DesktopCameraDialog::closePreview(&owner, QStringLiteral("session-b")));
    QVERIFY(DesktopCameraDialog::isPreviewOpen(&owner, QStringLiteral("session-a")));
    QVERIFY(DesktopCameraDialog::closePreview(&owner, QStringLiteral("session-a")));
    QVERIFY(DesktopCameraDialog::openPreview(&owner, QStringLiteral("session-b"), directory.path(), &error));
    QVERIFY(!DesktopCameraDialog::isPreviewOpen(&owner, QStringLiteral("session-a")));
    QVERIFY(DesktopCameraDialog::lastPreviewPhoto(&owner, QStringLiteral("session-a")).isEmpty());
    auto* second = visibleCamera(&owner);
    QVERIFY(second);
    QVERIFY(second != first);
    auto* shutter = second->findChild<QPushButton*>(QStringLiteral("desktopCameraShutter"));
    QVERIFY(shutter);
    readyFrame(old);
    Native::deliverState(old, MaiCameraCapture::State::Error, "Old session error");
    QTest::qWait(30);
    QVERIFY(!shutter->isEnabled());
    QVERIFY(DesktopCameraDialog::previewError(&owner, QStringLiteral("session-b")).isEmpty());
    readyFrame(Native::latest());
    QTRY_VERIFY_WITH_TIMEOUT(shutter->isEnabled(), 1000);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(DesktopCameraDialog::isPreviewOpen(&owner, QStringLiteral("session-b")));
    QCOMPARE(Native::startCount(), 2);
    QVERIFY(DesktopCameraDialog::closePreview(&owner, QStringLiteral("session-b")));
}

void DesktopCameraDialogTest::closeCancelsQueuedPhotoAndParentDestructionRejectsLateResult() {
    QTemporaryDir directory;
    auto owner = std::make_unique<QWidget>();
    QString error;
    QVERIFY(DesktopCameraDialog::openPreview(owner.get(), QStringLiteral("session-a"), directory.path(), &error));
    const auto handle = Native::latest();
    readyFrame(handle);
    auto* shutter = visibleCamera(owner.get())->findChild<QPushButton*>(QStringLiteral("desktopCameraShutter"));
    QTRY_VERIFY_WITH_TIMEOUT(shutter->isEnabled(), 1000);
    QTest::mouseClick(shutter, Qt::LeftButton);
    const auto late = jpegPhoto(jpeg_);
    Native::deliverPhoto(handle, late);
    QVERIFY(DesktopCameraDialog::closePreview(owner.get(), QStringLiteral("session-a")));
    owner.reset();
    Native::deliverPhoto(handle, late);
    readyFrame(handle);
    QTest::qWait(100);
    QVERIFY(QDir(directory.path()).entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty());
    QVERIFY(Native::stopCount() >= 1);
}

void DesktopCameraDialogTest::manualCaptureUsesSameNativeJpegPath() {
    QTemporaryDir directory;
    QWidget owner;
    QString error;
    QTimer driver;
    driver.setInterval(10);
    int stage = 0;
    connect(&driver, &QTimer::timeout, &owner, [&] {
        auto* dialog = visibleCamera(&owner);
        if (!dialog) return;
        auto* shutter = dialog->findChild<QPushButton*>(QStringLiteral("desktopCameraShutter"));
        if (!shutter) return;
        if (stage == 0) {
            readyFrame(Native::latest());
            stage = 1;
        } else if (stage == 1 && shutter->isEnabled()) {
            QTest::mouseClick(shutter, Qt::LeftButton);
            Native::deliverPhoto(Native::latest(), jpegPhoto(jpeg_));
            stage = 2;
        }
    });
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &owner, [&] {
        if (auto* dialog = visibleCamera(&owner)) dialog->reject();
    });
    driver.start();
    timeout.start(3000);
    const QString path = DesktopCameraDialog::capturePhoto(&owner, directory.path(), &error);
    driver.stop();
    timeout.stop();
    QVERIFY2(!path.isEmpty(), qPrintable(error));
    QVERIFY(error.isEmpty());
    QCOMPARE(stage, 2);
    QCOMPARE(Native::captureCount(), 1);
    QFile saved(path);
    QVERIFY(saved.open(QIODevice::ReadOnly));
    QCOMPARE(saved.readAll(), jpeg_);
}

QTEST_MAIN(DesktopCameraDialogTest)
#include "DesktopCameraDialogTest.moc"
