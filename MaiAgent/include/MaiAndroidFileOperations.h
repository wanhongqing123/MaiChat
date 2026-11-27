// Android 普通文件发布与内核不支持时的既有宿主兼容入口。
#pragma once

#if defined(__ANDROID__)

// The callback receives existing source and absent destination paths as native UTF-8 bytes.
// It may run on any MaiAgent worker thread and must block until publication finishes.
// Return 0 on success or an errno value on failure. A failure must leave the source in place;
// an existing destination must be rejected. The Android Java implementation serializes calls
// within this process and is for App-private paths only. Java Files.move does not guarantee
// no-replace against another process or any writer that bypasses this callback. The host
// registers before creating an agent and keeps the callback valid for the process lifetime.
using MaiAndroidPublishNewFileCallback = int (*)(const char* source, const char* destination);

void maiSetAndroidPublishNewFileCallback(MaiAndroidPublishNewFileCallback callback);

// 优先探测公开 libc 的原子 NOREPLACE 接口（NDK 标为 API30 起），不强链接新版符号。
// 源必须是非链接普通文件；缺少该接口时使用已安装宿主适配，没有适配则明确失败。
// 已有目标、权限拒绝、跨文件系统等错误不回退；仅不支持的原子操作可交既有宿主回调。
// 没有兼容回调时返回 ENOSYS。成功消费源路径；返回值为 0 或原始 errno。
int maiAndroidPublishNewFile(const char* source, const char* destination);

#endif
