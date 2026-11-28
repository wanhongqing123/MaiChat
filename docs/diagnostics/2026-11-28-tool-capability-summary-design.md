# 内置能力摘要、精简描述与按需帮助：源码核查

## 已确认需求与范围

用户要求先向模型提供内置延迟工具的简短能力目录，避免模型因为不知道存在某项能力而
没有主动 tool_search。目录保留真实名称、短用途及可达的发现/帮助入口；详细 schema 仍按需。
工具的 `description()` 参照实际 Codex 尽量简练，必要参数/权限/结果约束保留；
长例子与教程可由产品自带帮助或现有 Skill 读取机制承载，不建立第二执行核心。
这是 MaiChat 产品设计，不是安装或修改当前 Codex 的 openai-docs 技能。

本轮只读检查注入点并记录设计，未修改 Core、工具描述、执行权限、供应商请求或后台。
文件拖放功能继续独立收口，不因这条新方向放弃。

**后续实施更新：** 用户随后明确要求实施目录及描述精简，已沿下述入口完成 Core 实现与
独立审查；实际测试、发现的4k预算回归及其修复记录见
`2026-11-28-tool-capability-summary-core-validation.md`。
目录首次预算按实际窗口收敛，最多8192字节；未知窗口沿用该上限，完整请求预算仍为最终守卫。
旧v5声明不回写，17处描述已按合同精简，并非所有描述强制同长度。
FFmpeg原生-h帮助未进入工具返回值，未作为可用入口宣传；没有安装/修改当前Codex技能。

## 现有实现

- `MaiToolRegistry::metadata(model)`（`AgentTools/MaiTool.cpp`）已经只取公开且符合实际型号门禁的
  name/description，不构造 parametersSchema，不读取 Skill 正文。适合作为能力目录的普通入口。
- `MaiModelToolCatalog` 当前实现是 v5，按 session/model 固定声明并有界追加。
  bounded 初始只自举 exec_command、write_stdin、skill_read、tool_search、apply_patch、
  view_image、session_history、tool_output_read，且只加入实际注册并支持该模型的项。
- `MaiToolSearchTool` 已提供有界 metadata 搜索/分页和至多5个名称 select。
  select 只让下一次模型请求追加完整声明，不执行、不授予权限。
- `Runtime::dependencies` 把 exposedSpecs 接给 `MaiTurnRunner::Dependencies::prepareTools`；
  `buildRequest` 获取实际 request.tools。未声明名称的执行门禁独立存在，摘要不能放开它。
- `Runtime::prepareSkills` 和 `runUnchecked` 已有冻结目录→baseInstructions 的通路；
  `buildRequest` 将 baseInstructions 交给统一上下文准备、字节计量和请求适配。
- `MaiSkillManagerOptions.additionalRoots` 可挂宿主显式提供的技能目录；
  `.agents/skills` 已有工作区/用户发现和按需读正文规则。
  不能仅把教程放在仓库目录，就声称终端用户的任意工作区都能发现它。

## 推荐注入方案

复用 registry metadata，在目录核心增加普通的轻量摘要准备入口；由 Runtime 在轮次工作线程调用，
像现有 prepareSkills 一样把固定摘要并入请求的基础说明。不要从桌面 welcome 卡片抓取文字，
也不要在请求已计量之后临时追加。该入口与模型 tool_search 共享同一公开性/型号来源，
不引入执行、上传、网络或凭据读取。

摘要按 session/model/summary-version 固定，先保存再使用，重启重试保持相同字节与顺序。
沿用不可变会话上下文的 insert-if-absent、读取错误和失败不激活语义。
不把动态 loaded 状态、时间或每轮可变信息写入摘要；已加载的工具仍可作为可发现能力提及，
但不能一直错误地标成“尚未加载”。摘要明确要求先 tool_search/select、下一请求取得 schema 后再调用。
公开且支持型号不等于当前凭据已配置或权限已获准，不能宣传为保证可执行。

优先处理 bounded 发现模式；legacy 已有全量声明时不静默删去旧声明以腾出摘要预算。
既有 v5 schema/历史字节不在原位修改；新增摘要有独立快照身份与可追踪的配置边界。
若以后修改冻结 description 需要版本升级，保留旧快照并记录升级，不能偷偷改写缓存前缀。
当前头部注释仍提及较早版本，应随实际目录版本的实现变动同步更新。

渲染必须有总字节/token预算、单行短用途、UTF-8完整边界和确定排序。
预算不足先压缩描述，必要时保留真实名称与明确继续发现入口，不能截断 JSON/UTF-8或无提示地遗漏。
最终预算需以实际注册目录及小窗口模型验证，不在本轮凭空承诺固定开销。

不推荐把整份摘要塞进共享 tool_search 的动态 description：它缺会话上下文，
而已有 schema 被快照固定，容易把新摘要与旧字节混用。也不需要为了显示目录新建无意义的执行工具。

## description 与帮助分层

本机 Codex 基准 `6e09a1b58f759848942e3d5b7653ab714f045ada` 的
`core/src/tools/handlers/shell_spec.rs`：exec_command 的工具级描述简短，
参数说明和 Windows 必要规则仍保留；write_stdin 工具级描述也很短。
这支持逐项压缩冗长教程，不支持全局按字节或第一个句号机械截掉工具合同。

我方描述建议只保留动作、输入/输出形态及必要边界；参数范围继续由实际 schema/参数说明承载，
权限、覆写、付费/结果恢复等不能因为教程迁移而消失。需要按工具核查，避免保留一份过期的重复说明。

以 ffmpeg 为例，进程内执行、无 stdin、输出不得覆写、参数数组及诊断日志开关均是现有实际合同。
`ffmpeg -h` 的逐层帮助必须先用自有执行环境验证能返回可读帮助，再写入入口。
不能把该命令模式泛化到所有工具：例如受控 curl 参数集不等于完整 curl CLI，
未经实现/验证不能宣传任意 `--help` 都可用。

长教程若转到 Skill，应通过真实随包资源与宿主 additionalRoots 接线，
确认各平台打包、发现和 skill_read 可达后再在摘要里宣传。
Skill 只提供按需指导，仍调用原工具；它不能赋予权限或绕开模型/平台/费用/路径门禁。
这可与摘要/描述精简分阶段交付，但不能先删掉唯一可达说明再等待帮助资源上线。

## 后续验收要求

- 捕获首个模型请求：存在真实能力摘要，未展开延迟工具的全量 schemas，未读教程正文。
- tool_search 选中后只在下一请求追加该 schema；既有声明和摘要字节不变，未知名称仍被拒绝。
- hidden/unsupported/空目录、小窗口预算、UTF-8边界、重启、改模型、并发首次保存及读写故障。
- 普通 metadata/summary 调用和模型入口来自同一目录来源，不会执行工具、读取 Key 或发网络请求。
- 精简前后保留必要使用约束；仅当实际帮助/Skill 可达且内容一致才移动教程。
- ffmpeg 帮助的自有执行与结果验证；没有这项结果就不称完成。不能用模拟模型主动搜索证明所有真实模型都会搜索。

本轮未运行新测试或媒体帮助命令，以上为实现边界与验收计划，不是已上线能力。
