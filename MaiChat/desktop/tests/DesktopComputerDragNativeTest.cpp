// Owned local slider only. Exercise the ordinary controller/native input path
// without a model, external website, user account, or CAPTCHA interaction.
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <thread>
#include <json.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "MaiChatTools/DesktopComputerTools.h"

namespace {
class OwnedSlider final : public QWidget {
public:
    QPoint pressed;
    QPoint released;
    int presses = 0;
    int releases = 0;
    int handleX = 90;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor("#263147"));
        painter.setPen(Qt::white);
        painter.drawText(35, 60, QStringLiteral("MaiChat owned coordinate regression"));
        painter.fillRect(50, 145, 620, 10, QColor("#8b9cb8"));
        painter.fillRect(550, 125, 40, 50, QColor("#df8547"));
        painter.fillRect(handleX - 20, 125, 40, 50, QColor("#28af77"));
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) return;
        pressed = event->position().toPoint();
        ++presses;
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (event->buttons() & Qt::LeftButton) {
            handleX = event->position().toPoint().x();
            update();
        }
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) return;
        released = event->position().toPoint();
        ++releases;
    }
};
}  // namespace

class DesktopComputerDragNativeTest : public QObject {
    Q_OBJECT
private slots:
    void controllerDragUsesObservedNativeWindowBounds();
};

