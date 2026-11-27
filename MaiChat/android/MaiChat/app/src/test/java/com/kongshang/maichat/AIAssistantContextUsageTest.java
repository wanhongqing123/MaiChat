// 真实计量展示边界：缺失不当零，失效不显示旧百分比，累计不覆盖窗口占用。
package com.kongshang.maichat;

import static org.junit.Assert.*;
import org.json.JSONObject;
import org.junit.Test;

public final class AIAssistantContextUsageTest {
    @Test public void missingUsageNeverInventsAWindowOrPercent() throws Exception {
        AIAssistantContextUsage value = AIAssistantContextUsage.from(null, "s");
        assertEquals("服务端未提供上下文用量", value.text);
        assertEquals(-1, value.usedPercent);
        assertTrue(value.detail.contains("未提供"));
    }

    @Test public void providerZeroAndMissingCountsRemainDifferent() throws Exception {
        JSONObject usage = usage("s", 0, 65536, 100);
        AIAssistantContextUsage value = AIAssistantContextUsage.from(usage, "s");
        assertEquals("100% left (0 used / 65,536)", value.text);
        assertEquals(0, value.usedPercent);
        assertTrue(value.detail.contains("缓存命中：未提供"));
        usage.getJSONObject("latest_sampling").put("total_tokens", JSONObject.NULL)
            .put("total_tokens_source", "missing");
        assertEquals("服务端未提供上下文用量", AIAssistantContextUsage.from(usage, "s").text);
    }

    @Test public void unknownWindowAndDerivedTotalAreExplicit() throws Exception {
        JSONObject usage = usage("s", 133, 0, -1);
        usage.getJSONObject("latest_sampling").put("total_tokens_source", "derived");
        AIAssistantContextUsage value = AIAssistantContextUsage.from(usage, "s");
        assertEquals("推算 · 133 used / 窗口未知", value.text);
        assertEquals(-1, value.usedPercent);
        assertTrue(value.detail.contains("推算 133"));
    }

    @Test public void compactionKeepsCurrentSamplingSeparateFromAccumulatedConsumption() throws Exception {
        JSONObject usage = usage("s", 32111, 65536, 62).put("latest_operation", "compaction");
        usage.put("turn_token_usage", counts(80000)).put("thread_token_usage", counts(900000));
        AIAssistantContextUsage value = AIAssistantContextUsage.from(usage, "s");
        assertEquals("62% left (32,111 used / 65,536)", value.text);
        assertEquals(38, value.usedPercent);
        assertTrue(value.detail.contains("本轮累计：80,000"));
        assertTrue(value.detail.contains("会话累计（已记录部分）：900,000"));
        assertTrue(value.detail.contains("压缩请求"));
        usage.put("sampling_valid", false);
        value = AIAssistantContextUsage.from(usage, "s");
        assertEquals("上下文：等待新的服务端用量", value.text);
        assertEquals(-1, value.usedPercent);
        assertTrue(value.detail.contains("900,000"));
        assertFalse(value.detail.contains("最近主请求总量：32,111"));
    }

    @Test public void sessionSwitchRejectsOldOwnerAndSmallWindowHasNoFalseFullPercent() throws Exception {
        JSONObject usage = usage("old", 133, 4096, -1);
        assertEquals("服务端未提供上下文用量", AIAssistantContextUsage.from(usage, "new").text);
        AIAssistantContextUsage value = AIAssistantContextUsage.from(usage, "old");
        assertEquals("133 used / 4,096", value.text);
        assertEquals(-1, value.usedPercent);
    }

    @Test public void negativeFractionalAndUntrustedSourceNeverBecomeServerMeasurements() throws Exception {
        for (Object invalid : new Object[]{-1, 1.5, "133"}) {
            JSONObject usage = usage("s", 133, 65536, 100);
            usage.getJSONObject("latest_sampling").put("total_tokens", invalid);
            assertEquals("服务端未提供上下文用量", AIAssistantContextUsage.from(usage, "s").text);
        }
        JSONObject usage = usage("s", 133, 65536, 100);
        usage.getJSONObject("latest_sampling").put("total_tokens_source", "estimated");
        assertEquals("服务端未提供上下文用量", AIAssistantContextUsage.from(usage, "s").text);
    }

    @Test public void modelSwitchInvalidatesSamplingButRetainsRecordedConsumption() throws Exception {
        JSONObject usage = usage("s", 133, 65536, 100).put("model", "old-model");
        AIAssistantContextUsage value = AIAssistantContextUsage.from(usage, "s", "new-model");
        assertEquals("上下文：等待新的服务端用量", value.text);
        assertEquals(-1, value.usedPercent);
        assertTrue(value.detail.contains("会话累计（已记录部分）：133"));
    }

    @Test public void accumulatedProviderRecordsKeepTheirDerivedDisplayMarker() throws Exception {
        JSONObject usage = usage("s", 133, 0, -1)
            .put("turn_token_usage", counts(266).put("total_tokens_source", "derived"))
            .put("thread_token_usage", counts(532).put("total_tokens_source", "derived"));
        AIAssistantContextUsage value = AIAssistantContextUsage.from(usage, "s");
        assertEquals("133 used / 窗口未知", value.text);
        assertTrue(value.detail.contains("本轮累计：推算 266"));
        assertTrue(value.detail.contains("会话累计（已记录部分）：推算 532"));
    }

    private static JSONObject counts(long total) throws Exception {
        return new JSONObject().put("total_tokens", total).put("total_tokens_source", "provider");
    }
    private static JSONObject usage(String session, long total, long window, int left) throws Exception {
        return new JSONObject().put("version", 1).put("session_id", session).put("sampling_valid", true)
            .put("latest_sampling", counts(total).put("input_tokens", 111).put("input_tokens_source", "provider")
                .put("output_tokens", 22).put("output_tokens_source", "provider"))
            .put("window_tokens", window).put("window_source", window == 0 ? "missing" : "configuration")
            .put("remaining_percent", left < 0 ? JSONObject.NULL : left)
            .put("turn_token_usage", counts(total)).put("thread_token_usage", counts(total));
    }
}
