// 文件作用：将共用人像工具绑定到当前桌面账号的普通服务。
#pragma once

#include <QString>
class MaiToolRegistry;

// accountDirectory 来自宿主当前账号的 Agent 数据库目录，不接受工具参数指定。
void registerDesktopPortraitTools(MaiToolRegistry& registry, const QString& accountDirectory);
