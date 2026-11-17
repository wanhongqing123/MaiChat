// Verify real Windows client/HUD rectangles: Qt-only coordinates can agree
// while the separately mapped native Tool windows are visibly displaced.
#include <QApplication>
#include <QAbstractNativeEventFilter>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QScopeGuard>
#include <QSlider>
#include <QTimer>
#include <QtTest>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "app/RemoteIMApplication.h"
#include "im/FakeRemoteIMClient.h"
#include "ui/MainWindow.h"
#include "ui/MaiFfplayVideoDialog.h"

namespace {
class NativeHudShows final : public QAbstractNativeEventFilter {
public:
    int shows = 0;
    int beforeOwner = 0;
    bool nativeEventFilter(const QByteArray&, void* value, qintptr*) override {
        const auto* message = static_cast<MSG*>(value);
        if (message->message != WM_WINDOWPOSCHANGING) return false;
        const auto* position = reinterpret_cast<WINDOWPOS*>(message->lParam);
        if (!(position->flags & SWP_SHOWWINDOW)) return false;
        auto* widget = QWidget::find(WId(message->hwnd));
        if (!widget || (widget->objectName() != QStringLiteral("ffplayTitleBar") &&
                        widget->objectName() != QStringLiteral("ffplayOverlay")))
            return false;
        auto* owner = qobject_cast<MaiFfplayVideoDialog*>(widget->parentWidget());
        if (!owner) return false;
        ++shows;
        if (!IsWindowVisible(reinterpret_cast<HWND>(owner->winId()))) ++beforeOwner;
        return false;
    }
};

QRect nativeClient(QWidget* widget) {
    const HWND handle = reinterpret_cast<HWND>(widget->winId());
    RECT bounds{};
    POINT origin{};
    GetClientRect(handle, &bounds);
    ClientToScreen(handle, &origin);
    return QRect(origin.x, origin.y, bounds.right, bounds.bottom);
}

QJsonObject geometry(QWidget* widget) {
    const QRect native = nativeClient(widget);
    const QPoint qtOrigin = widget->mapToGlobal(QPoint());
    return {{"nativeX", native.x()},
            {"nativeY", native.y()},
            {"nativeWidth", native.width()},
            {"nativeHeight", native.height()},
            {"qtX", qtOrigin.x()},
            {"qtY", qtOrigin.y()},
            {"qtWidth", widget->width()},
            {"qtHeight", widget->height()},
            {"dpr", widget->devicePixelRatioF()},
            {"visible", widget->isVisible()},
            {"dpi", int(GetDpiForWindow(reinterpret_cast<HWND>(widget->winId())))}};
}

bool saveOwnWindow(QWidget* widget, const QString& path, QImage* captured = nullptr,
                   const QRect& clientRegion = {}) {
    if (path.isEmpty() && !captured) return false;
    const QRect client = nativeClient(widget);
    const QRect bounds = clientRegion.isEmpty() ? client : clientRegion.translated(client.topLeft());
    if (bounds.isEmpty() || !client.contains(bounds)) return false;
    // Color measurements use a small physical-pixel patch and check every pixel;
    // keep the existing whole-window geometry capture's ownership check spacing.
    const int step = clientRegion.isEmpty() ? 16 : 1;
    for (int y = bounds.top(); y <= bounds.bottom(); y += step)
        for (int x = bounds.left(); x <= bounds.right(); x += step) {
            DWORD process = 0;
            GetWindowThreadProcessId(WindowFromPoint(POINT{x, y}), &process);
            if (process != GetCurrentProcessId()) return false;
        }
    QImage image(bounds.size(), QImage::Format_RGB32);
    HDC display = GetDC(nullptr);
    if (!display) return false;
    const auto releaseDisplay = qScopeGuard([&] { ReleaseDC(nullptr, display); });
    HDC memory = CreateCompatibleDC(display);
    if (!memory) return false;
    const auto releaseMemory = qScopeGuard([&] { DeleteDC(memory); });
    HBITMAP bitmap = CreateCompatibleBitmap(display, bounds.width(), bounds.height());
    if (!bitmap) return false;
    const auto releaseBitmap = qScopeGuard([&] { DeleteObject(bitmap); });
    const auto previous = SelectObject(memory, bitmap);
    if (!previous || previous == HGDI_ERROR) return false;
    const BOOL copied = BitBlt(memory, 0, 0, bounds.width(), bounds.height(), display,
                               bounds.x(), bounds.y(), SRCCOPY);
    SelectObject(memory, previous);
    if (!copied) return false;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = bounds.width();
    info.bmiHeader.biHeight = -bounds.height();
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    if (GetDIBits(memory, bitmap, 0, bounds.height(), image.bits(), &info, DIB_RGB_COLORS) !=
        bounds.height())
        return false;
    if (captured) *captured = image;
    return path.isEmpty() || image.save(path);
}
}  // namespace

