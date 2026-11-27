// Account defaults for newly created sessions. Existing choices live in the shared session DB.
package com.kongshang.maichat;

import java.io.File;
import java.io.IOException;
import org.json.JSONObject;

final class AIAssistantModelSettings {
    static final String OPENAI_MODEL = "gpt-6.1-sol";
    static final String LOCAL_MODEL_ID = "builtin-qwen3.5-2b-q4_k_m";
    static final String OPENROUTER_MODEL = "anthropic/claude-fable-5.2";
    static final String OPUS_MODEL = "anthropic/claude-opus-5.5";
    static final String PARTICLE_FREE_MODEL = "particle-ai/particle-1:free";
    final String cloudModel, policy, effort;
    private final boolean local;

    private AIAssistantModelSettings(boolean local, String cloudModel, String policy, String effort) {
        this.local = local;
        this.cloudModel = cloudModel;
        this.policy = policy;
        this.effort = effort;
    }

    static AIAssistantModelSettings defaults() {
        return new AIAssistantModelSettings(false, "deepseek-flash", "on-request", "high");
    }

    static AIAssistantModelSettings fromJson(JSONObject saved) throws Exception {
        String provider = saved.optString("modelProvider", "remote");
        if (!provider.equals("remote") && !provider.equals("llama"))
            throw new IllegalArgumentException("主模型来源不受支持，原设置已保留");
        boolean local = provider.equals("llama");
        String cloud = restoredCloudModel(saved.optString("cloudModel", local ? "deepseek-flash"
            : saved.optString("model", "deepseek-flash")));
        if (!supportedCloud(cloud)) cloud = "deepseek-flash";
        if (!local && supportedCloud(restoredCloudModel(saved.optString("model"))))
            cloud = restoredCloudModel(saved.optString("model"));
        String policy = saved.optString("policy", "on-request");
        if (!supportedPolicy(policy)) policy = "on-request";
        String effort = saved.optString("reasoningEffort", "high");
        String selectedModel = local ? "local:" + LOCAL_MODEL_ID : cloud;
        effort = normalizeEffortForModel(selectedModel, effort);
        if (!supportedEffortForModel(selectedModel, effort)) effort = "high";
        // Old local IDs migrate to the same pinned bundled model as iOS. File paths are never
        // restored from settings: the asset installer resolves the verified immutable file.
        return new AIAssistantModelSettings(local, cloud, policy, effort);
    }
    boolean isLocal() { return local; }
    // 仅升级旧型号的设置投影；不修改传入 JSON、历史正文或 Flash 的独立型号。
    static String restoredCloudModel(String model) {
        return "glm-5.3".equals(model) ? "glm-5.4" : model;
    }
    String model() { return local ? "local:" + LOCAL_MODEL_ID : cloudModel; }
    String baseUrl() { return local ? "" : baseUrlForModel(cloudModel); }
    String wire() { return local ? "local" : "responses"; }

    // This is the host's endpoint projection only. Shared native routes own execution,
    // capabilities and request serialization for the same exact cloud model identifiers.
    static String baseUrlForModel(String model) {
        if (OPENAI_MODEL.equals(model)) return "https://api.openai.com/v1";
        if (isOpenRouterModel(model)) return "https://openrouter.ai/api/v1";
        if ("deepseek-flash".equals(model) || "deepseek-v4-pro".equals(model))
            return "https://api.deepseek.com";
        if ("glm-5.4".equals(model) || "glm-5.3-flash".equals(model))
            return "https://open.bigmodel.cn/api/v1";
        return "";
    }

    AIAssistantModelSettings selectSource(boolean local) {
        return this.local == local ? this : new AIAssistantModelSettings(local, cloudModel, policy,
            normalizeEffortForModel(local ? "local:" + LOCAL_MODEL_ID : cloudModel, effort));
    }
    AIAssistantModelSettings withOptions(String model, String policy, String effort) throws Exception {
        if (!supportedPolicy(policy)) throw new IllegalArgumentException("请选择受支持的操作权限");
        String cloud = local ? cloudModel : model.trim();
        if (!supportedCloud(cloud)) throw new IllegalArgumentException("请选择受支持的主模型");
        if (!supportedEffortForModel(local ? model() : cloud, effort))
            throw new IllegalArgumentException("所选模型不支持此思考强度");
        return new AIAssistantModelSettings(local, cloud, policy, effort);
    }
    JSONObject toJson() throws Exception {
        JSONObject result = new JSONObject().put("modelProvider", local ? "llama" : "remote")
            .put("cloudModel", cloudModel).put("model", model()).put("baseUrl", baseUrl())
            .put("wire", wire()).put("policy", policy).put("reasoningEffort", effort);
        if (local) result.put("localModelID", LOCAL_MODEL_ID);
        return result;
    }
    JSONObject configuration(File localFile, String cloudKey) throws Exception {
        JSONObject result = new JSONObject().put("modelProvider", local ? "llama" : "remote")
            .put("model", model()).put("wire", wire()).put("baseUrl", baseUrl())
            .put("apiKey", local ? "" : cloudKey).put("policy", policy).put("reasoningEffort", effort);
        if (local) {
            if (localFile == null || !localFile.isFile())
                throw new IOException("App 内置本地模型文件不可用，原配置已保留");
            result.put("localModelPath", localFile.getCanonicalPath())
                .put("localContextSize", 32768).put("localMaximumTokens", 0).put("localThreads", 2);
        }
        return result;
    }

    static boolean supportedCloud(String model) {
        return "deepseek-flash".equals(model) || "deepseek-v4-pro".equals(model)
            || OPENAI_MODEL.equals(model) || "glm-5.4".equals(model) || "glm-5.3-flash".equals(model) || isOpenRouterModel(model);
    }
    // Exact product identifiers use the current account's OpenRouter route;
    // aliases or the paid Particle endpoint must not borrow this credential.
    static boolean isOpenRouterModel(String model) {
        return OPENROUTER_MODEL.equals(model) || OPUS_MODEL.equals(model) || PARTICLE_FREE_MODEL.equals(model);
    }
    private static boolean supportedPolicy(String policy) {
        return "on-request".equals(policy) || "unless-trusted".equals(policy) || "never".equals(policy);
    }
    // Mirrors the shared catalog's exact model capability. Empty retains inheritance;
    // unsupported explicit input is rejected before any compatibility normalization.
    static boolean supportedEffortForModel(String model, String effort) {
        return "".equals(effort) || "low".equals(effort) || "high".equals(effort) || "max".equals(effort)
            || ((OPENROUTER_MODEL.equals(model) || OPUS_MODEL.equals(model) || OPENAI_MODEL.equals(model))
                && ("medium".equals(effort) || "xhigh".equals(effort)));
    }
    static String normalizeEffortForModel(String model, String effort) {
        return !OPENROUTER_MODEL.equals(model) && !OPUS_MODEL.equals(model) && !OPENAI_MODEL.equals(model)
            && ("medium".equals(effort) || "xhigh".equals(effort))
            ? "high" : effort;
    }
}
