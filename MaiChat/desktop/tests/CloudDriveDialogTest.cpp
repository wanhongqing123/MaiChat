#include "ui/CloudDriveDialog.h"
#include "MaiCloudVod.h"
#include "ui/DesktopCloudDriveThumbnail.h"
#include "ui/ImagePreviewDialog.h"
#include "ui/MainWindow.h"
#include "ui/MaiFfplayVideoDialog.h"
#include "app/RemoteIMApplication.h"
#include "im/FakeRemoteIMClient.h"

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QFutureWatcher>
#include <QImage>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QFrame>
#include <QPushButton>
#include <QProgressBar>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QThread>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <cstdlib>
#include <iostream>

namespace {

void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

QPushButton* visibleButton(QWidget& parent, const QString& name) {
    for (QPushButton* button : parent.findChildren<QPushButton*>(name))
        if (button->isVisible() && button->isEnabled()) return button;
    return nullptr;
}

bool waitForButton(QWidget& parent, const QString& name, int timeoutMs = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QApplication::processEvents();
        if (visibleButton(parent, name)) return true;
        QThread::msleep(20);
    }
    return false;
}

bool waitForLabel(QWidget& parent, const QString& name, const QString& needle,
                  int timeoutMs = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QApplication::processEvents();
        QLabel* label = parent.findChild<QLabel*>(name);
        if (label && label->text().contains(needle)) return true;
        QThread::msleep(20);
    }
    return false;
}

bool waitForVisibleText(QWidget& parent, const QString& needle, int timeoutMs = 3000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QApplication::processEvents();
        for (QLabel* label : parent.findChildren<QLabel*>())
            if (label->isVisible() && label->text().contains(needle)) return true;
        QThread::msleep(20);
    }
    return false;
}

bool waitForThumbnail(QWidget& parent, int timeoutMs = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QApplication::processEvents();
        for (QLabel* label : parent.findChildren<QLabel*>(QStringLiteral("cloudDriveThumbnail")))
            if (label->property("thumbnailLoaded").toBool()) return true;
        QThread::msleep(20);
    }
    return false;
}

ImagePreviewDialog* waitForImagePreview(QWidget& parent, int timeoutMs = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QApplication::processEvents();
        auto* preview = parent.findChild<ImagePreviewDialog*>(
            QStringLiteral("cloudDriveImagePreview"));
        if (preview && preview->isVisible()) return preview;
        QThread::msleep(20);
    }
    return nullptr;
}

QFrame* folderRow(QWidget& parent, const QString& name) {
    for (QFrame* row : parent.findChildren<QFrame*>(QStringLiteral("cloudDriveFolderRow"))) {
        QPushButton* open = row->findChild<QPushButton*>(QStringLiteral("cloudDriveFolderOpen"));
        if (row->isVisible() && open && open->isEnabled() && open->text() == name) return row;
    }
    return nullptr;
}

bool waitForFolder(QWidget& parent, const QString& name) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < 15000) {
        QApplication::processEvents();
        if (folderRow(parent, name)) return true;
        QThread::msleep(20);
    }
    return false;
}

void nameFolder(QWidget& parent, QPushButton* action, const QString& name) {
    require(action != nullptr, "folder name action is unavailable");
    QTimer fill;
    fill.setInterval(20);
    QObject::connect(&fill, &QTimer::timeout, [&] {
        QWidget* modal = QApplication::activeModalWidget();
        if (!modal) return;
        auto* input = modal->findChild<QLineEdit*>(QStringLiteral("appTextInput"));
        auto* confirm = modal->findChild<QPushButton*>(QStringLiteral("appTextInputConfirm"));
        if (!input || !confirm) return;
        fill.stop();
        input->setText(name);
        confirm->click();
    });
    fill.start();
    action->click();
    if (!waitForFolder(parent, name)) {
        auto* state = parent.findChild<QLabel*>(QStringLiteral("cloudDriveStatus"));
        std::cerr << "folder '" << name.toStdString() << "' status: "
                  << (state ? state->text().toStdString() : "missing") << '\n';
        require(false, "created/renamed folder was not listed");
    }
}

// 以可见媒体行和真实双击事件验证入口，不依赖已移除的操作按钮。
QFrame* mediaRow(QWidget& parent, const QString& type) {
    for (auto* row : parent.findChildren<QFrame*>(QStringLiteral("cloudDriveMediaRow")))
        if (row->isVisible() && row->property("mediaType").toString() == type) return row;
    return nullptr;
}

bool waitForMedia(QWidget& parent, const QString& type, int timeoutMs = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QApplication::processEvents();
        if (mediaRow(parent, type)) return true;
        QThread::msleep(20);
    }
    return false;
}

