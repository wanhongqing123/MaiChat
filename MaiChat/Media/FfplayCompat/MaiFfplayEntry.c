// 文件作用：包装 ffplay 的启动、停止、跳转和播放器状态，把宿主命令交给播放循环。
// 调用边界：由 MaiChat 媒体或 Agent 宿主调用；平台资源在停止或失败时须正确释放。
#include "MaiFfplayEntry.h"

#include <setjmp.h>
#include <stddef.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#define MAI_FFPLAY_THREAD_LOCAL __declspec(thread)
#define MAI_FFPLAY_NORETURN __declspec(noreturn)
#else
#include <stdatomic.h>
#include <pthread.h>
#define MAI_FFPLAY_THREAD_LOCAL _Thread_local
#define MAI_FFPLAY_NORETURN _Noreturn
#endif

#include "SDL.h"
#include "MaiFfplayAudioTempo.h"
#include "MaiFfplayCompatInternal.h"
#include "libavfilter/buffersrc.h"
#include "libavformat/avformat.h"
#if defined(_WIN32)
#include "cmdutils.h"
#endif

#if defined(_WIN32)
static volatile LONG sRunning;
static volatile LONG sPlaybackActive;
static volatile LONG sPlaybackEnded;
static volatile LONG64 sPositionUs;
static volatile LONG64 sDurationUs;
static volatile LONG64 sPendingSeekUs = -1;
static volatile LONG64 sSeekFeedbackUs = -1;
static volatile LONG sPaused;
static volatile LONG sPendingPauseToggles;
static volatile LONG sPlaybackSpeedPercent = 100;
static volatile LONG64 sAudioSettings = 200;
static volatile LONG sFirstAudioCallback;
static SRWLOCK sAudioCommandLock = SRWLOCK_INIT;
#define MAI_STATUS_STORE64(field, value) InterlockedExchange64(&(field), (value))
#define MAI_STATUS_LOAD64(field) InterlockedCompareExchange64(&(field), 0, 0)
#define MAI_STATUS_EXCHANGE64(field, value) InterlockedExchange64(&(field), (value))
#define MAI_STATUS_STORE(field, value) InterlockedExchange(&(field), (value))
#define MAI_STATUS_LOAD(field) InterlockedCompareExchange(&(field), 0, 0)
#define MAI_STATUS_EXCHANGE(field, value) InterlockedExchange(&(field), (value))
#else
static atomic_flag sRunning = ATOMIC_FLAG_INIT;
static atomic_int sPlaybackActive;
static atomic_int sPlaybackEnded;
static atomic_int_fast64_t sPositionUs;
static atomic_int_fast64_t sDurationUs;
static atomic_int_fast64_t sPendingSeekUs = -1;
static atomic_int_fast64_t sSeekFeedbackUs = -1;
static atomic_int sPaused;
static atomic_int sPendingPauseToggles;
static atomic_int sPlaybackSpeedPercent = 100;
static atomic_int_fast64_t sAudioSettings = 200;
static atomic_int sFirstAudioCallback;
static pthread_mutex_t sAudioCommandLock = PTHREAD_MUTEX_INITIALIZER;
#define MAI_STATUS_STORE64(field, value) atomic_store(&(field), (value))
#define MAI_STATUS_LOAD64(field) atomic_load(&(field))
#define MAI_STATUS_EXCHANGE64(field, value) atomic_exchange(&(field), (value))
#define MAI_STATUS_STORE(field, value) atomic_store(&(field), (value))
#define MAI_STATUS_LOAD(field) atomic_load(&(field))
#define MAI_STATUS_EXCHANGE(field, value) atomic_exchange(&(field), (value))
#endif
// 序号分配与 SDL 入队放在同一临界区，防止并发调用的序号与 FIFO 次序相反。
static uint64_t sAudioIssuedSerial;
static MAI_FFPLAY_THREAD_LOCAL jmp_buf sExitPoint;
static MAI_FFPLAY_THREAD_LOCAL int sExitReady;
static MAI_FFPLAY_THREAD_LOCAL int sDidCleanUp;
// 只有会话的播放线程读取初始设置；read_thread 创建前即复制进 VideoState。
static MAI_FFPLAY_THREAD_LOCAL int sInitialVolumePercent = -1;
static MAI_FFPLAY_THREAD_LOCAL int sInitialMuted;
static void (*sDiagnosticSink)(void* user_data, int level, const char* line);
static void* sDiagnosticUserData;
static int sObservedAudioSerial = -1;
// 仅由播放事件线程更新；目标反馈须等本次 seek 的新 serial 时钟生效后再解除。
static int64_t sAppliedSeekFeedbackUs = -1;
static int sFeedbackVideoSerial = -1;
static int sFeedbackAudioSerial = -1;

static void clearSeekFeedbackIfCurrent(int64_t completed_us) {
#if defined(_WIN32)
    InterlockedCompareExchange64(&sSeekFeedbackUs, -1, completed_us);
#else
    int_fast64_t expected = completed_us;
    atomic_compare_exchange_strong(&sSeekFeedbackUs, &expected, -1);
#endif
}

