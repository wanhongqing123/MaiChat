// 文件作用：验证“把屏幕截图能力封装为 Agent 工具”的正确行为和边界条件。
// 验证入口：test_tool_contract、test_builtin_registration_matches_platform_support、
// test_workspace_is_required_before_capture、test_native_window_capture_returns_a_png、
// test_native_window_id_capture；检查可观察结果而非实现细节。
#include <algorithm>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include <CoreGraphics/CoreGraphics.h>
#endif
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

std::string missingWindowRequest() {
    return Json{{"mode", "window"},
                {"windowTitle", MaiIdGenerator::generate("MaiAgent Missing Window ")}}
        .dump();
}

void test_tool_contract() {
    std::unique_ptr<MaiTool> tool = makeMaiScreenshotTool();
    CHECK(tool != nullptr);
    CHECK(tool->name() == "screenshot");
    CHECK(tool->requiresApproval("{}"));
    CHECK(tool->requiresPerCallApproval("{}"));
    CHECK(tool->parametersSchema().find("additionalProperties") != std::string::npos);
    CHECK(tool->parametersSchema().find("windowTitle") != std::string::npos);
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

    // 指定必定不存在的窗口，旧实现即使遗漏本地模型门禁也不会捕获任何用户画面。
    MaiScreenshotTestWorkspace workspace;
    MaiToolContext localOnly;
    localOnly.model = "local:qa";
    localOnly.root = workspace.root.toUtf8();
    const MaiToolResult localUnsupported = tool->execute(missingWindowRequest(), localOnly);
    CHECK(localUnsupported.hasError());
    CHECK(localUnsupported.error().code() == MaiErrorCode::NotSupported);
    CHECK(localUnsupported.images().empty());

    MaiToolContext imageCapable;
    imageCapable.model = "glm-5.3-flash";
    const MaiToolResult invalid = tool->execute("not-json", imageCapable);
    CHECK(invalid.hasError());
    CHECK(invalid.error().code() == MaiErrorCode::InvalidInput);
    const MaiToolResult wrongModeType = tool->execute(R"({"mode":7})", imageCapable);
    CHECK(wrongModeType.hasError());
    CHECK(wrongModeType.error().code() == MaiErrorCode::InvalidInput);
    const MaiToolResult extraArgument = tool->execute(R"({"unexpected":true})", imageCapable);
    CHECK(extraArgument.hasError());
    CHECK(extraArgument.error().code() == MaiErrorCode::InvalidInput);
}

void test_builtin_registration_matches_platform_support() {
    MaiToolRegistry registry;
    registerMaiBuiltinTools(registry);
    CHECK((registry.find("screenshot") != nullptr) == maiIsScreenshotSupported());
    for (const std::string model : {"glm-5.3", "local:qa", "glm-5.3-flash", "deepseek-flash"}) {
        const auto specs = registry.specs(model);
        const bool declared = std::any_of(specs.begin(), specs.end(), [](const MaiToolSpec& spec) {
            return spec.name == "screenshot";
        });
        CHECK(declared == (maiIsScreenshotSupported() &&
                           (model == "glm-5.3-flash" || model == "deepseek-flash")));
        CHECK((registry.findForModel("screenshot", model) != nullptr) == declared);
    }
}

