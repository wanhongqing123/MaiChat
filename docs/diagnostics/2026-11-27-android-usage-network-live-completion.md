# Android 用量显示、网络选路与真实业务补齐

## 范围与构建

用户要求补齐上轮未完成项并继续真机验收。主分支基准
`be9711aeac49e2be0c1c2f37069cc910ff90fc90`，保留既有工作区修改，无提交/推送/发布。
设备 PKD130，Android 15/API35，ARM64，正常包 com.kongshang.maichat，4.0.1(286)。
证据根 `.cache/android-completion-20261127/`，主执行者独占 ADB。

正常 Gradle/NDK 构建保持原 CMAKE_HOME、arm64 单 ABI、普通账号认证，未导入私有测试库或 ELF。
最终正常 APK 为 artifacts-04/app-debug.apk（artifacts-05/06 同字节），1,648,338,921 B，
SHA256 `62cb639f36963d6c3391afa3b94be247c5b1cbab81de31416dd7d528db561eb2`。
签名为原 Android Debug 证书，更新安装保留数据。01、03、04 是正常 App 构建；
02、05、06 只更新测试组件。04 相对 03 仅原生代码格式收口。

## 实现

- Core 实测 usage codec → MobileAgentAdapter 当前/远端会话快照 → 通用 JNI 字节传输 →
  Android State/presenter → 输入区上方用量条和可点击明细。当前占用与本轮/会话累计分开，
  显示输入、输出、缓存、思考及来源。未知窗口不显示假百分比；累计相加仍明确标“推算”。
- Android 网络观察器只观测系统已提供的网络，不绑定进程、不强开蜂窝、不关闭 Wi-Fi。
  无公网默认 Wi-Fi 下，仅前台、无 VPN/代理/省流量限制时允许选择已验证且 UID 可用的蜂窝。
  同一请求租约使用 NDK 选定网络 DNS 与 socket，IPv4/IPv6、局域网和代理保留原语义。
  新 socket 绑定前再次检查策略是否撤销；已连接流不因能力通知被主动杀掉。
- 共享 curl 路由接入 Responses、普通下载、云盘及 Android ASR WebSocket；
  连接池按网络代次隔离，网络变化后的失败模型请求不产生自动重投建议。
  ASR CONNECT_ONLY 握手独占句柄，保留新 socket 至录音结束，不套用关闭连接的 FORBID_REUSE。
- 修复云盘根目录的 JSON null 归一化，保持非根目录隔离及错误类型拒绝。

## 真实操作与结果

### 用量、Shell 与 Skill

正常已安装 App 的真实 Panel/Controller/JNI 使用自建 Responses 回环夹具，无付费模型请求。
输入框发送、真实“允许一次”、exec_command 文件操作、apply_patch、工作区 Skill 发现/正文读取、
两会话往返恢复及 JSONL provider usage 均实际执行。

`parity-final-01.log` 中已知窗口 UI 专项和 Skill/单请求用量两项通过；Shell/多次累计项首轮
仅测试误把累计“推算 532”当成无前缀的“532”。Core 与界面来源标记正确，修正测试后，
`usage-final-shell-02.log` 单独重跑通过（28.299s）。保留失败日志，未删除来源标记或放宽真实计数。

独立真实回环计数为 input111/output22/total133；test-model 不在官方端点/型号目录中，
因此端到端检查的是“133 used / 窗口未知”。另一个明示 synthetic UI 快照只验证进度条和文案，
复用已验证的 Core 样例 40000/100000→68% left；它不是任何供应商的真实容量或用量。
已亲看用量条、百分比样例、工具完成状态与云盘文件截图；未通过供应商付费请求验证容量。

### 网络

- `network-baseline-01.log`：正常网络下真实 HTTPS GET 通过，curlCode0，alternateUsed=false，
  generationChanged=false；此结果不冒充“无公网 Wi-Fi → 蜂窝”验收。
- 真机运行当前源码私有 ARM64 测试 ELF：MaiCurlNetworkRouteTests exit0；
  MaiNetworkRouteModelTests exit0（6 组服务端已接收 POST 后模拟网络代次变化，只有一次请求）；
  MaiMobileAgentTests --context-usage-only exit0。日志、源码与依赖 SHA 清单均保留。
  ELF 只在自有 /data/local/tmp 运行，未注入正常 App。
- Windows 独立路由契约也通过；Android 策略 JUnit 通过。