typedef struct MaiFfplaySeekTarget {
    int serial;
    int64_t timestamp_us;
    int reached;
} MaiFfplaySeekTarget;

// 解复用线程发布目标，音频/视频线程各自按 packet serial 消费。
// 互斥量保护整个目标快照，避免连续 seek 混用新时间和旧 serial。
static SDL_mutex* sSeekMutex;
static MaiFfplaySeekTarget sVideoSeekTarget;
static MaiFfplaySeekTarget sAudioSeekTarget;

struct VideoState;
static int maiFfplaySeekFile(struct VideoState* state, AVFormatContext* context, int stream_index, int64_t minimum,
                             int64_t timestamp, int64_t maximum, int flags);
static int maiFfplayAddFrameAfterSeek(struct VideoState* state, AVFilterContext* source, AVFrame* frame);
static int maiFfplayPeepEventsWithStatus(struct VideoState* state, SDL_Event* events, int count, int action,
                                         Uint32 min_type, Uint32 max_type);
static SDL_Thread* maiFfplayCreateThreadWithAudio(int (*function)(void*), const char* name, void* data);
static SDL_AudioDeviceID maiFfplayOpenAudioWithStatus(const char* device, int capture,
                                                     const SDL_AudioSpec* wanted, SDL_AudioSpec* obtained,
                                                     int allowed_changes);

static void (*maiFfplaySignal(int signal_number, void (*handler)(int)))(int) {
    (void)signal_number;
    (void)handler;
    return SIG_DFL;
}

static void maiFfplayQuit(void) {
    sDidCleanUp = 1;
    SDL_Quit();
}

static MAI_FFPLAY_NORETURN void maiFfplayExit(int status) {
    if (sExitReady) longjmp(sExitPoint, status + 1);
    abort();
}

#define main maiFfplayCliMain
#define exit maiFfplayExit
#define signal maiFfplaySignal
#define SDL_Quit maiFfplayQuit
// ffplay.c 只在事件循环中调用一次 SDL_PeepEvents；
// 在此处采样现有主时钟。精准 seek 在解码后、滤镜重定时前过滤预滚帧，
// 保持 ffplay 的解码依赖、队列、字幕和时钟调度实现不变。
#define SDL_PeepEvents(events, count, action, min_type, max_type) \
    maiFfplayPeepEventsWithStatus(is, events, count, action, min_type, max_type)
#define avformat_seek_file(context, stream, minimum, timestamp, maximum, flags) \
    maiFfplaySeekFile(is, context, stream, minimum, timestamp, maximum, flags)
#define av_buffersrc_add_frame(source, frame) maiFfplayAddFrameAfterSeek(is, source, frame)
// 在 upstream 创建 read_thread 之前恢复音量；返回 stream_open 后再恢复会与
// 解复用线程开启音频设备竞争，导致首批音频使用默认音量或未静音状态。
#define SDL_CreateThread(function, name, data) maiFfplayCreateThreadWithAudio(function, name, data)
#define SDL_OpenAudioDevice(device, capture, wanted, obtained, changes) \
    maiFfplayOpenAudioWithStatus(device, capture, wanted, obtained, changes)
#include "../ffplay.c"
#undef SDL_OpenAudioDevice
#undef SDL_CreateThread
#undef av_buffersrc_add_frame
#undef avformat_seek_file
#undef SDL_PeepEvents
#undef SDL_Quit
#undef signal
#undef exit
#undef main

enum MaiFfplayAudioCommand {
    MAI_AUDIO_SET_VOLUME = 1,
    MAI_AUDIO_SET_MUTED,
    MAI_AUDIO_TOGGLE_MUTED,
    MAI_AUDIO_VOLUME_UP,
    MAI_AUDIO_VOLUME_DOWN
};

static void publishAudioSettings(const VideoState* state, uint64_t serial) {
    const int percent = (state->audio_volume * 100 + SDL_MIX_MAXVOLUME / 2) / SDL_MIX_MAXVOLUME;
    // 一次原子发布两个字段，避免读取到不属于同一设置时刻的音量和静音组合。
    MAI_STATUS_STORE64(sAudioSettings, (int64_t)((serial << 8) | (percent * 2 + !!state->muted)));
}

static SDL_Thread* maiFfplayCreateThreadWithAudio(int (*function)(void*), const char* name, void* data) {
    if (function == read_thread && data) {
        VideoState* state = data;
        if (sInitialVolumePercent >= 0)
            state->audio_volume = (sInitialVolumePercent * SDL_MIX_MAXVOLUME + 50) / 100;
        state->muted = sInitialMuted;
        publishAudioSettings(state, 0);
    }
    return SDL_CreateThread(function, name, data);
}

