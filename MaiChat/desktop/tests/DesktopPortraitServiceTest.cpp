// 文件作用：普通 Qt 服务和共用工具入口的真实 PNG/账号登记回归，不依赖运行中的 App。
#include <atomic>
#include <future>
#include <memory>
#include <iostream>
#include <stdexcept>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QPainter>
#include "MaiPathGuard.h"
#include "MaiPortraitHostTools.h"
#include "MaiChatTools/DesktopPortraitService.h"
#include "MaiChatTools/DesktopPortraitTools.h"

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
QJsonObject run(const DesktopPortraitService& service, const DesktopPortraitRequest& request) {
    return std::async(std::launch::async, [&] { return service.execute(request); }).get();
}
QByteArray read(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "cannot read test file");
    return file.readAll();
}
}  // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        require(argc == 4 || (argc == 5 && QString::fromLocal8Bit(argv[4]) == QStringLiteral("--bundled-tools")),
                "expected repository root, ONNX runtime, artifact directory and optional --bundled-tools");
        const QString root = QString::fromLocal8Bit(argv[1]);
        const QString runtime = QString::fromLocal8Bit(argv[2]);
        const QDir output(QString::fromLocal8Bit(argv[3]));
        require(QDir().mkpath(output.path()), "cannot create test directory");
        const QString workspace = output.filePath(QStringLiteral("Workspace"));
        require(QDir().mkpath(workspace), "cannot create workspace");
        const QString profile = output.filePath(QStringLiteral("AccountA/Portrait"));
        DesktopPortraitService service(profile, QDir(root).filePath(QStringLiteral("MaiChat/Portrait/models")),
                                       runtime);
        QImage source(QDir(root).filePath(QStringLiteral("MaiAgent/third_party/opencv/samples/data/lena.jpg")));
        require(!source.isNull(), "missing local test fixture");
        const QString input = QDir(workspace).filePath(QStringLiteral("input.png"));
        require(source.save(input, "PNG"), "cannot save test input");
        const QByteArray original = read(input);
        DesktopPortraitRequest request;
        request.operation = QStringLiteral("beautify");
        request.workspace = workspace;
        request.arguments = {{QStringLiteral("path"), QStringLiteral("input.png")},
                             {QStringLiteral("smoothing"), 0.4},
                             {QStringLiteral("brightening"), 0},
                             {QStringLiteral("whitening"), 0.5},
                             {QStringLiteral("warmth"), 0}};
        request.resolvePath = [workspace](const QString& path) {
            return QString::fromStdString(maiResolvePathWithinRoot(workspace.toStdString(), path.toStdString()));
        };
        request.isCanceled = [] { return false; };
        require(!service.execute(request).value(QStringLiteral("ok")).toBool(), "UI thread processing was allowed");
        auto denied = request;
        denied.arguments.insert(QStringLiteral("path"), QStringLiteral("../outside.png"));
        require(!run(service, denied).value(QStringLiteral("ok")).toBool(), "outside source was allowed");
        auto canceled = request;
        canceled.isCanceled = [] { return true; };
        require(run(service, canceled).value(QStringLiteral("errorCode")).toString() == QStringLiteral("canceled"),
                "cancel was ignored");
        auto unknown = request;
        unknown.arguments.insert(QStringLiteral("delegate"), true);
        require(!run(service, unknown).value(QStringLiteral("ok")).toBool(), "unknown field was allowed");
        const auto response = run(service, request);
        std::cout << QJsonDocument(response).toJson(QJsonDocument::Compact).constData() << std::endl;
        require(response.value(QStringLiteral("ok")).toBool(), "ordinary portrait edit failed");
        const auto result = response.value(QStringLiteral("output")).toObject();
        require(result.value(QStringLiteral("processing_version")).toString().endsWith(QStringLiteral("-v8")),
                "portrait service used a stale processing engine");
        require(!result.value(QStringLiteral("parameters")).toObject().contains(QStringLiteral("remove_body_hair")) &&
                    result.value(QStringLiteral("body_cleanup_request_mode")).toString() == QStringLiteral("auto"),
                "automatic body cleansing leaked a non-boolean request field");
        require(result.value(QStringLiteral("whitening_color_policy")).toString() ==
                        QStringLiteral("source_hue_chroma_cap") &&
                    result.value(QStringLiteral("whitening_shadow_amount")).toDouble() == 18.75,
                "v8 color policy or actual fixed shadow amplitude was not forwarded");
        for (bool removeHair : {false, true}) {
            auto explicitHair = request;
            explicitHair.arguments.insert(QStringLiteral("remove_body_hair"), removeHair);
            const auto explicitResult = run(service, explicitHair);
            std::cout << QJsonDocument(explicitResult).toJson(QJsonDocument::Compact).constData() << std::endl;
            require(explicitResult.value(QStringLiteral("ok")).toBool(), "explicit body cleansing policy failed");
            const auto details = explicitResult.value(QStringLiteral("output")).toObject();
            require(details.value(QStringLiteral("body_cleanup_request_mode")).toString() ==
                        (removeHair ? QStringLiteral("enabled") : QStringLiteral("disabled")),
                    "explicit body cleansing request mode was lost");
            require(details.value(QStringLiteral("parameters")).toObject().value(QStringLiteral("remove_body_hair")) ==
                        QJsonValue(removeHair),
                    "explicit body cleansing policy was not preserved");
        }
        const QImage edited(QDir(workspace).filePath(result.value(QStringLiteral("path")).toString()));
        const QImage mask(QDir(workspace).filePath(result.value(QStringLiteral("mask_path")).toString()));
        require(edited.size() == source.size() && mask.size() == source.size(), "native dimensions were lost");
        require(!result.value(QStringLiteral("result_id")).toString().isEmpty(), "result was not registered");
        require(result.value(QStringLiteral("original_path")).toString() == QFileInfo(input).canonicalFilePath(),
                "original lineage was lost");
        require(read(input) == original, "source bytes changed");
        require(QFile::exists(QDir(profile).filePath(QStringLiteral("portrait.sqlite3"))), "account ledger missing");
        auto geometryRequest = request;
        geometryRequest.arguments.insert(QStringLiteral("smoothing"), 0);
        geometryRequest.arguments.insert(QStringLiteral("whitening"), 0);
        geometryRequest.arguments.insert(QStringLiteral("slim_face"), 0.3);
        geometryRequest.arguments.insert(QStringLiteral("enlarge_eyes"), 0.2);
        const auto geometry = run(service, geometryRequest);
        require(
            geometry.value(QStringLiteral("ok")).toBool() &&
                geometry.value(QStringLiteral("output")).toObject().value(QStringLiteral("geometry_applied")).toBool(),
            "native face geometry path failed");
        DesktopPortraitRequest status;
        status.operation = QStringLiteral("personalization");
        status.arguments = {{QStringLiteral("action"), QStringLiteral("status")}};
        require(run(service, status).value(QStringLiteral("ok")).toBool(), "ordinary account profile status failed");
        const auto publishedBefore = QDir(workspace).entryList({QStringLiteral("portrait-*.png")}, QDir::Files);
        auto cancelAfterImage = request;
        auto cancellation = std::make_shared<std::atomic<bool>>(false);
        cancelAfterImage.isCanceled = [workspace, publishedBefore, cancellation] {
            if (QDir(workspace).entryList({QStringLiteral("portrait-*.png")}, QDir::Files).size() >
                publishedBefore.size())
                cancellation->store(true);
            return cancellation->load();
        };
        const auto canceledPublication = run(service, cancelAfterImage);
        require(canceledPublication.value(QStringLiteral("errorCode")).toString() == QStringLiteral("canceled"),
                "publication-stage cancellation was ignored");
        require(QDir(workspace).entryList({QStringLiteral("portrait-*.png")}, QDir::Files) == publishedBefore,
                "canceled publication leaked an unregistered output");
        // Exercise the shared factory with the same normal service, preserving the actual tool context.
        MaiToolRegistry registry;
        if (argc == 5) {
            // The bundle probe exercises the actual desktop adapter and its packaged asset paths.
            registerDesktopPortraitTools(registry, QFileInfo(profile).absolutePath());
        } else {
            registerMaiPortraitHostTools(
                registry, [&](const std::string&, const std::string& arguments, const MaiToolContext& context) {
                    auto next = request;
                    next.arguments = QJsonDocument::fromJson(QByteArray::fromStdString(arguments)).object();
                    next.workspace = QString::fromStdString(context.root);
                    auto response = service.execute(next);
                    const auto bytes = QJsonDocument(response).toJson(QJsonDocument::Compact).toStdString();
                    return response.value(QStringLiteral("ok")).toBool()
                               ? MaiToolResult::success(bytes)
                               : MaiToolResult::failure(MaiErrorCode::InvalidInput, bytes);
                });
        }
        require(registry.find("mobile_beautify_image") && registry.find("mobile_reshape_body") &&
                    registry.find("mobile_portrait_feedback") && registry.find("model_continue_training"),
                "shared tools missing");
        MaiToolContext context;
        context.root = workspace.toStdString();
        const auto tool =
            std::async(std::launch::async, [&] {
                auto arguments = request.arguments;
                arguments.insert(QStringLiteral("path"), result.value(QStringLiteral("path")));
                return registry.find("mobile_beautify_image")
                    ->execute(QJsonDocument(arguments).toJson(QJsonDocument::Compact).toStdString(), context);
            }).get();
        require(!tool.hasError(), "shared tool edit failed");
        const auto toolOutput = QJsonDocument::fromJson(QByteArray::fromStdString(tool.output()))
                                    .object()
                                    .value(QStringLiteral("output"))
                                    .toObject();
        require(toolOutput.value(QStringLiteral("original_path")).toString() == QFileInfo(input).canonicalFilePath(),
                "tool re-edited generated pixels instead of original");
        if (argc == 5) {
            require(QDir(workspace).entryList({QStringLiteral("*.partial")}, QDir::Files).isEmpty(),
                    "bundled desktop adapter leaked a partial file");
            std::cout << "DesktopPortraitService: packaged desktop tool adapter and v8 request modes passed"
                      << std::endl;
            return 0;
        }
        // A distant person in a 32 MP raster proves the service uses the tile protocol beyond 12 MP.
        QImage large(8000, 4000, QImage::Format_RGB32);
        large.fill(Qt::white);
        {
            QPainter painter(&large);
            painter.drawImage(7400, 3400, source);
        }
        require(large.save(QDir(workspace).filePath(QStringLiteral("large.png")), "PNG"), "cannot save large input");
        auto largeRequest = request;
        largeRequest.arguments.insert(QStringLiteral("path"), QStringLiteral("large.png"));
        const auto largeResponse = run(service, largeRequest);
        std::cout << QJsonDocument(largeResponse).toJson(QJsonDocument::Compact).constData() << std::endl;
        require(largeResponse.value(QStringLiteral("ok")).toBool(), "large tiled edit failed");
        const auto largeOutput = largeResponse.value(QStringLiteral("output")).toObject();
        require(QImage(QDir(workspace).filePath(largeOutput.value(QStringLiteral("path")).toString())).size() ==
                    large.size(),
                "large output was resized");
        require(QImage(QDir(workspace).filePath(largeOutput.value(QStringLiteral("mask_path")).toString())).size() ==
                    large.size(),
                "large mask was truncated");
        require(largeOutput.value(QStringLiteral("tile_count")).toInt() > 1, "large edit bypassed tile plan");
        const auto artifacts = QDir(workspace).entryList({QStringLiteral("*.partial")}, QDir::Files);
        require(artifacts.isEmpty(), "partial file leaked");
        std::cout << "DesktopPortraitService: ordinary, tool, lineage, authorization, cancel, 32MP PNG/mask passed"
                  << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
