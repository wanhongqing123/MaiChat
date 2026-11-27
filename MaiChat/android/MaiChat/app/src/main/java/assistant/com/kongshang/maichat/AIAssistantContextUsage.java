// 文件作用：将 Core 的版本化实测计量快照变为不可变 UI 文案；不估算 token 或模型窗口。
package com.kongshang.maichat;

import java.text.NumberFormat;
import java.util.Locale;
import org.json.JSONObject;

final class AIAssistantContextUsage {
    final String text, detail;
    final int usedPercent;

    private AIAssistantContextUsage(String text, String detail, int usedPercent) {
        this.text = text;
        this.detail = detail;
        this.usedPercent = usedPercent;
    }

    /** 每次按当前选中会话重建，拒绝旧会话/未知协议；不在 UI 上缓存最近一次有效占用。 */
    static AIAssistantContextUsage from(JSONObject usage, String session) {
        return from(usage, session, "");
    }
    static AIAssistantContextUsage from(JSONObject usage, String session, String model) {
        StringBuilder detail = new StringBuilder("窗口占用取最近一次主请求的 total_tokens；缓存仍占窗口，累计消耗不作为窗口占用。\n");
        if (usage == null || session == null || session.isEmpty()
                || !session.equals(usage.optString("session_id")))
            return missing(detail);
        if (!usage.optBoolean("available", true))
            return new AIAssistantContextUsage("上下文用量读取失败", detail + "计量源暂不可读，请稍后重试。", -1);
        if (usage.optInt("version") != 1) return missing(detail);
        JSONObject latest = usage.optJSONObject("latest_sampling");
        Count total = count(latest, "total_tokens");
        long window = integer(usage.opt("window_tokens"));
        String windowSource = usage.optString("window_source");
        boolean knownWindow = window > 0 && ("provider".equals(windowSource) || "configuration".equals(windowSource));
        boolean valid = usage.optBoolean("sampling_valid")
            && (model.isEmpty() || model.equals(usage.optString("model")));
        int used = -1;
        String text;
        if (!valid) text = "上下文：等待新的服务端用量";
        else if (!total.known()) text = "服务端未提供上下文用量";
        else {
            String prefix = total.derived ? "推算 · " : "";
            // 百分比完全由 Core 计算；未知/小窗口没有有效比例，不在 Java 重复实现基线算法。
            long left = integer(usage.opt("remaining_percent"));
            if (!knownWindow) text = prefix + number(total.tokens) + " used / 窗口未知";
            else if (left >= 0 && left <= 100) {
                text = prefix + left + "% left (" + number(total.tokens) + " used / " + number(window) + ")";
                used = 100 - (int) left;
            } else text = prefix + number(total.tokens) + " used / " + number(window);
            detail.append("最近主请求总量：").append(total.measured()).append(" tokens。\n")
                .append("输入：").append(count(latest, "input_tokens").measured())
                .append("；缓存命中：").append(count(latest, "cached_input_tokens").measured())
                .append("；新输入：").append(count(latest, "non_cached_input_tokens").measured()).append("。\n")
                .append("缓存写入：").append(count(latest, "cache_write_input_tokens").measured())
                .append("；输出：").append(count(latest, "output_tokens").measured())
                .append("；思考：").append(count(latest, "reasoning_output_tokens").measured()).append("。\n")
                .append("本次新输入加输出：").append(count(latest, "blended_total_tokens").measured())
                .append(" tokens；这是消费统计，不是窗口大小。\n");
            if (!count(latest, "cached_input_tokens").known())
                detail.append("供应商未提供缓存明细，新输入计算按缓存 0 退化。\n");
        }
        if (knownWindow)
            detail.append(valid ? "模型窗口：" : "上次测量窗口（当前已失效）：").append(number(window))
                .append(" tokens（").append("provider".equals(windowSource) ? "服务端" : "模型配置").append("）。\n");
        else detail.append("模型窗口未知，未计算百分比。\n");
        detail.append("本轮累计：").append(count(usage.optJSONObject("turn_token_usage"), "total_tokens").measured())
            .append("；会话累计（已记录部分）：").append(count(usage.optJSONObject("thread_token_usage"), "total_tokens").measured()).append("。\n");
        if ("compaction".equals(usage.optString("latest_operation")))
            detail.append("最近一条记录为压缩请求；其消耗计入累计，不能覆盖主请求窗口占用。\n");
        detail.append("计数无前缀为服务端提供；“推算”仅为已知计数的算术结果。缺失值不代表零。");
        return new AIAssistantContextUsage(text, detail.toString(), used);
    }

    private static AIAssistantContextUsage missing(StringBuilder detail) {
        return new AIAssistantContextUsage("服务端未提供上下文用量", detail + "服务端计量：未提供；模型窗口：未知。", -1);
    }
    private static String number(long value) { return NumberFormat.getIntegerInstance(Locale.US).format(value); }
    // JSON 数字须为非负整数；字符串、浮点和溢出值不能冒充服务端实测零值。
    private static long integer(Object value) {
        if (!(value instanceof Byte || value instanceof Short || value instanceof Integer || value instanceof Long)) return -1;
        long result = ((Number) value).longValue();
        return result < 0 ? -1 : result;
    }
    private static Count count(JSONObject usage, String key) {
        if (usage == null) return new Count(-1, false);
        String source = usage.optString(key + "_source");
        return new Count("provider".equals(source) || "derived".equals(source) ? integer(usage.opt(key)) : -1,
            "derived".equals(source));
    }
    private static final class Count {
        final long tokens;
        final boolean derived;
        Count(long tokens, boolean derived) { this.tokens = tokens; this.derived = derived; }
        boolean known() { return tokens >= 0; }
        String measured() { return known() ? (derived ? "推算 " : "") + number(tokens) : "未提供"; }
    }
}
