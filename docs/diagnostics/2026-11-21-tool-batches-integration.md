# 工具改进第2至5批：第6批之前的 Windows 集成验收

日期：2026-11-21。分支 main，HEAD `38d25b9d7a41ad29128e95289f44937c676ca452`，
叠加未提交第2至5批源码。本记录不表示这些改动已经提交、推送或进入公开 Release。
第1批提示词因用户要求先解释有意保留的历史、再确定删减方案，尚未改生产文字。
第6批后续 shell 会话收敛另立验收，不混入下列旧产物结果。

## 复现与期望

分项步骤先写于 file-tool-capability-plan、desktop-browser-occlusion、
curl-cli-capabilities、tool-group-categories 四份同日诊断。使用自有临时文件、
loopback HTTP、临时浏览器profile和本地合成会话；不读私人对话、不取Key、不调用模型。

## 已验证

- 文件能力：核心8套及搜索、文本读取、工具schema专项退出0。聚合总数仅在完整范围内给
  exact，否则null+下界；新增move/copy/delete-directory/stat。跨卷move明确拒绝且保源，
  copy字节核等。41978B自有文件limit1实际读4096B，limit500读41978B，total_bytes精等。
- Curl：4套真实Windows普通库测试通过，含本地重定向、body文件、multipart、cookie、
  跨origin去凭据头、HEAD敏感头不混进body、303/307方法与body行为。无外部HTTP。
- Browser：原可见性门禁真实RED；正式普通API/工具实际运行完整headless/open/observe/
  PNG/type/click，加载中截图、等待、取消与旧快照/密码拒绝通过。真实Windows窗口覆盖时
  截出网页而非遮挡物，图像已查看；此时visibility=visible。确定性hidden fixture另过。
  Windows最小化截图CDP超时的额外失败保留，不把该状态计为通过。
- 分类：Windows真实QPA远程24/24、本地重点4/4，纯内置、shell、混合失败、ssh仅内置
  截图均查看；保留状态优先级和折叠行为。移动端界面未改。

## 正常App及包

正常缓存：`MaiChat/desktop/.cache/build-qt6-ui-20261109`，实际源码home未变；
`cmake --build ... --target maichat --parallel 2`退出0，部署后检查通过：
3 binaries / 114 PE imports / Qt plugins。

随后用原package-windows脚本生成仅供验收的开发ZIP，没有改版号或发布：

- 开发版本：3.1.0（沿本地main元数据，不冒称公开3.1.1包含本批）。
- exe SHA256：`e2ae001d2ebcd6ea3ec05b18c24b86768988e513522adf77684c5a77d08dc776`。
- ZIP：`MaiChat-win64-v3.1.0-20261121-38d25b9d.zip`，261219772B，SHA256
  `85aa2690873b6ab79b3b86c2c5274e089aded7b82bcd1ea735bfa401aa6a1392`。
- staging及实际ZIP解压85文件逐字节一致；两者分别裸Windows PATH、全新自有profile
  `--smoke`退出0，PID46556、59088，日志版本、exe路径、PID及5个实际Qt模块路径核对。
- QtCore已另以相同DLL实际验证known-folder在自有USERPROFILE内；smoke跳过登录/账号恢复。
- 完整源文件hash、产物、运行进程回执位于 `.cache/tool-batches-20261121/integration-safe.json`，
  同目录有smoke-stage.json、smoke-zip.json、Qt路径证明及打包日志。

## 边界

没有安装、替换或停止现装程序，没有修改公开Release/服务/手机/配对/TCC。
Mac原生遮挡、POSIX原生运行和iOS/Android消费者未在本轮运行；普通工具完整流程不等于
已登录正常App里通过真实模型完成的聊天轮次。App正常构建和启动、同源正式工具流程、
实际Qt界面合成历史这三种观测分别列明，不能互相替代。
未预留新的浏览器activate/PTY/PowerShell能力；这些不在第2至5批交付结论内。
本地既有三提交、Qt working/index及三份未跟踪Codex对照文档hash保持原样。

## 第6批之后的最终集成

新Core先通过原生Shell33项及核心8套，再移除旧DesktopProcess实现、注册、构建目标和
独立测试。`shell_input`普通轮次续读/写入/终止只绑定所属Agent会话；真实删除会话和
Agent析构回收进程树。C++生命周期钩子新增后，正常缓存重新构建同ABI消费者；
Controller的Ninja依赖记录明确含MaiTool.h。活动工具与欢迎目录只公开shell/shell_input，
旧四名仅用于历史卡片分类；新maichat_agent.lib中没有旧DesktopProcessTools成员。

最终源码验证：核心8套、Shell33项、目录129checks、远程界面26项、本地重点7项。
在最终ShellTool.cpp适配代码上另跑实际Controller/Panel两轮授权用例，3pass/0fail/0skip：
三次实际授权前后核对原生PID存活、stdin标记、回显和终止，未执行外部模型请求。
截图见shell-session-desktop-20261121目录，纯内置/混合/混合失败及两轮流程图片已查看。

正常App最终构建、部署与PE检查退出0；当前验收包（仍未发布开发版3.1.0）：

- exe SHA256：`8401c9d690a631158daa25e3fb1154207cde037596c557c95438be71984f12f9`。
- ZIP 261223437B，SHA256
  `e0cb738a6aa06e8c8ee8e4d6d3eb34af45a8203d54781b26b42a9209724fd78f`。
- staging与实际解压85文件精等，裸PATH/隔离profile smoke分别PID69824、9748，退出0。
- 回执：`.cache/tool-batches-20261121-final/integration-safe.json`，含全部当前修改与新增
  Core源码SHA、三件删除文件清单、两份smoke进程身份和实际Qt加载证据。
- 最终真实Windows浏览器完全遮挡时visibility=hidden，观察、输入及PNG成功；像素已查看。
  Windows最小化仍有CDP截图超时记录，Mac/POSIX/移动端本轮未运行边界保留。
- 从最终正常App的同一maiagent.lib另链接普通阻塞进程探针：正常完成、非零退出、
  输出截断、预先取消四项通过，确认新增shell会话没有替代或破坏maiRunProcess入口。
  证据为最终验收目录的blocking-process-result.log。

源码仍在main工作区未提交；未切换版本、上传Release、安装或替换现装。第1批仍等待用户
在收到A/B历史原因后选择公告方案；当前媒体提示词没有删除，字符节省尚未产生。
