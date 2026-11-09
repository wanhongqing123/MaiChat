// Content-based file/memory image contracts, including independent orientation fixtures.
#include "MaiImageDecode.h"

#include <atomic>
#include <thread>

#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QtTest>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

namespace {

QByteArray fixture(const QString& name) {
    QFile file(QStringLiteral(MAI_RASTER_FIXTURE_DIR) + QLatin1Char('/') + name);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QByteArray tinyWebp() {
    return QByteArray::fromHex("524946461e000000574542505650384c110000002f0fc0020007d0b1b6b4b1ff8188e87f0000");
}

QByteArray encode(const char* format, const QColor& color) {
    QImage pixels(16, 12, QImage::Format_RGBA8888);
    pixels.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    return pixels.save(&buffer, format, 100) ? bytes : QByteArray();
}

struct Image {
    MaiImageDecodeResult value{};
    ~Image() {
        maiImageDecodeFree(value.rgba);
    }
};

struct Cancellation {
    std::thread::id caller = std::this_thread::get_id();
    std::atomic<int> calls{0};
    std::atomic<bool> foreign{false};
    int cancelAt = 0;
};

int canceled(void* opaque) {
    auto* state = static_cast<Cancellation*>(opaque);
    if (std::this_thread::get_id() != state->caller) state->foreign = true;
    return ++state->calls >= state->cancelAt && state->cancelAt > 0;
}

}  // namespace

class MaiRasterImageTest : public QObject {
    Q_OBJECT
private slots:
    void fileAndMemory_data() {
        QTest::addColumn<QByteArray>("bytes");
        QTest::addColumn<int>("format");
        QTest::addColumn<QColor>("color");
        QTest::addColumn<int>("tolerance");
        QTest::newRow("38-byte-webp") << tinyWebp() << int(MAI_IMAGE_FORMAT_WEBP) << QColor(45, 99, 141) << 0;
        QTest::newRow("alpha-png") << encode("PNG", QColor(255, 0, 0, 117)) << int(MAI_IMAGE_FORMAT_PNG)
                                   << QColor(255, 0, 0, 117) << 0;
        QTest::newRow("jpeg") << encode("JPEG", QColor(45, 99, 141)) << int(MAI_IMAGE_FORMAT_JPEG)
                              << QColor(45, 99, 141) << 3;
    }

    void fileAndMemory() {
        QFETCH(QByteArray, bytes);
        QFETCH(int, format);
        QFETCH(QColor, color);
        QFETCH(int, tolerance);
        QVERIFY(!bytes.isEmpty());
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // The extension deliberately disagrees with the compressed content.
        const QString path = directory.filePath(QStringLiteral("\u56fe\u50cf.fake"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), qint64(bytes.size()));
        file.close();
        const QByteArray utf8 = path.toUtf8();
        for (bool memory : {false, true}) {
            const auto* data = reinterpret_cast<const unsigned char*>(bytes.constData());
            const MaiImageInfo inspected = memory ? maiImageInspectMemory(data, size_t(bytes.size()), nullptr)
                                                  : maiImageInspectFile(utf8.constData(), nullptr);
            QCOMPARE(inspected.error_code, 0);
            QCOMPARE(int(inspected.format), format);
            QCOMPARE(inspected.width, 16);
            QCOMPARE(inspected.height, 12);
            MaiImageInfo info{};
            Image image{memory ? maiImageReadMemory(data, size_t(bytes.size()), nullptr, &info)
                               : maiImageReadFile(utf8.constData(), nullptr, &info)};
            QCOMPARE(image.value.error_code, 0);
            QVERIFY(image.value.rgba);
            QCOMPARE(info.error_code, 0);
            QCOMPARE(int(info.format), format);
            QCOMPARE(image.value.width, 16);
            QCOMPARE(image.value.height, 12);
            for (int y = 0; y < 12; ++y) {
                for (int x = 0; x < 16; ++x) {
                    const auto* pixel = image.value.rgba + y * image.value.stride + x * 4;
                    QVERIFY(qAbs(int(pixel[0]) - color.red()) <= tolerance);
                    QVERIFY(qAbs(int(pixel[1]) - color.green()) <= tolerance);
                    QVERIFY(qAbs(int(pixel[2]) - color.blue()) <= tolerance);
                    QCOMPARE(int(pixel[3]), color.alpha());
                }
            }
        }
    }