static void maiFfplayAudioCallbackWithStatus(void* opaque, Uint8* stream, int length) {
    VideoState* state = opaque;
    const int first = MAI_STATUS_EXCHANGE(sFirstAudioCallback, 0);
    const int volume = first ? state->audio_volume : 0;
    const int muted = first ? state->muted : 0;
    sdl_audio_callback(opaque, stream, length);
    if (first) {
        int non_silent = 0;
        for (int index = 0; index < length && !non_silent; ++index) non_silent = stream[index] != 0;
        // 仅首批 PCM 记录真实回调设置和是否有声，用于验证恢复状态没有音量突发。
        // 不记录媒体字节；音量修改仍全部由 FFplay 事件线程执行。
        av_log(NULL, AV_LOG_VERBOSE, "Initial audio callback volume=%d muted=%d non_silent=%d\n",
               (volume * 100 + SDL_MIX_MAXVOLUME / 2) / SDL_MIX_MAXVOLUME, muted, non_silent);
    }
}

static SDL_AudioDeviceID maiFfplayOpenAudioWithStatus(const char* device, int capture,
                                                     const SDL_AudioSpec* wanted, SDL_AudioSpec* obtained,
                                                     int allowed_changes) {
    if (!wanted || wanted->callback != sdl_audio_callback)
        return SDL_OpenAudioDevice(device, capture, wanted, obtained, allowed_changes);
    SDL_AudioSpec spec = *wanted;
    spec.callback = maiFfplayAudioCallbackWithStatus;
    return SDL_OpenAudioDevice(device, capture, &spec, obtained, allowed_changes);
}

static void applyAudioCommand(VideoState* state, int operation, int value, uint64_t serial) {
    // 只有 FFplay 事件线程修改运行中的 VideoState。原有音量步进沿用 upstream
    // 对数增益，精确百分比则用四舍五入映射到 SDL 的 0..128 混音刻度。
    if (operation == MAI_AUDIO_SET_VOLUME)
        state->audio_volume = (value * SDL_MIX_MAXVOLUME + 50) / 100;
    else if (operation == MAI_AUDIO_SET_MUTED)
        state->muted = value;
    else if (operation == MAI_AUDIO_TOGGLE_MUTED)
        toggle_mute(state);
    else if (operation == MAI_AUDIO_VOLUME_UP)
        update_volume(state, 1, SDL_VOLUME_STEP);
    else if (operation == MAI_AUDIO_VOLUME_DOWN)
        update_volume(state, -1, SDL_VOLUME_STEP);
    publishAudioSettings(state, serial);
}

static int maiFfplaySeekFile(VideoState* state, AVFormatContext* context, int stream_index, int64_t minimum,
                             int64_t timestamp, int64_t maximum, int flags) {
    // 必须先回到目标之前的随机访问点，再正常解码；ANY/跳到后方关键帧
    // 都不能提供精准画面。字节 seek 没有可比较的媒体时间，维持原语义。
    const int timed = !(flags & AVSEEK_FLAG_BYTE);
    const int result = avformat_seek_file(context, stream_index, minimum, timestamp,
                                          timed ? FFMIN(maximum, timestamp) : maximum, flags);
    if (result >= 0) {
        SDL_LockMutex(sSeekMutex);
        // ffplay 紧接着 flush 队列；启动 -ss 时 packet_queue_start 同样递增 serial。
        sVideoSeekTarget = (MaiFfplaySeekTarget){state->videoq.serial + 1, timed ? timestamp : AV_NOPTS_VALUE, 0};
        sAudioSeekTarget = (MaiFfplaySeekTarget){state->audioq.serial + 1, timed ? timestamp : AV_NOPTS_VALUE, 0};
        SDL_UnlockMutex(sSeekMutex);
    } else if (timed) {
        // 失败时不能永久钉住滑块。只解除失败的当前目标，保留后续已排队的新请求。
        const int64_t feedback = MAI_STATUS_LOAD64(sSeekFeedbackUs);
        const int64_t start_us = context->start_time == AV_NOPTS_VALUE ? 0 : context->start_time;
        const int64_t target = context->duration > 0 ? FFMIN(feedback, context->duration - 1) : feedback;
        if (feedback >= 0 && timestamp == start_us + target) clearSeekFeedbackIfCurrent(feedback);
    }
    return result;
}

