// 文件作用：定义模型线协议持有的、不向用户展示的响应续接状态。
#pragma once

#include <string>

// 一次成功响应的完整续接快照，与显示文本和工具执行投影分别保存。
//
// 纯数据，不解析厂商协议。format 当前为 responses.output.v1；provider 和 model
// 绑定原始来源，wire 层只向相同来源重放 payload。payload 是 wire 所有的不透明串，
// 包含原始有序 output 项及其签名/加密字段；不得从可见思考重建或截断。
// 授权会话的 JSONL 权威原文必须保留这些字段；诊断日志不得复制其内容。
// 持久化、上下文构造及预算必须保留这四项；旧记录没有状态时保持既有行为。
// 值类型可跨线程复制；各字段只能在构造/接收完整成功快照时一起赋值，发布后不可改写。
struct MaiModelContinuationState {
    std::string format;
    std::string provider;
    std::string model;
    std::string payload;
};
