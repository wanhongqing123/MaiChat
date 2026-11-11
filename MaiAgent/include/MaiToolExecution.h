// 文件作用：定义工具执行与恢复的可信事实，独立于工具原始输出及界面状态。
#pragma once

#include <optional>
#include <string>

// 描述执行本身；旧界面 MaiToolState 的持久化数字不受此枚举影响。
// Unknown 表示宿主无法确定执行结果，不能将错误或中断解释成没有产生副作用。
enum class MaiToolExecutionStatus { NotStarted, Running, Completed, Failed, Canceled, Unknown };

// 只有宿主或工具已观察到的事实才能声明 None 或 Confirmed。
// Completed 与非零退出码都不能证明命令没有修改外部状态。
enum class MaiToolEffectStatus { None, Unknown, Confirmed };

// 下一步是交给主模型的建议，不会自动调用工具或重新执行原操作。
enum class MaiToolNextAction {
    None,
    ReadOutput,
    InspectState,
    QueryStatus,
    ReviseArguments,
    AskUser
};

// 纯值对象，可随工具结果和消息片段复制；不持有线程、存储或供应商资源。
// started/exitCode 无值与 false/0 含义不同：前者表示没有足够证据。
// outputId/taskId 只能来自实际已保存输出或已接受任务，不能从模型参数臆造。
// outputTruncated 描述初始结果省略，或执行未完成导致捕获不完整；保留输出另行说明完整性。
struct MaiToolExecution {
    MaiToolExecutionStatus status = MaiToolExecutionStatus::Unknown;
    MaiToolEffectStatus effectStatus = MaiToolEffectStatus::Unknown;
    MaiToolNextAction nextAction = MaiToolNextAction::None;
    std::optional<bool> started;
    std::optional<int> exitCode;
    bool timedOut = false;
    bool canceled = false;
    bool outputTruncated = false;
    std::string outputId;
    std::string taskId;
    std::string recoveryTool;
};

// 稳定字符串边界用于消息存储和上下文，不向公开头泄露序列化库类型。
// 序列化不修改输入，允许并发调用；缺少的可选事实不写入结果。
std::string maiSerializeToolExecution(const MaiToolExecution& execution);

// 只接受已知枚举和正确字段类型；失败不改变 out，也不从工具输出推测执行事实。
// 允许额外字段便于增加非破坏字段；函数不访问文件、网络或共享状态。
bool maiParseToolExecution(const std::string& source, MaiToolExecution& out);

// 内部兼容入口：保留已有结构化供应商错误的修复建议，供独立宿主观察使用。
// 返回值不是执行事实；调用方必须先按可信 execution 处理已接受任务和取消。
std::string maiToolFailureRecoveryGuidance(const std::string& tool, const std::string& error,
                                           bool executionUncertain = false);
