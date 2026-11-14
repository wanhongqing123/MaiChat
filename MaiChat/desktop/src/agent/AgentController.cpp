// 文件作用：桌面端创作中心和 Agent 会话；AgentController 负责流程编排、异步回调和状态转换，本文件给出其实现。
// 主要入口：openError、lastError、agent、createSession。
// 直接依赖：MaiAgentSendMediaTool.h、MaiAppStorageTool.h、MaiArkAssetTools.h、MaiArkMediaTools.h；实现与这些接口协作。
#include "agent/AgentController.h"
#include "agent/DesktopAccountModelClient.h"
#include "MaiAgentProgressInstructions.h"
#include "MaiAgentSendMediaTool.h"
#include "MaiAppStorageTool.h"
#include "MaiArkAssetTools.h"
#include "MaiArkMediaTools.h"
#include "MaiGlmMediaTools.h"
#include "MaiMemoryStore.h"
#include "MaiMiniMaxMediaTools.h"
#include "MaiModelStudioTools.h"
#include "MaiOpenAiClient.h"
#include "MaiSqliteStore.h"
#include "MaiSessionTools.h"
#include "MaiSystemResourcesTool.h"
#include "MaiQrCode.h"
#include "MaiSessionSchedule.h"
#include "MaiCalendarTools.h"
#include "MaiCloudModelCatalog.h"
#include "MaiProviderWebTools.h"
#include "MaiTime.h"
#if defined(MAICHAT_CV_VIDEO_ANALYSIS)
#include "MaiCvImageTools.h"
#include "MaiCvVideoAnalysis.h"
#include "MaiCvVideoTools.h"
#include "MaiVideoMatting.h"
#include "MaiVideoMattingTool.h"
#endif
#include "MaiPdfTool.h"
#if defined(MAICHAT_EMBEDDED_FFMPEG)
#include "MaiFfmpegTools.h"
#include "MaiVideoGeometryTool.h"
#include "mai_fftools_embed.h"
#endif
#include "MaiChatTools/DesktopPdfRenderer.h"
#include "MaiChatTools/DesktopPdfPasswordPrompt.h"
#include "MaiChatTools/DesktopCalendarEvents.h"
#include "MaiChatTools/DesktopProcessTools.h"
#include "MaiChatTools/DesktopBrowserTools.h"
#include "MaiChatTools/DesktopComputerTools.h"
#if defined(MAICHAT_DESKTOP_PORTRAIT)
#include "MaiChatTools/DesktopPortraitTools.h"
#endif
#if defined(Q_OS_MAC) || defined(Q_OS_WIN)
#include "agent/MaiDesktopGpu.h"
#endif
#if defined(MAICHAT_DESKTOP_VISION)
#include "MaiChatTools/DesktopVisionTools.h"
#endif

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMetaType>
#include <QMimeDatabase>
#include <QStandardPaths>
#include <QTimer>
#include <cstddef>
#include <utility>
#include <variant>

