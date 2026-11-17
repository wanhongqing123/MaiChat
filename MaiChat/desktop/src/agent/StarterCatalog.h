// 文件作用：将桌面 Agent 的实际公开工具声明投影为欢迎页能力说明。
// 目录只读取传入的声明，不注册工具、不查询凭据，也不执行示例或修改审批策略。
#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <vector>

#include "MaiTool.h"

struct StarterEntry {
    QString name;
    QString symbol;
    QString purpose;
    QString example;
    QStringList toolNames;
};

// 只返回实际公开工具的说明卡片；普通聊天不依赖卡片，空工具列表返回空目录。
QList<StarterEntry> desktopStarterCatalog(const std::vector<MaiToolSpec>& tools);
