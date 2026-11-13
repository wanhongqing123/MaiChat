// 文件作用：移动端 C++ Agent 适配层；MaiMobileAgent 负责移动 Agent 生命周期、工具注册和宿主回调，本文件给出其声明。
// 对外边界：本文件给出 MaiMobileAgent 的类型和函数声明；参数、返回值和对象生命周期以此为准。
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
// 进程内 C ABI。宿主必须从同一条后台串行队列调用，包括销毁。
// create 仅分配句柄；configure 才打开库。request 返回拥有的 UTF-8 JSON，必须
// free。 密钥只进 configure，不从响应/事件返回。异常不能跨语言边界传播。
// request 的 steer 操作为 {op:"steer",session,turnId,requestId,text,images?,videos?,
// quoteMessageId?}，媒体字段形状同 send。turnId 取该 session 的 snapshot.turnId，
// 整轮稳定；requestId 由宿主生成且重试复用。成功 {ok:true,id:<已落库 user ID>}，
// 只表示已接收，当前工具结束后消费；未执行的同批调用交回模型重新判断。idle/stop/收尾拒绝，
// 不启动另一轮；失败须保留草稿，禁止自动回退 send。同轮相同请求有界幂等。
// send/steer 成功另附 user_message_id 与 turnId：前者明确指向真实用户消息，
// 后者是原任务最初的 assistant ID，即使任务已完成或重试也不改为后来活动轮次。
// 原 id 语义不变（send 是 assistant、steer 是 user）；宿主不得用它猜 canonical user。
// 若成功响应缺明确收据字段，应保留未知状态，不新建编号盲重发。
// message 操作为 {op:"message",session,id}，只读精确查询该会话的一条记录；
// 成功 {ok:true,messages:[记录]}，找不到/归属不符 {ok:false,errorCode:"not_found"}。
// 不用 ID 前缀或游标模拟精读，不消费 snapshot 的增量状态，也不触发模型请求。
// tool_detail 操作为 {op:"tool_detail",session,requestId,messageId,partId,
// field:"input"|"output"|"error",offset?:0,limit?:4096,detailRevision?:string}。
// 只读取一次已落库工具快照，不重跑命令；limit为1..4096源字节，offset>0须带前页版本。
// 成功返回上述绑定（session改名sessionId）及callId/tool/state/detailRevision/text、
// sourceBytesRead/totalBytes/nextOffset/hasMore/textIsValidUtf8；nextOffset=0表示读完。
// 原文UTF-8标量不拆；坏编码页text为replacement预览并附rawBase64，游标仍按源字节。
// 工具任一字段变化返回errorCode:"busy"，须从首屏重读。非法字段/偏移为invalid_input，
// 归属不符为not_found；失败仍保requestId/sessionId/messageId/partId/field/offset绑定。
// 非法绑定不原样回显：ID须是1..128源字节字符串，否则为空；field非法为空；offset非法为0。
// 普通消息tool片段增加detailRevision/detailsAvailable及可选callId（源字节<=128才发布），
// 不截断异常callId，版本仍覆盖其完整原值；原input/output/error不变。
// local_model_probe 仅测已配置本地客户端：messages 为最多10条 {role,content}，
// role 仅 user/assistant，timeout_ms 为1..120000。复用同一完整工具声明，绝不执行工具、
// 读写会话或调用宿主凭据；工具输出请求本身算失败。用于隔离设备验收，不代表Agent工具闭环。
void* maiMobileAgentCreate(void);
void maiMobileAgentDestroy(void* handle);
// 会话模型操作：set_model 带 session/model/reasoningEffort?，get_session 带 session。
// effort 省略不修改，空串恢复继承，显式值仅 low/high/max；非法/未配置/目标 busy 整次拒绝。
// 成功均返回 ok/session/model/reasoningEffort/wire；snapshot sessions[] 有同名配置字段，
// selectedModel/selectedReasoningEffort/selectedWire 投影选中会话；旧 top model/wire 仅是新会话默认。
// defaultReasoningEffort 仅作空 override 的显示解释，reasoningEffort 本身仍是持久原值。
// prepare_local_model 仅本机宿主可用，带 model 和原 localModelPath/Projector/预算字段，
// 验证后初始化 lazy slot，不加载权重或重建 Agent；远端 set_model 不接受此操作/任何路径。
// send/enqueue/steer 不覆盖会话选择。configure 可提供 glmBaseUrl/deepseekBaseUrl，
// 可同时预检 localModelName/localModelPath 等现有本地参数；选择会话不重建 Agent。
// 四个正式云型号每请求使用既有 glm_api_key/deepseek_api_key 账号回调，不沿用旧 Key 兜底。
char* maiMobileAgentRequest(void* handle, const char* request);
void maiMobileAgentFree(char* response);

