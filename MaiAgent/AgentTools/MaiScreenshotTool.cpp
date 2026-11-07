// 文件作用：把屏幕截图能力封装为 Agent 工具。
// 直接依赖：MaiFilePath.h、MaiFileSystem.h、MaiIdGenerator.h、MaiPathGuard.h；完成捕获、边界校验与结果转换。
#include "MaiScreenshotTool.h"

#include <memory>
#include <string>

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiPathGuard.h"
#include "MaiScreenshot.h"

using json = nlohmann::json;

namespace {

class MaiScreenshotTool final : public MaiTool {
public:
    std::string name() const override {
        return "screenshot";
    }

    std::string description() const override {
        // 屏幕或窗口截图必须通过可见区域和授权边界；现成截图直接检查，不用脚本重复捕获。
        // JSON 返回工作区相对路径、类型和尺寸；用同一路径交给媒体工具，展示这份原截图。
        return "Capture a display or a visible top-level window and inspect the returned image. "
               "Use mode=display for the display containing the mouse pointer. Use mode=window "
               "with windowTitle when the user names a specific window; call list_windows first "
               "when the exact title is unknown. The JSON result includes image_path relative to "
               "this Agent workspace, type, mime_type, width, height, and mode. To show this same "
               "screenshot in the current conversation, call agent_send_media with "
               "file_path set to the returned image_path and type=image. Do not recapture it "
               "with shell scripts.";
    }

    bool isAvailableForModel(const std::string& model) const override {
        // 与 view_image 共用当前已接线的文本模型边界，避免宣告不能回灌像素的截图工具。
        return model != "glm-5.3" && model.compare(0, 6, "local:") != 0;
    }

    std::string parametersSchema() const override {
        // mode=display 截光标所在屏幕；mode=window 需要可见窗口标题 windowTitle。
        return R"({"type":"object","properties":{)"
               R"("mode":{"type":"string","enum":["display","window"],"default":"display"},)"
               R"("windowTitle":{"type":"string","description":"Visible window title; required for window mode"}},)"
               R"("required":[],"additionalProperties":false})";
    }

    bool requiresApproval(const std::string&) const override {
        return true;
    }

    bool requiresPerCallApproval(const std::string&) const override {
        return true;
    }

