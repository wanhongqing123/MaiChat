// 文件作用：定义模型请求、流式响应和客户端接口契约。
// 对外类型：MaiWireApi、MaiToolInvocation、MaiModelRole、MaiModelImage；线程、失败和生命周期以类型附近注释为准。
#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "MaiError.h"

// ── 中立的模型抽象 ──────────────────────────────────────────────
// 这里的结构**不是** OpenAI 的线格式，是我们自己的中间表示。
// 网络 wire 实现（Chat Completions / Responses）负责翻译供应商请求/响应，
// 本地实现负责 GGUF 原生聊天模板与输出解析；它们共享完整历史、流式和工具调用契约。
//
// 之前这一层直接长成了 OpenAI 的形状（tool_calls 原样照搬），那样加第二个供应商时要么污染这个头，
// 要么在上层写一堆 if。

enum class MaiWireApi {
    ChatCompletions,
    Responses,
    // 进程内模型复用同一消息/流式/工具契约，没有 HTTP 端点或 API Key。
    // 宿主必须通过 makeMaiLlamaModelClient 装配本地文件配置，远端工厂拒绝此项。
    Local,
};

const char* maiWireApiToString(MaiWireApi wire);

// 模型发起的一次工具调用。
//
// **攒完整才交付**：流式过程中 arguments 可能是一个个 JSON 碎片到达的
// （见 MaiStreamSink::onToolCall 的说明），中途给出去会是半截 JSON。
//
// 注意"可能"两个字——**分不分片是供应商的自由，不是协议的规定**。
// 实测过 GLM-5.3（zhipuai coding plan）：tool_calls 永远只有一帧，arguments 整块给，
// 连 1.2 KB 的长参数也不切。所以拿 GLM 跑通**不等于**这段聚合验过了，它根本没被执行到。
//
// 真正证明这段逻辑的是 MaiModelClientTests：
// 那里喂的是对抗性分片——切点故意落在 JSON 的引号和冒号中间，而且从 chunk=1 开始每个块大小都跑一遍，
// 比任何真实供应商都狠。换供应商时别把这组用例删了。
struct MaiToolInvocation {
    // 服务端给的调用 id。回灌结果时要**原样带回**，对不上的话模型认不出这是哪次调用的结果，
    // 下一轮会把同样的工具再调一遍。
    std::string id;
    std::string name;  // 工具名，要能在 MaiToolRegistry 里查到
    // 参数的 JSON 原文。核心**不解析**它，交给工具实现去解——这样加新工具不用动这一层的任何代码。
    //
    // 模型有时会给出不合法的 JSON（尤其是被截断时），所以工具实现解析失败要当成"参数不对"返回错误，
    // 不能崩。
    std::string arguments;
};

// 发给模型的一条消息里，说话的是谁。
//
// 比 MaiRole（只有 User / Assistant）多两个，因为这是**模型看到的视角**：
// 临时的系统指令和工具结果在协议里都是独立消息，而在我们的领域模型里前者根本不存历史、
// 后者是 assistant 消息里的一个片段。稳定的基础指令单独放在 MaiModelRequest::baseInstructions。
enum class MaiModelRole {
    System,  // 某一圈临时追加的系统指令，不属于对话历史
    User,
    Assistant,
    ToolResult,  // 一次工具调用的结果。Chat 是 role="tool"；Responses 是 function_call_output。
};

// 发给模型的一条消息。
//
// **刻意和领域模型的 MaiMessage 分开**：MaiMessage 是"我们怎么存"，这个是"模型怎么看"，
// 两者演化节奏不一样。比如 reasoning 片段存着但不回灌、
// 工具结果存成片段但发出去是独立消息——如果共用一个类型，
// 这些差异就得靠"某些字段在某些场景下不填"来表达，很快就没人说得清了。
//
// 谁来填：MaiContextBuilder 把历史 MaiMessage 翻成这个。谁来读：
// 各个 wire 实现（现在只有 MaiOpenAiClient）翻成自家的线格式。
struct MaiModelImage {
    // 相对路径必须在工作目录内；绝对路径只允许宿主在用户明确选择附件后传入，
    // 或由已获批准的宿主工具创建。线格式实现负责读取并编码。
    std::string path;
    std::string mimeType;
};

