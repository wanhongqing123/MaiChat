// 文件作用：在发送前把单次模型请求的编码字节适配到供应商请求体上限，并记录已学到的上限。
//
// 背景：上下文压缩按 token 触发，而 HTTP 413 是网关/供应商对请求体字节的限制。
// 图片在 token 估算里只占固定额度，线上却是数百 KB 的 base64；历史图片又随每次请求
// 重发。因此请求体可能在 token 压缩触发前就超过字节上限。
//
// 本模块只改写**本次请求副本**：存储中的原始消息、图片文件与工具输出完全不变。
// 被省略的内容在请求里留下精确回查指针（workspace_path、来源消息/片段 ID），
// 模型需要时可用 view_image / session_history / tool_output_read 取回。
#pragma once

#include <atomic>
#include <cstddef>
#include <string>

#include "MaiError.h"
#include "MaiModelClient.h"

struct MaiRequestBodyFitResult {
    MaiModelRequest request;
    // 适配前后的编码字节；measured=false 时为本地近似值（传输无法精确测量）。
    std::size_t originalBytes = 0;
    std::size_t fittedBytes = 0;
    bool measured = true;
    // 是否对请求做了任何省略。false 时 request 与输入相同。
    bool reduced = false;
    // fittedBytes <= byteLimit。
    bool fits = false;
    // 已到达保留完整正文的最小请求（基础指令、工具声明与工具必要骨架），无法继续缩小。
    bool exhausted = false;
    std::size_t omittedImages = 0;
    std::size_t truncatedTexts = 0;
    std::size_t droppedContinuations = 0;
};

// 把 request 适配到 byteLimit（编码后的 JSON 请求体字节）。byteLimit 为 0 时原样返回。
// 降级顺序（每级都只在仍超限时进行，且同级内由旧到新）：
//   1. 当前用户输入之前的历史图片 → 文本占位（保留 workspace_path）；
//   2. 当前轮较早的工具截图 → 占位（最新一组截图保留到更后面）；
//   3. 较早 assistant 消息的不透明续接状态 → 省略（回退到普通文本/调用投影）；
//   4. 工具结果、较早思考、工具参数 → 统一上限截断，保留首尾并注明回查方式；
//   5. 最新一组工具截图、当前用户附件 → 占位；
// 用户及助手正文（包括历史正文）始终逐字保留；工具可缩部分耗尽后仍超限，
// 返回 exhausted，不通过截断正文伪造 fits。宿主须明确报告容量限制。
// 不会删除消息或拆散工具调用/结果配对，因此请求结构对所有 wire 保持合法。
// 测量次数为 O(log n)，不在内层循环里反复编码。取消时返回 Canceled。
MaiResult<MaiRequestBodyFitResult> maiFitRequestBody(MaiModelClient& model,
                                                     const MaiModelRequest& request,
                                                     std::size_t byteLimit,
                                                     const std::atomic<bool>& cancel);

// 编码字节测量：优先使用传输的真实编码器；无法测量（返回 0 或错误）时使用保守近似。
// measured 输出是否为真实测量。
std::size_t maiMeasureRequestBody(MaiModelClient& model, const MaiModelRequest& request,
                                  bool* measured = nullptr);

// ── 已学到的请求体上限（进程内，按模型名）──────────────────────────
// 0 表示未知。上限只会变小。
std::size_t maiLearnedRequestBodyLimit(const std::string& model);
// 记录一次被拒的请求体字节数，返回新的上限。providerMessage 若明确写出上限
// （如 "Maximum size is 52428800 bytes"）且小于被拒字节，直接采用该值并留 2% 余量；
// 否则收紧到被拒字节的 keepPercent%（1..99）。结果总是严格小于 rejectedBytes。
std::size_t maiLearnRequestBodyRejection(const std::string& model, std::size_t rejectedBytes,
                                         const std::string& providerMessage = {},
                                         unsigned int keepPercent = 85);
// 从供应商错误文本中解析明确写出的请求体上限；没有则返回 0。
std::size_t maiParseStatedRequestBodyLimit(const std::string& providerMessage);
// 测试专用：清空已学到的上限。
void maiResetLearnedRequestBodyLimits();
