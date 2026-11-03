// 文件作用：声明包装 ffplay 的启动、停止、跳转和播放器状态，把宿主命令交给播放循环所需的接口和数据结构。
// 调用边界：平台实现与上层调用方通过本文件约定参数、所有权和释放方式。
#pragma once

#include "MaiFfplayHost.h"

#ifdef __cplusplus
extern "C" {
#endif

// 在调用方的播放线程阻塞，直到宿主 ffplay 会话关闭。
// 实现包含的 ffplay.c 与上游字节一致；进程退出和信号处理在此层拦截。
MAI_FFPLAY_EXPORT int maiFfplayRun(const MaiFfplayHost* host, int argc, char** argv);
// 以新倍速重新打开同一宿主播放器。调用方先停止并等待旧会话退出，
// 再把原媒体位置作为 start_position_us 传入。视频 PTS 和不变调音频一起重定时，
// 不创建输出文件。支持 0.25、0.5、1、1.5、2 倍速；上游 ffplay.c 保持原样。
MAI_FFPLAY_EXPORT int maiFfplayRunAtSpeed(const MaiFfplayHost* host, int argc, char** argv, double speed,
                                          int64_t start_position_us);
MAI_FFPLAY_EXPORT void maiFfplayRequestQuit(void);
// 将 Agent/MaiChat 指令映射到 ffplay 现有控制逻辑。
// 入队返回 1，播放器未运行返回 0，未知指令返回 -1。
MAI_FFPLAY_EXPORT int maiFfplaySendCommand(const char* command);
// 音频 -nodisp 模式没有视频窗口；由 FFplay 事件线程直接切换暂停状态。
MAI_FFPLAY_EXPORT int maiFfplayRequestPauseToggle(void);
// 请求从文件起点起算的源媒体时间（微秒，与倍速无关）。线程安全、异步，
// 返回 1 仅表示已入队，0 表示未运行，-1 表示负值或超过已知时长。
// 精准跳转先定位之前的关键帧，解码预滚到覆盖目标时间的画面，音频裁到目标采样；
// 误差受源视频帧间隔限制。保留原暂停状态，连续请求最终采用最后一次目标。
// 请求恰好等于时长时显示最后一帧。缺失时间戳的流保留 FFplay 的原始行为。
MAI_FFPLAY_EXPORT int maiFfplaySeekAbsoluteUs(int64_t position_us);
// fraction 范围 [0, 1]，与绝对时间接口使用相同的解码预滚规则。
MAI_FFPLAY_EXPORT int maiFfplaySeekPercent(double fraction);
typedef struct MaiFfplayPlaybackStatus {
    int64_t position_us;
    int64_t duration_us;
    int running;
    int paused;
    int speed_percent;
    // 请求已接受，但新队列的画面/播放时钟尚未生效。仅用于 UI 反馈；
    // position_us 仍是真实采样位置，不能把目标反馈当成 seek 已完成的证据。
    int seeking;
    // seeking 非零时有效，单位为源时间微秒。新请求覆盖旧请求，失败或停止时清除。
    int64_t seek_target_us;
} MaiFfplayPlaybackStatus;
// 在 ffplay 播放／事件线程采样的线程安全状态快照。
MAI_FFPLAY_EXPORT int maiFfplayGetPlaybackStatus(MaiFfplayPlaybackStatus* status);
// 接收播放工作线程的诊断信息。user_data 由调用方持有；
// maiFfplayRun 返回后必须清除诊断接收回调。
MAI_FFPLAY_EXPORT void maiFfplaySetDiagnosticSink(void (*sink)(void* user_data, int level, const char* line),
                                                  void* user_data);

#ifdef __cplusplus
}
#endif
