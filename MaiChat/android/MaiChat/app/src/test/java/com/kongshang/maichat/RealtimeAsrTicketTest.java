// 文件作用：用固定 STS 凭据/时钟及独立 HMAC 结果验证 V2 本地签名和五字段合同。
// 所有凭据均为公开测试数据，不访问真实账号或腾讯服务。
package com.kongshang.maichat;

import static org.junit.Assert.*;
import java.io.IOException;
import java.net.URI;
import java.net.URLDecoder;
import java.util.Map;
import java.util.TreeMap;
import org.json.JSONObject;
import org.junit.Test;

public final class RealtimeAsrTicketTest {
    static final long NOW = 1_780_000_000L;
    private static final String VOICE = "c64385ee-3e5c-4fc5-bbfd-7c71addb35b0";
    private static final String TOKEN = "fixture-token+/=ABC123456";

    static JSONObject fixture(long now) throws Exception {
        return new JSONObject().put("app_id", "1259220000").put("secret_id", "AKIDTempFixture123")
            .put("secret_key", "fixture-secret-key-123456").put("token", TOKEN)
            .put("expires_at", now + 1800);
    }

    @Test public void signatureMatchesIndependentHmacAndTokenStaysOutOfUrl() throws Exception {
        RealtimeAsrCredential credential = RealtimeAsrCredential.parse(fixture(NOW), NOW);
        MaiChatAuthClient.RealtimeAsrTicket ticket = credential.sign(NOW, VOICE, 9);
        // 预期值由 Python hmac/hashlib 独立生成，覆盖参数顺序以及 Base64 的 /、+、= 编码。
        assertEquals("wss://asr.cloud.tencent.com/asr/v2/1259220000?"
            + "engine_model_type=16k_zh_en_2.0&expired=1780000300&needvad=1&nonce=9&result_mod=1"
            + "&secretid=AKIDTempFixture123&timestamp=1780000000&voice_format=16&voice_id=" + VOICE
            + "&signature=%2FP5sxVeA7ghE4Q%2Bull1FxrqhaSE%3D", ticket.url);
        assertEquals(TOKEN, ticket.token);
        assertFalse(ticket.url.contains(TOKEN));
        assertFalse(ticket.url.contains("token="));
        assertEquals(NOW + 300, ticket.expiresAt);
    }

    @Test public void eachRecordingGetsANewVoiceIdAndFreshNonceInputs() throws Exception {
        RealtimeAsrCredential credential = RealtimeAsrCredential.parse(fixture(NOW), NOW);
        MaiChatAuthClient.RealtimeAsrTicket first = credential.newTicket(NOW);
        MaiChatAuthClient.RealtimeAsrTicket second = credential.newTicket(NOW);
        assertNotEquals(first.voiceId, second.voiceId);
        assertNotEquals(first.url, second.url);
        String nextVoice = "00000000-0000-4000-8000-000000000001";
        assertTrue(credential.sign(NOW, nextVoice, 10).url.contains("nonce=10&"));
        assertTrue(credential.sign(NOW, nextVoice, 10).url.contains("voice_id=" + nextVoice + "&"));
    }

    @Test public void opaqueTemporaryIdUsesRawCanonicalValuesButEncodedQueryBoundaries() throws Exception {
        String opaqueId = "AKID+/=one&two?three#four";
        RealtimeAsrCredential credential = RealtimeAsrCredential.parse(
            fixture(NOW).put("secret_id", opaqueId), NOW);
        MaiChatAuthClient.RealtimeAsrTicket ticket = credential.sign(NOW, VOICE, 9);
        assertTrue(ticket.url.contains("secretid=AKID%2B%2F%3Done%26two%3Fthree%23four&"));
        URI endpoint = new URI(ticket.url);
        assertNull(endpoint.getRawFragment());
        TreeMap<String, String> decoded = new TreeMap<>();
        for (String field : endpoint.getRawQuery().split("&")) {
            String[] pair = field.split("=", 2);
            assertEquals(2, pair.length);
            assertNull(decoded.put(pair[0], URLDecoder.decode(pair[1], "UTF-8")));
        }
        assertEquals(10, decoded.size());
        assertEquals(opaqueId, decoded.get("secretid"));
        // 独立 Python HMAC fixture 对原始不透明 ID 签名，不能对 percent 编码后的值签名。
        assertEquals("sp4A5MkguMyBjd40ccICXr/C/Vc=", decoded.remove("signature"));
        StringBuilder canonical = new StringBuilder(endpoint.getHost()).append(endpoint.getPath()).append('?');
        for (Map.Entry<String, String> field : decoded.entrySet()) {
            if (canonical.charAt(canonical.length() - 1) != '?') canonical.append('&');
            canonical.append(field.getKey()).append('=').append(field.getValue());
        }
        assertEquals("asr.cloud.tencent.com/asr/v2/1259220000?engine_model_type=16k_zh_en_2.0"
            + "&expired=1780000300&needvad=1&nonce=9&result_mod=1&secretid=" + opaqueId
            + "&timestamp=1780000000&voice_format=16&voice_id=" + VOICE, canonical.toString());
    }

    @Test public void opaqueTemporaryIdAllowsTheDocumentedLengthAndRejectsControls() throws Exception {
        String maximum = new String(new char[1024]).replace('\0', '+');
        RealtimeAsrCredential credential = RealtimeAsrCredential.parse(
            fixture(NOW).put("secret_id", maximum), NOW);
        assertTrue(credential.sign(NOW, VOICE, 9).url.length() < 4096);
        reject(fixture(NOW).put("secret_id", maximum + "A"));
        reject(fixture(NOW).put("secret_id", "opaque id"));
        reject(fixture(NOW).put("secret_id", "opaque\r\nid"));
    }

    @Test public void signedUrlCannotOutliveStsAndRefreshMarginIsRequired() throws Exception {
        RealtimeAsrCredential credential = RealtimeAsrCredential.parse(
            fixture(NOW).put("expires_at", NOW + 180), NOW);
        assertEquals(NOW + 180, credential.sign(NOW, VOICE, 9).expiresAt);
        try { credential.sign(NOW + 90, VOICE, 9); fail("Refresh margin was ignored"); }
        catch (IOException expected) { }
    }

    @Test public void allFiveFieldsAndTheirTypesAreRequired() throws Exception {
        for (String field : new String[] {"app_id", "secret_id", "secret_key", "token", "expires_at"}) {
            JSONObject value = fixture(NOW);
            value.remove(field);
            reject(value);
        }
        reject(fixture(NOW).put("app_id", 1259220000));
        reject(fixture(NOW).put("expires_at", Long.toString(NOW + 1800)));
        reject(fixture(NOW).put("expires_at", NOW + 1800.5));
        reject(fixture(NOW).put("expires_at", NOW + 90));
        reject(fixture(NOW).put("expires_at", NOW + 1861));
        reject(fixture(NOW).put("token", "fixture-token\r\nInjected: bad"));
        reject(fixture(NOW).put("secret_key", "fixture-secret\nkey"));
        reject(fixture(NOW).put("secret_id", "bad-id"));
        reject(fixture(NOW).put("app_id", "../host"));
    }

    private void reject(JSONObject value) throws Exception {
        try { RealtimeAsrCredential.parse(value, NOW); fail("Invalid STS credential accepted"); }
        catch (IOException expected) { }
    }
}
