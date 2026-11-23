// 文件作用：保留屏幕捕获可用性的逐项观察，不把运行期拒绝混同为权限或配置缺失。
#pragma once

#include "MaiError.h"

// Unknown 表示该阶段没有观察到值；False 只表示明确观察到否定结果。
enum class MaiScreenshotObservation { Unknown, False, True };

enum class MaiScreenshotCheckPhase { Unknown, Entry, Worker, Frame, Delivery };

// 普通值类型，不调用系统 API。权限必须来自本阶段实际执行的 preflight；帧回调保持 Unknown。
// loginWindow 在前台应用未知时保持 Unknown；会话字典键缺失或类型错误也保持 Unknown。
struct MaiScreenshotAvailability {
    MaiScreenshotObservation guiSession = MaiScreenshotObservation::Unknown;
    MaiScreenshotObservation onConsole = MaiScreenshotObservation::Unknown;
    MaiScreenshotObservation loginDone = MaiScreenshotObservation::Unknown;
    MaiScreenshotObservation frontmostKnown = MaiScreenshotObservation::Unknown;
    MaiScreenshotObservation loginWindow = MaiScreenshotObservation::Unknown;
    MaiScreenshotObservation screenRecordingPermission = MaiScreenshotObservation::Unknown;
};

// 检查瞬间的状态，不代表当前权限回执或持续活跃的原生流。nativeStreamPresent 只表示持有对象，
// capturing 沿用后端的观察标记；失败后仍可持有原生流。由调用方在现有状态锁下填充/发布。
struct MaiScreenshotAvailabilitySnapshot {
    MaiScreenshotCheckPhase phase = MaiScreenshotCheckPhase::Unknown;
    MaiScreenshotAvailability observed;
    bool failed = false;
    bool nativeStreamPresent = false;
    bool capturing = false;
};

// 最近观察与最近拒绝各自保留。健康检查只更新 current；由持有状态锁的后端发布，
// 普通调用方可复制后读取。此类型本身不持有线程/平台资源。
struct MaiScreenshotAvailabilityHistory {
    MaiScreenshotAvailabilitySnapshot current;
    bool hasRejection = false;
    MaiScreenshotAvailabilitySnapshot lastRejection;
};

// 纯计算，无系统读取、状态变更、捕获或权限请求。桌面闸门保持既有 fail-closed 条件；
// 缺失 loginWindow 仅在 frontmostKnown 已经失败时出现，不把未知值当作已知未锁屏。
bool maiScreenshotDesktopCanCapture(const MaiScreenshotAvailability& observed);

// 桌面拒绝优先返回 Busy，并给出每个字段和阶段；只有实际观察到权限 False 才返回
// NotConfigured。failed 本身不阻止现有 ensureNative 故障恢复，也不新增重试或重建。
// 返回文本只含上述值类型字段，不含用户窗口、进程、图像或其他内容。
MaiError maiScreenshotAvailabilityError(const MaiScreenshotAvailabilitySnapshot& snapshot);

// 纯状态更新，不采样系统；拒绝分类复用同一 evaluator，避免诊断和返回错误各走一套判断。
// 调用方同步访问 history。返回分类结果，健康观察不清除历史拒绝证据。
MaiError maiRecordScreenshotAvailability(MaiScreenshotAvailabilityHistory& history,
                                         const MaiScreenshotAvailabilitySnapshot& snapshot);
