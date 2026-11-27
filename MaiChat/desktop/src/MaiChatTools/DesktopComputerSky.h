#pragma once

#include <cstdint>
#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "MaiChatTools/DesktopComputerPlatform.h"

// 模型和普通调用共享同一窗口身份；id 必须来自枚举，不能解释为进程启动参数。
struct DesktopComputerWindow {
    std::string app;
    std::uint64_t id = 0;
    std::optional<std::string> title;
};

struct DesktopComputerWindowScreenshot {
    std::string id;
    std::string imagePath;
    double width = 0;
    double height = 0;
    double originX = 0;
    double originY = 0;
    int zIndex = 0;
    // 普通库调用保留 data URL；模型适配器使用图像附件，不重复打印 Base64。
    std::string url;
    // 编码像素与窗口逻辑尺寸分开，避免高 DPI 时把图像像素直接用作指针坐标。
    int imagePixelWidth = 0;
    int imagePixelHeight = 0;
};

struct DesktopComputerWindowState {
    DesktopComputerWindow window;
    std::optional<DesktopNativeAccessibilitySnapshot> accessibility;
    // 索引顺序与本状态中发布的树相同；失效后不能解析为新控件。
    std::vector<DesktopNativeElement> indexedElements;
    // 与索引一一对应的窗口相对逻辑 x/y/width/height；无法确认变换时不提供。
    std::vector<std::array<double, 4>> elementBounds;
    std::vector<DesktopComputerWindowScreenshot> screenshots;
    bool imageCoordinatesUsable = false;
    // MaiChat 高级输入扩展复用这个唯一观察绑定，不另建窗口状态。
    std::string nativeSnapshotId;
};

struct DesktopComputerSkyAction {
    std::string method;
    DesktopComputerWindow window;
    std::optional<std::size_t> elementIndex;
    std::string screenshotId;
    std::optional<double> x;
    std::optional<double> y;
    double toX = 0;
    double toY = 0;
    double scrollX = 0;
    double scrollY = 0;
    int clickCount = 1;
    DesktopMouseButton button = DesktopMouseButton::Left;
    std::string text;
    std::string key;
    std::string secondaryAction;
    std::string turnId;
    std::function<bool()> isCanceled;
};
