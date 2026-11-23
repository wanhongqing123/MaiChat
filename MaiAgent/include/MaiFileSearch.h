// 文件作用：提供不依赖工具协议的只读文件名与文件内容搜索。
// 对外类型：MaiFileSearch 及其选项、分页结果；调用方负责在允许阻塞的工作线程调用。
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "MaiError.h"
#include "MaiFilePath.h"

enum class MaiFileNameSearchMode { Glob, Fuzzy };
enum class MaiFileContentSearchMode { Lines, Files, Count };

// 每次调用的协作式预算。时间检查不能抢占系统文件 I/O；正则匹配不使用回溯引擎。
// maxEntries 限制一次建立目录快照的项数；超过时明确报告不完整，不能据此断言没有命中。
// 其余工作量上限可以通过返回的游标继续；单文件过大则跳过并记录原因。
struct MaiFileSearchBudget {
    std::size_t maxEntries = 50000;
    std::size_t maxFiles = 10000;
    std::uint64_t maxBytes = 32u * 1024 * 1024;
    std::uint64_t maxFileBytes = 2u * 1024 * 1024;
    std::size_t maxOutputBytes = 64u * 1024;
    std::uint64_t maxRegexSteps = 16000000;
    std::uint64_t maxMilliseconds = 5000;
};

// include/exclude 为根目录相对的 glob；无斜杠时按最后一段匹配。
// 支持 *, **, ?, [] 和有界 {a,b} 备选；**/ 可以匹配零层目录。exclude 优先于 include。
// 花括号必须配对且每个备选非空；查询、include、exclude 的无效规则均返回 InvalidInput。
// 默认剪枝版本控制元数据、依赖、常见构建目录及嵌套仓库/子模块；includeHidden 控制点号开头的项。
// 显式 root 不受自身目录名或仓库身份排除；非递归列举保留依赖/子仓库目录入口。
// gitignore 读取搜索根及其子目录的 .gitignore，支持顺序覆盖、!、/ 和目录规则；
// 不读取根之外的父级、全局 git 配置或 .git/info/exclude，也不重新进入已忽略的父目录。
struct MaiFileSearchFilter {
    bool includeHidden = false;
    bool useDefaultExcludes = true;
    bool respectGitIgnore = true;
    std::vector<std::string> includeGlobs;
    std::vector<std::string> excludeGlobs;
};

// 回调仅在本次同步调用中借用，不保存在缓存或游标中。
// pathAllowed 接收重新解析后的真实路径；未提供时普通 API 只检查 root 边界。
// decodeText 用于接入宿主已有的多编码解码器；未提供时只接受 UTF-8（允许 BOM）。
// 解码失败、二进制文件、路径拒绝和文件变化均进入 issues，不能伪装成无命中。
struct MaiFileSearchCallbacks {
    std::function<bool()> isCanceled;
    std::function<bool(const MaiFilePath&)> pathAllowed;
    std::function<MaiResult<std::string>(const MaiFilePath&, const std::string&)> decodeText;
};

struct MaiFileNameSearchOptions {
    MaiFilePath root;
    // 调用方提供的访问策略及解码配置身份；改变任一策略时必须改变此值。
    // 不承载密钥或令牌。不相同的 scope 不共享目录索引或查询游标。
    std::string scopeKey;
    // 强制重建新查询的目录索引；不能和 cursor 同时使用。
    bool refresh = false;
    MaiFileNameSearchMode mode = MaiFileNameSearchMode::Glob;
    std::string query = "**/*";
    bool caseSensitive = false;
    std::size_t pageSize = 300;
    MaiFileSearchFilter filter;
    MaiFileSearchBudget budget;
    std::string cursor;
    // 默认保持递归文件搜索；false 返回 root 的直接普通文件和目录，不进入子目录。
    // 独立绑定快照/游标范围，不把无斜杠 glob 的既有 basename 语义悄悄改为列表。
    bool recursive = true;
};

// pattern 的默认含义是字面串；兼容旧工具的正则调用应显式设置 isRegex。
// 正则为字节语义的安全子集：分组、|、.、[]、^、$、*、+、?、{m,n}、\d、\s、\w、\b。
// 花括号重复次数最多 128，全部展开后最多 2048 个状态；\b 的单词定义为 ASCII 字母数字下划线。
// 不支持回溯引用、环视、特殊分组或惰性量词，遇到这些语法返回 InvalidInput。
// 大小写折叠只覆盖 ASCII；Unicode UTF-8 字节保持原样。
// 每一完整逻辑行先匹配，再截取展示窗口；displayOffsetBytes 标识该窗口在原行中的位置。
// Count 统计命中行数，每个文件输出一个计数；Files 每个命中文件只输出一次。
struct MaiFileContentSearchOptions {
    // 可传目录或精确普通文件；精确文件不遍历父目录，并只应用显式 include/exclude。
    // 精确文件超过 maxFileBytes、读取或解码失败直接返回错误；目录搜索将单个失败记入 issues。
    MaiFilePath root;
    std::string scopeKey;
    bool refresh = false;
    std::string pattern;
    bool isRegex = false;
    bool caseSensitive = true;
    MaiFileContentSearchMode mode = MaiFileContentSearchMode::Lines;
    std::size_t contextBefore = 0;
    std::size_t contextAfter = 0;
    std::size_t pageSize = 200;
    std::size_t maxLineDisplayBytes = 400;
    // 显式只聚合统计：不输出明细，不因 pageSize 停止，仍受本次工作预算约束。
    // 未完成可沿 cursor 继续；Files 模式也扫描全部行以区分行数和文件数。
    bool summaryOnly = false;
    MaiFileSearchFilter filter;
    MaiFileSearchBudget budget;
    std::string cursor;
};