static int maiFfplayAddFrameAfterSeek(VideoState* state, AVFilterContext* source, AVFrame* frame) {
    if (!frame || frame->pts == AV_NOPTS_VALUE) return av_buffersrc_add_frame(source, frame);
    const int audio = source == state->in_audio_filter;
    MaiFfplaySeekTarget* shared = audio ? &sAudioSeekTarget : &sVideoSeekTarget;
    const int serial = audio ? state->auddec.pkt_serial : state->viddec.pkt_serial;
    SDL_LockMutex(sSeekMutex);
    const MaiFfplaySeekTarget target = *shared;
    SDL_UnlockMutex(sSeekMutex);
    if (target.serial != serial || target.timestamp_us == AV_NOPTS_VALUE || target.reached)
        return av_buffersrc_add_frame(source, frame);
    const AVRational time_base = audio ? (AVRational){1, frame->sample_rate} : state->video_st->time_base;
    if (av_compare_ts(frame->pts, time_base, target.timestamp_us, AV_TIME_BASE_Q) < 0) {
        if (audio) {
            const int64_t skip =
                av_rescale_q_rnd(target.timestamp_us, AV_TIME_BASE_Q, time_base, AV_ROUND_UP) - frame->pts;
            if (skip >= frame->nb_samples) {
                av_frame_unref(frame);
                return 0;
            }
            // 裁掉覆盖目标时间的音频帧前缀，保持采样精度和原有声道布局。
            const int planar = av_sample_fmt_is_planar(frame->format);
            const int planes = planar ? frame->ch_layout.nb_channels : 1;
            const int bytes = av_get_bytes_per_sample(frame->format) * (planar ? 1 : frame->ch_layout.nb_channels);
            for (int index = 0; index < planes; ++index) {
                frame->extended_data[index] += skip * bytes;
                if (index < AV_NUM_DATA_POINTERS) frame->data[index] = frame->extended_data[index];
            }
            frame->pts += skip;
            frame->nb_samples -= (int)skip;
        } else {
            // 目标落在一帧覆盖的时间段内时显示该帧，避免文件尾部无后继帧。
            int64_t duration = frame->duration;
            if (duration <= 0) {
                const AVRational rate = av_guess_frame_rate(state->ic, state->video_st, frame);
                if (rate.num > 0 && rate.den > 0) duration = av_rescale_q(1, av_inv_q(rate), time_base);
            }
            if (duration <= 0 ||
                av_compare_ts(frame->pts + duration, time_base, target.timestamp_us, AV_TIME_BASE_Q) <= 0) {
                av_frame_unref(frame);
                return 0;
            }
        }
    }
    SDL_LockMutex(sSeekMutex);
    if (shared->serial == serial) shared->reached = 1;
    SDL_UnlockMutex(sSeekMutex);
    av_log(NULL, AV_LOG_INFO, "Accurate seek %s serial=%d target=%.6fs first=%.6fs\n", audio ? "audio" : "video",
           serial, target.timestamp_us / (double)AV_TIME_BASE, frame->pts * av_q2d(time_base));
    return av_buffersrc_add_frame(source, frame);
}

static int maiFfplayPeepEventsWithStatus(VideoState* state, SDL_Event* events, int count, int action, Uint32 min_type,
                                         Uint32 max_type) {
    if (state && state->ic) {
        const int pauseToggles = MAI_STATUS_EXCHANGE(sPendingPauseToggles, 0);
        for (int index = 0; index < pauseToggles; ++index) toggle_pause(state);
        const int64_t length = state->ic->duration;
        if (state->audio_stream >= 0) {
            if (sObservedAudioSerial >= 0 && sObservedAudioSerial != state->audioq.serial)
                maiFfplayAudioTempoNotifySeek();
            sObservedAudioSerial = state->audioq.serial;
        }
        // 在 FFplay 事件线程执行绝对时间跳转，避免用模拟鼠标点击推算播放位置。
        // 上一次跳转仍在解码时保留新请求，等解码器空闲后再处理。
        if (!state->seek_req) {
            int64_t requested = MAI_STATUS_EXCHANGE64(sPendingSeekUs, -1);
            if (requested >= 0) {
                MAI_STATUS_STORE(sPlaybackEnded, 0);
                sAppliedSeekFeedbackUs = requested;
                sFeedbackVideoSerial = state->videoq.serial + 1;
                sFeedbackAudioSerial = state->audioq.serial + 1;
                // 100% 选中最后一帧，不能跳到已无画面可供 step 的 EOF 时间戳。
                if (length > 0) requested = FFMIN(requested, length - 1);
                const int64_t start_us = state->ic->start_time == AV_NOPTS_VALUE ? 0 : state->ic->start_time;
                av_log(NULL, AV_LOG_INFO, "Seek to %.1f%% at %.3fs\n", length > 0 ? requested * 100.0 / length : 0.0,
                       requested / (double)AV_TIME_BASE);
                stream_seek(state, requested + start_us, 0, 0);
                // 等 FFplay 的新 serial 目标帧触发刷新，避免重画上一次暂停的旧帧。
            }
        }
        // 暂停 seek 的一步画面更新已写入 vidclk；音频或外部主时钟可能还在旧位置。
        const double video_clock = state->video_stream >= 0 ? get_clock(&state->vidclk) : NAN;
        const double clock = state->paused && isfinite(video_clock) ? video_clock : get_master_clock(state);
        const double start =
            state->ic->start_time == AV_NOPTS_VALUE ? 0.0 : state->ic->start_time / (double)AV_TIME_BASE;
        if (length > 0 && isfinite(clock)) {
            // FFplay 的滤镜时钟按墙钟运行；外部播放器进度仍使用源文件时间轴。
            const double speed = MAI_STATUS_LOAD(sPlaybackSpeedPercent) / 100.0;
            const double relative = fmax(0.0, clock * speed - start);
            MAI_STATUS_STORE64(sPositionUs,
                               relative >= length / (double)AV_TIME_BASE ? length : (int64_t)(relative * AV_TIME_BASE));
            const int video_ready =
                state->video_stream < 0 || (isfinite(video_clock) && state->vidclk.serial == sFeedbackVideoSerial);
            const int audio_ready =
                get_master_sync_type(state) != AV_SYNC_AUDIO_MASTER || state->audclk.serial == sFeedbackAudioSerial;
            // 外部时钟在 seek 起始就设为目标，不能据此认定新画面已就绪。
            // 播放时还要等主时钟追到目标，避免音频设备延迟造成滑块短暂倒退。
            if (sAppliedSeekFeedbackUs >= 0 && !state->seek_req && video_ready &&
                ((state->paused && state->video_stream >= 0) ||
                 (audio_ready && relative * AV_TIME_BASE >= sAppliedSeekFeedbackUs))) {
                // 暂停定位到 100% 时，尾部可能还留有不足一帧的音频采样，
                // 它们要等恢复才会消费。末帧已呈现即可确认结尾，不等待这些采样。
                if (length > 0 && state->paused && sAppliedSeekFeedbackUs >= length)
                    MAI_STATUS_STORE(sPlaybackEnded, 1);
                // 先发布有效位置，再解除反馈；CAS 防止旧 seek 清掉新的用户目标。
                clearSeekFeedbackIfCurrent(sAppliedSeekFeedbackUs);
                sAppliedSeekFeedbackUs = -1;
            }
        }
        // FFplay 非 autoexit 模式耗尽队列后仍保留最后一帧和运行中的时钟。
        // 普通单次播放在这里稳定停到结尾；回拖沿用暂停 seek 的单帧路径，
        // 避免 EOF 旧时钟驱动新目标画面继续跑动或丢帧。显式循环保持上游语义。
        if (loop == 1 && !autoexit && state->eof && !state->seek_req && MAI_STATUS_LOAD64(sSeekFeedbackUs) < 0 &&
            (state->audio_st || state->video_st) &&
            (!state->audio_st ||
             (state->auddec.finished == state->audioq.serial && frame_queue_nb_remaining(&state->sampq) == 0)) &&
            (!state->video_st ||
             (state->viddec.finished == state->videoq.serial && frame_queue_nb_remaining(&state->pictq) == 0))) {
            if (!state->paused) stream_toggle_pause(state);
            MAI_STATUS_STORE(sPlaybackEnded, 1);
        }
        if (MAI_STATUS_LOAD(sPlaybackEnded) && length > 0) MAI_STATUS_STORE64(sPositionUs, length);
        MAI_STATUS_STORE64(sDurationUs, length > 0 ? length : 0);
        MAI_STATUS_STORE(sPaused, state->paused ? 1 : 0);
        // 原生键盘事件也可能进入 upstream 的音量分支；快照始终反映实际状态。
        publishAudioSettings(state, (uint64_t)MAI_STATUS_LOAD64(sAudioSettings) >> 8);
    }
    int received;
    while ((received = SDL_PeepEvents(events, count, action, min_type, max_type)) > 0) {
        // upstream 每次取一个事件。音量事件在同一 FIFO 内消费，既不跨线程
        // 裸写 VideoState，也不依赖只在存在视频窗口时处理的键盘分支。
        if (!state || count != 1 || events[0].type != SDL_USEREVENT + 3) return received;
        const uint64_t serial = (uint32_t)(uintptr_t)events[0].user.data1 |
                                ((uint64_t)(uint32_t)(uintptr_t)events[0].user.data2 << 32);
        applyAudioCommand(state, events[0].user.code / 128, events[0].user.code % 128, serial);
    }
    return received;
}

