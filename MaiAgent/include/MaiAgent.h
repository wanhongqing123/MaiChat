// 文件作用：组织 Agent 的公开操作、事件流和会话执行入口。
// 对外类型：MaiCreateSession、MaiUpdateSession、MaiDeleteSession、MaiClearMessages；线程、失败和生命周期以类型附近注释为准。
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "MaiError.h"
#include "MaiEventBus.h"
#include "MaiMessage.h"
#include "MaiModelClient.h"
#include "MaiConversationHistory.h"
#include "MaiPermission.h"
#include "MaiQuestion.h"
#include "MaiSubAgent.h"
#include "MaiSession.h"
#include "MaiSessionStore.h"
#include "MaiTool.h"
#include "MaiToolDetail.h"

// ── 能提交给核心的操作 ──────────────────────────────────────────
// 形状抄 codex（它的 core 对外就 submit(Op) / next_event() 两个方法）。
// 做成一个封闭的 variant 而不是一堆方法，是为了让"核心能做什么"可枚举：
// 加能力时编译器会强制每个 visit 都处理到，不会有人忘了改某个分支。
struct MaiCreateSession {
    std::string directory;
    std::string title;
    std::string model;
    // 空串继承宿主默认；显式值须为 low/high/max。追加末尾保留原三字段聚合初始化。
    std::string reasoningEffort;
};

struct MaiUpdateSession {
    std::string sessionId;
    // 空表示不改；非空须通过 maiValidateSessionTitle，并持久标记为显式名称。
    // 即使用户名称恰好等于占位标题，自动命名也不会再覆盖它。
    std::string title;
    std::string model;
    std::string agent;
    // 移动端更新后，应用沙箱的绝对路径可能改变。调用方传入当前工作目录时，
    // 必须同步修正持久化会话，否则历史图片仍会从旧容器路径读取。
    std::string directory;
    // nullopt 不修改；有值但空串恢复继承；非空仅 low/high/max。
    // 与 model/title 等同一次校验和写入；非法值不产生部分更新。
    // 修改 model/effort 时目标活动轮次返回 Busy，改名/修正目录保持原语义。
    // 追加末尾保留原五字段聚合初始化。
    std::optional<std::string> reasoningEffort;
};

struct MaiDeleteSession {
    std::string sessionId;
};

// 清掉这个会话的聊天记录，**会话本身留着**。
//
// 为什么不用"删了再建一个"顶替：
//
//   1. 会换 id。界面上只有一个固定的 AI 助手会话，换 id 意味着它要重新去认。
//   2. **会把"本会话都允许"那份授权记录一起丢掉**（MaiDeleteSession 里明确清了它）。
//      用户点"清空重来"想丢的是聊天记录，不是他刚给过的授权——而这件事他感觉不到，
//      只会发现它又开始一个一个问了。
//
// 正在跑的时候不让清（返回 Busy）：那一轮的 assistant 消息还在往库里写，
// 清了它下一次 putMessage 又会把消息塞回来，最后剩一条来历不明的半截记录。
// 界面应该先 MaiInterrupt，再清。
struct MaiClearMessages {
    std::string sessionId;
};

// 发一轮消息。**立刻返回**，真正的输出全部走事件流。
struct MaiSendPrompt {
    std::string sessionId;
    std::string text;
    std::vector<MaiModelImage> images;
    std::vector<MaiVideoPart> videos;
    std::string quotedMessageId;

    MaiSendPrompt(std::string sessionId, std::string text);
    MaiSendPrompt(std::string sessionId, std::string text, std::vector<MaiModelImage> images);
    MaiSendPrompt(std::string sessionId, std::string text, std::vector<MaiModelImage> images,
                  std::vector<MaiVideoPart> videos);
    MaiSendPrompt(std::string sessionId, std::string text, std::vector<MaiModelImage> images,
                  std::vector<MaiVideoPart> videos, std::string quotedMessageId);
};