namespace {

// 桌面端与移动端一致：先从工具描述初筛，再以 discover
// 的真实接线状态选择候选模型。 这里仅约束主模型如何读取能力，不给通用 Agent
// 核心添加任何单一供应商的硬路由。
// 屏幕截图走原生工具；模型回看像素、同一工作区文件交付和图像能力边界分别说明。
const char* const kDesktopSystemPrompt =
    R"(You are the AI assistant embedded in MaiChat.
Video generation rule: before any paid video delegate, use discover to identify configured
specialists with implemented capabilities for this request. Read descriptions for initial routing,
then call discover on each plausible video specialist before recommending one. Compare actual
tool_status and input-combination limits; model_support or api_support alone does not prove the
App can execute a path. Use the JSON schema for exact fields. Do not infer missing capabilities
from model names, marketing pages, or another model's limits. If the user has not chosen a video
model for this task, show suitable names and task-specific differences and ask the user to choose;
confirm the sole suitable model too. Do not choose or switch video models silently. Reuse a choice
already made for this task, but ask before any replacement model's paid call. When one specialist
offers multiple billable variants, show their supported duration and resolution and confirm the
variant too. A model choice is
separate from paid-call approval. Classify an input rejection before changing anything. If the
cause is ambiguous, inspect the actual image and prompt without claiming which one failed.
When replacing a person in a source video, preserve that video and the user's references and
plan the selected video model's edit path. Do not remove watermarks, subtitles, or end cards as
an unrequested preparation step. The dedicated local watermark repair specialist is currently
available in the iOS Agent only. If the user explicitly requests watermark removal here, explain
that limitation. Do not loop through FFmpeg/OpenCV as a fallback. Return to the original task.
For image and video specialist tools, local media paths are upload inputs: the tool sends
short-lived private OSS HTTPS URLs to the provider. If OSS upload fails, stop before paid
submission and do not embed local media bytes as Base64.
For Kling video, use kling_bailian_video with the Beijing Wan API Key and Workspace. Confirm its
Turbo, Standard, or Omni variant before paid submission.
Paid image generation is temporarily disabled to control costs. This includes text-to-image,
image editing, reference generation, and paid revisions through every image specialist. Do not
submit, revise, retry, or substitute another paid image model. Existing supported task recovery
remains available internally. Local image viewing, cropping, and format conversion remain available.
The later provider retry guidance applies only after image generation is explicitly re-enabled;
it never authorizes a currently paused image call.
Wan video generation (wan_video) is temporarily disabled to control costs. Do not submit or
revise Wan video tasks, retry them, or silently choose a replacement model. Existing submitted
tasks can still be recovered internally. Other listed video models are separate;
ask the user to choose before using a different video model.
Do not default to
flipping or oil-painting a real face to evade review. For other image issues, if an image edit is
allowed and still serves the user's goal, preserve the original, make a light
FFmpeg/OpenCV edit, preview it, and retry the selected model with new paid approval. If
image-related rejection persists, consider an FFmpeg oil-paint-style derivative and describe
permitted details lost in processing. Do not switch models before these bounded same-model
attempts. If the provider explicitly forbids a recognizable real-person reference without
an authorized asset and the user needs that person's identity, do not transform the photo to hide
the face and reconstruct it in the prompt. Keep the selected model; briefly offer its authorized-
asset path or a visibly fictional character. Offer another model only if the user wants the
original person's likeness, and ask before switching. Avoid lengthy technical
error narration.
Format every user-facing text response as valid GitHub Flavored Markdown (GFM), preserving actual
line breaks. Plain prose is valid Markdown; use headings, lists, fenced code blocks, links, and
tables only when they improve readability. Put a blank line before and after each table. Put the
header, delimiter row, and every table row on separate lines, with one delimiter cell per column.
Never imitate a table by writing pipe-separated rows in a single paragraph. Do not wrap the entire
response in a code fence.
If the user attached an image, it is already included in the current model request. Inspect that
image directly. Do not call view_image for a filename merely visible inside a screenshot.
On desktop, file tools accept absolute paths wherever the operating system allows access; relative
paths start in this session's working directory. If an original image is needed, use its accessible
path or ask the user to attach it.
For local file work, prefer file_create, file_create_directory, file_delete, file_read, file_write,
file_edit, and file_patch. file_create makes an empty file without overwriting; file_write sets content. Use
shell only when the built-in tools cannot perform the task.
For general desktop operation, use desktop_computer list_displays, then observe with target=display
and a display_id. This returns a whole-screen image bound to a snapshot and native input bounds.
Display-target actions can switch applications and return the resulting screen with a new snapshot;
inspect that observation before deciding the next action or claiming completion. Window-target observe
still returns only window metadata and Accessibility/UIA elements, without window image capture.
For standalone screen images, screenshot reuses the same resident native capture session and has no
window capture mode. Use screenshot action=stop when screen capture is no longer needed.
To show a capture in this
conversation, call agent_send_media with file_path set to the returned workspace-relative image_path
and type=image;
reuse that same file. Do not write shell scripts to recapture the screen or bypass a failed native
capture. If the primary model cannot accept images, explain that limitation and use only supported
image-free observations; never claim to have inspected pixels that were not provided.
Do not invent model quality, policy, or price claims.
Before calling delegate or revise on any paid image or video generation specialist, show the
user the concrete plan and let the tool's per-call approval be the final confirmation. For
video, name subject, clothing, scene, action, duration, resolution, and aspect ratio. Ask for
missing duration, resolution, or aspect ratio instead of silently using defaults. Read the selected
specialist's actual duration range; do not assume a universal 10-second limit or shorten a longer
request. Match the paid
call to the shown subject, clothing, scene, action, reference count, duration, aspect ratio, and
actual billable resolution. Check discover or local validation before approval; do not pass an
unsupported resolution or silently change it. Explain any local output conversion before approval.
If approval is rejected, do not submit or retry. Use model tools for semantic changes such as person,
clothes, scene, or action. Prefer local FFmpeg/OpenCV for geometry, crop, color,
stabilization, frame joining, or speed changes to preserve identity pixels. Reuse existing
cloud task IDs for progress checks instead of submitting a paid duplicate.
When the user asks to cancel a video job, use an available specialist's cancel action with its
saved conversation_id. Model Studio cancels only queued/PENDING jobs; if already running,
report the actual provider refusal and keep checking the existing job. Wan and Bailian Kling only
remove delivered local task records;
do not claim cloud deletion or remove the downloaded video in that case.
GLM and MiniMax likewise only delete local records after delivery; no verified cloud cancel
endpoint is wired for them, so do not claim their running cloud tasks were canceled.
For multiple user images, inspect the chosen video specialist's input limits and never silently
drop an image.
If the limit is exceeded, discover an available image specialist for semantic fusion or use
FFmpeg for a visible layout, then show the plan before a paid call. On task failure, inspect the provider
stage and reason, repair technical inputs when possible, and seek a new per-call approval before
any paid retry. Do not loop on provider content rejection or imply that an ambiguous error
identifies one input. Treat a specialist error as internal work to resolve, not an immediate
user-facing answer. Keep the original task and media references. If one provider lacks balance
or access, discover another capable configured specialist; for video generation, continue there
only after the user confirms the replacement model. Never
ask the user to recharge while a practical alternative remains. If an accepted task's status
is unknown, preserve its task ID and do not silently submit a duplicate. After delivering the
result, you may briefly explain a provider change. Only if no viable route remains should you
tell the user why the task could not be completed. For ambiguous image-related moderation failures,
preserve the original and inspect the actual image and prompt. Try a light permitted image-tool edit first,
then retry the same provider after a new paid approval. Only if that fails and image evidence
remains, try a stronger FFmpeg oil-paint-style edit as the last image-processing step. Preview each derivative,
record what visual details were lost, and describe permitted details accurately in the next prompt
so the result stays close to the user's goal. Do not infer an image problem from an ambiguous error,
disguise explicitly prohibited material, reconstruct a prohibited identity, or promise an edit
will pass moderation. Consider another
provider only after bounded same-provider attempts fail.)";

std::string toUtf8(const QString& text) {
    const QByteArray bytes = text.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

QString fromUtf8(const std::string& text) {
    return QString::fromUtf8(text.data(), static_cast<int>(text.size()));
}

// 界面要知道一个片段是正文、思考还是工具卡。增量事件本身不带这个信息（见头文件里那一段），
// 所以查一次记下来。
enum class PartKind { Unknown, Text, Reasoning, Tool };

}  // namespace

struct AgentController::Runtime {
    Runtime(QString model, QString effort)
        : initialDefaultModel(std::move(model)), initialDefaultReasoningEffort(std::move(effort)),
          newSessionDefaultModel(initialDefaultModel) {}

    bool supportsModel(const QString& model) const {
        if (!modelClient) return maiCloudModelInfo(toUtf8(model)).has_value();
        const auto wire = modelClient->wireApiForModel(toUtf8(model));
        return wire && *wire != MaiWireApi::Mixed;
    }

    std::unique_ptr<MaiAgent> agent;
    // 由 agent 持有，生命周期覆盖所有普通配置查询；不另建一份 wire/能力表。
    MaiModelClient* modelClient = nullptr;
    DesktopAccountModelClient* accountClient = nullptr;
    bool ordinaryConfigured = false;
    std::shared_ptr<MaiSessionScheduleStore> scheduleStore;
    std::unique_ptr<MaiSessionScheduleRunner> scheduleRunner;
    QTimer scheduleTimer;
    MaiEventBus::Token token = 0;
    QString openError;
    QString lastError;
    // 只有主线程碰它：所有事件都排队到主线程之后才分类。
    QHash<QString, QHash<QString, PartKind>> partKindsBySession;
    QHash<QString, QHash<QString, QString>> livePartTextBySession;
    // 旧空值只继承初始默认；改未来默认不能重新解释已有会话。
    const QString initialDefaultModel;
    const QString initialDefaultReasoningEffort;
    QString newSessionDefaultModel;
};

