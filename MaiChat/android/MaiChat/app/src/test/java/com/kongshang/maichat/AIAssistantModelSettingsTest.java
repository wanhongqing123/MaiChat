// Protocol expectations mirror the current iOS settings and shared configure contract.
package com.kongshang.maichat;

import static org.junit.Assert.*;

import java.io.File;
import java.nio.file.Files;
import org.json.JSONObject;
import org.junit.Test;

public final class AIAssistantModelSettingsTest {
    @Test public void localSettingsRestoreTheirSourceInsteadOfBeingForcedToTheCloud() throws Exception {
        JSONObject saved = new JSONObject("{\"modelProvider\":\"llama\","
            + "\"model\":\"local:builtin-qwen3.5-2b-q4_k_m\",\"cloudModel\":\"glm-5.3\","
            + "\"policy\":\"never\",\"reasoningEffort\":\"max\"}");
        AIAssistantModelSettings settings = AIAssistantModelSettings.fromJson(saved);
        assertTrue("Persisted local source must survive initialization", settings.isLocal());
        assertEquals("local:builtin-qwen3.5-2b-q4_k_m", settings.model());
        assertEquals("glm-5.3", settings.cloudModel);
        assertEquals("never", settings.policy);
        assertEquals("max", settings.effort);
        assertEquals("llama", settings.toJson().getString("modelProvider"));
    }

    @Test public void cloudRoundTripAndOptionChangesKeepTheExplicitProviderAndCloudChoice() throws Exception {
        AIAssistantModelSettings cloud = AIAssistantModelSettings.fromJson(new JSONObject()
            .put("model", "glm-5.3-flash").put("policy", "unless-trusted")
            .put("reasoningEffort", "low"));
        AIAssistantModelSettings local = cloud.selectSource(true)
            .withOptions("local:builtin-qwen3.5-2b-q4_k_m", "never", "max");
        assertTrue(local.isLocal());
        assertEquals("glm-5.3-flash", local.cloudModel);
        AIAssistantModelSettings reopened = AIAssistantModelSettings.fromJson(local.toJson());
        assertTrue(reopened.isLocal());
        AIAssistantModelSettings restoredCloud = reopened.selectSource(false);
        assertFalse(restoredCloud.isLocal());
        assertEquals("glm-5.3-flash", restoredCloud.model());
        assertEquals("never", restoredCloud.policy);
        assertEquals("max", restoredCloud.effort);
        assertSame("Selecting the same source is a no-op", reopened, reopened.selectSource(true));
    }

    @Test public void localConfigurationUsesTheSharedPrimaryModelProtocolWithoutCloudCredentials() throws Exception {
        File weight = File.createTempFile("primary-settings-", ".gguf");
        try {
            Files.write(weight.toPath(), new byte[32]);
            JSONObject config = AIAssistantModelSettings.defaults().selectSource(true)
                .configuration(weight, "must-not-reach-local-config");
            assertEquals("llama", config.getString("modelProvider"));
            assertEquals("local", config.getString("wire"));
            assertEquals("local:builtin-qwen3.5-2b-q4_k_m", config.getString("model"));
            assertEquals(weight.getCanonicalPath(), config.getString("localModelPath"));
            assertEquals(32768, config.getInt("localContextSize"));
            assertEquals(0, config.getInt("localMaximumTokens"));
            assertEquals(2, config.getInt("localThreads"));
            assertEquals("", config.optString("apiKey"));
            assertEquals("", config.optString("baseUrl"));
            assertFalse(config.has("localProjectorPath"));
        } finally { weight.delete(); }
    }

    @Test public void anUnknownPersistedProviderDoesNotSilentlyFallBackToCloud() throws Exception {
        try {
            AIAssistantModelSettings.fromJson(new JSONObject().put("modelProvider", "unknown"));
            fail("Unknown provider must be rejected");
        } catch (IllegalArgumentException expected) { }
    }
}
