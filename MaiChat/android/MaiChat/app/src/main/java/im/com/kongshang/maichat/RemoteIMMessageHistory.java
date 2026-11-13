// 文件作用：IM 持久化历史的普通代码接口；工具包装与业务调用共用同一查询契约。
// 边界：只覆盖当前账号的本机单聊数据库，不代表云端完整历史或一致性快照。
package com.kongshang.maichat;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Objects;

public final class RemoteIMMessageHistory {
    private RemoteIMMessageHistory() { }

    public static final class Query {
        public final String peerId, query, kind, direction, cursor;
        public final int limit;
        public final Long sinceMs, untilMs;

        /** query 为 null 时读取会话；非 null 时按现有 SQLite instr 文字语义搜索。 */
        public Query(String peerId, String query, int limit, String kind, String direction,
                     Long sinceMs, Long untilMs, String cursor) {
            this.peerId = clean(peerId);
            this.query = query == null ? null : query.trim();
            this.kind = clean(kind);
            this.direction = clean(direction);
            this.cursor = clean(cursor);
            if ((this.query == null && this.peerId.isEmpty()) ||
                    (this.query != null && this.query.isEmpty()))
                throw new IllegalArgumentException("peer_id or query is required");
            if (limit < 1) throw new IllegalArgumentException("limit must be positive");
            this.limit = Math.min(limit, 200);
            if (!this.kind.isEmpty() && !List.of("text", "image", "video", "voice", "file").contains(this.kind))
                throw new IllegalArgumentException("Invalid message kind");
            if (!this.direction.isEmpty() && !List.of("incoming", "outgoing").contains(this.direction))
                throw new IllegalArgumentException("Invalid message direction");
            if (sinceMs != null && untilMs != null && sinceMs > untilMs)
                throw new IllegalArgumentException("since_ms must not exceed until_ms");
            this.sinceMs = sinceMs;
            this.untilMs = untilMs;
        }

        boolean sameFilters(Query other) {
            return limit == other.limit && peerId.equals(other.peerId) && Objects.equals(query, other.query)
                && kind.equals(other.kind) && direction.equals(other.direction)
                && Objects.equals(sinceMs, other.sinceMs) && Objects.equals(untilMs, other.untilMs);
        }
    }

    /** 查询专用的七项持久化元数据；不恢复 UI 消息、附件路径、审批或引用对象。 */
    public static final class Row {
        public final String id, peerId, senderId, text, kind, direction;
        public final long createdAtMs;

        Row(String id, String peerId, String senderId, String text, String kind,
                String direction, long createdAtMs) {
            this.id = id;
            this.peerId = peerId;
            this.senderId = senderId;
            this.text = text;
            this.kind = kind;
            this.direction = direction;
            this.createdAtMs = createdAtMs;
        }
    }

    public static final class Page {
        public final List<Row> rows;
        public final boolean hasMore;
        public final String nextCursor;
        final String owner, loginScope;
        final Object databaseInstance;

        Page(List<Row> rows, boolean hasMore, String nextCursor,
             String owner, String loginScope, Object databaseInstance) {
            this.rows = Collections.unmodifiableList(new ArrayList<>(rows));
            this.hasMore = hasMore;
            this.nextCursor = nextCursor;
            this.owner = owner;
            this.loginScope = loginScope;
            this.databaseInstance = databaseInstance;
        }
    }

    private static String clean(String value) { return value == null ? "" : value.trim(); }
}
