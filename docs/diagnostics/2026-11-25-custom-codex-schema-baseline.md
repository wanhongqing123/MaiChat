# 本机定制 Codex 数据库结构基线

## 来源与读取范围

2026-11-25，当前进程 `CODEX_HOME` 为
`C:\Users\18034\multi-ai-code\.codex`。这是本次结构对齐的实际数据库基线，
不以另一个 Codex home 或上游 Git 提交的迁移文件直接代替。

使用 Python 标准库 SQLite，通过 `mode=ro`、`PRAGMA query_only=ON` 和只读事务读取：
`sqlite_master`、`table_xinfo`、`index_list`、`foreign_key_list` 及 `_sqlx_migrations`
中的版本/描述/成功标记。没有查询会话、消息、日志或记忆正文，没有修改数据库。
源数据仍在原位置；完整的纯结构快照保存在本地
`.cache/codex-schema-20261125/custom-codex-schema.json`。
文件实际字节 SHA256：`cf1112822dcced2f5e31b421a0603396200d946a6a1c2a9cd3893b9a924bd980`。
最初记录的 `e26abf...` 是写盘前 LF 序列化文本的摘要，Windows 写盘换行后并非文件摘要；
以本项实际字节校验值为准。

## 按用途划分数据库，而非按会话拆库

| 文件 | 主要职责 | 表/视图数（含迁移表） | 列总数 | 已记录迁移数 |
| --- | --- | ---: | ---: | ---: |
| state_5.sqlite | 会话、分组、项目、附件、动态工具和父子关系元数据 | 16 | 115 | 62 |
| thread_history_1.sqlite | 历史 item、轮次及投影游标 | 5 | 39 | 8 |
| logs_2.sqlite | 运行日志 | 2 | 18 | 2 |
| goals_1.sqlite | 会话目标及继续执行状态 | 3 | 16 | 2 |
| memories_1.sqlite | 记忆抽取、整合及相关任务 | 4 | 31 | 2 |
| queue_1.sqlite | 待处理输入及队列版本 | 3 | 14 | 2 |

一个文件内仍能保存多个会话的数据；不是一个会话分配一套文件。
这些是 SQLite 的用途分区，与 JSONL 原文目录是不同层。

## 核心表实测字段

`threads`：43 列。

```text
id, rollout_path, created_at, updated_at, source, model_provider, cwd, title,
sandbox_policy, approval_mode, tokens_used, has_user_event, archived, archived_at,
git_sha, git_branch, git_origin_url, cli_version, first_user_message,
agent_nickname, agent_role, memory_mode, model, reasoning_effort, agent_path,
created_at_ms, updated_at_ms, thread_source, preview, recency_at, recency_at_ms,
history_mode, name, is_pinned, thread_section_id, section_position,
section_entered_at_ms, project_id, originator, daybreak_enabled,
creator_user_id, creator_account_id, attached_thread_parent_id
```

`thread_items`：10 列。

```text
thread_id, turn_id, item_id, rollout_ordinal, created_at_ms, item_json,
item_type, updated_at_ordinal, started_at_ms, completed_at_ms
```

`thread_turns`：14 列。

```text
thread_id, turn_id, rollout_ordinal, status, error_json, started_at,
completed_at, duration_ms, first_user_item_id, final_agent_item_id,
rollout_byte_offset, rollout_end_ordinal, rollout_end_byte_offset, root_turn_id
```

`thread_history_projection_state`：3 列。

```text
thread_id, next_rollout_byte_offset, next_rollout_ordinal
```

## 与 MaiChat 迁移要求的关系

- 对照必须包含列类型、空值、默认值、主键、索引及语义，不能只比较字段数量。
- MaiChat 专有设备、IM 绑定、媒体和执行事实可用扩展列/关联表保存。
- 当前要求继续每账号一个 `agent.db`；对齐逻辑表结构并不要求照搬六个物理库。
- 用户已同意业务数据同库、JSONL 原文、运行日志独立的方案。`item_json` 采用可读的
  TEXT VIRTUAL NOT NULL 生成列，保留字段并通过原文偏移回读，不以空列凑数或物理复制全文。
  普通外部 SQLite 客户端仍可查询表结构与元数据；读取该正文列须注册 MaiChat 原文读取函数。
- 实测定制库的迁移版本与上游固定提交不完全相同。升级基线必须另做差异记录，不能覆盖本快照
  后声称仍是同一版本。数据来源未知时保留未知，不编造历史创建者、原始配置或 usage。

本次仅取得和记录结构，没有实施 MaiChat 存储迁移，也没有复制任何 Codex 会话正文。
