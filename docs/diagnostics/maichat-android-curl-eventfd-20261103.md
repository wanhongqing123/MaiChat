# Android curl threaded resolver eventfd 重复关闭（2026-11-03）

## 现场与复现

2026-11-03 09:34，OPPO PKD130 / Android 15 / ARM64，原位更新的正常 Release 2.2.9(241)。启动后后台头像下载触发 native SIGABRT，PID 4083，线程 pool-8-thread-1。此时不能把页面退出误判成用户主动切换应用，也不能据此前语音/UI通过宣布本轮交付完成。

关键错误：`fdsan: attempted to close file descriptor 261, expected to be unowned, actually owned by unique_fd`。日志只保留在本机 `.cache/android-settings-speech/fd-crash.txt`；不保存HTTP请求正文、URL或凭据。

初次出错候选 APK SHA-256：`893aeadbd2cc7fb42de6db16d0e8b38905f95128e42d12a50c2558ae215b8521`。`libmaichat_agent.so` Build ID `6f1b4ddb4e38ed4ff3215a631b0256cb35edf487` 与本机符号匹配，调用栈为 `MaiChatHttpTransport.getAvatar` → native HTTP → `curl_easy_perform` → `curl_multi_perform` → resolver 清理。

## 决定性根因证据

NDK addr2line 将 `0xef40f4` 定位到 `MaiAgent/third_party/curl/lib/asyn-thread.c` 的 `destroy_async_data()` 中 `wakeup_close(sock_rd)`。Android 构建启用 `HAVE_EVENTFD`；`socketpair.c` 为读写两槽返回同一个eventfd。

旧路径先在 `destroy_thread_sync_data()` 关闭写槽，再在 `destroy_async_data()` 关闭保存的读槽。eventfd情况下两个数字相同，因此第二次close可能关闭两次close之间刚被系统重新分配的文件描述符。现场的 unique_fd ownership abort 与该路径一致；缺陷不在头像图片内容或Java UI。

## 修复范围与上游依据

