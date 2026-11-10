// 文件作用：验证“把屏幕截图能力封装为 Agent 工具”的正确行为和边界条件。
// 验证入口：test_tool_contract、test_builtin_registration_matches_platform_support、
// test_capture_uses_only_screen_requests、test_stop_needs_no_capture_or_workspace、
// test_controlled_tool_uses_owned_native_window_source；普通 API 另测显式显示器的失败分派，
// 不把缺少 GUI/屏幕录制授权计为目标校验成功。
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#endif

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include <json.hpp>

#include "MaiAgentSendMediaTool.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiScreenshot.h"
#include "MaiScreenshotTool.h"
#include "MaiThread.h"
#include "MaiTool.h"

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

namespace {

using Json = nlohmann::json;

#if defined(_WIN32)
// 超时退出只针对本测试进程；即使消息分发或线程收尾发生回归，也不能让测试无限挂起。
class MaiScreenshotTestDeadline {
public:
    MaiScreenshotTestDeadline()
        : mThread([this] {
              MaiThread::setCurrentName("mai-cap-limit");
              std::unique_lock<std::mutex> lock(mMutex);
              if (!mCondition.wait_for(lock, std::chrono::seconds(90),
                                       [this] { return mFinished; })) {
                  std::fprintf(stderr,
                               "FAIL screenshot tool test exceeded its 90 second deadline\n");
                  std::fflush(stderr);
                  std::_Exit(1);
              }
          }) {}

    ~MaiScreenshotTestDeadline() {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mFinished = true;
        }
        mCondition.notify_one();
        mThread.join();
    }

private:
    std::mutex mMutex;
    std::condition_variable mCondition;
    bool mFinished = false;
    std::thread mThread;
};
#endif

template <typename Operation>
auto runCaptureCallWithOwnerMessages(Operation operation) -> decltype(operation()) {
#if defined(_WIN32)
    // 原生 API 要求在工作线程调用。窗口所有者继续泵消息，避免后台 EnumWindows 读取同进程
    // 窗口标题时，GetWindowTextLengthW 的同步消息与主线程等待采集互锁。
    std::optional<decltype(operation())> result;
    std::atomic<bool> finished{false};
    std::thread caller([&] {
        MaiThread::setCurrentName("mai-cap-test");
        result.emplace(operation());
        finished.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!finished.load(std::memory_order_acquire)) {
        MSG message{};
        for (int count = 0; count < 64 && ::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE);
             ++count) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            std::fprintf(stderr, "FAIL screenshot call exceeded its 20 second deadline\n");
            std::fflush(stderr);
            std::_Exit(1);
        }
        ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
    // 正常路径先 join，再读取结果和销毁 fixture；不 detach，回调不会越过其引用对象生命周期。
    caller.join();
    return std::move(*result);
#else
    return operation();
#endif
}

#if defined(_WIN32)
void stopNativeCaptureWithOwnerMessages() {
    const MaiError stopped =
        runCaptureCallWithOwnerMessages([] { return maiStopScreenshotCapture(); });
    CHECK(!stopped.hasError());
    const MaiScreenshotCaptureState state = maiScreenshotCaptureState();
    CHECK(!state.workerRunning);
    CHECK(!state.capturing);
}
#endif

// 每次运行使用独立的中文空格路径，同时覆盖 UTF-8 边界和截图交付的工作区隔离。
struct MaiScreenshotTestWorkspace {
    MaiFilePath root;