int maiFfplayGetPlaybackStatus(MaiFfplayPlaybackStatus* status) {
    if (!status) return 0;
    // 先读取反馈，再读取真实位置；反馈已清除时，位置已由事件线程发布。
    const int64_t feedback = MAI_STATUS_LOAD64(sSeekFeedbackUs);
    status->running = MAI_STATUS_LOAD(sPlaybackActive);
    status->position_us = MAI_STATUS_LOAD64(sPositionUs);
    status->duration_us = MAI_STATUS_LOAD64(sDurationUs);
    status->paused = MAI_STATUS_LOAD(sPaused);
    status->speed_percent = MAI_STATUS_LOAD(sPlaybackSpeedPercent);
    status->seeking = feedback >= 0;
    status->seek_target_us = feedback >= 0 ? feedback : 0;
    return status->running;
}

int maiFfplayGetAudioStatus(MaiFfplayAudioStatus* status) {
    if (!status || status->struct_size < sizeof(status->struct_size)) return -1;
    const uint64_t snapshot = (uint64_t)MAI_STATUS_LOAD64(sAudioSettings);
    const int settings = snapshot & 255;
    if (status->struct_size >= offsetof(MaiFfplayAudioStatus, volume_percent) + sizeof(status->volume_percent))
        status->volume_percent = settings / 2;
    if (status->struct_size >= offsetof(MaiFfplayAudioStatus, muted) + sizeof(status->muted))
        status->muted = settings % 2;
    if (status->struct_size >= offsetof(MaiFfplayAudioStatus, applied_serial) + sizeof(status->applied_serial))
        status->applied_serial = snapshot >> 8;
    return MAI_STATUS_LOAD(sPlaybackActive);
}