// Session IM 的即时控制边界，不借用 backend handle，也不访问 Agent/数据库/宿主回调。
// request 是 owned delivery 的 claim/ack，或带 bridge_id/scope 的 admit/abandon_input/
// takeover/logout。admit 只接收 live metadata，在 SDK 下载前保留原到达顺序；正文另走普通 worker。
// 任意 SDK 线程均可调用；过期/已销毁的弱注册实例拒绝旧票据。返回 JSON 由调用方持有，
// 仍用 maiMobileAgentFree 释放。claim 仅授权一次真实 SDK 派发，不承诺撤回已发出的消息。
char* maiMobileSessionIMControl(const char* request);
// 只读 IM 历史快照的 open/read/close，参数是原 owned history_task 或 snapshot_id。
// 可执行 SQLite I/O，必须在既有准备 worker 上调用；不能在 Main、SDK callback 或 Agent 串行队列调用。
// 不借用 backend handle；响应由 maiMobileAgentFree 释放。来源/旧账号/绑定代际由共同协调器校验。
char* maiMobileIMHistorySnapshot(const char* request);

// 普通 PDF 密码保护入口，与 generate_pdf 共用进程内核心，不调用模型工具。
// source/output 为已授权的绝对 UTF-8 路径。password 是借用的原始 UTF-8 bytes，
// 长度显式传入以拒绝 NUL 和非法编码，避免 JNI modified UTF-8 改写密码。
// 同步阻塞，只在工作线程调用；成功原子发布，失败保留输入和旧输出。
// 响应只有路径/尺寸/状态或固定安全错误，不含密码，须用 maiMobileAgentFree 释放。
char* maiMobilePdfEncrypt(const char* source, const char* output, const unsigned char* password, size_t passwordSize,
                          int replaceExisting);
// iOS 静态链接 ONNX Runtime 时，必须在 configure 前登记 OrtGetApiBase 返回的地址。
// Android 和桌面端改用打包的动态库路径。指针借用到宿主进程结束，只能在 Agent 工作线程设置。
int maiMobileAgentSetOrtApiBase(void* handle, const void* apiBase);

// 本地会话内容变化时的轻量通知。只允许在 configure 前设置；回调可能在 Agent
// 工作线程触发，sessionId 仅在本次回调内有效。宿主只能复制 ID 并异步投递，
// 不得在回调里读数据库、发网络请求或等待主线程。传空 handler 可禁用。
typedef void (*MaiMobileAgentChangeHandler)(const char* sessionId);
int maiMobileAgentSetChangeHandler(void* handle, MaiMobileAgentChangeHandler handler);

// 视频抠像的宿主直接入口与 cv_video_matting 工具共用参数 Schema 和处理器。
// 路径使用 UTF-8；argumentsJson 中的 output_path 仍相对于 workspace。
// 返回由调用方持有的 {ok,output|error} JSON 字符串，必须用 maiMobileAgentFree 释放。
char* maiMobileMatteVideo(const char* argumentsJson, const char* workspace, const char* modelPath,
                          const char* runtimePath, const void* apiBase);

// MaiChat 专属工具由嵌入它的 App 实现。MaiAgent 通过常规审批闸门后在工作线程调用 handler。
// 不同会话可能并发回调；宿主访问平台状态时须切到对应线程。
// toolName 与 argumentsJson 为仅在回调期间有效的借用 UTF-8 字符串。
// handler 返回宿主持有的、以 NUL 结束的 UTF-8 JSON：
//   {"ok":true,"output":"text returned to the model"}
//   {"ok":false,"error":"actionable failure"}
// responseFree 必须用宿主分配器释放响应。处理器被替换或桥接销毁且全部 Agent 工作结束后，
// contextRelease 只调用一次。
typedef const char* (*MaiMobileHostToolHandler)(void* context, const char* toolName, const char* argumentsJson);
typedef void (*MaiMobileHostToolResponseFree)(void* context, const char* response);
typedef void (*MaiMobileHostToolContextRelease)(void* context);

// 成功返回 1；句柄或回调组合无效，以及工具回调正在执行时返回 0。
// 传空 handler 会清除注册；否则三个回调指针都必须非空。
int maiMobileAgentSetHostToolHandler(void* handle, void* context, MaiMobileHostToolHandler handler,
                                     MaiMobileHostToolResponseFree responseFree,
                                     MaiMobileHostToolContextRelease contextRelease);

#ifdef __cplusplus
}
#endif
