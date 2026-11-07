// 文件作用：把屏幕截图能力封装为 Agent 工具。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

#include <memory>
#include <functional>

#include "MaiTool.h"
#include "MaiScreenshot.h"

// 创建屏幕截图工具。capture 需要用户审批，stop 只释放采集资源、不需要审批；
// 不支持原生捕获的平台不会在内置工具表中注册它。模型入口不提供窗口采集参数。
std::unique_ptr<MaiTool> makeMaiScreenshotTool();

// 宿主可提供受控采集源，复用同一屏幕工具与图像交付逻辑；留空时调用原生采集后端。
using MaiScreenshotCapture = std::function<MaiError(const MaiScreenshotRequest&, MaiScreenshot&)>;
std::unique_ptr<MaiTool> makeMaiScreenshotTool(MaiScreenshotCapture capture);
