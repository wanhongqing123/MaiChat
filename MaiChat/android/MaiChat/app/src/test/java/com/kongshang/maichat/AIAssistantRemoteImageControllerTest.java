// Real controller workers with owned image bytes only: no credentials, SDK, HTTP or model execution.
package com.kongshang.maichat;

import static org.junit.Assert.*;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import androidx.test.core.app.ApplicationProvider;
import java.io.File;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.time.Duration;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.Shadows;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.Implementation;
import org.robolectric.annotation.Implements;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, shadows = AIAssistantRemoteImageControllerTest.OwnedNative.class)
public final class AIAssistantRemoteImageControllerTest {
    private AIAssistantController controller;
    private Images images;
    private JSONObject image;

    @Before public void setup() throws Exception {
        Context context = ApplicationProvider.getApplicationContext();
        images = new Images();
        controller = new AIAssistantController(context, "http://owned.invalid", null, images);
        field("accountUserId").set(controller, "owned-" + UUID.randomUUID());
        image = AIAssistantRemoteImagesTest.part();
        page(AIAssistantRemoteImagesTest.owner(), image, 1);
    }
    @After public void close() { images.release.countDown(); controller.close(); }

    @Test public void coalescedReadUsesOriginalOwnerAndPublishesOnlyCompletedPrivateBytes() throws Exception {
        images.block = true;
        AIAssistantController.State clicked = controller.state;
        CompletableFuture<Reply> first = load(clicked, image), duplicate = load(clicked, image);
        assertTrue(images.started.await(3, TimeUnit.SECONDS));
        assertNull(controller.cachedRemoteImagePath(clicked, "message", image));
        images.release.countDown();
        Reply one = await(first), two = await(duplicate);
        assertEquals("", one.error); assertNotNull(one.file); assertEquals(one.file, two.file);
        assertEquals(1, images.reads.get());
        assertEquals("original-login", images.request.getString("owner_login_session_id"));
        assertEquals("message", images.request.getString("message_id"));
        assertEquals("part", images.request.getString("part_id"));
        assertArrayEquals(new byte[] {1, 2, 3, 4}, Files.readAllBytes(one.file.toPath()));
        assertEquals(one.file.getAbsolutePath(), controller.cachedRemoteImagePath(clicked, "message", image));
    }

    @Test public void leaveAndReturnRejectsTheOriginalReadAndReclaimsItsStage() throws Exception {
        images.block = true;
        AIAssistantController.State original = controller.state;
        CompletableFuture<Reply> old = load(original, image);
        assertTrue(images.started.await(3, TimeUnit.SECONDS));
        page(null, null, 2);
        page(AIAssistantRemoteImagesTest.owner(), image, 3);
        images.release.countDown();
        Reply refused = await(old);
        assertNull(refused.file); assertEquals("会话已切换", refused.error);
        assertNull(controller.cachedRemoteImagePath(original, "message", image));
        assertFalse("Canceled image bytes cannot survive in their uncommitted stage", images.target.exists());
    }

    @Test public void completedCopyRemainsReadableAfterExpiryAndCatalogRelogin() throws Exception {
        // Model a complete copy retained before this process started, with the server's fixed expiry now in the past.
        image.getJSONObject("remoteImage").put("state", "expired").put("expires_at", 1L);
        AIAssistantRemoteImages.Request request = AIAssistantRemoteImages.Request.parse(
            (String) field("accountUserId").get(controller), AIAssistantRemoteImagesTest.owner(), "message", image, images::mediaInfo);
        AIAssistantRemoteImages.Cache cache = (AIAssistantRemoteImages.Cache) field("remoteImageCache").get(controller);
        File staged = cache.staging(request); Files.write(staged.toPath(), new byte[] {1, 2, 3, 4});
        File retained = cache.commit(staged, request);
        page(AIAssistantRemoteImagesTest.owner(), image, 1);
        Reply ready = await(load(controller.state, image)); assertEquals(retained, ready.file);
        page(AIAssistantRemoteImagesTest.owner().put("login_session_id", "relogged"), image, 2);
        Reply cached = await(load(controller.state, image));
        assertEquals("", cached.error); assertEquals(ready.file, cached.file); assertEquals(0, images.reads.get());
    }

    @Test public void pendingFailedExpiredAndMismatchedReceiptsNeverStartTransport() throws Exception {
        image.getJSONObject("remoteImage").put("state", "pending"); page(AIAssistantRemoteImagesTest.owner(), image, 2);
        assertEquals("图片正在同步", await(load(controller.state, image)).error);
        image.getJSONObject("remoteImage").put("state", "failed"); page(AIAssistantRemoteImagesTest.owner(), image, 2);
        assertEquals("图片同步失败", await(load(controller.state, image)).error);
        image.getJSONObject("remoteImage").put("state", "expired").put("expires_at", 1L); page(AIAssistantRemoteImagesTest.owner(), image, 2);
        assertEquals("图片已过期", await(load(controller.state, image)).error);
        image.getJSONObject("remoteImage").put("state", "ready").put("expires_at", System.currentTimeMillis() + 60_000)
            .put("room_key", "windows:other"); page(AIAssistantRemoteImagesTest.owner(), image, 2);
        assertEquals("图片暂不可用", await(load(controller.state, image)).error);
        assertEquals(0, images.reads.get());
    }

