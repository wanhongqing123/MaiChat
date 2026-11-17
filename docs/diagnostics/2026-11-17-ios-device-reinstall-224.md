# iOS 真机保数据重装224

2026-11-17 用户明确要求“iOS安装到真机”。目标：保留现有数据，安装当前最新已验证的iOS构建，并正常无参数启动；没有新增产品代码或擅自将Desktop3.0.1版本写到iOS。

物理设备为已配对、可用的iPhone14 Pro Max。操作前查询现装2.2.4(224)。核对本地222→223→224合成清单中的15个iOS源文件SHA仍相同，复用已验证Release-iphoneos App：2.2.4/224，bundle com.kongshang.maichat，exe SHA256 4b79a5c43ade9002d2b1d1bf6df3ea00ec822c6fabd925721403a106d18e1bcf；deep/strict签名校验0。本地源码HEAD仍c3ea0bbbbb63f09b67329f57b31fd5e6b92c554c及既有WIP，未覆盖工作区或混入Desktop发布改动。

使用devicectl install app进行标准保数据覆盖安装，未执行uninstall。安装0，设备应用元数据复查0=2.2.4(224)，无参数普通launch0，新PID71215；独立process查询0，实际进程路径属于本次新安装bundle。未改配对、TCC或系统服务，没有查看用户数据库或实际消息。

此轮验证安装/版本/正常运行进程；没有独立读取设备exe SHA或重新执行真机触摸功能验收，既有224 UIKit/横屏/取消误报证据沿用原诊断，不冒称本次重新全测。

私有回执：/private/tmp/maichat-ios-device-install-20261117，包含artifact-checked.json、install.json、installed-app.json、launch.json、process.json、completed.json。

同时用户询问云盘“仅自己可见 · 视频2GB/图片20MB”含义。只读确认MaiCloudDriveView.swift说明文字、Model.swift按fileSize/max_size_bytes校验及后端按owner_user_id限制查询；已解释为账号自己的云盘可见性、单个视频/图片上传上限，未更改产品文案或限制。
