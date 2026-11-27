// 文件作用：通过宿主授权的标准目录发现并安装仓库技能，复用普通安装核心。
#pragma once

#include <functional>
#include <memory>

#include "MaiSkillInstaller.h"
#include "MaiTool.h"

// resolver 仅从当前会话/宿主取得 home 和 workspace，不接受模型传入任意写入根。
// 阻塞下载/写入仅在工具工作线程发生。发现不安装；安装仍经过宿主审批，且不执行脚本。
using MaiSkillInstallerOptionsResolver =
    std::function<MaiResult<MaiSkillInstallerOptions>(const MaiToolContext&)>;
std::unique_ptr<MaiTool> makeMaiSkillInstallTool(MaiSkillInstallerOptionsResolver resolver,
                                                 MaiSkillInstaller::Fetch fetch = {});
