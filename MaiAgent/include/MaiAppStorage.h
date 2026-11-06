// 文件作用：提供普通调用与工具共用的应用存储预览和受限清理服务。
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "MaiError.h"

// 只允许宿主填入 App 自有、互不嵌套的目录，不能把系统共享临时目录冒充 App 目录。
// workspace 为空时使用当前调用的工作区；服务不接受模型指定绝对根目录。
struct MaiAppStoragePaths {
    std::string temporary;
    std::string cache;
    std::string workspace;
};

enum class MaiAppStorageTarget { App, Temporary, Cache, Workspace };

struct MaiAppStorageSelection {
    // App 保留旧扫描语义：统计三类目录，只有旧临时/缓存文件进入预览。
    MaiAppStorageTarget target = MaiAppStorageTarget::App;
    // 相对目录或文件。Workspace 只接受 frames_<id>、wmframes、screenshots、search、
    // browser、.maiagent/model-previews 下的明确范围；frames_* 可选全部该前缀目录。
    std::string scope;
    // 相对于所选根目录的路径模式，仅支持单段内 *，不支持 **、? 或父目录。
    // Workspace 无 scope 时，每条模式必须自行限定到上述允许的中间文件目录。
    std::vector<std::string> patterns;
    // 默认保留最近 24 小时文件。零小时也必须先预览，并且范围必须明确且有界。
    int minimumAgeHours = 24;
};

struct MaiAppStorageContext {
    std::string sessionId;
    std::string workspace;
    // 宿主提供当前账号仍在使用的附件、已交付文件或活动模型的绝对路径。
    // 工具入口从同工作区会话收据补齐；执行时再次提供，以保护预览后新增的交付。
    std::vector<std::string> protectedPaths;
    const std::atomic<bool>* cancel = nullptr;
    // 工作区预览及执行均须由宿主确认保护清单完整；未知不等于没有已交付文件。
    bool protectionComplete = false;
};

struct MaiAppStorageCandidate {
    std::string path;
    std::string relativePath;
    std::string target;
    std::uint64_t bytes = 0;
    std::int64_t modified = 0;
};

struct MaiAppStorageSummary {
    std::string target;
    std::uint64_t bytes = 0, files = 0;
    std::uint64_t eligibleBytes = 0, eligibleFiles = 0;
    std::uint64_t excludedRecentFiles = 0, excludedRecentBytes = 0;
    std::uint64_t excludedProtectedFiles = 0, excludedPolicyFiles = 0;
    bool truncated = false;
};

struct MaiAppStoragePreview {
    std::string id;
    int minimumAgeHours = 24;
    MaiAppStorageSelection selection;
    std::vector<MaiAppStorageSummary> summaries;
    // 精确清理集合，最多 1000 项；超限的文件绝不会在本次执行时补入。
    std::vector<MaiAppStorageCandidate> candidates;
    std::uint64_t candidateBytes = 0;
    bool limited = false;
};

struct MaiAppStorageCleanup {
    std::uint64_t removedFiles = 0, reclaimedBytes = 0, skippedFiles = 0;
};

// 每个实例只保留一份十分钟、单次使用的预览，并绑定会话及解析后的三个根目录。
// 可在工作线程调用，内部串行保护预览状态；取消时不扩大集合，已经删除的文件不回滚。
// 只删除普通文件，绝不删除目录。大小、文件身份和高精度时间变化时跳过；
// gallery-*、宿主保护路径及范围外成品保留。服务不推断图片是否已经交付，宿主须提供收据。
// 这不是对不受控并发写入者的文件锁；宿主不得同时替换清理中的路径。
class MaiAppStorage {
public:
    explicit MaiAppStorage(MaiAppStoragePaths paths);
    ~MaiAppStorage();
    MaiAppStorage(const MaiAppStorage&) = delete;
    MaiAppStorage& operator=(const MaiAppStorage&) = delete;

    MaiResult<MaiAppStoragePreview> preview(const MaiAppStorageSelection& selection,
                                            const MaiAppStorageContext& context);
    MaiResult<MaiAppStorageCleanup> cleanup(const std::string& previewId,
                                            const MaiAppStorageContext& context);

private:
    class Implementation;
    std::unique_ptr<Implementation> mImplementation;
};
