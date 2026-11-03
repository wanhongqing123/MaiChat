// Defines the bounded, metadata-only remote diagnostics exchange used by Android.
package com.kongshang.maichat;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayDeque;
import java.util.List;
import java.util.Locale;
import java.util.TimeZone;
import java.util.regex.Pattern;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import org.json.JSONTokener;

final class MaiRemoteDiagnosticsProtocol {
    static final int MAX_BYTES = 2 * 1024 * 1024;
    private static final int MAX_JSON_DEPTH = 32;
    private static final int MAX_MESSAGES = 1000;
    private static final long MESSAGE_WINDOW_MS = 30 * 60 * 1000L;
    private static final Pattern REQUEST_ID = Pattern.compile(
        "[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}");
    private static final Pattern SAFE_STRING = Pattern.compile("[a-zA-Z0-9_.:@/-]+");
    private static final String[] STRING_METADATA = {
        "id", "ID", "remoteMessageId", "callId", "sessionId", "taskId", "replyId", "messageId", "partId",
        "threadId", "turnId", "eventTaskId", "eventReplyId", "sourceCommit", "onDiskBinarySha256",
        "onDiskBinaryStatus", "stage", "event", "kind", "type", "phase", "delivery", "sourceKind", "cli", "status",
        "terminalKind", "hostStopReason", "stopReasonRequested", "spawnErrorCode", "signal", "exitCodeHex",
        "appVersion"
    };
    private static final String[] NUMBER_METADATA = {
        "createdAt", "startedAt", "pid", "hostPid", "lifetimeMs", "lastInputAt", "lastOutputAt", "lastEtxAt",
        "stopRequestedAt", "exitCode", "textLength", "inputLength", "resolvedLength", "forwardedChunks", "code",
        "errorCode", "messageId", "attempt", "onDiskBinaryBytes", "duration_ms", "samples", "slow_samples",
        "slow_threshold_ms", "max_ms", "average_ms", "late_ms", "sample_interval_ms", "visibleLength", "cellCount",
        "replacedCells"
    };
    private static final String[] BOOLEAN_METADATA = {
        "ok", "sourceStarted", "autoReplyToIm", "sdkReady", "isReady", "accepted", "hasStream", "replay"
    };

    private MaiRemoteDiagnosticsProtocol() { }

    static String command(String id) {
        requireId(id);
        return "/diagnostics " + id;
    }

    static String reportName(String id) {
        requireId(id);
        return "remote-diagnostics-" + id + ".json";
    }

    // Apply the diagnostic ceiling only to canonical basenames, before download or cache delivery.
    static long attachmentLimit(String name, long ordinaryLimit) {
        String prefix = "remote-diagnostics-";
        if (name == null || !name.startsWith(prefix) || !name.endsWith(".json")) return ordinaryLimit;
        String id = name.substring(prefix.length(), name.length() - 5);
        return validId(id) ? Math.min(MAX_BYTES, ordinaryLimit) : ordinaryLimit;
    }

