#include "MaiCodexStoreSchema.h"

// DDL source: custom Codex schema-only, read-only metadata export, 2026-11-25.
// Snapshot: .cache/codex-schema-20261125/custom-codex-schema.json
// SHA256 of original file bytes (no JSON normalization):
// cf1112822dcced2f5e31b421a0603396200d946a6a1c2a9cd3893b9a924bd980
// Upstream reference: https://github.com/openai/codex (Apache-2.0).
// The custom snapshot, rather than generic upstream migrations, fixes the contract here.
// All native column, default, key and index definitions are retained; the two item_json
// fields are virtual projections of committed rollout bytes to avoid a second body copy.
// Migration metadata is deliberately excluded: each original DB has an independent
// _sqlx_migrations version space; the joint store owns its migration history.

static const char businessSchema[] = R"SCHEMA(
CREATE TABLE backfill_state (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    status TEXT NOT NULL,
    last_watermark TEXT,
    last_success_at INTEGER,
    updated_at INTEGER NOT NULL
);

CREATE TABLE external_agent_config_imports (
    import_id TEXT PRIMARY KEY,
    completed_at_ms INTEGER NOT NULL,
    successes TEXT NOT NULL,
    failures TEXT NOT NULL
, provider_id TEXT);

CREATE TABLE guardian_review_feedback (
    id TEXT PRIMARY KEY NOT NULL,
    thread_id TEXT NOT NULL REFERENCES threads(id) ON DELETE CASCADE,
    record BLOB NOT NULL
);

CREATE TABLE project_idempotency_keys (
    key TEXT PRIMARY KEY,
    project_id TEXT NOT NULL,
    created_at_ms INTEGER NOT NULL
);

CREATE TABLE project_roots (
    project_id TEXT NOT NULL REFERENCES projects(id) ON DELETE CASCADE,
    position INTEGER NOT NULL,
    path TEXT NOT NULL,
    PRIMARY KEY (project_id, position)
);

CREATE TABLE projects (
    id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    metadata TEXT NOT NULL DEFAULT '{}',
    position INTEGER NOT NULL,
    created_at_ms INTEGER NOT NULL,
    updated_at_ms INTEGER NOT NULL
);

CREATE TABLE remote_control_enrollments (
    websocket_url TEXT NOT NULL,
    account_id TEXT NOT NULL,
    app_server_client_name TEXT NOT NULL,
    server_id TEXT NOT NULL,
    environment_id TEXT NOT NULL,
    server_name TEXT NOT NULL,
    updated_at INTEGER NOT NULL, remote_control_enabled INTEGER,
    PRIMARY KEY (websocket_url, account_id, app_server_client_name)
);

CREATE TABLE rollout_migration_skipped_rollouts (
    migration_id TEXT NOT NULL,
    rollout_path TEXT NOT NULL,
    rollout_size_bytes INTEGER NOT NULL,
    rollout_modified_at_ns INTEGER NOT NULL,
    skip_reason TEXT NOT NULL,
    skipped_at INTEGER NOT NULL,
    PRIMARY KEY (migration_id, rollout_path)
);

CREATE TABLE rollout_migration_state (
    migration_id TEXT PRIMARY KEY,
    last_checked_thread_created_at INTEGER,
    last_checked_thread_id TEXT,
    updated_at INTEGER NOT NULL
);

CREATE TABLE "thread_attachments" (
    id TEXT PRIMARY KEY,
    thread_id TEXT NOT NULL REFERENCES threads(id) ON DELETE CASCADE,
    attachment_type TEXT NOT NULL,
    identity_key TEXT NOT NULL,
    payload TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    UNIQUE (thread_id, attachment_type, identity_key)
);

CREATE TABLE thread_dynamic_tools (
    thread_id TEXT NOT NULL,
    position INTEGER NOT NULL,
    name TEXT NOT NULL,
    description TEXT NOT NULL,
    input_schema TEXT NOT NULL, defer_loading INTEGER NOT NULL DEFAULT 0, namespace TEXT,
    PRIMARY KEY(thread_id, position),
    FOREIGN KEY(thread_id) REFERENCES threads(id) ON DELETE CASCADE
);

CREATE TABLE thread_read_receipts (
    thread_id TEXT PRIMARY KEY NOT NULL,
    first_unread_turn TEXT,
    revision TEXT NOT NULL,
    last_published_turn TEXT
);

CREATE TABLE thread_sections (
    id TEXT PRIMARY KEY,
    name TEXT NOT NULL
, appearance TEXT);

