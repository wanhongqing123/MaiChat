// 文件作用：移动端 C++ Agent 适配层；MaiMobileAgent 负责移动 Agent 生命周期、工具注册和宿主回调，本文件给出其实现。
// 直接依赖：MaiMobileHostTools.h、MaiAgent.h、MaiAgentSendMediaTool.h、MaiAppStorageTool.h；实现与这些接口协作。
#include "MaiMobileAgent.h"
#include "MaiMobileHostTools.h"
#include "MaiCloudDriveTools.h"
#include "MaiQrCode.h"
#include "MaiSessionSchedule.h"
#include "MaiCalendarTools.h"

#include "MaiAgent.h"
#include "MaiAgentProgressInstructions.h"
#include "MaiAgentSendMediaTool.h"
#include "MaiAppStorageTool.h"
#include "MaiArkAssetTools.h"
#include "MaiArkMediaTools.h"
#include "MaiCurlTools.h"
#include "MaiProviderWebTools.h"
#include "MaiCloudModelCatalog.h"
#include "MaiRoutedModelClient.h"
#include "MaiCvImageTools.h"
#include "MaiCvVideoAnalysis.h"
#include "MaiCvVideoTools.h"
#include "MaiFfmpegTools.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiFileTools.h"
#include "MaiGlmMediaTools.h"
#include "MaiMiniMaxMediaTools.h"
#include "MaiModelStudioTools.h"
#include "MaiNetworkIpTool.h"
#include "MaiOpenAiClient.h"
#include "MaiOpenSslCliTool.h"
#include "MaiOpenSslTool.h"
#include "MaiPdfTool.h"
#include "MaiIdGenerator.h"
#include "MaiLlamaTools.h"
#include "MaiLlamaModelClient.h"
#include "MaiLlamaModelValidation.h"
#include "MaiQuestionTool.h"
#include "MaiQuestionTranscript.h"
#include "MaiSessionTools.h"
#include "MaiSqliteStore.h"
#include "MaiSshTool.h"
#include "MaiSystemResourcesTool.h"
#include "MaiThread.h"
#include "MaiTime.h"
#include "MaiTemporalWatermark.h"
#include "MaiTemporalWatermarkTool.h"
#include "MaiTimeTool.h"
#include "MaiTodoWriteTool.h"
#include "MaiToolOutputTool.h"
#include "MaiVideoGeometryTool.h"
#include "MaiVideoMatting.h"
#include "MaiVideoMattingTool.h"
#include "MaiViewImageTool.h"
#include "MaiZlibTool.h"
#include "mai_fftools_embed.h"
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <json.hpp>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using Json = nlohmann::json;

namespace {

// 保留核心错误类型给宿主队列判断“明确未受理”。不能把未知异常或附件错误猜成Busy后重播。
class NativeOperationFailure final : public std::runtime_error {
public:
    explicit NativeOperationFailure(const MaiError& error) : std::runtime_error(error.message()), code(error.code()) {}
    MaiErrorCode code;
};

// 普通本地客户端测量只取消自己的 stream；任何返回/异常都先唤醒并回收截止线程。
// 不调用 Agent::stop，不接触主界面已有任务，也不把工具回调交给执行器。
class LocalModelProbeDeadline {
public:
    explicit LocalModelProbeDeadline(std::uint32_t milliseconds)
        : mThread([this, milliseconds] {
              MaiThread::setCurrentName("mai-probe-time");
              std::unique_lock<std::mutex> lock(mMutex);
              if (!mWake.wait_for(lock, std::chrono::milliseconds(milliseconds), [this] { return mFinished; }))
                  mCanceled.store(true);
          }) {}
    ~LocalModelProbeDeadline() {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mFinished = true;
        }
        mWake.notify_all();
        if (mThread.joinable()) mThread.join();
    }
    const std::atomic<bool>& canceled() const {
        return mCanceled;
    }

private:
    std::mutex mMutex;
    std::condition_variable mWake;
    bool mFinished = false;
    std::atomic<bool> mCanceled{false};
    std::thread mThread;
};

// 私有表单 token 只关联这一轮请求，不用作路径、账号或密码；与宿主 UUID 校验保持一致。
std::string pdfPasswordRequestId() {
    std::random_device random;
    std::array<unsigned char, 16> bytes{};
    for (auto& byte : bytes) byte = static_cast<unsigned char>(random());
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) result += '-';
        result += digits[bytes[index] >> 4];
        result += digits[bytes[index] & 15];
    }
    return result;
}

