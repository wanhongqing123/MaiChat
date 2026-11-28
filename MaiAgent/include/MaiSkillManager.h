// 文件作用：按 Codex 的目录约定发现技能元数据，并按需读取技能正文。
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "MaiError.h"
#include "MaiFilePath.h"

enum class MaiSkillScope { Workspace, User, Additional, Builtin };

// 路径保持平台原生编码。空 homeDirectory 禁用个人技能；空 workspaceDirectory
// 禁用工作区技能。宿主提供实际用户主目录，不从模型参数接收根目录。
// 工作区从当前目录向最近 .git 标记处逐层发现 .agents/skills；无标记时仅查当前目录。
// additionalRoots 是宿主显式配置的技能目录，不追加 .agents/skills。
// frontmatter 预算为 8..1MiB，整份正文为 1..4MiB，技能数量为 1..20000。
// 扫描每个根最多 6 层、2000 个目录和 20000 个条目；达到限制会在目录中报告错误。
struct MaiSkillManagerOptions {
    MaiFilePath workspaceDirectory;
    MaiFilePath homeDirectory;
    std::vector<MaiFilePath> additionalRoots;
    std::size_t maxFrontmatterBytes = 64 * 1024;
    std::size_t maxBodyBytes = 256 * 1024;
    std::size_t maxDiscoveredSkills = 2048;
    // 目录提示 token 预算；0 按实际模型窗口的 2%，窗口未知时用 8000 字符。
    // 显式预算优先于窗口，并限制到 10000；不限制被显式选中的完整正文。
    std::size_t maxCatalogTokens = 0;
    // 宿主显式启用随 App 编译的只读资源，不解压、不覆盖用户技能目录。
    bool includeBuiltinSkills = false;
};

// id 是已发现文件的规范路径或内置资源的 builtin: 标识，仅用于已有目录条目查找。
// 同名技能保留，按工作区、个人、附加目录、内置资源及名称/路径排序；歧义时必须用 id。
// Builtin 使用 builtin: 稳定标识；逻辑 path 不授予磁盘路径或资源目录读取权限。
// 描述经过单行化；正文不会进入目录快照。所有字符串以 UTF-8 交付。
struct MaiSkillMetadata {
    std::string id;
    std::string name;
    std::string description;
    MaiFilePath path;
    MaiSkillScope scope = MaiSkillScope::Workspace;
    // agents/openai.yaml 的 policy；false 仅禁止隐式推荐，显式选择仍可读取。
    bool allowImplicitInvocation = true;
};

struct MaiSkillDiscoveryError {
    MaiFilePath path;
    MaiError error;
};

// 值快照可跨线程复制，之后发现或文件变化不会改变旧快照。
// 私有数据保留发现时的文件身份；公开元数据副本不能授予任意路径的读取权限。
class MaiSkillCatalog {
public:
    MaiSkillCatalog() = default;
    const std::vector<MaiSkillMetadata>& skills() const;
    const std::vector<MaiSkillDiscoveryError>& errors() const;

private:
    struct Data;
    std::shared_ptr<const Data> mData;
    friend class MaiSkillManager;
};

// 纯目录提示预算；两项均为 0 时采用 Codex 的 8000 Unicode 字符默认值。
// maxContextTokens 非零时优先，最多 10000；否则已知窗口取其 2%，至少 1 token。
// token 数使用项目已有估计器，不冒充供应商真实计数。
struct MaiSkillCatalogRenderOptions {
    std::size_t contextWindowTokens = 0;
    std::size_t maxContextTokens = 0;
};

// 搜索只访问不可变元数据，不读取正文。页按目录顺序排列；nextOffset < total
// 表示仍有匹配结果。描述最多 1024 字符，整页保守限制在 64KiB，避免工具输出无界。
struct MaiSkillDiscoveryPage {
    std::vector<MaiSkillMetadata> skills;
    std::size_t total = 0;
    std::size_t nextOffset = 0;
};

struct MaiSkillContents {
    MaiSkillMetadata metadata;
    // 完整 SKILL.md，包括 frontmatter；磁盘技能资源以 path.dirName() 为基准。
    // Builtin 为自包含只读文本，没有外部资源目录或可执行脚本。
    std::string contents;
};