namespace {

QString initialModel(const QString& model) {
    return model.isEmpty() ? fromUtf8(MaiAgent::Options{}.defaultModel) : model;
}

QString initialReasoningEffort(const QString& effort) {
    return maiIsValidReasoningEffort(toUtf8(effort)) ? effort : QStringLiteral("high");
}

// 存储打不开时退化成内存存储，而不是让整个界面起不来。
//
// 理由：数据库打不开的常见原因是磁盘满、路径没权限、或者文件被另一个进程占着。
// 这些情况下用户仍然应该能用
// agent（只是这次的对话不会留下来），而不是面对一个打不开的窗口。 错误从
// openError() 取，界面自己决定怎么提示。
std::unique_ptr<MaiSessionStore> openStore(const QString& databasePath, QString& openError) {
    if (databasePath.isEmpty()) return makeMaiMemoryStore();

    auto opened = makeMaiSqliteStore(toUtf8(databasePath));
    if (!opened) {
        openError = fromUtf8(opened.error().message());
        return makeMaiMemoryStore();
    }
    return std::move(opened.value());
}

std::shared_ptr<MaiSessionScheduleStore> openScheduleStore(const QString& databasePath) {
    const std::string path = databasePath.isEmpty() ? ":memory:" : toUtf8(databasePath) + ".schedule.sqlite";
    const auto opened = MaiSessionScheduleStore::open(path);
    return opened ? opened.value() : nullptr;
}

std::unique_ptr<MaiAgent>
// 为桌面端装配 MaiAgent 核心、模型客户端和平台宿主工具。
// 工具的权限及工作目录在这里统一传入，实际操作仍由工具入口负责校验。
buildAgent(std::unique_ptr<MaiModelClient> model, std::unique_ptr<MaiSessionStore> store, const QString& defaultModel,
           MaiApprovalPolicy approvalPolicy, const AgentController::ToolRegistrar& hostTools,
           const AgentController::ModelConfig& credentials, const QString& databasePath,
           const std::shared_ptr<MaiSessionScheduleStore>& scheduleStore) {
    // MaiAgent 内置工具总是装着；平台不支持的能力由核心在注册时排除。
    // 会话没有工作目录时工具层会明确拒绝，
    // 要不要把工具声明给模型看是 MaiContextBuilder 的事，不是这里的。
    auto tools = std::make_unique<MaiToolRegistry>();
    registerMaiBuiltinTools(*tools);
    // 搜索/抓取只填宿主凭据与证书；普通工具由 Core 按该轮固定型号选择供应商。
    MaiProviderWebOptions webOptions;
    QFile webCertificates(QStringLiteral(":/maichat/auth/cacert.pem"));
    if (webCertificates.open(QIODevice::ReadOnly)) {
        const std::string caPem = toUtf8(QString::fromUtf8(webCertificates.readAll()));
        webOptions.deepseek.caPem = caPem;
        webOptions.glm.caPem = caPem;
    }
    webOptions.deepseek.apiKey = [provider = credentials.accountModelKeyProvider,
                                  key = toUtf8(credentials.deepseekApiKey)] {
        return provider ? toUtf8(provider(QStringLiteral("deepseek"))) : key;
    };
    webOptions.glm.apiKey = [provider = credentials.accountModelKeyProvider,
                            glmProvider = credentials.glmApiKeyProvider, key = toUtf8(credentials.glmApiKey)] {
        if (provider) return toUtf8(provider(QStringLiteral("glm")));
        return glmProvider ? toUtf8(glmProvider()) : key;
    };
    registerMaiProviderWebTools(*tools, std::move(webOptions));
    registerMaiQrCodeTools(*tools);
#if defined(Q_OS_MAC)
    registerMaiSessionScheduleTools(*tools, scheduleStore, "Mac");
#else
    registerMaiSessionScheduleTools(*tools, scheduleStore, "Windows");
#endif
    registerMaiCalendarTools(*tools,
        [](const MaiCalendarEventInput& event, const MaiToolContext& context) {
            return prepareDesktopCalendarImport(event, context);
        });
    const QString defaultWorkspace = databasePath.isEmpty() ? QString() :
        QDir(QFileInfo(databasePath).absolutePath()).filePath(QStringLiteral("Workspace"));
    tools->add(makeMaiCreateSessionTool(MaiSessionCreateMode::DefaultOrExplicitDirectory,
                                        toUtf8(defaultWorkspace)));
#if defined(MAICHAT_DESKTOP_PORTRAIT)
    // 数据库和服务随当前账号 AgentController 一起重建；模型不能指定 profile 路径。
    if (!databasePath.isEmpty())
        registerDesktopPortraitTools(*tools, QFileInfo(databasePath).absolutePath());
#endif
    registerDesktopProcessTools(*tools, std::make_shared<DesktopProcessManager>());
    registerDesktopBrowserTools(*tools, std::make_shared<DesktopBrowserManager>());
    registerDesktopComputerTools(*tools, std::make_shared<DesktopComputerController>());
#if defined(MAICHAT_EMBEDDED_FFMPEG)
    const MaiFfmpegEngine ffmpegEngine{mai_ffmpeg_execute,       mai_ffmpeg_set_cancel_check,
                                       mai_ffprobe_execute,      mai_ffprobe_set_cancel_check,
                                       mai_fftools_set_log_sink, mai_fftools_error_string};
    const MaiVideoMetadataProvider seedanceVideoMetadata = makeMaiFfprobeVideoMetadataProvider(ffmpegEngine);
#else
    const MaiVideoMetadataProvider seedanceVideoMetadata;
#endif
#if defined(Q_OS_MAC) || defined(Q_OS_WIN)
    tools->add(makeMaiSystemResourcesTool(maiDesktopGpuResources));
#endif
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    const QString temporaryBase = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    const QString temporary = QDir(temporaryBase).filePath(QStringLiteral("MaiChatAgentTemp"));
    if (!cache.isEmpty() && !temporaryBase.isEmpty() && QDir().mkpath(cache) && QDir().mkpath(temporary)) {
        tools->add(makeMaiAppStorageTool({toUtf8(temporary), toUtf8(cache), {}}));
    }
    tools->add(makeMaiAgentSendMediaTool());
    const auto arkKey = [provider = credentials.arkApiKeyProvider, key = toUtf8(credentials.arkApiKey)] {
        return provider ? toUtf8(provider()) : key;
    };
    const auto assetSettings = [provider =
                                    credentials.mediaServiceAccessProvider]() -> MaiResult<MaiArkAssetServiceSettings> {
        if (!provider) return {MaiErrorCode::NotConfigured, "Account login is required"};
        const auto access = provider();
        if (access.first.isEmpty() || access.second.isEmpty())
            return {MaiErrorCode::NotConfigured, "Account login is required"};
        return MaiArkAssetServiceSettings{toUtf8(access.first), toUtf8(access.second)};
    };
    // 桌面端与手机端使用同一 C++ OSS 上传实现，每个模型写入自己的明确目录。
    const bool assetsConfigured = static_cast<bool>(assetSettings());
    const auto modelUploadMedia = [&](MaiOssUploadPurpose purpose) {
        return assetsConfigured ? makeMaiPrivateOssMediaUploader(assetSettings, {}, purpose)
                                : MaiCreativeMediaUploadProvider{};
    };
    const MaiCreativeMediaUploadProvider seedreamUploadMedia = modelUploadMedia(MaiOssUploadPurpose::SeedreamInput);
    const MaiCreativeMediaUploadProvider miniMaxUploadMedia = modelUploadMedia(MaiOssUploadPurpose::MiniMaxInput);
    const MaiCreativeMediaUploadProvider klingUploadMedia = modelUploadMedia(MaiOssUploadPurpose::KlingInput);
    const MaiCreativeMediaUploadProvider wanUploadMedia = modelUploadMedia(MaiOssUploadPurpose::WanInput);
    const MaiArkAssetProvider assetProvider =
        assetsConfigured ? makeMaiArkAssetServiceProvider(assetSettings) : MaiArkAssetProvider{};
    // 不提供给主模型；保留实例供后台轮询已提交的 Seedance 任务。
    tools->addInternal(makeMaiSeedanceVideoTool(arkKey, {}, assetProvider, seedanceVideoMetadata));
    if (assetsConfigured)
        tools->addInternal(makeMaiArkAssetTool(assetProvider));
    else
        tools->addInternal(makeMaiArkAssetTool());
    tools->addInternal(makeMaiSeedreamImageTool(arkKey, {}, seedreamUploadMedia));
    const auto glmKey = [provider = credentials.glmApiKeyProvider, key = toUtf8(credentials.glmApiKey)] {
        return provider ? toUtf8(provider()) : key;
    };
    // 费用暂停只隐藏付费图像生成；保留实例供已接线的旧任务恢复，普通工厂同样受执行闸门约束。
    tools->addInternal(makeMaiGlmImageTool(glmKey));
    const auto miniMaxKey = [provider = credentials.miniMaxApiKeyProvider, key = toUtf8(credentials.miniMaxApiKey)] {
        return provider ? toUtf8(provider()) : key;
    };
    tools->add(makeMaiMiniMaxVideoTool(miniMaxKey, {}, miniMaxUploadMedia));
    tools->addInternal(makeMaiMiniMaxImageTool(miniMaxKey, {}, miniMaxUploadMedia));
    const auto wanCredentials = [provider = credentials.wanCredentialsProvider, key = toUtf8(credentials.wanApiKey),
                                 workspace = toUtf8(credentials.wanWorkspaceId)] {
        if (provider) {
            const auto current = provider();
            return MaiWanCredentials{toUtf8(current.first), toUtf8(current.second)};
        }
        return MaiWanCredentials{key, workspace};
    };
    // 因费用暂停万相视频的新任务；后台仍按原名字查询和回收已提交任务。
    tools->addInternal(makeMaiWanVideoTool(wanCredentials, {}, wanUploadMedia));
    tools->add(makeMaiBailianKlingVideoTool(wanCredentials, {}, klingUploadMedia));
    tools->addInternal(makeMaiWanImageTool(wanCredentials, {}, wanUploadMedia));
    tools->addInternal(makeMaiBailianKlingImageTool(wanCredentials, {}, klingUploadMedia));
#if defined(MAICHAT_EMBEDDED_FFMPEG)
    tools->add(makeMaiViewImageTool(makeMaiFfmpegImagePreview(ffmpegEngine)));
    tools->add(makeMaiFfmpegTool(ffmpegEngine));
    tools->add(makeMaiFfprobeTool(ffmpegEngine));
    tools->add(makeMaiStretchLowerVideoTool(ffmpegEngine));
#endif
#if defined(MAICHAT_CV_VIDEO_ANALYSIS)
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
#if defined(MAICHAT_RVM_BUNDLED)
    const QDir applicationDirectory(QCoreApplication::applicationDirPath());
#if defined(Q_OS_WIN)
    const QString modelPath = applicationDirectory.filePath(QStringLiteral("MaiAgentModels/rvm_mobilenetv3_fp32.onnx"));
    const QString runtimePath = applicationDirectory.filePath(QStringLiteral("onnxruntime.dll"));
    const QString providerPath = applicationDirectory.filePath(QStringLiteral("onnxruntime_providers_shared.dll"));
    const bool assetsAvailable =
        QFileInfo::exists(modelPath) && QFileInfo::exists(runtimePath) && QFileInfo::exists(providerPath);
#else
    const QString modelPath =
        applicationDirectory.filePath(QStringLiteral("../Resources/MaiAgentModels/rvm_mobilenetv3_fp32.onnx"));
    const QString runtimePath =
        applicationDirectory.filePath(QStringLiteral("../Frameworks/libonnxruntime.1.26.0.dylib"));
    const bool assetsAvailable = QFileInfo::exists(modelPath) && QFileInfo::exists(runtimePath);
#endif
    if (assetsAvailable) {
        tools->add(makeMaiVideoMattingTool(maiMatteVideo, toUtf8(modelPath), toUtf8(runtimePath)));
    }
#endif
#endif
    // 密码只由本机私有表单交给普通加密 API，不成为工具输入或输出，也不另加审批。
    tools->add(makeMaiPdfTool(renderDesktopPdf, requestDesktopPdfPassword));
#if defined(MAICHAT_DESKTOP_VISION)
    registerDesktopVisionTools(*tools);
#endif
    if (hostTools) hostTools(*tools);

    MaiAgent::Options options;
    if (!defaultModel.isEmpty()) options.defaultModel = toUtf8(defaultModel);
    // 会话空值继承这个构造期常量，不再逐请求读取可变的 UI 全局设置。
    options.reasoningEffort = toUtf8(initialReasoningEffort(credentials.reasoningEffort));
    // 0 = 无限等。桌面端有人盯着，超时自动拒绝等于替用户做决定。
    options.permissionTimeoutMs = 0;
    options.approvalPolicy = approvalPolicy;
    options.allowOutsideWorkingDirectory = true;
    // 与移动端共用可见正文进度约定，使工具执行前后的说明保持一致。
    options.baseInstructions = kDesktopSystemPrompt + maiAgentProgressInstructions();

    return std::make_unique<MaiAgent>(std::move(store), std::move(model), std::move(tools), options);
}

}  // namespace

