// 文件作用：普通原生相机采集接口。Qt 仅绑定控件和展示帧，平台实现负责设备、权限和原生拍照。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class MaiCameraCapture final {
public:
    enum class State { Stopped, Starting, Running, Error };

    // 拥有内存的 top-down BGRA8。stride >= width * 4，pixels 至少有 stride * height 字节。
    // alpha 为 255；方向/镜像策略与原生拍照保持一致，UI 不另行修改捕获内容。
    // sequence 是有效帧的正序号；回调引用只在调用期间有效，保留帧时须自行复制。
    struct Frame {
        std::vector<std::uint8_t> pixels;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t stride = 0;
        std::uint64_t sequence = 0;
    };

    // JPEG 由原生静态照片输出产生，不以预览帧编码冒充原生照片。
    // 成功时 jpeg 非空且 error 为空；失败时 jpeg 为空且 error 非空。
    // requestId 等于 capturePhoto 的调用标识，调用方负责新建文件和取消后的清理。
    struct Photo {
        std::uint64_t requestId = 0;
        std::vector<std::uint8_t> jpeg;
        std::string error;
    };

    struct Callbacks {
        std::function<void(const Frame&)> frame;
        std::function<void(State, const std::string&)> state;
        std::function<void(const Photo&)> photo;
    };

    // 回调在相机自己的串行交付线程上执行。调用方须投递 UI 工作，不得在回调里同步
    // stop 或析构本对象。回调不可抛异常；平台实现隔离异常，不允许终止系统回调线程。
    explicit MaiCameraCapture(Callbacks callbacks);
    ~MaiCameraCapture();
    MaiCameraCapture(const MaiCameraCapture&) = delete;
    MaiCameraCapture& operator=(const MaiCameraCapture&) = delete;

    // 只读设备枚举，不打开设备或触发授权。true 不保证设备未被占用或当前权限允许。
    static bool isAvailable();

    // 异步开始；true 仅表示受理，只有收到首个有效预览帧后才发 Running。
    // 正在运行或启动时返回 false。授权拒绝、设备中断等通过 Error 回调报告并失效缓存。
    // 本接口只采集视频设备，不得隐式打开麦克风。stop 后可再次 start。
    bool start();

    // 只在 Running 时接受非零 ID，最多一项 pending。受理后异步回调一次 Photo；
    // stop 取消的请求不再回调，迟到系统结果被丢弃。false 不代表已经拍照或保存文件。
    bool capturePhoto(std::uint64_t requestId);

    // 幂等。先失效本代请求/发布闸门，再停设备并排空/等待交付；返回后无用户回调。
    // 不得从上述用户回调中调用。关闭/取消须释放自己的设备，不能改变系统权限设置。
    void stop();

private:
    struct Runtime;
    std::shared_ptr<Runtime> mRuntime;
};