struct MaiModelMessage {
    MaiModelRole role = MaiModelRole::User;
    // 正文。assistant 发起工具调用的那条可以是空的（那时候 invocations 非空），
    // 线格式里对应 content: null。
    std::string content;
    // Responses 线协议可把同一轮原有的思考内容作为 reasoning item 回传。
    // Chat Completions 忽略此字段，避免把草稿混入可见正文。
    std::string reasoning;
    std::vector<MaiModelImage> images;
    // ToolResult 时**必填**：这条结果对应哪次调用。Chat 线上字段名为 tool_call_id，
    // Responses 使用 call_id；两种格式都必须原样配对，见 MaiModelClientTests。
    std::string toolCallId;
    // Assistant 发起调用时填。一条消息可以同时发起多个调用。
    std::vector<MaiToolInvocation> invocations;
};

// 一个工具对模型的完整声明。模型靠它决定什么时候调、怎么调。
//
// description 和 parametersJson 是**给模型看的提示词**，不是给人看的文档：写得含糊模型就会用错，
// 写得啰嗦就白烧 token。每次请求都会原样发过去。
struct MaiToolSpec {
    std::string name;
    std::string description;
    // JSON Schema 原文。
    // 这一层不校验它合不合法——序列化时解析失败会退化成空对象（见 MaiOpenAiClient 的 buildRequestBod
    // y）。
    std::string parametersJson;
};

// 一次请求的全部内容。每一轮对话会发好几次——模型每调一次工具就要重新问一遍，
// 所以 messages 会越滚越长（见 MaiContextBuilder 对增量缓存的说明，那是一号性能风险）。
struct MaiModelRequest {
    // 模型名。会话上配了就用会话的，否则用 MaiAgent::Options::defaultModel。
    std::string model;
    // Empty uses the provider default. DeepSeek accepts low, high, or max.
    // Wire clients omit unsupported values rather than sending invalid API parameters;
    // this value affects this request only and never changes stored conversation state.
    std::string reasoningEffort;
    // 模型的基础指令。它独立于历史：Chat Completions 在线协议边界转成首条 system 消息，
    // Responses 实现应直接映射到顶层 instructions 字段。
    std::string baseInstructions;
    // 相对图片路径从这个目录解析，且不能越过它。桌面端由用户明确选择的图片可以传绝对路径，
    // 因而允许位于工作区之外；移动端应先复制进自己的稳定沙盒目录，再传相对路径。
    std::string workingDirectory;
    // 按时间顺序的完整有效上下文：原文，或已成功持久化的历史摘要与尚未覆盖的原文。
    // 协议无状态，每次都必须带上这份上下文；不能只传新消息，也不能无摘要地丢弃旧消息。
    std::vector<MaiModelMessage> messages;
    // 这一轮允许模型用的工具。空表示纯对话模式，
    // 线上会**整个省掉 tools 字段**而不是发一个空数组（有的服务端见到空数组会报错）。
    std::vector<MaiToolSpec> tools;
    // < 0 表示不传这个字段，用服务端默认。不用 0 当哨兵：
    // 0 是一个合法且有意义的取值（完全确定性输出）。
    double temperature = -1.0;
    // 0 使用供应商默认值；非零是本次生成的 token 上限（包含供应商计入的推理）。
    // 压缩器以同一个值预留输出空间，wire 层负责映射到真实协议字段。
    std::size_t maxOutputTokens = 0;
};

// 模型目录的真实容量，不是历史字节上限。0 表示未知，不能擅自套用其他模型的窗口。
// 宿主/配置提供器须按实际 endpoint 与选中模型查询；本地实现使用已配置的 contextSize。
struct MaiModelContextLimits {
    std::size_t contextWindowTokens = 0;
    std::size_t maxOutputTokens = 0;
    // 仅用于输入预算的可选生成预留，不写入 wire、不会限制实际生成长度。
    // 0 保守预留目录中的 maxOutputTokens；本地 automatic 模式可指定运行时余量策略，
    // 因为其真实输出上限由“窗口减实际输入”动态决定，不能预留整个窗口而拒绝所有输入。
    std::size_t defaultOutputReserveTokens = 0;
};

// 本地估算与供应商 usage 分开，避免把 UTF-8 字节或启发式结果当成实际计费 token。
// exact=false 时 tokens 仅用于提前压缩；不能保证包含供应商模板/媒体成本的精确窗口校验。
struct MaiModelTokenEstimate {
    std::size_t tokens = 0;
    bool exact = false;
};

struct MaiModelUsage {
    // 仅由供应商实际返回的 usage 填写；没有 usage 就不触发 onUsage，不能用估计值补齐。
    std::size_t inputTokens = 0;
    std::size_t outputTokens = 0;
};

// 仅匹配已核对的官方地址与模型，不将同名代理/自托管 endpoint 当作官方容量。
// 目录来源及核对日期记录在实现中；未知组合返回全零。调用不访问网络、不读取凭据。
MaiModelContextLimits maiOfficialModelContextLimits(const std::string& baseUrl,
                                                    const std::string& model);

