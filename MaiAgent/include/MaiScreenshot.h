// 文件作用：声明屏幕截图能力和返回数据契约。
// 对外类型：MaiScreenshot、MaiScreenshotTarget、MaiScreenshotRequest；线程、失败和生命周期以类型附近注释为准。
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "MaiError.h"

// 平台原生屏幕捕获的结果。像素在平台实现内编码成完整 PNG 文件，调用方不接触 D3D、DXGI、
// ScreenCaptureKit 或界面框架。成功时 pngBytes 非空、width/height 是编码后图片的像素尺寸；失败时三者清空。
struct MaiScreenshot {
    // 完整 PNG 文件字节，不是裸像素。
    std::vector<std::uint8_t> pngBytes;
    // PNG 的最终像素尺寸；失败时都是 0。
    int width = 0;
    int height = 0;
    // 最近完整原生帧的进程内序号和接收时间（Unix 毫秒）。静止画面可沿用同一帧，
    // 不把本次 PNG 编码时间冒充帧更新时间；无元数据的平台保持 0。
    std::uint64_t frameSequence = 0;
    std::int64_t frameReceivedAtMs = 0;
};

enum class MaiScreenshotTarget {
    DisplayUnderPointer,  // 鼠标指针所在的显示器。
    WindowByTitle,        // 一个位于当前交互桌面的可见顶层窗口。
    WindowById,           // 按平台原生标识选择可见、未最小化的顶层窗口。
    DisplayById,          // 按原生显示器标识选择整屏，不依赖鼠标当前位置。
};

struct MaiScreenshotRequest {
    MaiScreenshotTarget target = MaiScreenshotTarget::DisplayUnderPointer;
    // WindowByTitle 时必填，UTF-8。平台实现先找标题完全相同的可见顶层窗口；没有完全匹配时，
    // 仅在子串匹配唯一时使用它，避免模型含糊指定后截错窗口。
    std::string windowTitle;
    // WindowById 时必填且非 0；Windows 为经 uintptr_t 转换的 HWND，macOS 为 CGWindowID。
    // 此模式忽略 windowTitle；标识无效、目标不可见或最小化时返回错误，不回退到标题或显示器。
    // 原生标识可能被系统回收重用，调用方须在截图前后核对窗口所属进程和边界。
    std::uint64_t windowId = 0;
    // DisplayById 时必填且非 0；无效目标失败，不回退到主屏或鼠标所在屏。
    std::uint64_t displayId = 0;
    // 非零时要求返回严格晚于指定帧/接收时间的图像；有界等待期间仍轮询取消。
    // 零保持现有截图复用最近完整帧的行为，不为等待新图重建常驻采集会话。
    std::uint64_t afterFrameSequence = 0;
    std::int64_t afterFrameReceivedAtMs = 0;
    // 可选线程安全取消检查；后端仅在本次阻塞调用返回前使用，不能保留或在返回后调用。
    std::function<bool()> isCanceled;
};

// 采集后端的普通诊断快照。计数按进程累计，stop 不清零；不把它当作图像内容或授权回执。
struct MaiScreenshotCaptureState {
    bool workerRunning = false;
    bool capturing = false;
    std::uint64_t workerStarts = 0;
    // 仅计可直接观测的图形设备创建；没有此类公开对象的平台保持 0。
    std::uint64_t deviceCreations = 0;
    std::uint64_t sessionStarts = 0;
    std::uint64_t framesReceived = 0;
};

// 当前平台是否提供原生屏幕捕获。调用方只有在返回 true 时才应把 screenshot 工具暴露给模型；
// 暴露一个永远失败的工具会诱使模型反复重试。
bool maiIsScreenshotSupported();

// 按 request 截取鼠标所在显示器或指定窗口，并编码为 PNG。
//
// 这是阻塞调用，只能在 agent 的工作线程调用。Windows 使用 Windows Graphics Capture，
// macOS 14+ 使用 ScreenCaptureKit；不依赖 Qt 或外部截屏进程。macOS 需要用户授予屏幕录制权限，
// 授权后可能需要重启 App。失败时 screenshot 会保持为空，并返回可供模型纠正的错误。
MaiError maiCaptureScreenshot(const MaiScreenshotRequest& request, MaiScreenshot& screenshot);

// 读取线程安全状态，不启动采集。停止会释放原生会话并收回工作线程；重复停止无副作用。
// 与停止并发的截图应结束等待并失败，后续新的调用可以重新启动后端。
MaiScreenshotCaptureState maiScreenshotCaptureState();
MaiError maiStopScreenshotCapture();

// 枚举当前交互桌面上可按标题捕获的可见顶层窗口。成功但没有窗口时返回空数组；不支持时返回
// NotSupported。标题统一转换为 UTF-8，顺序不保证稳定，调用方不能把位置当作窗口标识。
MaiError maiListCaptureWindows(std::vector<std::string>& titles);
