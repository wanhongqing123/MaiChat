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
import org.robolectric.util.ReflectionHelpers;
import org.robolectric.util.ReflectionHelpers.ClassParameter;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.Implementation;
import org.robolectric.annotation.Implements;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, shadows = {AIAssistantRemoteImageControllerTest.OwnedNative.class,
    MaiSessionAttachmentsTemporaryImageTest.OwnedAuth.class,
    MaiSessionAttachmentsTemporaryImageTest.OwnedTrust.class,
    MaiSessionAttachmentsTemporaryImageTest.OwnedNative.class})
public final class AIAssistantRemoteImageControllerTest {
    private AIAssistantController controller;
    private Images images;
    private Commands commands;
    private JSONObject image;

    @Before public void setup() throws Exception {
        Context context = ApplicationProvider.getApplicationContext();
        MaiSessionAttachmentsTemporaryImageTest.OwnedAuth.account = "public-" + UUID.randomUUID();
        MaiSessionAttachmentsTemporaryImageTest.OwnedAuth.generation = 7;
        MaiSessionAttachmentsTemporaryImageTest.OwnedAuth.accessCalls = 0;
        MaiSessionAttachmentsTemporaryImageTest.OwnedAuth.duringAccess = null;
        MaiSessionAttachmentsTemporaryImageTest.OwnedAuth.client = ReflectionHelpers.callConstructor(
            MaiChatAuthClient.class, ClassParameter.from(Context.class, context));
        MaiSessionAttachmentsTemporaryImageTest.OwnedNative.reset();
        images = new Images();
        commands = new Commands();
        controller = new AIAssistantController(context, "http://owned.invalid", null, images, commands);
        field("accountUserId").set(controller, "owned-" + UUID.randomUUID());
        image = AIAssistantRemoteImagesTest.part();
        page(AIAssistantRemoteImagesTest.owner(), image, 1);
    }
    @After public void close() {
        images.release.countDown(); controller.close();
        MaiSessionAttachmentsTemporaryImageTest.OwnedAuth.client = null;
        MaiSessionAttachmentsTemporaryImageTest.OwnedNative.reset();
    }

    @Test public void defaultImageReadUsesAuthAccountWithoutChangingImStorageOwner() throws Exception {
        String imOwner = (String) field("accountUserId").get(controller);
        controller.close();
        controller = new AIAssistantController(ApplicationProvider.getApplicationContext(), "http://owned.invalid", null, null, commands);
        field("accountUserId").set(controller, imOwner);
        MaiSessionAttachmentsTemporaryImageTest.OwnedNative.fileBytes = new byte[] {1, 2, 3, 4};
        page(AIAssistantRemoteImagesTest.owner(), image, 1);
        Reply read = await(load(controller.state, image));
        assertEquals("", read.error); assertNotNull(read.file);
        assertNotEquals(imOwner, MaiSessionAttachmentsTemporaryImageTest.OwnedAuth.account);
        assertEquals(imOwner, field("accountUserId").get(controller));
        assertEquals(1, MaiSessionAttachmentsTemporaryImageTest.OwnedNative.downloads);
        AIAssistantRemoteImages.Request expected = AIAssistantRemoteImages.Request.parse(
            MaiSessionAttachmentsTemporaryImageTest.OwnedAuth.account, controller.state.remoteSession, "message", image,
            AIAssistantRemoteImagesTest::info);
        AIAssistantRemoteImages.Cache cache = (AIAssistantRemoteImages.Cache) field("remoteImageCache").get(controller);
        assertEquals(read.file, cache.cached(expected));
    }

    @Test public void completedCacheRejectsAnotherAuthAccountEvenWithSameImOwner() throws Exception {
        AIAssistantController.State clicked = controller.state;
        Reply read = await(load(clicked, image)); assertNotNull(read.file);
        MaiSessionAttachmentsTemporaryImageTest.OwnedAuth.account = "another-public-account";
        assertNull(controller.cachedRemoteImagePath(clicked, "message", image));
        Reply refused = await(load(clicked, image));
        assertNull(refused.file); assertEquals("会话已切换", refused.error); assertEquals(1, images.reads.get());
    }

    @Test public void completedCacheRejectsSameAuthAccountReloginEpoch() throws Exception {
        AIAssistantController.State clicked = controller.state;
        Reply read = await(load(clicked, image)); assertNotNull(read.file);
        MaiSessionAttachmentsTemporaryImageTest.OwnedAuth.generation++;
        assertNull(controller.cachedRemoteImagePath(clicked, "message", image));
        Reply refused = await(load(clicked, image));
        assertNull(refused.file); assertEquals("会话已切换", refused.error); assertEquals(1, images.reads.get());
    }