void openMedia(QWidget& parent, const QString& type) {
    QFrame* row = mediaRow(parent, type);
    require(row != nullptr, "media row is unavailable");
    auto* thumbnail = row->findChild<QLabel*>(QStringLiteral("cloudDriveThumbnail"));
    require(thumbnail != nullptr, "media thumbnail is unavailable");
    QTest::mouseDClick(thumbnail, Qt::LeftButton);
}

QByteArray readOwnedBytes(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "could not read owned thumbnail fixture");
    return file.readAll();
}

void writeOwnedBytes(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
            "could not write owned thumbnail fixture");
}

void verifyPreparedCloudImage(const QString& directory) {
    QTemporaryDir owned(QDir(directory).filePath(QStringLiteral("cloud-helper-XXXXXX")));
    require(owned.isValid(), "could not create owned image helper fixture directory");
    const QString source = owned.path() + QStringLiteral("/cloud-thumbnail-source.jpg");
    const QString checkpoint = owned.path() + QStringLiteral("/prepared-image.maicloud.json");
    const QString jpeg = checkpoint + QStringLiteral(".thumbnail.jpg");
    const QString metadata = checkpoint + QStringLiteral(".thumbnail.meta");
    QImage original(960, 480, QImage::Format_RGB32);
    original.fill(QColor(36, 148, 202));
    require(original.save(source, "JPEG"), "could not create cloud thumbnail source");
    const QByteArray originalBytes = readOwnedBytes(source);
    auto prepared = prepareDesktopCloudDriveImage(source, checkpoint, QFileInfo(source).fileName());
    require(prepared.error.isEmpty() && prepared.image && prepared.thumbnailPath == jpeg,
            "ordinary cloud image helper did not prepare an owned JPEG");
    require(prepared.encodedOriginal == originalBytes,
            "thumbnail and original did not share an immutable image snapshot");
    require(QFileInfo(jpeg).isFile() && QFileInfo(metadata).isFile() && QFileInfo(jpeg).size() <= 2'000'000,
            "thumbnail was not atomically retained beside its checkpoint");
    QImage thumbnail(jpeg);
    require(!thumbnail.isNull() && thumbnail.width() == 320 && thumbnail.height() == 160,
            "prepared JPEG did not retain aspect ratio within the 320-pixel bound");
    const QByteArray jpegBytes = readOwnedBytes(jpeg);
    writeOwnedBytes(checkpoint, QJsonDocument(QJsonObject{
        {QStringLiteral("version"), 1},
        {QStringLiteral("video_id"), QStringLiteral("10000000-1111-2222-3333-444444444444")},
        {QStringLiteral("upload_id"), QStringLiteral("owned-original")},
        {QStringLiteral("file_size"), originalBytes.size()},
        {QStringLiteral("file_sha256"), QString::fromLatin1(
            QCryptographicHash::hash(originalBytes, QCryptographicHash::Sha256).toHex())},
        {QStringLiteral("source_id"), source},
        {QStringLiteral("name"), QFileInfo(source).fileName()},
        {QStringLiteral("content_type"), QStringLiteral("image/jpeg")},
        {QStringLiteral("room_key"), QString()},
        {QStringLiteral("part_size"), 8 * 1024 * 1024}, {QStringLiteral("parts_total"), 1},
        {QStringLiteral("completed"), false},
        {QStringLiteral("thumbnail"), QJsonObject{
            {QStringLiteral("upload_id"), QStringLiteral("owned-upload")},
            {QStringLiteral("size_bytes"), jpegBytes.size()},
            {QStringLiteral("file_sha256"), QString::fromLatin1(
                QCryptographicHash::hash(jpegBytes, QCryptographicHash::Sha256).toHex())},
            {QStringLiteral("completed"), false}}}}).toJson(QJsonDocument::Compact));
    require(finishDesktopCloudDriveImage(checkpoint, false) && QFileInfo(jpeg).isFile() &&
                QFileInfo(metadata).isFile(), "pause removed resumable cloud thumbnail data");
    auto resumed = prepareDesktopCloudDriveImage(source, checkpoint, QFileInfo(source).fileName());
    require(resumed.error.isEmpty() && resumed.thumbnailPath == jpeg && readOwnedBytes(jpeg) == jpegBytes,
            "resume regenerated or changed the retained thumbnail");
    resumed = {};
    require(QFile::remove(metadata), "could not remove owned thumbnail metadata fixture");
    QByteArray corruptedJpeg = jpegBytes;
    corruptedJpeg[corruptedJpeg.size() - 1] = char(corruptedJpeg.back() ^ 1);
    writeOwnedBytes(jpeg, corruptedJpeg);
    require(!prepareDesktopCloudDriveImage(source, checkpoint, QFileInfo(source).fileName()).error.isEmpty() &&
                !QFileInfo::exists(metadata),
            "corrupt retained JPEG was treated as recoverable missing metadata");
    writeOwnedBytes(jpeg, jpegBytes);
    auto recoveredMetadata = prepareDesktopCloudDriveImage(source, checkpoint, QFileInfo(source).fileName());
    require(recoveredMetadata.error.isEmpty() && recoveredMetadata.thumbnailPath == jpeg &&
                readOwnedBytes(jpeg) == jpegBytes && QFileInfo::exists(metadata),
            "verified retained JPEG without host metadata did not reconstruct its owned binding");
    recoveredMetadata = {};
    require(QFile::remove(jpeg), "could not remove owned thumbnail JPEG fixture");
    auto missingJpeg = prepareDesktopCloudDriveImage(source, checkpoint, QFileInfo(source).fileName());
    require(missingJpeg.error.isEmpty() && missingJpeg.thumbnailPath.isEmpty() &&
                QFileInfo::exists(metadata) && !QFileInfo::exists(jpeg),
            "missing JPEG with a matching receipt was regenerated or blocked authoritative recovery");
    missingJpeg = {};
    writeOwnedBytes(jpeg, jpegBytes);
    original.fill(Qt::red);
    require(original.save(source, "JPEG"), "could not modify owned source fixture");
    require(!prepareDesktopCloudDriveImage(source, checkpoint, QFileInfo(source).fileName()).error.isEmpty() &&
                readOwnedBytes(jpeg) == jpegBytes,
            "changed original reused or overwrote another thumbnail identity");
    writeOwnedBytes(source, originalBytes);
    require(finishDesktopCloudDriveImage(checkpoint, true) && !QFileInfo::exists(jpeg) &&
                !QFileInfo::exists(metadata) && QFileInfo::exists(source) && QFileInfo::exists(checkpoint),
            "confirmed completion did not clean only the owned auxiliary files");
    auto missingLocalThumbnail = prepareDesktopCloudDriveImage(source, checkpoint, QFileInfo(source).fileName());
    require(missingLocalThumbnail.error.isEmpty() && missingLocalThumbnail.image &&
                missingLocalThumbnail.encodedOriginal == originalBytes &&
                missingLocalThumbnail.thumbnailPath.isEmpty() &&
                !QFileInfo::exists(jpeg) && !QFileInfo::exists(metadata),
            "missing persisted thumbnail could not reach the shared authoritative-ready recovery path");
    missingLocalThumbnail = {};
    original.fill(Qt::red);
    require(original.save(source, "JPEG"), "could not change owned missing-thumbnail original fixture");
    require(!prepareDesktopCloudDriveImage(source, checkpoint, QFileInfo(source).fileName()).error.isEmpty(),
            "missing auxiliary files bypassed the checkpoint's original source binding");
    writeOwnedBytes(source, originalBytes);
    prepared = {};
    require(QFile::remove(checkpoint), "could not remove owned completed checkpoint fixture");

    // Old, small checkpoint records retain their original no-thumbnail contract.
    writeOwnedBytes(checkpoint, QByteArray("{\"version\":1}"));
    auto legacy = prepareDesktopCloudDriveImage(source, checkpoint, QFileInfo(source).fileName());
    require(legacy.error.isEmpty() && legacy.image && legacy.thumbnailPath.isEmpty() &&
                !QFileInfo::exists(jpeg) && !QFileInfo::exists(metadata),
            "legacy checkpoint was attached to a different upload protocol");
    legacy = {};
    require(QFile::remove(checkpoint), "could not remove owned legacy checkpoint fixture");
    require(prepareDesktopCloudDriveImage(source, checkpoint, QFileInfo(source).fileName()).error.isEmpty() &&
                finishDesktopCloudDriveImage(checkpoint, false) && !QFileInfo::exists(jpeg) &&
                !QFileInfo::exists(metadata) && QFileInfo::exists(source),
            "pre-initialization failure retained unregistered auxiliary data or removed original media");
    const QString broken = owned.path() + QStringLiteral("/cloud-invalid-thumbnail-source.jpg");
    writeOwnedBytes(broken, QByteArray("not an image"));
    require(!prepareDesktopCloudDriveImage(broken, checkpoint, QFileInfo(broken).fileName()).error.isEmpty() &&
                !QFileInfo::exists(jpeg) && !QFileInfo::exists(metadata),
            "invalid image became a prepared cloud upload");

    const QString firstCheckpoint = owned.path() + QStringLiteral("/first.maicloud.json");
    const QString secondCheckpoint = owned.path() + QStringLiteral("/second.maicloud.json");
    const QString thirdCheckpoint = owned.path() + QStringLiteral("/third.maicloud.json");
    auto first = prepareDesktopCloudDriveImage(source, firstCheckpoint, QFileInfo(source).fileName());
    auto borrowed = first;
    first = {};
    auto second = prepareDesktopCloudDriveImage(source, secondCheckpoint, QFileInfo(source).fileName());
    require(borrowed.error.isEmpty() && second.error.isEmpty(), "two bounded image snapshots were not admitted");
    auto blocked = prepareDesktopCloudDriveImage(source, thirdCheckpoint, QFileInfo(source).fileName());
    require(blocked.resourceBusy && !blocked.error.isEmpty() && blocked.encodedOriginal.isEmpty() &&
                !QFileInfo::exists(thirdCheckpoint + QStringLiteral(".thumbnail.jpg")),
            "third cloud image bypassed the shared snapshot memory bound");
    borrowed = {};
    auto afterRelease = prepareDesktopCloudDriveImage(source, thirdCheckpoint, QFileInfo(source).fileName());
    require(afterRelease.error.isEmpty() && !afterRelease.resourceBusy,
            "dropping the final borrowed snapshot did not release its memory slot");
    afterRelease = {};
    second = {};
    require(finishDesktopCloudDriveImage(firstCheckpoint, false) &&
                finishDesktopCloudDriveImage(secondCheckpoint, false) &&
                finishDesktopCloudDriveImage(thirdCheckpoint, false),
            "bounded snapshot fixtures did not release owned auxiliary files");
}

} // namespace

