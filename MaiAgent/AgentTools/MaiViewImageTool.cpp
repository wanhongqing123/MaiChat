// 文件作用：读取工作区图片并返回可供模型查看的内容。
// 直接依赖：MaiFilePath.h、MaiFileSystem.h、MaiPathGuard.h；本文件在这些接口之间完成调用与结果转换。
#include "MaiViewImageTool.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiPathGuard.h"

using json = nlohmann::json;

namespace {

constexpr std::uint64_t kMaximumImageBytes = 20u * 1024u * 1024u;

std::string detectMimeType(const std::string& bytes) {
    if (bytes.size() >= 8 && bytes.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0) return "image/png";
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xFFu &&
        static_cast<unsigned char>(bytes[1]) == 0xD8u &&
        static_cast<unsigned char>(bytes[2]) == 0xFFu)
        return "image/jpeg";
    if (bytes.size() >= 6 &&
        (bytes.compare(0, 6, "GIF87a") == 0 || bytes.compare(0, 6, "GIF89a") == 0))
        return "image/gif";
    if (bytes.size() >= 12 && bytes.compare(0, 4, "RIFF") == 0 && bytes.compare(8, 4, "WEBP") == 0)
        return "image/webp";
    return {};
}

class MaiViewImageTool final : public MaiTool {
public:
    explicit MaiViewImageTool(MaiImagePreviewCallback preview) : mPreview(std::move(preview)) {}

    std::string name() const override {
        return "view_image";
    }

    std::string description() const override {
        // 本工具把像素交给有视觉能力的主模型；用户展示入口不依赖模型理解图片。
        return "Analyze an existing PNG, JPEG, WebP, or GIF file with an image-capable primary "
               "model. "
               "MaiChat prepares a bounded JPEG preview for model analysis. To display a file to "
               "the "
               "user, use mobile_preview_image when available. "
               "Images attached by the user are already in the current prompt; inspect them "
               "directly instead of calling this tool for paths mentioned in a screenshot.";
    }

    bool isAvailableForModel(const std::string& model) const override {
        // 当前本地主模型传输只接受文本。按实际 local: 模型标识拒绝，不能因为
        // 底层架构可能支持视觉就宣称已经接入图像输入。
        return model != "glm-5.3" && model.compare(0, 6, "local:") != 0;
    }

    std::string parametersSchema() const override {
        // path 是宿主可访问的现有图片；工具只返回预览，不负责保存或编辑。
        return R"({"type":"object","properties":{)"
               R"("path":{"type":"string","description":"Absolute or relative image path"}},)"
               R"("required":["path"],"additionalProperties":false})";
    }

    // 只预览可访问的现有图片；路径解析和大小限制先于读取，不能借预览访问工作区外文件。
    // 读取失败明确返回错误，避免模型把空白预览误判为图片内容为空。
    MaiToolResult execute(const std::string& argumentsJson,
                          const MaiToolContext& context) override {
        const json arguments = json::parse(argumentsJson, nullptr, /*allow_exceptions=*/false);
        if (arguments.is_discarded() || !arguments.is_object() || arguments.size() != 1 ||
            !arguments.contains("path") || !arguments["path"].is_string()) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "view_image requires one string parameter named path");
        }
        if (!isAvailableForModel(context.model)) {
            if (context.model != "glm-5.3")
                return MaiToolResult::failure(
                    MaiErrorCode::NotSupported,
                    "The current local primary model accepts text only and cannot analyze image "
                    "pixels. To display this file to the user, use mobile_preview_image when "
                    "available. Do not claim that the image was analyzed.");
            return MaiToolResult::failure(
                MaiErrorCode::NotSupported,
                "The current glm-5.3 model accepts text only. Ask the user to switch to "
                "glm-5.3-flash before viewing an image.");
        }

        const std::string rawPath = arguments["path"].get<std::string>();
        const std::string resolved = context.resolvePath(rawPath);
        if (resolved.empty()) {
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "Image path is outside the area accessible to this host: " + rawPath +
                    ". Working directory for relative paths: " + context.root +
                    ". Inspect a user-attached image directly, or use an accessible image path.");
        }
        const MaiFilePath path = MaiFilePath::fromUtf8(resolved);
        if (!MaiFileSystem::exists(path))
            return MaiToolResult::failure(MaiErrorCode::NotFound,
                                          "image file does not exist: " + rawPath);
        if (MaiFileSystem::isDirectory(path))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "image path points to a directory: " + rawPath);

        std::string bytes;
        bool truncated = false;
        const MaiError readError =
            MaiFileSystem::readFile(path, bytes, kMaximumImageBytes, &truncated);
        if (readError.hasError())
            return MaiToolResult::failure(readError.code(), readError.message());
        if (truncated) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "image exceeds the 20 MB input limit: " + rawPath);
        }
        const std::string mimeType = detectMimeType(bytes);
        if (mimeType.empty()) {
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "file is not a supported PNG, JPEG, WebP, or GIF image: " + rawPath);
        }

        if (mPreview) {
            MaiResult<std::string> preview = mPreview(path.toUtf8(), context);
            if (!preview)
                return MaiToolResult::failure(preview.error().code(), preview.error().message());
            return MaiToolResult::successWithImages(
                "Loaded a resized model preview from " + rawPath + ".",
                {MaiToolImage{preview.value(), "image/jpeg"}});
        }
        return MaiToolResult::successWithImages("Loaded the image from " + rawPath + ".",
                                                {MaiToolImage{path.toUtf8(), mimeType}});
    }

private:
    MaiImagePreviewCallback mPreview;
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiViewImageTool(MaiImagePreviewCallback preview) {
    return std::make_unique<MaiViewImageTool>(std::move(preview));
}
