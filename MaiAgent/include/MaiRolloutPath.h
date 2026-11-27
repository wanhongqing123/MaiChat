// 文件作用：生成 rollout 的独立文件标识、相对路径和毫秒精度 UTC 时间文本。
#pragma once

#include <string>

#include "MaiError.h"
#include "MaiFilePath.h"
#include "MaiTime.h"

// rollout 文件的值语义身份，不替换会话、消息等已有标识。
// id 是小写 UUIDv7；relativePath 相对于存储根目录，调用方用 MaiFilePath::append 拼接。
struct MaiRolloutFileIdentity {
    std::string id;
    MaiFilePath relativePath;
};

// 无文件操作的 rollout 命名入口；返回值独立持有数据，无外部生命周期要求。
// 两个静态入口均可并发调用，不读取本地时区，不修改已有 MaiIdGenerator 标识规则。
class MaiRolloutPath {
public:
    // 接受 Unix 毫秒 [0, 253402300799999]，对应 UTC 1970 至 9999 年。
    // 结果为 sessions/YYYY/MM/DD/rollout-YYYY-MM-DDTHH-MM-SS.mmmZ-<id>.jsonl，
    // 始终相对且不含父目录跳转，文件名不含冒号；本函数不创建目录或文件。
    // UUIDv7 的前 48 位保存输入时间，其余可用位随机；不承诺同毫秒内的排序，
    // 也不能用作鉴权材料。范围错误返回 InvalidInput，UTC 转换或格式化失败返回 Internal。
    static MaiResult<MaiRolloutFileIdentity> create(MaiMillis timestamp);

    // 与 create 共享时间范围；固定返回 YYYY-MM-DDTHH:MM:SS.mmmZ 共 24 字节。
    // 无秒或毫秒精度损失；非法输入和平台 UTC 转换失败以 MaiResult 明确返回。
    static MaiResult<std::string> formatTimestamp(MaiMillis timestamp);
};
