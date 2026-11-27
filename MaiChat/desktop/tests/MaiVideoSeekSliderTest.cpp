// 自有时间轴输入；只验证坐标/提示/拖动事件，不以此冒作真实解码预览验收。
#include "ui/MaiVideoSeekSlider.h"
#include <QToolTip>
#include <QtTest/QtTest>

class MaiVideoSeekSliderTest final : public QObject {
    Q_OBJECT
  private slots:
    void hoverShowsPointerTimeWithoutSeeking() {
        MaiVideoSeekSlider slider;
        slider.resize(600, 40);
        slider.setRange(0, 60000);
        slider.setDurationUs(60000000);
        slider.setValue(0);
        slider.show();
        slider.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&slider));
        QSignalSpy changed(&slider, &QSlider::valueChanged);
        QMouseEvent move(QEvent::MouseMove, QPointF(300, 20),
                         QPointF(slider.mapToGlobal(QPoint(300, 20))), Qt::NoButton, Qt::NoButton,
                         Qt::NoModifier);
        QCoreApplication::sendEvent(&slider, &move);
        QTRY_COMPARE(QToolTip::text(), QStringLiteral("0:30"));
        QCOMPARE(slider.value(), 0);
        QCOMPARE(changed.count(), 0);
        QEvent leave(QEvent::Leave);
        QCoreApplication::sendEvent(&slider, &leave);
    }
    void dragEmitsPositionsBeforeRelease() {
        MaiVideoSeekSlider slider;
        slider.resize(600, 40);
        slider.setRange(0, 60000);
        slider.setDurationUs(60000000);
        slider.show();
        QSignalSpy moved(&slider, &QSlider::sliderMoved);
        QTest::mousePress(&slider, Qt::LeftButton, Qt::NoModifier, QPoint(150, 20));
        QTest::mouseMove(&slider, QPoint(450, 20));
        QVERIFY(slider.isSliderDown());
        QVERIFY(moved.count() >= 2);
        QVERIFY(slider.sliderPosition() > 40000);
        QTest::mouseRelease(&slider, Qt::LeftButton, Qt::NoModifier, QPoint(450, 20));
        QVERIFY(!slider.isSliderDown());
        QVERIFY(slider.sliderPosition() > 40000);
    }
};
QTEST_MAIN(MaiVideoSeekSliderTest)
#include "MaiVideoSeekSliderTest.moc"