    void independentOrientation_data() {
        QTest::addColumn<int>("orientation");
        for (int value = 1; value <= 8; ++value) QTest::newRow(qPrintable(QString::number(value))) << value;
    }

    void independentOrientation() {
        QFETCH(int, orientation);
        const QByteArray bytes = fixture(QStringLiteral("exif-orientation-%1.jpg").arg(orientation));
        const QByteArray expected = fixture(QStringLiteral("expected-orientation-%1.rgba").arg(orientation));
        QVERIFY(!bytes.isEmpty());
        QVERIFY(!expected.isEmpty());
        const auto* data = reinterpret_cast<const unsigned char*>(bytes.constData());
        MaiImageInfo info{};
        Image image{maiImageReadMemory(data, size_t(bytes.size()), nullptr, &info)};
        QCOMPARE(image.value.error_code, 0);
        QVERIFY(image.value.rgba);
        QCOMPARE(info.orientation, orientation);
        QCOMPARE(info.width, 96);
        QCOMPARE(info.height, 64);
        QCOMPARE(info.display_width, orientation < 5 ? 96 : 64);
        QCOMPARE(info.display_height, orientation < 5 ? 64 : 96);
        QCOMPARE(image.value.width, info.display_width);
        QCOMPARE(image.value.height, info.display_height);
        QCOMPARE(expected.size(), image.value.stride * image.value.height);
        for (qsizetype index = 0; index < expected.size(); ++index) {
            const int tolerance = index % 4 == 3 ? 0 : 12;
            QVERIFY2(qAbs(int(image.value.rgba[index]) - int(static_cast<unsigned char>(expected[index]))) <= tolerance,
                     qPrintable(QString::number(index)));
        }
        const MaiImageInfo inspected = maiImageInspectMemory(data, size_t(bytes.size()), nullptr);
        QCOMPARE(inspected.error_code, 0);
        QCOMPARE(inspected.orientation, orientation);
        QCOMPARE(inspected.display_width, info.display_width);
    }

    void preservesIcc() {
        const QByteArray bytes = fixture(QStringLiteral("exif-orientation-6-srgb2014.jpg"));
        const QByteArray expected = fixture(QStringLiteral("sRGB2014.icc"));
        QVERIFY(!bytes.isEmpty());
        QVERIFY(!expected.isEmpty());
        Image image{maiImageReadMemory(reinterpret_cast<const unsigned char*>(bytes.constData()), size_t(bytes.size()),
                                       nullptr, nullptr)};
        QCOMPARE(image.value.error_code, 0);
        QVERIFY(image.value.icc_profile);
        QCOMPARE(QByteArray(reinterpret_cast<const char*>(image.value.icc_profile), image.value.icc_size), expected);
    }

    void budgetsAndScaling() {
        const QByteArray bytes = tinyWebp();
        const auto* data = reinterpret_cast<const unsigned char*>(bytes.constData());
        MaiImageReadOptions options{};
        options.max_encoded_bytes = size_t(bytes.size()) - 1;
        QCOMPARE(maiImageInspectMemory(data, size_t(bytes.size()), &options).error_code, AVERROR(EFBIG));
        options = {};
        options.max_source_pixels = 191;
        Image overPixels{maiImageReadMemory(data, size_t(bytes.size()), &options, nullptr)};
        QCOMPARE(overPixels.value.error_code, AVERROR(EFBIG));
        QVERIFY(!overPixels.value.rgba);
        options = {};
        options.max_source_width = 15;
        QCOMPARE(maiImageInspectMemory(data, size_t(bytes.size()), &options).error_code, AVERROR(EFBIG));
        options = {};
        options.max_output_width = 8;
        options.max_output_height = 8;
        MaiImageInfo info{};
        Image scaled{maiImageReadMemory(data, size_t(bytes.size()), &options, &info)};
        QCOMPARE(scaled.value.error_code, 0);
        QCOMPARE(scaled.value.width, 8);
        QCOMPARE(scaled.value.height, 6);
        QCOMPARE(info.width, 16);
        options.max_output_width = -1;
        QCOMPARE(maiImageInspectMemory(data, size_t(bytes.size()), &options).error_code, AVERROR(EINVAL));
    }