    @Test public void inflightReadRejectsSameAuthAccountReloginBeforeCacheCommit() throws Exception {
        images.block = true;
        CompletableFuture<Reply> pending = load(controller.state, image);
        assertTrue(images.started.await(3, TimeUnit.SECONDS));
        MaiSessionAttachmentsTemporaryImageTest.OwnedAuth.generation++;
        images.release.countDown(); Reply refused = await(pending);
        assertNull(refused.file); assertEquals("会话已切换", refused.error);
        assertFalse(images.target.exists());
    }

    @Test public void inflightReadRejectsAuthAccountChangeBeforeCacheCommit() throws Exception {
        images.block = true;
        CompletableFuture<Reply> pending = load(controller.state, image);
        assertTrue(images.started.await(3, TimeUnit.SECONDS));
        MaiSessionAttachmentsTemporaryImageTest.OwnedAuth.account = "another-public-account";
        images.release.countDown(); Reply refused = await(pending);
        assertNull(refused.file); assertEquals("会话已切换", refused.error);
        assertFalse(images.target.exists());
    }

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
            MaiSessionAttachmentsTemporaryImageTest.OwnedAuth.account, AIAssistantRemoteImagesTest.owner(), "message", image, images::mediaInfo);
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
        for (String field : new String[] {"remoteImageAccountId", "remoteImageAuthAccountId", "remoteImageAccountGeneration"}) {
            JSONObject snapshot = new JSONObject(current.data.toString());
            snapshot.put(field, !field.equals("remoteImageAccountGeneration") ? "another-account"
                : current.data.optLong(field) + 1);
            AIAssistantController.State foreign = new AIAssistantController.State(snapshot, current.remoteSession,
                current.selected, current.baseUrl, current.model, current.policy, "", "", "", 0, false, true);
            Reply rejected = await(load(foreign, image));
            assertNull(rejected.file); assertEquals("会话已切换", rejected.error);
        }
        assertEquals(0, images.reads.get());
    }

    @Test public void explicitPublicationUsesCompleteExpiredCopyEvenWhileOwnerIsOffline() throws Exception {
        image.getJSONObject("remoteImage").put("state", "expired").put("expires_at", 1L);
        AIAssistantRemoteImages.Request request = AIAssistantRemoteImages.Request.parse(
            MaiSessionAttachmentsTemporaryImageTest.OwnedAuth.account, AIAssistantRemoteImagesTest.owner(), "message", image, images::mediaInfo);
        AIAssistantRemoteImages.Cache cache = (AIAssistantRemoteImages.Cache) field("remoteImageCache").get(controller);
        File staged = cache.staging(request); Files.write(staged.toPath(), new byte[] {1, 2, 3, 4});
        File retained = cache.commit(staged, request);
        page(AIAssistantRemoteImagesTest.owner().put("online", false), image, 2);
        CompletableFuture<Reply> completed = new CompletableFuture<>();
        try {
            Method prepare = AIAssistantController.class.getDeclaredMethod("requestRemoteImagePublication",
                AIAssistantController.State.class, String.class, JSONObject.class, java.util.function.BiConsumer.class);
            prepare.setAccessible(true);
            java.util.function.BiConsumer<File, String> callback = (file, failure) -> completed.complete(new Reply(file, failure));
            prepare.invoke(controller, controller.state, "message", image, callback);
        } catch (NoSuchMethodException missing) {
            throw new AssertionError("Explicit single-image publication entry must check complete local copies before contacting the owner", missing);
        }
        Reply shown = await(completed);
        assertEquals("", shown.error); assertEquals(retained, shown.file); assertEquals(0, images.reads.get());
        assertEquals(0, commands.sent.get());
    }

    @Test public void automaticCacheProbeKeepsCompletePendingAndFailedCopiesVisibleOfflineWithoutPublication() throws Exception {
        AIAssistantRemoteImages.Request request = AIAssistantRemoteImages.Request.parse(
            MaiSessionAttachmentsTemporaryImageTest.OwnedAuth.account, AIAssistantRemoteImagesTest.owner(), "message", image, images::mediaInfo);
        AIAssistantRemoteImages.Cache cache = (AIAssistantRemoteImages.Cache) field("remoteImageCache").get(controller);
        File staged = cache.staging(request); Files.write(staged.toPath(), new byte[] {1, 2, 3, 4});
        File retained = cache.commit(staged, request);
        for (String status : new String[] {"pending", "failed"}) {
            image.getJSONObject("remoteImage").put("state", status);
            page(AIAssistantRemoteImagesTest.owner().put("online", false), image, 1);
            Reply shown = await(load(controller.state, image));
            assertEquals(status, "", shown.error); assertEquals(status, retained, shown.file);
        }
        assertEquals(0, images.reads.get()); assertEquals(0, commands.sent.get());
    }

    @Test public void explicitLegacyImageCoalescesOneExactOwnerCommandThenDownloadsItsReadyReceipt() throws Exception {
        image.remove("remoteImage"); page(AIAssistantRemoteImagesTest.owner(), image, 1);
        AIAssistantController.State clicked = controller.state;
        CompletableFuture<Reply> first = prepare(clicked, image), duplicate = prepare(clicked, image);
        waitForCommands(1);
        assertEquals(4, commands.request.length());
        assertEquals("prepare_image", commands.request.getString("action"));
        assertEquals("message", commands.request.getString("messageId")); assertEquals("part", commands.request.getString("partId"));
        assertFalse(commands.request.has("path")); assertFalse(commands.request.has("account_id")); assertFalse(commands.request.has("url"));
        assertEquals("today-login", commands.owner.getString("login_session_id"));
        assertEquals(0, images.reads.get()); assertFalse(first.isDone());
        JSONObject metadata = ready(0);
        commands.reply(envelope(), success(metadata));
        Reply one = await(first), two = await(duplicate);
        assertEquals("", one.error); assertNotNull(one.file); assertEquals(one.file, two.file);
        assertEquals(1, commands.sent.get()); assertEquals(1, images.reads.get());
        assertEquals(metadata.toString(), visiblePart().getJSONObject("remoteImage").toString());
        assertFalse(visiblePart().has("path")); assertEquals(1, controller.state.data.getInt("remoteSelectionGeneration"));
    }

    @Test public void publicationRejectsWrongOwnerAndRequestTupleWithoutConsumingTheValidReply() throws Exception {
        image.remove("remoteImage"); page(AIAssistantRemoteImagesTest.owner(), image, 1);
        CompletableFuture<Reply> pending = prepare(controller.state, image); waitForCommands(1);
        commands.reply(envelope().put("from_login_session_id", "other-login"), success(ready(0)));
        for (String field : new String[] {"requestId", "messageId", "partId"})
            commands.reply(envelope(), success(ready(0)).put(field, "another"));
        drainWorker(); assertFalse(pending.isDone()); assertEquals(0, images.reads.get());
        commands.reply(envelope(), success(ready(0))); assertNotNull(await(pending).file);
    }

    @Test public void publicationOfflineAndOldClientUnsupportedAreExplicitAndNeverPoll() throws Exception {
        image.remove("remoteImage"); page(AIAssistantRemoteImagesTest.owner().put("online", false), image, 1);
        assertEquals("所属设备离线，请上线后重新加载图片", await(prepare(controller.state, image)).error);
        assertEquals(0, commands.sent.get());
        page(AIAssistantRemoteImagesTest.owner(), image, 1);
        CompletableFuture<Reply> pending = prepare(controller.state, image); waitForCommands(1);
        commands.reply(envelope(), new JSONObject().put("ok", false).put("error", "不支持的远端操作"));
        assertTrue(await(pending).error.contains("版本不支持"));
        drainWorker(); assertEquals(1, commands.sent.get()); assertEquals(0, images.reads.get());
        CompletableFuture<Reply> retry = prepare(controller.state, image); waitForCommands(2);
        commands.reply(envelope(), success(ready(0))); assertNotNull(await(retry).file);
    }

    @Test public void publicationLeaveAndReturnDiscardsLateAcknowledgmentWithoutChangingNewState() throws Exception {
        image.remove("remoteImage"); page(AIAssistantRemoteImagesTest.owner(), image, 1);
        CompletableFuture<Reply> pending = prepare(controller.state, image); waitForCommands(1);
        page(null, null, 2); page(AIAssistantRemoteImagesTest.owner(), image, 3);
        assertEquals("会话已切换", await(pending).error);
        commands.reply(envelope(), success(ready(0))); drainWorker();
        assertFalse(visiblePart().has("remoteImage")); assertEquals(0, images.reads.get());
    }

    @Test public void publicationAcceptsOnlySameOrNextGenerationAndHandlesItsPendingBroadcastBeforeAck() throws Exception {
        image.getJSONObject("remoteImage").put("state", "expired").put("expires_at", 1L);
        page(AIAssistantRemoteImagesTest.owner(), image, 1);
        CompletableFuture<Reply> pending = prepare(controller.state, image); waitForCommands(1);
        commands.reply(envelope(), success(ready(2))); drainWorker();
        assertFalse(pending.isDone()); assertEquals(0, images.reads.get());
        JSONObject progress = new JSONObject(image.toString()).put("remoteImage", new JSONObject()
            .put("state", "pending").put("retention", "temporary").put("generation", 1));
        page(AIAssistantRemoteImagesTest.owner(), progress, 1);
        commands.reply(envelope(), success(ready(1)));
        assertNotNull(await(pending).file); assertEquals(1, images.reads.get());
        assertEquals(1, visiblePart().getJSONObject("remoteImage").getInt("generation"));
    }

    @Test public void publicationCannotOverwriteAnotherNewerReadyReceipt() throws Exception {
        image.getJSONObject("remoteImage").put("state", "expired").put("expires_at", 1L);
        page(AIAssistantRemoteImagesTest.owner(), image, 1);
        CompletableFuture<Reply> pending = prepare(controller.state, image); waitForCommands(1);
        JSONObject newer = ready(2); page(AIAssistantRemoteImagesTest.owner(), new JSONObject(image.toString()).put("remoteImage", newer), 1);
        commands.reply(envelope(), success(ready(1)));
        Reply refused = await(pending); assertNull(refused.file); assertTrue(refused.error.contains("图片已更新"));
        assertEquals(newer.toString(), visiblePart().getJSONObject("remoteImage").toString()); assertEquals(0, images.reads.get());
    }

    @Test public void publicationNewMetadataDuringDownloadRejectsOldBytesAndDiscardsStage() throws Exception {
        images.block = true; image.remove("remoteImage"); page(AIAssistantRemoteImagesTest.owner(), image, 1);
        CompletableFuture<Reply> pending = prepare(controller.state, image); waitForCommands(1);
        commands.reply(envelope(), success(ready(0)));
        assertTrue(images.started.await(3, TimeUnit.SECONDS));
        JSONObject newer = ready(1); page(AIAssistantRemoteImagesTest.owner(), new JSONObject(image.toString()).put("remoteImage", newer), 1);
        images.release.countDown(); Reply refused = await(pending);
        assertNull(refused.file); assertEquals("会话已切换", refused.error); assertFalse(images.target.exists());
        assertEquals(newer.toString(), visiblePart().getJSONObject("remoteImage").toString());
    }

    private CompletableFuture<Reply> prepare(AIAssistantController.State owner, JSONObject part) {
        CompletableFuture<Reply> reply = new CompletableFuture<>();
        controller.requestRemoteImagePublication(owner, "message", part, (file, failure) -> reply.complete(new Reply(file, failure)));
        return reply;
    }
    private JSONObject visiblePart() throws Exception { return controller.state.data.getJSONArray("messages").getJSONObject(0).getJSONArray("parts").getJSONObject(0); }
    private JSONObject ready(int generation) throws Exception {
        JSONObject metadata = AIAssistantRemoteImagesTest.part().getJSONObject("remoteImage");
        metadata.put("generation", generation); return metadata;
    }
    private JSONObject success(JSONObject metadata) throws Exception {
        return new JSONObject().put("ok", true).put("requestId", commands.request.getString("requestId"))
            .put("messageId", "message").put("partId", "part").put("remoteImage", metadata);
    }
    private JSONObject envelope() throws Exception {
        return new JSONObject().put("session_id", "session").put("from_slot", "windows").put("from_login_session_id", "today-login");
    }
    private void waitForCommands(int count) throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(4);
        while (commands.sent.get() < count && System.nanoTime() < deadline) {
            Shadows.shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(25)); Thread.sleep(5);
        }
        assertEquals(count, commands.sent.get()); drainWorker();
    }
    private void drainWorker() throws Exception {
        CompletableFuture<Void> done = new CompletableFuture<>();
        ((Handler) field("worker").get(controller)).post(() -> done.complete(null)); done.get(3, TimeUnit.SECONDS);
        Shadows.shadowOf(Looper.getMainLooper()).idle();
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
                field("remoteSessions").set(controller, owner == null ? new JSONArray() : new JSONArray().put(owner));
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
    private static final class Commands implements AIAssistantController.ImagePublicationCommands {
        final AtomicInteger sent = new AtomicInteger();
        volatile JSONObject owner, request;
        volatile String command;
        volatile java.util.function.BiConsumer<JSONObject, JSONObject> received;
        @Override public String send(JSONObject owner, JSONObject request, java.util.function.BiConsumer<String, String> sent,
                                     java.util.function.BiConsumer<JSONObject, JSONObject> received) {
            assertNotEquals(Looper.getMainLooper(), Looper.myLooper());
            this.owner = owner; this.request = request; this.received = received; command = "prepare-" + this.sent.incrementAndGet();
            sent.accept(command, ""); return command;
        }
        void reply(JSONObject envelope, JSONObject result) throws Exception {
            received.accept(envelope, new JSONObject().put("type", "command_result").put("command_id", command).put("result", result));
        }
    }
    @Implements(value = AIAssistantController.class, isInAndroidSdk = false)
    public static class OwnedNative {
        @Implementation protected void initialize() { /* The real worker starts, with no native library or account I/O. */ }
        @Implementation protected static void nativeDestroy(long handle) { fail("No native session was created"); }
    }
}
