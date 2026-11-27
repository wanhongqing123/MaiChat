# PC Shell / Skills / token / 压缩集成验收

用户要求以 Codex 基础机制对齐 PC 常用能力，再叠加工具优先、每次约 20%、交流正文保护。
参考固定 Codex rust-v0.172.0，commit `3d86e43b075adeba2193ec5c22ac92f55afd82e6`。
main 基线 `be9711aeac49e2be0c1c2f37069cc910ff90fc90`，既有未提交工作保留。
Windows x64 / MSVC 14.44 / Qt 6.12，正常源与构建为 `.cache/r311/src` / `build-msvc`。

## 操作和结果

新会话运行 Shell/PTY、输入/取消与原文补读，显式使用技能并按需发现额外工具；查看最近
主请求用量、缓存与累计区别；继续同轮、新轮次和重启验证前缀。全部使用自有数据和受控
回环模型，没有操作真实用户会话库、停止/替换现装、真实 IM/供应商请求或发布资产。

- 实际占用取主采样 total，缓存仍占窗口，累计及摘要不作当前占用；缺失和字段推导有来源。
  已查看实际 1100x600 组件图：40,000/100,000 配合 12,000 基线显示 68% left，布局不重叠。
- 旧误差倍率放大整份后续输入的问题先红后绿。现以实测输入前缀加新增尾部估计，最多
  2 KiB 指纹随 usage 保存；跨新 Runner/SQLite 冷重开、失效来源等边界已验证。
- 签名、ID、JSON 包装不再当正文粗估；保留原始加密载体和必要思考估计。详情见同日
  `pc-context-codex-baseline-audit` 与 `pc-actual-context-usage` 诊断。
- Skills 初始目录预算、2048 技能、Unicode、元数据分页、显式策略及原有快照字节稳定通过。
- 工具 Schema 从选择后的下一请求追加，内部/未声明/同响应抢先调用拒绝。旧历史前缀保留。
  核心配置实测 42 工具/25221 字段字节到 5/6003；不把这当作费用或所有 token 节省比例。
- 摘要 413 仅在原请求仍适配时延后；超硬限明确失败，不推进窗口。旧 RequestBodyRecovery
  用例期望错误的超限 defer 行为，现改为软/硬配对与原史指纹检查，修正后通过。

## 审查和验证

独立审查的两个 P2 已修复：派生 total 加法溢出不再饱和为已知最大值，真实 SSE RED2→GREEN0；
缓存源读取失败作废旧占用，自有 JSONL 故障的实际 Qt RED1→GREEN0。未知窗口来源格式器
也经 RED1→GREEN0。远端按现有命令回执传递最大16KiB codec，旧服务历史不覆盖较新回执，
登录/账号/型号隔离及150工具重复快照合并验证通过，未改服务部署或 presence 大小。

- 同一正常构建：34 套 Core、控制台 E2E 全部通过。
- Qt Controller76/0、Panel138/0、Remote35/0、Host99/0及展示格式器通过；Host独立临时图片
  TLS用例按原门禁跳过1项，不将其说成已运行。
- windeployqt / 3 binaries / 114 PE imports 验证通过；staging、实际ZIP解压目录及交付目录
  均以隔离profile/裸PATH `--smoke` exit0。
- 150 源输入 main/构建副本/清单 SHA 全等；凭据排除检查通过。独立失败、重跑收据保留。
- 混用ZIP生产者追加说明导致中央目录/本地头分隔符不一致；失败包保留缓存，从同一已验
  staging统一生成POSIX member名称，testzip/解压/smoke通过，EXE SHA不变。

## 本地包及边界

- `C:/Users/18034/Downloads/MaiChat-local-20261126-pc-aligned/maichat.exe`
- `C:/Users/18034/Downloads/MaiChat-Windows-local-20261126-pc-aligned.zip`
- ZIP264449586 bytes，SHA256 `0ffc3e7f153f214234308dd98d42011cec0e984a0527e6f773ce0cf36e8afaeb`。
- EXE SHA256 `ad107d150204cd17e017f8414176ba04ecda9fc0285bce1cfe4908bfcd81abc9`。
- 收据 `.cache/pc-context-20261126/local-handoff.json`。

HTTP仍发完整有效上下文和所选声明，不承诺仅传新增字节或供应商缓存必命中。路径图片
采用保守估计回退。仅验收Windows受控数据，其他平台和真实双机未实测。OpenAI后台Key
部署仍待入口，未宣称上线。本轮未提交、推送、改公开版本或覆盖先前体验包。