// 无供应商 tokenizer 时的公开估计入口。按 Unicode 标量及 ASCII 段估计文本，
// 另计消息/工具声明/媒体余量；不是把字节数称作 token。wire/tokenizer 实现可覆盖更精确计数。
MaiModelTokenEstimate maiEstimateModelInputTokens(const MaiModelRequest& request);

// 可见 assistant 发言的阶段，和不可见的推理内容无关。
// Unspecified 用于未提供阶段的 Chat Completions、本地模型和兼容供应商。
enum class MaiModelTextPhase { Unspecified, Commentary, PartialAnswer, FinalAnswer };

struct MaiModelTextItem {
    // 供应商给出的不透明 ID，只在当前请求内定位发言；缺失时允许为空。
    std::string id;
    MaiModelTextPhase phase = MaiModelTextPhase::Unspecified;
    // 本次响应内的稳定位置，ID 缺失或晚到时仍能把快照补尾交还原片段。
    int index = -1;
};

// ── 流式回调 ────────────────────────────────────────────────────
//
// 线程契约（重要）：这些回调在 stream 调用线程同步调用。网络实现来自 libcurl 写回调，
// 本地实现来自推理解码循环；不要在里面做慢活（写 socket、落盘、等锁），否则会直接拖慢模型吐字。
// 需要慢处理就自己塞队列，
// 控制台的渲染线程就是这么做的（见 cli/MaiConsoleMain.cpp）。
//
// 回调可以为空，实现方调用前要判。比如只关心最终文本的调用方可以不设 onReasoning。
struct MaiStreamSink {
    // 正文增量。**只带这次新增的部分**，不是累计值——调用方自己往后拼。每秒会被调几十次。
    //
    // 参数是 string_view，指向的缓冲区**在回调返回后就不保证有效**了。要留着就自己拷一份。
    std::function<void(std::string_view)> onText;

    // 思考过程的增量（OpenAI 协议的 reasoning_content）。语义和 onText 一样，
    // 但要落成**另一个片段**——界面要能单独折叠它。
    std::function<void(std::string_view)> onReasoning;

    // 工具调用是**攒完整**才给的，不是增量。
    //
    // 流式过程中 arguments 是 JSON 字符串的碎片，按 index 分批到达：
    //     {index:0, id:"call_x", function:{name:"read", arguments:""}}
    //     {index:0,             function:{arguments:"{\"pa"}}
    //     {index:0,             function:{arguments:"th\":\"a.txt\"}"}}
    // 不同服务端分片时机不一样，有的整块给，有的一个字符一个字符给。中途交付会是半截 JSON，
    // 所以攒到流结束再一次性给。
    std::function<void(const MaiToolInvocation&)> onToolCall;

    // 可选的可见发言边界。Responses 的每个 message 独立开始/完成，
    // 完成发言不等于完成整个 Agent 回合；推理事件绝不使用这两个回调。
    // 未提供 item 生命周期的实现可以省略回调，调用方沿用每次响应一个正文片段。
    // 元信息的引用只在同步回调期间有效，phase 不改变正文的可见性。
    // 追加在原回调之后，保留现有三字段聚合初始化的含义。
    std::function<void(const MaiModelTextItem&)> onTextItemStarted;
    std::function<void(const MaiModelTextItem&)> onTextItemCompleted;
    // 需要保留 item 身份的调用方可接此增量；实现方有 item 时优先调用它，
    // 否则调用原 onText。两者不重复交付同一正文，Chat/local 仍可只用 onText。
    // 完成快照可能为此前 item 补尾，调用方必须按 index 定位原片段。
    std::function<void(const MaiModelTextItem&, std::string_view)> onTextItemDelta;
    // 供应商实际返回的输入/输出 token。与其他流回调使用相同线程和引用生命周期。
    // 可缺省，失败或未返回 usage 时不得合成“实际用量”。追加在末尾保留已有聚合初始化。
    std::function<void(const MaiModelUsage&)> onUsage;
};

using MaiModelImagePreparer =
    std::function<MaiResult<std::string>(const std::string& source, const std::string& workspace)>;

// 宿主为一次模型传输选择的网络路由；空地址表示显式直连。
// 代理认证单独传递，不能拼到模型地址或记录到日志。
struct MaiModelProxy {
    std::string url;
    std::string username;
    std::string password;
};