    MaiScreenshotTestWorkspace()
        : root(MaiFileSystem::temporaryDirectory()
                   .append(MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai-screenshot-test-")))
                   .append(MaiFilePath::fromUtf8(u8"\u622a\u56fe workspace"))) {
        CHECK(!MaiFileSystem::createDirectories(root).hasError());
    }

    ~MaiScreenshotTestWorkspace() {
        MaiFileSystem::removeRecursively(root.dirName());
    }
};

MaiScreenshotCapture rejectCapture(int& calls) {
    return [&calls](const MaiScreenshotRequest&, MaiScreenshot&) {
        ++calls;
        return MaiError{MaiErrorCode::Internal, "capture must not run in this validation case"};
    };
}

void test_tool_contract() {
    int captures = 0;
    std::unique_ptr<MaiTool> tool = makeMaiScreenshotTool(rejectCapture(captures));
    CHECK(tool != nullptr);
    CHECK(tool->name() == "screenshot");
    CHECK(tool->requiresApproval("{}"));
    CHECK(tool->requiresPerCallApproval("{}"));
    CHECK(tool->parametersSchema().find("additionalProperties") != std::string::npos);
    const Json schema = Json::parse(tool->parametersSchema(), nullptr, false);
    CHECK(schema.is_object());
    if (schema.is_object()) {
        CHECK(schema.value("additionalProperties", true) == false);
        CHECK(schema.value("properties", Json{}).is_object());
        if (schema.value("properties", Json{}).is_object()) {
            const Json& properties = schema["properties"];
            CHECK(properties.size() == 1);
            CHECK(properties.contains("action"));
            CHECK(!properties.contains("mode"));
            CHECK(!properties.contains("windowTitle"));
            CHECK(properties.value("action", Json{}).is_object());
            if (properties.value("action", Json{}).is_object()) {
                CHECK(properties["action"].value("type", std::string()) == "string");
                CHECK(properties["action"].value("enum", Json{}) ==
                      Json::array({"capture", "stop"}));
                CHECK(properties["action"].value("default", std::string()) == "capture");
            }
        }
    }
    CHECK(tool->description().find("mode=window") == std::string::npos);
    CHECK(tool->description().find("list_windows") == std::string::npos);
    CHECK(!tool->isAvailableForModel("glm-5.3"));
    CHECK(!tool->isAvailableForModel("local:qa"));
    CHECK(tool->isAvailableForModel("glm-5.3-flash"));
    CHECK(tool->isAvailableForModel("deepseek-flash"));

    MaiToolContext textOnly;
    textOnly.model = "glm-5.3";
    const MaiToolResult unsupported = tool->execute("{}", textOnly);
    CHECK(unsupported.hasError());
    CHECK(unsupported.error().code() == MaiErrorCode::NotSupported);
    CHECK(unsupported.error().message().find("glm-5.3-flash") != std::string::npos);

    // 受控回调拒绝任何采集，避免门禁回归触碰用户显示器。
    MaiScreenshotTestWorkspace workspace;
    MaiToolContext localOnly;
    localOnly.model = "local:qa";
    localOnly.root = workspace.root.toUtf8();
    const MaiToolResult localUnsupported = tool->execute("{}", localOnly);
    CHECK(localUnsupported.hasError());
    CHECK(localUnsupported.error().code() == MaiErrorCode::NotSupported);
    CHECK(localUnsupported.images().empty());

    MaiToolContext imageCapable;
    imageCapable.model = "glm-5.3-flash";
    const MaiToolResult invalid = tool->execute("not-json", imageCapable);
    CHECK(invalid.hasError());
    CHECK(invalid.error().code() == MaiErrorCode::InvalidInput);
    const MaiToolResult wrongActionType = tool->execute(R"({"action":7})", imageCapable);
    CHECK(wrongActionType.hasError());
    CHECK(wrongActionType.error().code() == MaiErrorCode::InvalidInput);
    const MaiToolResult extraArgument = tool->execute(R"({"unexpected":true})", imageCapable);
    CHECK(extraArgument.hasError());
    CHECK(extraArgument.error().code() == MaiErrorCode::InvalidInput);
    CHECK(captures == 0);
}

void test_legacy_window_arguments_are_rejected_before_capture() {
    MaiScreenshotTestWorkspace workspace;
    MaiToolContext context;
    context.model = "deepseek-flash";
    context.root = workspace.root.toUtf8();
    int captures = 0;
    auto tool = makeMaiScreenshotTool(rejectCapture(captures));
    for (const char* arguments :
         {R"({"mode":"display"})", R"({"mode":"window","windowTitle":"MaiAgent Legacy Window"})",
          R"({"windowTitle":"MaiAgent Legacy Window"})",
          R"({"action":"capture","window_id":"123"})", R"({"action":"stop","mode":"window"})",
          R"({"action":"window"})", R"({"action":"capture","displayId":123})",
          R"({"action":"capture","display_id":"123"})",
          R"({"action":"capture","target":"DisplayById"})",
          R"({"action":"capture","image_path":"screenshots/owned.png"})"}) {
        const MaiToolResult result = tool->execute(arguments, context);
        CHECK(result.hasError());
        CHECK(result.error().code() == MaiErrorCode::InvalidInput);
        CHECK(result.images().empty());
    }
    CHECK(captures == 0);
}

void test_stop_needs_no_capture_or_workspace() {
    int captures = 0;
    auto tool = makeMaiScreenshotTool(rejectCapture(captures));
    CHECK(!tool->requiresApproval(R"({"action":"stop"})"));
    CHECK(!tool->requiresPerCallApproval(R"({"action":"stop"})"));
    std::atomic<bool> canceled{true};
    MaiToolContext context;
    context.model = "glm-5.3";
    context.cancel = &canceled;
    // stop 是清理操作，取消、缺少工作区或模型不能接收像素都不能阻止释放原生会话。
    for (int repeat = 0; repeat < 2; ++repeat) {
        const MaiToolResult stopped = tool->execute(R"({"action":"stop"})", context);
        CHECK(!stopped.hasError());
        CHECK(stopped.images().empty());
        if (!stopped.hasError()) {
            const Json output = Json::parse(stopped.output(), nullptr, false);
            CHECK(output == Json({{"action", "stop"}, {"state", "stopped"}}));
        }
    }
    CHECK(captures == 0);
}

void test_builtin_registration_matches_platform_support() {
    MaiToolRegistry registry;
    registerMaiBuiltinTools(registry);
    CHECK((registry.find("screenshot") != nullptr) == maiIsScreenshotSupported());
    CHECK(registry.find("list_windows") == nullptr);
    for (const std::string model : {"glm-5.3", "local:qa", "glm-5.3-flash", "deepseek-flash"}) {
        const auto specs = registry.specs(model);
        const bool declared = std::any_of(specs.begin(), specs.end(), [](const MaiToolSpec& spec) {
            return spec.name == "screenshot";
        });
        CHECK(declared == (maiIsScreenshotSupported() &&
                           (model == "glm-5.3-flash" || model == "deepseek-flash")));
        CHECK((registry.findForModel("screenshot", model) != nullptr) == declared);
        CHECK(std::none_of(specs.begin(), specs.end(),
                           [](const MaiToolSpec& spec) { return spec.name == "list_windows"; }));
    }
}

void test_workspace_is_required_before_capture() {
    int captures = 0;
    auto tool = makeMaiScreenshotTool(rejectCapture(captures));
    MaiToolContext context;
    context.model = "glm-5.3-flash";
    const MaiToolResult result = tool->execute("{}", context);
    CHECK(result.hasError());
    CHECK(result.error().code() == MaiErrorCode::NotConfigured);
    CHECK(result.images().empty());
    CHECK(captures == 0);
}

void test_workspace_screenshot_directory_cannot_escape() {
    MaiScreenshotTestWorkspace workspace;
    MaiScreenshotTestWorkspace outside;
    const MaiFilePath link = workspace.root.append(MaiFilePath::fromUtf8("screenshots"));
#if defined(_WIN32)
    // Developer Mode 可允许普通用户创建目录链接；缺少系统权限时只跳过此独立边界用例。
    if (!::CreateSymbolicLinkW(link.value().c_str(), outside.root.value().c_str(),
                               SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2)) {
        std::printf("screenshot directory link test skipped: Win32 error %lu\n", ::GetLastError());
        return;
    }
#else
    if (::symlink(outside.root.value().c_str(), link.value().c_str()) != 0) {
        std::printf("screenshot directory link test skipped: symlink is unavailable\n");
        return;
    }
#endif
    CHECK(MaiFileSystem::isSymbolicLink(link));
    MaiToolContext context;
    context.model = "glm-5.3-flash";
    context.root = workspace.root.toUtf8();
    context.allowOutsideWorkingDirectory = true;
    int captures = 0;
    const MaiToolResult result =
        makeMaiScreenshotTool(rejectCapture(captures))->execute("{}", context);
    CHECK(result.hasError());
    CHECK(result.error().code() == MaiErrorCode::InvalidInput);
    CHECK(result.images().empty());
    CHECK(captures == 0);
    std::size_t outsideEntries = 0;
    MaiFileSystem::walk(outside.root, [&outsideEntries](const MaiFileEntry&) {
        ++outsideEntries;
        return MaiWalkAction::Continue;
    });
    CHECK(outsideEntries == 0);
    // 先移除链接本身，再让工作区析构清理，避免测试收尾依赖递归删除的链接语义。
#if defined(_WIN32)
    CHECK(::RemoveDirectoryW(link.value().c_str()) != 0);
#else
    CHECK(::unlink(link.value().c_str()) == 0);
#endif
}

void test_native_request_contract() {
    CHECK(static_cast<int>(MaiScreenshotTarget::DisplayUnderPointer) == 0);
    CHECK(static_cast<int>(MaiScreenshotTarget::WindowByTitle) == 1);
    CHECK(static_cast<int>(MaiScreenshotTarget::WindowById) == 2);
    CHECK(static_cast<int>(MaiScreenshotTarget::DisplayById) == 3);
    const MaiScreenshotRequest request;
    CHECK(request.target == MaiScreenshotTarget::DisplayUnderPointer);
    CHECK(request.windowTitle.empty());
    CHECK(request.windowId == 0);
    CHECK(request.displayId == 0);
}

void check_native_display_request_rejected(std::uint64_t displayId, MaiErrorCode expected) {
    MaiScreenshotRequest request;
    request.target = MaiScreenshotTarget::DisplayById;
    request.displayId = displayId;
    // 故意填旧结果，拒绝请求时不能把此前的帧及时间戳交给新目标。
    MaiScreenshot screenshot;
    screenshot.pngBytes = {0x7f};
    screenshot.width = 17;
    screenshot.height = 23;
    screenshot.frameSequence = 41;
    screenshot.frameReceivedAtMs = 1700000000123LL;
    const MaiScreenshotCaptureState before = maiScreenshotCaptureState();
    const MaiError error =
        runCaptureCallWithOwnerMessages([&] { return maiCaptureScreenshot(request, screenshot); });
    if (error.code() != expected) {
        std::printf("display ID %llu validation error: %s\n",
                    static_cast<unsigned long long>(displayId), error.message().c_str());
    }
    // GUI/权限前置条件不足不是目标分派通过；只能把精确错误计为本用例的通过。
    CHECK(error.code() == expected);
    CHECK(screenshot.pngBytes.empty());
    CHECK(screenshot.width == 0);
    CHECK(screenshot.height == 0);
    CHECK(screenshot.frameSequence == 0);
    CHECK(screenshot.frameReceivedAtMs == 0);
    const MaiScreenshotCaptureState after = maiScreenshotCaptureState();
    CHECK(after.deviceCreations == before.deviceCreations);
    CHECK(after.sessionStarts == before.sessionStarts);
}

void test_native_display_id_rejects_zero_and_invalid() {
    if (!maiIsScreenshotSupported()) return;
    check_native_display_request_rejected(0, MaiErrorCode::InvalidInput);
#if defined(_WIN32)
    const MaiErrorCode expected = sizeof(std::uintptr_t) < sizeof(std::uint64_t)
                                      ? MaiErrorCode::InvalidInput
                                      : MaiErrorCode::NotFound;
#else
    const MaiErrorCode expected = MaiErrorCode::InvalidInput;
#endif
    check_native_display_request_rejected(std::numeric_limits<std::uint64_t>::max(), expected);
}

void test_native_display_id_rejects_nonactive_target_without_fallback() {
    if (!maiIsScreenshotSupported()) return;
    std::vector<std::uint64_t> active;
#if defined(_WIN32)
    const BOOL enumerated = ::EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM context) -> BOOL {
            auto& displays = *reinterpret_cast<std::vector<std::uint64_t>*>(context);
            displays.push_back(reinterpret_cast<std::uintptr_t>(monitor));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&active));
    CHECK(enumerated != FALSE);
    if (!enumerated) return;
