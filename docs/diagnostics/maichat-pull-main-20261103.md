# 合并最新 main（2026-11-03）

## 请求与基线

用户要求拉取最新代码。工作区初始干净，当前为本地 `main`，HEAD `7f31f399`；fetch 后远端 `origin/main` 为 `ecef0b19`，包含5个新提交及 `v2.2.34` 标签。本地6个未推送提交必须保留，不使用reset、重写历史或替换已安装App。

预期：本地main包含远端最新提交和本地Android修复；无未解决冲突，编译核对双方接口兼容。只更新源码，不重新签包、安装或发布。

## 冲突与处理

`git merge --no-commit origin/main` 出现5个冲突文件：Android版本、MaiChatIMTools、AIAssistantPanel、RemoteIMSessionController、TencentIMClient。

- 版本取远端较大 `versionCode=246`，保留本地较大 `versionName=2.2.9`，避免代码和显示版本回退；未制作该版本安装包。
- 创作欢迎页保留用户已确认的只读卡片、无选择态、转写输入框测量和手势。双语音凭据缓存及已验证curl修复保留。
- 头像收敛到远端跨平台 `MaiAccountTools` / `MaiAccountAvatar` / 共享C++客户端；旧UI方法仅做适配，不保留第二条实际上传路径或第二套busy门禁。
- 工具采用远端正式 `maichat_set_avatar{path}` 及request/cancel协议；删除自动合并留下的Android旧 `{file_path}` 同名注册覆盖。保留本地注销表单和只读举报工具。
- SDK登录代际、头像提交revision和主线程读取护栏保留；已受理注销的本地账号清理逻辑保留。

## 编译与定点失败

首次合并后JVM运行331项，1项失败：远端新增 `MaiAccountToolsTest.rejectsSiblingWorkspaceAndSymlinkEscapes`。Windows JDK17 的 `File.getCanonicalFile()` 未解析该文件符号链接，导致实际指向工作区外的链接被错误接受。最小探针确认符号链接为真、canonical仍在工作区内而real path在外；使用真实路径解析修复该具体边界，并用同一测试回归。

完整ARM64原生目标在FFmpeg重新生成步骤被本机缺少 `bash` 阻断，日志 `.cache/merge-latest-20261103-native.log`。未为纯拉取任务改动系统构建环境，也未声称完整原生链接成功。使用已生成的实际NDK编译参数分别编译 `MaiAgentJni.cpp`、`MaiAccountTools.cpp`、`MaiMobileHostTools.cpp`、`MaiAvatarClient.cpp`，4个对象全部通过；这只覆盖桥接与注册编译，不替代完整链接或真机验收。

最终 `testDebugUnitTest` 运行83套/331项，0失败、0错误、0跳过；`compileDebugAndroidTestJavaWithJavac` 和 `compileReleaseJavaWithJavac` 均通过。路径校验改用 `Path.toRealPath()`，同一失败用例恢复通过，同时覆盖合法内部文件链接和越界目录链接。原生注册和头像事务的限定交叉审查未发现阻塞或重复实际上传路径。

合并提交以 `7f31f399` 和 `ecef0b19` 为父节点，包含本文记录及定点Windows兼容修正；未重写本地6个提交、未推送远端。当前手机仍运行此前已验证的2.2.9(241)，本次未操作手机或发布Release。
