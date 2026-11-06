// 文件作用：保留已消费 Native 更新的待发布页，网络失败不能把旧 owner 增量丢回 recent 窗口之外。
package com.kongshang.maichat;

import java.util.HashMap;
import java.util.Map;
import org.json.JSONObject;

final class AIAssistantPublishPages {
    private final Map<String, JSONObject> pending = new HashMap<>();
    private static String key(String session, String before) { return session + "\u001f" + before; }
    void remember(String session, String before, JSONObject page) {
        try { pending.put(key(session, before), new JSONObject(page.toString())); }
        catch (Exception impossible) { throw new IllegalStateException("Invalid captured publish page", impossible); }
    }
    JSONObject page(String session, String before) { return pending.get(key(session, before)); }
    void complete(String session, String before) { pending.remove(key(session, before)); }
    void discard(String session) { pending.keySet().removeIf(key -> key.startsWith(session + "\u001f")); }
}
