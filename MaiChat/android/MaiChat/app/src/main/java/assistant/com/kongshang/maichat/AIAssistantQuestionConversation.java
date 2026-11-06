// 文件作用：将持久化 question 工具投影到普通消息流；不向模型历史重复插入问题或答案。
package com.kongshang.maichat;

import org.json.JSONArray;
import org.json.JSONObject;

final class AIAssistantQuestionConversation {
    private AIAssistantQuestionConversation() { }

    // questionMessages 是独立小集合，只在显示层合流，不能污染主会话历史分页的 before/cursor。
    static JSONArray messages(JSONArray roots, JSONArray children, JSONArray pending) {
        return messages(roots, children, pending, new JSONArray());
    }
    static JSONArray messages(JSONArray roots, JSONArray children, JSONArray pending, JSONArray echoes) {
        java.util.Map<String, JSONObject> rows = new java.util.LinkedHashMap<>();
        java.util.Set<String> shown = new java.util.HashSet<>();
        for (JSONArray source : new JSONArray[] {roots, children}) {
            if (source == null) continue;
            for (int i = 0; i < source.length(); i++) {
                JSONObject message = source.optJSONObject(i);
                if (message == null) continue;
                try {
                    JSONObject display = echoes == null || echoes.length() == 0 ? message : new JSONObject(message.toString())
                        .put("questionAnswerEchoes", echoes);
                    rows.putIfAbsent(message.optString("id"), display);
                } catch (Exception impossible) { throw new IllegalStateException("Invalid display message", impossible); }
                JSONArray parts = message.optJSONArray("parts");
                if (parts == null) continue;
                for (int j = 0; j < parts.length(); j++) {
                    JSONObject part = parts.optJSONObject(j);
                    if (part != null && !questionText(part).isEmpty()) shown.add(part.optString("id"));
                }
            }
        }
        JSONArray missing = new JSONArray();
        for (JSONArray source : new JSONArray[] {pending, echoes})
            if (source != null) for (int i = 0; i < source.length(); i++) missing.put(source.optJSONObject(i));
        for (int i = 0; i < missing.length(); i++) {
            JSONObject question = missing.optJSONObject(i);
            if (question == null) continue;
            String part = question.optString("partId", question.optString("id"));
            if (shown.contains(part)) continue;
            String body = formatQuestion(question);
            if (body.isEmpty()) continue;
            try {
                // owner 消息暂未同步时仍显示普通正文；真实 part 到达后按 partId 去掉此占位。
                String id = "pending-question:" + part;
                rows.put(id, new JSONObject().put("id", id).put("role", "assistant")
                    .put("sessionId", question.optString("sessionId")).put("questionProjection", true)
                    .put("created", question.optLong("asked")).put("questionAnswerEchoes", echoes)
                    .put("parts", parts(new JSONArray().put(new JSONObject().put("id", part).put("kind", "tool")
                        .put("tool", "question").put("state", "running").put("input", question.toString())), echoes)));

                shown.add(part);
            } catch (Exception impossible) { throw new IllegalStateException("Invalid pending question", impossible); }
        }
        java.util.List<JSONObject> ordered = new java.util.ArrayList<>(rows.values());
        ordered.sort(java.util.Comparator.comparingLong((JSONObject row) -> row.optLong("created"))
            .thenComparing(row -> row.optString("id")));
        return interleaveParts(ordered);
    }

    static JSONArray messages(JSONArray roots, JSONArray children, JSONArray pending, JSONArray echoes, JSONArray updates) {
        if (updates == null || updates.length() == 0) return messages(roots, children, pending, echoes);
        JSONArray display = new JSONArray();
        java.util.Set<String> known = new java.util.HashSet<>(), context = new java.util.HashSet<>();
        long cutoff = Long.MAX_VALUE;
        if (roots != null) for (int i = 0; i < roots.length(); i++) {
            JSONObject root = roots.optJSONObject(i);
            if (root == null) continue;
            display.put(root); known.add(root.optString("id"));
            cutoff = Math.min(cutoff, anchorTime(root, 0));
        }
        if (cutoff == Long.MAX_VALUE) cutoff = 0;
        for (JSONArray questions : new JSONArray[] {pending, echoes}) if (questions != null)
            for (int i = 0; i < questions.length(); i++) {
                JSONObject question = questions.optJSONObject(i);
                if (question != null) context.add(question.optString("partId", question.optString("id")));
            }
        for (int i = 0; i < updates.length(); i++) {
            JSONObject update = updates.optJSONObject(i);
            if (update == null || known.contains(update.optString("id"))) continue;
            JSONArray all = update.optJSONArray("parts"), recent = new JSONArray();
            if (all == null) continue;
            for (int j = 0; j < all.length(); j++) {
                JSONObject part = all.optJSONObject(j);
                if (part != null && (anchorTime(part, update.optLong("created")) >= cutoff
                    || context.contains(part.optString("id")))) recent.put(part);
            }
            if (recent.length() == 0) continue;
            try {
                display.put(new JSONObject(update.toString()).put("parts", recent).put("displayUpdate", true));
            } catch (Exception impossible) { throw new IllegalStateException("Invalid root display update", impossible); }
        }
        return messages(display, children, pending, echoes);
    }

