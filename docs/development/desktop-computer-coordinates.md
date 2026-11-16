# desktop_computer 坐标与截图使用说明

适用于模型调用桌面 `desktop_computer` 与独立 `screenshot` 工具。工具的模型描述和参数描述也包含这些规则；本文提供完整调用顺序与换算示例。

## 先选操作范围

| 需求 | 使用方式 | observe 返回什么 | 坐标基准 |
| --- | --- | --- | --- |
| 看屏幕找位置、跨应用操作 | `target=display`，推荐 | 指定显示器的整屏图片、原生 bounds、快照 | 该显示器整屏 |
| 指定某个应用、根据控件树操作或读取输入值 | `target=window` | 窗口 bounds、Accessibility/UIA 控件、快照；没有窗口图片 | 该窗口完整 bounds，包含窗口边框等区域 |

`target` 表示观察和输入的目标范围。窗口模式记录窗口身份并在输入时激活、核验窗口，不会固定窗口的位置。`observe` 和输入调用都应显式写出 `target`；当前省略时仍按 `window` 处理。

`list`、`list_displays`、`list_keys` 只传 `action`。

## 推荐顺序：整屏观察 → 输入 → 检查新图

依次调用 `desktop_computer`，下面的 ID 和快照为占位符，必须替换为真实返回值。

```json
{"action":"list_displays"}
```

从结果中选择目标显示器，然后观察它：

```json
{"action":"observe","target":"display","display_id":"<返回的显示器ID>"}
```

查看这次返回的图片，在这张图上选点，并使用同一次结果中的 `snapshot_id`。例如点击整屏中心：

```json
{"action":"click","target":"display","display_id":"<同一显示器ID>","snapshot_id":"<observe返回的快照>","x_ratio":0.5,"y_ratio":0.5}
```

输入完成后检查返回的新图片。下一次操作使用新结果 **`observation.snapshot_id`**，不能继续用点击之前的 token。确认目标输入框已获得焦点后，再用新 token 执行 `type`。

如果结果表明输入已经发送，但后续截图失败，重新 `observe` 确认现状，不要直接重复点击、拖动或输入。等待界面加载可使用 display `observe` 的 `wait_ms`（0～5000）；有新帧不等于应用已经处理完输入。

## 坐标统一用 0～1 比例

- 左上角 `(0,0)`，右下角 `(1,1)`，中心 `(0.5,0.5)`；x 向右增加，y 向下增加。
- `x_ratio/y_ratio` 是目标区域中的位置，不是像素、桌面绝对坐标或移动距离。
- 拖动终点 `to_x_ratio/to_y_ratio` 与起点使用同一个目标区域。
- 从整屏图片选点时，设实际查看的图片宽高为 `Iw/Ih`，图中像素位置为 `px/py`：

```text
x_ratio = px / (Iw - 1)
y_ratio = py / (Ih - 1)
```

例如实际查看的是 1280×800 的整屏缩略图，点在 `(640,400)`，应算 `640/1279` 和 `400/799`；即使原始截图元数据是 2560×1600，也不能拿缩略图的像素除以原图尺寸。不额外乘 Windows DPI、Retina 或浏览器缩放倍数。

完整图片等比缩放后比例可以直接使用。裁剪图片、浏览器 viewport 截图有各自的原点和范围，不能直接当整屏比例；如果无法确定它们在整屏中的区域，重新获取并查看绑定的 display 图片。

## 窗口操作与跨范围换算

窗口模式先 `list`，再观察一个确定窗口：

```json
{"action":"observe","target":"window","window_id":"<list返回的窗口ID>","include_elements":true}
```

窗口 `observe` 中控件的 `center_x_ratio/center_y_ratio` 已经相对于窗口归一化，可以直接传给同窗口的点击，不要再除一次窗口尺寸。窗口输入同时传本次返回的 `window_id` 和 `snapshot_id`。

整屏图中的比例不能原样用于窗口点击。例如整屏高 1600、窗口高 1528，即使窗口位于屏幕顶部，同一个 `y_ratio=0.9` 在两种范围中的落点也相差约 65 个原生坐标单位。

确需把整屏图片上的点用于窗口操作时，必须已知该图对应显示器和目标窗口的、仍有效的原生 bounds。设显示器为 `D`、窗口为 `W`：

```text
u = px / (Iw - 1)
v = py / (Ih - 1)
desktopX = D.x + u * (D.width - 1)
desktopY = D.y + v * (D.height - 1)
windowXRatio = (desktopX - W.x) / (W.width - 1)
windowYRatio = (desktopY - W.y) / (W.height - 1)
```

这里使用的是工具返回的原生 bounds；原生单位与图片像素可能不同，显示器原点也可能非零或为负数。窗口 bounds 不是网页内容 viewport。若尺寸不大于 1、窗口已移动/缩放、图与显示器无法对应，或结果超出 0～1，应重新观察，不猜测、不强行裁到边缘。一般视觉操作优先继续使用 display 模式。

## 独立 screenshot、快照与焦点

- `screenshot` 抓取鼠标所在显示器，鼠标跨屏后可能换屏；它不返回 computer 的 `display_id/snapshot_id`，也不延长或恢复已有 token。
- 看过独立 screenshot 后要进行视觉输入，应通过 `desktop_computer observe(target=display, display_id=...)` 获取绑定图片，并在这张新图上重新确认位置。
- 不把另一屏、另一时刻或另一裁剪区域的图片与旧 token 配对。`frame_sequence` 是图像帧信息，不是输入 token。
- 输入快照按单次使用处理，包括窗口 `focus`；窗口操作后重新 `observe`。收到失效、目标变化等错误，也重新观察。
- display `move` 只移动鼠标，不聚焦窗口；`key/type` 发往当前键盘焦点。点击或前台切换后应确认焦点，不能因截图中看到了某窗口就认定它已获得输入焦点。
- 操作系统拒绝激活窗口或权限不足时停止重试并说明原因。窗口 `verify` 可读取非密码输入控件并核对 `expected_text`，但 `verified=true` 只证明读回一致，不代表业务已保存或提交。

## 常见错误对照

| 错误方式 | 正确方式 |
| --- | --- |
| 看整屏图，却把同一比例传给 `target=window` | 使用绑定的 display 模式；或依据实际原生 bounds 换算 |
| 预览点坐标除以原图宽高，再乘 DPI | 使用实际查看图片的宽高计算比例 |
| click 后用同一个 snapshot 继续 type | display 使用返回的新 observation token；window 重新 observe |
| 重新 screenshot 后继续用已消耗 token | 获取新的 computer observe 并查看它的图片 |
| 输入成功但截图失败，就重发输入 | 只重新 observe，先确认实际结果 |

这些规则约束选点、目标身份和调用顺序；不会保证模型识别图片时必然选中正确控件。实际任务仍需逐步查看输入后的图片或控件读回。
