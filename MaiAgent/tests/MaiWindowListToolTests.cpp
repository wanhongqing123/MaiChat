// 文件作用：验证“列举可见窗口及其基础信息”的正确行为和边界条件。
// 验证入口：test_contract_and_registration、test_lists_a_visible_native_window；
// 普通工厂保留兼容，但内置模型工具表不再提供窗口枚举。
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "MaiScreenshot.h"
#include "MaiTool.h"
#include "MaiWindowListTool.h"

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

namespace {

void test_contract_and_registration() {
    std::unique_ptr<MaiTool> tool = makeMaiWindowListTool();
    CHECK(tool->name() == "list_windows");
    CHECK(tool->requiresApproval("{}"));
    CHECK(tool->execute("not-json", MaiToolContext{}).hasError());

    MaiToolRegistry registry;
    registerMaiBuiltinTools(registry);
    CHECK(registry.find("list_windows") == nullptr);
    for (const std::string model : {"deepseek-flash", "glm-5.3-flash", "glm-5.3", "local:qa"}) {
        CHECK(registry.findForModel("list_windows", model) == nullptr);
        const auto specs = registry.specs(model);
        CHECK(std::none_of(specs.begin(), specs.end(),
                           [](const MaiToolSpec& spec) { return spec.name == "list_windows"; }));
    }
}

void test_lists_a_visible_native_window() {
#if defined(_WIN32)
    constexpr wchar_t kTitle[] = L"MaiAgent Window Listing Test";
    const HWND window =
        ::CreateWindowExW(0, L"STATIC", kTitle, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 480,
                          240, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    CHECK(window != nullptr);
    if (window == nullptr) return;
    ::ShowWindow(window, SW_SHOW);
    ::UpdateWindow(window);

    std::unique_ptr<MaiTool> tool = makeMaiWindowListTool();
    const MaiToolResult result = tool->execute("{}", MaiToolContext{});
    CHECK(!result.hasError());
    CHECK(result.output().find("MaiAgent Window Listing Test") != std::string::npos);
    ::DestroyWindow(window);
#endif
}

}  // namespace

int main(int argc, char** argv) {
    const bool validationOnly = argc == 2 && std::string(argv[1]) == "--request-validation-only";
    if (argc > 1 && !validationOnly) {
        std::printf("usage: MaiWindowListToolTests [--request-validation-only]\n");
        return 2;
    }
    test_contract_and_registration();
    if (!validationOnly) test_lists_a_visible_native_window();
    if (failures == 0) std::printf("window list tool tests passed\n");
    return failures == 0 ? 0 : 1;
}
