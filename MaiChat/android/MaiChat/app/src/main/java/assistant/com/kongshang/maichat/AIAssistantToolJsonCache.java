// 文件作用：限制工具呈现元数据的解析次数；不缓存或展开普通 shell 工具正文。
package com.kongshang.maichat;

import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.Map;
import org.json.JSONException;
import org.json.JSONObject;

/** Cached JSON is read-only. Bound both UTF-16 source bytes and entry count, including invalid JSON. */
final class AIAssistantToolJsonCache {
    interface Parser { JSONObject parse(String value) throws JSONException; }
    private final int byteLimit;
    private final Parser parser;
    private int bytes;
    private final LinkedHashMap<String, JSONObject> values = new LinkedHashMap<>(16, .75f, true);
    AIAssistantToolJsonCache(int byteLimit, Parser parser) {
        this.byteLimit = byteLimit; this.parser = parser;
    }
    synchronized JSONObject read(String source) throws JSONException {
        if (values.containsKey(source)) {
            JSONObject value = values.get(source);
            if (value == null) throw new JSONException("Invalid tool presentation JSON");
            return value;
        }
        JSONObject value = null;
        JSONException invalid = null;
        try { value = parser.parse(source); } catch (JSONException failure) { invalid = failure; }
        long size = (long) source.length() * 2;
        if (size <= byteLimit) {
            Iterator<Map.Entry<String, JSONObject>> iterator = values.entrySet().iterator();
            while (iterator.hasNext() && (bytes + size > byteLimit || values.size() >= 64)) {
                Map.Entry<String, JSONObject> entry = iterator.next();
                bytes -= entry.getKey().length() * 2; iterator.remove();
            }
            values.put(source, value); bytes += (int) size;
        }
        if (invalid != null) throw invalid;
        return value;
    }
    synchronized int retainedBytes() { return bytes; }
}
