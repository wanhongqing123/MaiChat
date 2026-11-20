# Windows desktop_computer 坐标、观察与 OCR 排查

## 用户现场与复现目标

2026-11-20，用户反馈钉钉文件操作 14 次点击中，UIA 精确定位 1 次成功，目测 13 次
发生 5 次错误。其一将显示器比例 `y_ratio=0.8712` 换算为约 940 像素，而目标窗口
纵向范围为 115–915，随后文件被粘到桌面。其他误点包含小按钮和相近语义按钮。

预期：坐标应绑定明确的参考空间和执行目标；单窗口任务的越界点应在输入前拒绝，
不能静默裁剪。OCR 应提供文字位置及原图尺寸，但不能把识别框或置信度宣传为必然正确。
初查时缺少事故 B 的实际调用参数；用户随后核对本地会话记录并补齐如下证据。
这不是本机重新操作钉钉的实机复现，不能把用户回执和本机 owned 测试混为同一次运行。

## 事故 B 的实际调用证据补齐

- observe：`target=display`、`display_id=65537`，bounds 为 `(0,0,1920,1080)`，
  返回图像同为 `1920×1080`。
- click：同一显示器和 observe snapshot，`x_ratio=0.568`、`y_ratio=0.8712`。
  原生公式得到 `(1089.992,940.0248)`，四舍五入为 `(1090,940)`。
- 后续 `Ctrl+V` 也声明 `target=display`、同一 display_id，使用另一枚 snapshot。
  新 snapshot 不会自动增加窗口意图或证明输入焦点属于钉钉。
- 意图窗口 `window_id=263310`，bounds 为 `(320,115,1454,800)`。
  点的 x 在窗口范围内，y 比下沿 915 多 25 像素，位于桌面区域，但仍在显示器内。

因此，本例已可确认所有调用只声明 display；没有丢失已提供的窗口约束，也不是
把 window 请求在底层错误地按 display 换算。缺口是无法同时声明截图坐标参考系和
单窗口执行约束。尚未在旧现装或当前候选中注入输入重演，不声称已修复或实机验收。

用户另明确此次观察图像与显示器 bounds 等大，按钮约 118 像素偏移是未测量的目测
估值。该组记录不支持初查时提出的截图缩放嫌疑；不能再把缩放作为该偏移的既定原因。

## 修改前的源码证据

源码提交 `12bad690dddec511b00308b046c7ef69d92214fe`，工作区仅保留原 Qt vendor 改动。

- `DesktopComputerTools.cpp` 已绑定 Session、snapshot、target 与 window/display ID，
  拒绝错误作用域和过期/变更证据。窗口输入的点相对窗口完整外框，显示器输入相对显示器。
- 显示器高 1080 时，`.8712 * 1079 = 940.0248`；窗口 y=115、高 800 时，
  相同数字经窗口公式得到 `115 + .8712 * 799 = 811.0888`。反馈数值符合 display
  换算；补齐的实际调用已确认该次确实选择 display，不能归因为乘法或 DPI 算术错误。
- Windows 原生输入已有屏幕边界和窗口命中归属检查。display 模式按现有合同明确允许
  跨窗口输入；屏内、窗口外的点仍合法。若请求只声明 display，没有窗口意图约束，
  工具无法推断用户本意是钉钉，单加显示器范围比较不能阻止该事故。
- 工具层窗口 observe 显式 `includeImage=false`，已返回原生 bounds；has_image=false
  和 image_width/height=0 是未捕获图像的合同，不是截图失败后被悄悄接受。
  普通 Controller 的窗口捕获入口已存在，但当前模型工具未暴露此选项。
- 源码已有 minimize/restore 和 snapshot-bound element_id，相关提交为 `2826c0e2`。
  不能把最小化窗口的恢复能力等同于已验证第三方程序隐藏到托盘后的恢复。
- 另有独立静态一致性点：UIA 发布 center_ratio 目前除 width/height，执行乘 extent-1，
  复制比例时边缘可能偏约 1 像素；element_id 的直接解析不走该比例。该点未修改，
  也不能用来解释本次 25/118 像素偏移。

## 调查期运行产物及验证（不含本轮新增能力）

现装进程 PID 59396，路径 `D:\Program Files\MaiChat\maichat.exe`，167359488 字节，
SHA256 `2e79dbe81e9dbf7c700d5ed0c0609f5a0e35dd7f83c58c26ff6aa87770a2653c`。
它包含旧 cross-scope 转换描述，但没有当前窗口状态/element_id 契约字符串及动作枚举。
这证明与当前候选存在功能版本差异，不据此猜定其精确 Git 提交；未关闭或替换现装。

