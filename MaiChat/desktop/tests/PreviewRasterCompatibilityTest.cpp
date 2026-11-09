#include <QtTest>

#include <QFile>
#include <QImage>
#include <QTemporaryDir>

#include "ui/ImagePreviewDialog.h"

class PreviewRasterCompatibilityTest : public QObject {
    Q_OBJECT
private slots:
    void filePixels_data() {
        QTest::addColumn<QString>("extension");
        QTest::newRow("owned-38-byte-webp") << QStringLiteral("webp");
        QTest::newRow("transparent-png") << QStringLiteral("png");
        QTest::newRow("opaque-jpeg") << QStringLiteral("jpg");
    }

    void filePixels() {
        QFETCH(QString, extension);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("owned.") + extension);
        // Saturated PNG channels remain exact through QPixmap's premultiplied
        // storage, so this checks alpha without testing quantization roundoff.
        const QColor expected = extension == QStringLiteral("png")
            ? QColor(255, 0, 0, 117) : QColor(45, 99, 141, 255);
        if (extension == QStringLiteral("webp")) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            const QByteArray bytes = QByteArray::fromHex(
                "524946461e000000574542505650384c110000002f0fc0020007d0b1b6b4b1ff8188e87f0000");
            QCOMPARE(bytes.size(), 38);
            QCOMPARE(file.write(bytes), qint64(bytes.size()));
        } else {
            QImage pixels(16, 12, QImage::Format_RGBA8888);
            pixels.fill(expected);
            QVERIFY(pixels.save(path, extension == QStringLiteral("jpg") ? "JPEG" : "PNG", 100));
        }
        ImagePreviewDialog preview(path);
        preview.setWindowTitle(QStringLiteral("Owned bitmap regression fixture"));
        preview.show();
        QTRY_VERIFY_WITH_TIMEOUT(!preview.sourceImage().isNull(), 1500);
        const QImage actual = preview.sourceImage().toImage();
        QCOMPARE(actual.size(), QSize(16, 12));
        const int tolerance = extension == QStringLiteral("jpg") ? 3 : 0;
        for (int y = 0; y < actual.height(); ++y) {
            for (int x = 0; x < actual.width(); ++x) {
                const QColor color = actual.pixelColor(x, y);
                QVERIFY(qAbs(color.red() - expected.red()) <= tolerance);
                QVERIFY(qAbs(color.green() - expected.green()) <= tolerance);
                QVERIFY(qAbs(color.blue() - expected.blue()) <= tolerance);
                QCOMPARE(color.alpha(), expected.alpha());
            }
        }
        preview.close();
    }
};

QTEST_MAIN(PreviewRasterCompatibilityTest)
#include "PreviewRasterCompatibilityTest.moc"
