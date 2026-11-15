// Verify real Windows client/HUD rectangles: Qt-only coordinates can agree
// while the separately mapped native Tool windows are visibly displaced.
#include <QApplication>
#include <QAbstractNativeEventFilter>
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

bool saveOwnWindow(QWidget* widget, const QString& path) {
    if (path.isEmpty()) return false;
    const QRect bounds = nativeClient(widget);
    for (int y = bounds.top(); y <= bounds.bottom(); y += 16)
        for (int x = bounds.left(); x <= bounds.right(); x += 16) {
            DWORD process = 0;
            GetWindowThreadProcessId(WindowFromPoint(POINT{x, y}), &process);
            if (process != GetCurrentProcessId()) return false;
        }
    QImage image(bounds.size(), QImage::Format_RGB32);
    HDC display = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(display);
    HBITMAP bitmap = CreateCompatibleBitmap(display, bounds.width(), bounds.height());
    const auto previous = SelectObject(memory, bitmap);
    BitBlt(memory, 0, 0, bounds.width(), bounds.height(), display, bounds.x(), bounds.y(), SRCCOPY);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = bounds.width();
    info.bmiHeader.biHeight = -bounds.height();
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    SelectObject(memory, previous);
    GetDIBits(memory, bitmap, 0, bounds.height(), image.bits(), &info, DIB_RGB_COLORS);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, display);
    return image.save(path);
}
}  // namespace

class MaiFfplayWindowsGeometryTest : public QObject {
    Q_OBJECT
private slots:
    void preparedPlayerKeepsNativeHudAligned_data();
    void preparedPlayerKeepsNativeHudAligned();
};

void MaiFfplayWindowsGeometryTest::preparedPlayerKeepsNativeHudAligned_data() {
    QTest::addColumn<bool>("prepared");
    QTest::newRow("ordinary-main-prepared") << true;
    QTest::newRow("direct-show") << false;
}

void MaiFfplayWindowsGeometryTest::preparedPlayerKeepsNativeHudAligned() {
    QFETCH(bool, prepared);
    QCOMPARE(QGuiApplication::platformName(), QStringLiteral("windows"));
    const QString path = QStringLiteral(MAICHAT_WINDOWS_VIDEO_FIXTURE);
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

QTEST_MAIN(MaiFfplayWindowsGeometryTest)
#include "MaiFfplayWindowsGeometryTest.moc"
