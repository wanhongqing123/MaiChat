// 文件作用：提供旧子 Agent 宿主的可选类型化结果接口默认行为，不猜测其执行事实。
#include "MaiSubAgent.h"

MaiResult<MaiTurnOutcome> MaiSubAgentHost::subAgentOutcome(const std::string&,
                                                           const std::string&) const {
    return {MaiErrorCode::NotSupported, "This host does not record turn outcomes."};
}

MaiResult<std::string> MaiSubAgentHost::subAgentReportForTurn(const std::string&,
                                                              const std::string&,
                                                              const std::string&) const {
    return {MaiErrorCode::NotSupported, "This host does not support recorded turn reports."};
}
