// 文件作用：从一次已落库工具快照分页读取详情，供本机界面和远端只读请求共用。
#pragma once

#include <cstddef>
#include <string>

#include "MaiError.h"
#include "MaiMessage.h"

class MaiSessionStore;

struct MaiToolDetailQuery {
    std::string messageId;
    std::string partId;
    // 只接受 input/output/error，禁止把路径、媒体或任意存储字段当作工具详情。
    std::string field = "input";
    std::size_t offsetBytes = 0;
    std::size_t maxBytes = 4096;
    // offsetBytes>0 必填。首屏也可传入已观察到的版本，防止点击后悄悄展示另一份结果。
    std::string expectedRevision;
};

struct MaiToolDetailPage {
    std::string messageId;
    std::string partId;
    std::string callId;
    std::string tool;
    MaiToolState state = MaiToolState::Pending;
    std::string field;
    std::string revision;
    // 原始源字节，不做编码替换；无效 UTF-8 由宿主展示 replacement 并可附原字节 Base64。
    std::string text;
    std::size_t offsetBytes = 0;
    std::size_t sourceBytesRead = 0;
    std::size_t totalBytes = 0;
    // 0 表示读完；非零时必须保留同一 revision 继续，不混入更新后的输出。
    std::size_t nextOffsetBytes = 0;
    bool textIsValidUtf8 = true;
};

// 对 tool/state/callId/input/output/error 及可选 execution 的原字节按长度分帧计算 SHA256。
// 结果为 64 位小写十六进制；execution 缺省时保持旧记录的版本兼容。
// 不包含查询偏移；任一工具字段改变都会改变版本。摘要失败返回空串，读取入口会报告 Internal。
std::string maiToolDetailRevision(const MaiToolPart& tool);

// sessionId 必须来自宿主已鉴权的归属绑定。同步只读：仅一次 getMessage 获取独立快照，
// 可与活动工具并发读取，不写原史、不调用模型/工具、不访问字段内的文件或 URL。
// output 有已验证的完整保留快照时分页读取它；捕获不完整时保留原预览警告，
// 此时普通 readToolOutput/tool_output_read 可补读部分快照并取得不完整标志。
// 消息/part不存在或归属不符为 NotFound；字段、ID、偏移或1..4096预算非法为 InvalidInput。
// 合法UTF-8标量不可拆；非法原字节按单字节前进。预算容不下下一个完整标量则 InvalidInput，
// 调用方可增大 limit；不返回无法推进的成功页。版本不符为 Busy（detail_changed），应从首屏重读。
MaiResult<MaiToolDetailPage> maiReadToolDetail(const MaiSessionStore& store,
                                               const std::string& sessionId,
                                               const MaiToolDetailQuery& query);