    private static String orderKey(String id) {
        String suffix = id.substring(id.lastIndexOf('_') + 1);
        String alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
        return (suffix.length() == 21 || suffix.length() == 26)
            && suffix.chars().allMatch(value -> alphabet.indexOf(value) >= 0) ? suffix : id;
    }
    private static long anchorTime(JSONObject anchor, long fallback) {
        long explicit = anchor.optLong("created");
        if (explicit > 0) return explicit;
        String id = anchor.optString("id"), key = orderKey(id);
        if (key.equals(id)) return fallback;
        String alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
        long time = 0;
        for (int i = 0; i < 10; i++) time = time * 32 + alphabet.indexOf(key.charAt(i));
        return time > 0 ? time : fallback;
    }

    private static final class Unit {
        final JSONObject owner;
        final JSONArray parts;
        final long time;
        final String order;
        Unit(JSONObject owner, JSONArray parts, JSONObject anchor) {
            this.owner = owner; this.parts = parts;
            order = orderKey(anchor.optString("id"));
            // 新宿主传 part.created，旧历史使用 Core 21 位标识中的时间，不假定标准 ULID 长度。
            time = anchorTime(anchor, owner.optLong("created"));
        }
    }

    private static JSONArray interleaveParts(java.util.List<JSONObject> rows) {
        JSONArray unchanged = new JSONArray();
        for (JSONObject row : rows) unchanged.put(row);
        java.util.List<Unit> units = new java.util.ArrayList<>();
        for (JSONObject row : rows) {
            JSONArray parts = row.optJSONArray("parts");
            if (!row.optString("role").equals("assistant") || parts == null || parts.length() == 0
                || row.optString("id").startsWith("pending-question:")) {
                units.add(new Unit(row, parts == null ? new JSONArray() : parts, row));
                continue;
            }
            for (int i = 0; i < parts.length(); i++) {
                JSONObject part = parts.optJSONObject(i);
                if (part != null) units.add(new Unit(row, new JSONArray().put(part), part));
            }
        }
        units.sort(java.util.Comparator.comparingLong((Unit unit) -> unit.time).thenComparing(unit -> unit.order));
        java.util.List<JSONObject> orderedOwners = new java.util.ArrayList<>();
        JSONObject lastOwner = null;
        for (Unit unit : units) if (unit.owner != lastOwner) { orderedOwners.add(unit.owner); lastOwner = unit.owner; }
        boolean sameOrder = orderedOwners.size() == rows.size();
        for (int i = 0; sameOrder && i < rows.size(); i++) sameOrder = orderedOwners.get(i) == rows.get(i);
        // 绝大多数消息没有跨 owner 交错，直接复用原 JSON/行；仅真交错时创建稳定显示分段。
        if (sameOrder) return unchanged;
        java.util.List<JSONObject> fragments = new java.util.ArrayList<>();
        JSONObject previousOwner = null, fragment = null;
        for (Unit unit : units) try {
            if (unit.owner != previousOwner) {
                fragment = new JSONObject(unit.owner.toString()).put("parts", new JSONArray());
                JSONObject first = unit.parts.optJSONObject(0);
                fragment.put("displayId", unit.owner.optString("id") + ":" + (first == null ? "empty" : first.optString("id")));
                fragments.add(fragment);
                previousOwner = unit.owner;
            }
            JSONArray target = fragment.getJSONArray("parts");
            for (int i = 0; i < unit.parts.length(); i++) target.put(unit.parts.get(i));
        } catch (Exception impossible) { throw new IllegalStateException("Invalid question chronology", impossible); }
        java.util.Set<String> lastOwners = new java.util.HashSet<>();
        for (int i = fragments.size() - 1; i >= 0; i--) {
            JSONObject row = fragments.get(i);
            if (!lastOwners.add(row.optString("id"))) try {
                // 中间片段不重复元信息或媒体；真实 message.id 始终保留，只有显示 ID 分段。
                row.put("fragmentContinues", true).put("active", false).remove("inline_media");
            } catch (Exception impossible) { throw new IllegalStateException("Invalid message fragment", impossible); }
        }
        JSONArray result = new JSONArray();
        for (JSONObject row : fragments) result.put(row);
        return result;
    }

