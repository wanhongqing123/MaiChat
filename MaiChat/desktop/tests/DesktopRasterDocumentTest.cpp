// Exercise the real HTML viewer and PDF resource policy without network access.
// Include the PDF implementation once to inspect its private document boundary;
// the target must not compile DesktopPdfRenderer.cpp a second time.
#include "../src/MaiChatTools/DesktopPdfRenderer.cpp"

#include <QtTest/QtTest>

#include <QApplication>
#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QRegularExpression>
#include <QScreen>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QVector>
#include <QtMath>

#include <utility>

#include "ui/FilePreviewDialog.h"

namespace {

QByteArray ownedRaster(const QString& format) {
    if (format == QStringLiteral("webp")) {
        // Existing 38-byte, 16x12 owned regression fixture, not a supplier image.
        return QByteArray::fromHex("524946461e000000574542505650384c110000002f0fc0020007d0b1b6b4b1ff8188e87f0000");
    }
    QImage image(16, 12, QImage::Format_RGBA8888);
    image.fill(format == QStringLiteral("png") ? QColor(255, 0, 0, 117) : QColor(45, 99, 141));
    QByteArray bytes;
    QBuffer output(&bytes);
    if (!output.open(QIODevice::WriteOnly) ||
        !image.save(&output, format == QStringLiteral("jpeg") ? "JPEG" : "PNG", 100))
        return {};
    return bytes;
}

QByteArray ownedSvg() {
    return "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='12' viewBox='0 0 16 12'>"
           "<rect width='16' height='12' fill='#2d638d'/></svg>";
}

QUrl dataResource(const QString& format, const QByteArray& bytes, bool base64 = true) {
    const QByteArray header = "data:image/" + format.toLatin1() + (base64 ? ";base64," : ",");
    return QUrl::fromEncoded(header + (base64 ? bytes.toBase64() : bytes.toPercentEncoding()));
}

QString htmlImage(const QUrl& url) {
    return QStringLiteral("<p>Owned raster fixture</p><img src=\"%1\">")
        .arg(QString::fromLatin1(url.toEncoded()).toHtmlEscaped());
}

class DefaultProviderGuard {
public:
    explicit DefaultProviderGuard(QTextDocument::ResourceProvider provider)
        : previous_(QTextDocument::defaultResourceProvider()) {
        QTextDocument::setDefaultResourceProvider(std::move(provider));
    }
    ~DefaultProviderGuard() {
        QTextDocument::setDefaultResourceProvider(previous_);
    }

private:
    QTextDocument::ResourceProvider previous_;
};

void addFormats() {
    QTest::addColumn<QString>("format");
    QTest::newRow("owned-38-byte-webp") << QStringLiteral("webp");
    QTest::newRow("transparent-png") << QStringLiteral("png");
    QTest::newRow("opaque-jpeg") << QStringLiteral("jpeg");
}

void verifyImagePixels(const QImage& image, const QString& format) {
    QVERIFY(!image.isNull());
    QCOMPARE(image.size(), QSize(16, 12));
    const QColor expected = format == QStringLiteral("png") ? QColor(255, 0, 0, 117) : QColor(45, 99, 141);
    const int tolerance = format == QStringLiteral("jpeg") ? 3 : 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = image.pixelColor(x, y);
            QVERIFY(qAbs(color.red() - expected.red()) <= tolerance);
            QVERIFY(qAbs(color.green() - expected.green()) <= tolerance);
            QVERIFY(qAbs(color.blue() - expected.blue()) <= tolerance);
            QCOMPARE(color.alpha(), expected.alpha());
        }
    }
}

void verifyPixels(const QVariant& resource, const QString& format) {
    // Returning compressed bytes or a GUI QPixmap would let Qt decode or own
    // the codec boundary. Both documents must instead return owning QImage.
    QCOMPARE(resource.metaType(), QMetaType::fromType<QImage>());
    verifyImagePixels(resource.value<QImage>(), format);
}

