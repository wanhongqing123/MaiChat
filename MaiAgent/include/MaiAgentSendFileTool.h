// 文件作用：校验工作区文件并交付给当前 AI 对话的持久文件卡片。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "MaiTool.h"

struct MaiAgentFile {
    std::string relativePath;
    std::string name;
    std::string mimeType;
    std::uint64_t bytes = 0;
};

// 同步普通入口；在工作线程调用，进行路径与文件元数据 I/O，不读取文件正文。
// filePath 必须相对于 context.root；绝对路径、越界链接、目录、非普通文件、空文件
// 和超过 512MiB 的文件均被拒绝。取消返回 Canceled；失败不修改源文件或交付卡片。
// 成功返回解析链接后的工作区相对路径、真实 basename、MIME 提示和实际文件大小。
// 不复制、不上传或执行文件，不持有 context；宿主稍后点击时仍须验证路径和文件存在。
MaiResult<MaiAgentFile> maiPrepareAgentFile(const std::string& filePath,
                                            const MaiToolContext& context);

// 与普通入口共用校验；只将文件元数据交付到当前 AI 对话，不接收 IM 的 peer_id。
std::unique_ptr<MaiTool> makeMaiAgentSendFileTool();
