# Windows 采集活跃期间桌面指针消失

## 用户复现与预期

2026-11-11 用户报告并依次确认：Windows 上工具启动屏幕采集后，真实指针在整个桌面、
任务栏和其他普通窗口均不可见；停止采集后恢复。预期为采集保持全会话共用常驻时，
用户仍可正常看见指针。尚未取得现场具体 capture target/工具参数，不能推断为指定窗口
或整屏请求。本轮不读取真实数据库、操作用户 App 或重放原任务。

排查基线 `7eb79ce80243cbe32ee6d2e56a4391fab66fdde3`；Windows11 build26200，
本机有 NVIDIA/Intel 与虚拟显示 adapter。现场硬件、运行包身份尚未独立核对。

## 源码链与独立观测

Windows Core 当前使用常驻 WGC worker/session；截图代码没有 ShowCursor、SetCursor、
SetSystemCursor、BlankCursor 或全局鼠标设置写入。DesktopScreenCaptureControl/Bar 仅
状态轮询、显示及异步停止。播放器独立全屏 HUD 会设置局部 BlankCursor，但采集链没有
调用该逻辑，且用户确认停止采集即可恢复，未据源码猜测把播放器定为根因。

普通自有320x220窗口 probe PID16692/exit0，采集和停止成功，GetCursorInfo 全程 flags1。
该 API 的可见位不能证明系统实际呈现了指针；这不构成现场问题未发生的证据。

进一步建立独立 DXGI metadata observer，不 Map、复制、保存桌面像素，不读取指针坐标，
WGC 仍只捕获自有320x220纯色 HWND。PID22964/exit0、7.293s，同一 monitor/output 的
有效 LastMouseUpdateTime 记录了独立指针 plane `Visible=true → false → true`，依次对应
采集前、WGC 活跃、停止后；五次 GetCursorInfo 仍全为 flags1。probe 完成后自己的
capture worker、duplication、窗口和进程均已回收，Core library 前后 SHA 未变。

这是独立指针 plane 转变的行为证据，不把它冒称物理消失的完整本机 RED。软件合成接替
硬件指针也会产生该转变；未通过屏幕实拍区分二者，observer 并存效应也未排除。
原 stdout、exe SHA、PID 和解析 proof 位于
`.cache/cursor-capture-20261111/dxgi-observer/observation-proof.json`。

## 外部主来源与修复方向

