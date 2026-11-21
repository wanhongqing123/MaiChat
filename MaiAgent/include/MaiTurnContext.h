// 文件作用：保存单进程执行轮次的身份快照和可观察阶段。
#pragma once
#include <string>

// 阶段只是进度，不创建新 turn，也不代替 SessionIdle/SessionError 终态。
enum class MaiTurnPhase {
    PreparingContext,
    Compacting,
    Sampling,
    ExecutingTools,
    Recovering,
    Finalizing
};
const char* maiTurnPhaseToString(MaiTurnPhase phase);

// 内部值对象：开始时固定型号、强度和工作目录，工具、摘要及重试共用同一身份。
// 不持有 UI、线程、凭据或网络资源；不得在流回调中重新读取可变会话配置。
struct MaiTurnContext {
    std::string sessionId;
    std::string turnId;
    std::string model;
    std::string reasoningEffort;
    std::string workingDirectory;
};
