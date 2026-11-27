// 文件作用：旗舰型号升级只迁移设置投影；Flash 和历史原文不受影响。
package com.kongshang.maichat;

import static org.junit.Assert.*;

import org.json.JSONObject;
import org.junit.Test;

public final class AIAssistantGlm54Test {
    @Test public void flagshipUsesExactResponsesModelAndExistingGlmCredential() throws Exception {
        AIAssistantModelSettings setting = AIAssistantModelSettings.defaults()
            .withOptions("glm-5.4", "on-request", "high");
        JSONObject configuration = setting.configuration(null, "owned-glm-fixture-key");
        assertEquals("glm-5.4", configuration.getString("model"));
        assertEquals("responses", configuration.getString("wire"));
        assertEquals("https://open.bigmodel.cn/api/v1", configuration.getString("baseUrl"));
        assertEquals("owned-glm-fixture-key", configuration.getString("apiKey"));
        assertTrue(AIAssistantMediaPolicy.supportsImages("glm-5.4"));
        assertFalse(AIAssistantModelSettings.supportedCloud("glm-5.3"));
        assertTrue(AIAssistantModelSettings.supportedCloud("glm-5.3-flash"));
    }

    @Test public void legacyPreferencesUpgradeTheirProjectionWithoutRewritingOriginalJson() throws Exception {
        JSONObject saved = new JSONObject().put("model", "glm-5.3")
            .put("policy", "never").put("reasoningEffort", "max");
        AIAssistantModelSettings restored = AIAssistantModelSettings.fromJson(saved);
        assertEquals("glm-5.4", restored.model());
        assertEquals("never", restored.policy);
        assertEquals("max", restored.effort);
        assertEquals("glm-5.3", saved.getString("model"));
        assertEquals("glm-5.4", restored.toJson().getString("model"));
    }

    @Test public void localSourceAndFlashKeepTheirOwnChoices() throws Exception {
        JSONObject saved = new JSONObject().put("modelProvider", "llama")
            .put("model", "local:" + AIAssistantModelSettings.LOCAL_MODEL_ID)
            .put("cloudModel", "glm-5.3");
        AIAssistantModelSettings local = AIAssistantModelSettings.fromJson(saved);
        assertTrue(local.isLocal());
        assertEquals("glm-5.4", local.cloudModel);
        AIAssistantModelSettings flash = AIAssistantModelSettings.fromJson(
            new JSONObject().put("model", "glm-5.3-flash"));
        assertEquals("glm-5.3-flash", flash.model());
        assertTrue(AIAssistantMediaPolicy.supportsImages(flash.model()));
        assertEquals("deepseek-flash", AIAssistantModelSettings.defaults().model());
    }
}