// 补充指定的正在运行轮次，不中断模型传输或当前工具，也不另起一轮。
// turnId 必须是该轮最初的 assistant ID（activeTurnId），后续 assistant 分段不改变它。
// requestId 是调用方生成的 1..128 字节 ASCII 标识，同一次提交重试必须复用。
// 成功返回已落库的 user 消息 ID；当前工具结束后即暂停尚未执行的同批调用，
// 补齐“未执行”回执后把消息带入下次模型请求，由模型决定后续工具。
// 同 sessionId + turnId + requestId 的相同内容重试返回原 ID；改内容返回 InvalidInput。
// 最近 128 份接收凭据在本 Agent 生命周期内保留，活动轮自身另保留全部凭据。
// 每轮至多接受 64 条补充，文本至多 64 KiB，图片/视频各至多 10 项。
// 空输入、失配的 turnId 返回 InvalidInput；idle 返回 NotFound；停止中返回 Canceled；
// 已封闭输入的收尾阶段返回 Busy。拒绝时宿主须保留草稿，不得自动回退成 MaiSendPrompt。
// 停止或模型失败后，已接受消息仍保留历史，但不会为它们自动再启动模型请求。
struct MaiSteerPrompt {
    std::string sessionId;
    std::string turnId;
    std::string requestId;
    std::string text;
    std::vector<MaiModelImage> images = {};
    std::vector<MaiVideoPart> videos = {};
    std::string quotedMessageId = {};
};

// 接收会话输入而不要求调用方观察 busy/turnId。先持久化用户消息，再返回 user ID；
// 活动轮在当前工具结束后消费，尚未执行的同批调用交给模型结合新输入重新判断。
// 正在运行的工具或模型流不被强制中断；收尾时收到的输入由下一轮按序消费。
// requestId 是调用方稳定的 1..128 ASCII 标识；同会话同编号同内容重试返回原 ID，
// 改内容报 InvalidInput。正文/媒体边界与 MaiSteerPrompt 相同，最多保留 256 条未消费输入。
// 停止暂停排队执行但保留内容；下一次 enqueue/重试恢复。退出不自动重放模型请求，
// 尚未消费的输入持久保留，下一次该会话 enqueue 时恢复。成功只代表接收，不代表完成。
// 删除会话或清空历史会同时结束该批接收凭据；调用方不得重送已明确清除的旧输入。
struct MaiEnqueuePrompt {
    MaiSendPrompt prompt;
    std::string requestId;
};

// 中断正在跑的那一轮。
struct MaiInterrupt {
    std::string sessionId;
};

// 对一次工具调用的授权请求作出裁决。
//
// 做成操作而不是直接调闸门，是为了让它和别的变更走同一条路：一样的错误码、一样的返回形状，
// 调用方不用为它单开一套处理。
struct MaiReplyPermission {
    std::string permissionId;
    MaiPermissionDecision decision = MaiPermissionDecision::Denied;
};

// 回答模型中途问的那句话。
//
// 和裁决授权分开而不是共用一个「回复请求」：一个的答案是枚举、另一个是自由文字，
// 合成一个操作的话调用方得先判断类型才知道该填哪个字段。
struct MaiReplyQuestion {
    std::string questionId;
    std::string answer;
};

using MaiOperation = std::variant<MaiCreateSession, MaiUpdateSession, MaiDeleteSession,
                                  MaiClearMessages, MaiSendPrompt, MaiSteerPrompt, MaiEnqueuePrompt,
                                  MaiInterrupt, MaiReplyPermission, MaiReplyQuestion>;

