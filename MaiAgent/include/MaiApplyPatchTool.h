// 文件作用：解析并顺序应用 Codex 格式补丁，普通调用和模型工具共用同一实现。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "MaiError.h"
#include "MaiTool.h"

// 普通代码的调用参数。相对路径以 root 为基准；默认禁止离开 root。
// resolvePath 是宿主可信的授权解析器，非空时替代默认边界检查；空返回值一律拒绝。
// 回调仅在同步调用期间借用，不保存；调用方必须在进入本函数前完成所需用户授权。
struct MaiPatchOptions {
    std::string root;
    std::function<std::string(const std::string&)> resolvePath;
};

// 已成功完成的实际文件操作，按提交顺序记录；移动包含目标写入和源文件删除两项。
// path 保留补丁中的拼写；resolvedPath 是本次访问的 UTF-8 绝对路径。
// status 为 A/M/D，bytesWritten 只计成功写入的字节，删除为零。
struct MaiPatchChange {
    std::string path;
    std::string resolvedPath;
    char status = 'M';
    std::uint64_t bytesWritten = 0;
};

// 语法先整份校验，随后按文件块顺序读写，允许 Add 后继续 Update 同一路径。
// 不是跨文件事务：失败时 appliedChanges 保留此前成功操作，不回滚。
// failurePath 是失败操作的补丁路径；语法/会话配置错误时可为空。
// 写入错误可能已截断或部分写入 failurePath；该情况置 failedWriteMayHaveChangedFile，
// 不把未知的实际字节数伪装成成功操作。调用方应复读现场，不能盲目重放整份补丁。
struct MaiPatchResult {
    MaiError error;
    std::vector<MaiPatchChange> appliedChanges;
    std::string failurePath;
    bool failedWriteMayHaveChangedFile = false;
};

// 阻塞调用，不持有跨调用状态，不提供文件锁或并发写入事务。
// UTF-8 文本语法、上下文匹配和换行处理对齐固定 Codex apply-patch 实现；
// 宿主路径范围仍由 MaiPatchOptions 的可信授权解析器控制。
MaiPatchResult maiApplyPatch(const std::string& patch, const MaiPatchOptions& options);

// apply_patch 模型工具：负责 JSON 参数、逐路径授权键和普通结果适配。
// execute 与普通 maiApplyPatch 共用解析及文件修改核心，不注册旧名称别名。
std::unique_ptr<MaiTool> makeMaiApplyPatchTool();