同源码正常 Windows maichat 构建和三产物/114 PE/Qt 插件门禁通过。纯 RecordingBackend
合同测试通过（1.39 秒）；owned native state-only 为 16 检查/0 失败（CTest 6.24 秒），
只使用测试自己的窗口做 UIA、minimize/restore、取消及失效身份检查，无键鼠注入、
无截图、无焦点请求，不操作钉钉、桌面文件或剪贴板。十套 Core 回归通过（50.88 秒）。

私有候选 `MaiChat-win64-v3.0.1-20261120-12bad690.zip` 为 242396807 字节，SHA256
`9b837855192fdcde6dba45cd621bacd46fb11a8092fdf6ee21c724af2e10a323`；exe SHA256
`82396a13afa5e7b4b07be7f624954cfcb65d446799f444e84e0d002d4504021d`。
staging 和真实 ZIP 提取目录分别以清除 Qt 环境、仅 Windows/System32 PATH 运行
`--smoke`，均 exit 0（2.004/4.021 秒）。候选含新 restore/element_id 契约，但没有
新增 OCR 或本次待设计的窗口约束，不能把更新候选称为已解决全部坐标事故。
运行进程的停止/替换确认已单独提出，未答复前保持现装。未上传 release/tag。

## 调查期方案与边界

优先明确单应用操作的执行窗口约束，同时保留有意的跨应用 display 操作。
坐标来源与执行目标应分别标注，先将来源点解析至原生空间，再检查执行目标边界，
越界返回原因、目标 bounds 和 requested point；不能依靠猜测语言意图或一律禁止桌面操作。
窗口观察应提供可选同源图像及明确的参考尺寸；未截图时不能伪造实际图像宽高。
事故 B 原调用现已补齐，具体新增输入合同仍待定，不在本轮调查中悄悄改变旧 display
跨窗语义。约束还必须覆盖键盘、文字和批量输入：单窗口操作在 `Ctrl+V` 派发前应
复核前台接收者属于限定窗口及允许的 owned popup；不应在仅有 display snapshot 时
声称窗口焦点已验证。鼠标范围拒绝不能代替后续键盘接收者检查。