该路径对应 curl 官方 [CVE-2025-0665](https://curl.se/docs/CVE-2025-0665.html)。回移官方
[`ff5091aa9f73802e894b1cbdf24ab84e103200e2`](https://github.com/curl/curl/commit/ff5091aa9f73802e894b1cbdf24ab84e103200e2)：
`destroy_thread_sync_data()` 在 `USE_EVENTFD` 下不关闭写槽，由父线程关闭唯一的读槽；
pipe/socketpair 仍各关闭两个独立端点。另外在 `init_thread_sync_data()` 的 `memset`
后立即把两个槽位初始化为 `CURL_SOCKET_BAD`，避免 mutex 分配失败时把 fd 0 当作已拥有的描述符。

本地补丁约定记入 `MaiAgent/third_party/README.md`。没有关闭 fdsan、修改全局 eventfd
创建方式、降低证书校验或更换 HTTP 实现。回归源码基线为
`b8234d710466e5d31a1ebf1b816966ece2d2644d`；修复版取此基线加本次工作区补丁。

## 确定性 Android 描述符复用回归

构建脚本 `MaiAgent/tests/test_curl_resolver_fd_ownership.py` 分别从指定旧提交和当前
`asyn-thread.c` 抽取真实的 `destroy_thread_sync_data()`、`init_thread_sync_data()`、
`destroy_async_data()` 函数体，插入 `MaiAgent/tests/fixtures/curl_resolver_fd_ownership.c`。
每次首次关闭被测 fd 后，fixture 立即打开 `/dev/null` 并用 `dup2` 复用同一个描述符数字；
随后通过 `fcntl` 检查新所有者是否仍存活。两种构建分别使用真实 eventfd 和真实 pipe。
全程不访问网络、账号、模型或用户文件。

Windows / NDK 27.3 构建命令（仓库根目录执行）：

```powershell
$curlNdkRoot = 'C:/Users/18034/AppData/Local/Android/Sdk/ndk/27.3.13750724/toolchains/llvm/prebuilt/windows-x86_64'
$curlProbeOutput = '.cache/android-settings-speech/curl-fd-regression'
python MaiAgent/tests/test_curl_resolver_fd_ownership.py `
  --compiler "$curlNdkRoot/bin/clang.exe" --sysroot "$curlNdkRoot/sysroot" `
  --output-dir $curlProbeOutput `
  --baseline-ref b8234d710466e5d31a1ebf1b816966ece2d2644d
```

脚本只生成 ARM64 Android 可执行文件。由设备操作方在目标 OPPO 上执行：

```powershell
foreach ($curlProbe in @('old-eventfd', 'old-pipe', 'fixed-eventfd', 'fixed-pipe')) {
  adb push "$curlProbeOutput/curl-resolver-$curlProbe" "/data/local/tmp/curl-resolver-$curlProbe"
  adb shell chmod 700 "/data/local/tmp/curl-resolver-$curlProbe"
  adb shell "/data/local/tmp/curl-resolver-$curlProbe"
  Write-Output "EXIT=$LASTEXITCODE"
}
```

主执行者已在同一台 OPPO PKD130 / Android 15 上实际执行四个 binary，结果如下：

| 清理分支 | 旧 eventfd | 旧 pipe | 修复 eventfd | 修复 pipe |
|---|---|---|---|---|
| DNS 正常完成后清理 | FAIL | PASS | PASS | PASS |
| 父线程分离后，resolver 回收自身数据 | FAIL | PASS | PASS | PASS |
| hostname 分配失败 | FAIL | PASS | PASS | PASS |
| mutex 早期分配失败，尚未创建 fd | FAIL | FAIL | PASS | PASS |
| binary 退出码 | 1 | 1 | 0 | 0 |

旧 eventfd 前三条路径均执行两次 close，强制复用后的新所有者已被关闭；旧 pipe 的两个
端点独立，因此前三条保持通过。旧实现早期分配失败均误关 fd 0。修复后 eventfd 前三条
只执行一次 close，pipe 仍执行两次；新所有者均存活，正常完成路径也保持通知先于读 fd
关闭。早期分配失败两种构建都执行零次 close，fd 0 保持存活。

设备原始输出保存在本机以下四份文件中，不纳入源码提交：

- `.cache/android-settings-speech/curl-fd-regression/old-eventfd-device.txt`
- `.cache/android-settings-speech/curl-fd-regression/old-pipe-device.txt`
- `.cache/android-settings-speech/curl-fd-regression/fixed-eventfd-device.txt`
- `.cache/android-settings-speech/curl-fd-regression/fixed-pipe-device.txt`

## 验证边界

上述新旧对照覆盖真实 resolver 清理函数中的所有权分支和 Android 内核 fd 复用。
线程、互斥锁与 `Curl_multi_closed` 通知由 fixture 替身提供；eventfd、pipe、close、dup2
及存活检查使用真实系统调用。因此它不是完整 DNS/HTTP/TLS 集成测试，也不替代正常 App
的头像下载、冷启动与 ASR 实机回归。最终 APK/native Build ID、安装核对和正常 App 复验
记录如下。

## 正常 App 最终复验

- curl修复提交 `e0feeefe`；最终Android应用源码提交 `30f32eec7f347c4556427219918d980d8102b236`，之后仅补充验收记录。
- 新仓库ARM64 `maichat_agent` 重新编译 `asyn-thread.c.o`、`libcurl.a` 并重链共享库成功；新Build ID `951ef201cafd8bf0f8691572c5da0d76d8c5a4c4`。
- 最终正常Release 2.2.9(241) APK SHA-256 `f864e6eafb4f2910cb3741e29306879d4eae7f81008c41322e730997a913492a`，100208340 B，09:41:36同包原位安装。手机拉回的base.apk哈希相同，确认不是仍运行出错候选。
- 连续三次冷启动及随后正常启动、头像展示、页面切换和两家ASR复验，安装后crash buffer无新记录。最终PID9842，腾讯SDK09:45:48登录成功；09:47实际页面显示消息已连接，用户头像和聊天保留。
- 最终产物上阿里云真实PCM/partial/final→编辑回填完整两行；腾讯云IM真实AAC/partial→取消，未发送测试消息。
- 完整构建/安装路径、UI截图、306项JVM测试和未覆盖范围见同日 `maichat-android-settings-speech-20261103.md`。未在本轮重新构建其他平台或MaiAgent宿主核心测试套件，不以Android定点用例代替那些平台的验收。