    void rejectsNonImageAndCorruption_data() {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("text") << QByteArray("not an image");
        QTest::newRow("svg") << QByteArray("<svg xmlns='http://www.w3.org/2000/svg'/>");
        QTest::newRow("playlist") << QByteArray("#EXTM3U\nhttp://127.0.0.1/never-fetch\n");
        QTest::newRow("truncated-webp") << tinyWebp().left(30);
        QTest::newRow("truncated-jpeg") << encode("JPEG", Qt::red).chopped(10);
        QTest::newRow("truncated-png") << encode("PNG", Qt::red).chopped(20);
        QTest::newRow("damaged-tiff-pixels") << fixture(QStringLiteral("extended/corrupt-deflate.tiff"));
        QTest::newRow("mjpeg-stream") << encode("JPEG", Qt::red) + encode("JPEG", Qt::blue);
    }

    void rejectsNonImageAndCorruption() {
        QFETCH(QByteArray, bytes);
        MaiImageInfo info{};
        Image image{maiImageReadMemory(reinterpret_cast<const unsigned char*>(bytes.constData()), size_t(bytes.size()),
                                       nullptr, &info)};
        QVERIFY(image.value.error_code < 0);
        QVERIFY(!image.value.rgba);
        QCOMPARE(info.error_code, image.value.error_code);
        QCOMPARE(info.width, 0);
        QCOMPARE(info.format, MAI_IMAGE_FORMAT_UNKNOWN);
    }

    void cancellationStaysOnCaller() {
        const QByteArray bytes = tinyWebp();
        const auto* data = reinterpret_cast<const unsigned char*>(bytes.constData());
        Cancellation state;
        MaiImageReadOptions options{};
        options.is_canceled = canceled;
        options.context = &state;
        Image image{maiImageReadMemory(data, size_t(bytes.size()), &options, nullptr)};
        QCOMPARE(image.value.error_code, 0);
        QVERIFY(state.calls > 0);
        QVERIFY(!state.foreign);
        state.cancelAt = 1;
        Image stopped{maiImageReadMemory(data, size_t(bytes.size()), &options, nullptr)};
        QCOMPARE(stopped.value.error_code, AVERROR_EXIT);
        QVERIFY(!stopped.value.rgba);
        QVERIFY(!state.foreign);
    }

    void extendedFormats_data() {
        QTest::addColumn<QString>("name");
        QTest::addColumn<int>("orientation");
        for (const QString& name :
             {QStringLiteral("owned.bmp"), QStringLiteral("owned.tiff"), QStringLiteral("owned.jp2"),
              QStringLiteral("owned.ppm"), QStringLiteral("owned-animated.gif"), QStringLiteral("owned-animated.png"),
              QStringLiteral("owned-animated.webp"), QStringLiteral("owned.avif")})
            QTest::newRow(qPrintable(name)) << name << 1;
        for (const QString& suffix : {QStringLiteral("png"), QStringLiteral("webp"), QStringLiteral("tiff")}) {
            const QString name = QStringLiteral("owned-exif-7.") + suffix;
            QTest::newRow(qPrintable(name)) << name << 7;
        }
    }

    void extendedFormats() {
        QFETCH(QString, name);
        QFETCH(int, orientation);
        const QByteArray bytes = fixture(QStringLiteral("extended/") + name);
        QVERIFY(!bytes.isEmpty());
        MaiImageInfo info{};
        Image image{maiImageReadMemory(reinterpret_cast<const unsigned char*>(bytes.constData()), size_t(bytes.size()),
                                       nullptr, &info)};
        QCOMPARE(image.value.error_code, 0);
        QCOMPARE(info.orientation, orientation);
        QCOMPARE(info.width, 96);
        QCOMPARE(info.height, 64);
        QCOMPARE(image.value.width, orientation >= 5 ? 64 : 96);
        QCOMPARE(image.value.height, orientation >= 5 ? 96 : 64);
        const QByteArray expected = fixture(QStringLiteral("expected-orientation-%1.rgba").arg(orientation));
        QCOMPARE(expected.size(), image.value.stride * image.value.height);
        for (qsizetype index = 0; index < expected.size(); ++index) {
            const int tolerance = index % 4 == 3 ? 0 : 12;
            QVERIFY2(qAbs(int(image.value.rgba[index]) - int(static_cast<unsigned char>(expected[index]))) <= tolerance,
                     qPrintable(name + QLatin1Char(':') + QString::number(index)));
        }
    }