MaiResult<std::string> requestPrivatePdfPassword(const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher,
                                                 const std::atomic<bool>* cancel) {
    const std::string requestId = pdfPasswordRequestId();
    const auto invoke = [&](const char* action) {
        return callMaiMobileHostTool(dispatcher, "pdf_password_request",
                                     Json{{"action", action}, {"request_id", requestId}}.dump());
    };
    const auto dismiss = [&] { (void)invoke("cancel"); };
    try {
        if (cancel && cancel->load(std::memory_order_relaxed))
            return {MaiErrorCode::Canceled, "PDF password entry was canceled"};
        const auto begin = invoke("begin");
        if (begin.hasError()) {
            // 通用 dispatcher 的解析错误可能附带原始响应片段。此处绝不转发其 message/output。
            return {begin.error().code(), "Private PDF password entry is unavailable"};
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
        while (std::chrono::steady_clock::now() < deadline) {
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                dismiss();
                return {MaiErrorCode::Canceled, "PDF password entry was canceled"};
            }
            const auto response = invoke("poll");
            if (response.hasError()) {
                dismiss();
                return {response.error().code(), "Private PDF password entry failed"};
            }
            const Json output = Json::parse(response.output(), nullptr, false);
            if (!output.is_object() || !output.contains("status") || !output["status"].is_string()) {
                dismiss();
                return {MaiErrorCode::Protocol, "Private PDF password entry returned an invalid response"};
            }
            const std::string status = output["status"].get<std::string>();
            if (status == "ready" && output.contains("password") && output["password"].is_string())
                return output["password"].get<std::string>();
            if (status == "canceled") return {MaiErrorCode::Canceled, "PDF password entry was canceled"};
            if (status != "pending") {
                dismiss();
                return {MaiErrorCode::Protocol, "Private PDF password entry returned an invalid state"};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        dismiss();
        return {MaiErrorCode::Canceled, "PDF password entry timed out"};
    } catch (...) {
        // 密码响应只留在本次栈上；解析异常不带入工具结果/历史，关闭同 token 表单即可。
        dismiss();
        return {MaiErrorCode::Internal, "Private PDF password entry failed"};
    }
}

constexpr auto kMarkdownBaseInstructions =
    // 工具名和 description 只用于初筛；推荐前必须逐个调用候选视频工具的
    // discover， 读取 tool_status 和输入组合限制。厂商宣称支持不等于当前 App
    // 已接通。
    // 付费视频任务再确认模型及型号；遇到失败先判断原因并保持用户已选方案。
    "Video generation rule: before any paid video delegate, use discover to "
    "identify configured specialists with implemented capabilities for this "
    "request. Read the tool description for initial routing, then call "
    "discover "
    "on each plausible video specialist before recommending one. Compare the "
    "actual tool_status and input combination limits; model_support or "
    "api_support alone does not mean the App can execute a path. Use the JSON "
    "schema for exact fields. Do not infer a missing feature from a model "
    "name, "
    "marketing page, or another model's limits. If the user has not chosen a "
    "video model for this task, show "
    "suitable names and task-specific differences and ask the user to choose; "
    "confirm the sole suitable model too. Do not choose or switch video models "
    "silently. When one specialist offers multiple billable model variants, "
    "show their supported duration and resolution and confirm the variant too. "
    "Reuse a choice already made for this task, but ask before any "
    "replacement model's paid call. A model choice is separate from paid-call "
    "approval. Classify an input rejection before changing anything. If the "
    "cause is ambiguous, inspect the actual image and prompt without claiming "
    "which one failed. "
    // 人物替换应直接规划视频编辑；水印、字幕和片尾不是默认预处理任务。
    // 用户明确要求去水印时才用独立工具，一次处理后回到原目标，避免反复探测。
    "When the user asks to replace a person in a source video, keep the source "
    "video and references intact and plan the selected video model's edit "
    "path. "
    "Do not remove watermarks, subtitles, or end cards as an unrequested "
    "preparation step. If the user explicitly requests watermark removal, use "
    "the dedicated media_remove_watermark tool, if available, once with all "
    "measured regions, "
    "time windows, and clean reference timestamps. Inspect enough source "
    "frames "
    "to confirm moving positions and foreground overlap. Do not loop through "
    "FFmpeg/OpenCV repair calls. If that tool is unavailable, state the "
    "capability gap. After one result, review it and return to the original "
    "task. "
    // 图片与视频专业模型的本地素材统一走私有 OSS；上传故障不能回退为 Base64。
    "For image and video specialist tools, local media paths are upload "
    "inputs: "
    "the tool sends short-lived private OSS HTTPS URLs to the provider. If OSS "
    "upload fails, stop before paid submission and report the upload stage. Do "
    "not retry by embedding local image or video bytes as Base64. "
    // 可灵权限已开通，只走百炼入口并复用万相北京地域 Key；仍要确认具体型号。
    "For Kling video, use kling_bailian_video with the Beijing Wan API Key and "
    "Workspace. Confirm its Turbo, "
    "Standard, "
    "or Omni variant before paid submission. "
    "For Kling images use kling_bailian_image with the same Beijing "
    "credentials; "
    "for Wan images use wan_image. "
    // MiniMax image-01 的图生图是人物主体参考，不是任意局部修图。
    "For MiniMax images use minimax_image: no reference is text-to-image; one "
    "image_path or image_url is character-reference image-to-image. Do not "
    "describe "
    "that path as pixel-preserving editing or use it for arbitrary object "
    "replacement. "
    // 旧专用入口已停止新任务。Wan3 使用一张人物参考图和一段视频，
    // 通过提示词分别引导换人或动作模仿，但不能保证逐帧还原或严格保留背景。
    // 本地源视频先测时长，超过 Wan3 的 15 秒上限不能静默截短或付费提交。
    "For Wan person replacement use wan_video mode=edit, set video_path to the "
    "source "
    "video and local_image_paths to an array containing one person image. In "
    "message, "
    "ask to replace Video 1's person with Image 1 while preserving the scene. "
    "For "
    "motion guided by a source video use wan_video mode=reference with the "
    "same media "
    "fields; ask Image 1's person to follow Video 1's motion and expressions "
    "while "
    "keeping Image 1's background. These "
    "are prompt-guided approximations, not guaranteed "
    "frame-accurate replacement or motion transfer. Measure "
    "source_duration_seconds "
    "with ffprobe: Wan3 accepts at most 15 seconds of reference video, and "
    "input plus output must be at most 30 seconds. Use ratio=adaptive to "
    "preserve framing unless the user requests another ratio. Do not silently "
    "trim longer source footage. Use only wan_video for new Wan person "
    "replacement or "
    "motion-guided generation. "
    // Wan3 视频延长用 reference_video 和明确延长意图；比例必须 adaptive，
    // 付费前先测源片时长并核对输入与输出合计不超过 30 秒。
    "For Wan3 extension use wan_video mode=extend with one source video, an "
    "explicit "
    "extension direction in the message, ratio=adaptive, and measured "
    "source_duration_seconds. Input plus requested output must be at most 30 "
    "seconds. "
    // 仅在允许的前提下处理图像：轻度处理优先，油画化是兜底；记录损失并补偿描述。
    "Do not default to "
    "flipping or oil-painting a real face to evade review. For other image "
    "issues, if an edit "
    "is allowed and still serves the user's goal, preserve the original, make "
    "a light "
    "FFmpeg/OpenCV edit, preview it, "
    "and retry the selected model with new paid approval. If image-related "
    "rejection persists, consider an FFmpeg oil-paint-style derivative and "
    "describe permitted details lost in processing. Do not switch models "
    "before these bounded same-model attempts. If the provider explicitly "
    "forbids a recognizable real-person reference without an authorized asset "
    "and the user needs that person's identity, do not transform the photo to "
    "hide the face and reconstruct it in the prompt. Keep the selected model; "
    "briefly offer its authorized-asset path or a visibly fictional character. "
    "Offer another model only if the user wants the original person's "
    "likeness, "
    "and ask before switching. Avoid lengthy technical error narration. "
    // 排障日志只在用户要求时导出到工作区，工具返回文件卡片；不把日志正文
    // 粘进会话，也不代替用户发送给联系人。
    "When the user asks to export troubleshooting logs, call "
    "mobile_export_diagnostics once and tell them to use the resulting "
    "shareable file card. Do not paste the log contents into the chat or "
    "send the file to a contact automatically. "
    // 消息用真正的 Markdown 换行；图像附件直接看，缺 OS 权限先申请再重试。
    "Write user-facing responses in valid GitHub-Flavored Markdown. Preserve "
    "real line breaks. "
    "For tables, put the header, separator, and every row on separate lines, "
    "with a blank line "
    "before and after the table. Use headings, lists, fenced code blocks, and "
    "tables only when "
    "they improve readability. Never emit table pipes as one continuous line. "
    // 用户消息自带的图像已作为视觉输入；相册编辑必须导出原件，预览图不能当源图。
    "Images included "
    "in a user message are already available as visual input; analyze them "
    "directly and do not "
    "call the read tool for image files. When a mobile task lacks an OS "
    "permission, call "
    "mobile_request_permission for that capability and retry once if granted. "
    "Android mobile_dial_phone requests CALL_PHONE itself; if the user denies it, "
    "do not prompt again for the same request. "
    "A limited grant "
    "may still allow the operation, such as reading selected photos or using "
    "approximate "
    "location; retry once when that access is sufficient. If the OS reports "
    "settings_required, "
    "tell the user which permission to enable instead of retrying in a loop. "
    "For any gallery "
    "photo edit, use mobile_export_photo_original as the input; "
    "mobile_read_photo returns a "
    "bounded JPEG preview that loses resolution and wide-gamut metadata. "
    "When the user writes in Chinese, write the final answer and any reasoning "
    "text exposed by the model in Simplified Chinese. Preserve code, paths, "
    "commands, and quoted tool output in their original form. "
    "Do not invent model quality, policy, or price claims. "
    "Use mobile_dial_phone only when the user explicitly asks to call. On Android, "
    "the operating system may ask once for CALL_PHONE permission, then the call "
    "starts without a second MaiChat approval. On iOS, the Phone app always asks "
    "the user to confirm. Never claim a call connected based on a requested or "
    "opened status. mobile_compose_sms only opens the iOS Messages composer; "
    "the user decides whether to send it. "
    // 付费调用前展示人物、场景和规格；闸门是最终确认，不能悄悄改时长或分辨率。
    "Before calling delegate or revise on any paid image or video generation "
    "specialist, "
    "show the user the concrete plan and let the tool's per-call approval be "
    "the final "
    "confirmation. For video, name subject, clothing, scene, action, duration, "
    "resolution, "
    "and aspect ratio. Ask for missing duration, resolution, or aspect ratio "
    "instead of "
    "silently using defaults. Read the selected specialist's actual duration "
    "range; "
    "do not assume a universal 10-second limit or shorten a longer request. "
    "Match the paid call to the shown plan: preserve "
    "the intended subject, clothing, scene, action, reference count, duration, "
    "aspect ratio, and the provider's actual billable resolution. Check "
    "discover or local validation before approval; never pass an unsupported "
    "resolution or silently change it. If exact output resolution requires "
    "local conversion, explain both steps before approval. If the user "
    "rejects the approval, do not submit "
    "or retry. "
    // 语义创作交给生成模型；裁剪、几何、色彩和速度调整优先使用本地工具。
    "Use video/image model tools for semantic changes such as person, clothes, "
    "scene, or "
    "action. Prefer local FFmpeg/OpenCV for geometry, crop, color, "
    "stabilization, frame "
    "joining, or speed changes so identity pixels can remain untouched. Reuse "
    "an existing "
    "cloud task ID when checking progress; do not submit a paid duplicate. "
    // 取消与删除必须走已保存的任务上下文；取消仅限供应商仍在排队的任务。
    // 百炼没有云端视频任务删除 API，其 delete 只移除已交付的本地记录。
    "When the user asks to cancel a video job, call an available specialist's "
    "cancel action with its saved conversation_id. Model Studio can cancel only "
    "queued/PENDING jobs; if the provider says it has started, preserve the task "
    "and continue polling. Wan and Bailian Kling delete only the delivered local task "
    "record, leaving cloud results and downloaded videos intact. GLM and "
    "MiniMax also allow local-record deletion after delivery, but no verified "
    "provider cancel API is wired for them; never claim a local stop canceled "
    "their cloud job. "
    // 多图任务先查模型上限；超出时用图像融合或可见拼接，让每张输入都进入流程。
    "For multiple user images, inspect the chosen video specialist's input "
    "limits and never "
    "silently drop an image. If the limit is exceeded, discover an available "
    "image specialist for semantic fusion or use FFmpeg for a visible layout, then show the plan "
    "before the paid "
    "call. On task failure, inspect the provider stage and reason, repair "
    "technical inputs "
    "when possible, and seek a new per-call approval before any paid retry. Do "
    "not loop on "
    "a provider content rejection or imply that an ambiguous error identifies "
    "one input. "
    // 子工具失败先由主模型处理；欠费找替代能力，状态未知保留任务 ID
    // 防重复扣费。
    "Treat a specialist error as internal work to resolve, not an immediate "
    "user-facing "
    "answer. Keep the original task and media references. If one provider "
    "lacks balance or "
    "access, discover another capable configured specialist and continue the "
    "task there only after the user confirms a replacement video model when "
    "this is video generation; "
    "never ask the user to recharge while a practical alternative remains. If "
    "an accepted "
    "task's status is unknown, preserve its task ID and do not silently submit "
    "a duplicate. "
    "After delivering the result, you may briefly explain any provider change. "
    "Only if no "
    "viable route remains should you tell the user why the task could not be "
    "completed. "
    // 模糊图像审核先辨别实际输入，再在同一模型内有界处理和重试，换模型放最后。
    "For ambiguous image-related moderation failures, preserve the original "
    "and inspect the actual image and prompt. Try a light permitted image-tool "
    "edit first, "
    "then retry the same provider after a new paid approval. Only if that "
    "fails and image evidence remains, try a stronger FFmpeg oil-paint-style "
    "edit as the "
    "last image-processing step. Preview each derivative, record what visual "
    "details were lost, and describe permitted details accurately in the "
    "next prompt so the result stays close to the user's goal. Do not infer "
    "an image problem from an ambiguous error, disguise explicitly prohibited "
    "material, reconstruct a prohibited identity, or promise an edit will pass "
    "moderation. Consider another "
    "provider only after bounded same-provider attempts fail.";

std::string encodeBase64(const std::string& input) {
    constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);
    for (std::size_t offset = 0; offset < input.size(); offset += 3) {
        const auto first = static_cast<unsigned char>(input[offset]);
        const auto second = offset + 1 < input.size() ? static_cast<unsigned char>(input[offset + 1]) : 0;
        const auto third = offset + 2 < input.size() ? static_cast<unsigned char>(input[offset + 2]) : 0;
        output.push_back(kAlphabet[first >> 2]);
        output.push_back(kAlphabet[((first & 0x03) << 4) | (second >> 4)]);
        output.push_back(offset + 1 < input.size() ? kAlphabet[((second & 0x0F) << 2) | (third >> 6)] : '=');
        output.push_back(offset + 2 < input.size() ? kAlphabet[third & 0x3F] : '=');
    }
    return output;
}

// 仅宿主提供已授权的沙箱路径；与 configure 共用相同 GGUF/模板/预算预检，不加载权重。
MaiLlamaModelConfig validatedMobileLocalConfig(const Json& request, const std::string& workspace,
                                               const std::string& appRoot) {
    MaiLlamaModelConfig local;
    MaiToolContext paths;
    paths.root = workspace;
    paths.fileAccessRoot = appRoot;
    const auto authorizedModelFile = [&](const char* field, bool optional) -> MaiFilePath {
        if (!request.contains(field)) {
            if (optional) return {};
            throw NativeOperationFailure(MaiError::make(MaiErrorCode::InvalidInput, "A local GGUF file is required."));
        }
        if (!request[field].is_string())
            throw NativeOperationFailure(
                MaiError::make(MaiErrorCode::InvalidInput, "Local model paths must be strings."));
        const auto raw = request[field].get<std::string>();
        if (raw.empty() && optional) return {};
        const auto path = paths.resolvePath(raw);
        if (path.empty())
            throw NativeOperationFailure(
                MaiError::make(MaiErrorCode::InvalidInput, "Local model file is outside the application root."));
        const auto file = MaiFilePath::fromUtf8(path);
        std::uint64_t size = 0;
        if (!MaiFileSystem::fileSize(file, size) || MaiFileSystem::isDirectory(file) || size < 24)
            throw NativeOperationFailure(
                MaiError::make(MaiErrorCode::NotFound, "Local GGUF file is missing or unavailable."));
        return file;
    };
    const auto boundedNumber = [&](const char* field, std::uint32_t fallback, std::uint32_t lower,
                                   std::uint32_t upper) -> std::uint32_t {
        if (!request.contains(field)) return fallback;
        const auto& value = request[field];
        std::uint64_t parsed = 0;
        if (value.is_number_unsigned())
            parsed = value.get<std::uint64_t>();
        else if (value.is_number_integer() && value.get<std::int64_t>() >= 0)
            parsed = static_cast<std::uint64_t>(value.get<std::int64_t>());
        else
            throw NativeOperationFailure(
                MaiError::make(MaiErrorCode::InvalidInput, "Local inference limits must be integers."));
        if (parsed < lower || parsed > upper)
            throw NativeOperationFailure(
                MaiError::make(MaiErrorCode::InvalidInput, "Local inference limit is out of range."));
        return static_cast<std::uint32_t>(parsed);
    };
    local.modelPath = authorizedModelFile("localModelPath", false);
    local.projectorPath = authorizedModelFile("localProjectorPath", true);
    if (!local.projectorPath.isEmpty())
        throw NativeOperationFailure(MaiError::make(
            MaiErrorCode::NotSupported,
            "Local primary models currently support text and tools; use the local inference test for images."));
    // 完整移动工具目录的首轮模板实测已超过 23k token；默认 8k 会在第一轮必然失败。
    // 保留显式较小预算的严格错误，不裁历史/工具来伪装适配。
    local.contextSize = boundedNumber("localContextSize", 32768, 128, 32768);
    // 0 交由模型自然结束，最大只受实际上下文余量约束；保留显式预算供普通调用/测试。
    local.maximumTokens = boundedNumber("localMaximumTokens", 0, 0, 4096);
    local.threads = boundedNumber("localThreads", 2, 1, 64);
    if (local.maximumTokens >= local.contextSize)
        throw NativeOperationFailure(
            MaiError::make(MaiErrorCode::InvalidInput, "Generation tokens must be smaller than local context size."));
    // 有界预检 GGUF 元数据和原生工具模板；失败时旧 Agent/设置仍保持有效。
    // 不加载张量，也不把预检通过当作权重质量、内存或实际推理成功保证。
    const auto checked = validateMaiLlamaModelConfig(local);
    if (checked) throw NativeOperationFailure(checked);
    return local;
}

struct MaiMobileLocalRouteState {
    std::mutex mutex;
    std::optional<MaiLlamaModelConfig> config;
};

char* ownedResponse(const std::string& response) {
    char* result = static_cast<char*>(std::malloc(response.size() + 1));
    if (result) std::memcpy(result, response.c_str(), response.size() + 1);
    return result;
}

}  // namespace

// 这是移动端适配器，JSON 只在语言边界，MaiAgent 的公开接口仍是领域对象。
struct MaiMobileAgent {
    std::mutex mutex;
    bool dirty = true;
    bool structureChanged = true;
    std::string cachedSession;
    int cachedLimit = -1;
    std::vector<MaiMessage> cachedMessages;
    MaiQuestionTranscript cachedQuestionTranscript;
    // 远端发布每 150ms 可能读取一次；不能复用本机被消费的 dirty/structureChanged。
    std::atomic<std::uint64_t> questionStructureRevision{1};
    std::uint64_t remoteQuestionRevision = 0;
    std::unordered_map<std::string, std::vector<MaiQuestionTranscriptMessage>> remoteQuestionHistory;
    // Android 有限页的更新游标独立于 latest 缓存；summary/iOS 查询不能消费旧页增量。
    bool trackMessageUpdates = false;
    std::unordered_map<std::string, std::unordered_set<std::string>> pendingMessageUpdates;
    // 同步生产方独立登记/消费；普通 runtime 查询和本机 UI 不能吃掉远端旧 owner 更新。
    std::unordered_set<std::string> remoteTrackedRoots;
    std::unordered_map<std::string, std::unordered_set<std::string>> remotePendingMessageUpdates;
    std::unordered_map<std::string, std::string> pendingDeltas;
    std::unordered_map<std::string, std::string> liveParts;
    std::unordered_map<std::string, std::string> errors;
    std::string workspace;
    std::string model;
    MaiWireApi primaryWire = MaiWireApi::ChatCompletions;
    // 普通建议与 Agent 使用同一种已配置客户端；本地模式不能绕回云端生成。
    std::function<std::unique_ptr<MaiModelClient>()> modelClientFactory;
    std::vector<MaiToolSpec> modelProbeTools;
    std::string modelProbeInstructions;
    bool configured = false;
    // 目录只在账号 configure 时替换；会话选择仅写 session，不重建 Agent/epoch。
    MaiRoutedModelConfig routeDirectory;
    std::string defaultReasoningEffort;
    std::string localModelName;
    bool localSupportsImages = false;
    std::shared_ptr<MaiMobileLocalRouteState> localRouteState;
    std::string fileAccessRoot;
    const void* ortApiBase = nullptr;
    std::shared_ptr<MaiMobileHostDispatcher> hostTools = makeMaiMobileHostDispatcher();
    MaiMobileAgentChangeHandler changeHandler = nullptr;
    std::shared_ptr<MaiSessionScheduleStore> scheduleStore;
    std::unique_ptr<MaiSessionScheduleRunner> scheduleRunner;
    std::thread scheduleThread;
    std::mutex scheduleMutex;
    std::condition_variable scheduleWake;
    std::atomic<bool> stopSchedule{false};
    struct ScheduleEvent {
        std::string sessionId;
        MaiEventType type;
        std::string error;
    };
    std::vector<ScheduleEvent> scheduleEvents;
    // 最后销毁 agent：回调捕获的字段必须活到工作线程退出。
    std::unique_ptr<MaiAgent> agent;

