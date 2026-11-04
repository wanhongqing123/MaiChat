// 文件作用：以真实本地模型选择所需工具声明，再复用同一模型执行原始完整请求。
#pragma once

#include <memory>

#include "MaiModelClient.h"

// 包装本地模型客户端；构造不做 I/O，stream 仍在调用线程阻塞，回调也同步执行。
// 有工具的请求先仅声明 tool_search 及完整名称目录。模型可直接回答，或选择工具后，
// 用未经修改的原始 system/历史和选中的原始声明再推理一次；内部选择不伪造成用户历史。
// 两次调用均使用同一取消标志，正文/思考按产生顺序交付，整个 stream 的耗时包含两次调用。
// 每次 stream 的选择状态只在栈上存在；不缓存账号、对话、KV 或发现结果。
// 只读 metadata lookup 复用 MaiLlamaToolCatalog，真实操作只通过最终 onToolCall 交给宿主。
// 未声明调用、混合发现/操作批次、无效/未完成选择和取消都不会交付操作调用；不回退到完整目录。
// 最多一次发现批次和一次实际任务推理，避免发现错误形成内部无限循环。
// client 必须非空且 wireApi() 为 Local；不满足时 stream 返回 NotConfigured/NotSupported。
std::unique_ptr<MaiModelClient> makeMaiLlamaToolDiscoveryClient(
    std::unique_ptr<MaiModelClient> client);
