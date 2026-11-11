// 文件作用：Windows 整屏采集内部适配，声明不暴露 COM、Windows 或工具协议类型。
// 这不是稳定的公共 API；资源只允许由创建它的采集工作线程调用和销毁。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "MaiError.h"

enum class MaiDesktopRotation { Identity, Clockwise90, Clockwise180, Clockwise270 };

// 将含行填充的 BGRA 表面转为显示方向的紧密不透明 BGRA。
// 输入长度、尺寸、步长或旋转无效时返回 false，并保留调用方输出。
bool maiCopyDesktopBgra(const std::uint8_t* source, std::size_t sourceBytes, std::uint32_t width,
                        std::uint32_t height, std::size_t stride, MaiDesktopRotation rotation,
                        std::vector<std::uint8_t>& pixels, std::uint32_t& outputWidth,
                        std::uint32_t& outputHeight);

// Windows FP16 桌面是线性 scRGB RGBA：1.0 代表 80 nits。
// 先按该显示器的 SDR white nits 归一化，再编码为 sRGB BGRA；PNG 不承载 HDR，
// 超出 SDR 范围的颜色裁剪，NaN 记黑、正无穷记白，alpha 始终不透明。
// 行填充、旋转、无效输入及输出保留的契约与 maiCopyDesktopBgra 相同。
bool maiCopyDesktopScRgb(const std::uint8_t* source, std::size_t sourceBytes, std::uint32_t width,
                         std::uint32_t height, std::size_t stride, MaiDesktopRotation rotation,
                         double sdrWhiteNits, std::vector<std::uint8_t>& pixels,
                         std::uint32_t& outputWidth, std::uint32_t& outputHeight);

// DXGI LastPresentTime 是原始 QPC tick，区别于 WGC 的 100ns TimeSpan。
// 用后采样 QPC 算帧龄并向上取整，返回保守 Unix 毫秒；无时间证明时返回 0。
std::int64_t maiDesktopSourceProducedAtMs(std::int64_t sourceQpc, std::int64_t unix100ns,
                                          std::int64_t qpcAfter, std::int64_t frequency);

// LastPresentTime 为 0 只证明收到指针更新，首次资源的像素也可能尚未初始化。
// 首次和后续都须真实桌面呈现；0、相同/倒退的呈现时间不能更新桌面帧元数据。
bool maiDesktopFrameUpdatesCache(bool hasFrame, std::int64_t previousSourceQpc,
                                 std::int64_t sourceQpc);

struct MaiDesktopDuplicationFrame {
    // 只在完整桌面呈现后置 true；纯指针更新不能初始化或替换桌面图。
    bool desktopChanged = false;
    std::int64_t sourceQpc = 0;
};

// 同线程持有匹配 HMONITOR 的 adapter、device、duplication 和最新完整桌面。
// poll 不等待；调用方负责有限间隔轮询、取消与环境检查。仅桌面呈现更新 sourceQpc；
// 指针移动和等待超时不会制造新帧。任何原生错误立即丢弃全部缓存及资源，
// 后续 poll/copy 不会返回旧图；只有显式 open 才能重新建立采集。
class MaiDesktopDuplication {
public:
    MaiDesktopDuplication();
    ~MaiDesktopDuplication();
    MaiDesktopDuplication(const MaiDesktopDuplication&) = delete;
    MaiDesktopDuplication& operator=(const MaiDesktopDuplication&) = delete;

    // deviceCreated 可选输出也在失败时填写，供累计设备计数区别设备成功而会话失败。
    MaiError open(std::uintptr_t monitor, bool* deviceCreated = nullptr);
    MaiError poll(MaiDesktopDuplicationFrame& frame);
    // 返回缓存副本，在副本上绘制独立指针，绝不修改系统指针或原桌面缓存。
    MaiError copyPixels(std::vector<std::uint8_t>& pixels, std::uint32_t& width,
                        std::uint32_t& height);
    bool isOpen() const;
    void close() noexcept;

private:
    struct State;
    std::unique_ptr<State> mState;
};
