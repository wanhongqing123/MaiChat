// 文件作用：提供读取已发现技能的模型工具，复用 MaiSkillManager 普通接口。
#pragma once

#include <functional>
#include <memory>

#include "MaiSkillManager.h"
#include "MaiTool.h"

// 工具仅按宿主提供的目录快照读取，模型不能指定根目录或任意文件路径。
// manager 和 catalog 随工具持有；快照应按轮次或工作目录更新，不能跨目录混用。
// 只读且无需交互，可并发；{skill} 返回完整正文，mode=discover 仅返回有界元数据页。
// 两者都调用同一 manager 普通接口，明确返回错误，不执行技能资源。
std::unique_ptr<MaiTool> makeMaiSkillTool(std::shared_ptr<MaiSkillManager> manager,
                                          MaiSkillCatalog catalog);

// Agent 只注册一个工具，用可信上下文在工作线程解析所属会话的已固定目录。
// resolver 不得从模型参数选择根目录，必须核对 sessionId/root 的归属；按值返回快照。
// 此入口读取已发现文件的当前正文，元数据仍来自固定目录，不自动执行或更新旧提示词。
using MaiSkillCatalogResolver = std::function<MaiResult<MaiSkillCatalog>(const MaiToolContext&)>;
std::unique_ptr<MaiTool> makeMaiSkillTool(std::shared_ptr<MaiSkillManager> manager,
                                          MaiSkillCatalogResolver resolver);