void test_workspace_is_required_before_capture() {
    auto tool = makeMaiScreenshotTool();
    MaiToolContext context;
    context.model = "glm-5.3-flash";
    const MaiToolResult result = tool->execute(missingWindowRequest(), context);
    CHECK(result.hasError());
    CHECK(result.error().code() == MaiErrorCode::NotConfigured);
    CHECK(result.images().empty());
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
    const MaiToolResult result = makeMaiScreenshotTool()->execute(missingWindowRequest(), context);
    CHECK(result.hasError());
    CHECK(result.error().code() == MaiErrorCode::InvalidInput);
    CHECK(result.images().empty());
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
    const MaiScreenshotRequest request;
    CHECK(request.target == MaiScreenshotTarget::DisplayUnderPointer);
    CHECK(request.windowTitle.empty());
    CHECK(request.windowId == 0);
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
    const MaiError error = maiCaptureScreenshot(request, screenshot);
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
    const MaiError error = maiCaptureScreenshot(request, screenshot);
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

void test_native_capture_returns_a_png() {
    if (!maiIsScreenshotSupported()) return;
#if defined(__APPLE__) && TARGET_OS_OSX
    if (!CGPreflightScreenCaptureAccess()) {
        std::vector<std::string> titles;
        const MaiError permission = maiListCaptureWindows(titles);
        CHECK(permission.code() == MaiErrorCode::NotConfigured);
        std::printf("live macOS capture skipped: Screen Recording permission is not granted\n");
        return;
    }
#endif

    std::unique_ptr<MaiTool> tool = makeMaiScreenshotTool();
    MaiScreenshotTestWorkspace workspace;
    MaiToolContext context;
    context.model = "glm-5.3-flash";
    context.root = workspace.root.toUtf8();
    const MaiToolResult captured = tool->execute("{}", context);
    if (captured.hasError()) std::printf("capture error: %s\n", captured.error().message().c_str());
    checkPngAndRemove(captured, context, "display");
}

void test_native_window_capture_returns_a_png() {
#if defined(_WIN32)
    constexpr wchar_t kTitle[] = L"MaiAgent Native Window Capture Test";
    const HWND window =
        ::CreateWindowExW(0, L"STATIC", kTitle, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 640,
                          360, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    CHECK(window != nullptr);
    if (window == nullptr) return;
    ::ShowWindow(window, SW_SHOW);
    ::UpdateWindow(window);

    std::unique_ptr<MaiTool> tool = makeMaiScreenshotTool();
    MaiScreenshotTestWorkspace workspace;
    MaiToolContext context;
    context.model = "deepseek-flash";
    context.root = workspace.root.toUtf8();
    const MaiToolResult captured = tool->execute(
        R"({"mode":"window","windowTitle":"MaiAgent Native Window Capture Test"})", context);
    if (captured.hasError())
        std::printf("window capture error: %s\n", captured.error().message().c_str());
    checkPngAndRemove(captured, context, "window");
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
    CHECK(maiCaptureScreenshot(request, screenshot).code() == MaiErrorCode::InvalidInput);

    request.target = MaiScreenshotTarget::WindowById;
    // 保留会歧义的标题，证明 ID 模式只按句柄选择两者中尺寸更小的窗口。
    request.windowId = reinterpret_cast<std::uintptr_t>(smallWindow);
    const MaiError captured = maiCaptureScreenshot(request, screenshot);
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
        CHECK(maiCaptureScreenshot(request, screenshot).code() == MaiErrorCode::NotFound);
        CHECK(screenshot.pngBytes.empty());
        ::DestroyWindow(child);
    }

    // 同一个 ID 失去可见性或被销毁时，必须清空旧帧并失败，不能借同名的大窗口兜底。
    request.windowId = reinterpret_cast<std::uintptr_t>(smallWindow);
    ::ShowWindow(smallWindow, SW_HIDE);
    CHECK(maiCaptureScreenshot(request, screenshot).code() == MaiErrorCode::NotFound);
    CHECK(screenshot.pngBytes.empty());
    CHECK(screenshot.width == 0);
    CHECK(screenshot.height == 0);
    ::ShowWindow(smallWindow, SW_SHOW);
    ::ShowWindow(smallWindow, SW_MINIMIZE);
    CHECK(::IsIconic(smallWindow));
    CHECK(maiCaptureScreenshot(request, screenshot).code() == MaiErrorCode::NotFound);
    CHECK(screenshot.pngBytes.empty());
    ::DestroyWindow(smallWindow);
    CHECK(maiCaptureScreenshot(request, screenshot).code() == MaiErrorCode::NotFound);
    CHECK(screenshot.pngBytes.empty());
    ::DestroyWindow(largeWindow);
#endif
}

}  // namespace

int main(int argc, char** argv) {
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
    test_tool_contract();
    test_builtin_registration_matches_platform_support();
    test_workspace_is_required_before_capture();
    test_workspace_screenshot_directory_cannot_escape();
    if (!validationOnly) {
        // --owned-window-only 只使用下面由本进程创建的 fixture，不进入显示器捕获。
        if (!ownedWindowOnly) test_native_capture_returns_a_png();
        test_native_window_capture_returns_a_png();
        test_native_window_id_capture();
    }
    if (failures == 0) std::printf("screenshot tool tests passed\n");
    return failures == 0 ? 0 : 1;
}
