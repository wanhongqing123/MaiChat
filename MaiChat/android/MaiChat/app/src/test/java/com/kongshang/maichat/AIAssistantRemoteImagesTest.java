// Temporary remote image receipts must survive display projection without using owner-device paths.
package com.kongshang.maichat;

import static org.junit.Assert.*;
import java.lang.reflect.Method;
import java.io.File;
import java.nio.file.Files;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.Rule;
import org.junit.rules.TemporaryFolder;

public final class AIAssistantRemoteImagesTest {
    @Rule public final TemporaryFolder folder = new TemporaryFolder();

    static JSONObject owner() throws Exception {
        return new JSONObject().put("id", "session").put("device_slot", "windows")
            .put("login_session_id", "today-login").put("online", true);
    }
    static MaiCloudDrive.MediaInfo info(String name) {
        return "image.png".equals(name) ? new MaiCloudDrive.MediaInfo("image/png", "image", 20 * 1024 * 1024) : null;
    }
    private AIAssistantRemoteImages.Request request(String account, JSONObject owner, String message, JSONObject part) {
        return AIAssistantRemoteImages.Request.parse(account, owner, message, part, AIAssistantRemoteImagesTest::info);
    }
    static JSONObject part() throws Exception {
        return new JSONObject().put("id", "part").put("kind", "image")
            .put("path", "C:/owner-only/image.png")
            .put("remoteImage", new JSONObject().put("state", "ready").put("retention", "temporary")
                .put("attachment_id", "12345678-1234-1234-1234-123456789abc")
                .put("owner_login_session_id", "original-login").put("room_key", "windows:session")
                .put("name", "image.png").put("content_type", "image/png").put("media_type", "image")
                .put("size_bytes", 4).put("expires_at", System.currentTimeMillis() + 60_000));
    }

    @Test public void temporaryImageReceiptDoesNotGetAnUnavailablePlaceholderOrLocalPath() throws Exception {
        JSONObject part = part();
        JSONArray parts = new JSONArray().put(part);
        JSONArray messages = new JSONArray().put(new JSONObject().put("id", "message").put("parts", parts));
        Method sanitize = AIAssistantController.class.getDeclaredMethod("sanitizeRemoteMedia", JSONArray.class);
        sanitize.setAccessible(true);
        sanitize.invoke(null, messages);
        assertFalse("A remote workspace path must never resolve on this phone", part.has("path"));
        assertTrue(part.has("remoteImage"));
        assertEquals("The shared temporary-image receipt is previewable like iOS", 1, parts.length());
    }

    @Test public void ownerOnlyImageKeepsRealIdentityForExplicitPublicationWithoutInventedMetadata() throws Exception {
        JSONObject part = part(); part.remove("remoteImage");
        JSONArray parts = new JSONArray().put(part);
        Method sanitize = AIAssistantController.class.getDeclaredMethod("sanitizeRemoteMedia", JSONArray.class);
        sanitize.setAccessible(true);
        sanitize.invoke(null, new JSONArray().put(new JSONObject().put("parts", parts)));
        assertFalse(part.has("path"));
        assertEquals(1, parts.length());
        assertEquals("part", part.getString("id"));
        assertFalse(part.has("remoteImage"));
    }

    @Test public void downloadUsesOriginalPublicationLoginAndExactMessagePartIdentity() throws Exception {
        AIAssistantRemoteImages.Request request = request("account", owner(), "message", part());
        assertNotNull(request);
        JSONObject read = request.downloadRequest();
        assertEquals("account", read.getString("account_id"));
        assertEquals("original-login", read.getString("owner_login_session_id"));
        assertEquals("windows", read.getString("owner_slot"));
        assertEquals("session", read.getString("session_id"));
        assertEquals("message", read.getString("message_id"));
        assertEquals("part", read.getString("part_id"));
        assertFalse(read.has("path"));
        assertFalse(read.has("read_url"));
        assertEquals(request.signature, request("account", owner().put("login_session_id", "future-login"), "message", partCopy(request)).signature);
    }

    private JSONObject partCopy(AIAssistantRemoteImages.Request request) throws Exception {
        JSONObject part = part(); part.getJSONObject("remoteImage").put("expires_at", request.expiry); return part;
    }

    @Test public void malformedOrMismatchedMetadataNeverBecomesAnImageRequest() throws Exception {
        for (String field : new String[] {"attachment_id", "owner_login_session_id", "room_key", "name", "content_type",
                                         "media_type", "size_bytes", "expires_at"}) {
            JSONObject missing = part(); missing.getJSONObject("remoteImage").remove(field);
            assertNull(field, request("account", owner(), "message", missing));
        }
        JSONObject bad = part(); bad.getJSONObject("remoteImage").put("room_key", "mac:another-session");
        assertNull(request("account", owner(), "message", bad));
        bad = part(); bad.put("remotePartId", "other-part"); assertNull(request("account", owner(), "message", bad));
        bad = part(); bad.getJSONObject("remoteImage").put("size_bytes", "4"); assertNull(request("account", owner(), "message", bad));
        bad = part(); bad.getJSONObject("remoteImage").put("expires_at", 1.5); assertNull(request("account", owner(), "message", bad));
        bad = part(); bad.getJSONObject("remoteImage").put("name", "../image.png"); assertNull(request("account", owner(), "message", bad));
        assertNull(request("", owner(), "message", part()));
        assertNull(request("account", owner().put("device_slot", "stranger"), "message", part()));
    }