    ~MaiMobileAgent() {
        stopSchedulePolling();
        agent.reset();
        clearMaiMobileHostToolHandler(hostTools);
    }

    static std::string result(MaiResult<std::string> value) {
        if (!value) throw NativeOperationFailure(value.error());
        return value.value();
    }

    bool anyBusy() const {
        if (!agent) return false;
        for (const auto& session : agent->listSessions())
            if (agent->isBusy(session.id)) return true;
        return false;
    }

    void stopSchedulePolling() {
        stopSchedule.store(true, std::memory_order_relaxed);
        scheduleWake.notify_all();
        if (scheduleThread.joinable()) scheduleThread.join();
    }

    void startSchedulePolling() {
        if (!scheduleStore || !agent) return;
        if (!scheduleRunner) {
            scheduleStore->markInterruptedUncertain();
            scheduleRunner = std::make_unique<MaiSessionScheduleRunner>(
                scheduleStore,
                [this](const std::string& sessionId) {
                    MaiSession session;
                    return agent->getSession(sessionId, session) && session.isRoot();
                },
                [this](const std::string& sessionId) { return !configured || agent->isBusy(sessionId); },
                [this](const std::string& sessionId, const std::string& instruction) -> MaiResult<std::string> {
                    return agent->submit(MaiSendPrompt{sessionId, instruction});
                });
        }
        stopSchedule.store(false, std::memory_order_relaxed);
        scheduleThread = std::thread([this] {
            MaiThread::setCurrentName("mai-schedule");
            while (!stopSchedule.load(std::memory_order_relaxed)) {
                std::vector<ScheduleEvent> events;
                {
                    std::lock_guard<std::mutex> lock(scheduleMutex);
                    events.swap(scheduleEvents);
                }
                for (const auto& event : events) {
                    if (event.type == MaiEventType::SessionDeleted)
                        scheduleStore->cancelSession(event.sessionId);
                    else if (event.type == MaiEventType::SessionError)
                        scheduleRunner->finish(event.sessionId, event.error);
                    else if (event.type == MaiEventType::SessionIdle)
                        scheduleRunner->finish(event.sessionId);
                }
                scheduleRunner->poll(MaiTime::getCurrentTime());
                std::unique_lock<std::mutex> lock(scheduleMutex);
                scheduleWake.wait_for(lock, std::chrono::seconds(1), [this] {
                    return stopSchedule.load(std::memory_order_relaxed) || !scheduleEvents.empty();
                });
            }
        });
    }

