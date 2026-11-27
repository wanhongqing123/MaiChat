// 文件作用：提供有界 GitHub 技能发现及无覆盖安装的普通代码入口。
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "MaiSkillManager.h"

// 根目录仅由宿主配置；空目录禁用对应作用域。预算是硬上限，超限不返回截断成功。
struct MaiSkillInstallerOptions {
    MaiFilePath homeDirectory;
    MaiFilePath workspaceDirectory;
    std::size_t maxFrontmatterBytes = 64 * 1024;
    std::size_t maxBodyBytes = 256 * 1024;
    std::size_t maxFileBytes = 2 * 1024 * 1024;
    std::size_t maxTotalBytes = 16 * 1024 * 1024;
    std::size_t maxFiles = 512;
    std::size_t maxTreeEntries = 20000;
    std::size_t maxReadmeBytes = 64 * 1024;
};

// sourceUrl 只接受公开 HTTPS github.com 仓库及 tree/blob 地址，不接受凭据或查询参数。
// path 是仓库相对技能目录，ref 是分支、标签或提交；二者独立可避免含斜杠分支歧义。
// 空 path 在安装时仅允许仓库中唯一的技能；Additional 不是可安装作用域。
struct MaiSkillInstallRequest {
    std::string sourceUrl;
    std::string path;
    std::string ref;
    MaiSkillScope scope = MaiSkillScope::User;
};

struct MaiSkillRepositoryCandidate {
    std::string path;
    // 未读取元数据时保持为空，不能把目录名冒充 frontmatter 名称。
    std::string name;
    std::string description;
};

struct MaiSkillRepositoryLink {
    std::string url;
    std::string context;
};

// README 链接和描述是未受信数据，供调用方语义判断；发现过程不自动跟随或执行。
struct MaiSkillRepositoryDiscovery {
    std::string sourceUrl;
    std::string commit;
    std::vector<MaiSkillRepositoryCandidate> candidates;
    std::vector<MaiSkillRepositoryLink> links;
};

struct MaiSkillInstallResult {
    MaiSkillMetadata metadata;
    MaiFilePath directory;
    std::string sourceUrl;
    std::string commit;
    std::string sourcePath;
    std::size_t fileCount = 0;
    std::size_t totalBytes = 0;
    // 安装只复制技能与资源，不安装其 CLI、浏览器或其他运行依赖。
    bool dependenciesInstalled = false;
};

// 同步阻塞入口，应在工作线程调用；实例无共享可变状态，调用可并发。
// Fetch 用于宿主接入或确定性测试，必须遵守 maxBytes、取消及固定来源契约。
// 默认实现验证 TLS，不跟随重定向；失败或取消只清理自身带身份守卫的暂存目录。
// 安装仅原子发布新目录，绝不覆盖现有技能；未知 frontmatter 不授予权限或安装依赖。
class MaiSkillInstaller {
public:
    using Fetch = std::function<MaiResult<std::string>(const std::string&, std::size_t,
                                                       const std::function<bool()>&)>;

    explicit MaiSkillInstaller(MaiSkillInstallerOptions options, Fetch fetch = {});
    MaiResult<MaiSkillRepositoryDiscovery> discover(
        const MaiSkillInstallRequest& request, const std::function<bool()>& isCanceled = {}) const;
    MaiResult<MaiSkillInstallResult> install(const MaiSkillInstallRequest& request,
                                             const std::function<bool()>& isCanceled = {}) const;

private:
    MaiSkillInstallerOptions mOptions;
    Fetch mFetch;
};