#elif defined(__APPLE__)
    std::uint32_t count = 0;
    const CGError counted = CGGetActiveDisplayList(0, nullptr, &count);
    CHECK(counted == kCGErrorSuccess);
    CHECK(count > 0 && count <= 64);
    if (counted != kCGErrorSuccess || count == 0 || count > 64) return;
    std::vector<CGDirectDisplayID> ids(count);
    const CGError enumerated = CGGetActiveDisplayList(count, ids.data(), &count);
    CHECK(enumerated == kCGErrorSuccess);
    if (enumerated != kCGErrorSuccess) return;
    active.assign(ids.begin(), ids.begin() + count);
#else
    CHECK(false);
    return;
#endif
    CHECK(!active.empty());
    if (active.empty()) return;
    // 枚举只读取原生标识，不读取任何显示器像素。从 N+1 个候选中选出不在 N 个活动标识
    // 内的非零值；不假定 1、主屏编号或 UINT32_MAX 永远不会是有效显示器。
    std::uint64_t missing = 1;
    while (std::find(active.begin(), active.end(), missing) != active.end()) ++missing;
    check_native_display_request_rejected(missing, MaiErrorCode::NotFound);
}

void test_native_window_id_rejects_zero() {
    if (!maiIsScreenshotSupported()) return;

    MaiScreenshotRequest request;
    // 固定目标值覆盖外部调用方传入的枚举，防止新增模式漏分派而回退为整屏。
    request.target = static_cast<MaiScreenshotTarget>(2);
    MaiScreenshot screenshot;
    screenshot.pngBytes = {0x7f};
    screenshot.width = 17;
    screenshot.height = 23;
    const MaiError error =
        runCaptureCallWithOwnerMessages([&] { return maiCaptureScreenshot(request, screenshot); });
    if (error.code() != MaiErrorCode::InvalidInput) {
        std::printf("zero window ID capture error: %s\n", error.message().c_str());
    }
    CHECK(error.code() == MaiErrorCode::InvalidInput);
    CHECK(screenshot.pngBytes.empty());
    CHECK(screenshot.width == 0);
    CHECK(screenshot.height == 0);
}

