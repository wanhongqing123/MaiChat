// 文件作用：按工具当前轮次的实际主模型选择官方检索入口，不读取可变 UI 全局设置。
#pragma once

#include "MaiDeepSeekWebTools.h"
#include "MaiGlmWebTools.h"

struct MaiProviderWebOptions {
    MaiDeepSeekWebOptions deepseek;
    MaiGlmWebOptions glm;
};

// 仅注册一组 web_search/web_fetch。声明和执行均按目标型号使用对应供应商契约；
// MaiToolContext.model 是当前轮的固定值，不能用 SessionStore 的后续选择覆盖。
// GLM 错误保留 GLM 错误，不改用 DeepSeek；未知型号不可调用。注册不取凭据、不访问网络。
void registerMaiProviderWebTools(MaiToolRegistry& registry, MaiProviderWebOptions options);
