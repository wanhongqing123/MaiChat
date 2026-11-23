# Shell 与 Codex 的解释器、等待及输出契约对照

日期：2026-11-21。MaiChat 为 main `38d25b9d` 加既有第2至6批工作区修改。
参考固定为 `reference/codex` 的 `553c764e8b99fa519e9c6eb9b657875ef0f5e1c1`。
用户本轮明确扩大范围：默认解释器、显式shell和login/PATH也要对齐；PTY明确不做。

## 修改前复现及预期

1. 正式shell schema应接受shell/login。Windows省略shell、指定login:false执行自有
   PowerShell表达式，应由发现的pwsh/PowerShell执行；显式cmd维持cmd语法，不偷偷改写命令。
2. 纯普通API核对argv：POSIX login为-lc，否则-c；PowerShell非登录加-NoProfile；
   登录不加该项。账户shell以getpwuid_r查询，避免GUI环境SHELL缺失误判。
3. 输出包含头标记、大段中间文本、尾部错误标记，压低max_output_tokens。预期保留头尾、
   明确省略并守住字节预算，原始tool_output_read快照不被提示文字污染。
4. 已完成进程用shell_input空chars、yield_time_ms=300000可立即返回，不真的等5分钟。
   非空输入仍限30000，初次shell仍限30000。长轮询在取消时必须迅速响应。
5. PowerShell表达式括号、脚本块、大括号、@及CR不能被echo等旧只读命令前缀放行。
   只读程序及git子命令名单保持，只增加新语法的保守审批守卫。
6. 审批键包括完整命令、实际解释器、login和规范工作目录；换解释器或登录模式不能沿用
   另一种解释器的批准。返回元数据说明实际选中的解释器，不回显环境变量或profile内容。

## 证据及边界

旧适配器新增用例实际RED8项：无shell/login、只有头而无尾/省略、300000空轮询被拒、
PowerShell默认请求被拒；旧审批守卫另RED4项，不执行破坏性命令。
Codex来源为shell-command/src/shell_detect.rs、core/src/shell.rs、
core/src/tools/handlers/shell_spec.rs、core/src/unified_exec/output.rs及
utils/output-truncation/src/lib.rs。

解释器按Codex顺序发现；显式路径只选择受支持的类型，再发现该类型的解释器。
未知显式类型明确失败，避免静默换解释器。Cmd保留/d抑制AutoRun，这是已有安全策略差异。
工具名command、shell/shell_input、硬超时、12进程上限、48KiB预览、原始补读和审批策略
保留产品约定，不因对齐而删除。

POSIX的简单-lc不等于全部交互配置：Codex startup/snapshot还涉及.zshrc/.bashrc、
函数、别名、选项和环境策略。普通登录启动和当前环境继承不得冒称完整快照实现；
本轮不导出真实凭据或把整机环境写入源码、日志、测试产物、脚本文件。

本文件先于Root适配器修改；Windows实际验证与Mac/POSIX未验证边界将在收口补记。

## 最终实现与验证（2026-11-22）

- 解释器发现和login/shell参数已接通正式普通API与模型工具；保留command旧名。
  Windows优先pwsh/PowerShell，显式cmd有正确的/d/s/c整段命令引用；Mac/POSIX按账户类型
  和平台顺序发现。返回实际路径/类型/login，审批同时绑定这些上下文。
- 新进程直接加载所需启动配置：Bash/Zsh rc、Sh的非求值ENV路径及配置；随后恢复指定目录。
  PowerShell在profile之后用模块限定cmdlet恢复目录，失败时本地catch显式退出，即使profile
  设置trap continue也不能继续错误目录中的命令。login:false不额外加载这些登录配置。
- 输出预览保留头尾和省略标记。超过16MiB前缀捕获时，首个响应另从实时尾环展示真正结尾，
  原始补读仍是连续原始字节，展示尾环不推进其游标；丢弃、不可用中间区与预览省略分别标记。
- 空轮询支持300000ms，初次等待/写入/终止仍限30000ms；取消、硬超时、所属会话与进程树
  清理契约保留。PowerShell括号/脚本块等表达式不能从只读命令前缀绕过审批。
- 46个Shell测试及核心8套通过，无跳过。包括真实默认PowerShell交互、参数引用、cmd带空格
  路径与引号、受控Bash/Sh配置的别名/函数/导出/PATH、目录恢复、恶意ENV字面量不求值、
  大日志首个响应真实尾部、原始存储精等及1/2/3 token小预算的UTF-8边界。
  旧实现RED记录分别覆盖参数8、审批4、启动9、cmd2、旧PS恢复4、PS trap2、Sh ENV3、
  大捕获结尾1。回执为MaiAgent/.cache/build-file-batch2-20261121/
  codex-parity-validation-receipt.json，含源码与测试程序SHA。
- 最终正常Desktop App及两个相关测试目标构建退出0，部署和PE检查通过。
  正式Controller/Panel两轮命令/输入/终止授权用例最终3pass/0fail/0skip，使用自有模型和
  显式cmd素材；默认PowerShell的真实交互由Core用例独立验证，没有调用模型供应商。
- 完整staging与实际ZIP解压87文件字节精等；裸Windows PATH、新profile smoke各退出0，
  PID65232、69752，exe SHA256为
  e8a773ffac43d26966513b96ce223139e3cb5b08b6029a3c721a28e4ce87ffd0。
  开发ZIP261246272B，SHA256为
  3d2f1cc5ecd34080ba198fb3c8b4ba0c0f2ced3b4986c58d71f49d9fb4a0d59c。
  证据位于.cache/shell-codex-parity-20261122/integration-safe.json。
- 移植的ENV启动片段保留Codex Apache-2.0许可与相关OpenAI声明，源码在
  MaiAgent/third_party/codex-shell；Windows/Mac打包路径带入并核对两份许可文件。
  Windows实际包已核字节，Mac脚本仅语法检查，本轮没有Mac打包运行。

## 仍需明确的差异

这是直接、每次新进程加载的启动状态，不是Codex的缓存快照/凭据代理实现；不导出整机
环境或将凭据写入脚本。修改配置文件后重新加载的时机与快照缓存可能不同。
未知解释器明确拒绝；cmd保留AutoRun抑制与正确引用。工具名、审批策略、硬超时、进程及
输出上限继续使用MaiChat产品约定；小于平台Codex等待下限的显式请求仍为兼容保留。
PTY按用户决定不实现，没有引入MCP、代理或新运行库。

原生Mac/Linux/POSIX分支本轮未编译运行；Windows上的Git Bash/Sh受控配置通过不等于
原生POSIX进程组及账户查询验收。主机真实profile没有作为测试素材读取或复制。
一次早期自有临时目录清理失败后同API可删除，后续五次独立片段和最终全套均通过；
未凭该现象归因产品或用无条件重试掩盖失败。
改动仍在main工作区，未提交/发布/替换现装。新配置选择授权来自用户本轮明确扩范围。
