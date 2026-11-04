// 文件作用：为本地主模型提供只读工具目录，按公开原名取回完整工具声明。
// 本类型不执行工具、不注册别名，也不改变宿主的权限和工具分派规则。
#pragma once

#include <string>
#include <vector>

#include "MaiError.h"
#include "MaiModelClient.h"

// 本地主模型的只读工具发现目录。
//
// 宿主传入当前允许公开的完整工具快照；目录拥有其副本，原始描述和参数 Schema 原文保持不变。
// 目录只按精确名字查找，不做关键词匹配、分类或名称改写，避免发现层悄悄缩小可用能力。
// 创建后没有修改接口；对象生命周期内可在多个线程并发读取，调用方持有返回值的独立副本。
// 发现结果仅用于后续模型请求中的工具声明，执行仍须经过宿主原有的权限和分派入口。
class MaiLlamaToolCatalog {
public:
    // 目录允许 1..512 个声明，名字必须非空、唯一，且不得占用发现入口 tool_search。
    // 所有字段须是没有嵌入 NUL 的合法 UTF-8，总计最多 2 MiB；参数 Schema 必须是合法对象。
    // 不合法时保存 InvalidInput，之后所有解析都失败；先检查 error() 再公开 specification()。
    // Schema 仅解析校验，不改写原文，避免发现层隐藏非法声明或改变其既有线协议。
    explicit MaiLlamaToolCatalog(std::vector<MaiToolSpec> specifications);

    // 成功时没有错误。引用只在本对象存活期间有效，不应与对象销毁并发。
    const MaiError& error() const;

    // 返回名为 tool_search 的声明，names 的枚举包含目录中的全部原名，顺序与快照一致。
    // 一次只允许请求 1..8 个不同名字；此工具只发现声明，不执行所选工具。
    // 无效目录返回空声明，调用方不得把它送给模型。
    MaiToolSpec specification() const;

    // 普通代码入口。按请求顺序返回完整声明的独立副本，不要求调用者构造工具请求文本。
    // 名字必须精确匹配；数量不在 1..8、重复或任一未知名字均返回 InvalidInput，绝不部分成功。
    MaiResult<std::vector<MaiToolSpec>> resolveNames(const std::vector<std::string>& names) const;

    // 模型工具入口，复用 resolveNames() 的查找与批次校验。
    // 参数必须是仅有一个 names 字段的对象，该字段必须是字符串数组；重复对象键同样拒绝。
    // JSON 无效、字段多余、名字不合法时返回 InvalidInput，不执行任何外部操作。
    MaiResult<std::vector<MaiToolSpec>> resolve(const std::string& argumentsJson) const;

private:
    // 构造后只读；保存原始文本，发现时不能把 Schema 解析再序列化而改变原声明。
    std::vector<MaiToolSpec> mSpecifications;
    // 构造时一次性确定的目录错误；非法目录不能靠请求参数绕过。
    MaiError mError;
};
