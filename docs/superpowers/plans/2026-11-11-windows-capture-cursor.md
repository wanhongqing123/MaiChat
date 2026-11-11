# Windows 整屏采集指针恢复实施计划

**目标：** 修复用户报告的 Windows 常驻屏幕采集期间整个桌面指针消失、停止采集即恢复；
保持此前全会话共用的常驻采集，以及截图的取消、来源与新帧校验。

**基线：** `7eb79ce80243cbe32ee6d2e56a4391fab66fdde3`。本轮不改已发布 v2.2.58/tag/资产，
不替换或停止用户现装进程。候选完成后再按实际包与用户现场验证交付。

## 已有证据与范围

- 用户依次确认：Windows 真实桌面指针消失；停止采集即恢复；任务栏和其他窗口也不可见。
- 当前 Windows 截图 worker 为持久 WGC 会话；采集代码没有 ShowCursor、SetCursor、
  BlankCursor 或系统设置写入。UI capture bar/control 也没有隐藏指针逻辑。
- 微软公开样例仓库 issue128 记录 WGC 改用软件指针后出现真实指针不可见的兼容边界。
  它与现场相似，但不能用公开 issue 代替本机复现或认定所有硬件都相同。
- 本机自有320x220普通窗口 probe PID16692，采集/停止成功；GetCursorInfo 全程 flags1。
  该接口不能看见实际指针合成输出，不能据此排除现场问题。原证据保存在
  `.cache/cursor-capture-20261111/`，没有采集用户窗口或写出桌面图片。

## 方案与边界

1. Windows 的 DisplayUnderPointer/DisplayById 使用原生 DXGI Desktop Duplication，避免
   保持 WGC 整屏会话。WindowByTitle/WindowById 保持指定窗口捕获，不扩大到整屏。
2. 继续复用原工作线程、队列、stop/join、交互桌面守卫、取消和 WIC PNG 编码；普通调用
   与模型 screenshot 仍走同一核心实现。无每次截图开关会话，无新的业务 fallback 链。
3. DXGI 根据 HMONITOR 找到实际 adapter/output，使用匹配 adapter 创建设备，处理旋转、
   stride、首帧、静态帧、ACCESS_LOST/设备丢失、显示器失效与有限等待。不得盲用 adapter0。
4. 帧源使用 DXGI LastPresentTime 原始 QPC，保守换算后走原新帧屏障。不得把 Acquire/
   编码时刻当源时间，pointer-only 更新也不得伪造新的桌面内容帧。维持真实计数与缓存语义。
5. 保留截图中的正常指针信息时仅在副本合成，不能移动系统鼠标、改显示计数或写全局设置。
   参考现移植 OBS duplicator 与上游 win-capture，避免引入无关 Graphics 运行时依赖。

## 实施与验证顺序

- [x] 先建立 DXGI metadata 观察：独立 duplication 只读帧信息，WGC 仅采集自有小窗口。
  基线无法观察独立指针 plane 时标明未复现，不制造物理可见性断言。
  PID22964/exit0 实测 pointer plane true→false→true；GetCursorInfo 始终flags1。
  只证明 plane 转变，不冒称完整物理消失 RED，原 proof/日志已保全。
- [x] 根据独立预期先写必要转换/旋转/帧时间与资源生命周期失败用例，再写最小实现。
  真机指针现象无法自动复现时保持用户报告/本机观测边界，不把编译失败称为行为 RED。
- [x] 生产改动限 Windows 截图实现、必要独立普通 helper/头、对应 CMake/注释与定点测试。
  helper 若独立，公开头只有声明，平台资源由同线程 opaque 实现持有。
- [x] 正常 Core 定点构建与相同用例回归：自有窗口/指定屏的像素或尺寸、静态复用、
  新帧屏障、目标切换、取消、stop/restart、失效目标及多 GPU/旋转可用边界。
- [x] 独立 code review 后完成仓库必要 Core 至少八组门禁；最终13/13 PASS77.72s。
- [x] 正常 Desktop build/deploy/PE、完整 staging/ZIP 两次裸 PATH smoke、正常显示尺寸
  /复用/stop 与严格自有色块和 steady pointer metadata 均过，源/产物/PID已绑定。
- [ ] 用户现场验证采集活跃时真实指针可见、停止后状态正常；不以 API flags 或独立
  pointer plane 代替人眼。指定窗口 WGC 路径保持分列，尚未宣称修复。
- [x] 在 `docs/diagnostics/` 记录旧/新源码、产物、原日志、用户现场与未验边界；
  修正已提交为 `9f9e7a49c45904403d7b8df83cea4ab87215fa11`，未替换现装。

## 不采用的处理

不通过反复 ShowCursor 或写鼠标设置补偿，不把 IsCursorCaptureEnabled 当作系统指针
开关，不用轮流 stop/restart 掩盖问题，也不把指定窗口静默替换为整屏裁切。