    void icoUsesLargestItem() {
        const QByteArray bytes = fixture(QStringLiteral("extended/owned-multiple.ico"));
        QVERIFY(!bytes.isEmpty());
        const auto* data = reinterpret_cast<const unsigned char*>(bytes.constData());
        const MaiImageInfo inspected = maiImageInspectMemory(data, size_t(bytes.size()), nullptr);
        QCOMPARE(inspected.error_code, 0);
        QCOMPARE(inspected.width, 32);
        QCOMPARE(inspected.height, 24);
        MaiImageInfo info{};
        Image image{maiImageReadMemory(data, size_t(bytes.size()), nullptr, &info)};
        QCOMPARE(image.value.error_code, 0);
        QCOMPARE(info.width, inspected.width);
        QCOMPARE(info.height, inspected.height);
        for (int y = 0; y < image.value.height; ++y)
            for (int x = 0; x < image.value.width; ++x) {
                const auto* pixel = image.value.rgba + y * image.value.stride + x * 4;
                QCOMPARE(pixel[0], 0);
                QCOMPARE(pixel[1], 0);
                QCOMPARE(pixel[2], 255);
                QCOMPARE(pixel[3], 255);
            }
    }

    void heifItemDimensionsExcludeCodecPadding() {
        const QByteArray bytes = fixture(QStringLiteral("extended/owned-padding.heic"));
        QVERIFY(!bytes.isEmpty());
        const auto* data = reinterpret_cast<const unsigned char*>(bytes.constData());
        const MaiImageInfo inspected = maiImageInspectMemory(data, size_t(bytes.size()), nullptr);
        QCOMPARE(inspected.error_code, 0);
        QCOMPARE(inspected.format, MAI_IMAGE_FORMAT_HEIF);
        QCOMPARE(inspected.width, 78);
        QCOMPARE(inspected.height, 110);
        MaiImageInfo info{};
        Image image{maiImageReadMemory(data, size_t(bytes.size()), nullptr, &info)};
        QCOMPARE(image.value.error_code, 0);
        QCOMPARE(info.width, inspected.width);
        QCOMPARE(info.height, inspected.height);
        QCOMPARE(image.value.width, 78);
        QCOMPARE(image.value.height, 110);
        for (int y = 0; y < image.value.height; ++y)
            for (int x = 0; x < image.value.width; ++x) {
                const auto* pixel = image.value.rgba + y * image.value.stride + x * 4;
                QVERIFY(qAbs(int(pixel[0]) - 45) <= 5);
                QVERIFY(qAbs(int(pixel[1]) - 99) <= 5);
                QVERIFY(qAbs(int(pixel[2]) - 141) <= 5);
                QCOMPARE(pixel[3], 255);
            }
    }

    void explicitHundredMegapixelPolicy() {
        QByteArray bytes;
        {
            QImage gray(10000, 10000, QImage::Format_Grayscale8);
            QVERIFY(!gray.isNull());
            gray.fill(QColor(127, 127, 127));
            QBuffer buffer(&bytes);
            QVERIFY(buffer.open(QIODevice::WriteOnly));
            QVERIFY(gray.save(&buffer, "PNG"));
        }
        const auto* data = reinterpret_cast<const unsigned char*>(bytes.constData());
        QCOMPARE(maiImageInspectMemory(data, size_t(bytes.size()), nullptr).error_code, AVERROR(EFBIG));
        MaiImageReadOptions options{};
        options.max_source_pixels = 100'000'000;
        options.max_output_width = options.max_output_height = 8;
        MaiImageInfo info{};
        Image image{maiImageReadMemory(data, size_t(bytes.size()), &options, &info)};
        QCOMPARE(image.value.error_code, 0);
        QCOMPARE(info.width, 10000);
        QCOMPARE(info.height, 10000);
        QCOMPARE(image.value.width, 8);
        QCOMPARE(image.value.height, 8);
        QCOMPARE(image.value.rgba[0], 127);
        options.max_source_pixels = 128'000'001;
        QCOMPARE(maiImageInspectMemory(data, size_t(bytes.size()), &options).error_code, AVERROR(EINVAL));
    }

