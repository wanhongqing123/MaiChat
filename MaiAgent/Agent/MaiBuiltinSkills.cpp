// 文件作用：让桌面与移动端共用由 resources/skills 生成的原始 UTF-8 资源。
#include "MaiBuiltinSkills.h"

#include "MaiBuiltinSkillResources.inc"

const std::vector<MaiBuiltinSkillResource>& maiBuiltinSkillResources() {
    return kMaiBuiltinSkillResources;
}
