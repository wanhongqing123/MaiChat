// 文件作用：校验账号下发的阿里项目标识与短期 NLS Token；凭据仅保存在内存。
package com.kongshang.maichat;

import java.io.IOException;
import org.json.JSONObject;

public final class AliyunAsrCredential {
    public final String appKey, token;
    public final long expiresAt;
    private AliyunAsrCredential(String appKey, String token, long expiresAt) {
        this.appKey = appKey; this.token = token; this.expiresAt = expiresAt;
    }
    static AliyunAsrCredential parse(JSONObject value, long now) throws IOException {
        String appKey = value.optString("app_key", ""), token = value.optString("token", "");
        long expiry = value.optLong("expires_at", 0);
        if (!(value.opt("app_key") instanceof String) || !(value.opt("token") instanceof String)
            || !(value.opt("expires_at") instanceof Number)
            || ((Number) value.opt("expires_at")).doubleValue() != expiry
            || !appKey.matches("[!-~]{1,256}") || !token.matches("[!-~]{1,8192}") || expiry <= now + 90)
            throw new IOException("阿里云语音识别临时凭据无效");
        // 真实 NLS ExpireTime 可超过一天，不套用腾讯 STS 的时长上限。
        return new AliyunAsrCredential(appKey, token, expiry);
    }
}
