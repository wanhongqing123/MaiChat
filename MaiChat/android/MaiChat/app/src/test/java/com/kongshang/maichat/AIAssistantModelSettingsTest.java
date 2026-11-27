// Protocol expectations mirror the current iOS settings and shared configure contract.
package com.kongshang.maichat;

import static org.junit.Assert.*;

import java.io.File;
import java.nio.file.Files;
import org.json.JSONObject;
import org.junit.Test;

public final class AIAssistantModelSettingsTest {
    @Test public void solFiveEffortsUseOpenAiAndSurviveSettingsReopen() throws Exception {
        for (String effort : new String[]{"low", "medium", "high", "xhigh", "max"}) {
            AIAssistantModelSettings settings = AIAssistantModelSettings.defaults()
                .withOptions("gpt-6.1-sol", "on-request", effort);
            JSONObject config = settings.configuration(null, "synthetic-openai-key");
            assertEquals("gpt-6.1-sol", config.getString("model"));
            assertEquals("https://api.openai.com/v1", config.getString("baseUrl"));
            assertEquals("responses", config.getString("wire"));
            assertEquals("synthetic-openai-key", config.getString("apiKey"));
            AIAssistantModelSettings reopened = AIAssistantModelSettings.fromJson(settings.toJson());
            assertEquals("gpt-6.1-sol", reopened.model());
            assertEquals(effort, reopened.effort);
        }
        assertFalse(AIAssistantModelSettings.supportedCloud("gpt-6.1-sol-preview"));
        assertFalse(AIAssistantModelSettings.isOpenRouterModel("gpt-6.1-sol"));
        assertEquals("deepseek-flash", AIAssistantModelSettings.defaults().model());
    }

    @Test public void opusFiveEffortsUseOpenRouterAndSurviveSettingsReopen() throws Exception {
        for (String effort : new String[]{"low", "medium", "high", "xhigh", "max"}) {
            String model = "anthropic/claude-opus-5.5";
            AIAssistantModelSettings settings = AIAssistantModelSettings.defaults()
                .withOptions(model, "on-request", effort);
            JSONObject config = settings.configuration(null, "synthetic-openrouter-key");
            assertEquals(model, config.getString("model"));
            assertEquals("responses", config.getString("wire"));
            assertEquals("https://openrouter.ai/api/v1", config.getString("baseUrl"));
            assertEquals("synthetic-openrouter-key", config.getString("apiKey"));
            assertEquals(effort, config.getString("reasoningEffort"));
            AIAssistantModelSettings reopened = AIAssistantModelSettings.fromJson(settings.toJson());
            assertEquals(model, reopened.model());
            assertEquals(effort, reopened.effort);
        }
        assertFalse(AIAssistantModelSettings.supportedCloud("anthropic/claude-opus-5.5-preview"));
        assertFalse(AIAssistantModelSettings.supportedEffortForModel("deepseek-flash", "medium"));
    }
    @Test public void allFiveCloudChoicesUseResponsesAndTheirOwnEndpoint() throws Exception {
        for (String model : new String[]{"glm-5.4", "glm-5.3-flash", "deepseek-flash", "deepseek-v4-pro",
                "anthropic/claude-fable-5.2"}) {
            AIAssistantModelSettings settings = AIAssistantModelSettings.fromJson(
                new JSONObject().put("model", model));
            assertEquals("The exact session choice must survive", model, settings.model());
            assertEquals("responses", settings.wire());
            assertEquals(model.startsWith("glm-") ? "https://open.bigmodel.cn/api/v1"
                : model.equals("anthropic/claude-fable-5.2") ? "https://openrouter.ai/api/v1"
                : "https://api.deepseek.com", settings.baseUrl());
        }
    }

    @Test public void openRouterConfigurationKeepsRemoteSourceAndExactSessionChoice() throws Exception {
        for (String effort : new String[]{"low", "medium", "high", "xhigh", "max"}) {
            AIAssistantModelSettings settings = AIAssistantModelSettings.defaults()
                .withOptions("anthropic/claude-fable-5.2", "on-request", effort);
            JSONObject config = settings.configuration(null, "synthetic-openrouter-key");
            assertEquals("remote", config.getString("modelProvider"));
            assertEquals("anthropic/claude-fable-5.2", config.getString("model"));
            assertEquals("responses", config.getString("wire"));
            assertEquals("https://openrouter.ai/api/v1", config.getString("baseUrl"));
            assertEquals("synthetic-openrouter-key", config.getString("apiKey"));
            assertEquals(effort, config.getString("reasoningEffort"));
            assertEquals("anthropic/claude-fable-5.2",
                AIAssistantModelSettings.fromJson(settings.toJson()).model());
        }
        assertEquals("deepseek-flash", AIAssistantModelSettings.defaults().model());
    }

    @Test public void fableExtraEffortsSurviveRestoreAndOtherModelsNormalizeOnlyTheirProjection() throws Exception {
        for (String effort : new String[]{"medium", "xhigh"}) {
            JSONObject fable = new JSONObject().put("model", "anthropic/claude-fable-5.2")
                .put("reasoningEffort", effort);
            assertEquals(effort, AIAssistantModelSettings.fromJson(fable).effort);
            for (String model : new String[]{"glm-5.4", "glm-5.3-flash", "deepseek-flash", "deepseek-v4-pro"}) {
                JSONObject saved = new JSONObject().put("model", model).put("reasoningEffort", effort);
                assertEquals("high", AIAssistantModelSettings.fromJson(saved).effort);
                assertEquals("Reading settings must not rewrite persisted source", effort,
                    saved.getString("reasoningEffort"));
                try {
                    AIAssistantModelSettings.defaults().withOptions(model, "on-request", effort);
                    fail("An explicit unsupported effort must be rejected for its target model");
                } catch (IllegalArgumentException expected) { }
            }
        }
        for (String effort : new String[]{"low", "high", "max"})
            for (String model : new String[]{"glm-5.4", "deepseek-flash", "anthropic/claude-fable-5.2"})
                assertEquals(effort, AIAssistantModelSettings.fromJson(new JSONObject()
                    .put("model", model).put("reasoningEffort", effort)).effort);
    }

    @Test public void localSettingsRestoreTheirSourceInsteadOfBeingForcedToTheCloud() throws Exception {
        JSONObject saved = new JSONObject("{\"modelProvider\":\"llama\","
            + "\"model\":\"local:builtin-qwen3.5-2b-q4_k_m\",\"cloudModel\":\"glm-5.3\","
            + "\"policy\":\"never\",\"reasoningEffort\":\"max\"}");
        AIAssistantModelSettings settings = AIAssistantModelSettings.fromJson(saved);
        assertTrue("Persisted local source must survive initialization", settings.isLocal());
        assertEquals("local:builtin-qwen3.5-2b-q4_k_m", settings.model());
        assertEquals("glm-5.4", settings.cloudModel);
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