CREATE TABLE thread_spawn_edges (
    parent_thread_id TEXT NOT NULL,
    child_thread_id TEXT NOT NULL PRIMARY KEY,
    status TEXT NOT NULL
);

CREATE TABLE threads (
    id TEXT PRIMARY KEY,
    rollout_path TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    source TEXT NOT NULL,
    model_provider TEXT NOT NULL,
    cwd TEXT NOT NULL,
    title TEXT NOT NULL,
    sandbox_policy TEXT NOT NULL,
    approval_mode TEXT NOT NULL,
    tokens_used INTEGER NOT NULL DEFAULT 0,
    has_user_event INTEGER NOT NULL DEFAULT 0,
    archived INTEGER NOT NULL DEFAULT 0,
    archived_at INTEGER,
    git_sha TEXT,
    git_branch TEXT,
    git_origin_url TEXT
, cli_version TEXT NOT NULL DEFAULT '', first_user_message TEXT NOT NULL DEFAULT '', agent_nickname TEXT, agent_role TEXT, memory_mode TEXT NOT NULL DEFAULT 'enabled', model TEXT, reasoning_effort TEXT, agent_path TEXT, created_at_ms INTEGER, updated_at_ms INTEGER, thread_source TEXT, preview TEXT NOT NULL DEFAULT '', recency_at INTEGER NOT NULL DEFAULT 0, recency_at_ms INTEGER NOT NULL DEFAULT 0, history_mode TEXT NOT NULL DEFAULT 'legacy', name TEXT, is_pinned INTEGER NOT NULL DEFAULT 0, thread_section_id TEXT
    REFERENCES thread_sections(id) ON DELETE SET NULL, section_position INTEGER, section_entered_at_ms INTEGER, project_id TEXT
    REFERENCES projects(id) ON DELETE SET NULL, originator TEXT, daybreak_enabled BOOLEAN, creator_user_id TEXT, creator_account_id TEXT, attached_thread_parent_id TEXT);

CREATE INDEX idx_projects_position
    ON projects(position ASC, id ASC);

CREATE INDEX idx_thread_attachments_identity_thread
    ON thread_attachments(attachment_type, identity_key, thread_id);

CREATE INDEX idx_thread_attachments_thread_created_id
    ON thread_attachments(thread_id, created_at, id);

CREATE INDEX idx_thread_dynamic_tools_thread ON thread_dynamic_tools(thread_id);

CREATE INDEX idx_thread_spawn_edges_parent_status
    ON thread_spawn_edges(parent_thread_id, status);

CREATE INDEX idx_threads_archive_created_at_ms
    ON threads(archived, created_at_ms DESC, id DESC)
    WHERE archived = 1;

CREATE INDEX idx_threads_archive_recency_at_ms
    ON threads(archived, recency_at_ms DESC, id DESC)
    WHERE archived = 1;

CREATE INDEX idx_threads_archive_updated_at_ms
    ON threads(archived, updated_at_ms DESC, id DESC)
    WHERE archived = 1;

CREATE INDEX idx_threads_archived ON threads(archived);

CREATE INDEX idx_threads_archived_cwd_created_at_ms ON threads(archived, cwd, created_at_ms DESC, id DESC);

CREATE INDEX idx_threads_archived_cwd_recency_at_ms
    ON threads(archived, cwd, recency_at_ms DESC, id DESC);

CREATE INDEX idx_threads_archived_cwd_updated_at_ms ON threads(archived, cwd, updated_at_ms DESC, id DESC);

CREATE INDEX idx_threads_attached_parent ON threads(attached_thread_parent_id)
    WHERE attached_thread_parent_id IS NOT NULL;

CREATE INDEX idx_threads_created_at ON threads(created_at DESC, id DESC);

CREATE INDEX idx_threads_created_at_ms ON threads(created_at_ms DESC, id DESC);

CREATE INDEX idx_threads_pinned_recency_at_ms
    ON threads(archived, recency_at_ms DESC, id DESC)
    WHERE is_pinned = 1 AND preview <> '';

CREATE INDEX idx_threads_project_id
    ON threads(project_id, archived, created_at_ms DESC, id DESC)
    WHERE project_id IS NOT NULL;

CREATE INDEX idx_threads_project_recency
    ON threads(project_id, recency_at_ms DESC)
    WHERE archived = 0 AND project_id IS NOT NULL;

CREATE INDEX idx_threads_provider ON threads(model_provider);

