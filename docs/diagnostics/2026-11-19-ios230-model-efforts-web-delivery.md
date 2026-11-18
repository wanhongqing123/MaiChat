# iOS 3.0.1(230) 安装与启动边界

产品源码：`f785e2c2bae85129774686125828d85b409fa82f`。本端 FF 前逐 SHA 核对 23 个已交付文件；仅接受协调端已说明的诊断隐私措辞调整。既有备份保留，产品源码工作区 clean 后开始构建。

## 正常产物与安装

- 正常 workspace/scheme 的 iphoneos Release 构建 exit 0，142.543 秒；版本 3.0.1，build 230，bundle `com.kongshang.maichat`。
- Deep/strict 签名验证 exit 0，签名 Team 与 229 一致。待安装 exe 95551632 字节，SHA256 `349f674f5787d25f469200e9f5243eacd204d0114c3e2b3e1222abdb88ad49b7`；这是本地候选哈希，不是设备 exe 读回。
- 正常 CMake 源目录正确。四个关键 Core 对象与 CAPI 对象到各自静态库、App 合并库共 5 项逐字节核等；没有修改旧 cache home 或 SDK stamp。
- 同 bundle 正常更新 exit 0，57.941 秒。安装前独立 metadata 为 229，安装后独立 metadata 确认 3.0.1(230) 和新 bundle URL。
- 没有卸载、擦除、恢复数据库或读取用户会话。data container 路径发生 relocation，未比较数据内容字节，不据此宣称数据字节完全不变。

## 尚待解锁的启动验证

安装后第一次普通无参数启动 exit 1，10.627 秒。系统返回 CoreDeviceError 10002、FBSOpenApplicationError 7，明确原因是 `Locked`。随后独立查询进程成功，但没有匹配新 bundle 的 PID。

因此当前只确认 **230 已安装**，不宣称普通启动或私人会话功能实机通过。解锁请求已由本端提出；用户回复后只继续普通启动和独立进程回读，无需重装、重新配对或重复供应商调用。

## 功能验证来源

最终正常模拟器 App 回归 3/0，192.414 秒，覆盖五档真实本地 Responses 请求、跨型号归一、保存/重启、远端 ACK 及旧页/跨 owner 默认值守卫。普通 Apple C ABI 107 检查/0 失败/0 供应商 HTTP。

真实 GLM 普通搜索与 Reader 共 2 operations、2 attempts 成功，使用固定公开输入；仅记录形状、数量及非空布尔。成功 HTTP 原始数值未由普通接口暴露，未伪造 200，也未保存密钥或响应正文。402 分类的共享假 HTTP 对照沿用原验证；没有通过重复付费请求复现用户当时的预算状态。

Mac 最终 v3 普通消费者编译/链接完成，未运行、签装或替换现装。公开 release/tag 和其他平台现装未变。