static int queueAudioCommand(int operation, int value, uint64_t* receipt) {
    if (receipt) *receipt = 0;
#if defined(_WIN32)
    AcquireSRWLockExclusive(&sAudioCommandLock);
#else
    pthread_mutex_lock(&sAudioCommandLock);
#endif
    int result = 0;
    SDL_Event event = {0};
    event.type = SDL_USEREVENT + 3;
    event.user.code = operation * 128 + value;
    // 低 8 位存音量和静音，其余正值位存回执；耗尽时明确拒绝而不回绕。
    if (MAI_STATUS_LOAD(sPlaybackActive) && sAudioIssuedSerial < ((uint64_t)INT64_MAX >> 8)) {
        const uint64_t serial = sAudioIssuedSerial + 1;
        event.user.data1 = (void*)(uintptr_t)(uint32_t)serial;
        event.user.data2 = (void*)(uintptr_t)(uint32_t)(serial >> 32);
        if (SDL_PushEvent(&event) > 0) {
            sAudioIssuedSerial = serial;
            if (receipt) *receipt = serial;
            result = 1;
        }
    }
#if defined(_WIN32)
    ReleaseSRWLockExclusive(&sAudioCommandLock);
#else
    pthread_mutex_unlock(&sAudioCommandLock);
#endif
    return result;
}

int maiFfplaySetVolumePercent(int volume_percent) {
    return maiFfplaySetVolumePercentWithReceipt(volume_percent, NULL);
}

int maiFfplaySetVolumePercentWithReceipt(int volume_percent, uint64_t* receipt) {
    if (receipt) *receipt = 0;
    if (volume_percent < 0 || volume_percent > 100) return -1;
    return queueAudioCommand(MAI_AUDIO_SET_VOLUME, volume_percent, receipt);
}

int maiFfplaySetMuted(int muted) {
    return maiFfplaySetMutedWithReceipt(muted, NULL);
}

int maiFfplaySetMutedWithReceipt(int muted, uint64_t* receipt) {
    if (receipt) *receipt = 0;
    if (muted != 0 && muted != 1) return -1;
    return queueAudioCommand(MAI_AUDIO_SET_MUTED, muted, receipt);
}

static void maiFfplayLogCallback(void* context, int level, const char* format, va_list arguments) {
    if (sDiagnosticSink) {
        char line[1024];
        int print_prefix = 1;
        va_list copied;
        va_copy(copied, arguments);
        av_log_format_line2(context, level, format, copied, line, sizeof(line), &print_prefix);
        va_end(copied);
        sDiagnosticSink(sDiagnosticUserData, level, line);
    }
    av_log_default_callback(context, level, format, arguments);
}

void maiFfplaySetDiagnosticSink(void (*sink)(void*, int, const char*), void* user_data) {
    sDiagnosticUserData = user_data;
    sDiagnosticSink = sink;
    av_log_set_callback(sink ? maiFfplayLogCallback : av_log_default_callback);
}

static void resetFfplayOptions(void) {
    file_iformat = NULL;
    input_filename = NULL;
    window_title = NULL;
    default_width = 640;
    default_height = 480;
    screen_width = screen_height = 0;
    screen_left = SDL_WINDOWPOS_CENTERED;
    screen_top = SDL_WINDOWPOS_CENTERED;
    audio_disable = video_disable = subtitle_disable = 0;
    for (int index = 0; index < AVMEDIA_TYPE_NB; ++index) wanted_stream_spec[index] = NULL;
    seek_by_bytes = -1;
    seek_interval = 10;
    display_disable = borderless = alwaysontop = 0;
    startup_volume = 100;
    show_status = -1;
    av_sync_type = AV_SYNC_AUDIO_MASTER;
    start_time = duration = AV_NOPTS_VALUE;
    fast = genpts = lowres = 0;
    decoder_reorder_pts = -1;
    autoexit = exit_on_keydown = exit_on_mousedown = 0;
    loop = 1;
    framedrop = infinite_buffer = -1;
    show_mode = SHOW_MODE_NONE;
    audio_codec_name = subtitle_codec_name = video_codec_name = NULL;
    rdftspeed = 0.02;
    cursor_last_shown = cursor_hidden = 0;
    vfilters_list = NULL;
    nb_vfilters = 0;
    afilters = NULL;
    autorotate = find_stream_info = 1;
    filter_nbthreads = enable_vulkan = 0;
    vulkan_params = video_background = NULL;
    hwaccel = NULL;
    is_full_screen = 0;
    audio_callback_time = 0;
    window = NULL;
    renderer = NULL;
    renderer_info = (SDL_RendererInfo){0};
    audio_dev = 0;
    vk_renderer = NULL;
}

