// 文件作用：工具只适配路径策略及取消信号，所有业务由普通 service 完成。
#include "MaiChatTools/DesktopPortraitTools.h"

#include <memory>
#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include "MaiPortraitHostTools.h"
#include "MaiChatTools/DesktopPortraitService.h"

void registerDesktopPortraitTools(MaiToolRegistry& registry, const QString& accountDirectory) {
    if (accountDirectory.isEmpty()) return;
    const QDir executable(QCoreApplication::applicationDirPath());
#if defined(Q_OS_MAC)
    const QString models = executable.filePath(QStringLiteral("../Resources/PortraitModels"));
    const QString runtime = executable.filePath(QStringLiteral("../Frameworks/libonnxruntime.1.26.0.dylib"));
#else
    const QString models = executable.filePath(QStringLiteral("PortraitModels"));
    const QString runtime = executable.filePath(QStringLiteral("onnxruntime.dll"));
#endif
    auto service = std::make_shared<DesktopPortraitService>(QDir(accountDirectory).filePath(QStringLiteral("Portrait")),
                                                            models, runtime);
    registerMaiPortraitHostTools(
        registry, [service](const std::string& name, const std::string& arguments, const MaiToolContext& context) {
            const QJsonDocument parsed = QJsonDocument::fromJson(QByteArray::fromStdString(arguments));
            if (!parsed.isObject())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "arguments must be an object");
            DesktopPortraitRequest request;
            request.operation = name == "mobile_beautify_image"      ? QStringLiteral("beautify")
                                : name == "mobile_reshape_body"      ? QStringLiteral("reshape_body")
                                : name == "mobile_portrait_feedback" ? QStringLiteral("record_feedback")
                                                                     : QStringLiteral("personalization");
            request.arguments = parsed.object();
            request.workspace = QString::fromStdString(context.root);
            request.resolvePath = [&context](const QString& path) {
                return QString::fromStdString(context.resolvePath(path.toUtf8().toStdString()));
            };
            request.isCanceled = [&context] { return context.isCanceled(); };
            const QJsonObject response = service->execute(request);
            const std::string encoded = QJsonDocument(response).toJson(QJsonDocument::Compact).toStdString();
            if (!response.value(QStringLiteral("ok")).toBool())
                return MaiToolResult::failure(
                    context.isCanceled() ? MaiErrorCode::Canceled : MaiErrorCode::InvalidInput, encoded);
            return MaiToolResult::success(encoded);
        });
}