struct MaiModelConfig {
    // 不带末尾斜杠，也不带具体路径。客户端按 wire 拼接端点。例如：
    //   https://open.bigmodel.cn/api/paas/v4   GLM
    //   http://127.0.0.1:11434/v1              Ollama
    std::string baseUrl;
    // 放进 Authorization: Bearer 头。
    std::string apiKey;
    MaiWireApi wire = MaiWireApi::ChatCompletions;
    long connectTimeoutSeconds = 20;
    // 0 = 不限。流式对话可能跑很久（模型想很长时间，或者输出很长），设了上限就会在半截把人掐掉。
    // 真要停用 cancel，那个干净得多。
    long totalTimeoutSeconds = 0;
    // 在此时长内没有接收字节或实际上传增长就中止流式请求。
    // SSE 保活与尚未拼完的事件能证明链路活跃；curl 自身的周期回调不能。0 表示关闭。
    long inactivityTimeoutSeconds = 120;
    // 只在还没有向调用方交付任何正文/思考/工具调用时重试，避免流式内容重复。
    int maxRetries = 2;
    long retryInitialDelayMs = 250;
    // 空时使用平台默认根证书；Android 宿主可传系统信任库导出的 PEM 文件。
    // 只更换信任根来源，不能关闭证书或主机名校验。
    std::string caBundlePath;
    // 大图请求可选用宿主图片预处理。宿主收到已授权的源图绝对路径和 Agent 工作区，
    // 返回不超过 2 MB、可持续读取的 JPEG 预览路径，不得修改源图。
    // 回调可能在任一模型工作线程执行；错误只终止当前图片请求。
    MaiModelImagePreparer prepareImage;
    // 独立限制没有实际模型进展的等待，避免只有心跳或重复状态时无限挂起。
    // 请求上传增长、正文/推理/工具参数增长、首次协议阶段推进会重新计时；单纯保活不会。
    // 900 秒是本产品默认策略，并非 Responses 协议保证。0 可关闭；持续有进展的长输出
    // 不受这个滑动期限截断，原 totalTimeoutSeconds 的总时长语义仍保持独立。
    // 放在结构末尾，保留已有聚合初始化各字段的位置。
    long progressTimeoutSeconds = 900;
    // 可选的逐目标网络路由，由宿主在模型工作线程同步查询；返回 false 拒绝请求。
    // 未提供时保留 libcurl 原有环境代理和重定向行为。提供时，空地址显式直连，
    // 非空地址覆盖环境代理；每个重定向目标重新查询，凭据仅用于该跳的代理。
    // 回调和捕获对象必须在线程并发调用期间有效且线程安全，不得记录目标或凭据。
    std::function<bool(const std::string&, MaiModelProxy&)> proxyResolver;
    // 按实际选中模型提供窗口；空时只查询 maiOfficialModelContextLimits 的已核官方目录。
    // 回调可能在多会话工作线程并发调用，必须线程安全，不得访问用户会话或发模型请求。
    std::function<MaiModelContextLimits(const std::string&)> contextLimits;
    // Chat Completions 是否声明 stream_options.include_usage。只有宿主/供应商确认支持时开启；
    // 无论是否开启，都解析实际到达的 usage 帧。Responses 自带完成 usage，不使用这个开关。
    bool includeStreamUsage = false;
};

// 模型客户端。一个接口，多个 wire 实现。
class MaiModelClient {
public:
    virtual ~MaiModelClient() = default;

    // 阻塞直到这一次应答结束。调用方负责把它放到自己的线程上。
    //
    // 回调在**这个线程**上同步触发（见 MaiStreamSink 的线程契约）。
    //
    // cancel 置 true 会让传输尽快中断——实现方要在收数据的循环里查它，这是中断功能的落点，
    // 比等超时干净得多。主动取消返回的是
    // MaiErrorCode::Canceled，**不是故障**，上层不该当错误报给用户。
    //
    // 返回错误时，**已经通过回调交付出去的内容仍然有效**——半截回答要保留下来，
    // 不要因为最后失败了就整段丢掉。
    virtual MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                            const std::atomic<bool>& cancel) = 0;

    virtual MaiWireApi wireApi() const = 0;
    // 默认未知；没有覆盖的自定义客户端保留原文，不得假设一个通用容量。
    virtual MaiModelContextLimits contextLimits(const std::string& model) const;
    // 默认返回明确标为估计的本地计数；有真实 tokenizer 的实现可返回 exact=true。
    virtual MaiModelTokenEstimate estimateInputTokens(const MaiModelRequest& request) const;
};

// 工厂在 MaiOpenAiClient.h——那是**实现**，这里是接口。
// 自己接别的供应商的人只需要这个文件，不该被迫看见我们碰巧提供了哪一种实现。
