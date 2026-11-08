// 文件作用：直接读取本地容器元数据，不启动 FFplay 或设备，也不让界面依赖媒体框架。
#include "MaiMediaProbe.h"

#include <algorithm>
#include <cerrno>
#include <chrono>

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavcodec/codec_par.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

namespace {

MaiMediaProbeResult failed(int error) {
    MaiMediaProbeResult result{};
    result.error_code = error < 0 ? error : AVERROR_INVALIDDATA;
    return result;
}

struct MaiProbeInput {
    AVFormatContext* value = avformat_alloc_context();
    ~MaiProbeInput() {
        if (value) avformat_close_input(&value);
    }
};

struct MaiProbeDeadline {
    std::chrono::steady_clock::time_point expires = std::chrono::steady_clock::now() + std::chrono::seconds(5);
};

int interruptProbe(void* context) {
    const auto* deadline = static_cast<const MaiProbeDeadline*>(context);
    return std::chrono::steady_clock::now() >= deadline->expires;
}

}  // namespace

extern "C" MaiMediaProbeResult maiMediaProbeFile(const char* utf8_path) {
    if (!utf8_path || !*utf8_path) return failed(AVERROR(EINVAL));
    // 输入关闭仍可能触发 FFmpeg I/O，因此 deadline 必须比输入上下文活得更久。
    MaiProbeDeadline deadline;
    MaiProbeInput input;
    if (!input.value) return failed(AVERROR(ENOMEM));
    input.value->interrupt_callback = {interruptProbe, &deadline};
    // 时长卡片只需容器和流信息。限制探测预算，禁止本地播放列表借媒体探测访问网络协议。
    input.value->probesize = 4 * 1024 * 1024;
    input.value->max_analyze_duration = 3 * AV_TIME_BASE;
    AVDictionary* options = nullptr;
    int status = av_dict_set(&options, "protocol_whitelist", "file", 0);
    if (status >= 0) status = avformat_open_input(&input.value, utf8_path, nullptr, &options);
    av_dict_free(&options);
    if (status < 0) return failed(status);
    status = avformat_find_stream_info(input.value, nullptr);
    if (status < 0) return failed(status);

    MaiMediaProbeResult result{};
    if (input.value->duration != AV_NOPTS_VALUE && input.value->duration > 0)
        result.duration_us = input.value->duration;
    int64_t longestStreamDuration = 0;
    for (unsigned index = 0; index < input.value->nb_streams; ++index) {
        const AVStream* stream = input.value->streams[index];
        if (!stream || !stream->codecpar || stream->codecpar->codec_id == AV_CODEC_ID_NONE) continue;
        const AVCodecParameters* parameters = stream->codecpar;
        const bool audio = parameters->codec_type == AVMEDIA_TYPE_AUDIO;
        const bool video =
            parameters->codec_type == AVMEDIA_TYPE_VIDEO && !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC);
        if (!audio && !video) continue;
        if (audio) result.has_audio = 1;
        if (video && !result.has_video) {
            result.has_video = 1;
            result.width = std::max(parameters->width, 0);
            result.height = std::max(parameters->height, 0);
        }
        if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0 && stream->time_base.num > 0 &&
            stream->time_base.den > 0) {
            longestStreamDuration =
                std::max(longestStreamDuration, av_rescale_q(stream->duration, stream->time_base, AV_TIME_BASE_Q));
        }
    }
    if (!result.has_audio && !result.has_video) return failed(AVERROR_INVALIDDATA);
    if (result.duration_us == 0) result.duration_us = longestStreamDuration;
    return result;
}
