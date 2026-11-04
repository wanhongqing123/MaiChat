// 文件作用：在替换现有主模型之前，有界检查本地模型配置与原生工具模板。
#pragma once

#include "MaiError.h"

struct MaiLlamaModelConfig;

// 同步、只读预检；调用方应放在允许阻塞文件访问的工作线程上。路径须已经由宿主授权，
// 本函数不扩大文件权限、不读取账号凭据、不联网，也不创建或改变驻留推理实例。
// 仅接受 GGUF v2/v3：最多读取 64 MiB 前缀，4096 个元数据项、100000 个张量，
// 单个数组最多 1000000 项，单个字符串/原生模板最多 1 MiB；文件最多 32 GiB。
// 校验张量形状、类型、对齐及数据范围。只执行已核准的 Qwen2.5/Qwen3.5 原作者工具模板：
// default/tool_use 的每个非空原文都须匹配固定 SHA256，再由聊天解析器核对工具与结果。
// 未核准模板、chatml 别名和缺模板均返回 NotSupported，不执行或替换未知模板。
// 核准来源是 Qwen/Qwen2.5-0.5B-Instruct-GGUF revision
// 9217f5db79a29953eb74d5343926648285ec7e67，以及 Qwen/Qwen3.5-2B revision
// 15852e8c16360a2fea060d615a32b45270f8a8fc 的完整原始模板；这只是模板兼容范围，
// 不是可安全执行任意 Jinja 的沙箱。扩展模板前须单独审核执行开销与解析契约。
// 配置边界与推理入口一致：context 128..32768，tokens=0 自动使用完整 prompt 后的上下文余量，
// 非零显式预算须小于 context；threads 1..64，temperature 为有限的 0..2。
// 原始输出内存边界 1..4 MiB，触及会明确失败而不静默截断；暂不支持 projector。
// 这不是权重内容校验、模型质量验收或设备内存可加载保证；检查后文件仍须保持不变，
// 真正加载和执行的错误由后续 stream 返回。失败不修改调用方的配置或现有 Agent。
MaiError validateMaiLlamaModelConfig(const MaiLlamaModelConfig& config);
