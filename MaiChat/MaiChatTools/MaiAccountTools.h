#pragma once

#include <functional>
#include <memory>
#include <string>

#include "MaiTool.h"

// 三端宿主适配现有普通账号服务；处理器同步返回，不持有 context 或其中的取消指针。
// 资料查询只允许公开账号字段；头像处理器必须在上传及 IM SDK 确认后才返回 updated=true。
using MaiAccountToolHandler =
    std::function<MaiToolResult(const std::string& argumentsJson, const MaiToolContext& context)>;
using MaiAccountToolCancelHandler = std::function<void(const std::string& requestId)>;

// 参数 {}；返回 logged_in，登录时含 account_id/im_user_id 及可空 nickname/phone_masked/avatar_url。
// 实现统一校验参数并白名单序列化结果，不把整个鉴权响应转给 Agent。
std::unique_ptr<MaiTool> makeMaiCurrentAccountTool(MaiAccountToolHandler handler);

// 参数 {"path":"workspace image"}；统一通过 context.resolvePath 后将绝对 path 交给宿主。
// 外部写入遵守已有审批策略；结果只含 updated=true 和稳定、无签名的 avatar_url。
// 移动桥接可提供内部取消通知；监视线程在处理器返回前回收，不保存悬空 context。
std::unique_ptr<MaiTool> makeMaiSetAvatarTool(MaiAccountToolHandler handler, MaiAccountToolCancelHandler cancel = {});