OCR 候选为官方 PP-OCRv5 mobile 检测与识别 ONNX，模型卡均 Apache-2.0：
[检测](https://huggingface.co/PaddlePaddle/PP-OCRv5_mobile_det_onnx)、
[识别](https://huggingface.co/PaddlePaddle/PP-OCRv5_mobile_rec_onnx)。
复用已有 ORT 1.26.0；目前 RVM、LaMa、人像的私有会话均不能直接当通用 OCR 会话。
OpenCV 已有 DB/CTC/unclip 参考实现和 imgproc 透视几何能力，但输出布局、字典、
预处理及方向假设必须按所选模型验证；不顺带添加 pyclipper、Shapely 或第二套推理依赖。
识别配置含模型专属字符表，不能混用旧字典。

截至调查阶段尚未下载模型、修改推理或坐标生产代码、运行真实 OCR、读取私人截图或发起模型 API。
OCR 输出须明确原图尺寸、框所处空间、ROI/缩放/EXIF 变换以及置信度；同文多目标、
低置信度和相近文字不得自动被解释为唯一可点击控件。

## 用户批准后的通用实现

用户随后明确要求通用能力并授权继续实施。产品实现没有识别应用名称、按钮文案或
预设点击/粘贴业务顺序；事故 B 与相邻文字按钮只用作回归材料。

- display observe 新增可选 `execution_window_id`，将精确原生窗口/PID及边界绑定到
  Session 快照。坐标仍按原显示器计算，所有后续新输入继承接收窗口，动作不能覆盖。
  未设接收窗口的显式整屏操作保留原合同。
- 定位输入先检查主窗口矩形；原生层再复读窗口身份、几何、前台家族及实际命中，
  拒绝同 PID 独立窗口、外部遮挡和窗口外落点。键盘、文本、批量输入及持续报告均
  复验同一约束，不自动抢焦点或重放。主矩形外的 owned popup 须明确另选；键盘可
  接受完整、可证的 owner 链弹窗。受限相对运动因系统加速度无法预证落点而明确拒绝，
  可使用绝对运动。旧 window 模式及无约束 display 的行为保留。
- 拒绝返回 `point_outside_target`、接收窗口、原生 bounds、requested point 及单位；
  Windows 另含整数 `requested_px`。释放仍依据原 owner，失焦、取消和旧 token
  都不能阻断必要的 UP。部分输入已发送时保留事实，不把错误写成应用回滚成功。
- 自动续接在捕获前后比较原接收者身份，不能只凭被复用的窗口 ID 重新授权给另一
  进程；普通 `releaseInput()` 的撤销标记也持续参与捕获取消，防止无 hold 时旧任务
  自动恢复有效 token。失败时只撤销本次 successor、删除本次图像并清理原 owner。
- window observe 新增 `include_image=true`，复用已有原生 WindowById 截图及同源
  token；默认 false 保留元数据入口。返回实际 PNG 宽高、原生 bounds 和明确的图像/
  输入参考空间。不会以 bounds 数字伪造未捕获图像的尺寸。Windows 返回
  `image_coordinates_usable_for_input=true`；macOS 当前窗口 surface/content 原点映射
  未获证明，明确返回 false 并提示图片仅供读取，视觉定位使用 display 快照加 receiver
  或原生 AX 元素，不把图像尺寸相近宣传为已校准。没有猜测补偿阴影或改 SCK 算法。
- 新增普通 C API `MaiChat/Media/MaiOcr.h` 和本地 `ocr_image` 工具。检测、透视裁剪、
  CTC、字典、预算和取消只实现一次；薄适配层复用现有有界、EXIF 转正解码及路径授权。
  输出图像空间的文字、置信度、四边形、边界与原栅格尺寸；不生成 computer token，
  不把文字区域升级为可点击控件或唯一同名目标。

OCR 使用官方固定 revision 的 PP-OCRv5 mobile det/rec，原始 ONNX、配置、专用
字典、Apache-2.0 许可及逐文件 SHA 见 `MaiChat/Media/data/ocr/PROVENANCE.md`。
模型内置于桌面资源，复用 ORT 1.26.0 与现有 OpenCV core/imgproc，没有新增库、
Python 推理依赖、MCP、代理或真实供应商请求。Windows 依赖搜索仅允许运行时所在目录
和 System32；可取消互斥覆盖解码至推理，排队请求不会各持三份大图。

## Windows 同源验证

本轮源码基线为 `12bad690dddec511b00308b046c7ef69d92214fe`；产品变更由 Root
与独占 Mac 平台适配者串行合源。原 qtbase HEAD、完整工作区 diff 和空 index 均与
开工前记录核等，没有提交或覆盖他人 Qt 改动。

| 观测 | 修改前失败 | 当前结果 |
| --- | --- | --- |
| receiver/frame/held/工具入口合同 | 105 失败 | 完整合同通过 |
| 自动后继接收者被回收替换，实际 native spy | 32 失败 | 通过，拒绝新 token 并清理 hold |
| 无取消回调、无 hold 的普通撤销 | 3 失败 | 通过，捕获中撤销不复活快照 |
| 普通 OCR stub | 9 失败 | 真实模型通过 |
| OCR 工具缺失 | Qt 4 失败 | 最终 Qt 11/0，含透明图、EXIF、普通/工具一致 |
| OCR 排队先分配像素 | 定点 1 失败 | 移锁后定点及完整工具测试通过 |
| Pen / Touch 持续接收者与 survivor/换绑 | 最终窄例 14 / 18 失败 | 75/0 与 78/0 |

最终相关九套回归为 9/0、8.77 秒，含 OCR、完整 computer 合同、ownership/events/
session、Windows pen/touch/native session 录制回归及原有 vision 工具。最终 OCR 工具
普通 Qt 入口为 11/0。正常 Windows maichat 构建、windeployqt 与三产物/114 PE/插件门禁
通过。没有为了验收触发主模型、外部检索或其他付费工具。

真实 OCR 自有图覆盖空白、中英文、1.5 倍缩放、90 度旋转，以及 1920×1080 中相邻
两个 133×42 按钮的 18/20 physical-px 中文。两个不同标签各自完整识别，quad 覆盖
独立 GDI 墨迹真值；图已人工查看。无关英文标点/空格仍见识别误差，因此不承诺任意
图片或小字都完全正确。

真实 owned window-image 额外验证了当前 150% 缩放：DPI 144、DPR 1.5，logical
client 640×440、physical client 960×660，WGC PNG 964×707。Controller 返回的
DWM frame 同为 964×707，而 GetWindowRect 为 982×716；两个独立 ClientToScreen
标记经图像比例映射后的浮点物理坐标误差均为 0.0 像素（容差 2）。截图只包含自有
未激活 HWND，activation/input/display enumeration 均为零，PNG 已亲看。没有猜测或
修改现有截图裁剪算法，也没有把这一次结果泛化为所有 DPI/显示器均实测通过。

原生 state-only 为 16/0、6.18 秒，实际最小化/恢复及 UIA 仅操作自有窗口；前后台
原生 foreground ID 前后相同，无键鼠、无截图、无焦点请求。新真实接收者前台分支仍
因无法取得 owned foreground 返回 77，不能把它算作原生点击/输入正向通过；没有
操作钉钉或发送用户文件，事故 B 仍是用户回执加自有独立用例的机制验证。

## 打包与交付边界

首次 staging（增加 Mac 图像映射说明之前）的 Windows exe SHA256 为
`e73640e98bc6bd9345395294e7470ba0d53dc615107615fd70233979c5b1842f`。
staging 按正常脚本组装，OCR 模型来源/许可与 ORT notices 单独随包；实际 runtime 与
既有 source vendor 哈希核等。清除 Qt 环境、仅 Windows/System32 PATH 的 staging
`--smoke` exit 0（1.817 秒）；同样环境用包内 ORT 跑自有图片普通 OCR exit 0
（2.001 秒）。模型输入来自固定仓库资产，该探针不等同于 App 内工具 UI 的实测。

正在运行的 Windows PID 59396 尚未替换，现装仍是调查期旧产物。更新需要中断该
进程当前任务，先前确认仍待答复；本轮不卸载或清空数据、不修改 release/tag。
Mac 最终 MM 已精核一次合入，基础供件 post blob `91130a0e`；正常 flags/libs 的 7 个
相关 TU 编译及完整 App 手工链接通过，v2 增编 Tools 后链接 0（0.869 秒）。这是普通
完整 App link，并非 Ninja 整 target 重配或所有对象重编。原始 64/0 安全门禁为 18
共用 bounds/JSON 加 46 pre-cancel/空账本/释放检查；实际 receiver/holds/popup/foreign
AX hit 正向均未验，不能由 pure predicate 或权限状态替代。Mac 普通 OCR 的既有套件和
3 个 owned 中英文/旋转/1080p20px 场景实推通过；未运行 Mac 工具队列、GUI 或屏幕。
详细 artifact/object/archive 证据见同目录 Mac 专项记录。

最后的产品提交、最终 metadata 源产物及实际 ZIP 解压验证在交付回执补齐；本记录不预称
未完成的安装或前台输入验收通过。

## 最终产品提交与 ZIP 回执

产品提交：`940c459ce4139bc9bd84b95423d0f56197a06ae8`，共 44 件，包含普通 OCR、
固定模型、共享/Windows/Mac receiver、工具适配、相关测试与诊断。原 Qt gitlink 未入
提交，其 HEAD 和完整工作区补丁 SHA 均保留。最终 Mac MM 另收口局部 CF/CGEvent
异常 RAII，post blob `8813017973461a98872fab213d9407464f4e7466`；Root 独立窄审
确认无遗漏释放或 double free，正常 Mac 编译/链接及 64/0 用的也是该新对象。
资源异常路径仅静态审查，不宣传为已注入异常实测。

最终 Windows exe SHA256：
`0a6134691d0a2901bac392eccd0b8e40bd2a1ceda11595386c1980df5052c5d6`。
正常打包脚本生成 `MaiChat-win64-v3.0.1-20261120-940c459c.zip`，236446296 字节，
SHA256 `01fb0c96f678ef0077d2bc99efe21fd29c6363844adc6ca8bcf8a42e583a830b`。
该文件位于 `.cache/desktop-ocr-coordinates-20261120/windows-generic-ocr-receiver/`，
不是替换 public release/tag。

staging 和真实 ZIP 解压目录分别清除 Qt 环境、仅 Windows/System32 PATH 运行
`--smoke`，exit 均为 0（1.730/1.790 秒）。相同隔离环境、使用各目录自身 ORT DLL
的普通自造图 OCR 探针也均 exit 0（1.927/1.798 秒）；source/build/staging/解压后的
exe 与 runtime 哈希一致。普通探针模型来自固定仓库资产，不把它冒充 App 工具 UI
或全新系统安装验收。最终新增 metadata 合同和 owned image 映射在实际重链产物再验
通过，未用旧产物代替。

Mac 最终源的正常 flags/libs 下 Tools/MM fresh 编译为 0（5.310/3.633 秒），完整
App link 为 0（0.834 秒），exe 180275688 字节、SHA256
`40ff43ff3be901cb0a16f6d00465dd8011b7b340cbd613bd793cb1bb228d4758`。
这些为同源普通编链结果，不是新 App 安装/运行或 AX 输入正向验收；详见 Mac 专项
记录的源、对象、归档及安全用例边界。

真实供应商生成请求、外部工具执行、凭据/服务配置变更均为 0。Windows 现装进程
仍未停止或替换，安装和前台输入正向验收边界仍如上；后续按用户的运行中更新确认处理。
