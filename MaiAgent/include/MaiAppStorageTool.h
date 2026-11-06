// 文件作用：提供应用私有存储的查询与文件操作工具。
// 对外类型：MaiAppStoragePaths；线程、失败和生命周期以类型附近注释为准。
#pragma once

#include <memory>

#include "MaiAppStorage.h"
#include "MaiTool.h"

// 普通入口见 MaiAppStorage；工具只适配参数、已有交付收据和逐次审批。
// scan 和 dry_run 默认只读，执行须 dry_run=false 及该实例最近一次预览 ID。
std::unique_ptr<MaiTool> makeMaiAppStorageTool(MaiAppStoragePaths paths);