// ── 核心的门面 ──────────────────────────────────────────────────
//
// 它只做四件事：接收操作、转发查询、持有依赖、暴露事件流。
// **跑一轮对话的逻辑不在这里**——那在 MaiTurnRunner，上下文组装在 MaiContextBuilder，
// 给会话起名在 MaiSessionTitler。
//
// Qt 桌面、iOS / Android、命令行都只认这一个类。
// 这里不出现任何 HTTP 或 JSON 的概念——这是"库是边界"的落点。
//
// 这条边界已经受过一次检验：给 Electron 界面用的那层 REST + SSE 适配器整个删掉时，
// 这个文件一行都没改。
//
// ── 怎么用 ──────────────────────────────────────────────────────
//
//   auto store = makeMaiMemoryStore();               // 或 makeMaiSqliteStore(path)
//   MaiModelConfig config;
//   config.baseUrl = "https://open.bigmodel.cn/api/paas/v4";
//   config.apiKey  = apiKey;
//   auto tools = std::make_unique<MaiToolRegistry>();
//   registerMaiBuiltinTools(*tools);
//
//   MaiAgent agent(std::move(store), makeMaiModelClient(config), std::move(tools));
//
//   // 先订阅，再发消息——否则会漏掉最前面的事件
//   agent.eventBus().subscribe([](const MaiEvent& event) { /* 刷界面 */ });
//
//   const std::string sessionId = agent.submit(MaiCreateSession{workdir, "", ""}).value();
//   agent.submit(MaiSendPrompt{sessionId, "你好"});   // 立刻返回，不等模型
//
// ── submit 是异步的 ─────────────────────────────────────────────
//
// **这是用这个类最容易搞错的地方。** MaiSendPrompt 提交之后立刻返回，
// 返回的只是"新建的 assistant 消息 id"，不是回答。
// 真正的输出全部通过事件流出来——模型每吐一点就是一条 MessagePartDelta。
//
// 同步等模型答完会让界面卡几十秒，所以这一层不提供那种接口。测试里要等结果用 waitIdle()。
//
// ── 线程 ────────────────────────────────────────────────────────
//
// **所有公开方法都可以从任意线程调用**，内部自己加锁。
//
// 每一轮对话跑在**自己的工作线程**上（一轮一个），所以：
//   - 多个会话真的并行，一个卡在等授权不影响别的；
//   - 跑着的时候照样可以查询（listMessages 等）；
//   - 事件处理函数在**发布事件的那个线程**上同步跑，流式期间那是网络
//     读线程——里面不要做慢活，有守卫盯着（见 MaiBlockingCheck.h）。
//
// ── 析构 ────────────────────────────────────────────────────────
//
// 析构会把所有在跑的轮次叫停并**等它们退出**，所以析构可能阻塞若干秒（要等 HTTP 传输真的断掉）。
// 不这么做的话工作线程会去访问已经销毁的 store 和 emitter。
//
// 推论：**事件订阅者必须活得比 MaiAgent 久**，否则析构过程中最后几条事件会调到悬空的处理函数上。
class MaiAgent final : public MaiSubAgentHost {
public:
    struct Options {
        // 会话没指定模型时用这个。会话上配了就用会话的（MaiUpdateSession）。
        std::string defaultModel = "glm-5.3";
        // 会话 effort 为空时的宿主默认。空串保留供应商默认；显式值仅 low/high/max。
        std::string reasoningEffort;
        // 可选宿主默认提供器，仅在轮次开始且会话没有显式值时调用一次。
        // 本轮（包括工具跟进/压缩）固定该快照，不随其他会话或 UI 选择变化。
        // 回调在 Agent 工作线程同步调用，须线程安全且活到 Agent 析构。
        std::function<std::string()> reasoningEffortProvider;
        // 模型可以连着调工具，一轮对话因此会有多次请求。
        // 相同调用与结果重复三次会提前停止；这里是不同操作也持续进行时的最终兜底。
        int maxToolIterations = 64;
        // 每个会话同时只跑一轮。第二条来了直接拒绝而不是排队——排队会让用户以为消息丢了，
        // 界面上看不出区别。
        bool rejectWhenBusy = true;
        // 等用户授权的超时。0 = 无限等，理由见 MaiPermissionGate::Options。
        MaiMillis permissionTimeoutMs = 0;
        MaiApprovalPolicy approvalPolicy = MaiApprovalPolicy::OnRequest;
        // 相对路径仍以各会话目录为基准。留空时该目录就是文件访问边界；
        // 移动宿主可以扩大到自身 App 容器。
        std::string fileAccessRoot;
        // 桌面宿主可以把路径访问交给操作系统权限控制。
        // 无论该设置如何，修改操作仍单独经过审批策略。
        bool allowOutsideWorkingDirectory = false;
        std::function<MaiResult<std::string>(const std::string&, const std::string&)> decodeText;
        // 模型的基础指令。和 Codex 的 base_instructions 一样独立于消息历史，空表示不注入。
        std::string baseInstructions;