CREATE INDEX idx_threads_recency_at_ms
    ON threads(recency_at_ms DESC, id DESC);

CREATE INDEX idx_threads_section_position
    ON threads(archived, thread_section_id, section_position ASC, id ASC)
    WHERE thread_section_id IS NOT NULL;

CREATE INDEX idx_threads_section_recency_at_ms
    ON threads(archived, thread_section_id, recency_at_ms DESC, id DESC)
    WHERE thread_section_id IS NOT NULL;

CREATE INDEX idx_threads_source ON threads(source);

CREATE INDEX idx_threads_updated_at ON threads(updated_at DESC, id DESC);

CREATE INDEX idx_threads_updated_at_ms ON threads(updated_at_ms DESC, id DESC);

CREATE INDEX idx_threads_visible_created_at_ms
    ON threads(archived, created_at_ms DESC)
    WHERE preview <> '';

CREATE INDEX idx_threads_visible_recency_at_ms
    ON threads(archived, recency_at_ms DESC, id DESC)
    WHERE preview <> '';

CREATE INDEX idx_threads_visible_updated_at_ms
    ON threads(archived, updated_at_ms DESC)
    WHERE preview <> '';

CREATE TABLE thread_history_projection_state (
    thread_id TEXT PRIMARY KEY,
    next_rollout_byte_offset INTEGER NOT NULL,
    next_rollout_ordinal INTEGER NOT NULL
);

CREATE TABLE thread_items (
    thread_id TEXT NOT NULL,
    turn_id TEXT NOT NULL,
    item_id TEXT NOT NULL,
    rollout_ordinal INTEGER NOT NULL,
    created_at_ms INTEGER NOT NULL,
    item_json TEXT GENERATED ALWAYS AS
        (mai_rollout_item_json(mai_rollout_path, byte_offset, byte_len)) VIRTUAL NOT NULL, item_type TEXT NOT NULL DEFAULT '', updated_at_ordinal INTEGER NOT NULL DEFAULT 0, started_at_ms INTEGER, completed_at_ms INTEGER,
    mai_rollout_path TEXT NOT NULL,
    byte_offset INTEGER NOT NULL,
    byte_len INTEGER NOT NULL,
    PRIMARY KEY (thread_id, turn_id, item_id)
);

CREATE TABLE thread_realtime_items (
    thread_id TEXT NOT NULL,
    item_id TEXT NOT NULL,
    rollout_ordinal INTEGER NOT NULL,
    created_at_ms INTEGER NOT NULL,
    item_type TEXT NOT NULL,
    item_json TEXT GENERATED ALWAYS AS
        (mai_rollout_item_json(mai_rollout_path, byte_offset, byte_len)) VIRTUAL NOT NULL,
    mai_rollout_path TEXT NOT NULL,
    byte_offset INTEGER NOT NULL,
    byte_len INTEGER NOT NULL,
    PRIMARY KEY (thread_id, item_id)
);

CREATE TABLE thread_turns (
    thread_id TEXT NOT NULL,
    turn_id TEXT NOT NULL,
    rollout_ordinal INTEGER NOT NULL,
    status TEXT NOT NULL,
    error_json TEXT,
    started_at INTEGER,
    completed_at INTEGER,
    duration_ms INTEGER,
    first_user_item_id TEXT,
    final_agent_item_id TEXT, rollout_byte_offset INTEGER, rollout_end_ordinal INTEGER, rollout_end_byte_offset INTEGER, root_turn_id TEXT,
    PRIMARY KEY (thread_id, turn_id)
);

CREATE INDEX idx_thread_items_by_turn_page
    ON thread_items(thread_id, turn_id, rollout_ordinal);

CREATE INDEX idx_thread_items_by_turn_updated_page
    ON thread_items(thread_id, turn_id, updated_at_ordinal);

CREATE UNIQUE INDEX idx_thread_items_page
    ON thread_items(thread_id, rollout_ordinal);

CREATE INDEX idx_thread_items_updated_page
    ON thread_items(thread_id, updated_at_ordinal);

CREATE INDEX idx_thread_items_user_messages
    ON thread_items(thread_id, rollout_ordinal)
    WHERE item_type = 'userMessage';

CREATE INDEX idx_thread_realtime_items_boundary
    ON thread_realtime_items(thread_id, rollout_ordinal)
    WHERE item_type IN ('realtime_session_started', 'realtime_session_closed');

CREATE UNIQUE INDEX idx_thread_realtime_items_page
    ON thread_realtime_items(thread_id, rollout_ordinal);

