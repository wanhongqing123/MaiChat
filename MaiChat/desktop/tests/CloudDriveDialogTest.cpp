#include "ui/CloudDriveDialog.h"
#include "MaiCloudVod.h"
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
#include <QLabel>
#include <QLineEdit>
#include <QFrame>
#include <QPushButton>
#include <QProgressBar>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QThread>
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

    DesktopVideoCircleAccess access;
    access.authBaseUrl = base + QStringLiteral("/auth");
    access.mediaBaseUrl = base + QStringLiteral("/oss");
    access.accountToken = QStringLiteral("author-token");
    access.accountId = QStringLiteral("author");
    access.caPem = ca.readAll();
    access.signedUploadHostSuffix = QStringLiteral("localhost");
    access.proxyUrl = QStringLiteral("");
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
                !(player->windowFlags() & Qt::FramelessWindowHint), "cloud did not open independent window");
        auto* dock = window.findChild<QWidget*>(QStringLiteral("videoDockSlot"));
        require(dock && !dock->isVisible() && page->size() == pageSize,
                "cloud video resized the main list or opened the dock");
        timer.restart();
        while (player->presentedFrames() == 0 && timer.elapsed() < 15000) QTest::qWait(20);
        require(player->presentedFrames() > 0, "independent cloud window produced no frames");
        QTest::qWait(300); // 等正常进度计时器收起缓冲提示后再记录控件画面。
        require(window.grab().save(screenshots + QStringLiteral("/cloud-main-window.png")),
                "could not capture normal MainWindow cloud page");
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
