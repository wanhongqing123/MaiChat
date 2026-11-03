// 文件作用：无网验证阿里短期凭据合同与真实 ExpireTime，不使用账号或密钥。
package com.kongshang.maichat;

import java.lang.reflect.InvocationTargetException;
import org.json.JSONObject;
import org.junit.Test;
import static org.junit.Assert.*;

public class AliyunAsrCredentialTest {
    static final long NOW = 1800000000L;
    static JSONObject fixture(long expiry) throws Exception {
        return new JSONObject().put("app_key", "fixture-app-key").put("token", "fixture-nls-token")
            .put("expires_at", expiry);
    }
    static Object parse(JSONObject value) throws Exception {
        Class<?> type;
        try { type = Class.forName("com.kongshang.maichat.AliyunAsrCredential"); }
        catch (ClassNotFoundException missing) { throw new AssertionError("Aliyun needs its own short-lived credential contract", missing); }
        return type.getDeclaredMethod("parse", JSONObject.class, long.class).invoke(null, value, NOW);
    }
    @Test public void acceptsThirtySixHourTokenUsingTheServerExpiry() throws Exception {
        Object value = parse(fixture(NOW + 36 * 3600));
        assertEquals(NOW + 36 * 3600, value.getClass().getField("expiresAt").getLong(value));
    }
    @Test public void rejectsMissingWrongTypeExpiredAndHeaderInjection() throws Exception {
        for (String key : new String[] {"app_key", "token", "expires_at"}) {
            JSONObject value = fixture(NOW + 3600); value.remove(key); rejects(value);
        }
        rejects(fixture(NOW + 90));
        rejects(fixture(NOW + 3600).put("expires_at", "1800003600"));
        rejects(fixture(NOW + 3600).put("expires_at", NOW + 3600.5));
        rejects(fixture(NOW + 3600).put("token", "fixture\r\nInjected:yes"));
        rejects(fixture(NOW + 3600).put("app_key", "bad key"));
    }
    private void rejects(JSONObject value) throws Exception {
        try { parse(value); fail("Invalid Aliyun credential accepted"); }
        catch (InvocationTargetException expected) { assertTrue(expected.getCause() instanceof java.io.IOException); }
    }
}
