// 文件作用：测试私有的原生相机替身；使用冻结的普通接口，不枚举或打开真实设备。
#pragma once

#include "MaiCameraCapture.h"

#include <memory>
#include <vector>

namespace DesktopCameraNativeFake {
struct Control;

// 故意保留旧回调，模拟 stop 前已排入 Qt 的工作及迟到系统结果。
struct Handle {
    std::shared_ptr<Control> control;
    MaiCameraCapture::Callbacks callbacks;
};

void reset();
void setAvailable(bool available);
void setStartAccepted(bool accepted);
Handle latest();
int startCount();
int stopCount();
int captureCount();
std::uint64_t pendingRequestId();

// 每次交付都在非 Qt 线程执行并等待返回；替身不需要 Qt、设备或权限。
void deliverState(const Handle& handle, MaiCameraCapture::State state, const std::string& error = {});
void deliverFrame(const Handle& handle, const MaiCameraCapture::Frame& frame);
void deliverFrames(const Handle& handle, const std::vector<MaiCameraCapture::Frame>& frames);
void deliverPhoto(const Handle& handle, const MaiCameraCapture::Photo& photo);
}  // namespace DesktopCameraNativeFake
