// 文件作用：验证“读取工作区图片并返回可供模型查看的内容”的正确行为和边界条件。
// 验证入口：test_png_is_returned_as_a_model_image、test_rejects_invalid_images_and_escaped_paths、
// test_desktop_can_view_image_outside_session_directory；检查可观察结果而非实现细节。
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiTool.h"
#include "MaiViewImageTool.h"

namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

namespace {

struct Workspace {
    fs::path root = fs::temp_directory_path() / ("mai-view-image-" + std::to_string(std::rand()));
    Workspace() {
        fs::create_directories(root);
    }
    ~Workspace() {
        std::error_code error;
        fs::remove_all(root, error);
    }
};

void test_png_is_returned_as_a_model_image() {
    Workspace workspace;
    const fs::path path = workspace.root / "diagram.bin";
    std::ofstream(path, std::ios::binary).write("\x89PNG\r\n\x1a\n", 8);

    std::unique_ptr<MaiTool> tool = makeMaiViewImageTool();
    MaiToolContext context;
    context.root = workspace.root.u8string();
    context.model = "glm-5.3-flash";
    const MaiToolResult result = tool->execute(R"({"path":"diagram.bin"})", context);

    CHECK(!result.hasError());
    CHECK(result.images().size() == 1);
    if (!result.images().empty()) {
        CHECK(result.images().front().path == fs::canonical(path).u8string());
        CHECK(result.images().front().mimeType == "image/png");
    }
}

void test_rejects_invalid_images_and_escaped_paths() {
    Workspace workspace;
    std::ofstream(workspace.root / "text.txt", std::ios::binary) << "not an image";
    std::unique_ptr<MaiTool> tool = makeMaiViewImageTool();
    MaiToolContext context;
    context.root = workspace.root.u8string();
    context.model = "glm-5.3-flash";

    CHECK(tool->execute(R"({"path":"text.txt"})", context).hasError());
    const MaiToolResult outside = tool->execute(R"({"path":"../outside.png"})", context);
    CHECK(outside.hasError());
    CHECK(outside.error().message().find(context.root) != std::string::npos);
    CHECK(outside.error().message().find("user-attached image") != std::string::npos);
    CHECK(tool->execute(R"({"path":7})", context).hasError());
    CHECK(tool->execute("not-json", context).hasError());
}

void test_desktop_can_view_image_outside_session_directory() {
    Workspace session;
    Workspace other;
    const fs::path image = other.root / "outside.png";
    std::ofstream(image, std::ios::binary).write("\x89PNG\r\n\x1a\n", 8);
    MaiToolContext context;
    context.root = session.root.u8string();
    context.model = "glm-5.3-flash";
    context.allowOutsideWorkingDirectory = true;
    const std::string arguments = "{\"path\":\"" + image.generic_u8string() + "\"}";
    const MaiToolResult result = makeMaiViewImageTool()->execute(arguments, context);
    CHECK(!result.hasError());
    CHECK(result.images().size() == 1);
}

void test_rejects_text_only_models() {
    std::unique_ptr<MaiTool> tool = makeMaiViewImageTool();
    MaiToolContext context;
    context.model = "glm-5.3";
    const MaiToolResult result = tool->execute(R"({"path":"image.png"})", context);
    CHECK(result.hasError());
    CHECK(result.error().code() == MaiErrorCode::NotSupported);
    CHECK(result.error().message().find("glm-5.3-flash") != std::string::npos);
}

void test_local_primary_rejects_pixels_before_preview_work() {
    Workspace workspace;
    const fs::path image = workspace.root / "photo.png";
    CHECK(!MaiFileSystem::writeFile(MaiFilePath::fromUtf8(image.u8string()),
                                    std::string("\x89PNG\r\n\x1a\n", 8))
               .hasError());
    int previewCalls = 0;
    auto tool = makeMaiViewImageTool([&](const std::string& path, const MaiToolContext&) {
        ++previewCalls;
        return MaiResult<std::string>(path);
    });
    for (const char* model : {"local:qa", "local:qwen3.5-2b-instruct-q4_k_m"}) {
        MaiToolContext context;
        context.root = workspace.root.u8string();
        context.model = model;
        const auto result = tool->execute(R"({"path":"photo.png"})", context);
        CHECK(result.hasError() && result.error().code() == MaiErrorCode::NotSupported);
        CHECK(result.images().empty());
        CHECK(result.error().message().find("text only") != std::string::npos);
        CHECK(result.error().message().find("mobile_preview_image") != std::string::npos);
    }
    CHECK(previewCalls == 0);
}

void test_registry_filters_the_same_model_capability_as_direct_execution() {
    MaiToolRegistry registry;
    registry.add(makeMaiViewImageTool());
    CHECK(registry.find("view_image") != nullptr);
    for (const char* model : {"local:qa", "local:qwen3.5-2b-instruct-q4_k_m", "glm-5.3"}) {
        CHECK(registry.findForModel("view_image", model) == nullptr);
        CHECK(registry.specs(model).empty());
    }
    for (const char* model : {"glm-5.3-flash", ""}) {
        CHECK(registry.findForModel("view_image", model) != nullptr);
        CHECK(registry.specs(model).size() == 1);
    }
}

}  // namespace

int main() {
    test_png_is_returned_as_a_model_image();
    test_rejects_invalid_images_and_escaped_paths();
    test_desktop_can_view_image_outside_session_directory();
    test_rejects_text_only_models();
    test_local_primary_rejects_pixels_before_preview_work();
    test_registry_filters_the_same_model_capability_as_direct_execution();
    if (failures == 0) std::printf("view image tool tests passed\n");
    return failures == 0 ? 0 : 1;
}
