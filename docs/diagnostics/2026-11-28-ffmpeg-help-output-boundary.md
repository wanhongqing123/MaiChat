# FFmpeg 帮助没有进入工具返回值：Mac owned 探针

用户认可“先简要介绍能力，再通过帮助逐层查参数”的模式。本轮不能在能力目录中
直接宣传 ffmpeg -h 已能给模型返回帮助，需先验证现有嵌入链路。

Mac 使用产品23288正常 App 的实际 Core/libmaifftools 链接输入，在独立 owned TU 中调用：
普通 `maiRunFfmpegSerialized` 执行 ffmpeg/-hide_banner/-h，
以及真实 `makeMaiFfmpegTool(...).execute`，参数为 arguments=[-h]、capture_log=true。
只查帮助，不处理媒体、不请求供应商或模型，不使用用户文件或现装 App。

供件5152字节，SHA256
`dab2228673e10efc39088d000d45308d7d2a3c3d16fb84085275dc15497c037b`。
Root 核全部6项manifest，TU实际构造嵌入引擎，未替换成 fake engine。
供件中 FFmpeg工具cpp/header 的大小及SHA与23288 Git blob精等；
实际库和新exe只有Mac绑定信息，没有提供binary供Root独立重哈希。

结果分列：

- 普通执行exit0，direct diagnostic为0字节。
- 工具执行成功，但返回值仅48字节：status0、空log_tail、log_truncated=false；帮助0字节。
- 父进程捕获stdout/stderr共6868字节，包含两次真实帮助块和完成标记。
  Root从原输出核了两个“Universal media converter”及单个完成标记。

现有capture_log接av_log，而该帮助走printf/stdout，是源码与实际输出相符的边界。
这是Mac同库owned验证，不冒称Windows执行结果；也不能拿exit0等同模型已拿到帮助。

本轮能力摘要/描述精简不得引用未闭合的-h工具输出入口。
可先保留足够的真实schema/必要使用合同，再在实际可达的产品帮助/Skill就绪后移入教程。
后续如实现嵌入命令输出闭包，应复用现有执行核心与宿主输出接口，
不能以进程全局stdout dup2重定向影响其他线程/用户App。
本记录不包含该输出闭包修复，也未安装或修改当前Codex技能。

原始证据在 `.cache/tool-capabilities-20261128/ffmpeg-help-probe/`。