    // 截图涉及屏幕隐私，审批后才解析窗口或显示器目标；目标无效时不能回退为整屏截图。
    // 图像保存到会话工作区；JSON 相对路径与图片附件绝对路径引用同一文件，供后续交付和检查。
    // 桌面宿主即使允许读取工作区外文件，截图输出仍必须经过工作区边界校验。
    MaiToolResult execute(const std::string& argumentsJson,
                          const MaiToolContext& context) override {
        const json arguments = json::parse(argumentsJson, nullptr, /*allow_exceptions=*/false);
        if (arguments.is_discarded() || !arguments.is_object()) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "screenshot expects a JSON object");
        }
        for (auto field = arguments.begin(); field != arguments.end(); ++field) {
            if (field.key() != "mode" && field.key() != "windowTitle") {
                return MaiToolResult::failure(
                    MaiErrorCode::InvalidInput,
                    "screenshot received an unknown parameter: " + field.key());
            }
        }
        if (arguments.contains("mode") && !arguments["mode"].is_string()) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "screenshot mode must be a string");
        }
        const std::string mode = arguments.contains("mode") ? arguments["mode"].get<std::string>()
                                                            : std::string("display");
        MaiScreenshotRequest request;
        if (mode == "window") {
            request.target = MaiScreenshotTarget::WindowByTitle;
            if (!arguments.contains("windowTitle") || !arguments["windowTitle"].is_string() ||
                arguments["windowTitle"].get_ref<const std::string&>().empty()) {
                return MaiToolResult::failure(
                    MaiErrorCode::InvalidInput,
                    "windowTitle is required and must be non-empty when mode is window");
            }
            request.windowTitle = arguments["windowTitle"].get<std::string>();
        } else if (mode != "display") {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "screenshot mode must be display or window");
        }
        if (context.isCanceled()) {
            return MaiToolResult::failure(MaiErrorCode::Canceled, "screen capture was canceled");
        }
        if (!isAvailableForModel(context.model)) {
            if (context.model != "glm-5.3")
                return MaiToolResult::failure(
                    MaiErrorCode::NotSupported,
                    "The current local primary model accepts text only and cannot inspect "
                    "screenshots. Ask the user to select an image-capable primary model.");
            return MaiToolResult::failure(
                MaiErrorCode::NotSupported,
                "The current glm-5.3 model accepts text only. Ask the user to switch to "
                "glm-5.3-flash before taking a screenshot.");
        }
        if (context.root.empty() ||
            !MaiFileSystem::isDirectory(MaiFilePath::fromUtf8(context.root))) {
            return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                          "screenshot requires an accessible Agent workspace");
        }

        // Windows 对不存在文件的解析只做词法归一化，必须先独立解析已存在的截图目录链接。
        const std::string directoryPath = maiResolvePathWithinRoot(context.root, "screenshots");
        if (directoryPath.empty()) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "screenshot output leaves the Agent workspace");
        }
        const MaiFilePath directory = MaiFilePath::fromUtf8(directoryPath);
        const MaiError directoryError = MaiFileSystem::createDirectories(directory);
        if (directoryError.hasError()) {
            return MaiToolResult::failure(directoryError.code(), directoryError.message());
        }
        const std::string fileName = MaiIdGenerator::generate("screenshot_") + ".png";
        const std::string relativePath = "screenshots/" + fileName;
        const std::string resolvedPath = maiResolvePathWithinRoot(
            context.root, directory.append(MaiFilePath::fromUtf8(fileName)).toUtf8());
        const MaiFilePath path = MaiFilePath::fromUtf8(resolvedPath);
        // 目录创建与捕获可能阻塞；每个阶段重新解析链接，变化时拒绝写入或交付失效路径。
        const auto outputIsWithinWorkspace = [&] {
            return !resolvedPath.empty() &&
                   maiResolvePathWithinRoot(context.root, "screenshots") == directoryPath &&
                   maiResolvePathWithinRoot(context.root, resolvedPath) == resolvedPath;
        };
        if (!outputIsWithinWorkspace() || MaiFileSystem::exists(path)) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "screenshot output must be a new workspace file");
        }
        if (!maiIsScreenshotSupported()) {
            return MaiToolResult::failure(
                MaiErrorCode::NotSupported,
                "native screen capture is not supported on this platform");
        }
        if (context.isCanceled()) {
            return MaiToolResult::failure(MaiErrorCode::Canceled, "screen capture was canceled");
        }

        MaiScreenshot screenshot;
        const MaiError captureError = maiCaptureScreenshot(request, screenshot);
        if (captureError.hasError()) {
            return MaiToolResult::failure(captureError.code(), captureError.message());
        }
        if (screenshot.pngBytes.empty() || screenshot.width <= 0 || screenshot.height <= 0) {
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "native screen capture returned an empty image");
        }

        if (context.isCanceled()) {
            return MaiToolResult::failure(MaiErrorCode::Canceled, "screen capture was canceled");
        }
        if (!outputIsWithinWorkspace()) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "screenshot output left the Agent workspace");
        }
        const std::string bytes(reinterpret_cast<const char*>(screenshot.pngBytes.data()),
                                screenshot.pngBytes.size());
        const MaiError writeError = MaiFileSystem::writeFile(path, bytes);
        if (writeError.hasError()) {
            return MaiToolResult::failure(writeError.code(), writeError.message());
        }

        if (!outputIsWithinWorkspace()) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "screenshot output left the Agent workspace");
        }
        const std::string output = json{{"image_path", relativePath},  {"type", "image"},
                                        {"mime_type", "image/png"},    {"width", screenshot.width},
                                        {"height", screenshot.height}, {"mode", mode}}
                                       .dump();
        return MaiToolResult::successWithImages(output, {MaiToolImage{path.toUtf8(), "image/png"}});
    }
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiScreenshotTool() {
    return std::make_unique<MaiScreenshotTool>();
}