    static JSONObject sanitized(byte[] data, String id) throws IOException {
        if (data == null || data.length > MAX_BYTES) throw new IOException("Remote report exceeds 2 MiB");
        if (!validId(id)) throw invalidReport();
        try {
            String json = StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(data)).toString();
            checkJsonDepth(json);
            JSONTokener tokener = new JSONTokener(json);
            Object value = tokener.nextValue();
            if (!(value instanceof JSONObject) || tokener.nextClean() != 0) throw invalidReport();
            JSONObject raw = (JSONObject) value;
            Number schema = finiteNumber(raw.opt("schemaVersion"));
            JSONArray files = raw.optJSONArray("files");
            if (schema == null || schema.doubleValue() != 1 || !id.equals(raw.opt("requestId"))
                    || files == null || files.length() > 16) throw invalidReport();
            JSONObject clean = new JSONObject();
            clean.put("schemaVersion", 1).put("requestId", id);
            copyStrings(raw, clean, new String[]{"appVersion", "platform", "projectId", "electronVersion", "timeZone"});
            copyNumbers(raw, clean, new String[]{"exportedAt", "from"});
            JSONArray cleanFiles = new JSONArray();
            for (int i = 0; i < files.length(); i++) {
                JSONObject file = files.optJSONObject(i);
                if (file == null) throw invalidReport();
                JSONObject cleanFile = new JSONObject();
                copyStrings(file, cleanFile, new String[]{"source", "status"});
                JSONArray events = file.optJSONArray("events");
                Object truncated = file.opt("truncated");
                cleanFile.put("truncated", Boolean.TRUE.equals(truncated) || (events != null && events.length() > 1000));
                Number invalidLines = finiteNumber(file.opt("invalidLines"));
                cleanFile.put("invalidLines", invalidLines == null ? 0 : Math.max(0, invalidLines.longValue()));
                cleanFile.put("events", metadataArray(events, 1000, 0));
                cleanFiles.put(cleanFile);
            }
            clean.put("files", cleanFiles);
            clean.put("activeSessions", metadataArray(raw.optJSONArray("activeSessions"), 32, 0));
            JSONObject coverage = raw.optJSONObject("sourceCoverage");
            if (coverage != null) {
                JSONObject cleanCoverage = new JSONObject();
                Object original = coverage.opt("aicliOriginalEvents");
                if ("see-codex-original-events".equals(original) || "unavailable".equals(original))
                    cleanCoverage.put("aicliOriginalEvents", original);
                if ("unavailable".equals(coverage.opt("uiPerformance"))) cleanCoverage.put("uiPerformance", "unavailable");
                if (cleanCoverage.length() > 0) clean.put("sourceCoverage", cleanCoverage);
            }
            return clean;
        } catch (JSONException | CharacterCodingException exception) {
            // Never echo malformed remote content or parser exception excerpts into reports or UI.
            throw invalidReport();
        }
    }

    // Cache files may grow after stat; read at most the ceiling plus one byte before parsing.
    static JSONObject readReport(File file, String id) throws IOException {
        if (file == null || !file.isFile()) throw invalidReport();
        if (file.length() > MAX_BYTES) throw new IOException("Remote report exceeds 2 MiB");
        try (FileInputStream stream = new FileInputStream(file);
                ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[8192];
            while (output.size() <= MAX_BYTES) {
                int count = stream.read(buffer, 0, Math.min(buffer.length, MAX_BYTES + 1 - output.size()));
                if (count < 0) return sanitized(output.toByteArray(), id);
                output.write(buffer, 0, count);
            }
            throw new IOException("Remote report exceeds 2 MiB");
        }
    }

    // Match only receipts bound to this request; remote free text is replaced with a fixed explanation.
    static String failureReceipt(String text, String id) {
        if (text == null || !validId(id)) return null;
        if (text.equals("远程排障请求过于频繁，请稍后重试（" + id + "）。"))
            return "远端拒绝了过于频繁的采集请求；本报告仅含本地现场。";
        if (text.startsWith("远程排障采集失败（编号 " + id + "）："))
            return "远端报告采集失败；本报告仅含本地现场。";
        if (text.equals("当前 MultiAICode 尚未接入远程排障（" + id + "），请升级后重试。"))
            return "远端排障服务尚未接入，暂未取得远端现场；本报告仅含本地现场。";
        int newline = text.indexOf('\n');
        String firstLine = newline < 0 ? text : text.substring(0, newline);
        if (firstLine.equals("不支持的 IM 控制命令：/diagnostics " + id))
            return "远端版本不支持远程排障；本报告仅含本地现场。";
        return null;
    }

    // Match DiagnosticLogPrivacy.stableTag: trimmed UTF-8 SHA-256, first eight bytes.
    static String tag(String value, String prefix) {
        String clean = trimWhitespace(value);
        if (clean.isEmpty()) return "<empty>";
        try {
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(clean.getBytes(StandardCharsets.UTF_8));
            StringBuilder result = new StringBuilder();
            String cleanPrefix = trimWhitespace(prefix == null ? "" : prefix.replace('\r', ' ').replace('\n', ' ').replace('\t', ' '));
            result.append(cleanPrefix.length() > 12 ? cleanPrefix.substring(0, 12) + "…" : cleanPrefix).append('#');
            final char[] hex = "0123456789abcdef".toCharArray();
            for (int i = 0; i < 8; i++) result.append(hex[(digest[i] & 0xff) >>> 4]).append(hex[digest[i] & 0xf]);
            return result.toString();
        } catch (NoSuchAlgorithmException exception) {
            throw new IllegalStateException("SHA-256 unavailable", exception);
        }
    }

    // The caller supplies only the current account and peer's messages; no raw log or message body enters this API.
    static JSONObject localEvidence(String version, String os, String owner, String peer, boolean displayed,
            long collectedAt, List<MessageMetadata> messages) {
        try {
            JSONObject local = new JSONObject();
            local.put("appVersion", safeString(version) == null ? "unavailable" : version);
            local.put("platform", "Android").put("osVersion", safeString(os) == null ? "unavailable" : os);
            local.put("ownerTag", tag(owner, "u")).put("peerTag", tag(peer, "u"));
            local.put("collectedAt", collectedAt).put("displayedConversation", displayed);
            local.put("timeZone", TimeZone.getDefault().getID());
            ArrayDeque<MessageMetadata> selected = new ArrayDeque<>();
            long since = collectedAt < Long.MIN_VALUE + MESSAGE_WINDOW_MS ? Long.MIN_VALUE : collectedAt - MESSAGE_WINDOW_MS;
            if (messages != null) for (MessageMetadata message : messages) {
                if (message == null || message.createdAt < since || message.createdAt > collectedAt) continue;
                if (selected.size() == MAX_MESSAGES) selected.removeFirst();
                selected.addLast(message);
            }
            JSONArray metadata = new JSONArray();
            for (MessageMetadata message : selected) {
                JSONObject item = new JSONObject();
                item.put("id", tag(message.id, "m"));
                if (message.remoteId != null && !message.remoteId.trim().isEmpty()) item.put("remoteID", tag(message.remoteId, "m"));
                item.put("logMessageTag", tag(message.remoteId == null || message.remoteId.trim().isEmpty() ? message.id : message.remoteId, "m"));
                item.put("direction", enumValue(message.direction, "incoming", "outgoing"));
                item.put("status", enumValue(message.status, "pending", "sent", "received", "failed"));
                item.put("kind", enumValue(message.kind, "message", "image", "file", "video", "voice"));
                item.put("createdAt", message.createdAt);
                metadata.put(item);
            }
            int loaded = messages == null ? 0 : messages.size();
            local.put("messages", metadata).put("loadedMessageCount", loaded).put("excludedMessageCount", loaded - metadata.length());
            local.put("logs", new JSONArray()).put("speechSessions", new JSONArray());
            local.put("logCoverage", "Current account and peer loaded message metadata from the last 30 minutes; at most 1000 entries. No message bodies or raw logs.");
            local.put("performanceCoverage", new JSONObject().put("status", "unavailable")
                .put("scope", "current-account-peer-loaded-messages").put("stackTraces", "unavailable")
                .put("interpretation", "No retained performance samples or latency instrumentation; absent evidence is not zero latency. Message timestamps do not prove screen presentation."));
            local.put("speechCoverage", new JSONObject().put("status", "unavailable")
                .put("limits", "No speech stage, audio packet, network RTT or screen-presentation timing."));
            return local;
        } catch (JSONException exception) {
            throw new IllegalStateException("Cannot encode local diagnostics metadata", exception);
        }
    }

    static byte[] mergedReport(String id, JSONObject local, JSONObject remote, String missingReason)
            throws IOException {
        if (!validId(id) || local == null) throw invalidReport();
        try {
            String remoteJSON = remote == null ? "null" : sanitized(remote.toString().getBytes(StandardCharsets.UTF_8), id).toString(2);
            String note = missingReason == null ? "已收到远端报告；各源的缺失、损坏或截断情况见报告字段。" : missingReason;
            String report = "# MaiChat 跨端远程排障报告\n\n"
                + "请协助排查这段聊天近期的消息收发、键盘输入、界面响应或展示异常；以下是自动收集的现场，未预判根因。\n\n"
                + "排障编号：" + id + "\n\n" + note + "\n\n"
                + "## A 端现场（Android MaiChat）\n\n"
                + "以下是本地消息元数据及采集时的会话选择状态，不是历史屏幕实际绘制完成的证明。不包含消息正文或原始日志。\n\n"
                + "```json\n" + local.toString(2) + "\n```\n\n"
                + "## B 端现场（MultiAICode）\n\n```json\n" + remoteJSON + "\n```\n\n"
                + "缺失记录不代表没有故障或耗时为零；两端时钟可能不同。请结合稳定消息标签与排障编号分析；本地标签不能直接还原原始消息 ID。\n"
                + "sourceCommit 是进程启动时记录的 manifest 提交；onDiskBinarySha256 是采集时的磁盘文件，不证明旧进程已经加载它。\n";
            return report.getBytes(StandardCharsets.UTF_8);
        } catch (JSONException exception) {
            throw invalidReport();
        }
    }

    private static JSONObject metadata(Object value, int depth) throws JSONException {
        JSONObject clean = new JSONObject();
        if (depth > 3 || !(value instanceof JSONObject)) return clean;
        JSONObject raw = (JSONObject) value;
        copyStrings(raw, clean, STRING_METADATA);
        copyNumbers(raw, clean, NUMBER_METADATA);
        for (String key : BOOLEAN_METADATA) if (raw.opt(key) instanceof Boolean) clean.put(key, raw.opt(key));
        if (raw.has("detail")) clean.put("detail", metadata(raw.opt("detail"), depth + 1));
        JSONArray candidates = raw.optJSONArray("candidates");
        if (candidates != null) clean.put("candidates", metadataArray(candidates, 20, depth + 1));
        return clean;
    }

    private static JSONArray metadataArray(JSONArray raw, int limit, int depth) throws JSONException {
        JSONArray clean = new JSONArray();
        if (raw != null) for (int i = 0; i < Math.min(limit, raw.length()); i++) clean.put(metadata(raw.opt(i), depth));
        return clean;
    }

    private static String safeString(Object value) {
        if (!(value instanceof String)) return null;
        String text = (String) value;
        return text.length() <= 200 && SAFE_STRING.matcher(text).matches() ? text : null;
    }

    private static Number finiteNumber(Object value) {
        if (!(value instanceof Number)) return null;
        Number number = (Number) value;
        return Double.isNaN(number.doubleValue()) || Double.isInfinite(number.doubleValue()) ? null : number;
    }

    private static void copyStrings(JSONObject raw, JSONObject clean, String[] keys) throws JSONException {
        for (String key : keys) {
            String value = safeString(raw.opt(key));
            if (value != null) clean.put(key, value);
        }
    }

    private static void copyNumbers(JSONObject raw, JSONObject clean, String[] keys) throws JSONException {
        for (String key : keys) {
            Number value = finiteNumber(raw.opt(key));
            if (value != null) clean.put(key, value);
        }
    }

    // Validate strict JSON before org.json parses recursively. Its permissive single-quote/comment
    // syntax must not hide nested containers from the depth boundary.
    private static void checkJsonDepth(String json) throws IOException {
        new JsonBoundary(json).validate();
    }

    private static final class JsonBoundary {
        private final String json;
        private int cursor;

        JsonBoundary(String json) { this.json = json; }

        void validate() throws IOException {
            value(0);
            whitespace();
            if (cursor != json.length()) throw invalidReport();
        }

        private void value(int depth) throws IOException {
            whitespace();
            char token = peek();
            if (token == '{' || token == '[') {
                if (depth >= MAX_JSON_DEPTH) throw invalidReport();
                cursor++;
                whitespace();
                boolean object = token == '{';
                char closing = object ? '}' : ']';
                if (take(closing)) return;
                while (true) {
                    if (object) {
                        string();
                        whitespace();
                        if (!take(':')) throw invalidReport();
                    }
                    value(depth + 1);
                    whitespace();
                    if (take(closing)) return;
                    if (!take(',')) throw invalidReport();
                    whitespace();
                }
            } else if (token == '"') string();
            else if (token == 't') literal("true");
            else if (token == 'f') literal("false");
            else if (token == 'n') literal("null");
            else number();
        }

        private void string() throws IOException {
            if (!take('"')) throw invalidReport();
            while (cursor < json.length()) {
                char token = json.charAt(cursor++);
                if (token == '"') return;
                if (token < 0x20) throw invalidReport();
                if (token != '\\') continue;
                if (cursor >= json.length()) throw invalidReport();
                char escape = json.charAt(cursor++);
                if (escape == 'u') {
                    for (int i = 0; i < 4; i++) {
                        if (cursor >= json.length()) throw invalidReport();
                        char hex = json.charAt(cursor++);
                        if (!(hex >= '0' && hex <= '9') && !(hex >= 'a' && hex <= 'f')
                                && !(hex >= 'A' && hex <= 'F')) throw invalidReport();
                    }
                } else if ("\"\\/bfnrt".indexOf(escape) < 0) throw invalidReport();
            }
            throw invalidReport();
        }

        private void number() throws IOException {
            take('-');
            if (!take('0')) {
                if (peek() < '1' || peek() > '9') throw invalidReport();
                digits();
            }
            if (take('.')) {
                if (!digit(peek())) throw invalidReport();
                digits();
            }
            if (take('e') || take('E')) {
                if (!take('+')) take('-');
                if (!digit(peek())) throw invalidReport();
                digits();
            }
        }

        private void literal(String text) throws IOException {
            if (!json.startsWith(text, cursor)) throw invalidReport();
            cursor += text.length();
        }

        private void whitespace() {
            while (peek() == ' ' || peek() == '\t' || peek() == '\r' || peek() == '\n') cursor++;
        }

        private boolean take(char token) {
            if (peek() != token) return false;
            cursor++;
            return true;
        }

        private char peek() { return cursor < json.length() ? json.charAt(cursor) : 0; }
        private boolean digit(char token) { return token >= '0' && token <= '9'; }
        private void digits() { while (digit(peek())) cursor++; }
    }

    private static String enumValue(String value, String... allowed) {
        String clean = value == null ? "" : value.toLowerCase(Locale.ROOT);
        for (String candidate : allowed) if (candidate.equals(clean)) return candidate;
        return "unknown";
    }

    // Foundation's whitespace/newline set also contains Unicode spaces and the next-line character.
    private static String trimWhitespace(String value) {
        if (value == null) return "";
        int start = 0;
        int end = value.length();
        while (start < end && whitespace(value.charAt(start))) start++;
        while (end > start && whitespace(value.charAt(end - 1))) end--;
        return value.substring(start, end);
    }

    private static boolean whitespace(char value) {
        return Character.isWhitespace(value) || Character.isSpaceChar(value) || value == '\u0085';
    }

    private static boolean validId(String id) { return id != null && REQUEST_ID.matcher(id).matches(); }
    private static void requireId(String id) {
        if (!validId(id)) throw new IllegalArgumentException("Expected a canonical lowercase diagnostics UUID");
    }
    private static IOException invalidReport() { return new IOException("Remote report format or request ID mismatch"); }

    static final class MessageMetadata {
        final String id, remoteId, direction, status, kind;
        final long createdAt;

        MessageMetadata(String id, String remoteId, String direction, String status, long createdAt, String kind) {
            this.id = id;
            this.remoteId = remoteId;
            this.direction = direction;
            this.status = status;
            this.createdAt = createdAt;
            this.kind = kind;
        }
    }
}
