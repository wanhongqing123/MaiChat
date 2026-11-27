// 文件作用：固定会话的模型工具声明，并按需追加有界发现选中的声明。
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "MaiError.h"
#include "MaiModelClient.h"
#include "MaiTurnContext.h"

class MaiSessionStore;
class MaiToolRegistry;

// 只搜索名称与描述，不读取技能正文或返回 Schema。空查询按名称排序浏览。
// offset 是匹配列表中的位置；limit 为 1..10，select 最多 5 个精确公开名称。
struct MaiToolCatalogQuery {
    std::string query;
    std::size_t offset = 0;
    std::size_t limit = 5;
    std::vector<std::string> select;
};

struct MaiToolCatalogMetadata {
    std::string name;
    std::string description;
    bool loaded = false;
};

struct MaiToolCatalogPage {
    std::vector<MaiToolCatalogMetadata> tools;
    std::vector<std::string> selected;
    std::size_t total = 0;
    std::size_t nextOffset = 0;
};

// 普通入口与 tool_search 共用的核心。借用 store/registry，二者必须活到析构。
// 注册表在使用期间不得增删；方法可跨线程调用，内部串行化会话快照的准备与追加。
// 声明按 session/model 隔离，首次保存的字节及后续追加顺序跨轮次和重启保持固定。
// v2 显式升级 canonical apply_patch 与原生输入格式；保留 v1 模式和快照原文，
// 按当前注册表重建 v2 初始声明；升级可能使提示缓存失效一次，后续再次固定 v2 字节。
// 标准存储以 insert-if-absent 保存；不支持该能力的旧后端只保证实例生命周期。
// 读写错误返回原错误；失败保存不激活候选声明，不授予执行权限。
class MaiModelToolCatalog {
public:
    MaiModelToolCatalog(MaiSessionStore& store, const MaiToolRegistry& registry);
    ~MaiModelToolCatalog();
    MaiModelToolCatalog(const MaiModelToolCatalog&) = delete;
    MaiModelToolCatalog& operator=(const MaiModelToolCatalog&) = delete;

    // 仅在宿主已成功创建新会话、尚未发布会话 ID 时调用，固定 bounded 模式。
    // 不覆盖已有不可变模式；保存失败不更新缓存，也不假称初始化完成。
    // 自定义后端明确不支持持久化时仅缓存到本实例；不能用于改写旧会话。
    MaiError initializeNewSession(const std::string& sessionId);

    // 在工作线程调用；context.turnId 可为空，返回独立的不可变声明副本。
    // 已有旧历史而没有目录快照的会话保留全部声明，避免缩改旧前缀。
    MaiResult<std::vector<MaiToolSpec>> exposedSpecs(const MaiTurnContext& context);
    // 元数据结果描述至多 512 字节，每页至多 10 项；选择只影响下一次声明快照。
    // 非公开、当前型号不支持或未知名称拒绝，选择批次先完整校验再写入。
    MaiResult<MaiToolCatalogPage> search(const MaiTurnContext& context,
                                         const MaiToolCatalogQuery& query);
    // 宿主成功清空历史/删除会话后调用；不删除存储中的任何内容。
    void forgetSession(const std::string& sessionId);

private:
    struct Impl;
    std::unique_ptr<Impl> mImplementation;
};