bool pdfContainsFixtureImage(const QByteArray& bytes) {
    const QRegularExpression objects(QStringLiteral(R"((?:\A|\n)\d+\s+\d+\s+obj\s*<<(.*?)>>)"),
                                     QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpression imageType(QStringLiteral(R"(/Subtype\s*/Image\b)"));
    const QRegularExpression width(QStringLiteral(R"(/Width\s+16\b)"));
    const QRegularExpression height(QStringLiteral(R"(/Height\s+12\b)"));
    auto matches = objects.globalMatch(QString::fromLatin1(bytes));
    while (matches.hasNext()) {
        const auto dictionary = matches.next().captured(1);
        if (imageType.match(dictionary).hasMatch() && width.match(dictionary).hasMatch() &&
            height.match(dictionary).hasMatch())
            return true;
    }
    return false;
}

void verifyRejected(const QVariant& resource) {
    // A valid, typed null blocks QTextDocument's provider fallback and the
    // QTextImageHandler retry that would otherwise read/decode the URL itself.
    QCOMPARE(resource.metaType(), QMetaType::fromType<QImage>());
    QVERIFY(resource.value<QImage>().isNull());
}

}  // namespace

class DesktopRasterDocumentTest final : public QObject {
    Q_OBJECT

private slots:
    void filePreviewLocalPixels_data() {
        addFormats();
    }
    void filePreviewLocalPixels();
    void filePreviewDataPixels_data() {
        addFormats();
    }
    void filePreviewDataPixels();
    void filePreviewPercentEncodedPng();
    void initialHeightUsesTheSameRasterPolicyAsTheVisibleDocument();
    void filePreviewRejectsExternalAndInvalidResources();
    void pdfDataPixels_data() {
        addFormats();
    }
    void pdfDataPixels();
    void pdfPercentEncodedPng();
    void simpleSvgPresentationRemainsAvailable_data();
    void simpleSvgPresentationRemainsAvailable();
    void selfContainedSvgReturnsPixels_data();
    void selfContainedSvgReturnsPixels();
    void filePreviewLocalSvgReturnsPixels();
    void resourcePixelsOutliveDocumentAndInput_data();
    void resourcePixelsOutliveDocumentAndInput();
    void bothDocumentsRejectSvgSpoofingAndExternalReferences();
    void pdfRejectsLocalAndExternalResources();
    void publicPdfRendererEmbedsOwnedRaster_data() {
        addFormats();
    }
    void publicPdfRendererEmbedsOwnedRaster();
};

void DesktopRasterDocumentTest::filePreviewLocalPixels() {
    QFETCH(QString, format);
    const QByteArray bytes = ownedRaster(format);
    QVERIFY(!bytes.isEmpty());
    if (format == QStringLiteral("webp")) QCOMPARE(bytes.size(), 38);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("owned raster.") + format);
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), qint64(bytes.size()));
    file.close();
    const QUrl url = QUrl::fromLocalFile(path);
    FilePreviewDialog preview(QStringLiteral("owned.html"), htmlImage(url));
    auto* browser = preview.findChild<QTextBrowser*>(QStringLiteral("filePreviewContent"));
    QVERIFY(browser);
    verifyPixels(browser->document()->resource(QTextDocument::ImageResource, url), format);
}

void DesktopRasterDocumentTest::filePreviewDataPixels() {
    QFETCH(QString, format);
    const QUrl url = dataResource(format, ownedRaster(format));
    FilePreviewDialog preview(QStringLiteral("owned.html"), htmlImage(url));
    auto* browser = preview.findChild<QTextBrowser*>(QStringLiteral("filePreviewContent"));
    QVERIFY(browser);
    verifyPixels(browser->document()->resource(QTextDocument::ImageResource, url), format);
}

