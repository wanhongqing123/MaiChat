#pragma once

#include "MaiError.h"
#include "MaiFilePath.h"

// 同步启动检查，可能阻塞；仅从存储准备工作线程调用。Mac使用公开libproc查询同UID
// 对指定源库或保留旧库inode的外部打开引用，忽略当前进程；不读取DB正文、不终止进程。
// 发现占用返回Busy，检查失败返回错误，宿主必须拒绝继续并要求退出旧App后重试。
// 这是单次状态检查，不是跨扫描/重命名的原子锁，也不排斥未来新打开；Core仍需独立
// 冻结源证据及SQLite写保留。非macOS桌面返回成功，由各自原生/Core guard负责排他。
MaiError checkDesktopSessionDatabaseUse(const MaiFilePath& databasePath);
