// 文件作用：定义一轮执行的持久状态，和响应正文、消息分段结束及工具状态分别建模。
#pragma once

#include <string>

#include "MaiError.h"
#include "MaiTime.h"

// 只记录执行事实；旧消息没有此元数据时是未知，不能按有无正文补造成功。
enum class MaiTurnStatus { Running, Completed, Failed, Interrupted };

const char* maiTurnStatusToString(MaiTurnStatus status);

// 值快照，归调用方持有，可跨线程传递。由核心写在初始 assistant 消息上；
// turnId 始终为初始 assistant ID，steering 后结果消息可以有不同 ID。
// Completed 可以没有正文；Failed/Interrupted 可保留半截正文，均不能当成完整成功。
// Running 的 completed=0；进程重建时遗留 Running 被标为 Interrupted，不重放工具或请求。
// 错误使用现有类型，不携带供应商原始响应或思考；此记录本身不进入模型上下文。
struct MaiTurnOutcome {
    std::string turnId;
    MaiTurnStatus status = MaiTurnStatus::Running;
    std::string resultMessageId;
    MaiMillis completed = 0;
    MaiError error;
};
