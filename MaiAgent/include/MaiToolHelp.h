// 文件作用：提供当前模型可见工具的分组短帮助，以及复用它的模型工具工厂。
#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "MaiError.h"
#include "MaiTool.h"

// 普通调用方直接构造请求，无需伪造 JSON 或执行工具协议。
struct MaiToolHelpRequest {
    // 仅接受 []、["-h"] 或 [group,"-h"]。前两者列出当前实际存在的组，
    // 后者读取一组短帮助；不解释单个工具自己的参数或原生命令行选项。
    // 每个参数最多 64 个 UTF-8 字节；空组名、未知组和多余参数均明确失败。
    std::vector<std::string> arguments;
    // 输出 UTF-8 字节预算，合法范围 512..8192；未知范围明确失败，不静默放宽。
    std::size_t byteBudget = 8192;
};

// 结果持有自己的文本，不借用注册表、请求或取消标志；可跨线程传递。
struct MaiToolHelpDocument {
    // 仅含组名、真实工具名和短用途；完整 Schema 与技能正文仍须按需读取。
    std::string text;
    // 表示预算导致整项省略；短用途本身固定为摘要，不是完整使用合同。
    bool truncated = false;
};

// 只查询 registry.metadata(model)，复用注册表的公开性与型号门禁。
// 已知内置名称归入稳定组，未知公开扩展归 general；未知或当前不可用的组失败。
// 不构造工具 Schema、不执行工具、不读取技能、凭据、文件或网络。
// 注册完成且不再修改注册表后，可由多个工作线程并发调用；调用期间 registry
// 必须存活。model 为空沿用注册表的全公开目录语义，不代表实际型号支持全部工具。
// cancel 可为空，非空时由调用方保证在本次同步调用期间存活；取消返回 Canceled，
// 无效请求返回 InvalidInput，无效公开元数据返回 Internal，失败不交付部分文本。
MaiResult<MaiToolHelpDocument> maiBuiltinToolHelp(const MaiToolRegistry& registry,
                                                  const std::string& model,
                                                  const MaiToolHelpRequest& request,
                                                  const std::atomic<bool>* cancel = nullptr);

// 薄适配器借用 registry；注册表必须在工具销毁前存活且不能移动。
// 工具仅将严格的 {arguments:[...]} 参数适配到同一普通入口，返回相同文本及截断标记。
// 它是可并行的只读工具，不需要工作目录或批准；注册不自动加入自举 Schema 集。
std::unique_ptr<MaiTool> makeMaiToolHelpTool(const MaiToolRegistry& registry);