struct MaiFileSearchIssue {
    std::string reason;
    std::string path;
    std::size_t count = 1;
};

struct MaiFileNameMatch {
    std::string path;
    std::int64_t score = 0;
    // 仅 recursive=false 可以返回目录；目录身份同样在分页重放时校验。
    bool isDirectory = false;
};

struct MaiFileSearchLine {
    std::size_t line = 0;
    std::string text;
    std::size_t displayOffsetBytes = 0;
    bool truncated = false;
};

struct MaiFileContentMatch {
    std::string path;
    std::size_t line = 0;
    std::string text;
    std::size_t displayOffsetBytes = 0;
    bool truncated = false;
    std::size_t count = 0;
    std::vector<MaiFileSearchLine> before;
    std::vector<MaiFileSearchLine> after;
};

// hasMore/nextCursor 表示确实保留的下一页，和 partial（搜索覆盖范围不完整）分开。
// 游标绑定实例、真实根、scopeKey、全部查询参数和冻结的目录快照；不会在分页时重新遍历。
// 返回及重放的路径逐个重新核对访问许可、真实边界和普通文件身份，变化时明确报错。
// 未返回路径的变化、新建文件和新忽略规则不会自动加入冻结快照；这不是实时数据库或监视器。
// 同根、scope、过滤条件的新查询在采集完成后的 5 秒内共享目录索引；可显式 refresh，
// 或 invalidate/clear 后重建。无命中只针对所标识的快照，snapshotAgeMilliseconds 表明其年龄。
// 每个实例最多保留 8 个查询会话、总计 64 MiB（共享索引只计一次），空闲 120 秒淘汰。
// 若当前会话本身无法保留，会返回 partial/cache_capacity 且不返回失效的 nextCursor。
// 游标可重放同一页，进程退出后失效；取消不保存本次进度。
// visitedEntries/searchedFiles/readBytes 是本次调用的工作量；缓存重放三项均为零。
// canceled 保留已产生结果但不继续保存本次进度；调用方可重放原游标或发起新查询。
struct MaiFileSearchPage {
    MaiError error;
    bool partial = false;
    bool canceled = false;
    bool hasMore = false;
    bool cacheReused = false;
    std::uint64_t snapshotAgeMilliseconds = 0;
    std::string snapshotId;
    std::string nextCursor;
    std::size_t visitedEntries = 0;
    std::size_t searchedFiles = 0;
    std::uint64_t readBytes = 0;
    std::size_t matchedLines = 0;
    // 总数绑定冻结查询范围；未覆盖内容返回 null，下界只累计真正消费的命中。
    // 递归 name/Files 的单位为不同文件；浅层 name 为文件或目录条目；Lines/Count 为命中行。
    // 页重放保留原统计，不重复累计；目录枚举完成不代表内容已经搜索完成。
    std::optional<std::size_t> totalMatches;
    std::size_t totalMatchesLowerBound = 0;
    std::optional<std::size_t> totalMatchingFiles;
    std::size_t totalMatchingFilesLowerBound = 0;
    std::vector<MaiFileSearchIssue> issues;
    std::vector<MaiFileNameMatch> files;
    std::vector<MaiFileContentMatch> matches;
    // 内容查询是否已尝试读取/扫描文件。保存到分页快照，重放不会因本次 I/O 为零而变回 false。
    // 对文件名查询无意义；完整空语料也可能为 false，调用方须同时看覆盖范围和 issues。
    bool contentSearchStarted = false;
    // 默认查询范围排除的嵌套仓库数量；最多保留 8 个、各 256 UTF-8 字节的路径供说明。
    // 这是过滤范围而不是读取失败，不设置 partial；缓存重放保留同一快照的统计。
    std::size_t excludedNestedRepositories = 0;
    std::vector<std::string> excludedNestedRepositoryPaths;
};

// 线程安全的同步服务；内部互斥量串行化同一实例的调用。等待期间也轮询取消和时间预算。
// 回调不能重入本实例；单次系统 I/O 或宿主回调无法被本服务抢占。
// 实例拥有快照与分页进度；析构和 clear 释放全部缓存，invalidate 仅清除指定真实根。
// 不跟随目录或文件符号链接；枚举、读取前后和分页重放均复核真实路径及普通文件身份。
// 这是尽力防御并发修改的只读搜索，不提供对恶意并发重命名的文件句柄级沙箱保证。
class MaiFileSearch {
public:
    MaiFileSearch();
    ~MaiFileSearch();
    MaiFileSearch(const MaiFileSearch&) = delete;
    MaiFileSearch& operator=(const MaiFileSearch&) = delete;

    MaiFileSearchPage findFiles(const MaiFileNameSearchOptions& options,
                                const MaiFileSearchCallbacks& callbacks = {});
    MaiFileSearchPage searchText(const MaiFileContentSearchOptions& options,
                                 const MaiFileSearchCallbacks& callbacks = {});
    void invalidate(const MaiFilePath& root);
    void clear();

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};