int main(int argc, char** argv) {
    require(argc == 5, "expected TLS base, certificate, fixture video, screenshots directory");
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("MaiChatTests"));
    QCoreApplication::setApplicationName(QStringLiteral("CloudDriveDialogTest-") +
        QString::number(QCoreApplication::applicationPid()));
    QFile ca(QString::fromUtf8(argv[2]));
    require(ca.open(QIODevice::ReadOnly), "could not load test certificate");
    const QString base = QString::fromUtf8(argv[1]);
    const QString source = QString::fromUtf8(argv[3]);
    const QString screenshots = QString::fromUtf8(argv[4]);
    require(QFileInfo(source).isFile(), "missing fixture video");
    const QString imageSource = screenshots + QStringLiteral("/cloud-test-image.jpg");
    QImage imageFixture(320, 180, QImage::Format_RGB32);
    imageFixture.fill(QColor(28, 138, 211));
    require(imageFixture.save(imageSource, "JPEG"), "could not create image fixture");
    if (!qEnvironmentVariableIsSet("MAICHAT_CLOUD_413_ONLY")) verifyPreparedCloudImage(screenshots);
    if (qEnvironmentVariableIsSet("MAICHAT_CLOUD_HELPER_ONLY")) return 0;

    DesktopVideoCircleAccess access;
    access.authBaseUrl = base + QStringLiteral("/auth");
    access.mediaBaseUrl = base + QStringLiteral("/oss");
    access.accountToken = QStringLiteral("author-token");
    access.accountId = QStringLiteral("author");
    access.caPem = ca.readAll();
    access.signedUploadHostSuffix = QStringLiteral("localhost");
    access.proxyUrl = QStringLiteral("");
    if (qEnvironmentVariableIsSet("MAICHAT_CLOUD_413_ONLY")) {
        access.authBaseUrl = base + QStringLiteral("/quota-rejection");
        QString shownError;
        CloudDriveDialog rejected([access] { return access; },
            [] { return QStringLiteral("author"); }, {});
        rejected.setErrorHandler([&shownError](const QString& error) { shownError = error; });
        rejected.openDrive();
        QElapsedTimer wait;
        wait.start();
        while (shownError.isEmpty() && wait.elapsed() < 5000) {
            QApplication::processEvents();
            QThread::msleep(10);
        }
        std::cerr << "[owned-cloud-ui-413] " << shownError.toUtf8().constData() << std::endl;
        require(!shownError.isEmpty() && shownError.contains(QStringLiteral("空间")) &&
                    !shownError.contains(QStringLiteral("10 GB")) &&
                    !shownError.contains(QStringLiteral("2 GB")) &&
                    !shownError.contains(QStringLiteral("20 MB")),
                "ordinary cloud 413 error retained a fixed historical account quota or media cap");
        rejected.close();
        return 0;
    }
    require(CloudDriveDialog::availability(access) == 1,
            "cloud feature gate did not accept available account API");
    DesktopVideoCircleAccess disabled = access;
    disabled.authBaseUrl = base + QStringLiteral("/disabled");
    require(CloudDriveDialog::availability(disabled) == 0,
            "cloud feature gate exposed a disabled API");
    QString playedName;
    std::shared_ptr<MaiCloudVod> playedVod;
    CloudDriveDialog dialog([access] { return access; },
        [] { return QStringLiteral("author"); },
        [&playedName, &playedVod](std::shared_ptr<MaiCloudVod> source,
                                  const QString& name) {
            playedVod = std::move(source);
            playedName = name;
        });
    dialog.openDrive();
    // The page stays concise; size policy is reported only for an attempted
    // upload, rather than left in persistent labels or an obsolete tooltip.
    for (QLabel* label : dialog.findChildren<QLabel*>()) {
        require(!label->text().contains(QStringLiteral("仅自己可见")) &&
                    !label->text().contains(QStringLiteral("2 GB")) &&
                    !label->text().contains(QStringLiteral("20 MB")),
                "persistent cloud privacy or old file-size hint remains");
    }
    const auto* uploadAction =
        dialog.findChild<QPushButton*>(QStringLiteral("cloudDriveUpload"));
    require(uploadAction && uploadAction->toolTip() == QStringLiteral("上传图片或视频"),
            "cloud upload tooltip retains obsolete file-size policy");
    require(!dialog.findChild<QProgressBar*>(QStringLiteral("cloudDriveProgress")) &&
            !dialog.findChild<QLabel*>(QStringLiteral("cloudDriveStatus")) &&
            !dialog.findChild<QPushButton*>(QStringLiteral("cloudDrivePause")),
            "removed cloud footer is still present");
    require(waitForLabel(dialog, QStringLiteral("cloudDriveQuota"),
        QStringLiteral("10.00 GB")), "10 GB cloud quota did not load");
    require(!visibleButton(dialog, QStringLiteral("cloudDrivePlay")),
            "empty cloud has play button");
    require(waitForVisibleText(dialog, QStringLiteral("云盘里还没有图片或视频")),
            "empty cloud text is not visible");
    require(dialog.grab().save(screenshots + QStringLiteral("/cloud-empty.png")),
            "failed to save empty state screenshot");

    // 改名/移动留在 Agent 工具，不保留隐藏的 UI 表单或查看/播放按钮。
    for (const QString& removed : {QStringLiteral("cloudDriveFolderRename"),
                                  QStringLiteral("cloudDriveFolderMove"),
                                  QStringLiteral("cloudDriveRenameMedia"),
                                  QStringLiteral("cloudDriveMoveMedia"),
                                  QStringLiteral("cloudDriveViewImage"),
                                  QStringLiteral("cloudDrivePlay")})
        require(dialog.findChildren<QPushButton*>(removed).isEmpty(), "removed action remains");
    nameFolder(dialog, visibleButton(dialog, QStringLiteral("cloudDriveNewFolder")),
               QStringLiteral("工作"));
    folderRow(dialog, QStringLiteral("工作"))->findChild<QPushButton*>(
        QStringLiteral("cloudDriveFolderOpen"))->click();
    require(waitForButton(dialog, QStringLiteral("cloudDriveNewFolder")), "folder did not load");
    nameFolder(dialog, visibleButton(dialog, QStringLiteral("cloudDriveNewFolder")),
               QStringLiteral("文档"));
    folderRow(dialog, QStringLiteral("文档"))->findChild<QPushButton*>(
        QStringLiteral("cloudDriveFolderOpen"))->click();
    require(waitForButton(dialog, QStringLiteral("cloudDriveNewFolder")), "nested folder did not load");
    require(waitForLabel(dialog, QStringLiteral("cloudDriveFolderPath"),
        QStringLiteral("我的云盘 / 工作 / 文档")), "nested path was incorrect");

    QTimer confirmation;
    confirmation.setInterval(20);
    QObject::connect(&confirmation, &QTimer::timeout, [&] {
        auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!modal) return;
        auto* confirm = modal->findChild<QPushButton*>(QStringLiteral("appMessageConfirm"));
        if (!confirm) confirm = modal->findChild<QPushButton*>(QStringLiteral("appMessageDanger"));
        if (confirm) { confirmation.stop(); confirm->click(); }
    });
    confirmation.start();
    dialog.openDrive(source);
    require(waitForMedia(dialog, QStringLiteral("video")), "uploaded video did not appear");
    require(waitForThumbnail(dialog), "signed video thumbnail did not load");
    auto* row = mediaRow(dialog, QStringLiteral("video"));
    auto* download = row->findChild<QPushButton*>(QStringLiteral("cloudDriveSaveVideo"));
    auto* remove = row->findChild<QPushButton*>(QStringLiteral("cloudDriveDelete"));
    require(download && remove && download->text().isEmpty() && remove->text().isEmpty() &&
            !download->accessibleName().isEmpty() && !remove->accessibleName().isEmpty(),
            "download/delete icons lost their accessible names");
    require(dialog.grab().save(screenshots + QStringLiteral("/cloud-uploaded.png")),
            "could not capture simplified cloud list");
    dialog.resize(540, 640);
    QApplication::processEvents();
    require(dialog.grab().save(screenshots + QStringLiteral("/cloud-folders-narrow.png")),
            "could not capture narrow cloud list");
    dialog.resize(740, 640);
    auto* thumbnail = row->findChild<QLabel*>(QStringLiteral("cloudDriveThumbnail"));
    QTest::mouseClick(thumbnail, Qt::LeftButton);
    QApplication::processEvents();
    require(!playedVod, "single click unexpectedly started playback");
    openMedia(dialog, QStringLiteral("video"));
    QElapsedTimer playback;
    playback.start();
    while (!playedVod && playback.elapsed() < 15000) {
        QApplication::processEvents();
        QThread::msleep(20);
    }
    require(playedVod && playedName == QFileInfo(source).fileName() &&
            mai_cloud_vod_size(playedVod.get()) == quint64(QFileInfo(source).size()),
            "double click did not open seekable cloud input");
    playedVod.reset();

    if (QApplication::platformName() != QStringLiteral("offscreen")) {
        // 使用正式 MainWindow 和云盘双击回调，验证独立窗口不挤压云盘、关闭后仍在原目录。
        RemoteIMApplication fakeApp(QStringLiteral("cloud-window-test"),
                                    std::make_unique<FakeRemoteIMClient>());
        QString owner = QStringLiteral("author");
        MainWindow window(fakeApp);
        window.setMaiChatAccountIdProvider([&] { return owner; });
        window.setVideoCircleAccessProvider([access] { return access; });
        window.resize(1280, 820);
        window.show();
        auto* stack = window.findChild<QStackedWidget*>(QStringLiteral("contentStack"));
        auto* page = window.findChild<QWidget*>(QStringLiteral("cloudDrivePage"));
        auto* drive = dynamic_cast<CloudDriveDialog*>(
            window.findChild<QWidget*>(QStringLiteral("cloudDriveDialog")));
        require(stack && page && drive, "normal MainWindow cloud page is unavailable");
        stack->setCurrentWidget(page);
        drive->openDrive();
        require(waitForFolder(*drive, QStringLiteral("工作")), "main cloud folder did not load");
        folderRow(*drive, QStringLiteral("工作"))->findChild<QPushButton*>(
            QStringLiteral("cloudDriveFolderOpen"))->click();
        require(waitForFolder(*drive, QStringLiteral("文档")), "main cloud child did not load");
        folderRow(*drive, QStringLiteral("文档"))->findChild<QPushButton*>(
            QStringLiteral("cloudDriveFolderOpen"))->click();
        require(waitForMedia(*drive, QStringLiteral("video")), "main cloud video did not load");
        const QSize pageSize = page->size();
        openMedia(*drive, QStringLiteral("video"));
        QElapsedTimer timer;
        timer.start();
        while (!MaiFfplayVideoDialog::activeDialog() && timer.elapsed() < 15000)
            QTest::qWait(20);
        QPointer<MaiFfplayVideoDialog> player = MaiFfplayVideoDialog::activeDialog();
        require(player && player->isWindow() &&
                (player->windowFlags() & Qt::FramelessWindowHint) &&
                player->findChild<QWidget*>(QStringLiteral("ffplayTitleBar")),
                "cloud did not open the shared independent player window");
        auto* buffering = player->findChild<QWidget*>(QStringLiteral("ffplayBufferingIndicator"));
        auto* error = player->findChild<QLabel*>(QStringLiteral("ffplayErrorLabel"));
        require(buffering && error, "playback loading or error widget is unavailable");
        // prepareAndShow publishes the active session while its owner is still
        // hidden awaiting the real video size. Inspect the user-visible phase,
        // not an allocated hidden window whose children cannot be isVisible().
        std::cerr << "[owned-cloud-player-before-map] ownerVisible=" << player->isVisible()
                  << " indicatorVisible=" << buffering->isVisible()
                  << " indicatorExplicitHidden=" << buffering->isHidden()
                  << " hasFrame=" << player->hasPresentedFrame() << std::endl;
        QElapsedTimer mapped;
        mapped.start();
        while (player && !player->isVisible() && mapped.elapsed() < 15000) QTest::qWait(20);
        const auto requireOwnedPlayer = [&](bool condition, const char* message) {
            if (!condition && player) {
                player->close();
                QTest::qWait(20);
            }
            require(condition, message);
        };
        requireOwnedPlayer(player && player->isVisible(), "prepared cloud player did not map its owner window");
        std::cerr << "[owned-cloud-player-mapped] ownerVisible=" << player->isVisible()
                  << " indicatorVisible=" << buffering->isVisible()
                  << " indicatorExplicitHidden=" << buffering->isHidden()
                  << " hasFrame=" << player->hasPresentedFrame() << std::endl;
        requireOwnedPlayer(!error->isVisible(), "buffering was shown as an error banner");
        if (!player->hasPresentedFrame()) {
            requireOwnedPlayer(buffering->isVisible(), "first-frame loading indicator was missing");
            // 只记录实际加载控件，避开 QWidget::grab 中不可靠的原生视频 backingstore。
            const QImage icon = buffering->grab().toImage();
            int darkest = 255;
            int lightest = 0;
            for (int y = 0; y < icon.height(); ++y)
                for (int x = 0; x < icon.width(); ++x) {
                    const int gray = qGray(icon.pixel(x, y));
                    darkest = qMin(darkest, gray);
                    lightest = qMax(lightest, gray);
                }
            require(lightest - darkest > 100, "loading icon has no visible contrast");
            require(icon.save(screenshots + QStringLiteral("/cloud-buffering-icon.png")),
                    "could not capture cloud loading indicator");
        }
        auto* dock = window.findChild<QWidget*>(QStringLiteral("videoDockSlot"));
        require(dock && !dock->isVisible() && page->size() == pageSize,
                "cloud video resized the main list or opened the dock");
        timer.restart();
        while (player->presentedFrames() == 0 && timer.elapsed() < 15000) QTest::qWait(20);
        require(player->presentedFrames() > 0, "independent cloud window produced no frames");
        timer.restart();
        while (buffering->isVisible() && timer.elapsed() < 2000) QTest::qWait(20);
        require(!buffering->isVisible() && !error->isVisible(), "loading indicator remained over decoded video");
        QTest::qWait(300); // 等正常进度计时器收起缓冲提示后再记录控件画面。
        require(window.grab().save(screenshots + QStringLiteral("/cloud-main-window.png")),
                "could not capture normal MainWindow cloud page");
        // 此截图仅供检查 Qt 控件布局；真实原生视频合成由独立播放器录屏验收。
        require(player->grab().save(screenshots + QStringLiteral("/cloud-independent-player.png")),
                "could not capture independent player controls");
        player->close();
        QTest::qWait(100);
        require(!MaiFfplayVideoDialog::activeDialog() && mediaRow(*drive, QStringLiteral("video")),
                "closing playback lost the cloud list");
        openMedia(*drive, QStringLiteral("video"));
        timer.restart();
        while (!MaiFfplayVideoDialog::activeDialog() && timer.elapsed() < 15000) QTest::qWait(20);
        require(bool(MaiFfplayVideoDialog::activeDialog()), "could not reopen independent player");
        owner.clear();
        timer.restart();
        while (MaiFfplayVideoDialog::activeDialog() && timer.elapsed() < 2000) QTest::qWait(20);
        require(!MaiFfplayVideoDialog::activeDialog(), "account change did not close cloud playback");
        window.close();
    }
    confirmation.start();
    visibleButton(dialog, QStringLiteral("cloudDriveDelete"))->click();
    require(waitForLabel(dialog, QStringLiteral("cloudDriveQuota"), QStringLiteral("已使用 0.0 KB")),
            "video deletion did not release quota");
    require(!mediaRow(dialog, QStringLiteral("video")), "deleted video remained visible");

    visibleButton(dialog, QStringLiteral("cloudDriveParentFolder"))->click();
    require(waitForFolder(dialog, QStringLiteral("文档")), "parent navigation lost child folder");
    confirmation.start();
    folderRow(dialog, QStringLiteral("文档"))->findChild<QPushButton*>(
        QStringLiteral("cloudDriveFolderDelete"))->click();
    require(waitForButton(dialog, QStringLiteral("cloudDriveNewFolder")), "folder delete did not finish");
    require(!folderRow(dialog, QStringLiteral("文档")), "empty folder remained visible");
    visibleButton(dialog, QStringLiteral("cloudDriveRootFolder"))->click();
    require(waitForFolder(dialog, QStringLiteral("工作")), "root folder did not load");
    require(dialog.grab().save(screenshots + QStringLiteral("/cloud-folders-root.png")),
            "could not capture root folders");
    confirmation.start();
    folderRow(dialog, QStringLiteral("工作"))->findChild<QPushButton*>(
        QStringLiteral("cloudDriveFolderDelete"))->click();
    require(waitForButton(dialog, QStringLiteral("cloudDriveNewFolder")), "root cleanup did not finish");

    confirmation.start();
    dialog.openDrive(imageSource);
    require(waitForMedia(dialog, QStringLiteral("image")), "uploaded image did not appear");
    require(waitForThumbnail(dialog), "signed image thumbnail did not load");
    require(dialog.grab().save(screenshots + QStringLiteral("/cloud-image-uploaded.png")),
            "could not capture simplified image row");
    openMedia(dialog, QStringLiteral("image"));
    ImagePreviewDialog* preview = waitForImagePreview(dialog);
    require(preview && !preview->sourceImage().isNull(), "double click did not decode original image");
    require(preview->grab().save(screenshots + QStringLiteral("/cloud-image-preview.png")),
            "could not capture image preview");
    preview->close();
    confirmation.start();
    visibleButton(dialog, QStringLiteral("cloudDriveDelete"))->click();
    require(waitForLabel(dialog, QStringLiteral("cloudDriveQuota"), QStringLiteral("已使用 0.0 KB")),
            "image deletion did not release quota");

    const QString largeFile = screenshots + QStringLiteral("/resume-fixture.mov");
    QFile large(largeFile);
    require(large.open(QIODevice::WriteOnly | QIODevice::Truncate),
            "could not create resumable fixture");
    const QByteArray block(512 * 1024, 'v');
    for (int i = 0; i < 48; ++i)
        require(large.write(block) == block.size(), "could not write resumable fixture");
    large.close();
    const QString authorId = QStringLiteral("cloud-ui-author-") +
                             QString::number(QCoreApplication::applicationPid());
    const QString viewerId = QStringLiteral("cloud-ui-viewer-") +
                             QString::number(QCoreApplication::applicationPid());
    QString currentAccount = authorId;
    DesktopVideoCircleAccess mutableAccess = access;
    mutableAccess.accountId = authorId;
    CloudDriveDialog switching([&] { return mutableAccess; },
        [&] { return currentAccount; },
        [](std::shared_ptr<MaiCloudVod>, const QString&) {});
    confirmation.start();
    switching.openDrive(largeFile);
    const QString key = QString::fromLatin1(QCryptographicHash::hash(
        authorId.toUtf8(), QCryptographicHash::Sha256).toHex());
    const QString checkpointDir = QDir(QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation)).filePath(QStringLiteral("CloudDrive/") + key);
    QElapsedTimer checkpointWait;
    checkpointWait.start();
    while (checkpointWait.elapsed() < 12000 &&
           QDir(checkpointDir).entryList({QStringLiteral("*.maicloud.json")},
                                         QDir::Files).isEmpty()) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    require(!QDir(checkpointDir).entryList({QStringLiteral("*.maicloud.json")},
                                           QDir::Files).isEmpty(),
            "upload did not make a durable checkpoint");
    // Switching accounts while a part is in flight cancels the old account's
    // worker and must never show its rows under the new account.
    currentAccount = viewerId;
    mutableAccess.accountId = viewerId;
    mutableAccess.accountToken = QStringLiteral("viewer-token");
    switching.openDrive();
    require(waitForLabel(switching, QStringLiteral("cloudDriveQuota"),
        QStringLiteral("已使用 0.0 KB")), "new account did not load isolated cloud quota");
    QElapsedTimer finishOldWorker;
    finishOldWorker.start();
    while (finishOldWorker.elapsed() < 5000) {
        QApplication::processEvents();
        bool pending = false;
        for (QFutureWatcherBase* worker : switching.findChildren<QFutureWatcherBase*>())
            pending |= !worker->isFinished();
        if (!pending) break;
        QThread::msleep(20);
    }
    require(!QDir(checkpointDir).entryList({QStringLiteral("*.maicloud.json")},
                                           QDir::Files).isEmpty(),
            "account switch lost the resumable checkpoint");
    switching.close();
    const QString movedFile = screenshots + QStringLiteral("/relocated-video.mov");
    QFile::remove(movedFile);
    require(QFile::rename(largeFile, movedFile), "could not relocate upload source");
    DesktopVideoCircleAccess restored = access;
    restored.accountId = authorId;
    CloudDriveDialog resumed([restored] { return restored; },
        [authorId] { return authorId; },
        [](std::shared_ptr<MaiCloudVod>, const QString&) {}, nullptr,
        [&movedFile](const QString&) { return movedFile; });
    resumed.openDrive();
    require(waitForButton(resumed, QStringLiteral("cloudDriveResume")),
            "recreated page did not enumerate the pending upload");
    require(resumed.grab().save(screenshots + QStringLiteral("/cloud-resume.png")),
            "failed to save resume screenshot");
    visibleButton(resumed, QStringLiteral("cloudDriveResume"))->click();
    require(waitForMedia(resumed, QStringLiteral("video"), 30000),
            "resumed upload did not complete");
    confirmation.start();
    visibleButton(resumed, QStringLiteral("cloudDriveDelete"))->click();
    require(waitForLabel(resumed, QStringLiteral("cloudDriveQuota"),
        QStringLiteral("已使用 0.0 KB")), "resumed video was not deleted");
    QFile::remove(movedFile);
    const QString testData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    require(testData.contains(QStringLiteral("CloudDriveDialogTest-")),
            "refusing to remove a non-test data directory");
    require(QDir(testData).removeRecursively(), "could not remove isolated test checkpoints");
    std::cout << "desktop cloud navigation/double-click/icons/account isolation/resume lifecycle passed\n";
    return 0;
}