static int runWithAudio(const MaiFfplayHost* host, int argc, char** argv,
                        const MaiFfplayAudioOptions* options) {
    if (!host || !host->graphics_view_id || argc < 2 || !argv) return -1;
#if defined(_WIN32)
    if (InterlockedCompareExchange(&sRunning, 1, 0) != 0) return -1;
#else
    if (atomic_flag_test_and_set(&sRunning)) return -1;
#endif
    resetFfplayOptions();
    sInitialVolumePercent = options ? options->volume_percent : -1;
    sInitialMuted = options ? options->muted : 0;
    sSeekMutex = SDL_CreateMutex();
    if (!sSeekMutex) {
#if defined(_WIN32)
        InterlockedExchange(&sRunning, 0);
#else
        atomic_flag_clear(&sRunning);
#endif
        return -1;
    }
    sVideoSeekTarget = (MaiFfplaySeekTarget){-1, AV_NOPTS_VALUE, 0};
    sAudioSeekTarget = (MaiFfplaySeekTarget){-1, AV_NOPTS_VALUE, 0};
    MAI_STATUS_STORE64(sPositionUs, 0);
    MAI_STATUS_STORE64(sDurationUs, 0);
    MAI_STATUS_STORE64(sPendingSeekUs, -1);
    MAI_STATUS_STORE64(sSeekFeedbackUs, -1);
    sAppliedSeekFeedbackUs = -1;
    sFeedbackVideoSerial = sFeedbackAudioSerial = -1;
    sObservedAudioSerial = -1;
    MAI_STATUS_STORE(sPaused, 0);
    MAI_STATUS_STORE(sPlaybackEnded, 0);
    MAI_STATUS_STORE(sPendingPauseToggles, 0);
    MAI_STATUS_STORE64(sAudioSettings, (options ? options->volume_percent : 100) * 2 + sInitialMuted);
    MAI_STATUS_STORE(sFirstAudioCallback, 1);
    MAI_STATUS_STORE(sPlaybackActive, 1);
    maiFfplayBindHost(host);
    sExitReady = 1;
    sDidCleanUp = 0;
    const int jumped = setjmp(sExitPoint);
    int result = jumped ? jumped - 1 : maiFfplayCliMain(argc, argv);
    sExitReady = 0;
    if (!sDidCleanUp) {
        uninit_opts();
        av_freep(&input_filename);
        avformat_network_deinit();
        SDL_Quit();
    }
    maiFfplayBindHost(NULL);
    SDL_DestroyMutex(sSeekMutex);
    sSeekMutex = NULL;
    MAI_STATUS_STORE(sPlaybackActive, 0);
    MAI_STATUS_STORE(sPlaybackEnded, 0);
    MAI_STATUS_STORE64(sPendingSeekUs, -1);
    MAI_STATUS_STORE64(sSeekFeedbackUs, -1);
    MAI_STATUS_STORE(sPendingPauseToggles, 0);
#if defined(_WIN32)
    InterlockedExchange(&sRunning, 0);
#else
    atomic_flag_clear(&sRunning);
#endif
    return result;
}

int maiFfplayRun(const MaiFfplayHost* host, int argc, char** argv) {
    return runWithAudio(host, argc, argv, NULL);
}

int maiFfplayRunAtSpeed(const MaiFfplayHost* host, int argc, char** argv, double speed, int64_t start_position_us) {
    return maiFfplayRunAtSpeedWithAudio(host, argc, argv, speed, start_position_us, NULL);
}

int maiFfplayRunAtSpeedWithAudio(const MaiFfplayHost* host, int argc, char** argv, double speed,
                                int64_t start_position_us, const MaiFfplayAudioOptions* audio_options) {
    MaiFfplayAudioOptions audio = {sizeof(MaiFfplayAudioOptions), 100, 0};
    if (audio_options) {
        if (audio_options->struct_size < sizeof(audio_options->struct_size)) return -1;
        if (audio_options->struct_size >=
            offsetof(MaiFfplayAudioOptions, volume_percent) + sizeof(audio_options->volume_percent))
            audio.volume_percent = audio_options->volume_percent;
        if (audio_options->struct_size >= offsetof(MaiFfplayAudioOptions, muted) + sizeof(audio_options->muted))
            audio.muted = audio_options->muted;
        if (audio.volume_percent < 0 || audio.volume_percent > 100 || (audio.muted != 0 && audio.muted != 1)) return -1;
    }
    const char* rate = NULL;
    int percent = 100;
    if (fabs(speed - 0.25) < 0.0001) {
        rate = "0.25";
        percent = 25;
    } else if (fabs(speed - 0.5) < 0.0001) {
        rate = "0.5";
        percent = 50;
    } else if (fabs(speed - 1.0) < 0.0001) {
        rate = "1";
        percent = 100;
    } else if (fabs(speed - 1.5) < 0.0001) {
        rate = "1.5";
        percent = 150;
    } else if (fabs(speed - 2.0) < 0.0001) {
        rate = "2";
        percent = 200;
    }
    if (!rate || !host || !argv || argc < 2 || argc > 9 || start_position_us < 0 || MAI_STATUS_LOAD(sPlaybackActive))
        return -1;
    char seek[48];
    char video_filter[64];
    char* options[16];
    int count = 0;
    for (int index = 0; index < argc - 1; ++index) options[count++] = argv[index];
    if (start_position_us > 0) {
        snprintf(seek, sizeof(seek), "%lld.%06lld", (long long)(start_position_us / 1000000),
                 (long long)(start_position_us % 1000000));
        options[count++] = "-ss";
        options[count++] = seek;
    }
    if (percent != 100) {
        snprintf(video_filter, sizeof(video_filter), "setpts=PTS/%s", rate);
        options[count++] = "-vf";
        options[count++] = video_filter;
        // 视频时间轴按倍速缩放；音频由输出设备上的 SoundTouch.setTempo
        // 处理 PCM，并以视频为同步主时钟，避免重新编码或改变音高。
        options[count++] = "-sync";
        options[count++] = "video";
    }
    options[count++] = argv[argc - 1];
    MAI_STATUS_STORE(sPlaybackSpeedPercent, percent);
    // 未提供扩展设置时保留原 argv 的 -volume；不能用默认 100% 覆盖旧 API。
    const int result = runWithAudio(host, count, options, audio_options ? &audio : NULL);
    MAI_STATUS_STORE(sPlaybackSpeedPercent, 100);
    return result;
}

