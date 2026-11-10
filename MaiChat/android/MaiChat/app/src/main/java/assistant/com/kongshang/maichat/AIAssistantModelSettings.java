// Account defaults for newly created sessions. Existing choices live in the shared session DB.
package com.kongshang.maichat;

import java.io.File;
import java.io.IOException;
import org.json.JSONObject;

final class AIAssistantModelSettings {
    static final String LOCAL_MODEL_ID = "builtin-qwen3.5-2b-q4_k_m";
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
        String cloud = saved.optString("cloudModel", local ? "deepseek-flash"
            : saved.optString("model", "deepseek-flash"));
        if (!supportedCloud(cloud)) cloud = "deepseek-flash";
        if (!local && supportedCloud(saved.optString("model"))) cloud = saved.optString("model");
        String policy = saved.optString("policy", "on-request");
        if (!supportedPolicy(policy)) policy = "on-request";
        String effort = saved.optString("reasoningEffort", "high");
        if (!supportedEffort(effort)) effort = "high";
        // Old local IDs migrate to the same pinned bundled model as iOS. File paths are never
        // restored from settings: the asset installer resolves the verified immutable file.
        return new AIAssistantModelSettings(local, cloud, policy, effort);
    }
    boolean isLocal() { return local; }
    String model() { return local ? "local:" + LOCAL_MODEL_ID : cloudModel; }
    String baseUrl() { return local ? "" : cloudModel.startsWith("deepseek-")
        ? "https://api.deepseek.com" : "https://open.bigmodel.cn/api/paas/v4"; }
    String wire() { return local ? "local" : cloudModel.startsWith("glm-")
        ? "chat_completions" : "responses"; }

    AIAssistantModelSettings selectSource(boolean local) {
        return this.local == local ? this : new AIAssistantModelSettings(local, cloudModel, policy, effort);
    }
    AIAssistantModelSettings withOptions(String model, String policy, String effort) throws Exception {
        if (!supportedPolicy(policy)) throw new IllegalArgumentException("请选择受支持的操作权限");
        if (!supportedEffort(effort)) throw new IllegalArgumentException("思考强度只支持 low、high、max");
        String cloud = local ? cloudModel : model.trim();
        if (!supportedCloud(cloud)) throw new IllegalArgumentException("请选择受支持的主模型");
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
            || "glm-5.3".equals(model) || "glm-5.3-flash".equals(model);
    }
    private static boolean supportedPolicy(String policy) {
        return "on-request".equals(policy) || "unless-trusted".equals(policy) || "never".equals(policy);
    }
    private static boolean supportedEffort(String effort) {
        return "low".equals(effort) || "high".equals(effort) || "max".equals(effort);
    }
}
