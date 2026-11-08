// 文件作用：提供不启动播放器的本地媒体探测接口，供普通业务与界面共用。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MaiMediaProbeResult {
    // 成功为 0，失败为负数；失败时其余字段全部为 0。
    int error_code;
    // 源媒体总时长，单位微秒；容器和流都未提供有效时长时为 0。
    int64_t duration_us;
    int has_audio;
    int has_video;
    // 首个普通视频流的编码尺寸；纯音频或无有效视频尺寸时为 0。
    int width;
    int height;
} MaiMediaProbeResult;

// 同步只读探测一个 UTF-8 本地文件，调用方应在工作线程执行。
// 不启动 FFplay、不读取音频设备、不生成输出文件，也不经模型工具或网络协议访问媒体。
// 空路径、无法打开或无法识别媒体时返回失败；结果不包含借用指针或需释放的内存。
MaiMediaProbeResult maiMediaProbeFile(const char* utf8_path);

#ifdef __cplusplus
}
#endif
