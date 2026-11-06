// 普通 C++ 身体净肤：已确认的身体内区去细毛/小暗点，再平滑匀肤；不负责人物分析。
#pragma once

#include <cstdint>
#include <array>
#include <vector>

struct MaiBodySkinRetouchOutput {
    // 紧密排列、已合成的非预乘 sRGB RGBA8；alpha 与输入逐字节相同。
    std::vector<unsigned char> rgba;
    // 诊断作用域，不是需要宿主再次相乘的合成 alpha；零强度/范围外为零。
    std::vector<unsigned char> mask;
    // 全强度阶段识别出的细毛/小暗点支持域。它不是毛发语义分类结果。
    std::vector<unsigned char> repairMask;
    std::uint64_t scopePixels = 0, repairPixels = 0, changedPixels = 0;
    int candidateComponents = 0, acceptedComponents = 0;
};

struct MaiBodySkinMaterial {
    // 未乘边缘 alpha 的目标 RGB（0..255）；独立保留，禁止从已合成 RGBA 反除小 alpha。
    std::vector<std::array<float, 3>> target;
    std::vector<unsigned char> alpha;
    std::uint64_t repairPixels = 0;
};

class MaiBodySkinRetouch {
public:
    // 输入必须是已转正的非预乘 sRGB RGBA8；行跨度至少 width*4。
    // bodyScope 为同尺寸的最终二值可信身体内区（只能为 0/255），行跨度至少 width。
    // 宿主须先排除脸、头发、眉眼、胡须、衣服、其他人物及不可靠边缘。本算子
    // 不推断上述语义，且不能保证区分细纹身与细毛/暗点。不会扩张输入 scope。
    // strength 为 [0,1]，1 精确使用已验证的 v2 Telea + v3 净肤参数；中间值在
    // 完整固定效果与原图间连续混合。0 不运行颜色转换/修复，RGBA 逐字节保真。
    // 完整场契约：整个分析 ROI 必须一次提交，且始终使用同一原生源/最终 scope。
    // 连通组件分类及 Telea 传播不能由固定 8px halo 等价重建；禁止独立处理相邻 tile。
    // 将完整结果分块写入是安全的；需要内存分块时须先设计全局组件/修复场，不能截断输入。
    // σ3 低通引导 + r9 联合滤波本身需 18px 依赖范围，但这不是全流程 halo 保证。
    // 图边界沿用参考实现：形态学默认边界、低通复制边界、联合滤波截断支持。
    // 尺寸每边 <=20000、总计 <=3200 万像素；不修改输入，失败抛 std::invalid_argument。
    static MaiBodySkinRetouchOutput apply(const unsigned char* rgba, int width, int height, int rgbaStride,
                                          const unsigned char* bodyScope, int scopeStride, float strength);
    // 有界工作平面专用；完整 ROI 一次计算，最长边 <=1024。target 内保留参考的20%工作纹理，
    // alpha 为 scope 内6px渐变；宿主共享场在原生像素上补20% native-W，再按alpha合成一次。
    static MaiBodySkinMaterial buildMaterial(const unsigned char* rgba, int width, int height, int rgbaStride,
                                             const unsigned char* bodyScope, int scopeStride);
};