        // 子 Agent 最多能套多少层。根会话是 0，所以 2 表示「孙子辈就到头了」。
        //
        // **必须有上限。** 不封的话模型能把自己 fork 到爆：每一层都觉得
        // 「这活该交出去」，而每一层都在烧钱。codex 也是这么卡的。
        int maxSubAgentDepth = 2;
        // 一棵会话树里同时开着的子 Agent 上限。收掉的不算。
        int maxOpenSubAgents = 8;
    };

    // 三个依赖**都被接管所有权**，活到 MaiAgent 析构为止。
    //
    // store 不能为空。model 可以为空：那样发消息会以 NotConfigured 收场，但建会话、
    // 查历史这些照常（M1 的空转骨架就是这个配置）。tools 可以为空：那是纯对话模式，
    // 模型收不到任何工具声明。
    MaiAgent(std::unique_ptr<MaiSessionStore> store, std::unique_ptr<MaiModelClient> model,
             std::unique_ptr<MaiToolRegistry> tools = nullptr);
    MaiAgent(std::unique_ptr<MaiSessionStore> store, std::unique_ptr<MaiModelClient> model,
             std::unique_ptr<MaiToolRegistry> tools, Options options);
    ~MaiAgent();
    MaiAgent(const MaiAgent&) = delete;
    MaiAgent& operator=(const MaiAgent&) = delete;

    // ── 查询 ────────────────────────────────────────────────────
    // 纯读，直接问存储，不经过操作队列。轮次跑着的时候也能查，
    // 查到的是**那一刻已经落库的内容**（流式期间 assistant 消息会随着每次工具调用逐步变长）。

    // 按 updated 倒序——界面左侧列表直接用这个顺序。
    std::vector<MaiSession> listSessions() const;
    // 会话不存在返回 false，out 不动。
    bool getSession(const std::string& id, MaiSession& out) const;
    // 按生成顺序。会话不存在时返回空 vector，和"会话存在但没有消息"分不开——要分清先用 getSession。
    std::vector<MaiMessage> listMessages(const std::string& sessionId) const;
    // 按新到旧读取一个会话的一页已落库消息。空 beforeId 从最新开始，非空游标
    // 不包含自身；limit 为 0 或会话不存在时返回空列表。纯读，可与运行中的轮次
    // 并发调用；返回值归调用方持有。只用于界面分页，不改变模型轮次读取完整历史。
    std::vector<MaiMessage> listMessagesPage(const std::string& sessionId,
                                             const std::string& beforeId, std::size_t limit) const;
    // 精确读取所属会话的一条已落库消息。纯读、可与活动轮次并发；
    // 不存在、归属不符或读取失败返回 false 且 out 不动，成功结果由调用方持有。
    // 不改变模型上下文，也不生成操作、事件或新的模型请求。
    bool getMessage(const std::string& sessionId, const std::string& messageId,
                    MaiMessage& out) const;
    // 普通原史回查，和 session_history 模型工具复用同一实现。仅查询传入的已授权会话，
    // 精确消息/片段/字段读取和字面检索均返回原文来源及分页偏移，不调用模型或读用户媒体。
    // 失败返回 NotFound/InvalidInput；可与活动轮次并发。宿主负责其会话授权边界。
    MaiResult<MaiConversationHistoryPage> readHistory(
        const std::string& sessionId, const MaiConversationHistoryQuery& query) const;
    // 版本绑定的工具详情分页，复用普通只读入口；只取已落库的单条消息快照，不执行原工具。
    // Busy表示详情版本变化，须首屏重读；其余身份、预算与UTF-8边界见MaiToolDetail.h。
    MaiResult<MaiToolDetailPage> readToolDetail(const std::string& sessionId,
                                                const MaiToolDetailQuery& query) const;
    // 这个会话现在有没有一轮在跑。注意这是**那一瞬间**的答案，
    // 拿它去做"没跑就发消息"的判断是有竞态的——直接 submit，忙的话会返回 Busy，那个判断在锁里做。
    bool isBusy(const std::string& sessionId) const;
    // 正在运行轮次的稳定 ID；idle 返回空。捕获后仍须在 MaiSteerPrompt 中回传核对，
    // 不能先查 isBusy 再把补充消息投给此后恰好开始的另一轮。
    std::string activeTurnId(const std::string& sessionId) const;
    bool setApprovalPolicy(MaiApprovalPolicy policy);
    MaiApprovalPolicy approvalPolicy() const;