void DesktopRasterDocumentTest::filePreviewPercentEncodedPng() {
    const QUrl url = dataResource(QStringLiteral("png"), ownedRaster(QStringLiteral("png")), false);
    FilePreviewDialog preview(QStringLiteral("owned.html"), htmlImage(url));
    auto* browser = preview.findChild<QTextBrowser*>(QStringLiteral("filePreviewContent"));
    QVERIFY(browser);
    verifyPixels(browser->document()->resource(QTextDocument::ImageResource, url), QStringLiteral("png"));
}

void DesktopRasterDocumentTest::initialHeightUsesTheSameRasterPolicyAsTheVisibleDocument() {
    const auto html = [](const QUrl& url) {
        // Width alone preserves the decoder's intrinsic aspect ratio: the
        // real 16x12 fixture is 240px tall, a 16x16 broken-image icon is 320px.
        return QStringLiteral("<img width='320' src=\"%1\">").arg(QString::fromLatin1(url.toEncoded()).toHtmlEscaped());
    };
    FilePreviewDialog png(QStringLiteral("owned.html"),
                          html(dataResource(QStringLiteral("png"), ownedRaster(QStringLiteral("png")))));
    FilePreviewDialog webp(QStringLiteral("owned.html"),
                           html(dataResource(QStringLiteral("webp"), ownedRaster(QStringLiteral("webp")))));
    QVERIFY(png.height() > png.minimumHeight() + 20);
    const QScreen* screen = QApplication::primaryScreen();
    QVERIFY(screen);
    QVERIFY(png.height() < qFloor(screen->availableGeometry().height() * 0.90) - 20);
    QCOMPARE(webp.height(), png.height());
}

void DesktopRasterDocumentTest::filePreviewRejectsExternalAndInvalidResources() {
    int providerCalls = 0;
    QImage bait(2, 2, QImage::Format_RGBA8888);
    bait.fill(Qt::magenta);
    DefaultProviderGuard guard([&](const QUrl&) -> QVariant {
        ++providerCalls;
        return bait;
    });
    for (const auto& url :
         {QUrl(QStringLiteral("https://127.0.0.1:9/never-request.png")),
          QUrl(QStringLiteral("http://127.0.0.1:9/never-request.png")),
          QUrl(QStringLiteral("data:image/png;base64,not-valid-base64!")),
          QUrl(QStringLiteral("data:image/webp;base64,Ym9ndXM=")),
          QUrl::fromEncoded("data:text/plain;base64," + ownedRaster(QStringLiteral("png")).toBase64())}) {
        FilePreviewDialog preview(QStringLiteral("owned.html"), htmlImage(url));
        auto* browser = preview.findChild<QTextBrowser*>(QStringLiteral("filePreviewContent"));
        QVERIFY(browser);
        verifyRejected(browser->document()->resource(QTextDocument::ImageResource, url));
    }
    // The constructor's size-measurement document must obey the same policy.
    QCOMPARE(providerCalls, 0);
}

void DesktopRasterDocumentTest::pdfDataPixels() {
    QFETCH(QString, format);
    const QUrl url = dataResource(format, ownedRaster(format));
    SelfContainedPdfDocument document;
    document.setHtml(htmlImage(url));
    verifyPixels(document.resource(QTextDocument::ImageResource, url), format);
}

void DesktopRasterDocumentTest::pdfPercentEncodedPng() {
    const QUrl url = dataResource(QStringLiteral("png"), ownedRaster(QStringLiteral("png")), false);
    SelfContainedPdfDocument document;
    document.setHtml(htmlImage(url));
    verifyPixels(document.resource(QTextDocument::ImageResource, url), QStringLiteral("png"));
}

void DesktopRasterDocumentTest::simpleSvgPresentationRemainsAvailable_data() {
    QTest::addColumn<bool>("pdf");
    QTest::newRow("html-existing-simple-svg") << false;
    QTest::newRow("pdf-existing-simple-svg") << true;
}

