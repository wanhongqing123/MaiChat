// 文件作用：在进程内命令调用之间保留有界的可信文本读取游标。
#pragma once

#include <memory>
#include <string>

#include "MaiError.h"
#include "MaiTextFileReader.h"

// 全部状态只在内存，不保存内容或文件句柄。至多 128 个游标，空闲 120 秒过期。
// 调用方 binding 必须固定会话、访问根、真实文件路径及编码参数；不能来自模型提供的权限。
// recall 返回原始值供 MaiTextFileReader 校验文件快照；本缓存不是授权或文件锁。
// 游标可重放，读取不会消费；绑定不符或失效时拒绝。所有方法可跨线程调用。
class MaiTextFileCursorCache {
public:
    MaiTextFileCursorCache();
    ~MaiTextFileCursorCache();
    MaiTextFileCursorCache(const MaiTextFileCursorCache&) = delete;
    MaiTextFileCursorCache& operator=(const MaiTextFileCursorCache&) = delete;

    MaiResult<std::string> remember(const MaiTextFileCursor& cursor, const std::string& binding);
    MaiResult<MaiTextFileCursor> recall(const std::string& token, const std::string& binding);

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};
