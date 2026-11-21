// 文件作用：提供轮次阶段的稳定字符串，不依赖界面或传输协议。
#include "MaiTurnContext.h"
const char* maiTurnPhaseToString(MaiTurnPhase phase) {
    switch (phase) {
        case MaiTurnPhase::PreparingContext: return "preparing_context";
        case MaiTurnPhase::Compacting: return "compacting";
        case MaiTurnPhase::Sampling: return "sampling";
        case MaiTurnPhase::ExecutingTools: return "executing_tools";
        case MaiTurnPhase::Recovering: return "recovering";
        case MaiTurnPhase::Finalizing: return "finalizing";
    }
    return "preparing_context";
}
