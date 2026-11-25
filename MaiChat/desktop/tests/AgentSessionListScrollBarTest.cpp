// Verify the production creation-center sidebar with owned in-memory sessions.
// No provider, account database, clipboard or desktop capture participates.
#include <QApplication>
#include <QDir>
#include <QImage>
#include <QJsonArray>
#include <QListWidget>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStyleOptionSlider>
#include <QTemporaryDir>
#include <QWheelEvent>
#include <QtTest>

#include "agent/AgentController.h"
#include "agent/AgentSessionList.h"
#include "ui/UiZoom.h"

namespace {
class IdleModel final : public MaiModelClient {
public:
    MaiWireApi wireApi() const override { return MaiWireApi::Responses; }
    MaiError stream(const MaiModelRequest&, const MaiStreamSink&, const std::atomic<bool>&) override {
        return {MaiErrorCode::Internal, "The sidebar fixture must not invoke a model."};
    }
};

struct OwnedSidebar {
    QTemporaryDir workspace;
    AgentController controller{std::make_unique<IdleModel>(), QString()};
    AgentSessionList sidebar{controller};

    void populate() {
        for (int index = 0; index < 3; ++index)
            controller.createSession(workspace.path(), QStringLiteral("Owned local task %1").arg(index + 1));
        const QJsonObject device{{QStringLiteral("device_slot"), QStringLiteral("ios_phone")},
                                 {QStringLiteral("device_id"), QStringLiteral("owned-phone")},
                                 {QStringLiteral("device_name"), QStringLiteral("iPhone fixture")},
                                 {QStringLiteral("online"), true}};
        sidebar.setRemoteDevices(QJsonArray{device});
        QJsonArray sessions;
        for (int index = 0; index < 22; ++index) {
            auto session = device;
            session.insert(QStringLiteral("id"), QStringLiteral("owned_remote_%1").arg(index));
            session.insert(QStringLiteral("title"), QStringLiteral("Owned phone conversation %1").arg(index + 1));
            sessions.append(session);
        }
        sidebar.setRemoteSessions(sessions);
    }
};

QStyleOptionSlider scrollOption(QScrollBar* bar) {
    QStyleOptionSlider option;
    option.initFrom(bar);
    option.orientation = bar->orientation();
    option.minimum = bar->minimum();
    option.maximum = bar->maximum();
    option.sliderPosition = bar->sliderPosition();
    option.sliderValue = bar->value();
    option.pageStep = bar->pageStep();
    option.singleStep = bar->singleStep();
    return option;
}

QRect subcontrol(QScrollBar* bar, QStyle::SubControl control) {
    const auto option = scrollOption(bar);
    return bar->style()->subControlRect(QStyle::CC_ScrollBar, &option, control, bar);
}

QColor renderedColor(QScrollBar* bar, const QPoint& logicalPoint) {
    const auto image = bar->grab().toImage();
    return image.pixelColor((QPointF(logicalPoint) * image.devicePixelRatio()).toPoint());
}

bool capture(AgentSessionList& sidebar, const QString& name) {
    const auto directory = qEnvironmentVariable("MAICHAT_SIDEBAR_CAPTURE_DIR");
    if (directory.isEmpty()) return true;
    return QDir().mkpath(directory) && sidebar.grab().save(QDir(directory).filePath(name + QStringLiteral(".png")));
}
}  // namespace

class AgentSessionListScrollBarTest final : public QObject {
    Q_OBJECT
private slots:
    void themedScrollbarKeepsNativeScrolling_data() {
        QTest::addColumn<int>("width");
        QTest::addColumn<int>("height");
        QTest::addColumn<qreal>("zoom");
        QTest::newRow("long-list") << 300 << 700 << qreal(1);
        QTest::newRow("narrow-list") << 230 << 360 << qreal(1);
        QTest::newRow("zoomed-list") << 360 << 650 << qreal(1.5);
    }