    // 作用：按宿主配置装配移动 Agent 的模型、工具和工作区能力。
    // 先校验外部输入，再更新本地状态；失败路径不能覆盖已有有效结果。
    void configure(const Json& request) {
        // 切换主模型时不能中断正在运行的任务；先创建共享存储，再配置线协议和工具集。
        if (anyBusy()) throw std::runtime_error("Stop active tasks before changing the model.");
        const std::string databasePath = request.at("database").get<std::string>();
        auto store = makeMaiSqliteStore(databasePath);
        if (!store) throw std::runtime_error(store.error().message());
        const auto scheduleOpened = MaiSessionScheduleStore::open(
            databasePath == ":memory:" ? databasePath : databasePath + ".schedule.sqlite");
        const auto nextScheduleStore =
            scheduleOpened.isOk() ? scheduleOpened.value() : std::shared_ptr<MaiSessionScheduleStore>();
        if (request.contains("modelProvider") && !request["modelProvider"].is_string())
            throw NativeOperationFailure(
                MaiError::make(MaiErrorCode::InvalidInput, "Model provider must be a string."));
        const std::string provider = request.value("modelProvider", std::string("remote"));
        if (provider != "remote" && provider != "llama")
            throw NativeOperationFailure(MaiError::make(MaiErrorCode::InvalidInput, "Unsupported model provider."));
        const bool isLocal = provider == "llama";
        MaiModelConfig config;
        config.baseUrl = isLocal ? std::string{} : request.at("baseUrl").get<std::string>();
        if (!isLocal && config.baseUrl.empty())
            throw NativeOperationFailure(
                MaiError::make(MaiErrorCode::InvalidInput, "A remote model endpoint is required."));
        config.apiKey = request.value("apiKey", "");
        config.caBundlePath = request.value("caBundle", "");
        // Responses/Chat 是主模型线协议，和 Seedance 等专业工具的供应商 API 无关。
        const std::string wire = request.value("wire", std::string{});
        if (isLocal)
            config.wire = MaiWireApi::Local;
        else if (wire == "responses" || (wire.empty() && request.value("model", std::string{}) == "deepseek-flash"))
            config.wire = MaiWireApi::Responses;
        else if (!wire.empty() && wire != "chat_completions")
            throw std::runtime_error("Unsupported model wire protocol.");
        MaiFfmpegEngine ffmpegEngine{mai_ffmpeg_execute,       mai_ffmpeg_set_cancel_check,
                                     mai_ffprobe_execute,      mai_ffprobe_set_cancel_check,
                                     mai_fftools_set_log_sink, mai_fftools_error_string};
        config.prepareImage = makeMaiFfmpegModelImagePreparer(ffmpegEngine);
        std::function<std::unique_ptr<MaiModelClient>()> nextModelClientFactory;
        std::optional<MaiModelRoute> nextLocalRoute;
        bool nextLocalSupportsImages = false;
        const auto requestedModel = request.at("model").get<std::string>();
        const auto defaultCapability = maiCloudModelInfo(requestedModel);
        if (!isLocal && defaultCapability) {
            config.wire = defaultCapability->wire;
            // 旧 GLM Responses 官方配置只在这个精确已知迁移点改到兼容 Coding Plan 的 Chat。
            if (defaultCapability->provider == "glm" && config.baseUrl == "https://open.bigmodel.cn/api/v1")
                config.baseUrl = "https://open.bigmodel.cn/api/paas/v4";
        }
        const bool hasLocalConfiguration = isLocal || !request.value("localModelPath", std::string{}).empty();
        const auto nextLocalName = isLocal ? requestedModel : request.value("localModelName", std::string{});
        auto nextLocalState = std::make_shared<MaiMobileLocalRouteState>();
        if (hasLocalConfiguration) {
            nextLocalState->config =
                validatedMobileLocalConfig(request, request.at("workspace").get<std::string>(),
                                           request.value("appRoot", request.at("workspace").get<std::string>()));
            nextLocalSupportsImages = !nextLocalState->config->projectorPath.isEmpty();
        }
        if (hasLocalConfiguration || !nextLocalName.empty()) {
            if (nextLocalName.empty() || nextLocalName.rfind("local:", 0) != 0)
                throw NativeOperationFailure(
                    {MaiErrorCode::InvalidInput, "A local route needs its explicit local: identifier."});
            MaiModelRoute route;
            route.modelNames = {nextLocalName};
            route.config.wire = MaiWireApi::Local;
            route.config.contextLimits = [state = nextLocalState](const std::string&) {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (!state->config) return MaiModelContextLimits{};
                const auto& local = *state->config;
                return MaiModelContextLimits{local.contextSize,
                                             local.maximumTokens != 0 ? local.maximumTokens : local.contextSize - 1,
                                             local.maximumTokens != 0 ? local.maximumTokens : local.contextSize / 8};
            };
            route.clientFactory = [state = nextLocalState]() -> std::unique_ptr<MaiModelClient> {
                std::optional<MaiLlamaModelConfig> local;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    local = state->config;
                }
                return local ? makeMaiLlamaModelClient(*local) : nullptr;
            };
            nextLocalRoute = std::move(route);
        }
        // 账号提供器在实际请求线程读取。缺失 GLM 不能使用 DeepSeek 或旧快照 Key 顶替。
        const auto accountKey = [dispatcher = hostTools](const std::string& provider) -> std::string {
            const auto response = callMaiMobileHostTool(dispatcher, provider + "_api_key", "{}");
            if (response.hasError()) return {};
            const auto parsed = Json::parse(response.output(), nullptr, false);
            return parsed.is_object() && parsed.value("key", Json{}).is_string() ? parsed["key"].get<std::string>()
                                                                                 : std::string{};
        };
        MaiRoutedModelConfig nextRoutes;
        for (const std::string providerName : {"glm", "deepseek"}) {
            MaiModelRoute route;
            route.modelNames = providerName == "glm" ? std::vector<std::string>{"glm-5.3", "glm-5.3-flash"}
                                                     : std::vector<std::string>{"deepseek-flash", "deepseek-v4-pro"};
            route.config = config;
            route.config.apiKey.clear();
            route.config.wire = providerName == "glm" ? MaiWireApi::ChatCompletions : MaiWireApi::Responses;
            const char* field = providerName == "glm" ? "glmBaseUrl" : "deepseekBaseUrl";
            const char* official =
                providerName == "glm" ? "https://open.bigmodel.cn/api/paas/v4" : "https://api.deepseek.com";
            route.config.baseUrl = request.value(field, defaultCapability && defaultCapability->provider == providerName
                                                            ? config.baseUrl
                                                            : std::string(official));
            route.apiKeyProvider = [accountKey, providerName] { return accountKey(providerName); };
            nextRoutes.routes.push_back(std::move(route));
        }
        if (nextLocalRoute) nextRoutes.routes.push_back(*nextLocalRoute);
        if (!isLocal && !defaultCapability) {
            // 普通 C ABI 仍可显式配置自有端点/型号（含合成测试），只允许该精确型号。
            // 产品四型号始终走以上账号提供器；这不是账号缺失时的另一条业务兜底。
            MaiModelRoute route;
            route.modelNames = {requestedModel};
            route.config = config;
            nextRoutes.routes.push_back(std::move(route));
        }
        nextModelClientFactory = [nextRoutes] { return makeMaiRoutedModelClient(nextRoutes); };
        auto tools = std::make_unique<MaiToolRegistry>();
        // 两个移动端只提供真实可用的本地文件和网络工具，不暴露桌面 shell。
        registerMaiFileTools(*tools);
        // 移动宿主的媒体与导出工具仍使用统一的默认工作目录，不能只切换 Agent 会话路径。
        registerMaiSessionTools(*tools, /*includeWorkspaceSwitch=*/false);
        // 移动端手工注册表也必须提供只读回查，才能消费Runner返回的原始输出句柄。
        tools->add(makeMaiToolOutputReadTool());
        registerMaiCurlTools(*tools, config.caBundlePath);
        // 与普通 C ABI 共用进程内的唯一驻留模型；主聊天仍直连原供应商。
        registerMaiLlamaTools(*tools);
        // 检索由供应商API处理；凭据每次从所属账号读取，不用curl抓搜索引擎HTML。
        MaiProviderWebOptions webOptions;
        webOptions.deepseek.caBundlePath = config.caBundlePath;
        webOptions.glm.caBundlePath = config.caBundlePath;
        webOptions.deepseek.apiKey = [accountKey] { return accountKey("deepseek"); };
        webOptions.glm.apiKey = [accountKey] { return accountKey("glm"); };
        registerMaiProviderWebTools(*tools, std::move(webOptions));
        tools->add(makeMaiZlibCompressTool());
        tools->add(makeMaiZlibDecompressTool());
        tools->add(makeMaiOpenSslCliTool());
        tools->add(makeMaiSystemResourcesTool([dispatcher = hostTools]() -> std::optional<MaiGpuResources> {
            const MaiToolResult response = callMaiMobileHostTool(dispatcher, "mobile_gpu_info", "{}");
            if (response.hasError()) return std::nullopt;
            const Json parsed = Json::parse(response.output(), nullptr, false);
            if (!parsed.is_object() || !parsed.value("available", false)) return std::nullopt;
            MaiGpuResources gpu;
            gpu.name = parsed.value("name", std::string{});
            gpu.unifiedMemory = parsed.value("unified_memory", false);
            if (parsed.value("app_allocated_bytes", Json{}).is_number_unsigned())
                gpu.appAllocatedBytes = parsed["app_allocated_bytes"].get<std::uint64_t>();
            if (parsed.value("recommended_working_set_bytes", Json{}).is_number_unsigned())
                gpu.recommendedWorkingSetBytes = parsed["recommended_working_set_bytes"].get<std::uint64_t>();
            return gpu;
        }));
        tools->add(makeMaiNetworkIpTool(config.caBundlePath));
        if (request.value("temporaryDirectory", Json{}).is_string() &&
            request.value("cacheDirectory", Json{}).is_string()) {
            tools->add(makeMaiAppStorageTool({request["temporaryDirectory"].get<std::string>(),
                                              request["cacheDirectory"].get<std::string>(),
                                              request.at("workspace").get<std::string>()}));
        }
        const auto sshPassword = [dispatcher = hostTools](const std::string& host, int port,
                                                          const std::string& username) -> MaiResult<std::string> {
            const MaiToolResult response = callMaiMobileHostTool(
                dispatcher, "mobile_ssh_password", Json{{"host", host}, {"port", port}, {"username", username}}.dump());
            if (response.hasError()) return response.error();
            const Json parsed = Json::parse(response.output(), nullptr, false);
            if (!parsed.is_object() || !parsed.value("password", Json{}).is_string())
                return {MaiErrorCode::Protocol, "SSH password dialog returned no password"};
            return parsed["password"].get<std::string>();
        };
        const auto sshTrust = [dispatcher = hostTools](const std::string& host, int port,
                                                       const std::string& fingerprint) -> MaiResult<bool> {
            const MaiToolResult response =
                callMaiMobileHostTool(dispatcher, "mobile_ssh_trust_host",
                                      Json{{"host", host}, {"port", port}, {"fingerprint", fingerprint}}.dump());
            if (response.hasError()) return response.error();
            const Json parsed = Json::parse(response.output(), nullptr, false);
            if (!parsed.is_object() || !parsed.value("trusted", Json{}).is_boolean())
                return {MaiErrorCode::Protocol, "SSH host-key dialog returned no decision"};
            return parsed["trusted"].get<bool>();
        };
        tools->add(makeMaiSshTool(sshPassword, sshTrust));
        tools->add(makeMaiAgentSendMediaTool());
        tools->add(makeMaiQuestionTool());
        tools->add(makeMaiCurrentTimeTool());
        tools->add(makeMaiTodoWriteTool());
        tools->add(makeMaiViewImageTool(makeMaiFfmpegImagePreview(ffmpegEngine)));
        tools->add(makeMaiFfmpegTool(ffmpegEngine));
        tools->add(makeMaiFfprobeTool(ffmpegEngine));
        tools->add(makeMaiStretchLowerVideoTool(ffmpegEngine));
        tools->add(makeMaiCvSceneDetectTool(analyzeMaiCvVideo));
        tools->add(makeMaiCvMotionDetectTool(analyzeMaiCvVideo));
        tools->add(makeMaiCvImageQualityTool(analyzeMaiCvImage));
        tools->add(makeMaiCvImageCompareTool(analyzeMaiCvImage));
        tools->add(makeMaiCvFindContoursTool(analyzeMaiCvImage));
        tools->add(makeMaiCvDetectEdgesTool(analyzeMaiCvImage));
        tools->add(makeMaiCvTemplateMatchTool(analyzeMaiCvImage));
        tools->add(makeMaiCvRegisterTranslationTool(analyzeMaiCvImage));
        tools->add(makeMaiCvDetectLinesTool(analyzeMaiCvImage));
        tools->add(makeMaiCvDocumentCornersTool(analyzeMaiCvImage));
        tools->add(makeMaiCvThresholdMaskTool(analyzeMaiCvImage));
        const auto arkKey = [dispatcher = hostTools]() -> std::string {
            const MaiToolResult result = callMaiMobileHostTool(dispatcher, "ark_api_key", "{}");
            if (result.hasError()) return {};
            const Json response = Json::parse(result.output(), nullptr, false);
            return response.is_object() && response.value("key", Json{}).is_string()
                       ? response["key"].get<std::string>()
                       : std::string{};
        };
        const auto assetSettings = [dispatcher = hostTools]() -> MaiResult<MaiArkAssetServiceSettings> {
            const MaiToolResult response = callMaiMobileHostTool(dispatcher, "mobile_ark_service_config", "{}");
            if (response.hasError()) return response.error();
            const Json value = Json::parse(response.output(), nullptr, false);
            if (!value.is_object() || !value.value("base_url", Json{}).is_string() ||
                !value.value("token", Json{}).is_string())
                return {MaiErrorCode::Protocol, "Ark Assets service configuration is invalid"};
            return MaiArkAssetServiceSettings{value["base_url"].get<std::string>(), value["token"].get<std::string>()};
        };
        // 本地主模型初始化不探测云端账号或刷新令牌。只绑定现有延迟回调，
        // 用户随后明确调用云媒体工具时才按所属账号查询凭据/审批；素材仍由客户端直传。
        // 远端模型保留原有能力探测，各模型仍写入自己的明确 OSS 目录。
        const bool assetsConfigured = isLocal || static_cast<bool>(assetSettings());
        const auto modelUploadMedia = [&](MaiOssUploadPurpose purpose) {
            return assetsConfigured ? makeMaiPrivateOssMediaUploader(assetSettings, config.caBundlePath, purpose)
                                    : MaiCreativeMediaUploadProvider{};
        };
        const MaiCreativeMediaUploadProvider seedreamUploadMedia = modelUploadMedia(MaiOssUploadPurpose::SeedreamInput);
        const MaiCreativeMediaUploadProvider miniMaxUploadMedia = modelUploadMedia(MaiOssUploadPurpose::MiniMaxInput);
        const MaiCreativeMediaUploadProvider klingUploadMedia = modelUploadMedia(MaiOssUploadPurpose::KlingInput);
        const MaiCreativeMediaUploadProvider wanUploadMedia = modelUploadMedia(MaiOssUploadPurpose::WanInput);
        const MaiArkAssetProvider assetProvider =
            assetsConfigured ? makeMaiArkAssetServiceProvider(assetSettings, config.caBundlePath)
                             : MaiArkAssetProvider{};
        const MaiVideoMetadataProvider seedanceVideoMetadata = makeMaiFfprobeVideoMetadataProvider(ffmpegEngine);
        if (assetsConfigured) {
            // Seedance 本地素材由同一 C++ 资产通道完成 OSS 上传、Ark 入库与状态查询。
            // 密钥和服务令牌由宿主提供；工具拿到的是回调，不把凭据写进模型提示词。
            // 对主模型隐藏，后台仍按工具名轮询已提交的 Seedance 任务。
            tools->addInternal(
                makeMaiSeedanceVideoTool(arkKey, config.caBundlePath, assetProvider, seedanceVideoMetadata));
            tools->addInternal(makeMaiArkAssetTool(assetProvider));
        } else {
            tools->addInternal(makeMaiSeedanceVideoTool(arkKey, config.caBundlePath, {}, seedanceVideoMetadata));
            tools->addInternal(makeMaiArkAssetTool());
        }
        tools->addInternal(makeMaiSeedreamImageTool(arkKey, config.caBundlePath, seedreamUploadMedia));
        const auto glmKey = [dispatcher = hostTools]() -> std::string {
            const MaiToolResult result = callMaiMobileHostTool(dispatcher, "glm_api_key", "{}");
            if (result.hasError()) return {};
            const Json response = Json::parse(result.output(), nullptr, false);
            return response.is_object() && response.value("key", Json{}).is_string()
                       ? response["key"].get<std::string>()
                       : std::string{};
        };
        tools->add(makeMaiGlmImageTool(glmKey, config.caBundlePath));
        const auto miniMaxKey = [dispatcher = hostTools]() -> std::string {
            const MaiToolResult result = callMaiMobileHostTool(dispatcher, "minimax_api_key", "{}");
            if (result.hasError()) return {};
            const Json response = Json::parse(result.output(), nullptr, false);
            return response.is_object() && response.value("key", Json{}).is_string()
                       ? response["key"].get<std::string>()
                       : std::string{};
        };
        tools->add(makeMaiMiniMaxVideoTool(miniMaxKey, config.caBundlePath, miniMaxUploadMedia));
        tools->add(makeMaiMiniMaxImageTool(miniMaxKey, config.caBundlePath, miniMaxUploadMedia));
        const auto wanCredentials = [dispatcher = hostTools]() -> MaiWanCredentials {
            const MaiToolResult result = callMaiMobileHostTool(dispatcher, "wan_credentials", "{}");
            if (result.hasError()) return {};
            const Json response = Json::parse(result.output(), nullptr, false);
            if (!response.is_object()) return {};
            return {response.value("key", std::string{}), response.value("workspace_id", std::string{})};
        };
        tools->add(makeMaiWanVideoTool(wanCredentials, config.caBundlePath, wanUploadMedia));
        tools->add(makeMaiBailianKlingVideoTool(wanCredentials, config.caBundlePath, klingUploadMedia));
        tools->add(makeMaiWanImageTool(wanCredentials, config.caBundlePath, wanUploadMedia));
        tools->add(makeMaiBailianKlingImageTool(wanCredentials, config.caBundlePath, klingUploadMedia));
        const std::string rvmModel = request.value("rvmModelPath", "");
        const std::string lamaModel = request.value("lamaModelPath", "");
        const std::string ortRuntime = request.value("ortRuntimePath", "");
        if (!rvmModel.empty() && MaiFileSystem::exists(MaiFilePath::fromUtf8(rvmModel))) {
            if (ortApiBase != nullptr ||
                (!ortRuntime.empty() && MaiFileSystem::exists(MaiFilePath::fromUtf8(ortRuntime)))) {
                tools->add(makeMaiVideoMattingTool(maiMatteVideo, rvmModel, ortRuntime, ortApiBase));
            }
        }
        // 去水印仅暴露新的本地模型 + 临近干净帧工具，不再注册旧 delogo 实现。
        if (!lamaModel.empty() && MaiFileSystem::exists(MaiFilePath::fromUtf8(lamaModel)) &&
            (ortApiBase != nullptr ||
             (!ortRuntime.empty() && MaiFileSystem::exists(MaiFilePath::fromUtf8(ortRuntime))))) {
            tools->add(makeMaiTemporalWatermarkTool(maiRepairVideoWatermarks, lamaModel, ortRuntime, ortApiBase));
        }
        tools->add(makeMaiPdfTool(
            [dispatcher = hostTools](const std::string& html, const std::string& output,
                                     const std::atomic<bool>* cancel) {
                if (cancel != nullptr && cancel->load(std::memory_order_relaxed))
                    return MaiToolResult::failure(MaiErrorCode::Canceled, "PDF generation was canceled");
                return callMaiMobileHostTool(dispatcher, "generate_pdf",
                                             Json{{"html", html}, {"output_path", output}}.dump());
            },
            [dispatcher = hostTools](const std::atomic<bool>* cancel) {
                return requestPrivatePdfPassword(dispatcher, cancel);
            }));
        registerMaiChatHostTools(*tools, hostTools);
        registerMaiQrCodeTools(*tools);
#if defined(__ANDROID__)
        const std::string scheduleDevice = request.value("scheduleDeviceName", "Android");
#else
        const std::string scheduleDevice = request.value("scheduleDeviceName", "iPhone or iPad");
#endif
        registerMaiSessionScheduleTools(*tools, nextScheduleStore,
                                        scheduleDevice.size() <= 40 ? scheduleDevice : scheduleDevice.substr(0, 40));
        registerMaiCalendarTools(
            *tools, [dispatcher = hostTools](const MaiCalendarEventInput& event, const MaiToolContext&) {
                const Json request{{"title", event.title},
                                   {"notes", event.notes},
                                   {"session_id", event.sessionId},
                                   {"starts_at_utc_ms", event.startsAtUtcMs},
                                   {"ends_at_utc_ms", event.endsAtUtcMs}};
                return callMaiMobileHostTool(dispatcher, "maichat_calendar_create_event", request.dump());
            });
        registerMaiCloudDriveTools(
            *tools,
            [dispatcher = hostTools](const std::string& name, const std::string& argumentsJson, const MaiToolContext&) {
                return callMaiMobileHostTool(dispatcher, name, argumentsJson);
            });
        registerMobileBrowserTools(*tools, hostTools);
#if defined(__ANDROID__)
        registerMobileScreenCaptureTools(*tools, hostTools);
#elif defined(MAICHAT_MOBILE_IOS)
        if (request.value("supportsScreenCapture", false)) registerMobileScreenCaptureTools(*tools, hostTools);
#endif
        registerMobilePhotoTools(*tools, hostTools);
        MaiAgent::Options options;
        options.defaultModel = request.at("model").get<std::string>();
        options.reasoningEffort = request.value("reasoningEffort", std::string("high"));
        if (!maiIsValidReasoningEffort(options.reasoningEffort))
            throw NativeOperationFailure({MaiErrorCode::InvalidInput, "Unsupported reasoning effort."});
        options.fileAccessRoot = request.value("appRoot", request.at("workspace").get<std::string>());
        options.decodeText = [dispatcher = hostTools](const std::string& bytes,
                                                      const std::string& encoding) -> MaiResult<std::string> {
            const MaiToolResult result = callMaiMobileHostTool(
                dispatcher, "mobile_decode_text", Json{{"base64", encodeBase64(bytes)}, {"encoding", encoding}}.dump());
            if (result.hasError()) return result.error();
            return result.output();
        };
        // 与桌面端共用可见正文进度约定，后续本地模型能力边界仍按当前配置追加。
        options.baseInstructions = kMarkdownBaseInstructions + maiAgentProgressInstructions();
        options.baseInstructions +=
            "\nPrimary model selection belongs to this session. "
            "A text-only primary model cannot analyze image pixels merely "
            "because a tool can display an image to the user.";
        // 内置模型路径由原生宿主复制/校验后提供，只允许当前App容器中的现有文件。
        // 提示词只介绍已知可用的英文测试权重，不改变主聊天供应商，也不让模型猜目录。
        if (!isLocal && request.value("localTestModelPath", Json{}).is_string()) {
            MaiToolContext localModelContext;
            localModelContext.root = request.at("workspace").get<std::string>();
            localModelContext.fileAccessRoot = options.fileAccessRoot;
            const auto localModel = localModelContext.resolvePath(request["localTestModelPath"].get<std::string>());
            if (!localModel.empty() && MaiFileSystem::exists(MaiFilePath::fromUtf8(localModel))) {
                options.baseInstructions +=
                    "\nA tiny English CPU inference test model is available locally at " + localModel +
                    ". Use the llama tool with -m pointing to this existing GGUF, "
                    "-p 'Once upon a time', -c 256 -n 32 -t 2 --temp 0 --no-conversation to test "
                    "the engine. It is an English continuation test model, with no OCR or image "
                    "capability. Do not search directories or download models to run this test.";
            }
        }
        const auto policy = request.value("policy", "on-request");
        if (policy == "never")
            options.approvalPolicy = MaiApprovalPolicy::Never;
        else if (policy == "unless-trusted")
            options.approvalPolicy = MaiApprovalPolicy::UnlessTrusted;
        else if (policy != "on-request")
            throw std::runtime_error("Unknown approval policy.");
        // QA 与正式 Agent 必须使用相同的模型能力过滤，不能用更宽的工具清单验收本地模型。
        const auto nextProbeTools = tools->specs(options.defaultModel);
        const auto nextProbeInstructions = options.baseInstructions;
        auto replacement =
            std::make_unique<MaiAgent>(std::move(store.value()), nextModelClientFactory(), std::move(tools), options);
        stopSchedulePolling();
        if (anyBusy()) {
            startSchedulePolling();
            throw std::runtime_error("Stop active tasks before changing the model.");
        }
        agent.reset();
        scheduleRunner.reset();
        scheduleStore = nextScheduleStore;
        {
            std::lock_guard<std::mutex> lock(scheduleMutex);
            scheduleEvents.clear();
        }
        pendingDeltas.clear();
        liveParts.clear();
        errors.clear();
        agent = std::move(replacement);
        modelClientFactory = std::move(nextModelClientFactory);
        modelProbeTools = nextProbeTools;
        modelProbeInstructions = nextProbeInstructions;
        workspace = request.at("workspace").get<std::string>();
        model = options.defaultModel;
        primaryWire = config.wire;
        routeDirectory = std::move(nextRoutes);
        defaultReasoningEffort = options.reasoningEffort;
        localModelName = nextLocalRoute ? nextLocalRoute->modelNames.front() : std::string{};
        localSupportsImages = nextLocalSupportsImages;
        localRouteState = std::move(nextLocalState);
        fileAccessRoot = options.fileAccessRoot;
        configured = !model.empty();
        dirty = true;
        structureChanged = true;
        cachedSession.clear();
        cachedLimit = -1;
        cachedMessages.clear();
        cachedQuestionTranscript = {};
        questionStructureRevision.store(1, std::memory_order_relaxed);
        remoteQuestionRevision = 0;
        remoteQuestionHistory.clear();
        trackMessageUpdates = false;
        pendingMessageUpdates.clear();
        remoteTrackedRoots.clear();
        remotePendingMessageUpdates.clear();
        agent->eventBus().subscribe([this](const MaiEvent& event) {
            // 网络线程只合并增量，不读数据库、不解析 Markdown、不调用 UI。
            if (event.type != MaiEventType::MessagePartDelta && event.type != MaiEventType::SessionStatus)
                questionStructureRevision.fetch_add(1, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(mutex);
                dirty = true;
                if (trackMessageUpdates && !event.messageId.empty())
                    pendingMessageUpdates[event.sessionId].insert(event.messageId);
                if (!event.messageId.empty() && remoteTrackedRoots.count(event.sessionId) != 0)
                    remotePendingMessageUpdates[event.sessionId].insert(event.messageId);
                if (event.type == MaiEventType::SessionDeleted) {
                    remoteTrackedRoots.erase(event.sessionId);
                    remotePendingMessageUpdates.erase(event.sessionId);
                }
                if (event.type == MaiEventType::MessagePartDelta)
                    pendingDeltas[event.partId] += event.delta;
                else
                    structureChanged = true;
                if (event.type == MaiEventType::SessionError) errors[event.sessionId] = event.detail;
            }
            if (changeHandler != nullptr && !event.sessionId.empty() && event.type != MaiEventType::SessionStatus)
                changeHandler(event.sessionId.c_str());
            if (event.type == MaiEventType::SessionIdle || event.type == MaiEventType::SessionError ||
                event.type == MaiEventType::SessionDeleted) {
                std::lock_guard<std::mutex> lock(scheduleMutex);
                scheduleEvents.push_back({event.sessionId, event.type, event.detail});
                scheduleWake.notify_one();
            }
        });
        startSchedulePolling();
    }

    // 测量同一个已配置本地主模型及完整工具模板，始终不执行工具、不落库、不读会话历史。
    // 供宿主在隔离实例中做设备验收；调用者须自己准备已授权的测试文件/工作区。
    Json localModelProbe(const Json& request) {
        for (const auto& item : request.items())
            if (item.key() != "op" && item.key() != "messages" && item.key() != "timeout_ms")
                throw NativeOperationFailure(MaiError::make(MaiErrorCode::InvalidInput, "Unknown local probe field."));
        if (!configured || primaryWire != MaiWireApi::Local || !modelClientFactory)
            throw NativeOperationFailure(
                MaiError::make(MaiErrorCode::NotConfigured, "A local primary model is required for this probe."));
        if (anyBusy())
            throw NativeOperationFailure(
                MaiError::make(MaiErrorCode::Busy, "The configured Agent is busy; its task was preserved."));
        if (!request.contains("messages") || !request["messages"].is_array() || request["messages"].empty() ||
            request["messages"].size() > 10)
            throw NativeOperationFailure(
                MaiError::make(MaiErrorCode::InvalidInput, "Probe messages must contain one to ten text messages."));
        MaiModelRequest input;
        input.model = model;
        input.workingDirectory = workspace;
        input.baseInstructions = modelProbeInstructions;
        input.tools = modelProbeTools;
        std::size_t bytes = 0;
        for (const auto& message : request["messages"]) {
            if (!message.is_object() || message.size() != 2 || !message.value("role", Json{}).is_string() ||
                !message.value("content", Json{}).is_string())
                throw NativeOperationFailure(
                    MaiError::make(MaiErrorCode::InvalidInput, "Probe messages need role and content strings only."));
            const auto role = message["role"].get<std::string>();
            if (role != "user" && role != "assistant")
                throw NativeOperationFailure(
                    MaiError::make(MaiErrorCode::InvalidInput, "Only user and assistant probe text is accepted."));
            MaiModelMessage item;
            item.role = role == "user" ? MaiModelRole::User : MaiModelRole::Assistant;
            item.content = message["content"].get<std::string>();
            if (item.content.size() > 65536 - bytes)
                throw NativeOperationFailure(
                    MaiError::make(MaiErrorCode::InvalidInput, "Probe text exceeds its memory budget."));
            bytes += item.content.size();
            input.messages.push_back(std::move(item));
        }
        std::uint64_t timeout = 120000;
        if (request.contains("timeout_ms")) {
            const auto& value = request["timeout_ms"];
            if (value.is_number_unsigned())
                timeout = value.get<std::uint64_t>();
            else if (value.is_number_integer() && value.get<std::int64_t>() > 0)
                timeout = static_cast<std::uint64_t>(value.get<std::int64_t>());
            else
                timeout = 0;
        }
        if (timeout == 0 || timeout > 120000)
            throw NativeOperationFailure(
                MaiError::make(MaiErrorCode::InvalidInput, "Probe deadline must be 1 to 120000 milliseconds."));
        LocalModelProbeDeadline deadline(static_cast<std::uint32_t>(timeout));
        const auto start = std::chrono::steady_clock::now();
        double firstToken = -1;
        std::string output;
        Json calls = Json::array();
        MaiStreamSink sink;
        sink.onText = [&](std::string_view text) {
            if (firstToken < 0 && !text.empty())
                firstToken =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            output.append(text.data(), text.size());
        };
        // 绝不能在这里执行工具、回写用户会话或请求宿主凭据。
        sink.onToolCall = [&](const MaiToolInvocation& call) {
            calls.push_back({{"id", call.id}, {"name", call.name}, {"arguments", call.arguments}});
        };
        const auto error = modelClientFactory()->stream(input, sink, deadline.canceled());
        const double total =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const auto state = getSharedMaiLlamaRuntime()->status();
        Json advertised = Json::array();
        for (const auto& tool : input.tools) advertised.push_back(tool.name);
        // 完整原生调用也是有效的测量结果，但它不代表工具已执行。
        // 回传实际名称、参数和 ID，避免把“返回了文本”误当成任务质量验收。
        Json response{{"ok", !error && (!calls.empty() || !output.empty())},
                      {"text", output},
                      {"tool_call_count", calls.size()},
                      {"tool_calls", std::move(calls)},
                      {"tools_executed", false},
                      {"advertised_tool_names", std::move(advertised)},
                      {"first_token_ms", firstToken},
                      {"total_ms", total},
                      {"wire", maiWireApiToString(primaryWire)},
                      {"model", model},
                      {"localGpuSelected", state.gpuSelected},
                      {"localGpuModelBytes", state.gpuModelBytes},
                      {"localGpuBackend", state.gpuBackend}};
        if (error)
            response.update({{"errorCode", maiErrorCodeToString(error.code())}, {"error", error.message()}});
        else if (!response["ok"].get<bool>())
            response.update({{"errorCode", "protocol"}, {"error", "The probe returned no text or tool calls."}});
        return response;
    }

    // 将持久化消息转为移动端可读快照，并保留最新消息和未完成任务的状态。
    // 返回值只包含本次请求选中的会话内容，不改动 Agent 存储。
    Json serializeMessages(const std::vector<MaiMessage>& source, bool sessionBusy, const std::string& sessionId,
                           const std::unordered_map<std::string, std::string>* liveOverride = nullptr) {
        Json messages = Json::array();
        std::string latestId;
        for (const auto& message : source)
            if (message.id > latestId) latestId = message.id;
        for (const auto& message : source) {
            Json parts = Json::array();
            for (const auto& part : message.parts) {
                Json value = {{"id", part.id}, {"created", part.created}};
                std::visit(
                    [&](const auto& body) {
                        using T = std::decay_t<decltype(body)>;
                        if constexpr (std::is_same_v<T, MaiToolPart>) {
                            value.update({{"kind", "tool"},
                                          {"tool", body.tool},
                                          {"detailRevision", maiToolDetailRevision(body)},
                                          {"detailsAvailable", true},
                                          {"input", body.input},
                                          {"output", body.output},
                                          {"error", body.error},
                                          {"state", maiToolStateToString(body.state)}});
                            // 可选身份元数据不撑破同步消息预算；省略异常值而不截断伪造另一个callId。
                            // revision仍覆盖完整源值，按需详情读取也保留该源值。
                            if (body.callId.size() <= 128) value["callId"] = body.callId;
                        } else if constexpr (std::is_same_v<T, MaiImagePart>) {
                            value.update({{"kind", "image"}, {"path", body.path}, {"mimeType", body.mimeType}});
                        } else if constexpr (std::is_same_v<T, MaiVideoPart>) {
                            value.update({{"kind", "video"}, {"path", body.path}, {"mimeType", body.mimeType}});
                        } else if constexpr (std::is_same_v<T, MaiQuotePart>) {
                            value.update({{"kind", "quote"}, {"messageId", body.messageId}, {"preview", body.preview}});
                        } else {
                            std::string text = body.text;
                            const auto& streams = liveOverride ? *liveOverride : liveParts;
                            auto live = streams.find(part.id);
                            if (message.isInProgress() && live != streams.end() && live->second.size() > text.size())
                                text = live->second;
                            value.update(
                                {{"kind", std::is_same_v<T, MaiTextPart> ? "text" : "reasoning"}, {"text", text}});
                        }
                    },
                    part.body);
                parts.push_back(std::move(value));
                if (!message.isInProgress() && liveOverride == nullptr) liveParts.erase(part.id);
            }
            messages.push_back({{"id", message.id},
                                {"sessionId", sessionId},
                                {"role", maiRoleToString(message.role)},
                                {"created", message.created},
                                {"completed", message.completed},
                                {"active", message.isInProgress() && sessionBusy && message.id == latestId},
                                {"parts", parts}});
        }
        return messages;
    }

    // 子任务问答独立于根消息页传输，真实消息/片段 ID 用于原位更新。
    // 不合并进 messages，避免 before 游标或 hasOlderMessages 被子任务历史改变。
    Json serializeQuestionMessages(const MaiQuestionTranscript& transcript) {
        Json messages = Json::array();
        for (const auto& projected : transcript.childMessages) {
            Json serialized = serializeMessages({projected.message}, agent->isBusy(projected.ownerSessionId),
                                                projected.ownerSessionId);
            serialized.front()["questionProjection"] = true;
            messages.push_back(std::move(serialized.front()));
        }
        // 与 SessionSync 的小状态帧使用同一 48 KiB 上限。超限先移除较旧的已结束问答，
        // 不截断用户回答或改写 Core 历史；单条过大的当前问题仍可从 questions 正文显示。
        std::unordered_set<std::string> pendingMessages;
        for (const auto& question : transcript.pending) pendingMessages.insert(question.messageId);
        while (!messages.empty() && messages.dump().size() > 48 * 1024) {
            auto removed = std::find_if(messages.begin(), messages.end(), [&](const auto& message) {
                return pendingMessages.count(message.at("id").template get<std::string>()) == 0;
            });
            messages.erase(removed == messages.end() ? messages.begin() : removed);
        }
        return messages;
    }

    Json serializePendingQuestions(const MaiQuestionTranscript& transcript) const {
        Json questions = Json::array();
        for (const auto& question : transcript.pending)
            questions.push_back({{"id", question.id},
                                 {"sessionId", question.sessionId},
                                 {"messageId", question.messageId},
                                 {"partId", question.partId},
                                 {"asked", question.asked},
                                 {"question", question.question},
                                 {"options", question.options}});
        return questions;
    }

    const MaiModelRoute* routeForModel(const std::string& selectedModel) const {
        for (const auto& route : routeDirectory.routes)
            if (std::find(route.modelNames.begin(), route.modelNames.end(), selectedModel) != route.modelNames.end())
                return &route;
        return nullptr;
    }

    MaiSession requireSession(const std::string& id) const {
        MaiSession session;
        if (!agent || id.empty() || !agent->getSession(id, session))
            throw NativeOperationFailure({MaiErrorCode::NotFound, "Session not found."});
        return session;
    }

    Json sessionConfiguration(const MaiSession& session) const {
        const auto selectedModel = session.model.empty() ? model : session.model;
        const auto* route = routeForModel(selectedModel);
        return {{"session", session.id},
                {"model", selectedModel},
                {"reasoningEffort", session.reasoningEffort},
                {"defaultReasoningEffort", defaultReasoningEffort},
                {"wire", route ? maiWireApiToString(route->config.wire) : ""}};
    }

    void requireConfiguredModel(const std::string& selectedModel) const {
        const auto* route = routeForModel(selectedModel);
        if (!route) throw NativeOperationFailure({MaiErrorCode::InvalidInput, "The selected model is unavailable."});
        if (route->clientFactory) {
            if (selectedModel == localModelName) {
                std::lock_guard<std::mutex> lock(localRouteState->mutex);
                if (!localRouteState->config)
                    throw NativeOperationFailure(
                        {MaiErrorCode::NotConfigured, "The bundled local model has not been prepared."});
            }
            return;
        }
        std::string key;
        try {
            key = route->apiKeyProvider ? route->apiKeyProvider() : route->config.apiKey;
        } catch (...) {
        }
        if (key.empty() || key.size() > 8192 || key.find_first_of("\r\n") != std::string::npos ||
            key.find('\0') != std::string::npos)
            throw NativeOperationFailure(
                {MaiErrorCode::NotConfigured, "The selected provider is not configured for this account."});
    }

    // 作用：读取指定会话的有限消息快照和任务状态。
    Json snapshot(const std::string& selected, bool force, int limit, bool includeMessageUpdates = false) {
        std::unordered_map<std::string, std::string> errorCopy;
        std::unordered_set<std::string> changedParts;
        std::vector<std::string> updatedIds;
        bool reload = force || cachedSession != selected || cachedLimit != limit;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (includeMessageUpdates) trackMessageUpdates = true;
            auto pending = pendingMessageUpdates.find(selected);
            const bool hasUpdates = includeMessageUpdates && limit > 0 && pending != pendingMessageUpdates.end() &&
                                    !pending->second.empty();
            if (!dirty && !reload && !hasUpdates) return {{"ok", true}, {"changed", false}};
            dirty = false;
            reload |= structureChanged;
            structureChanged = false;
            for (auto& delta : pendingDeltas) {
                liveParts[delta.first] += delta.second;
                changedParts.insert(delta.first);
            }
            pendingDeltas.clear();
            errorCopy = errors;
            if (hasUpdates) {
                // 每次最多一旧页；未消费的 ID 留到下次，不把全部已加载历史 ID 带在线上。
                auto& ids = pending->second;
                for (auto id = ids.begin(); id != ids.end() && updatedIds.size() < 20;)
                    updatedIds.push_back(*id), id = ids.erase(id);
                if (ids.empty()) pendingMessageUpdates.erase(pending);
            }
        }
        Json sessions = Json::array();
        bool busy = false;
        for (const auto& session : agent->listSessions()) {
            if (!session.isRoot()) continue;
            const bool active = agent->isBusy(session.id);
            busy |= active;
            sessions.push_back({{"id", session.id},
                                {"title", session.title},
                                {"busy", active},
                                {"turnId", agent->activeTurnId(session.id)},
                                {"model", sessionConfiguration(session)["model"]},
                                {"reasoningEffort", session.reasoningEffort},
                                {"wire", sessionConfiguration(session)["wire"]}});
        }
        // 增量期间复用消息结构，避免每个批次重新读整段 SQLite 历史。
        if (reload) {
            if (limit < 0)
                cachedMessages = agent->listMessages(selected);
            else if (limit == 0)
                cachedMessages.clear();
            else
                cachedMessages = agent->listMessagesPage(selected, "", limit);
            cachedSession = selected;
            cachedLimit = limit;
        }
        const bool sessionBusy = agent->isBusy(selected);
        Json messages;
        if (limit >= 0 && !reload && !changedParts.empty()) {
            std::vector<MaiMessage> changedMessages;
            for (const auto& message : cachedMessages) {
                for (const auto& part : message.parts) {
                    if (changedParts.count(part.id) == 0) continue;
                    changedMessages.push_back(message);
                    break;
                }
            }
            messages = serializeMessages(changedMessages, sessionBusy, selected);
        } else {
            messages = serializeMessages(cachedMessages, sessionBusy, selected);
        }
        std::vector<MaiMessage> updatedMessages;
        for (const auto& id : updatedIds) {
            MaiMessage message;
            if (agent->getMessage(selected, id, message)) updatedMessages.push_back(std::move(message));
        }
        Json messageUpdates = serializeMessages(updatedMessages, sessionBusy, selected);
        Json permissions = Json::array();
        for (const auto& p : agent->listPendingPermissions())
            if (p.sessionId == selected)
                permissions.push_back({{"id", p.id},
                                       {"tool", p.toolName},
                                       {"input", p.arguments},
                                       {"allowForSession", p.allowForSession},
                                       {"rememberOnApproval", p.rememberOnApproval},
                                       {"fileCount", p.approvalKeys.size()}});
        // 模型逐字流式期间复用同一份问答结构，避免每个 delta 重读后代历史。
        // 提问/回答/工具状态变化都是非 delta 事件，触发 reload 后刷新权威快照。
        if (reload) cachedQuestionTranscript = captureMaiQuestionTranscript(*agent, selected);
        if (!busy) liveParts.clear();
        MaiSession chosen;
        const bool hasChosen = agent->getSession(selected, chosen);
        const auto choice = hasChosen ? sessionConfiguration(chosen)
                                      : Json{{"model", model},
                                             {"reasoningEffort", defaultReasoningEffort},
                                             {"wire", maiWireApiToString(primaryWire)}};
        const auto localStatus = choice["wire"] == "local" ? getSharedMaiLlamaRuntime()->status() : MaiLlamaStatus{};
        bool selectedConfigured = configured;
        if (choice["wire"] == "local") {
            std::lock_guard<std::mutex> lock(localRouteState->mutex);
            selectedConfigured = localRouteState->config.has_value();
        }
        return {{"ok", true},
                {"changed", true},
                {"sessions", sessions},
                {"messages", messages},
                {"messageUpdates", std::move(messageUpdates)},
                {"hasOlderMessages", limit > 0 && cachedMessages.size() >= static_cast<std::size_t>(limit)},
                {"permissions", permissions},
                {"questions", serializePendingQuestions(cachedQuestionTranscript)},
                {"questionMessages", serializeQuestionMessages(cachedQuestionTranscript)},
                {"busy", busy},
                {"turnId", agent->activeTurnId(selected)},
                {"model", model},
                {"wire", maiWireApiToString(primaryWire)},
                {"selectedModel", choice["model"]},
                {"selectedReasoningEffort", choice["reasoningEffort"]},
                {"defaultReasoningEffort", defaultReasoningEffort},
                {"selectedWire", choice["wire"]},
                {"localGpuSelected", localStatus.gpuSelected},
                {"localGpuModelBytes", localStatus.gpuModelBytes},
                {"localGpuBackend", localStatus.gpuBackend},
                {"configured", selectedConfigured},
                {"error", errorCopy[selected]}};
    }

    // 远端读取不消费本机 UI 的 dirty 标记与增量缓存。否则 PC 拉一次历史，
    // iPhone 自己的下一次刷新会误以为没有新消息。
    Json remoteSnapshot(const std::string& sessionId, const std::string& before, int limit,
                        bool includeMessageUpdates) {
        MaiSession session;
        if (!agent->getSession(sessionId, session)) throw std::runtime_error("Session not found.");
        const bool busy = agent->isBusy(sessionId);
        const auto page = agent->listMessagesPage(sessionId, before, static_cast<std::size_t>(limit));
        std::vector<std::string> updatedIds;
        bool hasMessageUpdates = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (includeMessageUpdates && session.isRoot()) {
                remoteTrackedRoots.insert(sessionId);
                auto pending = remotePendingMessageUpdates.find(sessionId);
                if (pending != remotePendingMessageUpdates.end()) {
                    updatedIds.assign(pending->second.begin(), pending->second.end());
                    std::sort(updatedIds.begin(), updatedIds.end());
                    if (updatedIds.size() > 20) updatedIds.resize(20);
                    for (const auto& id : updatedIds) pending->second.erase(id);
                    hasMessageUpdates = !pending->second.empty();
                    if (!hasMessageUpdates) remotePendingMessageUpdates.erase(pending);
                }
            }
        }
        std::vector<MaiMessage> updatedMessages;
        for (const auto& id : updatedIds) {
            MaiMessage message;
            if (agent->getMessage(sessionId, id, message)) updatedMessages.push_back(std::move(message));
        }
        // 精读正文尚可能短于 1024B checkpoint。只复制本次消息的 live 文本，
        // 不消费本机 pendingDeltas/liveParts，也不把 reasoning 拼入正文。
        std::unordered_map<std::string, std::string> remoteLiveParts;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto captureLive = [&](const std::vector<MaiMessage>& source) {
                for (const auto& message : source)
                    if (message.isInProgress())
                        for (const auto& part : message.parts)
                            if (std::holds_alternative<MaiTextPart>(part.body)) {
                                std::string text;
                                const auto saved = liveParts.find(part.id);
                                if (saved != liveParts.end()) text = saved->second;
                                const auto delta = pendingDeltas.find(part.id);
                                if (delta != pendingDeltas.end()) text += delta->second;
                                if (!text.empty()) remoteLiveParts.emplace(part.id, std::move(text));
                            }
            };
            captureLive(page);
            captureLive(updatedMessages);
        }
        Json permissions = Json::array();
        for (const auto& item : agent->listPendingPermissions())
            if (item.sessionId == sessionId)
                permissions.push_back({{"id", item.id},
                                       {"tool", item.toolName},
                                       {"input", item.arguments},
                                       {"allowForSession", item.allowForSession},
                                       {"rememberOnApproval", item.rememberOnApproval},
                                       {"fileCount", item.approvalKeys.size()}});
        // 保持待回答列表实时，后代已持久化问答则按独立结构 revision 复用。
        // 本机 snapshot(force) 不消费此缓存，逐字 delta 也不触发后代 SQLite 读取。
        auto questionTranscript = captureMaiQuestionTranscript(*agent, sessionId, false);
        const auto revision = questionStructureRevision.load(std::memory_order_relaxed);
        if (remoteQuestionRevision != revision) {
            remoteQuestionHistory.clear();
            remoteQuestionRevision = revision;
        }
        const auto found = remoteQuestionHistory.find(sessionId);
        if (found != remoteQuestionHistory.end()) {
            questionTranscript.childMessages = found->second;
        } else {
            questionTranscript.childMessages = captureMaiQuestionTranscript(*agent, sessionId).childMessages;
            if (remoteQuestionHistory.size() >= 3) remoteQuestionHistory.clear();
            remoteQuestionHistory.emplace(sessionId, questionTranscript.childMessages);
        }
        const auto choice = sessionConfiguration(session);
        return {{"ok", true},
                {"session", sessionId},
                {"model", choice["model"]},
                {"reasoningEffort", choice["reasoningEffort"]},
                {"defaultReasoningEffort", defaultReasoningEffort},
                {"wire", choice["wire"]},
                {"rootSessionId", maiQuestionRootSessionId(*agent, sessionId)},
                {"messages", serializeMessages(page, before.empty() && busy, sessionId, &remoteLiveParts)},
                {"messageUpdates", serializeMessages(updatedMessages, busy, sessionId, &remoteLiveParts)},
                {"hasMessageUpdates", hasMessageUpdates},
                {"permissions", permissions},
                {"questions", serializePendingQuestions(questionTranscript)},
                {"questionMessages", serializeQuestionMessages(questionTranscript)},
                {"busy", busy},
                {"turnId", agent->activeTurnId(sessionId)}};
    }

    // 详情读取不消费增量快照。所有失败也带回原请求绑定，宿主可丢弃已切页/切账号的迟到响应。
    Json toolDetail(const Json& r) const {
        const auto boundedIdentity = [&](const char* key) -> std::string {
            if (!r.contains(key) || !r[key].is_string()) return {};
            const auto& value = r[key].get_ref<const std::string&>();
            return value.empty() || value.size() > 128 ? std::string{} : value;
        };
        const auto readSize = [](const Json& value, std::size_t& out) {
            if (!value.is_number_integer()) return false;
            if (!value.is_number_unsigned() && value.get<std::int64_t>() < 0) return false;
            const auto number = value.get<std::uint64_t>();
            out = static_cast<std::size_t>(number);
            return static_cast<std::uint64_t>(out) == number;
        };
        std::size_t offset = 0;
        const bool validOffset = !r.contains("offset") || readSize(r["offset"], offset);
        if (!validOffset) offset = 0;
        std::string selectedField = "input";
        if (r.contains("field")) {
            selectedField.clear();
            if (r["field"].is_string()) {
                const auto& field = r["field"].get_ref<const std::string&>();
                if (field == "input" || field == "output" || field == "error") selectedField = field;
            }
        }
        // 先规范化回执绑定，再校验请求；不能把巨大字符串、数组或对象原样回显到失败消息。
        Json response{{"ok", false},
                      {"requestId", boundedIdentity("requestId")},
                      {"sessionId", boundedIdentity("session")},
                      {"messageId", boundedIdentity("messageId")},
                      {"partId", boundedIdentity("partId")},
                      {"field", selectedField},
                      {"offset", offset}};
        const Json binding = response;
        const auto fail = [&](MaiErrorCode code, const std::string& message) {
            Json failure = binding;
            failure["errorCode"] = maiErrorCodeToString(code);
            failure["error"] = message;
            return failure;
        };
        for (const auto& field : r.items())
            if (field.key() != "op" && field.key() != "session" && field.key() != "requestId" &&
                field.key() != "messageId" && field.key() != "partId" && field.key() != "field" &&
                field.key() != "offset" && field.key() != "limit" && field.key() != "detailRevision")
                return fail(MaiErrorCode::InvalidInput, "Unknown tool detail field.");
        for (const char* field : {"requestId", "session", "messageId", "partId"})
            if (!r.contains(field) || !r[field].is_string() || r[field].get_ref<const std::string&>().empty() ||
                r[field].get_ref<const std::string&>().size() > 128)
                return fail(MaiErrorCode::InvalidInput, "Invalid tool detail identity.");
        if (selectedField.empty() || (r.contains("detailRevision") && !r["detailRevision"].is_string()))
            return fail(MaiErrorCode::InvalidInput, "Invalid tool detail field or revision.");
        MaiToolDetailQuery query;
        query.messageId = r["messageId"];
        query.partId = r["partId"];
        query.field = response["field"];
        query.expectedRevision = r.value("detailRevision", std::string{});
        query.offsetBytes = offset;
        if (!validOffset || (r.contains("limit") && !readSize(r["limit"], query.maxBytes)))
            return fail(MaiErrorCode::InvalidInput, "Tool detail offset and limit must be nonnegative integers.");
        if (!agent) return fail(MaiErrorCode::NotConfigured, "Agent is not initialized.");
        const auto detail = agent->readToolDetail(r["session"], query);
        if (!detail) return fail(detail.error().code(), detail.error().message());
        const auto& page = detail.value();
        response.update({{"ok", true},
                         {"messageId", page.messageId},
                         {"partId", page.partId},
                         {"callId", page.callId},
                         {"tool", page.tool},
                         {"state", maiToolStateToString(page.state)},
                         {"field", page.field},
                         {"detailRevision", page.revision},
                         {"offset", page.offsetBytes},
                         {"sourceBytesRead", page.sourceBytesRead},
                         {"totalBytes", page.totalBytes},
                         {"nextOffset", page.nextOffsetBytes},
                         {"hasMore", page.nextOffsetBytes != 0},
                         {"textIsValidUtf8", page.textIsValidUtf8}});
        if (page.textIsValidUtf8) {
            response["text"] = page.text;
        } else {
            // 预览允许替换，但游标始终按源字节推进；Base64只为坏编码页提供无损原文。
            response["text"] = Json::parse(Json(page.text).dump(-1, ' ', false, Json::error_handler_t::replace));
            response["rawBase64"] = encodeBase64(page.text);
        }
        // 4096源字节的转义和Base64仍有界；异常旧元数据不能撑破宿主的控制消息上限。
        if (response.dump(-1, ' ', false, Json::error_handler_t::replace).size() >= 64 * 1024)
            return fail(MaiErrorCode::InvalidInput, "Tool detail metadata exceeds the response budget.");
        return response;
    }

    Json request(const Json& r) {
        const std::string op = r.at("op");
        if (op == "configure") {
            configure(r);
            return {{"ok", true}};
        }
        if (op == "tool_detail") return toolDetail(r);
        if (!agent) throw std::runtime_error("Agent is not initialized.");
        if (op == "pin_input_file") {
            // 图片转换留在平台；转换后的私有临时文件统一由 C++ 交付到稳定路径。
            // 同一请求/附件跨传输重试不换路径，避免接收凭据因随机导入文件名而冲突。
            const std::string requestId = r.at("requestId");
            const std::string attachment = r.at("attachmentId");
            const std::string relative = r.at("path");
            if (requestId.empty() || requestId.size() > 128 || attachment.empty() || attachment.size() > 128 ||
                !std::all_of(attachment.begin(), attachment.end(), [](unsigned char c) {
                    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                           c == '-';
                }))
                throw std::runtime_error("Invalid input attachment identity.");
            MaiToolContext paths;
            paths.root = workspace;
            const auto source = MaiFilePath::fromUtf8(paths.resolvePath(relative));
            MaiRegularFileInfo sourceInfo;
            if (source.isEmpty() || !MaiFileSystem::regularFileInfo(source, sourceInfo))
                throw std::runtime_error("Imported input attachment is unavailable.");
            constexpr char digits[] = "0123456789abcdef";
            std::string key;
            for (unsigned char c : requestId) {
                key += digits[c >> 4];
                key += digits[c & 15];
            }
            const std::string base = source.baseName().toUtf8();
            const auto dot = base.find_last_of('.');
            const std::string extension =
                dot == std::string::npos || base.size() - dot > 16 ? ".bin" : base.substr(dot);
            const std::string targetName = ".maichat/session-inputs/" + key.substr(0, 128) +
                                           (key.size() > 128 ? "/" + key.substr(128) : "") + "/" + attachment +
                                           extension;
            const std::string destination = paths.resolvePath(targetName);
            if (destination.empty()) throw std::runtime_error("Input attachment path is outside the workspace.");
            const auto target = MaiFilePath::fromUtf8(destination);
            if (const auto error = MaiFileSystem::createDirectories(target.dirName()))
                throw std::runtime_error(error.message());
            MaiRegularFileInfo existing;
            if (MaiFileSystem::regularFileInfo(target, existing)) {
                if (existing.bytes != sourceInfo.bytes) throw std::runtime_error("Input attachment content changed.");
                if (source != target) MaiFileSystem::removeFile(source);
            } else if (const auto error = MaiFileSystem::publishNewFile(source, target)) {
                throw std::runtime_error(error.message());
            }
            return {{"ok", true}, {"path", targetName}};
        }
        if (op == "local_model_probe") return localModelProbe(r);
        const std::string session = r.value("session", "");
        if (op == "prepare_local_model") {
            for (const auto& field : r.items())
                if (field.key() != "op" && field.key() != "force" && field.key() != "model" &&
                    field.key() != "localModelPath" && field.key() != "localProjectorPath" &&
                    field.key() != "localContextSize" && field.key() != "localMaximumTokens" &&
                    field.key() != "localThreads")
                    throw NativeOperationFailure(
                        {MaiErrorCode::InvalidInput, "Unknown local model preparation field."});
            if (localModelName.empty() || r.value("model", std::string{}) != localModelName)
                throw NativeOperationFailure({MaiErrorCode::InvalidInput, "The local route identity does not match."});
            const auto candidate = validatedMobileLocalConfig(r, workspace, fileAccessRoot);
            {
                std::lock_guard<std::mutex> lock(localRouteState->mutex);
                if (localRouteState->config && (localRouteState->config->modelPath != candidate.modelPath ||
                                                localRouteState->config->projectorPath != candidate.projectorPath ||
                                                localRouteState->config->contextSize != candidate.contextSize ||
                                                localRouteState->config->maximumTokens != candidate.maximumTokens ||
                                                localRouteState->config->threads != candidate.threads))
                    throw NativeOperationFailure(
                        {MaiErrorCode::Busy, "An existing local route cannot be replaced by session selection."});
                localRouteState->config = candidate;
                localSupportsImages = !candidate.projectorPath.isEmpty();
            }
            return {{"ok", true}, {"model", localModelName}, {"wire", "local"}};
        }
        if (op == "get_session") {
            auto choice = sessionConfiguration(requireSession(session));
            choice["ok"] = true;
            return choice;
        }
        if (op == "set_model") {
            for (const auto& field : r.items())
                if (field.key() != "op" && field.key() != "session" && field.key() != "model" &&
                    field.key() != "reasoningEffort" && field.key() != "force")
                    throw NativeOperationFailure({MaiErrorCode::InvalidInput, "Unknown session model field."});
            requireSession(session);
            if (!r.contains("model") || !r["model"].is_string() || r["model"].get<std::string>().empty())
                throw NativeOperationFailure({MaiErrorCode::InvalidInput, "A model identifier is required."});
            std::optional<std::string> effort;
            if (r.contains("reasoningEffort")) {
                if (!r["reasoningEffort"].is_string() ||
                    !maiIsValidReasoningEffort(r["reasoningEffort"].get<std::string>()))
                    throw NativeOperationFailure({MaiErrorCode::InvalidInput, "Unsupported reasoning effort."});
                effort = r["reasoningEffort"].get<std::string>();
            }
            if (agent->isBusy(session))
                throw NativeOperationFailure(
                    {MaiErrorCode::Busy, "Stop the target session before changing its model."});
            const auto selectedModel = r["model"].get<std::string>();
            requireConfiguredModel(selectedModel);
            result(agent->submit(MaiUpdateSession{session, "", selectedModel, "", "", effort}));
            auto choice = sessionConfiguration(requireSession(session));
            choice["ok"] = true;
            return choice;
        }
        if (op == "snapshot") {
            const int limit = r.value("messageLimit", -1);
            if (limit < -1 || limit > 100) throw std::runtime_error("messageLimit must be between 0 and 100.");
            return snapshot(session, r.value("force", false), limit, r.value("includeMessageUpdates", false));
        }
        if (op == "remote_snapshot") {
            const int limit = r.value("limit", 10);
            if (limit < 1 || limit > 20) throw std::runtime_error("limit must be between 1 and 20.");
            return remoteSnapshot(session, r.value("before", std::string{}), limit,
                                  r.value("includeMessageUpdates", false));
        }
        if (op == "message") {
            if (!r.contains("id") || !r["id"].is_string())
                throw NativeOperationFailure(
                    MaiError::make(MaiErrorCode::InvalidInput, "Message id must be a string."));
            const std::string messageId = r["id"];
            if (messageId.empty() || messageId.size() > 128)
                throw NativeOperationFailure(MaiError::make(MaiErrorCode::InvalidInput, "Message id is invalid."));
            MaiMessage message;
            if (!agent->getMessage(session, messageId, message))
                throw NativeOperationFailure(
                    MaiError::make(MaiErrorCode::NotFound, "Message was not found in this conversation."));
            return {{"ok", true}, {"messages", serializeMessages({message}, agent->isBusy(session), session)}};
        }
        if (op == "messages_page") {
            const int limit = r.value("limit", 30);
            if (limit < 1 || limit > 100) throw std::runtime_error("limit must be between 1 and 100.");
            const std::string before = r.value("before", "");
            const auto page = agent->listMessagesPage(session, before, limit);
            return {{"ok", true},
                    {"messages", serializeMessages(page, before.empty() && agent->isBusy(session), session)}};
        }
        if (op == "suggest_replies") {
            requireConfiguredModel(model);
            if (!r.contains("messages") || !r["messages"].is_array() || r["messages"].empty() ||
                r["messages"].size() > 30)
                throw std::runtime_error("Messages must contain between 1 and 30 items.");
            MaiModelRequest modelRequest;
            modelRequest.model = model;
            modelRequest.temperature = 0.5;
            modelRequest.baseInstructions =
                "You generate reply suggestions for a private chat. Return only one "
                "JSON object "
                "with exactly these string fields: natural, casual, professional. "
                "Each value must "
                "be a short reply in the conversation language. Do not send a "
                "message, call a "
                "tool, use Markdown fences, or include explanations.";
            MaiModelMessage context;
            context.role = MaiModelRole::User;
            context.content = "Recent conversation, oldest first:\n" + r["messages"].dump();
            modelRequest.messages.push_back(std::move(context));
            std::string response;
            MaiStreamSink sink;
            sink.onText = [&](std::string_view delta) { response.append(delta.data(), delta.size()); };
            std::atomic<bool> cancel{false};
            const MaiError error = modelClientFactory()->stream(modelRequest, sink, cancel);
            if (error) throw std::runtime_error(error.message());
            std::string clean = response;
            const auto firstLine = clean.find('\n');
            const auto closingFence = clean.rfind("```");
            if (clean.rfind("```", 0) == 0 && firstLine != std::string::npos && closingFence > firstLine)
                clean = clean.substr(firstLine + 1, closingFence - firstLine - 1);
            const Json suggestions = Json::parse(clean);
            for (const char* key : {"natural", "casual", "professional"})
                if (!suggestions.contains(key) || !suggestions[key].is_string() ||
                    suggestions[key].get<std::string>().empty())
                    throw std::runtime_error("The model returned incomplete reply suggestions.");
            return {{"ok", true},
                    {"natural", suggestions["natural"]},
                    {"casual", suggestions["casual"]},
                    {"professional", suggestions["professional"]}};
        }
        std::string id;
        std::string acceptedUserId;
        std::string acceptedTurnId;
        if (op == "create") {
            const auto selectedModel = r.value("model", model);
            const auto effort = r.value("reasoningEffort", defaultReasoningEffort);
            if (!maiIsValidReasoningEffort(effort))
                throw NativeOperationFailure({MaiErrorCode::InvalidInput, "Unsupported reasoning effort."});
            if (!routeForModel(selectedModel))
                throw NativeOperationFailure({MaiErrorCode::InvalidInput, "The selected model is unavailable."});
            if (r.contains("model")) requireConfiguredModel(selectedModel);
            id = result(agent->submit(MaiCreateSession{workspace, "", selectedModel, effort}));
        } else if (op == "send" || op == "steer" || op == "enqueue") {
            const auto target = requireSession(session);
            const auto selectedModel = target.model.empty() ? model : target.model;
            requireConfiguredModel(selectedModel);
            // iOS 在升级或恢复后可能给同一数据容器分配新的绝对路径。图片片段只保存
            // 工作区相对路径，所以每轮发送前都把会话根目录迁到当前容器位置。
            if (op == "send" || (op == "enqueue" && !agent->isBusy(session))) {
                result(agent->submit(MaiUpdateSession{session, "", "", "", workspace}));
                std::lock_guard<std::mutex> lock(mutex);
                errors.erase(session);
            }
            std::vector<MaiModelImage> images;
            if (r.contains("images")) {
                if (!r["images"].is_array() || r["images"].size() > 10)
                    throw std::runtime_error("Images must be an array with at most 10 items.");
                for (const auto& value : r["images"]) {
                    if (!value.is_object() || !value.contains("path") || !value["path"].is_string() ||
                        !value.contains("mimeType") || !value["mimeType"].is_string())
                        throw std::runtime_error("Each image needs path and mimeType strings.");
                    images.push_back({value["path"].get<std::string>(), value["mimeType"].get<std::string>()});
                }
            }
            std::vector<MaiVideoPart> videos;
            if (r.contains("videos")) {
                if (!r["videos"].is_array() || r["videos"].size() > 10)
                    throw std::runtime_error("Videos must be an array with at most 10 items.");
                for (const auto& value : r["videos"]) {
                    if (!value.is_object() || !value.contains("path") || !value["path"].is_string() ||
                        !value.contains("mimeType") || !value["mimeType"].is_string())
                        throw std::runtime_error("Each video needs path and mimeType strings.");
                    videos.push_back({value["path"].get<std::string>(), value["mimeType"].get<std::string>()});
                }
            }
            const auto capability = maiCloudModelInfo(selectedModel);
            if ((!images.empty() || !videos.empty()) && ((capability && !capability->supportsImages) ||
                                                         (selectedModel == localModelName && !localSupportsImages)))
                throw NativeOperationFailure({MaiErrorCode::NotSupported,
                                              "The target session model accepts text only; its draft was preserved."});
            if (r.contains("quoteMessageId") && !r["quoteMessageId"].is_string())
                throw std::runtime_error("quoteMessageId must be a string.");
            if (op == "enqueue") {
                if (!r.contains("requestId") || !r["requestId"].is_string())
                    throw std::runtime_error("Enqueue requires a stable requestId.");
                id = result(agent->submit(
                    MaiEnqueuePrompt{MaiSendPrompt{session, r.at("text"), std::move(images), std::move(videos),
                                                   r.value("quoteMessageId", std::string{})},
                                     r["requestId"]}));
            } else if (op == "steer") {
                if (!r.contains("turnId") || !r["turnId"].is_string() || !r.contains("requestId") ||
                    !r["requestId"].is_string())
                    throw std::runtime_error("Steering requires turnId and requestId strings.");
                // 指定现有轮次，不改模型或工作区，也不在 idle 时隐式回退为新 send。
                id = result(
                    agent->submit(MaiSteerPrompt{session, r["turnId"], r["requestId"], r.at("text"), std::move(images),
                                                 std::move(videos), r.value("quoteMessageId", std::string{})}));
                acceptedUserId = id;
                acceptedTurnId = r["turnId"];
            } else {
                id = result(agent->submit(MaiSendPrompt{session, r.at("text"), std::move(images), std::move(videos),
                                                        r.value("quoteMessageId", std::string{})}));
                acceptedTurnId = id;
                // Send returns the initial assistant ID. Its immediately preceding
                // row in the same conversation is the submitted user; a strict
                // before cursor stays tied to this receipt even after new output.
                const auto previous = agent->listMessagesPage(session, id, 1);
                if (!previous.empty() && previous.front().role == MaiRole::User) acceptedUserId = previous.front().id;
            }
        } else if (op == "stop") {
            // 可选轮次来自点击时的所有者快照；兼容旧调用，但不能让迟到取消影响新任务。
            if (r.contains("turnId") && !r["turnId"].is_string())
                throw NativeOperationFailure(
                    MaiError::make(MaiErrorCode::InvalidInput, "Stop turnId must be a string."));
            const auto expectedTurn = r.value("turnId", std::string{});
            result(agent->interruptTurn(session, expectedTurn));
            if (scheduleRunner) scheduleRunner->finish(session, "Interrupted by user");
        } else if (op == "delete")
            result(agent->submit(MaiDeleteSession{session}));
        else if (op == "clear")
            result(agent->submit(MaiClearMessages{session}));
        else if (op == "rename")
            result(agent->submit(MaiUpdateSession{session, r.at("title"), "", ""}));
        else if (op == "permission") {
            MaiPermissionDecision decision = MaiPermissionDecision::Denied;
            if (!maiParsePermissionDecision(r.at("decision"), decision))
                throw std::runtime_error("Unknown permission decision.");
            result(agent->submit(MaiReplyPermission{r.at("id"), decision}));
        } else if (op == "answer") {
            if (!r.contains("id") || !r["id"].is_string() || !r.contains("text") || !r["text"].is_string() ||
                session.empty() || r["id"].get<std::string>().empty() || r["id"].get<std::string>().size() > 128 ||
                r["text"].get<std::string>().empty() || r["text"].get<std::string>().size() > 16 * 1024)
                throw NativeOperationFailure(MaiError::make(MaiErrorCode::InvalidInput, "Question answer is invalid."));
            // 回答协议只接收自由文字。媒体草稿属于用户，不能在接收文字时悄悄忽略。
            for (const char* field : {"images", "videos", "local_files", "resolved_attachments", "attachments"})
                if (r.contains(field) && !r[field].empty())
                    throw NativeOperationFailure(MaiError::make(
                        MaiErrorCode::InvalidInput, "Question answers accept text only; keep media as a draft."));
            const std::string questionId = r["id"];
            bool owned = false;
            for (const auto& question : agent->listPendingQuestions())
                if (question.id == questionId && maiQuestionBelongsToSession(*agent, session, question.sessionId)) {
                    owned = true;
                    break;
                }
            if (!owned)
                throw NativeOperationFailure(
                    MaiError::make(MaiErrorCode::NotFound, "Question is not pending in this conversation."));
            result(agent->submit(MaiReplyQuestion{questionId, r["text"]}));
            Json receipt{{"ok", true}, {"question_id", questionId}};
            if (r.contains("requestId") && r["requestId"].is_string()) receipt["requestId"] = r["requestId"];
            return receipt;
        } else
            throw std::runtime_error("Unknown operation.");
        Json response{{"ok", true}, {"id", id}};
        if (op == "send" || op == "steer") {
            response["user_message_id"] = acceptedUserId;
            response["turnId"] = acceptedTurnId;
        }
        return response;
    }
};