void test_native_window_id_rejects_invalid() {
    if (!maiIsScreenshotSupported()) return;

    MaiScreenshotRequest request;
    request.target = MaiScreenshotTarget::WindowById;
    request.windowId = std::numeric_limits<std::uint64_t>::max();
    MaiScreenshot screenshot;
    screenshot.pngBytes = {0x7f};
    screenshot.width = 17;
    screenshot.height = 23;
    const MaiError error =
        runCaptureCallWithOwnerMessages([&] { return maiCaptureScreenshot(request, screenshot); });
#if defined(_WIN32)
    const MaiErrorCode expected = sizeof(std::uintptr_t) < sizeof(std::uint64_t)
                                      ? MaiErrorCode::InvalidInput
                                      : MaiErrorCode::NotFound;
    CHECK(error.code() == expected);
#else
    CHECK(error.code() == MaiErrorCode::InvalidInput);
#endif
    CHECK(screenshot.pngBytes.empty());
    CHECK(screenshot.width == 0);
    CHECK(screenshot.height == 0);
}

#if defined(_WIN32)
void checkNativePng(const MaiScreenshot& screenshot) {
    const std::vector<std::uint8_t> signature = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    CHECK(screenshot.pngBytes.size() >= signature.size());
    if (screenshot.pngBytes.size() >= signature.size()) {
        CHECK(std::equal(signature.begin(), signature.end(), screenshot.pngBytes.begin()));
    }
    CHECK(screenshot.width > 0);
    CHECK(screenshot.height > 0);
}
#endif

