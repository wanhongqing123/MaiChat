# 桌面输入参数与状态规则收敛

日期：2026-11-17。收敛实现提交 `efd27a7e`，基线 `4044f058`，原生输入原始实现 `94f785d7`。范围为 Windows 底层输入原语；25 个模型 action 继续作为独立提效接口。虚拟设备、手柄、其他端和目录规整均不在本次实现范围。没有新增第三方库、Git 依赖、驱动或服务，也没有替换现装 App。

## 本次固定的五类规则

### 1. 原始键身份先规范化，再重映射

- 旧 `0x8000` 标记的键码先取低 8 位。
- 不带旧标记、且 flags bit 0 置位的编辑键 `0x7a..0x7d`，先转换到 `0x100..0x103`。
- canonical source code 与 flags 组成逻辑身份；modifiers、extended、release、repeat 不改变这个身份。
- 用户键码映射不改调用者的逻辑身份。不同源键映射到同一原生键时，仍是共享一个原生绑定的不同租约。
- 首次请求的原始表示仍可显示在状态中，但同一键的 prefix/plain 编码不会再建立两个持有身份。有效 native 请求保留既有 legacy marker 和解析规则。

### 2. 修饰键按来源语义选择，按实际原生效果校验

来源 Ctrl/Alt/Shift 是否已持有，从现有唯一 alias ledger 派生，不另加状态表。例如来源 Ctrl 映射为 F7 后，再发送“带 Ctrl 的 F8”，不会凭空补出一个真正的 Ctrl。

缺失的临时修饰键也经过同一重映射与原生解析器。现有 Meta 自动补齐作为 MaiChat 扩展保留。已持有的命名键、raw 键和物理用户键可被借用，但本次调用不抬起它们。

完整发射计划先预检；之后每次真正 down 前还会重新检查当前安全焦点和实际修饰键。这样即使临时映射成 Tab 并切入密码框，也会拦截后续字符，并清理已经按下的临时键。映射形成 Ctrl+Alt+Delete 时，在首个事件前拒绝。

### 3. 首次 DOWN 冻结目标与临时绑定

首次 DOWN 固定实际原生键及当时缺失的 synthetic modifier 集合/绑定。重复和 UP 复用这些绑定，后续配置或布局变化不重译已经持有的键。

如果来源 Alt/Ctrl 在首次 DOWN 时已经持有，当时无需补齐；后来释放来源修饰键，repeat 不会把它重新补出来。仍可复用当前已按住的相同原生/物理绑定，且只释放本调用新增的租约。

这项是明确选择的安全合同，不照搬固定 Sunshine 每次发射重新查询映射的行为。

### 4. 方向和接触范围分别定义

定位仍使用既有原生边界与 `llround(origin + ratio*(size-1))`；本次没有修改坐标端点或负半整数取整方式。

屏幕平面方向以 +Y 为起点，朝 -X 增加，与笔的方位角一致：

- 0°：向下，Windows orientation 90。
- 90°：向左，Windows orientation 180。
- 180°：向上，Windows orientation 270。
- 270°：向右，Windows orientation 0。
- 360° 与 0° 相同；未知值不冒充已知零方向。

修正了 Touch 原先 `+270` 的反向换算，统一为 `(rotation+90)%360`。方向是独立元数据，不因压力、面积、裁剪或目标宽高比改变而重推主轴。

接触两轴为 NDC 椭圆直径，保留正确的独立投影：

```text
bboxWidth  = hypot(major*W*sin(angle), minor*W*cos(angle))
bboxHeight = hypot(major*H*cos(angle), minor*H*sin(angle))
```

缺一轴用另一已知轴，两轴皆零表示未知；角度未知时仅以 45°估计范围，方向 mask 仍为未知。不引入 covariance/eigen-axis 新算法，也不复制上游矩形近似来制造逐字段相同。

### 5. 笔 UP 与 LEAVE 分开

