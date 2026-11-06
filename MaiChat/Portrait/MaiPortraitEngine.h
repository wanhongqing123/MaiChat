// 文件作用：定义照片美颜、美体的共用普通 C++ 接口；平台仅负责授权、解码和显示。
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// 八项参数依次为肤质平滑、提亮、冷暖、瘦脸、大眼、收腰、腿长、美白。
// 除冷暖为 [-1,1] 外均为 [0,1]；几何项的实际位移由处理器进一步限制。
// 美白使用共享 GPUPixel LUT 参考；身体净肤使用同一个原图 ROI 工作平面。
struct MaiPortraitParameters {
    static constexpr int kCount = 8;
    static constexpr int kWhiteningIndex = 7;
    using Values = std::array<float, kCount>;
    Values values;
    // 不属于八项训练属性：空为随平滑/美白强度渐进启用，false 保留体毛，true 显式净肤。
    std::optional<bool> removeBodyHair;
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
    bool skinAnalysisPerformed = false, personOwnershipReliable = false, faceColorSkipped = false;
    std::uint64_t bodySkinPixels = 0;
    int unreliablePersonMasks = 0;
    std::string skinScope;
    bool skinRefinementApplied = false;
    std::array<int, 4> skinAnalysisSourceRegion{};
    int skinAnalysisProxyWidth = 0, skinAnalysisProxyHeight = 0;
    int faceCount = 0;
    int personCount = 0;
    std::string analysisVersion;
    bool cleanApplied = false;
    float cleanStrength = 0, whiteningShaderStrength = 0, whiteningBlendGate = 0, whiteningShadowAmount = 0;
    std::string cleanSkipReason, cleanScope, bodyOwnershipSkipReason;
};

struct MaiPortraitRegionContext;

