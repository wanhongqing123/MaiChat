// 文件作用：照片修饰结果、属性反馈和个人排序模型的本地普通代码接口。
#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "MaiPortraitEngine.h"

struct MaiPortraitTrainingSummary {
    int version = 0;
    int activeVersion = 0;
    int feedbackCount = 0;
    int sourceCount = 0;
    double trainingLoss = 0;
    double validationLoss = -1;
    bool activated = false;
};

struct MaiPortraitProfileStatus {
    int activeVersion = 0;
    int feedbackCount = 0;
    std::vector<int> versions;
};

// profileDirectory 必须来自宿主当前账号的私有目录，不能由工具参数指定。
// 同一个配置的实例只能后台串行使用；不同实例的写事务由 SQLite 串行化。
// 结果只在宿主完整保存输出后登记。反馈比较同一源像素的不同结果，并仅标注明确评价
// 的属性。未知属性不自动生成标签。train 进行有界梯度更新，保存真实权重 checkpoint；
// 少于三个独立源图时只保存候选，不替换正式版本。具备分组验证集且验证改善才启用。
// 美白为追加的第八属性；旧七属性反馈和权重保留，新属性从关闭状态开始学习。
// 冷暖范围 [-1,1]，其余 [0,1]；基础感知模型不参与本接口训练。
// 文件／数据库错误、无效结果或版本通过异常返回，C ABI 边界负责转成错误对象。
class MaiPortraitPersonalization {
public:
    explicit MaiPortraitPersonalization(const std::string& profileDirectory);
    ~MaiPortraitPersonalization();
    MaiPortraitPersonalization(const MaiPortraitPersonalization&) = delete;
    MaiPortraitPersonalization& operator=(const MaiPortraitPersonalization&) = delete;

    std::string originalSource(const std::string& path);
    std::string recordResult(const std::string& sourceFingerprint, const std::string& sourcePath,
                             const std::string& outputPath, const MaiPortraitParameters& parameters,
                             const std::array<float, 8>& features);
    int recordPreference(const std::string& preferredResult, const std::string& rejectedResult,
                         const std::vector<int>& attributes);
    MaiPortraitParameters recommend(const std::array<float, 8>& features);
    MaiPortraitTrainingSummary train();
    MaiPortraitProfileStatus status();
    void rollback(int version);

private:
    class Implementation;
    std::unique_ptr<Implementation> mImplementation;
};