    // 现在有哪些工具调用在等授权。
    //
    // 界面重连之后必须能补上这一份：SSE 断开的那个窗口期里发出的 permission.asked 是看不到的，
    // 没有这个查询，那一轮会一直挂着，而界面上什么都没显示。
    std::vector<MaiPermissionRequest> listPendingPermissions() const;

    // 还在等回答的提问。界面刷新后靠它重新摆出输入框——
    // 没有这个，断线重连窗口期里发出的提问就永远看不见了，那一轮会一直挂着。
    std::vector<MaiQuestionRequest> listPendingQuestions() const;

    // ── 子 Agent（MaiSubAgentHost）──────────────────────────────
    //
    // 这几个是给 spawn_agent 那组工具用的，不是给界面用的。界面要看子 Agent
    // 的话走 listSessions()，那里已经把非根会话滤掉了。
    MaiResult<std::string> spawnSubAgent(const std::string& parentSessionId,
                                         const std::string& taskName,
                                         const std::string& prompt) override;
    MaiError sendToSubAgent(const std::string& parentSessionId, const std::string& childSessionId,
                            const std::string& prompt) override;
    bool waitForSubAgent(const std::string& parentSessionId, const std::string& childSessionId,
                         MaiMillis timeoutMs, const std::atomic<bool>& cancel) override;
    std::vector<MaiSubAgentInfo> listSubAgents(const std::string& parentSessionId) override;
    MaiError closeSubAgent(const std::string& parentSessionId,
                           const std::string& childSessionId) override;
    std::string subAgentReport(const std::string& childSessionId) override;

    // ── 变更 ────────────────────────────────────────────────────

    // 提交一个操作。**立刻返回**，见上面"submit 是异步的"。
    //
    // 成功时返回受影响的对象 id：建会话返回 ses_...，
    // 发消息返回新建的 assistant 消息 id（msg_...），裁决授权返回 per_...。
    //
    // 失败时带错误码，调用方据此分支：NotFound（会话不存在）、Busy（这个会话已经有一轮在跑）、
    // InvalidInput（空 prompt 等）。界面据此决定是弹错误、还是提示"先等这一轮跑完"。
    MaiResult<std::string> submit(const MaiOperation& operation);

    // 中断指定活动轮次。非空 expectedTurnId 必须等于该会话最初的 assistant ID；
    // 查找、比较、设置取消及唤醒审批/提问在同一 Runtime 锁内完成，避免旧 Stop 误取消新轮。
    // 空 expectedTurnId 保留 MaiInterrupt 的“中断当前轮”语义；两入口共用同一实现。
    // 会话无活动轮次或轮次已变化返回 NotFound，不改其他轮次的取消/审批/提问状态。
    // 线程安全、同步受理后返回 sessionId；不等待模型、工具或网络工作线程退出。
    // 与普通操作相同，调用方须保证 MaiAgent 生命周期覆盖本次调用；不改公开 C ABI。
    MaiResult<std::string> interruptTurn(const std::string& sessionId,
                                         const std::string& expectedTurnId);

    // 等所有在跑的轮次结束。给测试和优雅退出用。
    void waitIdle();

    MaiEventBus& eventBus();
    const MaiEventBus& eventBus() const;

private:
    void pollSpecialistTasks();
    void forwardSpecialistReply(const MaiSpecialistTask& task);
    // pimpl。实现体叫 Runtime 而不是 Implementation：后者任何一个 pimpl 类都能叫，等于没说。
    // 这个名字说的是它装什么——运行时状态：依赖、事件管线、正在跑的轮次。
    //
    // 前向声明必须跟着写 struct（实现体内部全公开），否则 MSVC 报 C4099。
    struct Runtime;
    std::unique_ptr<Runtime> mRuntime;
};
