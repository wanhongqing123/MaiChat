// 文件作用：把屏幕截图能力封装为 Agent 工具。
// 直接依赖：MaiFilePath.h、MaiFileSystem.h、MaiIdGenerator.h、MaiPathGuard.h；完成捕获、边界校验与结果转换。
#include "MaiScreenshotTool.h"

#include <memory>
#include <string>
#include <utility>

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
    explicit MaiScreenshotTool(MaiScreenshotCapture capture = {}) : mCapture(std::move(capture)) {}
    std::string name() const override {
        return "screenshot";
    }

    std::string description() const override {
        // 工具只采集光标所在屏幕，不向模型提供窗口选择；capture 获取最近完整帧，默认执行此动作。
        // 原生会话可保持活跃，stop 释放采集会话和工作线程；停止不需要截图审批或工作区。
        // JSON 返回工作区相对路径、类型、尺寸和原始帧元数据；用同一文件交付，不用脚本重截。
        return "Capture the display containing the mouse pointer and inspect the returned image. "
               "Use action=capture (the default) to retrieve the latest complete native frame. "
               "The native capture session can remain active between calls; use action=stop "
               "to release the capture session and worker. Capture follows per-call approval; "
               "stop needs no screenshot approval or workspace. The JSON result includes "
               "image_path relative to this Agent workspace, type, mime_type, width, height, "
               "mode=display, frame_sequence, and frame_received_at_ms. Frame metadata reports "
               "when the native frame was received, not when it was encoded. To show this same "
               "screenshot in the current conversation, call agent_send_media with file_path "
               "set to the returned image_path and type=image. Do not recapture it with shell "
               "scripts.";
    }

    bool isAvailableForModel(const std::string& model) const override {
        // 与 view_image 共用当前已接线的文本模型边界，避免宣告不能回灌像素的截图工具。
        return model != "glm-5.3" && model.compare(0, 6, "local:") != 0;
    }

    std::string parametersSchema() const override {
        // action 只有 capture 和 stop；缺省 capture，旧窗口参数严格拒绝，不能回退到整屏采集。
        return R"({"type":"object","properties":{)"
               R"("action":{"type":"string","enum":["capture","stop"],"default":"capture"}},)"
               R"("required":[],"additionalProperties":false})";
    }

    bool requiresApproval(const std::string& raw) const override {
        return requiresPerCallApproval(raw);
    }

    bool requiresWorkingDirectory(const std::string& raw) const override {
        // 和审批使用同一合法 stop 判定，避免调度层在 execute 清理前因缺少工作目录而拒绝。
        return requiresPerCallApproval(raw);
    }

    bool requiresPerCallApproval(const std::string& raw) const override {
        // 只对完整且合法的 stop 免审批；畸形或附带其他参数的调用仍按默认拒绝方向处理。
        const json arguments = json::parse(raw, nullptr, false);
        return !arguments.is_object() || arguments.size() != 1 ||
               arguments.value("action", json{}) != "stop";
    }

    // 捕获涉及屏幕隐私；旧窗口参数在触碰采集源前拒绝，目标始终由普通屏幕请求确定。
    // stop 是清理操作，先于取消、模型视觉能力和工作区检查，避免取消后无法释放采集资源。
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
            if (field.key() != "action") {
                return MaiToolResult::failure(
                    MaiErrorCode::InvalidInput,
                    "screenshot received an unknown parameter: " + field.key());
            }
        }
        if (arguments.contains("action") && !arguments["action"].is_string()) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "screenshot action must be a string");
        }
        const std::string action = arguments.value("action", std::string("capture"));
        if (action == "stop") {
            const MaiError stopped = maiStopScreenshotCapture();
            if (stopped.hasError())
                return MaiToolResult::failure(stopped.code(), stopped.message());
            return MaiToolResult::success(json{{"action", "stop"}, {"state", "stopped"}}.dump());
        }
        if (action != "capture") {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "screenshot action must be capture or stop");
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
        MaiScreenshotRequest request;
        request.target = MaiScreenshotTarget::DisplayUnderPointer;
        request.isCanceled = [&context] { return context.isCanceled(); };
        const MaiError captureError =
            mCapture ? mCapture(request, screenshot) : maiCaptureScreenshot(request, screenshot);
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
        const std::string output = json{{"image_path", relativePath},
                                        {"type", "image"},
                                        {"mime_type", "image/png"},
                                        {"width", screenshot.width},
                                        {"height", screenshot.height},
                                        {"mode", "display"},
                                        {"frame_sequence", screenshot.frameSequence},
                                        {"frame_received_at_ms", screenshot.frameReceivedAtMs}}
                                       .dump();
        return MaiToolResult::successWithImages(output, {MaiToolImage{path.toUtf8(), "image/png"}});
    }

private:
    MaiScreenshotCapture mCapture;
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiScreenshotTool() {
    return std::make_unique<MaiScreenshotTool>();
}

std::unique_ptr<MaiTool> makeMaiScreenshotTool(MaiScreenshotCapture capture) {
    return std::make_unique<MaiScreenshotTool>(std::move(capture));
}