    void themedScrollbarKeepsNativeScrolling() {
        QFETCH(int, width);
        QFETCH(int, height);
        QFETCH(qreal, zoom);
        const auto previousZoom = UiZoom::factor();
        struct RestoreZoom {
            qreal value;
            ~RestoreZoom() { UiZoom::setFactor(value); }
        } restore{previousZoom};
        UiZoom::setFactor(zoom);
        QScrollBar peer(Qt::Vertical);
        peer.resize(30, 240);
        peer.setRange(0, 100);
        peer.show();
        QVERIFY(QTest::qWaitForWindowExposed(&peer));
        const QRect peerAddLine = subcontrol(&peer, QStyle::SC_ScrollBarAddLine);
        const QRect peerSubLine = subcontrol(&peer, QStyle::SC_ScrollBarSubLine);
        const int peerExtent = peer.style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, &peer);
        OwnedSidebar fixture;
        QVERIFY(fixture.workspace.isValid());
        fixture.populate();
        fixture.sidebar.resize(width, height);
        fixture.sidebar.show();
        QVERIFY(QTest::qWaitForWindowExposed(&fixture.sidebar));
        auto* list = fixture.sidebar.findChild<QListWidget*>();
        QVERIFY(list);
        auto* bar = list->verticalScrollBar();
        QTRY_VERIFY(bar->isVisible() && bar->maximum() > 0);
        QTest::mouseMove(list->viewport(), list->viewport()->rect().center());
        QCoreApplication::processEvents();
        qInfo() << "[owned-sidebar]" << QTest::currentDataTag() << "width" << bar->width()
                << "add-line" << subcontrol(bar, QStyle::SC_ScrollBarAddLine)
                << "sub-line" << subcontrol(bar, QStyle::SC_ScrollBarSubLine)
                << "range" << bar->maximum();
        QVERIFY(capture(fixture.sidebar, QString::fromLatin1(QTest::currentDataTag())));
        QCOMPARE(bar->width(), UiZoom::s(8));
        QVERIFY(subcontrol(bar, QStyle::SC_ScrollBarAddLine).isEmpty());
        QVERIFY(subcontrol(bar, QStyle::SC_ScrollBarSubLine).isEmpty());
        const QRect handle = subcontrol(bar, QStyle::SC_ScrollBarSlider);
        QVERIFY(handle.height() >= UiZoom::s(24));
        QCOMPARE(renderedColor(bar, handle.center()), QColor(QStringLiteral("#d3dae4")));
        // Rounded thumb corners expose the transparent track rather than a square block.
        QVERIFY(renderedColor(bar, handle.topLeft()) != QColor(QStringLiteral("#d3dae4")));
        const QPoint track(bar->width() / 2, handle.bottom() + UiZoom::s(12));
        QCOMPARE(renderedColor(bar, track).alpha(), 0);
        QTest::mouseMove(bar, handle.center());
        QTRY_COMPARE(renderedColor(bar, handle.center()), QColor(QStringLiteral("#b9c3d1")));
        QVERIFY(capture(fixture.sidebar, QString::fromLatin1(QTest::currentDataTag()) + QStringLiteral("-hover")));

        auto* create = fixture.sidebar.findChild<QPushButton*>(QStringLiteral("agentLocalCreateSessionButton"));
        QVERIFY(create && create->isVisible());
        QVERIFY(list->visualItemRect(list->item(0)).contains(create->geometry()));
        QVERIFY(list->viewport()->rect().contains(create->geometry()));
        QSignalSpy local(&fixture.sidebar, &AgentSessionList::newSessionRequested);
        QSignalSpy remote(&fixture.sidebar, &AgentSessionList::remoteCreateRequested);
        QTest::mouseClick(create, Qt::LeftButton);
        QCOMPARE(local.count(), 1);
        const auto phoneHeader = list->visualItemRect(list->item(4));
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier,
                          QPoint(create->geometry().center().x(), phoneHeader.center().y()));
        QCOMPARE(remote.count(), 1);
        QCOMPARE(list->count(), 27);

        // Transparent page regions still keep native click-to-page behavior.
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, track);
        QTRY_VERIFY(bar->value() > 0);
        bar->setValue(0);
        const QPoint point = list->viewport()->rect().center();
        QWheelEvent wheel(QPointF(point), QPointF(list->viewport()->mapToGlobal(point)),
                          QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(list->viewport(), &wheel);
        QTRY_VERIFY(bar->value() > 0);
        bar->setValue(bar->maximum());
        QTRY_VERIFY(!create->isVisible());
        const auto last = list->visualItemRect(list->item(list->count() - 1));
        QVERIFY(list->viewport()->rect().intersects(last));
        bar->setValue(0);
        QTRY_VERIFY(create->isVisible());
        QVERIFY(list->visualItemRect(list->item(0)).contains(create->geometry()));

        // The sidebar stylesheet must not change unrelated application scrollbars.
        QCOMPARE(subcontrol(&peer, QStyle::SC_ScrollBarAddLine), peerAddLine);
        QCOMPARE(subcontrol(&peer, QStyle::SC_ScrollBarSubLine), peerSubLine);
        QCOMPARE(peer.style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, &peer), peerExtent);
    }
};

QTEST_MAIN(AgentSessionListScrollBarTest)
#include "AgentSessionListScrollBarTest.moc"