尚无可控 Wi-Fi AP 外网出口；已向用户询问能否将手机连到无公网测试 Wi-Fi，继续完成其他项。
当前不能声称完整断网 Wi-Fi、蜂窝策略、VPN、IPv6-only、后台/省流量组合均已实测。
不以关闭 Wi-Fi 或强制发布假蜂窝句柄冒充该场景。

### 真实 IM

正常 Android 账号向主执行者自己的 CLI 测试账号发送唯一 nonce；CLI 实际收到，随后回信。
只给本轮真机自己的 IM ID 增加 CLI 允许目的端，不联系其他人。
CLI 固定追加“此消息来自 imcli”尾注；首轮测试漏了这个协议事实而超时，修正为完整精确匹配后，
`im-live-02.log` 真机双向闭环通过（65.181s，含 OEM 打开测试组件确认等待）。
截图见正常消息列表的唯一测试 nonce。测试消息保留供核对，不删除既有会话。

### 真实云盘及本轮发现的产品 bug

首轮夹具在正常账号异步恢复前打开保存窗口，MainActivity 的 owner-change 保护关闭了窗口；
修正为等待恢复完成，不改生产账号保护。第二轮真实上传成功、quota 有值，但根目录列表为空。

决定性证据：服务协议以 `folder_id:null`/`parent_id:null` 表示根；Android 实际 JSONObject
把 optString 的 JSON null 转成字符串 "null"，与当前根 ID "" 比较失败。
普通 JVM org.json 行为不同，旧 JVM 测试掩盖该问题。用 Android35 原始 org.json 逻辑
重现 8 测 3 失败，nullableId 修复后 8 测通过；主执行者独立重跑 8 测亦通过。

`cloud-live-03.log`：正常保存确认 UI → 唯一自建 PNG → 实际云盘卡片 → 下载 SHA256 一致 →
仅删除新 media_id → PLAY_LINK404，通过52.292s。同时按本机已有 nonce receipt 精确处理前两轮
可能遗留的测试对象/上传记录，未删除其他资产。
最终格式收口构建 `cloud-final-04.log` 再次通过50.492s；已亲看实际上传卡片截图
`normal-live-671b4f71-b66a-4f30-b358-11caa40046ca-cloud-upload.png`。
这是 App 普通保存 UI 和共享核心的真实云端闭环，不包含系统相册/文档选择器授权验收。

### 语音

在自建空会话实际申请“仅本次使用时允许”麦克风，长按约3秒，浮层显示录音，拖向“编辑”后
松手，实际转写结果进入编辑框；未发送给模型，随后清除自建草稿并删除自建空会话。
现场没有标准朗读音源，因此只证明录音/转写/编辑链路，不能声称识别准确率通过。
原始截图在前一证据根 `.cache/android-4.0.1-functional-20261127/ui/live-voice-*.png`。

最终 62cb 构建再次实测长按约3秒→编辑目标松手；安全阶段日志显示 ticket-ready、asr-ready、
audio-sent/end-sent(samples49152)、server-final(chars0)。本轮无已知语音音源，正常提示“没有识别到文字”，
麦克风/凭据/WebSocket/服务终态均走通，不把静音处理算成准确转写通过。
最终正常 MainActivity 还实际点击用量条、查看明细弹窗并截图；旧会话计量失效时显示等待，
没有为展示效果发新模型请求。仪器截图 context-usage-details.png 采集早于弹窗实际绘制，
因此以正常 App 手工点击后的 normal-usage-dialog.png 作为真实弹窗像素证据。

## 数据与清理边界

没有登录/退出账号、清空数据、读取令牌或用户数据库、发付费模型请求、永久授予麦克风权限。
正常 MainActivity、设置、联系人、历史会话保留。归档的 JSONL、文件和会话只来自 UUID 自建测试根；
真实云盘只处理有本机测试 receipt 证明的本轮唯一名字和精确 ID。
最终清理和安装/运行核对收据由证据目录记录；不将目录 inode 不变解读为后台用户文件逐字节不变。

最终 `final-device-receipt.json`：APK SHA 如文首、PID20971，数据目录 inode/UID/mode 与测试前一致；
六个 UUID 自建根、独立 native 测试目录和 `.test` 组件均不存在。只保留本轮安全 receipt/截图和
唯一测试 IM 消息供核对。OEM 拒绝 ADB shell pm revoke；改从系统麦克风权限页恢复“每次使用时询问”，
最终 RECORD_AUDIO granted=false/ONE_TIME 与测试前状态一致。原有剪贴板等其他权限没有改动。
最终个人页已亲看既有账号、消息已连接及4.0.1(286)，App正常前台运行。