void DesktopRasterDocumentTest::simpleSvgPresentationRemainsAvailable() {
    QFETCH(bool, pdf);
    const auto url = dataResource(QStringLiteral("svg+xml"), ownedSvg());
    QVariant resource;
    if (pdf) {
        SelfContainedPdfDocument document;
        document.setHtml(htmlImage(url));
        resource = document.resource(QTextDocument::ImageResource, url);
    } else {
        FilePreviewDialog preview(QStringLiteral("owned.html"), htmlImage(url));
        auto* browser = preview.findChild<QTextBrowser*>(QStringLiteral("filePreviewContent"));
        QVERIFY(browser);
        resource = browser->document()->resource(QTextDocument::ImageResource, url);
    }
    // This compatibility row intentionally accepts the old QPixmap result.
    // Its actual pixels establish the original SVG behavior before migration.
    verifyImagePixels(resource.value<QImage>(), QStringLiteral("svg"));
}

void DesktopRasterDocumentTest::selfContainedSvgReturnsPixels_data() {
    simpleSvgPresentationRemainsAvailable_data();
}

void DesktopRasterDocumentTest::selfContainedSvgReturnsPixels() {
    QFETCH(bool, pdf);
    const auto url = dataResource(QStringLiteral("svg+xml"), ownedSvg());
    if (pdf) {
        SelfContainedPdfDocument document;
        document.setHtml(htmlImage(url));
        verifyPixels(document.resource(QTextDocument::ImageResource, url), QStringLiteral("svg"));
    } else {
        FilePreviewDialog preview(QStringLiteral("owned.html"), htmlImage(url));
        auto* browser = preview.findChild<QTextBrowser*>(QStringLiteral("filePreviewContent"));
        QVERIFY(browser);
        verifyPixels(browser->document()->resource(QTextDocument::ImageResource, url), QStringLiteral("svg"));
    }
}

void DesktopRasterDocumentTest::filePreviewLocalSvgReturnsPixels() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("owned vector.svg"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(ownedSvg()), qint64(ownedSvg().size()));
    file.close();
    const auto url = QUrl::fromLocalFile(path);
    FilePreviewDialog preview(QStringLiteral("owned.html"), htmlImage(url));
    auto* browser = preview.findChild<QTextBrowser*>(QStringLiteral("filePreviewContent"));
    QVERIFY(browser);
    verifyPixels(browser->document()->resource(QTextDocument::ImageResource, url), QStringLiteral("svg"));
}

void DesktopRasterDocumentTest::resourcePixelsOutliveDocumentAndInput_data() {
    QTest::addColumn<QString>("format");
    QTest::addColumn<bool>("pdf");
    for (const auto* format : {"webp", "png", "jpeg"}) {
        QTest::newRow(qPrintable(QStringLiteral("html-%1").arg(QString::fromLatin1(format))))
            << QString::fromLatin1(format) << false;
        QTest::newRow(qPrintable(QStringLiteral("pdf-%1").arg(QString::fromLatin1(format))))
            << QString::fromLatin1(format) << true;
    }
}

void DesktopRasterDocumentTest::resourcePixelsOutliveDocumentAndInput() {
    QFETCH(QString, format);
    QFETCH(bool, pdf);
    QImage retained;
    {
        QByteArray input = ownedRaster(format);
        const auto url = dataResource(format, input);
        QVariant resource;
        if (pdf) {
            SelfContainedPdfDocument document;
            document.setHtml(htmlImage(url));
            resource = document.resource(QTextDocument::ImageResource, url);
        } else {
            FilePreviewDialog preview(QStringLiteral("owned.html"), htmlImage(url));
            auto* browser = preview.findChild<QTextBrowser*>(QStringLiteral("filePreviewContent"));
            QVERIFY(browser);
            resource = browser->document()->resource(QTextDocument::ImageResource, url);
        }
        QCOMPARE(resource.metaType(), QMetaType::fromType<QImage>());
        retained = resource.value<QImage>();
        input.fill('\0');
    }
    // Both the document and compressed input are gone. Reuse native decode
    // allocations and heap blocks before inspecting the retained pixel owner.
    QVector<QByteArray> allocations;
    for (int index = 0; index < 96; ++index) allocations.append(QByteArray(16 * 12 * 4, char(index)));
    for (int index = 0; index < 8; ++index) {
        SelfContainedPdfDocument replacement;
        const auto url = dataResource(QStringLiteral("png"), ownedRaster(QStringLiteral("png")));
        const auto resource = replacement.resource(QTextDocument::ImageResource, url);
        QVERIFY(!resource.value<QImage>().isNull());
    }
    verifyImagePixels(retained, format);
}

