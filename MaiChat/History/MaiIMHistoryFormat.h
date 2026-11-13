#pragma once

#include "MaiContextImport.h"
#include "MaiIMHistorySnapshot.h"

// 只把普通持久消息的安全元数据转成带出处的引用上下文；不访问文件或执行模型。
// 不构造 assistant、工具、像素或媒体交付片段，也不赋予引用路径的读取/发送权限。
MaiResult<MaiContextImportRecord> maiFormatIMHistoryContextRecord(
    const MaiIMHistoryRecord& record, const MaiIMHistorySource& source);