// 普通调用与 skill_read 工具共用本对象，不执行脚本，也不授予技能声明的权限。
// snapshot/read 阻塞调用线程，应由轮次工作线程调用，不能放入界面或事件回调。
// 所有成员可跨会话并发调用；内部缓存有界，并以原生文件身份复核变化。
// 支持目录符号链接，按规范化文件路径去重；发现深度/条目/正文均有预算。
// 当前 frontmatter 的 name/description 支持普通/单引号标量、JSON 兼容的双引号转义、
// 缩进续行及 |/> 块；相关字段的 YAML 锚点、标签、流式容器和跨行引号明确报错。
// agents/openai.yaml 额外支持 policy 下普通映射的 allow_implicit_invocation: true/false。
// 文件最多 64KiB；相关策略解析失败时保留显式可访问条目、禁止隐式调用并报告错误。
// 其余可选字段（包括 metadata 的嵌套映射/列表）按不使用的数据跳过；不把其中的
// 权限、依赖或工具声明应用于产品。这是有界 frontmatter 子集，并非完整 YAML 解析器。
class MaiSkillManager {
public:
    MaiSkillManager();
    ~MaiSkillManager();
    MaiSkillManager(const MaiSkillManager&) = delete;
    MaiSkillManager& operator=(const MaiSkillManager&) = delete;

    MaiResult<MaiSkillCatalog> snapshot(const MaiSkillManagerOptions& options);
    MaiResult<MaiSkillMetadata> find(const MaiSkillCatalog& catalog,
                                     const std::string& idOrName) const;
    // 仅接受本实例发现的目录条目。文件自发现后变化返回 InvalidInput，调用方须刷新目录；
    // 超过 maxBodyBytes 明确失败，不把截断内容当作成功结果。
    MaiResult<MaiSkillContents> read(const MaiSkillCatalog& catalog, const std::string& idOrName);
    // 仍只允许目录中已有的规范/逻辑路径，但读取该文件当前的完整字节。
    // 用于会话已固定元数据之后的按需工具读取；允许普通覆写，拒绝链接重定向。
    // 句柄读取前后校验同一版本，其余预算和失败契约与 read 相同。
    MaiResult<MaiSkillContents> readCurrent(const MaiSkillCatalog& catalog,
                                            const std::string& idOrName);

    // 在工作线程查询平台原生用户主目录，不访问模型参数或更改进程环境。
    // Windows 使用宽字符 USERPROFILE；POSIX 使用 HOME，缺失时查账户目录。
    // 无法确定绝对目录时明确报错，不能静默禁用个人技能。
    static MaiResult<MaiFilePath> defaultHomeDirectory();

    // 纯内存校验，复用目录发现的同一 frontmatter 解析器；不授予 catalog/read 权限。
    // skillPath 只用于元数据路径及缺省名称，必须由调用方提供可信目标；不访问该路径。
    // contents 是完整 SKILL.md，正文及 frontmatter 超限均失败，未知字段仍是未受信数据。
    static MaiResult<MaiSkillMetadata> validateMetadata(const std::string& contents,
                                                        const MaiFilePath& skillPath,
                                                        std::size_t maxFrontmatterBytes = 64 * 1024,
                                                        std::size_t maxBodyBytes = 256 * 1024);

    // 纯内存格式化/选择，不做文件访问。显式 $name 只匹配唯一名称；
    // [$name](path) 按目录中的规范/逻辑路径选择，支持 skill:// 前缀。保留目录顺序。
    // description/name/path 都作为不可信目录数据引用，不自动拼入技能正文。
    static std::string formatCatalog(const MaiSkillCatalog& catalog);
    // 完整提示（包括遗漏/错误提示和发现入口）服从同一预算；先缩短所有描述，再舍弃整行。
    // 最小真实发现提示仍放不下时明确失败，不能静默隐藏技能或超过预算。
    static MaiResult<std::string> formatCatalog(const MaiSkillCatalog& catalog,
                                                const MaiSkillCatalogRenderOptions& options);
    // query 按 ASCII 不区分大小写的子串匹配名称、完整描述和 id；空串列出全部。
    // query 最多 4096 UTF-8 字节，limit 为 1..50，offset 是匹配序列的位置。
    // 包括禁止隐式调用的条目，并显式标志；只接受本实例的快照，不访问文件。
    MaiResult<MaiSkillDiscoveryPage> discover(const MaiSkillCatalog& catalog,
                                              const std::string& query = {}, std::size_t offset = 0,
                                              std::size_t limit = 20) const;
    static std::vector<MaiSkillMetadata> explicitMentions(const MaiSkillCatalog& catalog,
                                                          const std::string& text);
    // 只读取本条用户文本显式选择的技能，并渲染为带来源路径的不可信用户上下文。
    // 无选择返回空串，不读取正文；只匹配目录中的唯一名称或精确链接路径。
    // 输出预算为 1..4MiB，超限/读取失败明确返回错误，不执行脚本或授予权限。
    MaiResult<std::string> explicitContext(const MaiSkillCatalog& catalog, const std::string& text,
                                           std::size_t maxBytes = 4 * 1024 * 1024);

private:
    MaiResult<MaiSkillContents> readImpl(const MaiSkillCatalog& catalog,
                                         const std::string& idOrName, bool currentSource);
    struct State;
    std::unique_ptr<State> mState;
};
