// 文件作用：封装 MiniMax 图片及视频生成任务。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "MaiCreativeMediaSupport.h"

// 宿主按需从安全存储提供 MiniMax API Key，回调须能在工作线程安全调用。
// 密钥为空时仍可查询能力，但不得提交付费任务。
using MaiMiniMaxApiKeyProvider = std::function<std::string()>;

// 视频使用 H3/H3 Max V2 异步接口。图片使用 image-01 或 image-01-live 的同步接口；
// 无参考图是文生图，一张人物参考图走 character 主体图生图，不提供任意局部重绘。
// 本地图只作为私有 OSS 上传源，供应商收到 HTTPS URL；输出 URL 立刻下载到工作区。
// 当前视频新生成与重做因费用暂停，在凭据、素材上传和 HTTP 前拒绝，返回未开始且无副作用。
// 视频对所有模型均不可见；宿主仅内部注册供旧任务 continue/delete 使用。
// 本地 validate 和 discover 不创建任务；已有任务查询、下载及本地记录清理保持原行为。
std::unique_ptr<MaiTool> makeMaiMiniMaxVideoTool(MaiMiniMaxApiKeyProvider apiKey,
                                                 std::string caBundlePath = {},
                                                 MaiCreativeMediaUploadProvider uploadMedia = {});
// 图像生成同样因费用暂停；文生图和参考图生成在凭据、上传及 HTTP 之前拒绝。
std::unique_ptr<MaiTool> makeMaiMiniMaxImageTool(MaiMiniMaxApiKeyProvider apiKey,
                                                 std::string caBundlePath = {},
                                                 MaiCreativeMediaUploadProvider uploadMedia = {});
