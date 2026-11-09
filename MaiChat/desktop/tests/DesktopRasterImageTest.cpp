// Test the real presentation/resource boundary without system image decoding.
#include "ui/ComposerTextEdit.h"
#include "ui/DesktopRasterImage.h"

#include <QColorSpace>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextImageFormat>
#include <QtTest>

namespace {
QByteArray tinyWebp() {
    return QByteArray::fromHex("524946461e000000574542505650384c110000002f0fc0020007d0b1b6b4b1ff8188e87f0000");
}

QImage resourceImage(const QVariant& resource) {
    return resource.metaType() == QMetaType::fromType<QPixmap>() ? resource.value<QPixmap>().toImage()
                                                                 : resource.value<QImage>();
}
}  // namespace

class DesktopRasterImageTest : public QObject {
    Q_OBJECT
private slots:
    void memoryPixelsOwnStorage() {
        QByteArray bytes = tinyWebp();
        const auto image = desktopReadRasterBytes(bytes);
        bytes.fill('x');
        QCOMPARE(image.errorCode, 0);
        QCOMPARE(image.info.format, MAI_IMAGE_FORMAT_WEBP);
        QCOMPARE(image.image.size(), QSize(16, 12));
        for (int y = 0; y < 12; ++y)
            for (int x = 0; x < 16; ++x) QCOMPARE(image.image.pixelColor(x, y), QColor(45, 99, 141));
    }

    void composerFileAndDraft() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("\u56fe\u50cf.webp"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(tinyWebp()), qint64(38));
        file.close();
        ComposerTextEdit source;
        QTextImageFormat image;
        image.setName(path);  // Keep attachment identity through saved drafts.
        image.setWidth(16);
        image.setHeight(12);
        source.textCursor().insertImage(image);
        const QTextDocumentFragment draft(source.document());
        ComposerTextEdit restored;
        restored.textCursor().insertFragment(draft);
        const QImage pixels = resourceImage(restored.document()->resource(QTextDocument::ImageResource, QUrl(path)));
        QCOMPARE(pixels.size(), QSize(16, 12));
        QCOMPARE(pixels.pixelColor(5, 7), QColor(45, 99, 141));
        QCOMPARE(restored.document()->begin().begin().fragment().charFormat().toImageFormat().name(), path);
    }

    void composerMemoryData() {
        ComposerTextEdit editor;
        const QUrl url(QStringLiteral("data:image/webp;base64,") + QString::fromLatin1(tinyWebp().toBase64()));
        const QImage pixels = resourceImage(editor.document()->resource(QTextDocument::ImageResource, url));
        QCOMPARE(pixels.size(), QSize(16, 12));
        QCOMPARE(pixels.pixelColor(0, 0), QColor(45, 99, 141));
    }

    void rejectedResourceStopsProviders_data() {
        QTest::addColumn<QUrl>("url");
        QTest::newRow("remote") << QUrl(QStringLiteral("https://127.0.0.1/never-request.png"));
        QTest::newRow("broken-data") << QUrl(QStringLiteral("data:image/png;base64,bm90LXBpeGVscw=="));
        QTest::newRow("missing-file") << QUrl::fromLocalFile(
            QStringLiteral("/does-not-exist-owned-raster-fixture.webp"));
    }

    void rejectedResourceStopsProviders() {
        QFETCH(QUrl, url);
        ComposerTextEdit editor;
        int providerCalls = 0;
        editor.document()->setResourceProvider([&](const QUrl&) -> QVariant {
            ++providerCalls;
            return QImage(2, 2, QImage::Format_RGBA8888);
        });
        const QVariant value = editor.document()->resource(QTextDocument::ImageResource, url);
        QCOMPARE(providerCalls, 0);
        QVERIFY(value.isValid());
        QCOMPARE(value.metaType(), QMetaType::fromType<QImage>());
        QVERIFY(value.value<QImage>().isNull());
    }

    void rawClipboardPixelsRemainPresentation() {
        ComposerTextEdit editor;
        QImage raw(3, 2, QImage::Format_RGBA8888);
        raw.fill(QColor(17, 99, 231, 87));
        const QUrl url(QStringLiteral("owned-pixels://fixture"));
        editor.document()->addResource(QTextDocument::ImageResource, url, raw);
        QCOMPARE(resourceImage(editor.document()->resource(QTextDocument::ImageResource, url)), raw);
    }
};

QTEST_MAIN(DesktopRasterImageTest)
#include "DesktopRasterImageTest.moc"