    @Test public void cacheNamespaceIncludesAccountOriginalOwnerSessionMessagePartAndAttachment() throws Exception {
        JSONObject part = part(); AIAssistantRemoteImages.Request baseline = request("account", owner(), "message", part);
        assertNotEquals(baseline.cacheKey, request("other", owner(), "message", part).cacheKey);
        assertNotEquals(baseline.cacheKey, request("account", owner(), "other-message", part).cacheKey);
        assertNotEquals(baseline.cacheKey, request("account", owner(), "message", new JSONObject(part.toString()).put("id", "other-part")).cacheKey);
        JSONObject changed = new JSONObject(part.toString()); changed.getJSONObject("remoteImage").put("owner_login_session_id", "other-original-login");
        assertNotEquals(baseline.cacheKey, request("account", owner(), "message", changed).cacheKey);
        changed = new JSONObject(part.toString()); changed.getJSONObject("remoteImage").put("attachment_id", "22345678-1234-1234-1234-123456789abc");
        assertNotEquals(baseline.cacheKey, request("account", owner(), "message", changed).cacheKey);
        changed = new JSONObject(part.toString()); changed.getJSONObject("remoteImage").put("room_key", "windows:other-session");
        assertNotEquals(baseline.cacheKey, request("account", owner().put("id", "other-session"), "message", changed).cacheKey);
    }

    @Test public void onlyCompleteHashVerifiedCopySurvivesTemporaryObjectExpiry() throws Exception {
        JSONObject part = part(); part.getJSONObject("remoteImage").put("expires_at", 1L);
        AIAssistantRemoteImages.Request request = request("account", owner(), "message", part);
        assertTrue(request.expired(System.currentTimeMillis()));
        AIAssistantRemoteImages.Cache cache = new AIAssistantRemoteImages.Cache(folder.newFolder());
        assertNull(cache.cached(request));
        File staged = cache.staging(request); Files.write(staged.toPath(), new byte[] {1, 2, 3, 4});
        assertNull("A complete but uncommitted staging file is not a cache receipt", cache.cached(request));
        File committed = cache.commit(staged, request);
        assertEquals(committed, cache.cached(request));
        Files.write(committed.toPath(), new byte[] {4, 3, 2, 1});
        assertNull("Same-size corruption cannot be reused", cache.cached(request));
        File repaired = cache.staging(request); Files.write(repaired.toPath(), new byte[] {1, 2, 3, 4});
        committed = cache.commit(repaired, request);
        assertEquals(committed, cache.cached(request));
        Files.delete(new File(committed.getParentFile(), "complete.json").toPath());
        assertNull("A naked image file is never a completed cache entry", cache.cached(request));
    }

    @Test public void truncatedStageCannotPublishAndDiscardCannotDeleteAnotherFolder() throws Exception {
        AIAssistantRemoteImages.Request request = request("account", owner(), "message", part());
        AIAssistantRemoteImages.Cache cache = new AIAssistantRemoteImages.Cache(folder.newFolder());
        File staged = cache.staging(request); Files.write(staged.toPath(), new byte[] {1, 2});
        try { cache.commit(staged, request); fail("Truncated image cannot commit"); }
        catch (java.io.IOException expected) { assertTrue(expected.getMessage().contains("不完整")); }
        cache.discard(staged); assertFalse(staged.exists()); assertNull(cache.cached(request));
        File unrelated = folder.newFile("image.png"); cache.discard(unrelated); assertTrue(unrelated.exists());
    }

    @Test public void trustedSandboxRootIsCanonicalBeforeRejectingUntrustedFileAliases() throws Exception {
        AIAssistantRemoteImages.Request request = request("account", owner(), "message", part());
        File base = folder.newFolder();
        AIAssistantRemoteImages.Cache cache = new AIAssistantRemoteImages.Cache(new File(base, "."));
        File staged = cache.staging(request); Files.write(staged.toPath(), new byte[] {1, 2, 3, 4});
        File committed = cache.commit(staged, request);
        assertEquals(committed.getCanonicalFile(), committed);
        assertEquals(committed, cache.cached(request));
    }

    @Test public void completeRetryableReceiptCanIdentifyLocalCacheWithoutAuthorizingDownload() throws Exception {
        JSONObject part = part(); AIAssistantRemoteImages.Request ready = request("account", owner(), "message", part);
        for (String state : new String[] {"pending", "failed"}) {
            JSONObject retryable = new JSONObject(part.toString()); retryable.getJSONObject("remoteImage").put("state", state);
            assertNull(request("account", owner(), "message", retryable));
            AIAssistantRemoteImages.Request cached = AIAssistantRemoteImages.Request.forCache("account", owner(), "message", retryable,
                AIAssistantRemoteImagesTest::info);
            assertNotNull(cached); assertEquals(ready.signature, cached.signature);
        }
    }

    @Test public void generationZeroKeepsInstalledCacheIdentityAndNewGenerationNeverLeaksIntoDownloadPayload() throws Exception {
        JSONObject part = part(); AIAssistantRemoteImages.Request legacy = request("account", owner(), "message", part);
        part.getJSONObject("remoteImage").put("generation", 0);
        assertEquals(legacy.signature, request("account", owner(), "message", part).signature);
        part.getJSONObject("remoteImage").put("generation", 1);
        AIAssistantRemoteImages.Request renewed = request("account", owner(), "message", part);
        assertNotEquals(legacy.signature, renewed.signature); assertEquals(legacy.cacheKey, renewed.cacheKey);
        assertFalse(renewed.downloadRequest().has("generation"));
        for (Object invalid : new Object[] {-1, "1", 1.5, 9_007_199_254_740_992L}) {
            part.getJSONObject("remoteImage").put("generation", invalid);
            assertNull(String.valueOf(invalid), request("account", owner(), "message", part));
        }
    }
}