AgentController::AgentController(std::unique_ptr<MaiModelClient> model, const QString& databasePath, QObject* parent)
    : AgentController(std::move(model), databasePath, ToolRegistrar(), parent) {}

AgentController::AgentController(std::unique_ptr<MaiModelClient> model, const QString& databasePath,
                                 ToolRegistrar hostTools, QObject* parent)
    : QObject(parent),
      runtime_(std::make_unique<Runtime>(initialModel(QString()),
                                        initialReasoningEffort(ModelConfig{}.reasoningEffort))) {
    // 排队投递要求这个类型是注册过的。放在构造里而不是全局静态，是因为注册本身是幂等且线程安全的，
    // 而全局静态的初始化顺序不好讲。
    qRegisterMetaType<MaiEvent>("MaiEvent");

    runtime_->scheduleStore = openScheduleStore(databasePath);
    runtime_->modelClient = model.get();
    runtime_->ordinaryConfigured = model != nullptr;
    runtime_->agent = buildAgent(std::move(model), openStore(databasePath, runtime_->openError),
                                 runtime_->initialDefaultModel,
                                 MaiApprovalPolicy::OnRequest, hostTools, AgentController::ModelConfig{}, databasePath,
                                 runtime_->scheduleStore);

    // **显式 QueuedConnection，不用 Auto。**
    //
    // Auto 连接方式按“发射线程和接收方线程是不是同一个”在运行期决定：
    // 核心工作线程过来的走排队，
    // 而主线程自己调 submit()
    // 时同步发出的那几条（会话建好、消息建好）会走直连。两条路混着，
    // 事件到达界面的顺序就不再和发生顺序一致了——界面可能先收到某条增量，再收到"这条消息建好了"。
    //
    // 一律排队就都是先进先出，而且分类逻辑永远只在主线程上跑。
    connect(this, &AgentController::eventQueued, this, &AgentController::onEventQueued, Qt::QueuedConnection);

    // 订阅要放在最后：前面几步没做完就收到事件的话，onEventQueued
    // 会碰到半个对象。
    runtime_->token = runtime_->agent->eventBus().subscribe([this](const MaiEvent& event) { rawEvent(event); });
    startScheduledTasks();
}