class MaiFfplayWindowsGeometryTest : public QObject {
    Q_OBJECT
private slots:
    void preparedPlayerKeepsNativeHudAligned_data();
    void preparedPlayerKeepsNativeHudAligned();
    void sdrGrayPlaybackKeepsExpectedBrightness_data();
    void sdrGrayPlaybackKeepsExpectedBrightness();
};

void MaiFfplayWindowsGeometryTest::preparedPlayerKeepsNativeHudAligned_data() {
    QTest::addColumn<bool>("prepared");
    QTest::newRow("ordinary-main-prepared") << true;
    QTest::newRow("direct-show") << false;
}

void MaiFfplayWindowsGeometryTest::preparedPlayerKeepsNativeHudAligned() {
    QFETCH(bool, prepared);
    QCOMPARE(QGuiApplication::platformName(), QStringLiteral("windows"));
    // A supplied local clip is only an opt-in incident reproduction; normal
    // automated runs retain the repository-owned fixture and never scan user media.
    const QString suppliedPath = qEnvironmentVariable("MAICHAT_WINDOWS_VIDEO_FIXTURE_OVERRIDE");
    const QString path = suppliedPath.isEmpty() ? QStringLiteral(MAICHAT_WINDOWS_VIDEO_FIXTURE) : suppliedPath;
    QVERIFY(QFileInfo::exists(path));
    RemoteIMApplication app(QStringLiteral("owned-windows-video-geometry"), std::make_unique<FakeRemoteIMClient>());
    MainWindow main(app);
    main.setWindowFlag(Qt::WindowStaysOnTopHint);
    main.move(120, 70);
    main.show();
    QVERIFY(QTest::qWaitForWindowExposed(&main));
    main.raise();
    SetForegroundWindow(reinterpret_cast<HWND>(main.winId()));
    NativeHudShows probe;
    qApp->installNativeEventFilter(&probe);
    const auto removeProbe = qScopeGuard([&] { qApp->removeNativeEventFilter(&probe); });
    QPointer<MaiFfplayVideoDialog> player;
    if (prepared) {
        QVERIFY(main.openVideoWindow(path, {}));
        player = MaiFfplayVideoDialog::activeDialog();
    } else {
        player = new MaiFfplayVideoDialog(path, &main);
        player->setAttribute(Qt::WA_DeleteOnClose);
        player->show();
    }
    QVERIFY(player);
    const auto close = qScopeGuard([&] {
        if (player) player->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    });
    auto* volume = player->findChild<QSlider*>(QStringLiteral("ffplayVolumeSlider"));
    QVERIFY(volume);
    volume->setValue(0);
    QTRY_VERIFY_WITH_TIMEOUT(player->hasPresentedFrame() && player->isVisible(), 10000);
    auto* header = player->findChild<QWidget*>(QStringLiteral("ffplayTitleBar"));
    auto* footer = player->findChild<QWidget*>(QStringLiteral("ffplayOverlay"));
    auto* surface = player->findChild<QWidget*>(QStringLiteral("ffplayVideoSurface"));
    QVERIFY(header && footer && surface);
    player->raise();
    player->activateWindow();
    SetForegroundWindow(reinterpret_cast<HWND>(player->winId()));
    QTRY_VERIFY(header->isVisible() && footer->isVisible());
    QTest::qWait(350);
    QJsonObject observation{{"player", geometry(player)}, {"surface", geometry(surface)},
                            {"header", geometry(header)}, {"footer", geometry(footer)},
                            {"hudShows", probe.shows},    {"hudShowsBeforeOwner", probe.beforeOwner}};
    qInfo().noquote() << QJsonDocument(observation).toJson(QJsonDocument::Compact);
    const QString outputBase = qEnvironmentVariable("MAICHAT_WINDOWS_GEOMETRY_OUTPUT");
    if (!outputBase.isEmpty()) {
        const QString output = outputBase + QStringLiteral("-") + QString::fromLatin1(QTest::currentDataTag());
        QFile file(output + QStringLiteral(".json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(QJsonDocument(observation).toJson()) > 0);
        QVERIFY2(saveOwnWindow(player, output + QStringLiteral(".png")),
                 "Owned player must be unobscured and its evidence image must save successfully");
    }
    const QRect owner = nativeClient(player);
    QCOMPARE(nativeClient(surface), owner);
    QCOMPARE(nativeClient(header).topLeft(), owner.topLeft());
    QCOMPARE(nativeClient(header).width(), owner.width());
    QCOMPARE(nativeClient(footer).left(), owner.left());
    QCOMPARE(nativeClient(footer).bottom(), owner.bottom());
    QCOMPARE(nativeClient(footer).width(), owner.width());
    QVERIFY(probe.shows > 0);
    QCOMPARE(probe.beforeOwner, 0);
    const auto aligned = [&] {
        const QRect actualOwner = nativeClient(player);
        const QRect actualHeader = nativeClient(header);
        const QRect actualFooter = nativeClient(footer);
        // Fractional screen scaling can round a logical fullscreen edge one
        // physical pixel beyond Windows' exact monitor rectangle.
        const auto closeEdge = [](int a, int b) { return std::abs(a - b) <= 1; };
        return closeEdge(actualHeader.left(), actualOwner.left()) && closeEdge(actualHeader.top(), actualOwner.top()) &&
               closeEdge(actualHeader.right(), actualOwner.right()) &&
               closeEdge(actualFooter.left(), actualOwner.left()) &&
               closeEdge(actualFooter.bottom(), actualOwner.bottom()) &&
               closeEdge(actualFooter.right(), actualOwner.right());
    };
    const auto alignmentState = [&](const char* stage) {
        return QByteArray(stage) + ": " +
               QJsonDocument(QJsonObject{{"player", geometry(player)},
                                         {"header", geometry(header)},
                                         {"footer", geometry(footer)}})
                   .toJson(QJsonDocument::Compact);
    };
    player->move(player->pos() + QPoint(47, 31));
    QTRY_VERIFY2_WITH_TIMEOUT(aligned(), alignmentState("move").constData(), 1500);
    player->resize(960, 540);
    QTRY_VERIFY2_WITH_TIMEOUT(aligned(), alignmentState("resize").constData(), 1500);
    QTest::keyClick(header, Qt::Key_F);
    QTRY_VERIFY(player->isFullScreen());
    QTRY_VERIFY2_WITH_TIMEOUT(aligned(), alignmentState("fullscreen").constData(), 1500);
    QTest::keyClick(header, Qt::Key_Escape);
    QTRY_VERIFY(!player->isFullScreen());
    QTRY_VERIFY2_WITH_TIMEOUT(aligned(), alignmentState("restore").constData(), 1500);
    player->showMinimized();
    QTRY_VERIFY(!header->isVisible() && !footer->isVisible());
    player->showNormal();
    QTRY_VERIFY(player->isVisible() && header->isVisible() && footer->isVisible());
    QTRY_VERIFY2_WITH_TIMEOUT(aligned(), alignmentState("unminimize").constData(), 1500);
}

void MaiFfplayWindowsGeometryTest::sdrGrayPlaybackKeepsExpectedBrightness_data() {
    QTest::addColumn<QString>("fixtureName");
    QTest::newRow("full-601") << QStringLiteral("video-gray-full-601.mp4");
    QTest::newRow("limited-709") << QStringLiteral("video-gray-limited-709.mp4");
}

void MaiFfplayWindowsGeometryTest::sdrGrayPlaybackKeepsExpectedBrightness() {
    QFETCH(QString, fixtureName);
    QCOMPARE(QGuiApplication::platformName(), QStringLiteral("windows"));
    const QString path = QFileInfo(QStringLiteral(MAICHAT_WINDOWS_VIDEO_FIXTURE)).dir().filePath(fixtureName);
    QVERIFY(QFileInfo::exists(path));
    RemoteIMApplication app(QStringLiteral("owned-windows-video-color"), std::make_unique<FakeRemoteIMClient>());
    MainWindow main(app);
    main.setWindowFlag(Qt::WindowStaysOnTopHint);
    main.move(120, 70);
    main.show();
    QVERIFY(QTest::qWaitForWindowExposed(&main));
    main.raise();
    SetForegroundWindow(reinterpret_cast<HWND>(main.winId()));
    QVERIFY(main.openVideoWindow(path, {}));
    QPointer<MaiFfplayVideoDialog> player = MaiFfplayVideoDialog::activeDialog();
    const auto close = qScopeGuard([&] {
        if (player) player->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    });
    QVERIFY(player);
    auto* volume = player->findChild<QSlider*>(QStringLiteral("ffplayVolumeSlider"));
    QVERIFY(volume);
    volume->setValue(0);
    QTRY_VERIFY_WITH_TIMEOUT(player && player->hasPresentedFrame() && player->isVisible(), 10000);
    auto* surface = player->findChild<QWidget*>(QStringLiteral("ffplayVideoSurface"));
    auto* header = player->findChild<QWidget*>(QStringLiteral("ffplayTitleBar"));
    auto* footer = player->findChild<QWidget*>(QStringLiteral("ffplayOverlay"));
    QVERIFY(surface && header && footer);
    player->raise();
    player->activateWindow();
    SetForegroundWindow(reinterpret_cast<HWND>(player->winId()));
    // A submitted frame can precede DWM's visible update. Wait once, then measure
    // the fixed sample; never poll until its brightness happens to pass.
    QTest::qWait(350);
    QVERIFY(player);
    const QRect owner = nativeClient(player);
    const QRect sample(owner.center() - QPoint(31, 31), QSize(64, 64));
    QVERIFY(nativeClient(surface).contains(sample));
    QVERIFY(!nativeClient(header).intersects(sample));
    QVERIFY(!nativeClient(footer).intersects(sample));
    const QRect clientSample = sample.translated(-owner.topLeft());
    // Optional evidence is a file prefix. Without it the same owned pixels are
    // still measured in memory, so ordinary test runs retain the assertion.
    const QString outputBase = qEnvironmentVariable("MAICHAT_WINDOWS_COLOR_OUTPUT");
    const QString output = outputBase.isEmpty() ? QString() :
        outputBase + QStringLiteral("-") + QString::fromLatin1(QTest::currentDataTag());
    if (!output.isEmpty()) QVERIFY(QDir().mkpath(QFileInfo(output).absolutePath()));
    QImage captured;
    QVERIFY2(saveOwnWindow(player, output.isEmpty() ? QString() : output + QStringLiteral(".png"),
                          &captured, clientSample),
             "Color sample must contain only this process's unobscured player and capture must succeed");
    double sums[3]{};
    int minimum[3]{255, 255, 255};
    int maximum[3]{};
    for (int y = 0; y < captured.height(); ++y)
        for (int x = 0; x < captured.width(); ++x) {
            const QRgb pixel = captured.pixel(x, y);
            const int channels[]{qRed(pixel), qGreen(pixel), qBlue(pixel)};
            for (int channel = 0; channel < 3; ++channel) {
                sums[channel] += channels[channel];
                minimum[channel] = qMin(minimum[channel], channels[channel]);
                maximum[channel] = qMax(maximum[channel], channels[channel]);
            }
        }
    const double count = captured.width() * captured.height();
    const double means[]{sums[0] / count, sums[1] / count, sums[2] / count};
    constexpr double expected = 128.0;
    constexpr double tolerance = 8.0;
    const QJsonObject observation{
        {"fixture", path}, {"executable", QCoreApplication::applicationFilePath()},
        {"processId", qint64(GetCurrentProcessId())},
        {"player", geometry(player)}, {"surface", geometry(surface)},
        {"samplePhysical", QJsonObject{{"x", sample.x()}, {"y", sample.y()},
                                        {"width", sample.width()}, {"height", sample.height()}}},
        {"meanRgb", QJsonArray{means[0], means[1], means[2]}},
        {"minRgb", QJsonArray{minimum[0], minimum[1], minimum[2]}},
        {"maxRgb", QJsonArray{maximum[0], maximum[1], maximum[2]}},
        {"expected", expected}, {"tolerance", tolerance}};
    const QByteArray evidence = QJsonDocument(observation).toJson(QJsonDocument::Compact);
    qInfo().noquote() << evidence;
    // Preserve the observed pixels and means before the color assertion, so a
    // failing renderer leaves evidence without encoding the anticipated fault.
    if (!output.isEmpty()) {
        QFile file(output + QStringLiteral(".json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(QJsonDocument(observation).toJson()) > 0);
    }
    for (const double mean : means)
        QVERIFY2(std::abs(mean - expected) <= tolerance, evidence.constData());
    QVERIFY2(std::abs(means[0] - means[1]) <= 3.0 && std::abs(means[1] - means[2]) <= 3.0 &&
                 std::abs(means[0] - means[2]) <= 3.0, evidence.constData());
}

QTEST_MAIN(MaiFfplayWindowsGeometryTest)
#include "MaiFfplayWindowsGeometryTest.moc"