    void imageGateRejectsVideoButLegacyKeepsCover() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray path = directory.filePath(QStringLiteral("actual-video.png")).toUtf8();
        AVFormatContext* format = nullptr;
        QCOMPARE(avformat_alloc_output_context2(&format, nullptr, "matroska", path.constData()), 0);
        QVERIFY(format);
        AVStream* stream = avformat_new_stream(format, nullptr);
        QVERIFY(stream);
        stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        stream->codecpar->codec_id = AV_CODEC_ID_PNG;
        stream->codecpar->width = 16;
        stream->codecpar->height = 12;
        stream->time_base = AVRational{1, 25};
        QCOMPARE(avio_open(&format->pb, path.constData(), AVIO_FLAG_WRITE), 0);
        QCOMPARE(avformat_write_header(format, nullptr), 0);
        const QByteArray bytes = encode("PNG", Qt::red);
        AVPacket* packet = av_packet_alloc();
        QVERIFY(packet);
        QCOMPARE(av_new_packet(packet, int(bytes.size())), 0);
        memcpy(packet->data, bytes.constData(), size_t(bytes.size()));
        packet->pts = packet->dts = 0;
        packet->duration = 40;
        packet->flags = AV_PKT_FLAG_KEY;
        QCOMPARE(av_interleaved_write_frame(format, packet), 0);
        av_packet_free(&packet);
        QCOMPARE(av_write_trailer(format), 0);
        avio_closep(&format->pb);
        avformat_free_context(format);
        QVERIFY(maiImageInspectFile(path.constData(), nullptr).error_code < 0);
        Image rejected{maiImageReadFile(path.constData(), nullptr, nullptr)};
        QVERIFY(rejected.value.error_code < 0);
        QVERIFY(!rejected.value.rgba);
        Image cover{maiImageDecodeFile(path.constData(), 16, 12)};
        QCOMPARE(cover.value.error_code, 0);
        QCOMPARE(cover.value.width, 16);
        QCOMPARE(cover.value.height, 12);
    }

    void avifCodedBudgetCannotBeHiddenByItemExtent_data() {
        QTest::addColumn<bool>("memory");
        QTest::addColumn<bool>("inspect");
        QTest::addColumn<bool>("axisLimit");
        for (bool memory : {false, true})
            for (bool inspect : {false, true})
                for (bool axis : {false, true}) {
                    const QString name = QStringLiteral("%1-%2-%3")
                                             .arg(memory ? QStringLiteral("memory") : QStringLiteral("file"),
                                                  inspect ? QStringLiteral("inspect") : QStringLiteral("read"),
                                                  axis ? QStringLiteral("axes") : QStringLiteral("pixels"));
                    QTest::newRow(qPrintable(name)) << memory << inspect << axis;
                }
    }

    void avifCodedBudgetCannotBeHiddenByItemExtent() {
        QFETCH(bool, memory);
        QFETCH(bool, inspect);
        QFETCH(bool, axisLimit);
        const QString name = QStringLiteral("extended/owned-ispe-16.avif");
        const QByteArray bytes = fixture(name);
        QCOMPARE(bytes.size(), 407);
        const auto* data = reinterpret_cast<const unsigned char*>(bytes.constData());
        const QByteArray path = (QStringLiteral(MAI_RASTER_FIXTURE_DIR) + QLatin1Char('/') + name).toUtf8();
        MaiImageReadOptions options{};
        if (axisLimit)
            options.max_source_width = options.max_source_height = 16;
        else
            options.max_source_pixels = 4000;
        if (inspect) {
            const MaiImageInfo info = memory ? maiImageInspectMemory(data, size_t(bytes.size()), &options)
                                             : maiImageInspectFile(path.constData(), &options);
            QCOMPARE(info.error_code, AVERROR(EFBIG));
            QCOMPARE(info.format, MAI_IMAGE_FORMAT_UNKNOWN);
        } else {
            MaiImageInfo info{};
            Image image{memory ? maiImageReadMemory(data, size_t(bytes.size()), &options, &info)
                               : maiImageReadFile(path.constData(), &options, &info)};
            QCOMPARE(image.value.error_code, AVERROR(EFBIG));
            QVERIFY(!image.value.rgba);
            QCOMPARE(info.error_code, image.value.error_code);
        }
    }
};

QTEST_GUILESS_MAIN(MaiRasterImageTest)
#include "MaiRasterImageTest.moc"