    static String copyText(JSONObject message) {
        StringBuilder body = new StringBuilder();
        JSONArray source = message.optJSONArray("parts");
        boolean questionMessage = false;
        if (source != null) for (int i = 0; i < source.length(); i++) {
            JSONObject part = source.optJSONObject(i);
            if (part != null && !questionText(part).isEmpty()) { questionMessage = true; break; }
        }
        JSONArray visible = parts(source, message.optJSONArray("questionAnswerEchoes"));
        for (int i = 0; i < visible.length(); i++) {
            JSONObject part = visible.optJSONObject(i);
            if (part == null || !(part.optString("kind").equals("text")
                || part.optString("kind").equals("question_answer"))) continue;
            if (questionMessage && body.length() > 0) body.append("\n\n");
            body.append(part.optString("text"));
        }
        return body.toString();
    }

    static boolean waiting(JSONObject row, JSONArray pending) {
        JSONArray parts = row.optJSONArray("parts");
        if (parts == null || pending == null) return false;
        for (int i = 0; i < pending.length(); i++) {
            JSONObject question = pending.optJSONObject(i);
            if (question == null) continue;
            String expected = question.optString("partId", question.optString("id"));
            for (int j = 0; j < parts.length(); j++) {
                JSONObject part = parts.optJSONObject(j);
                if (part != null && expected.equals(part.optString("id"))) return true;
            }
        }
        return false;
    }

    static JSONArray parts(JSONArray source) { return parts(source, new JSONArray()); }

    static JSONArray parts(JSONArray source, JSONArray echoes) {
        JSONArray visible = new JSONArray();
        if (source == null) return visible;
        for (int i = 0; i < source.length(); i++) {
            JSONObject part = source.optJSONObject(i);
            if (part == null) continue;
            String question = questionText(part);
            if (question.isEmpty()) { visible.put(part); continue; }
            try {
                String id = part.optString("id");
                visible.put(new JSONObject().put("id", id).put("kind", "text").put("text", question));
                // Core 仅在真正回答后把 output 设为答案；取消/失败不能伪装成用户发言。
                String answer = part.optString("state").equals("completed") ? part.optString("output") : "";
                if (answer.isEmpty() && echoes != null) for (int j = 0; j < echoes.length(); j++) {
                    JSONObject echo = echoes.optJSONObject(j);
                    if (echo != null && id.equals(echo.optString("partId"))) { answer = echo.optString("answer"); break; }
                }
                if (!answer.isEmpty()) visible.put(new JSONObject().put("id", id + ":answer").put("kind", "question_answer")
                    .put("text", answer));
            } catch (Exception impossible) { throw new IllegalStateException("Invalid question projection", impossible); }
        }
        return visible;
    }

    private static String questionText(JSONObject part) {
        if (!part.optString("kind").equals("tool") || !part.optString("tool").equals("question")) return "";
        try {
            return formatQuestion(new JSONObject(part.optString("input")));
        } catch (Exception partialInput) {
            // 工具参数仍在流入时保持普通工具状态，完整问题出现后再投影，避免显示残缺 JSON。
            return "";
        }
    }
    private static String formatQuestion(JSONObject input) {
        String question = input.optString("question");
        if (question.trim().isEmpty()) return "";
        StringBuilder body = new StringBuilder(question);
        JSONArray options = input.optJSONArray("options");
        if (options != null && options.length() > 0) {
            body.append("\n");
            for (int i = 0; i < options.length(); i++) {
                String option = options.optString(i);
                if (!option.trim().isEmpty()) body.append("\n- ").append(option);
            }
        }
        return body.toString();
    }

}