CREATE INDEX idx_thread_turns_end_page
    ON thread_turns(thread_id, rollout_end_ordinal, turn_id)
    WHERE rollout_end_ordinal IS NOT NULL;

CREATE UNIQUE INDEX idx_thread_turns_page
    ON thread_turns(thread_id, rollout_ordinal);

CREATE TABLE thread_goal_continuation_deferrals (
    thread_id TEXT PRIMARY KEY NOT NULL REFERENCES thread_goals(thread_id) ON DELETE CASCADE
);

CREATE TABLE thread_goals (
    thread_id TEXT PRIMARY KEY NOT NULL,
    goal_id TEXT NOT NULL,
    objective TEXT NOT NULL,
    status TEXT NOT NULL CHECK(status IN (
        'active',
        'paused',
        'blocked',
        'usage_limited',
        'budget_limited',
        'complete'
    )),
    token_budget INTEGER,
    tokens_used INTEGER NOT NULL DEFAULT 0,
    time_used_seconds INTEGER NOT NULL DEFAULT 0,
    created_at_ms INTEGER NOT NULL,
    updated_at_ms INTEGER NOT NULL
);

CREATE TABLE consolidation_progress (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    max_thread_count INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE jobs (
    kind TEXT NOT NULL,
    job_key TEXT NOT NULL,
    status TEXT NOT NULL,
    worker_id TEXT,
    ownership_token TEXT,
    started_at INTEGER,
    finished_at INTEGER,
    lease_until INTEGER,
    retry_at INTEGER,
    retry_remaining INTEGER NOT NULL,
    last_error TEXT,
    input_watermark INTEGER,
    last_success_watermark INTEGER,
    PRIMARY KEY (kind, job_key)
);

CREATE TABLE stage1_outputs (
    thread_id TEXT PRIMARY KEY,
    source_updated_at INTEGER NOT NULL,
    raw_memory TEXT NOT NULL,
    rollout_summary TEXT NOT NULL,
    rollout_slug TEXT,
    generated_at INTEGER NOT NULL,
    usage_count INTEGER,
    last_usage INTEGER,
    selected_for_phase2 INTEGER NOT NULL DEFAULT 0,
    selected_for_phase2_source_updated_at INTEGER
);

CREATE INDEX idx_jobs_kind_status_retry_lease
    ON jobs(kind, status, retry_at, lease_until);

CREATE INDEX idx_stage1_outputs_source_updated_at
    ON stage1_outputs(source_updated_at DESC, thread_id DESC);

CREATE TABLE queued_items (
    id TEXT PRIMARY KEY NOT NULL,
    thread_id TEXT NOT NULL,
    payload_json TEXT NOT NULL,
    queue_order INTEGER NOT NULL,
    created_at_ms INTEGER NOT NULL,
    updated_at_ms INTEGER NOT NULL
);

CREATE TABLE queued_thread_revisions (
    revision INTEGER PRIMARY KEY AUTOINCREMENT,
    thread_id TEXT NOT NULL UNIQUE
);

CREATE UNIQUE INDEX queued_items_thread_order_idx
    ON queued_items(thread_id, queue_order);
)SCHEMA";

static const char logSchema[] = R"SCHEMA(
CREATE TABLE logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts INTEGER NOT NULL,
    ts_nanos INTEGER NOT NULL,
    level TEXT NOT NULL,
    target TEXT NOT NULL,
    feedback_log_body TEXT,
    module_path TEXT,
    file TEXT,
    line INTEGER,
    thread_id TEXT,
    process_uuid TEXT,
    estimated_bytes INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX idx_logs_process_uuid_threadless_ts ON logs(process_uuid, ts DESC, ts_nanos DESC, id DESC)
WHERE thread_id IS NULL;

CREATE INDEX idx_logs_thread_id ON logs(thread_id);

CREATE INDEX idx_logs_thread_id_ts ON logs(thread_id, ts DESC, ts_nanos DESC, id DESC);

CREATE INDEX idx_logs_ts ON logs(ts DESC, ts_nanos DESC, id DESC);
)SCHEMA";

static const char baselineId[] =
    "custom-codex-schema-20261125-sha256:"
    "cf1112822dcced2f5e31b421a0603396200d946a6a1c2a9cd3893b9a924bd980";

const char* maiCodexBusinessSchemaSql() {
    return businessSchema;
}

const char* maiCodexLogSchemaSql() {
    return logSchema;
}

const char* maiCodexSchemaBaselineId() {
    return baselineId;
}