AgentController::AgentController(const ModelConfig& model, const QString& databasePath, QObject* parent)
    : AgentController(model, databasePath, ToolRegistrar(), parent) {}

AgentController::AgentController(const ModelConfig& model, const QString& databasePath, ToolRegistrar hostTools,
                                 QObject* parent)
    : QObject(parent),
      runtime_(std::make_unique<Runtime>(initialModel(model.modelName), initialReasoningEffort(model.reasoningEffort))) {
    qRegisterMetaType<MaiEvent>("MaiEvent");
    runtime_->scheduleStore = openScheduleStore(databasePath);

    // baseUrl 为空 = 空转。建会话、翻历史照常，发消息会以 NotConfigured 收场。
    std::unique_ptr<MaiModelClient> client;
    if (model.accountModelKeyProvider) {
        DesktopAccountModelClient::Config config;
        config.glmBaseUrl = model.glmBaseUrl;
        config.deepseekBaseUrl = model.deepseekBaseUrl;
        config.keyProvider = model.accountModelKeyProvider;
#if defined(MAICHAT_EMBEDDED_FFMPEG)
        MaiFfmpegEngine ffmpegEngine{mai_ffmpeg_execute,       mai_ffmpeg_set_cancel_check,
                                     mai_ffprobe_execute,      mai_ffprobe_set_cancel_check,
                                     mai_fftools_set_log_sink, mai_fftools_error_string};
        config.prepareImage = makeMaiFfmpegModelImagePreparer(ffmpegEngine);
#endif
        auto accountClient = std::make_unique<DesktopAccountModelClient>(std::move(config));
        runtime_->accountClient = accountClient.get();
        client = std::move(accountClient);
    } else if (!model.baseUrl.isEmpty()) {
        MaiModelConfig config;
        config.baseUrl = toUtf8(model.baseUrl);
        config.apiKey = toUtf8(model.apiKey);
        config.wire = model.wire;
#if defined(MAICHAT_EMBEDDED_FFMPEG)
        MaiFfmpegEngine ffmpegEngine{mai_ffmpeg_execute,       mai_ffmpeg_set_cancel_check,
                                     mai_ffprobe_execute,      mai_ffprobe_set_cancel_check,
                                     mai_fftools_set_log_sink, mai_fftools_error_string};
        config.prepareImage = makeMaiFfmpegModelImagePreparer(ffmpegEngine);
#endif
        client = makeMaiModelClient(config);
    }

    runtime_->modelClient = client.get();
    runtime_->ordinaryConfigured = client && model.wire != MaiWireApi::Local && model.wire != MaiWireApi::Mixed;
    runtime_->agent = buildAgent(std::move(client), openStore(databasePath, runtime_->openError),
                                 runtime_->initialDefaultModel,
                                 model.approvalPolicy, hostTools, model, databasePath, runtime_->scheduleStore);

    connect(this, &AgentController::eventQueued, this, &AgentController::onEventQueued, Qt::QueuedConnection);
    runtime_->token = runtime_->agent->eventBus().subscribe([this](const MaiEvent& event) { rawEvent(event); });
    startScheduledTasks();
}

void AgentController::startScheduledTasks() {
    if (!runtime_->scheduleStore) return;
    runtime_->scheduleStore->markInterruptedUncertain();
    runtime_->scheduleRunner = std::make_unique<MaiSessionScheduleRunner>(
        runtime_->scheduleStore,
        [this](const std::string& id) {
            MaiSession session;
            return runtime_->agent->getSession(id, session) && session.isRoot();
        },
        [this](const std::string& id) { return runtime_->agent->isBusy(id); },
        [this](const std::string& id, const std::string& instruction) -> MaiResult<std::string> {
            return runtime_->agent->submit(MaiSendPrompt{id, instruction});
        });
    runtime_->scheduleTimer.setInterval(1000);
    connect(&runtime_->scheduleTimer, &QTimer::timeout, this, [this] {
        if (runtime_->scheduleRunner) runtime_->scheduleRunner->poll(MaiTime::getCurrentTime());
    });
    runtime_->scheduleTimer.start();
}

AgentController::~AgentController() {
    // 顺序很讲究，别调。
    //
    // 退订只是第一道：MaiEventBus
    // 明说了"退订返回之后，处理函数仍可能正在别的线程上执行"。
    // 真正让它安全的是下面那句 reset——MaiAgent
    // 的析构会叫停所有在跑的轮次**并等它们退出**， 返回之后就再没有线程能进到
    // rawEvent 里了。
    //
    // 反过来写（先 reset 再退订）也能跑，
    // 但析构过程中发出的最后几条事件会打到一个正在销毁的 QObject 上。
    runtime_->scheduleTimer.stop();
    runtime_->agent->eventBus().unsubscribe(runtime_->token);
    runtime_->agent.reset();
}