void checkPngAndRemove(const MaiToolResult& captured, const MaiToolContext& context,
                       const std::string& expectedMode) {
    CHECK(!captured.hasError());
    CHECK(captured.images().size() == 1);
    if (captured.hasError() || captured.images().empty()) return;

    const MaiToolImage& image = captured.images().front();
    CHECK(image.mimeType == "image/png");
    const MaiFilePath path = MaiFilePath::fromUtf8(image.path);
    CHECK(path.isAbsolute());

    std::string bytes;
    const MaiError readError = MaiFileSystem::readFile(path, bytes, 24);
    CHECK(!readError.hasError());
    const std::string signature("\x89PNG\r\n\x1a\n", 8);
    CHECK(bytes.size() >= signature.size());
    CHECK(bytes.compare(0, signature.size(), signature) == 0);

    // 模型可读路径与像素附件必须指向同一文件，再通过真实媒体工具验证当前会话可交付。
    const Json output = Json::parse(captured.output(), nullptr, false);
    CHECK(output.is_object());
    if (output.is_object()) {
        CHECK(output.value("type", std::string()) == "image");
        CHECK(output.value("mime_type", std::string()) == "image/png");
        CHECK(output.value("mode", std::string()) == expectedMode);
        CHECK(output.value("width", 0) > 0);
        CHECK(output.value("height", 0) > 0);
        if (bytes.size() >= 24) {
            const auto pngDimension = [&bytes](std::size_t offset) {
                std::uint32_t value = 0;
                for (std::size_t index = offset; index < offset + 4; ++index)
                    value = (value << 8) | static_cast<unsigned char>(bytes[index]);
                return value;
            };
            CHECK(output.value("width", 0u) == pngDimension(16));
            CHECK(output.value("height", 0u) == pngDimension(20));
        }
        CHECK(output.value("image_path", Json{}).is_string());
        if (output.value("image_path", Json{}).is_string()) {
            const std::string relative = output["image_path"].get<std::string>();
            const MaiFilePath relativePath = MaiFilePath::fromUtf8(relative);
            CHECK(!relativePath.isAbsolute());
            CHECK(relative.compare(0, 12, "screenshots/") == 0);
            CHECK(relativePath.dirName().toGenericUtf8() == "screenshots");
            CHECK(relativePath.baseName().toUtf8().find(".png") != std::string::npos);
            const MaiFilePath root = MaiFileSystem::resolve(MaiFilePath::fromUtf8(context.root));
            CHECK(root.isParentOf(MaiFileSystem::resolve(path)));
            CHECK(MaiFileSystem::resolve(root.append(relativePath)) ==
                  MaiFileSystem::resolve(path));
            const MaiToolResult delivered = makeMaiAgentSendMediaTool()->execute(
                Json{{"file_path", relative}, {"type", "image"}}.dump(), context);
            CHECK(!delivered.hasError());
            if (!delivered.hasError()) {
                const Json receipt = Json::parse(delivered.output(), nullptr, false);
                CHECK(receipt.is_object());
                if (receipt.is_object()) {
                    CHECK(receipt.value("delivery", std::string()) == "current_ai_session");
                    CHECK(receipt.value("path", std::string()) == relative);
                    CHECK(receipt.value("mime_type", std::string()) == "image/png");
                }
            }
        }
    }
    CHECK(!MaiFileSystem::removeFile(path).hasError());
}

