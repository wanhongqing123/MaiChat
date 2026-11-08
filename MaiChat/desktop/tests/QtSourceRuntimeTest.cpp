// Validate the source SDK's real plugins without a provider request, camera or OS input.
#include <QApplication>
#include <QBuffer>
#include <QImage>
#include <QImageReader>
#include <QLibraryInfo>
#include <QSvgRenderer>
#include <QtTest>

class QtSourceRuntimeTest final : public QObject {
    Q_OBJECT
private slots:
    void pinnedVersion() {
        QCOMPARE(QLibraryInfo::version(), QVersionNumber(6, 12, 0));
    }
    void webpAndSvgPlugins() {
        const auto formats = QImageReader::supportedImageFormats();
        QVERIFY(formats.contains("webp"));
        QVERIFY(formats.contains("svg"));
        QImage original(4, 4, QImage::Format_ARGB32);
        original.fill(Qt::red);
        QByteArray encoded;
        QBuffer output(&encoded);
        QVERIFY(output.open(QIODevice::WriteOnly));
        QVERIFY(original.save(&output, "WEBP"));
        const QImage decoded = QImage::fromData(encoded, "WEBP");
        QCOMPARE(decoded.size(), original.size());
    }
    void svgRendererIsUsable() {
        const QByteArray document("<svg xmlns='http://www.w3.org/2000/svg' width='4' height='4'><rect width='4' height='4' fill='red'/></svg>");
        QSvgRenderer renderer(document);
        QVERIFY(renderer.isValid());
        QCOMPARE(renderer.defaultSize(), QSize(4, 4));
    }
};

QTEST_MAIN(QtSourceRuntimeTest)
#include "QtSourceRuntimeTest.moc"