QString AgentController::openError() const {
    return runtime_->openError;
}

QString AgentController::lastError() const {
    return runtime_->lastError;
}

MaiAgent& AgentController::agent() {
    return *runtime_->agent;
}

// ---- 变更 ----

QString AgentController::createSession(const QString& directory, const QString& title) {
    // 显式保存创建时默认，随后切换其他会话或未来默认均不改变它。
    MaiResult<std::string> created = runtime_->agent->submit(
        MaiCreateSession{toUtf8(directory), toUtf8(title), toUtf8(runtime_->newSessionDefaultModel),
                         toUtf8(runtime_->initialDefaultReasoningEffort)});
    if (!created) {
        runtime_->lastError = fromUtf8(created.error().message());
        return QString();
    }
    return fromUtf8(created.value());
}

bool AgentController::setSessionDirectory(const QString& sessionId, const QString& directory) {
    const QFileInfo selected(directory);
    const QString canonical = selected.canonicalFilePath();
    if (sessionId.isEmpty() || !selected.isDir() || canonical.isEmpty()) {
        runtime_->lastError = QStringLiteral("工作目录不存在或不可访问。");
        return false;
    }
    MaiSession session;
    if (!runtime_->agent->getSession(toUtf8(sessionId), session)) {
        runtime_->lastError = QStringLiteral("当前会话不存在。");
        return false;
    }
    if (QDir(fromUtf8(session.directory)).canonicalPath() == canonical) return true;
    if (runtime_->agent->isBusy(toUtf8(sessionId))) {
        runtime_->lastError = QStringLiteral("任务进行中，请结束后再切换工作目录。");
        return false;
    }
    MaiResult<std::string> updated =
        runtime_->agent->submit(MaiUpdateSession{toUtf8(sessionId), "", "", "", toUtf8(canonical)});
    if (!updated) {
        runtime_->lastError = fromUtf8(updated.error().message());
        return false;
    }
    return true;
}

bool AgentController::sendPrompt(const QString& sessionId, const QString& text) {
    return sendPrompt(sessionId, text, {});
}

bool AgentController::sendPrompt(const QString& sessionId, const QString& text, const QStringList& imagePaths) {
    return sendPrompt(sessionId, text, imagePaths, QString());
}

bool AgentController::sendPrompt(const QString& sessionId, const QString& text, const QStringList& imagePaths,
                                 const QString& quotedMessageId) {
    return sendPrompt(sessionId, text, imagePaths, {}, quotedMessageId);
}

bool AgentController::sendPrompt(const QString& sessionId, const QString& text, const QStringList& imagePaths,
                                 const QStringList& videoPaths, const QString& quotedMessageId) {
    return submitPrompt(sessionId, text, imagePaths, videoPaths, quotedMessageId, {}, {});
}

bool AgentController::steerPrompt(const QString& sessionId, const QString& turnId, const QString& requestId,
                                  const QString& text, const QStringList& imagePaths, const QStringList& videoPaths,
                                  const QString& quotedMessageId) {
    if (turnId.isEmpty() || requestId.isEmpty()) {
        runtime_->lastError = QStringLiteral("补充消息需要绑定原任务与请求编号，内容已保留。");
        return false;
    }
    return submitPrompt(sessionId, text, imagePaths, videoPaths, quotedMessageId, turnId, requestId);
}

bool AgentController::submitPrompt(const QString& sessionId, const QString& text, const QStringList& imagePaths,
                                   const QStringList& videoPaths, const QString& quotedMessageId, const QString& turnId,
                                   const QString& requestId) {
    if (imagePaths.size() + videoPaths.size() > 10) {
        runtime_->lastError = QStringLiteral("每条消息最多添加 10 项图片或视频。");
        return false;
    }
    std::vector<MaiModelImage> images;
    images.reserve(static_cast<std::size_t>(imagePaths.size()));
    QMimeDatabase mimeDatabase;
    for (const QString& path : imagePaths) {
        const QString mimeType = mimeDatabase.mimeTypeForFile(path).name().toLower();
        if (mimeType != QStringLiteral("image/jpeg") && mimeType != QStringLiteral("image/png") &&
            mimeType != QStringLiteral("image/webp") && mimeType != QStringLiteral("image/gif")) {
            runtime_->lastError = QStringLiteral("暂不支持这种图片格式：%1").arg(mimeType);
            return false;
        }
        images.push_back({toUtf8(path), toUtf8(mimeType)});
    }
    std::vector<MaiVideoPart> videos;
    videos.reserve(static_cast<std::size_t>(videoPaths.size()));
    for (const QString& path : videoPaths) {
        const QFileInfo file(path);
        if (!file.isFile()) {
            runtime_->lastError = QStringLiteral("视频文件不存在：%1").arg(file.fileName());
            return false;
        }
        QString mimeType = mimeDatabase.mimeTypeForFile(path).name().toLower();
        const QString suffix = file.suffix().toLower();
        if (suffix == QStringLiteral("mp4"))
            mimeType = QStringLiteral("video/mp4");
        else if (suffix == QStringLiteral("mov"))
            mimeType = QStringLiteral("video/quicktime");
        else if (suffix == QStringLiteral("webm"))
            mimeType = QStringLiteral("video/webm");
        if (!mimeType.startsWith(QStringLiteral("video/"))) {
            runtime_->lastError = QStringLiteral("暂不支持这种视频格式：%1").arg(mimeType);
            return false;
        }
        videos.push_back({toUtf8(file.absoluteFilePath()), toUtf8(mimeType)});
    }
    // 普通发送与补充均由 Core 读取该会话配置，绝不把其他页面的选择写进目标会话。
    MaiResult<std::string> sent =
        turnId.isEmpty()
            ? runtime_->agent->submit(MaiSendPrompt{toUtf8(sessionId), toUtf8(text), std::move(images),
                                                    std::move(videos), toUtf8(quotedMessageId)})
            : runtime_->agent->submit(MaiSteerPrompt{toUtf8(sessionId), toUtf8(turnId), toUtf8(requestId), toUtf8(text),
                                                     std::move(images), std::move(videos), toUtf8(quotedMessageId)});
    if (!sent) {
        runtime_->lastError = fromUtf8(sent.error().message());
        return false;
    }
    return true;
}