void test_capture_uses_only_screen_requests() {
    MaiScreenshotTestWorkspace workspace;
    MaiToolContext context;
    context.model = "deepseek-flash";
    context.root = workspace.root.toUtf8();
    int captures = 0;
    auto tool = makeMaiScreenshotTool(
        [&captures](const MaiScreenshotRequest& request, MaiScreenshot& screenshot) {
            ++captures;
            CHECK(request.target == MaiScreenshotTarget::DisplayUnderPointer);
            CHECK(request.windowTitle.empty());
            CHECK(request.windowId == 0);
            CHECK(request.displayId == 0);
            CHECK(request.isCanceled && !request.isCanceled());
            // 有效 1x1 PNG 是受控数据源，不启动原生 worker，也不读取显示器。
            const char png[] =
                "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d\x49\x48\x44\x52"
                "\x00\x00\x00\x01\x00\x00\x00\x01\x08\x06\x00\x00\x00\x1f\x15\xc4\x89"
                "\x00\x00\x00\x0d\x49\x44\x41\x54\x78\x9c\x63\x60\x68\xf8\xff\x1f"
                "\x00\x04\x82\x02\x7f\xe7\xd8\x5a\x6c\x00\x00\x00\x00\x49\x45\x4e"
                "\x44\xae\x42\x60\x82";
            screenshot.pngBytes.assign(png, png + sizeof(png) - 1);
            screenshot.width = 1;
            screenshot.height = 1;
            screenshot.frameSequence = 42;
            screenshot.frameReceivedAtMs = 1700000000123LL;
            return MaiError{};
        });
    int expectedCaptures = 0;
    for (const char* arguments : {"{}", R"({"action":"capture"})"}) {
        const MaiToolResult captured = tool->execute(arguments, context);
        CHECK(captures == ++expectedCaptures);
        CHECK(!captured.hasError());
        if (!captured.hasError()) {
            const Json output = Json::parse(captured.output(), nullptr, false);
            CHECK(output.is_object());
            if (output.is_object()) {
                CHECK(output.value("frame_sequence", std::uint64_t{}) == 42);
                CHECK(output.value("frame_received_at_ms", std::int64_t{}) == 1700000000123LL);
            }
        }
        checkPngAndRemove(captured, context, "display");
    }
    CHECK(captures == 2);
}