void DesktopComputerDragNativeTest::controllerDragUsesObservedNativeWindowBounds() {
    if (!qEnvironmentVariableIsSet("MAICHAT_RUN_NATIVE_DRAG"))
        QSKIP("Explicit owned native pointer regression required");
    QCOMPARE(QGuiApplication::platformName(), QStringLiteral("windows"));
    OwnedSlider slider;
    slider.setWindowTitle(QStringLiteral("MaiChat owned drag coordinates"));
    slider.setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    slider.resize(720, 320);
    slider.move(180, 120);
    slider.show();
    QVERIFY(QTest::qWaitForWindowExposed(&slider));
    const HWND window = reinterpret_cast<HWND>(slider.winId());
    RECT bounds{};
    QVERIFY(GetWindowRect(window, &bounds));
    const auto displays = maiDesktopDisplays();
    QVERIFY2(displays, displays.error().message().c_str());
    const POINT ownPoint{bounds.left + 30, bounds.top + 30};
    DWORD underPointerProcess = 0;
    GetWindowThreadProcessId(WindowFromPoint(ownPoint), &underPointerProcess);
    QCOMPARE(underPointerProcess, GetCurrentProcessId());
    // A native click on our verified owned surface grants ordinary foreground
    // focus; never inject a focus shortcut into whichever app happens to be active.
    DesktopNativeInput activate;
    activate.desktopScope = true;
    for (const auto& display : displays.value())
        if (ownPoint.x >= display.x && ownPoint.x < display.x + display.width && ownPoint.y >= display.y &&
            ownPoint.y < display.y + display.height)
            activate.display = display;
    QVERIFY(activate.display.id != 0);
    activate.kind = DesktopNativeInputKind::Click;
    activate.x = ownPoint.x;
    activate.y = ownPoint.y;
    const auto focused = maiDesktopInput(activate);
    QVERIFY2(!focused, focused.message().c_str());
    QTRY_COMPARE(GetForegroundWindow(), window);
    QTest::qWait(80);
    slider.presses = 0;
    slider.releases = 0;
    DesktopComputerController controller;
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    const QPoint start(90, 150);
    const QPoint end(570, 150);
    QJsonArray observations;
    for (const QPoint origin : {QPoint(180, 120), QPoint(245, 173)}) {
        slider.move(origin);
        slider.handleX = start.x();
        slider.update();
        QTest::qWait(80);
        const auto observed = controller.observe("owned-drag", {}, workspace.path().toStdString(), false,
                                                 std::uint64_t(slider.winId()), true);
        QVERIFY2(observed, observed.error().message().c_str());
        const auto snapshot = nlohmann::json::parse(observed.value().json);
        QVERIFY(GetWindowRect(window, &bounds));
        const double scale = slider.devicePixelRatioF();
        const QImage image(QString::fromStdString(observed.value().imagePath));
        QVERIFY(!image.isNull());
        QCOMPARE(image.size(), QSize(bounds.right - bounds.left, bounds.bottom - bounds.top));
        const auto imageCenter = [&](QRgb color) {
            QRect colored;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    if ((image.pixel(x, y) & 0xffffff) == (color & 0xffffff)) colored |= QRect(x, y, 1, 1);
            return colored;
        };
        const QRect capturedHandle = imageCenter(QColor("#28af77").rgb());
        const QRect capturedTarget = imageCenter(QColor("#df8547").rgb());
        QVERIFY(!capturedHandle.isEmpty() && !capturedTarget.isEmpty());
        const QPointF imageStart = QRectF(capturedHandle).center();
        const QPointF imageEnd = QRectF(capturedTarget).center();
        const QString evidence = qEnvironmentVariable("MAICHAT_NATIVE_DRAG_EVIDENCE");
        if (!evidence.isEmpty()) {
            QVERIFY(QDir().mkpath(evidence));
            QVERIFY(image.save(QDir(evidence).filePath(QStringLiteral("slider-%1.png").arg(origin.x()))));
        }
        DesktopComputerAction request;
        request.action = "drag";
        request.snapshotId = snapshot.at("snapshot_id").get<std::string>();
        request.windowId = std::uint64_t(slider.winId());
        request.xRatio = imageStart.x() / (image.width() - 1);
        request.yRatio = imageStart.y() / (image.height() - 1);
        request.toXRatio = imageEnd.x() / (image.width() - 1);
        request.toYRatio = imageEnd.y() / (image.height() - 1);
        request.durationMs = 200;
        std::atomic<bool> finished{false};
        MaiError error;
        bool dispatched = false;
        std::thread worker([&] {
            const auto result = controller.perform("owned-drag", request);
            if (result)
                dispatched = result.value().dispatched;
            else
                error = result.error();
            finished = true;
        });
        // Pump the real receiver while native input executes on the worker.
        while (!finished) QTest::qWait(5);
        worker.join();
        QVERIFY2(!error, error.message().c_str());
        QVERIFY(dispatched);
        QTRY_VERIFY(slider.presses > 0 && slider.releases == slider.presses);
        // The OS pointer lands on an integer physical pixel before Qt converts
        // the received position back to fractional logical coordinates.
        const QPoint expectedStart(qRound(qRound(imageStart.x()) / scale), qRound(qRound(imageStart.y()) / scale));
        const QPoint expectedEnd(qRound(qRound(imageEnd.x()) / scale), qRound(qRound(imageEnd.y()) / scale));
        QCOMPARE(slider.pressed, expectedStart);
        QCOMPARE(slider.released, expectedEnd);
        QVERIFY((expectedStart - start).manhattanLength() <= 1);
        QVERIFY((expectedEnd - end).manhattanLength() <= 1);
        POINT actualPointer{};
        QVERIFY(GetCursorPos(&actualPointer));
        const QPoint expectedPointer(bounds.left + qRound(imageEnd.x()), bounds.top + qRound(imageEnd.y()));
        QCOMPARE(QPoint(actualPointer.x, actualPointer.y), expectedPointer);
        observations.append(QJsonObject{{"dpi", int(GetDpiForWindow(window))},
                                        {"dpr", scale},
                                        {"windowX", int(bounds.left)},
                                        {"windowY", int(bounds.top)},
                                        {"imageWidth", image.width()},
                                        {"imageHeight", image.height()},
                                        {"pressedX", slider.pressed.x()},
                                        {"pressedY", slider.pressed.y()},
                                        {"releasedX", slider.released.x()},
                                        {"releasedY", slider.released.y()},
                                        {"nativeEndX", int(actualPointer.x)},
                                        {"nativeEndY", int(actualPointer.y)}});
        slider.presses = 0;
        slider.releases = 0;
    }
    qInfo().noquote() << QJsonDocument(observations).toJson(QJsonDocument::Compact);
}

QTEST_MAIN(DesktopComputerDragNativeTest)
#include "DesktopComputerDragNativeTest.moc"
