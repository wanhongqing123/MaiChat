// 文件作用：定义照片美颜、美体的共用普通 C++ 接口；平台仅负责授权、解码和显示。
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// 八项参数依次为肤质平滑、提亮、冷暖、瘦脸、大眼、收腰、腿长、美白。
// 除冷暖为 [-1,1] 外均为 [0,1]；几何项的实际位移由处理器进一步限制。
// 美白在选定脸部皮肤内提高 L* 并温和收缩 a*/b*，不以冷暖偏移替代；默认关闭。
struct MaiPortraitParameters {
    static constexpr int kCount = 8;
    static constexpr int kWhiteningIndex = 7;
    using Values = std::array<float, kCount>;
    Values values;
    MaiPortraitParameters();
};

struct MaiPortraitFaceHint {
    float x = 0, y = 0, width = 0, height = 0;
    float leftEyeX = 0, leftEyeY = 0, rightEyeX = 0, rightEyeY = 0;
    float confidence = 0;
};

struct MaiPortraitOutput {
    std::vector<unsigned char> rgba;
    // 与输入像素同尺寸的皮肤作用 alpha；输出 rgba 已经合成，不能再次乘此 mask。
    // 几何修饰不等同于皮肤 mask，调用方以完整 ROI patch 替换相同位置。
    std::vector<unsigned char> skinMask;
    std::array<float, 8> features{};
    std::array<int, 4> processingRegion{};
    std::array<double, 2> skinLightness{};
    std::array<double, 2> skinChroma{};
    std::uint64_t skinPixels = 0, changedPixels = 0;
    double skinWeight = 0;
    bool faceAnalysisPerformed = false, personAnalysisPerformed = false;
    int faceCount = 0;
    int personCount = 0;
    std::string analysisVersion;
};

struct MaiPortraitRegionContext;

// 会话持有模型和 ORT 资源。构造及 apply 都只能在后台串行执行，禁止 UI 线程调用。
// modelDirectory 为宿主已校验的打包模型目录；runtimePath 仅允许宿主打包的运行库。
// iOS 静态链接通过 apiBase 提供 OrtGetApiBase()；其生命周期必须覆盖整个会话。
// 输入为已按 EXIF 转正的非预乘 sRGB RGBA8，stride >= width*4，允许引用大图行跨度。
// 无 context 时整图至多 1200 万像素；有 context 时只能传原生 ROI（每边 <=2048），
// context 内定位和全局特征须来自可信宿主，不能以 ROI 特征替换原图的个性化特征。
// apply 不修改输入；结果拥有独立像素。subjectIndex 为从左到右排序的目标索引，
// -1 仅在检测到单个目标时自动选择。无法定位、姿态不适合或参数无效时抛异常，
// 调用边界必须转换为失败结果，不得回退到整图模糊或返回伪成功。
class MaiPortraitEngine {
public:
    MaiPortraitEngine(const std::string& modelDirectory, const std::string& runtimePath, const void* apiBase);
    ~MaiPortraitEngine();
    MaiPortraitEngine(const MaiPortraitEngine&) = delete;
    MaiPortraitEngine& operator=(const MaiPortraitEngine&) = delete;

    MaiPortraitOutput apply(const unsigned char* rgba, int width, int height, int stride,
                            const std::vector<MaiPortraitFaceHint>& faces, const MaiPortraitParameters& parameters,
                            bool body, int subjectIndex = -1, const MaiPortraitRegionContext* context = nullptr);

private:
    class Implementation;
    std::unique_ptr<Implementation> mImplementation;
};