// 全图颜色阶段使用拥有的低分辨率语义/人物遮罩，再以原生像素引导边缘上采样。
// 各 tile 的写入矩形不重叠，采样矩形带 halo；不能把每个 tile 当独立人像重新分析。
struct MaiPortraitSkinTile {
    std::array<int, 4> sample{};
    std::array<int, 4> write{};
};
struct MaiPortraitPersonSeed {
    std::array<float, 4> box{};
    std::array<float, 2> hip{}, extent{};
    float confidence = 0;
};
struct MaiPortraitSkinPlan {
    int sourceWidth = 0, sourceHeight = 0, proxyWidth = 0, proxyHeight = 0;
    int faceCount = 0, personCount = 0, subjectIndex = 0;
    bool hasPersonOwnership = false, faceColorSkipped = false, faceProtected = false;
    int unreliablePersonMasks = 0, bodyConfidencePixels = 0, personAnalysisWindows = 1;
    std::array<int, 4> skinProxyRegion{}, analysisSourceRegion{}, refinementSourceRegion{};
    int refinementProxyWidth = 0, refinementProxyHeight = 0, selectedPerson = -1;
    bool refinementRequired = false, refinementApplied = false, visibleSourcePartial = false;
    std::vector<MaiPortraitPersonSeed> people;
    // 原全图的竞争人物硬排除独立保留；不能随粗 selected alpha 被替换或再作软乘法。
    int foreignProxyWidth = 0, foreignProxyHeight = 0;
    std::vector<unsigned char> globalForeignVeto;
    MaiPortraitFaceHint selectedFace;
    std::array<int, 4> faceRegion{};
    std::vector<unsigned char> alpha, ownership, proxyRgba, faceProtection, faceBlend, faceExclusion;
    // 仅 class2 且可靠归属的身体语义；脸部 class3 不得进入去毛工作场。
    std::vector<unsigned char> bodyAlpha;
    // 所有 tile 共享真实原图 ROI 工作平面 W 和未合成目标 T，最长边 <=1024。
    // native 只加回 20% 高频，再按成熟 bodyAlpha 合成一次；不从已混结果反除小 alpha。
    std::vector<std::array<float, 3>> materialOriginal, materialTarget;
    bool materialPrepared = false, cleanApplied = false;
    float cleanStrength = 0, whiteningShaderStrength = 0, whiteningBlendGate = 0, whiteningShadowAmount = 0;
    // 唯一有效人脸仍可单独保护；竞争人物seeds不构成可靠身体归属，具体原因如实返回。
    std::string cleanSkipReason, cleanScope, bodyOwnershipSkipReason;
    std::vector<std::array<float, 4>> excludedFaces;
    std::vector<MaiPortraitSkinTile> tiles;
};

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
    // 保留旧低级Lab工具供普通调用兼容；正式美白业务只使用下方shader/gate映射与共享LUT。
    static float whiteningResponse(float strength);
    // 正式美白参数映射：.6 -> 原 shader .25；整个 LUT 效果在 0..0.05 C1 渐入。
    static float whiteningShaderStrength(float strength);
    static float whiteningBlendGate(float strength);
    // 固定请求的逐像素暗部曲线，D=25*s*(2-s)；不以图像均值控制效果。
    // L<=3与L>=50不变，固定s下曲线C1且严格递增；这不是整条管线跨输入的单调性承诺。
    static float whiteningShadowAmount(float strength);
    static float whiteningShadowLightness(float lightness, float strength);
    // 正式美白的未合成目标：保留已生效平滑/提亮/暖度后的inputLab色相，
    // 色度不超过该输入及LUT目标二者的较小值，L使用上述暗部曲线。
    // Lab输入范围同旧低级工具；strength=0返回inputLab。正强度的目标尚未乘gate/皮肤alpha。
    static std::array<float, 3> whiteningTargetLab(const std::array<float, 3>& inputLab,
                                                 const std::array<float, 3>& lutLab, float strength);
    // 目标超出sRGB时沿同一Lab色相减色度，保L；返回0..1浮点sRGB，供普通调用及原生行算子共用。
    static std::array<float, 3> whiteningTargetRgb(const std::array<float, 3>& inputLab,
                                                 const std::array<float, 3>& lutLab, float strength);
    // 解析 D65 Lab 到线性 RGB，再连续压缩临界色差；不会依赖 OpenCV 的 RGB clamp。
    // 保持亮度和黑白端点，输入须有限、L 为 [0,100]、a/b 为 [-160,160]。
    // amount 必须显式给出（[0,1]）；肩部宽度随它趋零，软 mask/微小强度不产生固定去色跳变。
    static std::array<float, 3> skinRgbFromLab(const std::array<float, 3>& lab, float amount);
    static std::array<float, 3> toneSkinLab(const std::array<float, 3>& lab, float amount);
    // 全图 proxy 按原图比例提供，最长边 <=512；原图每边 <=20000、至多 3200 万像素。
    // 返回复制的遮罩/图像及 <=1040 的采样 tile，写入区 <=1024，halo 为 8。
    // 真实宿主人脸坐标以原图左上为原点；有脸时 subjectIndex 按脸从左到右编号，
    // 无脸时按检测人物编号；-1 只允许唯一目标。归属不确定时抛异常，不编辑其他人物。
    // hasPersonOwnership=false 时范围仅为已选脸；不能宣称整个人体均已覆盖。
    MaiPortraitSkinPlan analyzeSkin(const unsigned char* rgba, int width, int height, int stride, int sourceWidth,
                                    int sourceHeight, const std::vector<MaiPortraitFaceHint>& faces, int subjectIndex);
    // 使用计划绑定的真实原图区域工作平面（最长边<=1024）重新估计同一人物，替换粗 selected mask，
    // 保留全图竞争人物硬排除及全局坐标/特征/身份。必须在 Protect 前至多一次。
    void refineSkin(MaiPortraitSkinPlan& plan, const unsigned char* rgba, int width, int height, int stride);
    // 只提交 faceRegion 指定的原生像素（每边 <=2048），不能用缩略图冒充。
    // 无脸时接受 nullptr/0；必须恰好调用一次。mesh 失败记录 faceColorSkipped，
    // 不猜眼嘴位置；调用方仍须验证是否有足够身体皮肤，否则不得发布成功结果。
    void protectSkinFace(MaiPortraitSkinPlan& plan, const unsigned char* rgba, int width, int height, int stride);
    // Protect 后、tile 前一次构建共享身体净肤场；C ABI 内部调用，宿主无需新阶段。
    // 工作平面来自 Refine 的真实源 ROI（或本身已为原生尺寸的初始小图），不得放大旧 proxy。
    static void prepareSkinMaterial(MaiPortraitSkinPlan& plan, const MaiPortraitParameters& parameters);
    // 此方法只使用拥有的 plan，不依赖 ORT 会话；配置更新不能使正在处理的计划悬空。
    // 输入严格匹配 sample；参数有限且沿用八项范围。验证 mask 大小/矩形边界后才取像素。
    // 返回 sample 大小的已合成 RGBA/灰度 alpha，统计只覆盖 write；调用方只合入 write。
    // plan 不变，可顺序处理；C ABI 额外负责拒绝重复 tile 和未完成的发布。
    static MaiPortraitOutput applySkinTile(const unsigned char* rgba, int width, int height, int stride,
                                           const MaiPortraitSkinPlan& plan, int tileIndex,
                                           const MaiPortraitParameters& parameters);

private:
    class Implementation;
    std::unique_ptr<Implementation> mImplementation;
};
