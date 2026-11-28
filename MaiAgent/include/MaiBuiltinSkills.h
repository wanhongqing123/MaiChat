// 文件作用：提供随共享核心编译的同一份只读技能资源，不涉及用户目录写入。
#pragma once

#include <string_view>
#include <vector>

// 资源借用编译期静态字节，生命周期与进程一致；正文不会自动进入模型上下文。
struct MaiBuiltinSkillResource {
    std::string_view name;
    std::string_view description;
    std::string_view contents;
};

const std::vector<MaiBuiltinSkillResource>& maiBuiltinSkillResources();
