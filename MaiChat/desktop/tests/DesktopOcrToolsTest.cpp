// Exercise local OCR through the ordinary API and its read-only tool adapter.
// Every raster is owned by this test; no desktop capture or user file is read.
#include <QtTest/QtTest>

#include <QBuffer>
#include <QFile>
#include <QFont>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QTemporaryDir>

#include <atomic>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <future>
#include <memory>

#include "MaiTool.h"
#include "MaiChatTools/DesktopOcrTools.h"
#include "MaiChatTools/DesktopVisionTools.h"

class DesktopOcrToolsTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { Q_INIT_RESOURCE(resources); }

    void visionRegistryIncludesReadOnlyOcr() {
        MaiToolRegistry registry;
        registerDesktopVisionTools(registry);
        MaiTool* tool = registry.find("ocr_image");
        QVERIFY2(tool != nullptr, "vision tools must expose local OCR");
        QVERIFY(!tool->requiresApproval(R"({"path":"owned.png"})"));
        const QJsonObject schema = QJsonDocument::fromJson(
            QByteArray::fromStdString(tool->parametersSchema())).object();
        QCOMPARE(schema.value(QStringLiteral("required")).toArray(),
                 QJsonArray{QStringLiteral("path")});
        QCOMPARE(schema.value(QStringLiteral("properties")).toObject().size(), 1);
        QCOMPARE(schema.value(QStringLiteral("additionalProperties")).toBool(true), false);
    }

    void refusesMalformedAndInaccessiblePaths() {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        MaiToolRegistry registry;
        registerDesktopVisionTools(registry);
        MaiTool* tool = registry.find("ocr_image");
        QVERIFY(tool != nullptr);
        MaiToolContext context;
        context.root = workspace.path().toUtf8().toStdString();
        const char* rejected[] = {
            "not-json", "[]", "{}", R"({"path":0})", R"({"path":""})",
            R"({"path":"owned.png","snapshot_id":"made-up"})",
            R"({"path":"../outside.png"})", R"({"path":"missing.png"})",
            R"({"path":"https://example.invalid/image.png"})",
            R"({"path":"embedded\u0000nul.png"})"
        };
        for (const char* arguments : rejected) {
            const MaiToolResult result = tool->execute(arguments, context);
            QVERIFY2(result.hasError(), arguments);
            QCOMPARE(result.error().code(), MaiErrorCode::InvalidInput);
        }
        MaiToolContext noRoot;
        const MaiToolResult denied = tool->execute(R"({"path":"owned.png"})", noRoot);
        QCOMPARE(denied.error().code(), MaiErrorCode::InvalidInput);
    }

    void cancellationStopsBeforeReading() {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        MaiToolRegistry registry;
        registerDesktopVisionTools(registry);
        MaiTool* tool = registry.find("ocr_image");
        QVERIFY(tool != nullptr);
        std::atomic<bool> canceled{true};
        MaiToolContext context;
        context.root = workspace.path().toUtf8().toStdString();
        context.cancel = &canceled;
        const MaiToolResult result = tool->execute(R"({"path":"missing.png"})", context);
        QCOMPARE(result.error().code(), MaiErrorCode::Canceled);
    }

    void rejectsCorruptAndOversizedInputs() {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        QFile corrupt(workspace.filePath(QStringLiteral("corrupt.png")));
        QVERIFY(corrupt.open(QIODevice::WriteOnly));
        QCOMPARE(corrupt.write("owned invalid raster"), qint64(20));
        corrupt.close();
        QFile oversized(workspace.filePath(QStringLiteral("oversized.png")));
        QVERIFY(oversized.open(QIODevice::WriteOnly));
        QVERIFY(oversized.resize(50 * 1024 * 1024 + 1));
        oversized.close();
        // Compress a real oversized raster so file bytes cannot hide the pixel limit.
        QImage large(4001, 3000, QImage::Format_RGB32);
        QVERIFY(!large.isNull());
        large.fill(Qt::white);
        QVERIFY(large.save(workspace.filePath(QStringLiteral("too-many-pixels.png"))));
        MaiToolRegistry registry;
        registerDesktopVisionTools(registry);
        MaiTool* tool = registry.find("ocr_image");
        QVERIFY(tool != nullptr);
        MaiToolContext context;
        context.root = workspace.path().toUtf8().toStdString();
        const char* names[] = {"corrupt.png", "oversized.png", "too-many-pixels.png"};
        for (const char* name : names) {
            const QByteArray arguments = QJsonDocument(QJsonObject{
                {QStringLiteral("path"), QString::fromLatin1(name)}}).toJson(QJsonDocument::Compact);
            const MaiToolResult result = tool->execute(arguments.toStdString(), context);
            QCOMPARE(result.error().code(), MaiErrorCode::InvalidInput);
        }
    }

    void ordinaryApiRejectsRelativePathsAndHonorsCancellation() {
        DesktopOcrService service;
        const auto relative = service.recognizeImageFile("owned.png");
        QCOMPARE(relative.error().code(), MaiErrorCode::InvalidInput);
        const auto remote = service.recognizeImageFile("https://example.invalid/owned.png");
        QCOMPARE(remote.error().code(), MaiErrorCode::InvalidInput);
        DesktopOcrReadOptions canceled;
        canceled.isCanceled = [] { return true; };
        const auto stopped = service.recognizeImageFile("owned.png", canceled);
        QCOMPARE(stopped.error().code(), MaiErrorCode::Canceled);
    }

    void missingBundledModelsAreNotConfigured() {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        QImage image(80, 40, QImage::Format_RGB32);
        image.fill(Qt::white);
        const QString path = workspace.filePath(QStringLiteral("owned-configuration.png"));
        QVERIFY(image.save(path));
        QVERIFY(QFile::exists(QStringLiteral(":/maichat/models/ocr/det.onnx")));
        // Only this isolated test process's own compiled resources are removed.
        // Restore them before assertions so a failing case cannot affect later tests.
        Q_CLEANUP_RESOURCE(resources);
        DesktopOcrService service;
        const auto result = service.recognizeImageFile(path.toUtf8().toStdString());
        Q_INIT_RESOURCE(resources);
        QVERIFY(!result.isOk());
        QCOMPARE(result.error().code(), MaiErrorCode::NotConfigured);
        QVERIFY(!result.error().message().empty());
    }

    void queuedCancellationPrecedesRasterDecode() {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        QImage blank(80, 40, QImage::Format_RGB32);
        blank.fill(Qt::white);
        const QString source = workspace.filePath(QStringLiteral("owned-serialized.png"));
        QVERIFY(blank.save(source));
        const QString corruptPath = workspace.filePath(QStringLiteral("owned-queued-corrupt.png"));
        QFile corrupt(corruptPath);
        QVERIFY(corrupt.open(QIODevice::WriteOnly));
        QCOMPARE(corrupt.write("owned invalid raster"), qint64(20));
        corrupt.close();

        DesktopOcrService service;
        std::size_t completedPolls = 0;
        DesktopOcrReadOptions sampled;
        sampled.isCanceled = [&] { ++completedPolls; return false; };
        const auto warmup = service.recognizeImageFile(source.toUtf8().toStdString(), sampled);
        QVERIFY2(warmup.isOk(), warmup.error().message().c_str());
        QVERIFY(completedPolls > 0);

        // Sample the last synchronous cancellation poll with a real raster,
        // rather than assume a codec-specific callback count. Pause a matching
        // call there while it owns the engine, then exercise another caller.
        std::promise<void> holdingPromise;
        auto holding = holdingPromise.get_future();
        std::promise<void> releasePromise;
        const auto release = releasePromise.get_future().share();
        auto first = std::async(std::launch::async, [&] {
            std::size_t polls = 0;
            DesktopOcrReadOptions options;
            options.isCanceled = [&] {
                if (++polls == completedPolls) {
                    holdingPromise.set_value();
                    // Bound the test's own gate so an unexpected early test
                    // return cannot strand a worker or the service mutex.
                    release.wait_for(std::chrono::seconds(5));
                }
                return false;
            };
            return service.recognizeImageFile(source.toUtf8().toStdString(), options);
        });
        if (holding.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            releasePromise.set_value();
            first.wait();
            QFAIL("owned OCR call did not reach its synchronous completion gate");
        }
        std::atomic<bool> canceled{false};
        std::promise<void> queuedStartedPromise;
        auto queuedStarted = queuedStartedPromise.get_future();
        auto queued = std::async(std::launch::async, [&] {
            bool announced = false;
            DesktopOcrReadOptions options;
            options.isCanceled = [&] {
                if (!announced) {
                    announced = true;
                    queuedStartedPromise.set_value();
                }
                return canceled.load();
            };
            return service.recognizeImageFile(corruptPath.toUtf8().toStdString(), options);
        });
        const bool started = queuedStarted.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
        const bool decodedBeforeCancel = queued.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
        canceled.store(true);
        const bool canceledWhileFirstHeld = queued.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
        releasePromise.set_value();
        const auto queuedResult = queued.get();
        const auto firstResult = first.get();
        // A corrupt queued image must not be decoded before taking its turn.
        // Its cancellation also must complete without releasing the first call.
        QVERIFY(started);
        QVERIFY(!decodedBeforeCancel);
        QVERIFY(canceledWhileFirstHeld);
        QCOMPARE(queuedResult.error().code(), MaiErrorCode::Canceled);
        QVERIFY2(firstResult.isOk(), firstResult.error().message().c_str());
    }

    void ordinaryAndToolShareRealEngineAndImageCoordinates() {
        QFile detection(QStringLiteral(":/maichat/models/ocr/det.onnx"));
        QFile recognition(QStringLiteral(":/maichat/models/ocr/rec.onnx"));
        QFile dictionary(QStringLiteral(":/maichat/models/ocr/dictionary.txt"));
        QVERIFY(detection.size() > 100000);
        QVERIFY(recognition.size() > 100000);
        QVERIFY(dictionary.size() > 1000);
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const QString name = QStringLiteral("owned-\u6587\u672c.png");
        const QString path = workspace.filePath(name);
        // Black text on transparent black pixels tests the service's white
        // compositing. Ignoring alpha would turn this entire raster black.
        QImage image(900, 260, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        {
            QPainter painter(&image);
            painter.setRenderHint(QPainter::TextAntialiasing);
            QFont font(QStringLiteral("Arial"));
            font.setPixelSize(64);
            painter.setFont(font);
            painter.setPen(Qt::black);
            painter.drawText(QPoint(45, 105), QStringLiteral("HELLO 12345"));
            painter.drawText(QPoint(45, 205), QStringLiteral("LOCAL OCR 527"));
        }
        QVERIFY(image.save(path));
        QFile unchanged(path);
        QVERIFY(unchanged.open(QIODevice::ReadOnly));
        const QByteArray originalBytes = unchanged.readAll();
        auto service = std::make_shared<DesktopOcrService>();
        const auto ordinary = service->recognizeImageFile(path.toUtf8().toStdString());
        QVERIFY2(ordinary.isOk(), ordinary.error().message().c_str());
        QCOMPARE(ordinary.value().imageWidth, image.width());
        QCOMPARE(ordinary.value().imageHeight, image.height());
        QVERIFY(!ordinary.value().blocks.empty());
        QString recognized;
        for (const DesktopOcrBlock& block : ordinary.value().blocks)
            recognized += QString::fromUtf8(block.text.data(), block.text.size());
        QVERIFY2(recognized.contains(QStringLiteral("HELLO"), Qt::CaseInsensitive), qPrintable(recognized));
        QVERIFY2(recognized.contains(QStringLiteral("12345")), qPrintable(recognized));

        MaiToolRegistry registry;
        registerDesktopOcrTools(registry, service);
        MaiTool* tool = registry.find("ocr_image");
        QVERIFY(tool != nullptr);
        MaiToolContext context;
        context.root = workspace.path().toUtf8().toStdString();
        const auto encoded = QJsonDocument(QJsonObject{{QStringLiteral("path"), name}})
            .toJson(QJsonDocument::Compact);
        const MaiToolResult called = tool->execute(encoded.toStdString(), context);
        QVERIFY2(!called.hasError(), called.error().message().c_str());
        const QJsonObject result = QJsonDocument::fromJson(QByteArray::fromStdString(called.output())).object();
        QCOMPARE(result.value(QStringLiteral("image_width")).toInt(), image.width());
        QCOMPARE(result.value(QStringLiteral("image_height")).toInt(), image.height());
        QCOMPARE(result.value(QStringLiteral("space")).toString(), QStringLiteral("image"));
        QCOMPARE(result.value(QStringLiteral("space_id")).toString(), name);
        QCOMPARE(result.value(QStringLiteral("source_path")).toString(), name);
        QCOMPARE(result.value(QStringLiteral("coordinate_origin")).toString(), QStringLiteral("top_left"));
        QCOMPARE(result.value(QStringLiteral("unit")).toString(), QStringLiteral("pixel"));
        QVERIFY(result.value(QStringLiteral("orientation_applied")).toBool());
        QVERIFY(!result.contains(QStringLiteral("snapshot_id")));
        const QJsonArray blocks = result.value(QStringLiteral("blocks")).toArray();
        QCOMPARE(static_cast<std::size_t>(blocks.size()), ordinary.value().blocks.size());
        for (qsizetype index = 0; index < blocks.size(); ++index) {
            const QJsonObject block = blocks.at(index).toObject();
            const DesktopOcrBlock& expected = ordinary.value().blocks[static_cast<std::size_t>(index)];
            QCOMPARE(block.value(QStringLiteral("text")).toString(), QString::fromUtf8(expected.text.c_str()));
            QCOMPARE(static_cast<float>(block.value(QStringLiteral("confidence")).toDouble()), expected.confidence);
            QVERIFY(std::isfinite(expected.confidence));
            QVERIFY(expected.confidence >= 0 && expected.confidence <= 1);
            const QJsonArray quad = block.value(QStringLiteral("quad")).toArray();
            QCOMPARE(quad.size(), qsizetype(4));
            float left = float(image.width());
            float top = float(image.height());
            float right = 0;
            float bottom = 0;
            for (qsizetype point = 0; point < quad.size(); ++point) {
                const QJsonObject found = quad.at(point).toObject();
                const DesktopOcrPoint& expectedPoint = expected.quad[static_cast<std::size_t>(point)];
                QCOMPARE(static_cast<float>(found.value(QStringLiteral("x")).toDouble()), expectedPoint.x);
                QCOMPARE(static_cast<float>(found.value(QStringLiteral("y")).toDouble()), expectedPoint.y);
                QVERIFY(std::isfinite(expectedPoint.x) && std::isfinite(expectedPoint.y));
                QVERIFY(expectedPoint.x >= 0 && expectedPoint.x <= image.width());
                QVERIFY(expectedPoint.y >= 0 && expectedPoint.y <= image.height());
                left = std::min(left, expectedPoint.x);
                top = std::min(top, expectedPoint.y);
                right = std::max(right, expectedPoint.x);
                bottom = std::max(bottom, expectedPoint.y);
            }
            const QJsonObject bounds = block.value(QStringLiteral("bounds")).toObject();
            QCOMPARE(static_cast<float>(bounds.value(QStringLiteral("x")).toDouble()), left);
            QCOMPARE(static_cast<float>(bounds.value(QStringLiteral("y")).toDouble()), top);
            QCOMPARE(static_cast<float>(bounds.value(QStringLiteral("width")).toDouble()), right - left);
            QCOMPARE(static_cast<float>(bounds.value(QStringLiteral("height")).toDouble()), bottom - top);
        }
        std::atomic<bool> canceled{true};
        context.cancel = &canceled;
        QCOMPARE(tool->execute(encoded.toStdString(), context).error().code(), MaiErrorCode::Canceled);
        unchanged.seek(0);
        QCOMPARE(unchanged.readAll(), originalBytes);
    }

    void exifOrientationDefinesReturnedRasterDimensions() {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        QImage image(480, 160, QImage::Format_RGB32);
        image.fill(Qt::white);
        QByteArray jpeg;
        QBuffer buffer(&jpeg);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(image.save(&buffer, "JPEG"));
        // Independent APP1/TIFF orientation=6; only the normal image codec
        // rotates pixels, so applying it again would restore the wrong size.
        jpeg.insert(2, QByteArray::fromHex(
            "ffe1002245786966000049492a0008000000010012010300010000000600000000000000"));
        const QString path = workspace.filePath(QStringLiteral("owned-exif6.jpg"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(jpeg), qint64(jpeg.size()));
        file.close();
        DesktopOcrService service;
        const auto result = service.recognizeImageFile(path.toUtf8().toStdString());
        QVERIFY2(result.isOk(), result.error().message().c_str());
        QCOMPARE(result.value().imageWidth, 160);
        QCOMPARE(result.value().imageHeight, 480);
        QVERIFY(result.value().blocks.empty());
    }
};

QTEST_MAIN(DesktopOcrToolsTest)
#include "DesktopOcrToolsTest.moc"
