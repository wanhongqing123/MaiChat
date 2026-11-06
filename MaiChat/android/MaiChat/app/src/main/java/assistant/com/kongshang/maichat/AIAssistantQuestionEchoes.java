// 文件作用：回答 ACK 与工具完成快照之间保留显示回声；不写模型历史或伪造 User message id。
package com.kongshang.maichat;

import java.util.LinkedHashMap;
import java.util.Iterator;
import java.util.Map;
import org.json.JSONObject;
import org.json.JSONArray;

final class AIAssistantQuestionEchoes {
    private final LinkedHashMap<String, JSONObject> acknowledged = new LinkedHashMap<>();

    void acknowledge(String owner, JSONObject payload) {
        try {
            JSONObject captured = payload.optJSONObject("question");
            JSONObject echo = captured == null ? new JSONObject() : new JSONObject(captured.toString());
            String part = echo.optString("partId", payload.optString("questionId"));
            if (part.isEmpty()) return;
            echo.put("partId", part).put("answer", payload.optString("text")).put("echoOwner", owner);
            acknowledged.put(owner + "\u001f" + part, echo);
            while (acknowledged.size() > 64) acknowledged.remove(acknowledged.keySet().iterator().next());
        } catch (Exception impossible) { throw new IllegalStateException("Invalid answer echo", impossible); }
    }

    JSONArray visible(String owner, JSONArray roots, JSONArray children) {
        return visible(owner, roots, children, new JSONArray());
    }
    JSONArray visible(String owner, JSONArray roots, JSONArray children, JSONArray updates) {
        java.util.Set<String> canonical = new java.util.HashSet<>();
        for (JSONArray messages : new JSONArray[] {roots, children, updates}) {
            if (messages == null) continue;
            for (int i = 0; i < messages.length(); i++) {
                JSONObject row = messages.optJSONObject(i);
                JSONArray parts = row == null ? null : row.optJSONArray("parts");
                if (parts == null) continue;
                for (int j = 0; j < parts.length(); j++) {
                    JSONObject part = parts.optJSONObject(j);
                    if (part != null && part.optString("kind").equals("tool") && part.optString("tool").equals("question")
                        && part.optString("state").equals("completed") && !part.optString("output").isEmpty())
                        canonical.add(part.optString("id"));
                }
            }
        }
        JSONArray result = new JSONArray();
        Iterator<Map.Entry<String, JSONObject>> entries = acknowledged.entrySet().iterator();
        while (entries.hasNext()) {
            JSONObject echo = entries.next().getValue();
            if (!owner.equals(echo.optString("echoOwner"))) continue;
            if (canonical.contains(echo.optString("partId"))) { entries.remove(); continue; }
            try { result.put(new JSONObject(echo.toString())); }
            catch (Exception impossible) { throw new IllegalStateException("Invalid retained echo", impossible); }
        }
        return result;
    }
}
