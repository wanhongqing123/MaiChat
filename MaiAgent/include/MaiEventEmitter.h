// 文件作用：构造和发布 Agent 执行过程中的事件。
// 对外类型：MaiEventEmitter；线程、失败和生命周期以类型附近注释为准。
#pragma once
#include <string>
#include <string_view>

#include "MaiEventBus.h"
#include "MaiError.h"
#include "MaiPermission.h"
#include "MaiQuestion.h"
#include "MaiTime.h"

// 把"构造事件并发布"这件事收在一处。
//
// 之前 MaiAgent::Runtime 里散着四个发事件的私有方法，每个都在手工填
// id/time/type 那几个字段——加一种事件就要再抄一遍，抄漏一个字段不会编译报错，
// 只会在界面上表现成某些更新丢了。
//
// 方法名都以 emit 开头：不带的话 `emitter->session(...)` 读起来
// 像在取 session，而它其实是在发事件。
class MaiEventEmitter {
public:
    explicit MaiEventEmitter(MaiEventBus& bus);

    // field 仅用于 SessionUpdated；缺省空值表示未分类，宿主必须按未知更新撤销旧授权。
    // 标题须显式传 title，输入受理传 input，工作区变更传 workspace；其它变更传空值或明确类别，
    // 不得伪装成标题/输入更新，否则宿主会错误保留旧工作区授权。
    void emitSession(MaiEventType type, const std::string& sessionId,
                     const std::string& detail = {}, const std::string& field = {});

    void emitMessage(MaiEventType type, const std::string& sessionId, const std::string& messageId);
    // 用户/assistant 已落库且活动轮次已登记后、工作线程启动前发布 running 状态。
    // 不携带推理内容；宿主按准确 turn ID 展示等待状态，不能等模型首段才发现任务已开始。
    void emitTurnStarted(const std::string& sessionId, const std::string& turnId);
    // 恢复期间任务仍在运行；独立 ModelRequestRetrying 事件承载宿主进度。
    // 不产生成功提交、工具调用或 idle，准确绑定原轮次。
    // 发布同一轮的内部阶段，不改变输入闸门和终态。
    void emitTurnPhase(const std::string& sessionId, const std::string& turnId, MaiTurnPhase phase);
    void emitModelRetry(const std::string& sessionId, const std::string& turnId, int retry,
                        int limit, long delayMs);
    // 结束事件保留准确轮次，宿主不得用旧 idle 清理替代轮次的跨调用资源。
    void emitTurnIdle(const std::string& sessionId, const std::string& turnId);
    // 轮次失败必须携带最初的 turn ID，宿主不能把新轮次的失败归给仍在发送的旧回复。
    // 不改变普通会话错误的缺省语义；未知归属的错误不得由宿主猜测轮次。
    void emitTurnError(const std::string& sessionId, const std::string& turnId,
                       const std::string& detail);

    // Runner 在落库后传最初的 turn ID，后续 assistant 分段不改变该身份。
    // 旧调用缺省为空，不可据此推测活动轮次；宿主仍须读取准确的 typed part，
    // 不把工具参数、推理或未提交正文当成媒体/正文交付。同步发布不增加 I/O。
    void emitPart(MaiEventType type, const std::string& sessionId, const std::string& messageId,
                  const std::string& partId, const std::string& turnId = {});

    // 只能在完整成功响应的正文落库后调用；不改变轮次/工具状态，不携带正文副本。
    void emitTextCommitted(const std::string& sessionId, const std::string& turnId,
                           const std::string& messageId, const std::string& partId);

    // 新 assistant 及用户消费状态落库后、普通 MessageUpdated 前调用；ID 顺序不能改写。
    void emitSteeringApplied(const std::string& sessionId, const std::string& turnId,
                             const std::string& messageId,
                             const std::vector<std::string>& inputMessageIds);

    // 增量。**只带这次新增的内容**，不重推全量——流式期间每秒几十条，全量重推是纯烧 CPU。
    void emitDelta(const std::string& sessionId, const std::string& messageId,
                   const std::string& partId, const char* field, std::string_view chunk);

    // 有一次工具调用在等用户点头。partId 指向那个工具 part，界面从 part 上就能拿到工具名和参数，
    // 不用这个事件再带一份。
    void emitPermissionAsked(const MaiPermissionRequest& request);

    // 裁决落地了。界面据此把对话框关掉——**包括不是它发起的那次裁决**，
    // 多端同时开着的时候这一条是唯一的同步手段。
    void emitPermissionReplied(const MaiPermissionRequest& request, MaiPermissionDecision decision);

    // 模型问了用户一句，正等着回答。
    void emitQuestion(const std::string& sessionId, const MaiQuestionRequest& request);

    // 回答落地了。界面据此把输入框收掉——**包括不是它发起的那次回答**，
    // 多端同时开着的时候这一条是唯一的同步手段。
    void emitQuestionAnswered(const MaiQuestionRequest& request);

private:
    MaiEventBus& mBus;
};