void* maiMobileAgentCreate(void) {
    try {
        return new MaiMobileAgent();
    } catch (...) {
        return nullptr;
    }
}
void maiMobileAgentDestroy(void* handle) {
    delete static_cast<MaiMobileAgent*>(handle);
}
int maiMobileAgentSetHostToolHandler(void* handle, void* context, MaiMobileHostToolHandler handler,
                                     MaiMobileHostToolResponseFree responseFree,
                                     MaiMobileHostToolContextRelease contextRelease) {
    if (handle == nullptr) return 0;
    auto* mobile = static_cast<MaiMobileAgent*>(handle);
    if (mobile->anyBusy()) return 0;
    return setMaiMobileHostToolHandler(mobile->hostTools, context, handler, responseFree, contextRelease) ? 1 : 0;
}
int maiMobileAgentSetOrtApiBase(void* handle, const void* apiBase) {
    if (handle == nullptr || apiBase == nullptr) return 0;
    auto* mobile = static_cast<MaiMobileAgent*>(handle);
    if (mobile->agent != nullptr) return 0;
    mobile->ortApiBase = apiBase;
    return 1;
}
int maiMobileAgentSetChangeHandler(void* handle, MaiMobileAgentChangeHandler handler) {
    if (handle == nullptr) return 0;
    auto* mobile = static_cast<MaiMobileAgent*>(handle);
    if (mobile->agent != nullptr) return 0;
    mobile->changeHandler = handler;
    return 1;
}
char* maiMobileMatteVideo(const char* argumentsJson, const char* workspace, const char* modelPath,
                          const char* runtimePath, const void* apiBase) {
    try {
        if (argumentsJson == nullptr || workspace == nullptr || modelPath == nullptr)
            return ownedResponse(R"({"ok":false,"error":"invalid matting arguments"})");
        auto tool =
            makeMaiVideoMattingTool(maiMatteVideo, modelPath, runtimePath != nullptr ? runtimePath : "", apiBase);
        if (!tool) return ownedResponse(R"({"ok":false,"error":"video matting is unavailable"})");
        MaiToolContext context;
        context.root = workspace;
        context.allowOutsideWorkingDirectory = true;
        const MaiToolResult result = tool->execute(argumentsJson, context);
        if (result.hasError())
            return ownedResponse(Json{{"ok", false},
                                      {"errorCode", maiErrorCodeToString(result.error().code())},
                                      {"error", result.error().message()}}
                                     .dump());
        return ownedResponse(Json{{"ok", true}, {"output", result.output()}}.dump());
    } catch (const std::exception& error) {
        return ownedResponse(Json{{"ok", false}, {"error", error.what()}}.dump());
    }
}
char* maiMobilePdfEncrypt(const char* source, const char* output, const unsigned char* password, size_t passwordSize,
                          int replaceExisting) {
    try {
        if (!source || !output || !password || passwordSize == 0 || passwordSize > 127 ||
            (replaceExisting != 0 && replaceExisting != 1))
            return ownedResponse(
                R"({"ok":false,"errorCode":"invalid_input","error":"Invalid PDF encryption arguments"})");
        MaiPdfEncryptionOptions options;
        options.userPassword.assign(reinterpret_cast<const char*>(password), passwordSize);
        options.replaceExisting = replaceExisting != 0;
        const auto result = maiEncryptPdfFile(MaiFilePath::fromUtf8(source), MaiFilePath::fromUtf8(output), options);
        if (!result)
            return ownedResponse(Json{{"ok", false},
                                      {"errorCode", maiErrorCodeToString(result.error().code())},
                                      {"error", result.error().message()}}
                                     .dump());
        return ownedResponse(Json{{"ok", true},
                                  {"output", Json{{"path", result.value().path.toUtf8()},
                                                  {"bytes", result.value().bytes},
                                                  {"page_count", result.value().pageCount},
                                                  {"password_protected", true}}}}
                                 .dump());
    } catch (...) {
        // 原生异常不能跨语言边界，也不把可能包含密码的异常文本转回宿主日志。
        return ownedResponse(R"({"ok":false,"errorCode":"internal","error":"PDF encryption failed"})");
    }
}
char* maiMobileAgentRequest(void* handle, const char* request) {
    std::string response;
    try {
        if (!handle || !request) throw std::runtime_error("Invalid bridge handle.");
        response = static_cast<MaiMobileAgent*>(handle)->request(Json::parse(request)).dump();
    } catch (const NativeOperationFailure& error) {
        response = Json{{"ok", false}, {"errorCode", maiErrorCodeToString(error.code)}, {"error", error.what()}}.dump();
    } catch (const std::exception& e) {
        response = Json{{"ok", false}, {"errorCode", "internal"}, {"error", e.what()}}.dump();
    } catch (...) {
        response = "{\"ok\":false,\"error\":\"Native agent failure.\"}";
    }
    return ownedResponse(response);
}
void maiMobileAgentFree(char* response) {
    std::free(response);
}