bool AgentController::sessionModelConfiguration(const QString& sessionId, SessionModelConfiguration& out) const {
    out = {};
    MaiSession session;
    if (!runtime_->agent->getSession(toUtf8(sessionId), session)) return false;
    SessionModelConfiguration resolved;
    resolved.model = session.model.empty() ? runtime_->initialDefaultModel : fromUtf8(session.model);
    resolved.reasoningEffort = session.reasoningEffort.empty() ? runtime_->initialDefaultReasoningEffort
                                                              : fromUtf8(session.reasoningEffort);
    if (runtime_->modelClient) {
        const auto wire = runtime_->modelClient->wireApiForModel(toUtf8(resolved.model));
        if (!wire || *wire == MaiWireApi::Mixed) return false;
        resolved.wire = *wire;
        resolved.configured = runtime_->accountClient
                                  ? runtime_->accountClient->isConfiguredForModel(toUtf8(resolved.model))
                                  : runtime_->ordinaryConfigured;
    } else {
        // 未配置客户端仍允许显示会话选项；云型号协议由共享目录给出。
        const auto info = maiCloudModelInfo(toUtf8(resolved.model));
        if (!info) return false;
        resolved.wire = info->wire;
    }
    out = std::move(resolved);
    return true;
}

bool AgentController::setSessionModelConfiguration(const QString& sessionId, const QString& model,
                                                    std::optional<QString> reasoningEffort) {
    const QString selected = model.trimmed();
    if (sessionId.isEmpty() || selected.isEmpty()) {
        runtime_->lastError = QStringLiteral("模型名称不能为空。");
        return false;
    }
    if (!runtime_->supportsModel(selected)) {
        runtime_->lastError = QStringLiteral("当前客户端不支持此模型。");
        return false;
    }
    if (reasoningEffort && !maiIsValidReasoningEffort(toUtf8(*reasoningEffort))) {
        runtime_->lastError = QStringLiteral("思考强度仅支持 low、high、max 或继承默认。");
        return false;
    }
    MaiSession session;
    if (!runtime_->agent->getSession(toUtf8(sessionId), session)) {
        runtime_->lastError = QStringLiteral("当前会话不存在。");
        return false;
    }
    const QString currentModel = session.model.empty() ? runtime_->initialDefaultModel : fromUtf8(session.model);
    const QString currentEffort = session.reasoningEffort.empty() ? runtime_->initialDefaultReasoningEffort
                                                                 : fromUtf8(session.reasoningEffort);
    QString requestedEffort = currentEffort;
    if (reasoningEffort)
        requestedEffort = reasoningEffort->isEmpty() ? runtime_->initialDefaultReasoningEffort : *reasoningEffort;
    if (runtime_->agent->isBusy(toUtf8(sessionId))) {
        if (selected != currentModel || requestedEffort != currentEffort) {
            runtime_->lastError = QStringLiteral("任务进行中，请结束后再切换模型或思考强度。");
            return false;
        }
        // 有效值未变化时不把 legacy 空值物化，避免向活动轮次提交任何 mutation。
        runtime_->lastError.clear();
        return true;
    }
    const std::optional<std::string> storedEffort = reasoningEffort
        ? std::make_optional(toUtf8(*reasoningEffort)) : std::nullopt;
    if (toUtf8(selected) == session.model && (!storedEffort || *storedEffort == session.reasoningEffort)) {
        runtime_->lastError.clear();
        return true;
    }
    // 仅明确装配的普通客户端允许 custom/local 型号；空转实例不能把选择保存成已配置。
    // 账号侧只读所选供应商的缓存，不取另一个 Key，也不在这里发凭据或模型请求。
    if (!runtime_->modelClient ||
        (runtime_->accountClient && !runtime_->accountClient->isConfiguredForModel(toUtf8(selected)))) {
        runtime_->lastError = QStringLiteral("所选模型尚未配置可用凭据或客户端，当前会话配置未修改。");
        return false;
    }
    MaiResult<std::string> updated =
        runtime_->agent->submit(MaiUpdateSession{toUtf8(sessionId), "", toUtf8(selected), "", "", storedEffort});
    if (!updated) {
        runtime_->lastError = fromUtf8(updated.error().message());
        return false;
    }
    runtime_->lastError.clear();
    return true;
}

bool AgentController::setModel(const QString& sessionId, const QString& model) {
    return setSessionModelConfiguration(sessionId, model);
}

bool AgentController::setDefaultModel(const QString& model) {
    const QString selected = model.trimmed();
    if (selected.isEmpty()) {
        runtime_->lastError = QStringLiteral("模型名称不能为空。");
        return false;
    }
    if (!runtime_->supportsModel(selected)) {
        runtime_->lastError = QStringLiteral("当前客户端不支持此模型。");
        return false;
    }
    runtime_->newSessionDefaultModel = selected;
    runtime_->lastError.clear();
    return true;
}

bool AgentController::interrupt(const QString& sessionId) {
    return interrupt(sessionId, {});
}

bool AgentController::interrupt(const QString& sessionId, const QString& expectedTurn) {
    MaiResult<std::string> stopped = runtime_->agent->interruptTurn(toUtf8(sessionId), toUtf8(expectedTurn));
    if (!stopped) {
        runtime_->lastError = fromUtf8(stopped.error().message());
        return false;
    }
    if (runtime_->scheduleRunner) runtime_->scheduleRunner->finish(toUtf8(sessionId), "Interrupted by user");
    return true;
}

bool AgentController::setApprovalPolicy(MaiApprovalPolicy policy) {
    if (!runtime_->agent->setApprovalPolicy(policy)) {
        runtime_->lastError = QStringLiteral("请先停止正在执行的任务，再切换批准方式。");
        return false;
    }
    return true;
}

MaiApprovalPolicy AgentController::approvalPolicy() const {
    return runtime_->agent->approvalPolicy();
}

bool AgentController::approvePermission(const QString& permissionId, bool approveForSession) {
    const MaiPermissionDecision decision =
        approveForSession ? MaiPermissionDecision::ApprovedForSession : MaiPermissionDecision::Approved;
    MaiResult<std::string> replied = runtime_->agent->submit(MaiReplyPermission{toUtf8(permissionId), decision});
    if (!replied) {
        runtime_->lastError = fromUtf8(replied.error().message());
        return false;
    }
    return true;
}

bool AgentController::denyPermission(const QString& permissionId) {
    MaiResult<std::string> replied =
        runtime_->agent->submit(MaiReplyPermission{toUtf8(permissionId), MaiPermissionDecision::Denied});
    if (!replied) {
        runtime_->lastError = fromUtf8(replied.error().message());
        return false;
    }
    return true;
}

bool AgentController::answerQuestion(const QString& questionId, const QString& answer) {
    MaiResult<std::string> replied = runtime_->agent->submit(MaiReplyQuestion{toUtf8(questionId), toUtf8(answer)});
    if (!replied) {
        runtime_->lastError = fromUtf8(replied.error().message());
        return false;
    }
    return true;
}