void DesktopRasterDocumentTest::bothDocumentsRejectSvgSpoofingAndExternalReferences() {
    int providerCalls = 0;
    DefaultProviderGuard guard([&](const QUrl&) -> QVariant {
        ++providerCalls;
        return QImage(2, 2, QImage::Format_RGBA8888);
    });
    const QByteArray imageReference =
        "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='12'>"
        "<image href='file:///never-read-owned-external.png' width='16' height='12'/></svg>";
    const QByteArray useReference =
        "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' "
        "width='16' height='12'><use xlink:href='https://127.0.0.1:9/never-request.svg#shape'/></svg>";
    for (const auto& url : {dataResource(QStringLiteral("png"), ownedSvg()),
                            dataResource(QStringLiteral("svg+xml"), ownedRaster(QStringLiteral("png"))),
                            dataResource(QStringLiteral("svg+xml"), imageReference),
                            dataResource(QStringLiteral("svg+xml"), useReference)}) {
        SelfContainedPdfDocument document;
        document.setHtml(htmlImage(url));
        verifyRejected(document.resource(QTextDocument::ImageResource, url));
        FilePreviewDialog preview(QStringLiteral("owned.html"), htmlImage(url));
        auto* browser = preview.findChild<QTextBrowser*>(QStringLiteral("filePreviewContent"));
        QVERIFY(browser);
        verifyRejected(browser->document()->resource(QTextDocument::ImageResource, url));
    }
    QCOMPARE(providerCalls, 0);
}

void DesktopRasterDocumentTest::pdfRejectsLocalAndExternalResources() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("must-not-read.png"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray bytes = ownedRaster(QStringLiteral("png"));
    QCOMPARE(file.write(bytes), qint64(bytes.size()));
    file.close();
    int providerCalls = 0;
    QImage bait(2, 2, QImage::Format_RGBA8888);
    bait.fill(Qt::magenta);
    DefaultProviderGuard guard([&](const QUrl&) -> QVariant {
        ++providerCalls;
        return bait;
    });
    for (const auto& url : {QUrl::fromLocalFile(path), QUrl(QStringLiteral("owned-relative.png")),
                            QUrl(QStringLiteral("https://127.0.0.1:9/never-request.png")),
                            QUrl(QStringLiteral("http://127.0.0.1:9/never-request.png")),
                            QUrl(QStringLiteral("qrc:/maichat/app-icon.png")),
                            QUrl(QStringLiteral("data:image/png;base64,not-valid-base64!"))}) {
        SelfContainedPdfDocument document;
        document.setHtml(htmlImage(url));
        verifyRejected(document.resource(QTextDocument::ImageResource, url));
    }
    QCOMPARE(providerCalls, 0);
}

void DesktopRasterDocumentTest::publicPdfRendererEmbedsOwnedRaster() {
    QFETCH(QString, format);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto url = dataResource(format, ownedRaster(format));
    const QString path = directory.filePath(QStringLiteral("owned.pdf"));
    const auto result = renderDesktopPdf(htmlImage(url).toUtf8().toStdString(), path.toUtf8().toStdString(), nullptr);
    QVERIFY2(!result.hasError(), result.error().message().c_str());
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    QVERIFY(bytes.startsWith("%PDF-"));
    QVERIFY(bytes.contains("%%EOF"));
    QVERIFY(pdfContainsFixtureImage(bytes));
}

QTEST_MAIN(DesktopRasterDocumentTest)
#include "DesktopRasterDocumentTest.moc"
