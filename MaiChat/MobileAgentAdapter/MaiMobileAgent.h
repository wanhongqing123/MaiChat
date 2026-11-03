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
// 只表示已接收，完整工具批次结束后、下一次模型请求前消费。idle/stop/收尾拒绝，
// 不启动另一轮；失败须保留草稿，禁止自动回退 send。同轮相同请求有界幂等。
// local_model_probe 仅测已配置本地客户端：messages 为最多10条 {role,content}，
// role 仅 user/assistant，timeout_ms 为1..120000。复用同一完整工具声明，绝不执行工具、
// 读写会话或调用宿主凭据；工具输出请求本身算失败。用于隔离设备验收，不代表Agent工具闭环。
void* maiMobileAgentCreate(void);
void maiMobileAgentDestroy(void* handle);
char* maiMobileAgentRequest(void* handle, const char* request);
void maiMobileAgentFree(char* response);

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
