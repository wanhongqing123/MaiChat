// 文件作用：为原生设置页适配共享 MaiAppStorage 普通服务，不经过 Agent 或工具协议。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// 三个参数都是借用的、以 NUL 结尾的绝对 UTF-8 路径；宿主先创建 App 自有目录。
// temporary、cache、workspace 必须真实存在、互不嵌套；不能传系统共享临时根目录。
// 本函数不创建或扫描目录，路径安全性由共享核心在每次 preview/cleanup 时重新验证。
// 空/相对路径或分配失败返回 NULL。句柄独立于聊天 Agent，并固定绑定这三个根目录和
// 一个实例内稳定的身份；账号/Workspace 改变时销毁旧句柄并创建新句柄。
// 句柄是不透明的标识，不能解引用、持久化、计算偏移或用其他 API 释放。
void* maiMobileAppStorageCreate(const char* temporary, const char* cache, const char* workspace);

// 同步、可能遍历磁盘，只在工作队列调用。宿主应串行提交同一句柄上的请求；内部也串行
// 保护其状态。不同句柄独立。每个句柄只保留最后一次成功预览的十分钟、单次使用 token。
// preview 是只读操作，固定保留最近 24 小时的文件，且不接受 scope/patterns/年龄覆盖：
//   {"action":"preview","target":"app"|"temporary"|"cache"}，target 缺省为 app。
// app 汇总三根目录，但只读；Workspace 原片、成品及中间文件均不能通过本接口删除。
// cleanup 必须使用同一句柄明确 temporary/cache 预览返回的 token：
//   {"action":"cleanup","preview_id":"..."}。拒绝未知字段、类型错误及重复字段。
// 核心只删除预览中的固定普通文件集合；预览后新增/改动文件、链接、受保护模型及
// gallery-* 都保留。最多 1000 个候选，统计遍历也有上限，不代表安装包或全 App 占用。
// success: {ok:true,output:{...}}；failure: {ok:false,errorCode:"...",error:"..."}。
// preview output: preview_id, summaries[{target,bytes,files,eligible_bytes,eligible_files,
// excluded_recent_files,excluded_recent_bytes,excluded_protected_files,excluded_policy_files,
// truncated}], candidate_bytes, candidate_files, limited, min_age_hours,
// cleanup_allowed, workspace_cleanup_allowed(false)。不返回候选的绝对路径。
// cleanup output: removed_files, reclaimed_bytes, skipped_files。
// 每次响应均由调用方持有，必须用 maiMobileAppStorageFree 释放；分配失败可能返回 NULL。
// 解析/核心异常不会跨 C ABI。NULL、未知或已销毁的句柄会返回失败 JSON。
char* maiMobileAppStorageRequest(void* handle, const char* request);

// 可与正在执行的请求并发：先撤销句柄并发出取消信号，再等待该请求退出；不要在 UI
// 线程销毁。取消不回滚已经删除的文件。返回后不会继续访问根目录；重复/NULL 销毁无事。
// 销毁后的标识不会重新分配给其他实例，迟到请求只会失败。释放响应可晚于销毁句柄。
void maiMobileAppStorageDestroy(void* handle);
void maiMobileAppStorageFree(char* response);

#ifdef __cplusplus
}
#endif