微软公开样例仓库 [issue128](https://github.com/microsoft/Windows.UI.Composition-Win32-Samples/issues/128)
记录了 WGC 活跃时真实系统指针不可见、停止恢复的相似情况，并讨论强制软件指针与全屏
合成的边界。该报告不代表现场根因已完全等同，也不证明某 Windows build 已有修复。
[IsCursorCaptureEnabled](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.iscursorcaptureenabled)
控制截图是否包括指针，不是恢复真实系统指针的开关。

候选将整屏 DisplayUnderPointer/DisplayById 改为原生 DXGI Desktop Duplication，保留
原常驻线程、取消、交互桌面/目标验证、真实帧时戳和 stop/join。指定窗口不能无声扩大
为整屏或可包含其他窗口的裁切；窗口路径和用户现场 target 仍需分列确认。
实施细节及门禁见[计划](../superpowers/plans/2026-11-11-windows-capture-cursor.md)。

当前尚未宣称修复、打包或现场验收；已有 v2.2.58 与 iOS build205 产物保持原样。

## 早期整屏候选与失败定位

保持基线不变，整屏候选改为独立 DXGI Desktop Duplication；指定 HWND 仍为 WGC，
不能把整屏结果用于声明窗口路径已修复。`IsCursorCaptureEnabled(false)` 的私有单变量
对照仍为 plane true→false→true，未作为修复合入。

首个 DXGI 候选在本机真实整屏输入上明确失败：输出为 R16G16B16A16_FLOAT / HDR，
8bit-only reader 返回 NotSupported。保留原失败；未用单元测试代替整屏运行。
第二候选支持 FP16 scRGB，使用同 monitor 的实际 SDR white 280 nits 转为 sRGB，
具有纯转换、stride、旋转、source QPC、资源清理与设备创建计数断言。该 cohort 冻结
Core library SHA256 为 `a32dd7cbd5a99fd4747ec95e5b4647689c069fc7c161af1a9575afbdd6592303`。

- 第二候选 normal Core gate PID16796 实际 exit0，13/13 PASS、105.70s；同源 normal
  Desktop App PID47728 build/deploy/PE dependency gate exit0、22.181s，尚未打包或启动。
- 最初 owned-window SessionTests 有一次 90s deadline 失败；随后私有行级观测及正常
  定点运行通过，原因未证明，原失败单列保留，不声明已修复该超时。
- Cross-process observer 与 production DisplayById child 分开，PNG 只留 RAM；仅在
  WindowFromPoint 中心/四角、自有 PID、窗口 bounds 和前后 geometry 全部满足时读取
  自有 3x3 色块，不导出、保存、查看真实桌面图片。
- Protocol2 以 PNG capture-completed 至 stop-request 的至少 1s 作为 steady 区间，
  要求起点指针 plane 已可见且之后无有效 hidden 更新。旧库 parent46236/child47580
  实际 RED，steady1094.109ms 内 hidden；第二候选 parent45112/child39084 steady
  1138.2633ms 全可见。候选启动有 0.2053ms hidden→visible，恢复早于 PNG 返回；
  原“任意 hidden 即失败”的观察仍为 FAIL，未覆盖或改写。
- **图像内容仍阻断：** 同一个自有色块的 GDI reference 与旧库 RAM PNG 都为
  RGB `[220,30,40]`，第二候选 PNG 为 `[0,0,0]`，超过预设最大误差8。两轮 ownership
  guard 均全真；未放宽断言，不能把指针 steady 通过写成整体成功。

完整原日志、对象/库/子进程身份与独立计算位于
`.cache/cursor-capture-20261111/dxgi-observer/display-protocol2-owned-colour-proof.json`。
该阻断候选未交付。上述 plane 结果不等于用户物理屏幕的人眼验收；现场 target 与指定
窗口路径边界保持待确认。

## 最终整屏修正及正常构建

相同私有单变量用例证明两个独立原因，未修改 SDR-white 公式或放宽颜色阈值：

1. 首次 poll 为 LastPresentTime=0 / AccumulatedFrames=0，worker 已把该 pointer-only
   texture 编为黑 PNG；等 sourceQpc>0 的实际桌面 present 后黑图消失。
2. 此时 legacy DuplicateOutput 在 cross-process 双 duplication 场景实际返回 BGRA87，
   PNG 为255/60/77。不能反向恢复已裁损的红通道。改为 Output5.DuplicateOutput1 显式
   支持 FP16/BGRA，实际得到 Format10、white280；自有 raw FP16 half bits 为
   16642/10704/11456，保持原白电平转换后9像素全为220/30/40，误差0。

正式修正仅要求所有初始化/更新均有正且前进的 sourceQpc，并协商原生 FP16/BGRA；失败
明确返回，不回退已知裁损的 legacy API。source0 原断言实际 RED 后同例 GREEN。
最终六源、普通构建及严格 native 对照记录在
`.cache/cursor-capture-20261111/implementation/implementation-present-format-receipt.json`。
最终正常 Core library SHA256：
`680649e56c897dc6a9dac5463923b17612fa77607bfb60572649422f2f6ed44b`。

- 原 baseline 库名字虽含 `cursor-disabled`，SHA a9d64... 实为未改的正常7eb库；关闭
  cursor 的私有实验是外部 TU/object 覆盖。最终 Protocol2 baseline link 未带该 object，
  Root 独立核 preparation/两份 link 命令，未因文件名误写为不同生产源。
- 相同 Protocol2 child.obj、最终正常库，无 trace/私有 object override：parent46196 /
  child45196 exit0，PNG2560×1600，前后自有 guard 全真、RGB误差0；采集仍活跃时
  steady1117.2361ms 全可见，stop 后 inactive。启动 plane hidden→visible0.5215ms 在
  PNG 返回前已恢复；原 any-hidden 观察仍 FAIL，未冒称物理可见性验收。
- normal 定点3/3 PASS2.79s；Root 最终 Core13 PID2300 exit0、13/13 PASS77.72s。
- normal Desktop App PID43408 build/deploy/PE exit0、21.572s，实际编译 helper object、
  link 正常 maiagent archive 和 maichat.exe。独立最终六源 review 未见具体阻断。
- normal SessionTests `--display-metadata-only` PID20980 exit0、0.520s，连续两张
 2560×1600，复用同 worker/device/session 和同源帧；停止后 inactive。PNG 留 RAM，
  未写出、查看或发送桌面图片。
- 完整私有 candidate staging 与 ZIP78文件逐 SHA 相同，两处裸 PATH `--smoke` 分别
  PID44904 / PID37500 exit0，实际 exe SHA 均为
  `5fe20c2b0075ad1fc68b4fd52210bcf70065c10ecd311ef3ddb8b54b43920efd`。
  `.cache/cursor-capture-20261111/final-candidate-verification.json` 绑定12项 Cursor /
  Desktop免打扰源码、Core13、App、PNG metadata、包和两次运行身份。最初脚本路径用
  forward slash 导致 cmd 在构建前拒绝的两次 driver failure 另存，不当代码 RED。

该候选没有安装、替换现装或改公开 v2.2.58 资产。实机人眼指针可见性尚待现场；
WindowById/WindowByTitle 保持指定窗口 WGC，仍不属于此次整屏修复的已验范围。