void test_controlled_tool_uses_owned_native_window_source() {
#if defined(_WIN32)
    constexpr wchar_t kTitle[] = L"MaiAgent Native Window Capture Test";
    const HWND window =
        ::CreateWindowExW(0, L"STATIC", kTitle, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 640,
                          360, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    CHECK(window != nullptr);
    if (window == nullptr) return;
    ::ShowWindow(window, SW_SHOW);
    ::UpdateWindow(window);

    int captures = 0;
    auto tool = makeMaiScreenshotTool(
        [&captures, window](const MaiScreenshotRequest& request, MaiScreenshot& screenshot) {
            ++captures;
            CHECK(request.target == MaiScreenshotTarget::DisplayUnderPointer);
            CHECK(request.windowTitle.empty());
            CHECK(request.windowId == 0);
            // 模型工具只请求屏幕；测试宿主把采集源收窄到自有窗口，不能据此声称已验真实显示器。
            MaiScreenshotRequest owned = request;
            owned.target = MaiScreenshotTarget::WindowById;
            owned.windowId = reinterpret_cast<std::uintptr_t>(window);
            return maiCaptureScreenshot(owned, screenshot);
        });
    MaiScreenshotTestWorkspace workspace;
    MaiToolContext context;
    context.model = "deepseek-flash";
    context.root = workspace.root.toUtf8();
    const MaiToolResult captured =
        runCaptureCallWithOwnerMessages([&] { return tool->execute("{}", context); });
    if (captured.hasError())
        std::printf("window capture error: %s\n", captured.error().message().c_str());
    checkPngAndRemove(captured, context, "display");
    CHECK(captures == 1);
    stopNativeCaptureWithOwnerMessages();
    ::DestroyWindow(window);
#endif
}

void test_native_window_id_capture() {
#if defined(_WIN32)
    // WGC 返回物理像素；fixture 也按物理坐标创建，避免系统缩放改变同名窗口的尺寸证据。
    const DPI_AWARENESS_CONTEXT previousContext =
        ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CHECK(previousContext != nullptr);
    if (previousContext == nullptr) return;
    struct MaiScreenshotTestDpiRestore {
        DPI_AWARENESS_CONTEXT previousContext;
        ~MaiScreenshotTestDpiRestore() {
            CHECK(::SetThreadDpiAwarenessContext(previousContext) != nullptr);
        }
    } dpiRestore{previousContext};

    constexpr wchar_t kTitle[] = L"MaiAgent Same Title Window ID Capture Test";
    const HWND largeWindow =
        ::CreateWindowExW(0, L"STATIC", kTitle, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 640,
                          360, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    const HWND smallWindow =
        ::CreateWindowExW(0, L"STATIC", kTitle, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 300, 200, 320,
                          240, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    CHECK(largeWindow != nullptr);
    CHECK(smallWindow != nullptr);
    if (largeWindow == nullptr || smallWindow == nullptr) {
        if (smallWindow) ::DestroyWindow(smallWindow);
        if (largeWindow) ::DestroyWindow(largeWindow);
        return;
    }
    ::UpdateWindow(largeWindow);
    ::UpdateWindow(smallWindow);

    MaiScreenshotRequest request;
    request.target = MaiScreenshotTarget::WindowByTitle;
    request.windowTitle = "MaiAgent Same Title Window ID Capture Test";
    MaiScreenshot screenshot;
    CHECK(runCaptureCallWithOwnerMessages([&] {
              return maiCaptureScreenshot(request, screenshot);
          }).code() == MaiErrorCode::InvalidInput);

    request.target = MaiScreenshotTarget::WindowById;
    // 保留会歧义的标题，证明 ID 模式只按句柄选择两者中尺寸更小的窗口。
    request.windowId = reinterpret_cast<std::uintptr_t>(smallWindow);
    const MaiError captured =
        runCaptureCallWithOwnerMessages([&] { return maiCaptureScreenshot(request, screenshot); });
    if (captured.hasError()) {
        std::printf("window ID capture error: %s\n", captured.message().c_str());
    }
    CHECK(!captured.hasError());
    checkNativePng(screenshot);
    CHECK(screenshot.width < 500);
    CHECK(screenshot.height < 300);

    const HWND child =
        ::CreateWindowExW(0, L"STATIC", L"MaiAgent Child Window", WS_CHILD | WS_VISIBLE, 0, 0, 100,
                          50, largeWindow, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    CHECK(child != nullptr);
    if (child) {
        request.windowId = reinterpret_cast<std::uintptr_t>(child);
        CHECK(runCaptureCallWithOwnerMessages([&] {
                  return maiCaptureScreenshot(request, screenshot);
              }).code() == MaiErrorCode::NotFound);
        CHECK(screenshot.pngBytes.empty());
        ::DestroyWindow(child);
    }

    // 同一个 ID 失去可见性或被销毁时，必须清空旧帧并失败，不能借同名的大窗口兜底。
    request.windowId = reinterpret_cast<std::uintptr_t>(smallWindow);
    ::ShowWindow(smallWindow, SW_HIDE);
    CHECK(runCaptureCallWithOwnerMessages([&] {
              return maiCaptureScreenshot(request, screenshot);
          }).code() == MaiErrorCode::NotFound);
    CHECK(screenshot.pngBytes.empty());
    CHECK(screenshot.width == 0);
    CHECK(screenshot.height == 0);
    ::ShowWindow(smallWindow, SW_SHOW);
    ::ShowWindow(smallWindow, SW_MINIMIZE);
    CHECK(::IsIconic(smallWindow));
    CHECK(runCaptureCallWithOwnerMessages([&] {
              return maiCaptureScreenshot(request, screenshot);
          }).code() == MaiErrorCode::NotFound);
    CHECK(screenshot.pngBytes.empty());
    ::DestroyWindow(smallWindow);
    CHECK(runCaptureCallWithOwnerMessages([&] {
              return maiCaptureScreenshot(request, screenshot);
          }).code() == MaiErrorCode::NotFound);
    CHECK(screenshot.pngBytes.empty());
    stopNativeCaptureWithOwnerMessages();
    ::DestroyWindow(largeWindow);
#endif
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    MaiScreenshotTestDeadline deadline;
#endif
    const bool validationOnly = argc == 2 && std::string(argv[1]) == "--request-validation-only";
    const bool ownedWindowOnly = argc == 2 && std::string(argv[1]) == "--owned-window-only";
    if (argc > 1 && !validationOnly && !ownedWindowOnly) {
        std::printf(
            "usage: MaiScreenshotToolTests [--request-validation-only|--owned-window-only]\n");
        return 2;
    }
    test_native_request_contract();
    test_native_window_id_rejects_zero();
    test_native_window_id_rejects_invalid();
    test_native_display_id_rejects_zero_and_invalid();
    test_native_display_id_rejects_nonactive_target_without_fallback();
    test_tool_contract();
    test_legacy_window_arguments_are_rejected_before_capture();
    test_stop_needs_no_capture_or_workspace();
    test_builtin_registration_matches_platform_support();
    test_workspace_is_required_before_capture();
    test_workspace_screenshot_directory_cannot_escape();
    test_capture_uses_only_screen_requests();
    if (!validationOnly) {
        // 全部入口均只使用本进程创建的 fixture；受控来源不等于实际显示器采集验收。
        test_controlled_tool_uses_owned_native_window_source();
        test_native_window_id_capture();
    }
#if defined(_WIN32)
    stopNativeCaptureWithOwnerMessages();
#endif
    if (failures == 0) std::printf("screenshot tool tests passed\n");
    return failures == 0 ? 0 : 1;
}