void maiFfplayRequestQuit(void) {
    SDL_Event event = {0};
    event.type = SDL_USEREVENT + 2;
    SDL_PushEvent(&event);
}

int maiFfplaySendCommand(const char* command) {
    if (!command) return -1;
    if (!strcmp(command, "pause")) return maiFfplayRequestPauseToggle();
    if (!strcmp(command, "mute")) return queueAudioCommand(MAI_AUDIO_TOGGLE_MUTED, 0, NULL);
    if (!strcmp(command, "volume_up")) return queueAudioCommand(MAI_AUDIO_VOLUME_UP, 0, NULL);
    if (!strcmp(command, "volume_down")) return queueAudioCommand(MAI_AUDIO_VOLUME_DOWN, 0, NULL);
    int64_t relative_us = 0;
    if (!strcmp(command, "seek_forward"))
        relative_us = 10 * (int64_t)AV_TIME_BASE;
    else if (!strcmp(command, "seek_backward"))
        relative_us = -10 * (int64_t)AV_TIME_BASE;
    else if (!strcmp(command, "seek_minute_forward"))
        relative_us = 60 * (int64_t)AV_TIME_BASE;
    else if (!strcmp(command, "seek_minute_backward"))
        relative_us = -60 * (int64_t)AV_TIME_BASE;
    if (relative_us) {
        if (!MAI_STATUS_LOAD(sPlaybackActive)) return 0;
        const int64_t duration_us = MAI_STATUS_LOAD64(sDurationUs);
        if (duration_us <= 0) return 0;
        // 控件/Agent 的相对跳转同样使用源时间，不把倍速时钟或字节位置当作时间。
        const int64_t pending_us = MAI_STATUS_LOAD64(sPendingSeekUs);
        const int64_t position_us = pending_us >= 0 ? pending_us : MAI_STATUS_LOAD64(sPositionUs);
        return maiFfplaySeekAbsoluteUs(av_clip64(position_us + relative_us, 0, duration_us));
    }
    if (!strcmp(command, "close")) {
        SDL_Event event = {0};
        event.type = SDL_USEREVENT + 2;
        return SDL_PushEvent(&event) > 0 ? 1 : 0;
    }
    struct {
        const char* command;
        int key;
    } keys[] = {
        {"step", SDLK_s},         {"next_chapter", SDLK_PAGEUP}, {"previous_chapter", SDLK_PAGEDOWN},
        {"next_audio", SDLK_a},   {"next_video", SDLK_v},        {"next_subtitle", SDLK_t},
        {"next_program", SDLK_c}, {"next_filter", SDLK_w},       {"fullscreen", SDLK_f},
    };
    for (size_t index = 0; index < sizeof(keys) / sizeof(keys[0]); ++index) {
        if (strcmp(command, keys[index].command)) continue;
        SDL_Event event = {0};
        event.type = SDL_KEYDOWN;
        event.key.keysym.sym = keys[index].key;
        return SDL_PushEvent(&event) > 0 ? 1 : 0;
    }
    return -1;
}

int maiFfplayRequestPauseToggle(void) {
    if (!MAI_STATUS_LOAD(sPlaybackActive)) return 0;
    // 结尾的“播放”重新开始；先入队回到零点，再在同一事件轮恢复播放。
    if (MAI_STATUS_LOAD(sPlaybackEnded) && maiFfplaySeekAbsoluteUs(0) != 1) return 0;
#if defined(_WIN32)
    InterlockedIncrement(&sPendingPauseToggles);
#else
    atomic_fetch_add(&sPendingPauseToggles, 1);
#endif
    return 1;
}

int maiFfplaySeekAbsoluteUs(int64_t position_us) {
    if (position_us < 0) return -1;
    if (!MAI_STATUS_LOAD(sPlaybackActive)) return 0;
    const int64_t length = MAI_STATUS_LOAD64(sDurationUs);
    if (length > 0 && position_us > length) return -1;
    // 在入队前发布用户目标，两端 UI 在异步 seek 窗口显示它而不是旧时钟。
    MAI_STATUS_STORE64(sSeekFeedbackUs, position_us);
    MAI_STATUS_STORE64(sPendingSeekUs, position_us);
    return 1;
}

int maiFfplaySeekPercent(double fraction) {
    if (!(fraction >= 0.0 && fraction <= 1.0)) return -1;
    const int64_t length = MAI_STATUS_LOAD64(sDurationUs);
    if (length <= 0) return 0;
    return maiFfplaySeekAbsoluteUs((int64_t)(fraction * length));
}
