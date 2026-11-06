# 应用文字与 Markdown 呈现契约

`typography.json` 是 iOS、Android 和 Qt 的共同定义。修改后运行：

```sh
python3 MaiChat/scripts/generate-typography.py
python3 MaiChat/scripts/generate-typography.py --check
```

生成的 Swift、Java 和 C++ 文件只保存数据，不要分别修改。正文标准值为 16，
大为 18，特大为 20；说明、时间、标题和等宽代码通过角色比例派生。普通段落、
列表、引用和表格使用正文角色，Markdown 保留标题层级、强调及等宽语义。

平台适配只负责字体 API、环境更新和取整：iOS 使用 `AppTypography`，Android 使用
`MaiChatTypography`，Qt 使用 `UiZoom::TextRole`。页面选择角色，避免自行定义文字
大小。系统字体沿用平台的中英文回退；图形符号的固定尺寸、媒体画布和导出文档不
属于正文设置。字体在呈现时缩放一次，不能在富文本解析缓存内保存已缩放字体。

`font_families` 同时定义字体选择。默认保留系统字体；普通界面和 Markdown 正文可
选择 Noto Sans SC 黑体或 Noto Serif SC 宋体。字体、许可和固定上游提交的 SHA-256
清单只保存在 `fonts/`，三个宿主引用同一目录，不在运行时联网下载。Windows 还会
列出系统确实安装的微软雅黑、宋体；没有安装的字体不展示，也不复制 Windows 字体。
Noto 字体遵循随资源分发的 [SIL OFL 1.1](https://github.com/notofonts/noto-cjk/blob/main/Serif/LICENSE)。

iOS 原生列表文字通过应用的语义字体渲染，导航栏和自定义标题共用 `AppPageTitle`，
不依赖系统默认标题字体。Android 原生控件和 Markwon 共用所选 Typeface；Qt 应用
字体与 Markdown 主题显式传递字体家族，排版缓存键包含家族，切换后不得复用旧字形。

手机端可选择跟随系统；其倍率以系统标准字号为 1。固定档位不叠加系统缩放。
桌面保留原来的整体界面缩放，文字倍率再与其组合。Qt 样式使用 `font-size: @body`
等语义令牌，并经 `UiZoom::setStyle` 保存原始样式，修改偏好后重新解析现有控件。

颜色同时定义浅色与深色前景/背景，iOS 动态颜色和 Android 当前主题均读取同一份
令牌。切换时应同时检查文字、面板、表格和代码，不能只替换底色。Qt 当前的标准
浅色主题共用 Markdown 令牌；本次不新增桌面主题选择入口。

验收采用相同的中英混合样本，检查三档字号、输入框、表格、代码和长标题的操作
区域；用实际页面截图或录屏确认，测试数据通过隔离的本地测试账号/内存样本提供。