- UP：结束接触，保留已接受的悬停位置和 in-range 状态。
- HOVER_LEAVE：离开感应范围。
- 接触直接转 hover：显式结束接触。
- BUTTON_ONLY：保持位置、接触和姿态，只处理按键/工具变化。
- 三逻辑侧键仍 OR 到 Windows 唯一 barrel 位；橡皮切换使用已验证的合法序列。

这些既有正确规则以新增精确合同锁定，没有为了模仿固定上游后端的副作用而重写。

## 失败用例和修复证据

证据根：`.cache/desktop-input-semantics-20261117/`。

- canonical identity：正常构建 RED 679 项中 17 失败，修复后 679/0。
- 来源修饰键/冻结计划：正常构建 RED 399 项中 38 失败；收敛后通过。
- 独立复核发现映射 Tab 后安全焦点变化的泄漏：实际 recording RED 409 项中 9 失败；三处逐 down 重查后 409/0。覆盖首次输入、repeat、多前缀、配对清理及物理用户键保留。
- Touch 初轮 84 失败中包含新测试夹具对负半整数取整的错误期望。保留该日志，先按项目既有绝对 llround 规则修正夹具，再取得仅方向的干净 RED 472/16；最窄方向修正后 472/0。
- Pen 新规则合同直接 GREEN，完整 300/0；未假称这些既有行为需要产品修复。
- Common 新增 canonical alias 重复/释放和首次绑定验证，完整 251/0；无需修改调度核心产品代码。

## 最终 Windows 验证

- 普通构建通过；9 组本地合同/Agent 回归全部通过。
- 实际键鼠接收：87 项、0 failure、0 skip，新增 prefix/plain 单一持有、冻结映射及 extended、alias UP、来源修饰键释放后新 repeat 不复活 Ctrl 等断言全部通过。
- 实际笔接收：54 项、0 failure、0 skip，136 条记录；native hold 1566 ms。新增 UP 保 hover、原生 in-range 无接触、LEAVE 原生退出及状态均通过。
- 实际触摸接收：62 项、0 failure、0 skip，212 条记录。源方向 90°实际收到 orientation 180；双点持续、十点、满槽有序复用及清理仍通过。
- 跨进程编辑器原生回归通过（13.07 s）；完整 ScriptModel Agent/工具/原生编辑器 3/0、无 skip（2.243 s）；整屏输入/捕获/读回通过；窗口发现矩阵 3002/0、无 skip。
- 已亲看最终 Agent 对话和整屏输入截图。以上使用受控自有窗口，不等于每个第三方应用或供应商模型视觉推理已实测。

`loaded-runtime.json` 记录三个新测试进程及实际加载的 Qt 模块路径/哈希，三者完整、无查询错误；实际 qwindows 仍为 `47bcb160c01ffe4a171c6147b5b8b07063088c9db26756b757d2581f99ebc549`。最终源码、二进制和运行证据由 `source-artifact-manifest.json` 记录。

## 边界

本次将五类差异收敛成一套明确、共享、可验证的合同；不会据此重新宣称固定 Sunshine 所有后端逐事件严格相同。原始命名键与 raw 键仍是不同调用身份，Meta 合成保留扩展，绑定冻结及笔状态保留安全规则。共同 Windows 缺少的距离/笔椭圆/笔杆自转没有被虚构为新增能力。

参考：固定 [Moonlight 参数语义](https://github.com/moonlight-stream/moonlight-common-c/blob/94c65fd364d17b223f00a8763c9a5d94a66cb8dd/src/Limelight.h)、[Sunshine 输入处理](https://github.com/SunshineStream/Sunshine/blob/bb6930bd782ed3edd2eea9a71d46b42183be9358/src/input.cpp)及 [Windows Touch orientation 定义](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-pointer_touch_info)。Moonlight 文档只写“从 Y 方向起算”，本项目正向选择明确依据现有笔方位角，不把该正向选择伪称为原文逐字规定。