    @Test public void changedMetadataCannotReuseAnOldCallbackOrSameSizeCorruptCache() throws Exception {
        Reply ready = await(load(controller.state, image)); assertNotNull(ready.file);
        Files.write(ready.file.toPath(), new byte[] {9, 9, 9, 9});
        Reply repaired = await(load(controller.state, image));
        assertNotNull(repaired.file); assertEquals(2, images.reads.get());
        assertArrayEquals(new byte[] {1, 2, 3, 4}, Files.readAllBytes(repaired.file.toPath()));
        JSONObject forged = new JSONObject(image.toString()); forged.getJSONObject("remoteImage").put("size_bytes", 5);
        assertEquals("会话已切换", await(load(controller.state, forged)).error);
        assertEquals(2, images.reads.get());
    }

    @Test public void receivedMainUpdatesAndQuestionProjectionAllRemoveOwnerWorkspacePaths() throws Exception {
        JSONObject envelope = new JSONObject().put("session_id", "session").put("from_slot", "windows")
            .put("from_login_session_id", "today-login");
        JSONObject payload = new JSONObject().put("ok", true)
            .put("messages", new JSONArray().put(message("message")))
            .put("messageUpdates", new JSONArray().put(message("a-old-message")))
            .put("questionMessages", new JSONArray().put(message("question-projection")));
        controller.acceptRemotePayload(envelope, payload);
        CompletableFuture<Void> done = new CompletableFuture<>();
        ((Handler) field("worker").get(controller)).post(() -> done.complete(null)); done.get(3, TimeUnit.SECONDS);
        for (String field : new String[] {"messages", "messageDisplayUpdates", "questionMessages"}) {
            JSONArray messages = controller.state.data.getJSONArray(field); assertEquals(field, 1, messages.length());
            JSONArray parts = messages.getJSONObject(0).getJSONArray("parts"); assertEquals(field, 1, parts.length());
            assertFalse(field, parts.getJSONObject(0).has("path"));
            assertTrue(field, parts.getJSONObject(0).has("remoteImage"));
        }
    }

    @Test public void identicalRemoteTupleFromAnotherAccountOrLoginGenerationCannotStartARead() throws Exception {
        AIAssistantController.State current = controller.state;
        for (String field : new String[] {"remoteImageAccountId", "remoteImageAccountGeneration"}) {
            JSONObject snapshot = new JSONObject(current.data.toString());
            snapshot.put(field, field.equals("remoteImageAccountId") ? "another-account"
                : current.data.optLong(field) + 1);
            AIAssistantController.State foreign = new AIAssistantController.State(snapshot, current.remoteSession,
                current.selected, current.baseUrl, current.model, current.policy, "", "", "", 0, false, true);
            Reply rejected = await(load(foreign, image));
            assertNull(rejected.file); assertEquals("会话已切换", rejected.error);
        }
        assertEquals(0, images.reads.get());
    }

    private JSONObject message(String id) throws Exception {
        return new JSONObject().put("id", id).put("role", "assistant")
            .put("parts", new JSONArray().put(new JSONObject(image.toString())));
    }

    private CompletableFuture<Reply> load(AIAssistantController.State owner, JSONObject part) {
        CompletableFuture<Reply> reply = new CompletableFuture<>();
        controller.loadRemoteImage(owner, "message", part, (file, failure) -> reply.complete(new Reply(file, failure)));
        return reply;
    }
    private Reply await(CompletableFuture<Reply> reply) throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (!reply.isDone() && System.nanoTime() < deadline) {
            Shadows.shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(25)); Thread.sleep(5);
        }
        return reply.get(1, TimeUnit.SECONDS);
    }
    private void page(JSONObject owner, JSONObject image, int generation) throws Exception {
        CompletableFuture<Void> ready = new CompletableFuture<>();
        ((Handler) field("worker").get(controller)).post(() -> {
            try {
                field("selectedRemote").set(controller, owner);
                field("remoteSelectionGeneration").setInt(controller, generation);
                JSONArray messages = new JSONArray();
                if (image != null) messages.put(new JSONObject().put("id", "message").put("role", "assistant")
                    .put("parts", new JSONArray().put(new JSONObject(image.toString()))));
                field("remoteData").set(controller, new JSONObject().put("messages", messages));
                Method publish = AIAssistantController.class.getDeclaredMethod("publishState"); publish.setAccessible(true); publish.invoke(controller);
                ready.complete(null);
            } catch (Exception failure) { ready.completeExceptionally(failure); }
        });
        ready.get(3, TimeUnit.SECONDS);
    }
    private static Field field(String name) throws Exception {
        Field value = AIAssistantController.class.getDeclaredField(name); value.setAccessible(true); return value;
    }
    private static final class Reply {
        final File file; final String error;
        Reply(File file, String error) { this.file = file; this.error = error; }
    }
    private static final class Images implements AIAssistantController.RemoteImageReads {
        final AtomicInteger reads = new AtomicInteger();
        final CountDownLatch started = new CountDownLatch(1), release = new CountDownLatch(1);
        volatile boolean block;
        volatile JSONObject request;
        volatile File target;
        @Override public MaiCloudDrive.MediaInfo mediaInfo(String name) { return AIAssistantRemoteImagesTest.info(name); }
        @Override public void download(JSONObject request, File target, AIAssistantPreparationJobs.Cancellation cancellation) throws Exception {
            assertNotEquals(Looper.getMainLooper(), Looper.myLooper());
            this.request = request; this.target = target; reads.incrementAndGet(); started.countDown();
            if (block) { cancellation.onCancel(release::countDown); assertTrue(release.await(3, TimeUnit.SECONDS)); }
            cancellation.check(); Files.write(target.toPath(), new byte[] {1, 2, 3, 4});
        }
    }
    @Implements(value = AIAssistantController.class, isInAndroidSdk = false)
    public static class OwnedNative {
        @Implementation protected void initialize() { /* The real worker starts, with no native library or account I/O. */ }
        @Implementation protected static void nativeDestroy(long handle) { fail("No native session was created"); }
    }
}