std::vector<MaiQuestionRequest> AgentController::pendingQuestions() const {
    return runtime_->agent->listPendingQuestions();
}

std::vector<MaiSubAgentInfo> AgentController::subAgents(const QString& parentSessionId) const {
    return runtime_->agent->listSubAgents(toUtf8(parentSessionId));
}

bool AgentController::isChildOf(const QString& sessionId, const QString& parentSessionId) const {
    if (sessionId.isEmpty() || parentSessionId.isEmpty()) return false;
    MaiSession session;
    if (!runtime_->agent->getSession(toUtf8(sessionId), session)) return false;
    return fromUtf8(session.parentId) == parentSessionId;
}

QString AgentController::livePartText(const QString& sessionId, const QString& partId) const {
    const auto session = runtime_->livePartTextBySession.constFind(sessionId);
    if (session == runtime_->livePartTextBySession.constEnd()) return {};
    return session.value().value(partId);
}

// ---- 事件 ----

void AgentController::rawEvent(const MaiEvent& event) {
    // **这里跑在核心的线程上**（流式期间是网络读线程）。能做的只有一件事：把事件排队交给主线程。
    // emit 一个跨线程的排队连接就是加锁往队列塞个指针，
    // 符合 MaiEventBus 那条"处理函数里不许做慢活"的契约。
    //
    // 不要在这里查存储、碰部件、或者做任何分类——那些都在 onEventQueued 里做。
    emit eventQueued(event);
}

void AgentController::onEventQueued(const MaiEvent& event) {
    // 这里已经在主线程上，可以交付界面事件；查存储仍只读取事件所属消息，
    // 避免后台会话的工具更新把整段历史扫描带到前台交互路径。
    const QString sessionId = fromUtf8(event.sessionId);
    const QString messageId = fromUtf8(event.messageId);
    const QString partId = fromUtf8(event.partId);
    if (event.type == MaiEventType::SessionCreated || event.type == MaiEventType::SessionDeleted) {
        if (event.type == MaiEventType::SessionDeleted && runtime_->scheduleStore)
            runtime_->scheduleStore->cancelSession(event.sessionId);
        emit sessionStructureChanged();
        return;
    }

    switch (event.type) {
        case MaiEventType::MessageUpdated: {
            MaiMessage message;
            if (runtime_->agent->getMessage(event.sessionId, event.messageId, message)) {
                emit storedMessageUpdated(sessionId, messageId);
                if (message.role == MaiRole::User) emit userMessageAdded(sessionId, messageId);
            }
            return;
        }

        case MaiEventType::MessagePartUpdated: {
            // 片段第一次出现，或者工具卡状态变了。查一次存储看它是什么。
            //
            // 工具片段每次都要查（状态会变）；正文和思考只需要查第一次——它们的类型不会变，
            // 之后来的全是增量。
            auto& partKinds = runtime_->partKindsBySession[sessionId];
            const auto known = partKinds.constFind(partId);
            if (known != partKinds.constEnd() && known.value() != PartKind::Tool) return;

            MaiMessage message;
            if (!runtime_->agent->getMessage(event.sessionId, event.messageId, message)) return;
            PartKind kind = PartKind::Unknown;
            for (const MaiMessagePart& part : message.parts) {
                if (part.id != event.partId) continue;
                if (std::get_if<MaiTextPart>(&part.body) != nullptr) {
                    kind = PartKind::Text;
                } else if (std::get_if<MaiReasoningPart>(&part.body) != nullptr) {
                    kind = PartKind::Reasoning;
                } else {
                    kind = PartKind::Tool;
                }
                break;
            }
            if (kind == PartKind::Unknown) return;

            partKinds.insert(partId, kind);
            if (kind == PartKind::Tool) emit toolPartChanged(sessionId, messageId, partId);
            // 正文和思考片段这里不发信号：内容还是空的，界面没什么可画。等第一条增量到了再说。
            return;
        }

        case MaiEventType::MessagePartDelta: {
            // 两种增量的 field 都是 "text"，只能靠上面那张表区分。
            // 查不到就当正文——但那说明 message.part.updated 没到，属于上游的 bug，
            // 不是这里该悄悄兜住的事（MaiAgent 那边为此改过一次"先落库再广播"）。
            PartKind kind = PartKind::Text;
            const auto sessionKinds = runtime_->partKindsBySession.constFind(sessionId);
            if (sessionKinds != runtime_->partKindsBySession.constEnd())
                kind = sessionKinds.value().value(partId, PartKind::Text);
            const QString delta = fromUtf8(event.delta);
            runtime_->livePartTextBySession[sessionId][partId] += delta;
            if (kind == PartKind::Reasoning) {
                emit reasoningDelta(sessionId, messageId, partId, delta);
            } else {
                emit textDelta(sessionId, messageId, partId, delta);
            }
            return;
        }

        case MaiEventType::ContextImported:
            emit contextImported(sessionId);
            return;

        case MaiEventType::SessionIdle:
            // 一轮结束，这一轮的片段表可以丢了。不丢的话长会话会一直涨。
            runtime_->partKindsBySession.remove(sessionId);
            if (runtime_->scheduleRunner) runtime_->scheduleRunner->finish(event.sessionId);
            emit turnFinished(sessionId);
            runtime_->livePartTextBySession.remove(sessionId);
            return;

        case MaiEventType::SessionError:
            if (runtime_->scheduleRunner) runtime_->scheduleRunner->finish(event.sessionId, event.detail);
            emit turnFailed(sessionId, fromUtf8(event.detail));
            return;

        case MaiEventType::SessionUpdated:
            if (!event.detail.empty()) {
                emit sessionTitleChanged(sessionId, fromUtf8(event.detail));
            }
            return;

        case MaiEventType::PermissionAsked: emit permissionAsked(fromUtf8(event.permissionId), sessionId); return;

        case MaiEventType::PermissionReplied:
            emit permissionReplied(fromUtf8(event.permissionId), fromUtf8(event.detail));
            return;

        case MaiEventType::QuestionAsked:
            emit questionAsked(fromUtf8(event.questionId), sessionId, fromUtf8(event.partId));
            return;

        case MaiEventType::QuestionAnswered: emit questionAnswered(fromUtf8(event.questionId), sessionId); return;

        default:
            // SessionCreated / SessionDeleted / MessageUpdated / MessageRemoved /
            // MessagePartRemoved / SessionStatus：界面拿不到新信息。
            // 建会话和发消息的返回值里已经有 id 了，删除由发起那一侧自己知道。
            return;
    }
}
