// Real controller workers and the ordinary history transport seam; no model,
// JNI, SDK, credentials, user database or device operations.
package com.kongshang.maichat;

import static org.junit.Assert.*;
import android.os.Handler;
import android.os.Looper;
import androidx.test.core.app.ApplicationProvider;
import java.lang.reflect.Field;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BiConsumer;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.After;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.Shadows;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.Implementation;
import org.robolectric.annotation.Implements;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, shadows = AIAssistantRemoteSessionSyncTest.OwnedNative.class)
public final class AIAssistantRemoteSessionSyncTest {
    private AIAssistantController controller;

    @After public void close() { if (controller != null) controller.close(); }

    @Test public void completedBroadcastSurvivesDelayedInitialHistory() throws Exception {
        Reads reads = new Reads();
        controller = new AIAssistantController(ApplicationProvider.getApplicationContext(),
            "http://owned.invalid", reads);
        JSONObject owner = new JSONObject().put("id", "owned-session")
            .put("device_slot", "windows").put("login_session_id", "owned-login")
            .put("online", true).put("busy", true).put("turnId", "msg_002");
        controller.selectRemote(owner, accepted -> {});
        assertTrue(reads.started.await(3, TimeUnit.SECONDS));
        JSONObject envelope = new JSONObject().put("session_id", "owned-session")
            .put("from_slot", "windows").put("from_login_session_id", "owned-login");

        // The request captured an earlier running page, but the final live
        // publication arrives first through the ordinary receive path.
        JSONObject captured = page(true, false);
        controller.acceptRemotePayload(envelope, page(false, true));
        drainWorker();
        assertFalse(controller.state.busy());
        assertEquals("completed", visibleToolState());
        assertEquals("Owned completed final answer", visibleFinalText());

        reads.complete.accept(captured, "");
        drainWorker(); drainWorker();
        System.out.println("[delayed-initial-history] busy=" + controller.state.busy()
            + " tool=" + visibleToolState());
        assertFalse("An older initial read must not resurrect a completed owner's busy state",
            controller.state.busy());
        assertEquals("completed", visibleToolState());
        assertEquals("The late initial page cannot remove the final body", "Owned completed final answer",
            visibleFinalText());
        assertEquals(0, controller.state.data.getJSONArray("permissions").length());
        assertEquals(0, controller.state.data.getJSONArray("questions").length());
        assertEquals(0, controller.state.data.getJSONArray("questionMessages").length());
        JSONArray messages = controller.state.data.getJSONArray("messages");
        boolean olderPresent = false;
        for (int i = 0; i < messages.length(); i++)
            olderPresent |= "msg_001".equals(messages.getJSONObject(i).optString("id"));
        assertTrue("The read still contributes previously unseen historical rows", olderPresent);
    }

    @Test public void lateInitialReadKeepsNewerRunningTurnAndItsPermission() throws Exception {
        Reads reads = new Reads();
        controller = new AIAssistantController(ApplicationProvider.getApplicationContext(),
            "http://owned.invalid", reads);
        JSONObject owner = owner("owned-login");
        controller.selectRemote(owner, accepted -> {});
        assertTrue(reads.started.await(3, TimeUnit.SECONDS));
        JSONObject captured = page(true, false);
        controller.acceptRemotePayload(envelope("owned-login"), page(false, true));
        drainWorker();
        JSONObject next = page(true, true).put("turnId", "msg_003")
            .put("permissions", new JSONArray().put(new JSONObject().put("id", "newer-permission")));
        JSONObject newerMessage = next.getJSONArray("messages").getJSONObject(0).put("id", "msg_003");
        newerMessage.getJSONArray("parts").getJSONObject(0).put("id", "prt_newer_tool");
        newerMessage.getJSONArray("parts").getJSONObject(1).put("id", "prt_newer_text")
            .put("text", "Owned newer turn progress");
        controller.acceptRemotePayload(envelope("owned-login"), next);
        drainWorker();
        reads.complete.accept(captured, "");
        drainWorker(); drainWorker();
        assertTrue(controller.state.busy());
        assertEquals("msg_003", controller.state.remoteSession.optString("turnId"));
        assertEquals("newer-permission", controller.state.data.getJSONArray("permissions")
            .getJSONObject(0).getString("id"));
        assertEquals("completed", visibleToolState());
        assertEquals("Owned completed final answer", visibleFinalText());
    }

    @Test public void oldLoginInitialReadCannotAlterReselectedSameSession() throws Exception {
        Reads reads = new Reads();
        controller = new AIAssistantController(ApplicationProvider.getApplicationContext(),
            "http://owned.invalid", reads);
        controller.selectRemote(owner("old-login"), accepted -> {});
        assertTrue(reads.started.await(3, TimeUnit.SECONDS));
        BiConsumer<JSONObject, String> oldReply = reads.complete;
        controller.selectRemote(owner("new-login"), accepted -> {});
        drainWorker();
        controller.acceptRemotePayload(envelope("new-login"), page(false, true));
        drainWorker();
        oldReply.accept(page(true, false), "");
        drainWorker(); drainWorker();
        assertEquals("new-login", controller.state.remoteSession.optString("login_session_id"));
        assertFalse(controller.state.busy());
        assertEquals("completed", visibleToolState());
        assertEquals("Owned completed final answer", visibleFinalText());
    }

    @Test public void freshInitialHistoryCanCorrectEarlierQueuedProgress() throws Exception {
        Reads reads = new Reads();
        controller = new AIAssistantController(ApplicationProvider.getApplicationContext(),
            "http://owned.invalid", reads);
        controller.selectRemote(owner("owned-login"), accepted -> {});
        assertTrue(reads.started.await(3, TimeUnit.SECONDS));
        // The mailbox is durable: an earlier progress event can arrive while
        // opening a Session whose authoritative history has already completed.
        controller.acceptRemotePayload(envelope("owned-login").put("id", 1).put("created_at", 1000),
            page(true, false));
        drainWorker();
        assertTrue(controller.state.busy());
        reads.complete.accept(page(false, true), "");
        drainWorker(); drainWorker();
        assertFalse("A fresh history read must still supersede older queued progress", controller.state.busy());
        assertEquals("completed", visibleToolState());
        assertEquals("Owned completed final answer", visibleFinalText());
    }

    @Test public void completedFirstSegmentDoesNotEndItsContinuingTurn() throws Exception {
        Reads reads = new Reads();
        controller = new AIAssistantController(ApplicationProvider.getApplicationContext(),
            "http://owned.invalid", reads);
        controller.selectRemote(owner("owned-login"), accepted -> {});
        assertTrue(reads.started.await(3, TimeUnit.SECONDS));
        JSONObject firstSegment = page(false, true).getJSONArray("messages").getJSONObject(0);
        JSONObject continuation = new JSONObject().put("id", "msg_003").put("role", "assistant")
            .put("created", 3).put("completed", 0).put("active", true)
            .put("parts", new JSONArray().put(new JSONObject().put("id", "prt_continuation")
                .put("kind", "text").put("text", "Owned same-turn continuation")));
        JSONObject live = page(true, true).put("turnId", "msg_002")
            .put("messages", new JSONArray().put(continuation).put(firstSegment));
        controller.acceptRemotePayload(envelope("owned-login"), live);
        drainWorker();
        // The stable initial turn id remains msg_002 after a new assistant
        // segment. A completed earlier segment says nothing about turn idle.
        reads.complete.accept(page(false, true).put("busy", true).put("turnId", "msg_002"), "");
        drainWorker(); drainWorker();
        assertTrue(controller.state.busy());
        assertEquals("msg_002", controller.state.remoteSession.optString("turnId"));
        boolean foundContinuation = false;
        JSONArray messages = controller.state.data.getJSONArray("messages");
        for (int i = 0; i < messages.length(); i++)
            foundContinuation |= "msg_003".equals(messages.getJSONObject(i).optString("id"));
        assertTrue(foundContinuation);
    }

    @Test public void explicitIdleCanConfirmAnAlreadyCompletedLastSegment() throws Exception {
        Reads reads = new Reads();
        controller = new AIAssistantController(ApplicationProvider.getApplicationContext(),
            "http://owned.invalid", reads);
        controller.selectRemote(owner("owned-login"), accepted -> {});
        assertTrue(reads.started.await(3, TimeUnit.SECONDS));
        controller.acceptRemotePayload(envelope("owned-login"),
            page(false, true).put("busy", true).put("turnId", "msg_002"));
        drainWorker();
        assertTrue(controller.state.busy());
        reads.complete.accept(page(false, true), "");
        drainWorker(); drainWorker();
        assertFalse(controller.state.busy());
        assertEquals("completed", visibleToolState());
    }

    @Test public void stateOnlyIdleUsesOneReadOnlyConfirmationInsteadOfGuessing() throws Exception {
        Reads reads = new Reads();
        controller = new AIAssistantController(ApplicationProvider.getApplicationContext(),
            "http://owned.invalid", reads);
        controller.selectRemote(owner("owned-login"), accepted -> {});
        assertTrue(reads.started.await(3, TimeUnit.SECONDS));
        controller.acceptRemotePayload(envelope("owned-login"), page(false, true).put("messages", new JSONArray()));
        drainWorker();
        reads.complete.accept(page(true, false), "");
        drainWorker(); drainWorker();
        assertFalse(controller.state.busy());
        assertEquals("A race schedules one bounded existing read API call", 2, reads.requests.get());
        reads.complete.accept(page(false, true), "");
        drainWorker(); drainWorker();
        assertFalse(controller.state.busy());
        assertEquals("completed", visibleToolState());
        assertEquals("Owned completed final answer", visibleFinalText());
        assertEquals(2, reads.requests.get());
    }

    @Test public void confirmationReplyCannotOverwriteAnInterveningLiveTurn() throws Exception {
        Reads reads = new Reads();
        controller = new AIAssistantController(ApplicationProvider.getApplicationContext(),
            "http://owned.invalid", reads);
        controller.selectRemote(owner("owned-login"), accepted -> {});
        assertTrue(reads.started.await(3, TimeUnit.SECONDS));
        controller.acceptRemotePayload(envelope("owned-login"), page(false, true));
        drainWorker();
        reads.complete.accept(page(true, false), "");
        drainWorker(); drainWorker();
        assertEquals(2, reads.requests.get());
        BiConsumer<JSONObject, String> confirmation = reads.complete;
        JSONObject next = page(true, true).put("turnId", "msg_003");
        JSONObject newerMessage = next.getJSONArray("messages").getJSONObject(0).put("id", "msg_003");
        newerMessage.getJSONArray("parts").getJSONObject(0).put("id", "prt_latest_tool");
        newerMessage.getJSONArray("parts").getJSONObject(1).put("id", "prt_latest_text");
        controller.acceptRemotePayload(envelope("owned-login"), next);
        drainWorker();
        confirmation.accept(page(false, true), "");
        drainWorker(); drainWorker();
        assertTrue(controller.state.busy());
        assertEquals("msg_003", controller.state.remoteSession.optString("turnId"));
        assertEquals(2, reads.requests.get());
    }

    @Test public void confirmationReplyCannotOverwriteAnInterveningCatalogTurn() throws Exception {
        Reads reads = new Reads();
        controller = new AIAssistantController(ApplicationProvider.getApplicationContext(),
            "http://owned.invalid", reads);
        controller.selectRemote(owner("owned-login"), accepted -> {});
        assertTrue(reads.started.await(3, TimeUnit.SECONDS));
        controller.acceptRemotePayload(envelope("owned-login"), page(false, true));
        drainWorker();
        reads.complete.accept(page(true, false), "");
        drainWorker(); drainWorker();
        BiConsumer<JSONObject, String> confirmation = reads.complete;
        JSONObject next = owner("owned-login").put("turnId", "msg_003");
        controller.acceptRemoteCatalog(new JSONArray().put(next), new JSONArray(), "android_phone");
        drainWorker();
        confirmation.accept(page(false, true), "");
        drainWorker(); drainWorker();
        assertTrue(controller.state.busy());
        assertEquals("msg_003", controller.state.remoteSession.optString("turnId"));
    }

    @Test public void confirmationRechecksRevisionAtQueuedApplyBoundary() throws Exception {
        Reads reads = new Reads();
        controller = new AIAssistantController(ApplicationProvider.getApplicationContext(),
            "http://owned.invalid", reads);
        controller.selectRemote(owner("owned-login"), accepted -> {});
        assertTrue(reads.started.await(3, TimeUnit.SECONDS));
        controller.acceptRemotePayload(envelope("owned-login"), page(false, true));
        drainWorker();
        reads.complete.accept(page(true, false), "");
        drainWorker(); drainWorker();
        BiConsumer<JSONObject, String> confirmation = reads.complete;
        CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
        Field field = AIAssistantController.class.getDeclaredField("worker"); field.setAccessible(true);
        ((Handler) field.get(controller)).post(() -> {
            entered.countDown();
            try { release.await(3, TimeUnit.SECONDS); }
            catch (InterruptedException interrupted) { Thread.currentThread().interrupt(); }
        });
        assertTrue(entered.await(3, TimeUnit.SECONDS));
        confirmation.accept(page(false, true), "");
        JSONObject next = page(true, true).put("turnId", "msg_003");
        next.getJSONArray("messages").getJSONObject(0).put("id", "msg_003");
        controller.acceptRemotePayload(envelope("owned-login"), next);
        release.countDown();
        drainWorker(); drainWorker();
        assertTrue(controller.state.busy());
        assertEquals("msg_003", controller.state.remoteSession.optString("turnId"));
    }

    @Test public void queuedProgressCannotRollbackAlreadyAcceptedCompletedHistory() throws Exception {
        Reads reads = new Reads();
        controller = new AIAssistantController(ApplicationProvider.getApplicationContext(),
            "http://owned.invalid", reads);
        controller.selectRemote(owner("owned-login"), accepted -> {});
        assertTrue(reads.started.await(3, TimeUnit.SECONDS));
        JSONObject capturedProgress = page(true, false);
        capturedProgress.getJSONArray("messages").getJSONObject(0).put("created", 1000);
        JSONObject completed = page(false, true);
        completed.getJSONArray("messages").getJSONObject(0).put("created", 1000).put("completed", 2000);
        reads.complete.accept(completed, "");
        drainWorker(); drainWorker();
        assertFalse(controller.state.busy());
        assertEquals("completed", visibleToolState());
        controller.acceptRemotePayload(envelope("owned-login").put("id", 1).put("created_at", 1500),
            capturedProgress);
        drainWorker(); drainWorker();
        System.out.println("[queued-live-after-complete-history] busy=" + controller.state.busy()
            + " tool=" + visibleToolState());
        assertFalse("Queued older progress must not resurrect busy after accepted completed history",
            controller.state.busy());
        assertEquals("completed", visibleToolState());
        assertEquals("Owned completed final answer", visibleFinalText());
    }

    @Test public void genuinelyNewTurnAfterConfirmedIdleStillStarts() throws Exception {
        Reads reads = new Reads();
        controller = new AIAssistantController(ApplicationProvider.getApplicationContext(), "http://owned.invalid", reads);
        controller.selectRemote(owner("owned-login"), accepted -> {});
        assertTrue(reads.started.await(3, TimeUnit.SECONDS));
        reads.complete.accept(page(false, true), "");
        drainWorker(); drainWorker();
        controller.acceptRemotePayload(envelope("owned-login").put("id", 1), page(true, false));
        drainWorker();
        JSONObject newer = page(true, true).put("turnId", "msg_003");
        JSONObject row = newer.getJSONArray("messages").getJSONObject(0).put("id", "msg_003");
        row.getJSONArray("parts").getJSONObject(0).put("id", "prt_new_turn_tool");
        row.getJSONArray("parts").getJSONObject(1).put("id", "prt_new_turn_text");
        controller.acceptRemotePayload(envelope("owned-login").put("id", 2), newer);
        drainWorker();
        assertTrue(controller.state.busy());
        assertEquals("msg_003", controller.state.remoteSession.optString("turnId"));
        assertEquals("completed", visibleToolState());
    }

    @Test public void mailboxOrderSurvivesFirstPendingSelectionCutover() throws Exception {
        Reads reads = new Reads();
        controller = new AIAssistantController(ApplicationProvider.getApplicationContext(), "http://owned.invalid", reads);
        controller.selectRemote(owner("owned-login"), accepted -> {});
        assertTrue(reads.started.await(3, TimeUnit.SECONDS));
        JSONObject latest = page(true, true);
        latest.getJSONArray("messages").getJSONObject(0).getJSONArray("parts").getJSONObject(1)
            .put("text", "Owned latest progress");
        controller.acceptRemotePayload(envelope("owned-login").put("id", 10), latest);
        drainWorker();
        JSONObject older = page(true, true);
        older.getJSONArray("messages").getJSONObject(0).getJSONArray("parts").getJSONObject(1)
            .put("text", "Owned ");
        controller.acceptRemotePayload(envelope("owned-login").put("id", 9), older);
        drainWorker();
        assertEquals("Owned latest progress", visibleFinalText());
    }

    @Test public void oldIdleCannotEndANewerKnownTurn() throws Exception {
        Reads reads = new Reads();
        controller = new AIAssistantController(ApplicationProvider.getApplicationContext(), "http://owned.invalid", reads);
        controller.selectRemote(owner("owned-login"), accepted -> {});
        assertTrue(reads.started.await(3, TimeUnit.SECONDS));
        reads.complete.accept(page(true, true), "");
        drainWorker(); drainWorker();
        JSONObject oldIdle = page(false, true);
        oldIdle.getJSONArray("messages").getJSONObject(0).put("id", "msg_001");
        controller.acceptRemotePayload(envelope("owned-login").put("id", 1), oldIdle);
        drainWorker();
        assertTrue(controller.state.busy());
        assertEquals("msg_002", controller.state.remoteSession.optString("turnId"));
        assertEquals(2, reads.requests.get());
    }

    @Test public void stateOnlyLiveIdleIsConfirmedWithoutGuessingFromASegment() throws Exception {
        Reads reads = new Reads();
        controller = new AIAssistantController(ApplicationProvider.getApplicationContext(), "http://owned.invalid", reads);
        controller.selectRemote(owner("owned-login"), accepted -> {});
        assertTrue(reads.started.await(3, TimeUnit.SECONDS));
        reads.complete.accept(page(true, true), "");
        drainWorker(); drainWorker();
        controller.acceptRemotePayload(envelope("owned-login").put("id", 1),
            page(false, true).put("messages", new JSONArray()));
        drainWorker();
        assertTrue(controller.state.busy());
        assertEquals(2, reads.requests.get());
        reads.complete.accept(page(false, true), "");
        drainWorker(); drainWorker();
        assertFalse(controller.state.busy());
        assertEquals("completed", visibleToolState());
    }

    @Test public void delayedCatalogCannotReopenConfirmedEndedTurn() throws Exception {
        Reads reads = new Reads();
        controller = new AIAssistantController(ApplicationProvider.getApplicationContext(), "http://owned.invalid", reads);
        controller.selectRemote(owner("owned-login"), accepted -> {});
        assertTrue(reads.started.await(3, TimeUnit.SECONDS));
        reads.complete.accept(page(false, true), "");
        drainWorker(); drainWorker();
        assertFalse(controller.state.busy());
        // A directory refresh can have captured this same turn while it was running.
        controller.acceptRemoteCatalog(new JSONArray().put(owner("owned-login")),
            new JSONArray(), "android_phone");
        drainWorker();
        assertFalse("A stale same-turn catalog must not reopen an explicitly confirmed ended turn",
            controller.state.busy());
        assertEquals("completed", visibleToolState());
        assertEquals("Owned completed final answer", visibleFinalText());
    }

    @Test public void catalogStillAdoptsMetadataAndGenuineNewTurn() throws Exception {
        Reads reads = new Reads();
        controller = new AIAssistantController(ApplicationProvider.getApplicationContext(), "http://owned.invalid", reads);
        controller.selectRemote(owner("owned-login"), accepted -> {});
        assertTrue(reads.started.await(3, TimeUnit.SECONDS));
        reads.complete.accept(page(false, true), "");
        drainWorker(); drainWorker();
        JSONObject stale = owner("owned-login").put("title", "Updated owned title")
            .put("device_name", "Updated owned device");
        controller.acceptRemoteCatalog(new JSONArray().put(stale), new JSONArray(), "android_phone");
        drainWorker();
        assertFalse(controller.state.busy());
        assertEquals("Updated owned title", controller.state.remoteSession.optString("title"));
        assertEquals("Updated owned device", controller.state.remoteSession.optString("device_name"));
        assertTrue("Do not mutate the transport's catalog object", stale.optBoolean("busy"));
        JSONObject newer = owner("owned-login").put("turnId", "msg_003").put("title", "New turn title");
        controller.acceptRemoteCatalog(new JSONArray().put(newer), new JSONArray(), "android_phone");
        drainWorker();
        assertTrue(controller.state.busy());
        assertEquals("msg_003", controller.state.remoteSession.optString("turnId"));
        controller.acceptRemoteCatalog(new JSONArray().put(owner("owned-login").put("title", "Latest metadata")),
            new JSONArray(), "android_phone");
        drainWorker();
        assertTrue(controller.state.busy());
        assertEquals("msg_003", controller.state.remoteSession.optString("turnId"));
        assertEquals("Latest metadata", controller.state.remoteSession.optString("title"));
    }

    @Test public void delayedIdleCatalogCannotEndNewerLiveTurn() throws Exception {
        Reads reads = new Reads();
        controller = new AIAssistantController(ApplicationProvider.getApplicationContext(), "http://owned.invalid", reads);
        controller.selectRemote(owner("owned-login"), accepted -> {});
        assertTrue(reads.started.await(3, TimeUnit.SECONDS));
        reads.complete.accept(page(false, true), "");
        drainWorker(); drainWorker();
        JSONObject next = page(true, true).put("turnId", "msg_003");
        JSONObject row = next.getJSONArray("messages").getJSONObject(0).put("id", "msg_003");
        row.getJSONArray("parts").getJSONObject(0).put("id", "prt_new_turn_tool");
        row.getJSONArray("parts").getJSONObject(1).put("id", "prt_new_turn_text");
        controller.acceptRemotePayload(envelope("owned-login").put("id", 10), next);
        drainWorker();
        assertTrue(controller.state.busy());
        JSONObject oldIdle = owner("owned-login").put("busy", false).put("turnId", "")
            .put("title", "Updated directory title");
        controller.acceptRemoteCatalog(new JSONArray().put(oldIdle), new JSONArray(), "android_phone");
        drainWorker();
        assertTrue("A delayed idle catalog cannot terminate a newer accepted live turn", controller.state.busy());
        assertEquals("msg_003", controller.state.remoteSession.optString("turnId"));
        assertEquals("Updated directory title", controller.state.remoteSession.optString("title"));
    }

    private static JSONObject owner(String login) throws Exception {
        return new JSONObject().put("id", "owned-session").put("device_slot", "windows")
            .put("login_session_id", login).put("online", true).put("busy", true).put("turnId", "msg_002");
    }

    @Test public void genuineIdleCatalogFinishesThroughOneReadOnlyConfirmation() throws Exception {
        Reads reads = new Reads();
        controller = new AIAssistantController(ApplicationProvider.getApplicationContext(), "http://owned.invalid", reads);
        controller.selectRemote(owner("owned-login"), accepted -> {});
        assertTrue(reads.started.await(3, TimeUnit.SECONDS));
        reads.complete.accept(page(false, true), "");
        drainWorker(); drainWorker();
        JSONObject running = page(true, true).put("turnId", "msg_003");
        JSONObject row = running.getJSONArray("messages").getJSONObject(0).put("id", "msg_003");
        row.getJSONArray("parts").getJSONObject(0).put("id", "prt_busy_tool");
        row.getJSONArray("parts").getJSONObject(1).put("id", "prt_busy_text");
        controller.acceptRemotePayload(envelope("owned-login").put("id", 10), running);
        drainWorker();
        JSONObject idle = owner("owned-login").put("busy", false).put("turnId", "")
            .put("title", "Authoritative idle catalog metadata");
        controller.acceptRemoteCatalog(new JSONArray().put(idle), new JSONArray(), "android_phone");
        drainWorker();
        assertTrue("A bare catalog cannot alone establish which turn ended", controller.state.busy());
        assertEquals("Authoritative idle catalog metadata", controller.state.remoteSession.optString("title"));
        assertEquals(2, reads.requests.get());
        // Metadata-only refreshes must coalesce and must not cancel a valid runtime confirmation.
        controller.acceptRemoteCatalog(new JSONArray().put(new JSONObject(idle.toString()).put("title", "Newer title")),
            new JSONArray(), "android_phone");
        drainWorker();
        assertEquals(2, reads.requests.get());
        JSONObject done = page(false, true);
        JSONObject finalRow = done.getJSONArray("messages").getJSONObject(0).put("id", "msg_003");
        finalRow.getJSONArray("parts").getJSONObject(0).put("id", "prt_busy_tool");
        finalRow.getJSONArray("parts").getJSONObject(1).put("id", "prt_busy_text");
        reads.complete.accept(done, "");
        drainWorker(); drainWorker();
        assertFalse("A genuinely idle owner still completes after the bounded read", controller.state.busy());
        assertEquals("Newer title", controller.state.remoteSession.optString("title"));
        assertEquals(2, reads.requests.get());
    }

    @Test public void knownLaterInitialTurnRejectsOldCatalogWithoutNeedingIdleGap() throws Exception {
        Reads reads = new Reads();
        controller = new AIAssistantController(ApplicationProvider.getApplicationContext(), "http://owned.invalid", reads);
        controller.selectRemote(owner("owned-login"), accepted -> {});
        assertTrue(reads.started.await(3, TimeUnit.SECONDS));
        reads.complete.accept(page(true, true), "");
        drainWorker(); drainWorker();
        JSONObject next = page(true, true).put("turnId", "msg_003");
        next.getJSONArray("messages").getJSONObject(0).put("id", "msg_003");
        controller.acceptRemotePayload(envelope("owned-login"), next);
        drainWorker();
        controller.acceptRemoteCatalog(new JSONArray().put(owner("owned-login").put("title", "Still adopt metadata")),
            new JSONArray(), "android_phone");
        drainWorker();
        assertTrue(controller.state.busy());
        assertEquals("msg_003", controller.state.remoteSession.optString("turnId"));
        assertEquals("Still adopt metadata", controller.state.remoteSession.optString("title"));
    }

    @Test public void knownLaterInitialTurnRejectsOldLiveRuntimeWithoutNeedingIdleGap() throws Exception {
        Reads reads = new Reads();
        controller = new AIAssistantController(ApplicationProvider.getApplicationContext(), "http://owned.invalid", reads);
        controller.selectRemote(owner("owned-login"), accepted -> {});
        assertTrue(reads.started.await(3, TimeUnit.SECONDS));
        reads.complete.accept(page(true, true), "");
        drainWorker(); drainWorker();
        JSONObject next = page(true, true).put("turnId", "msg_003")
            .put("permissions", new JSONArray().put(new JSONObject().put("id", "new-turn-permission")));
        next.getJSONArray("messages").getJSONObject(0).put("id", "msg_003");
        controller.acceptRemotePayload(envelope("owned-login"), next);
        drainWorker();
        controller.acceptRemotePayload(envelope("owned-login"), page(true, false));
        drainWorker();
        assertTrue(controller.state.busy());
        assertEquals("msg_003", controller.state.remoteSession.optString("turnId"));
        assertEquals("new-turn-permission", controller.state.data.getJSONArray("permissions")
            .getJSONObject(0).getString("id"));
    }

    private static JSONObject envelope(String login) throws Exception {
        return new JSONObject().put("session_id", "owned-session").put("from_slot", "windows")
            .put("from_login_session_id", login);
    }

    private static JSONObject page(boolean running, boolean live) throws Exception {
        JSONObject part = new JSONObject().put("id", "prt_owned_tool").put("kind", "tool")
            .put("tool", "owned_long_tool").put("state", running ? "running" : "completed");
        JSONObject message = new JSONObject().put("id", "msg_002").put("role", "assistant")
            .put("active", running).put("created", 2).put("completed", running ? 0 : 3)
            .put("parts", new JSONArray().put(part));
        if (live) message.getJSONArray("parts").put(new JSONObject().put("id", "prt_final")
            .put("kind", "text").put("text", "Owned completed final answer"));
        JSONArray messages = new JSONArray().put(message);
        if (!live) messages.put(new JSONObject().put("id", "msg_001").put("role", "user")
            .put("parts", new JSONArray().put(new JSONObject().put("id", "prt_older")
                .put("kind", "text").put("text", "Owned original request"))));
        JSONObject page = new JSONObject().put("ok", true).put("messages", messages)
            .put("busy", running).put("turnId", running ? "msg_002" : "")
            .put("permissions", new JSONArray()).put("questions", new JSONArray())
            .put("questionMessages", new JSONArray());
        if (!live) {
            page.put("permissions", new JSONArray().put(new JSONObject().put("id", "old-permission")));
            page.put("questions", new JSONArray().put(new JSONObject().put("id", "old-question")));
            page.put("questionMessages", new JSONArray().put(new JSONObject().put("id", "old-question-row")
                .put("role", "assistant").put("parts", new JSONArray())));
        }
        return page;
    }

    private String visibleToolState() throws Exception {
        JSONArray messages = controller.state.data.getJSONArray("messages");
        for (int i = 0; i < messages.length(); i++) {
            JSONObject message = messages.getJSONObject(i);
            if (message.optString("id").equals("msg_002"))
                return message.getJSONArray("parts").getJSONObject(0).getString("state");
        }
        throw new AssertionError("Owned assistant was not visible");
    }

    private String visibleFinalText() throws Exception {
        JSONArray messages = controller.state.data.getJSONArray("messages");
        for (int i = 0; i < messages.length(); i++) {
            JSONObject message = messages.getJSONObject(i);
            if (!message.optString("id").equals("msg_002")) continue;
            JSONArray parts = message.getJSONArray("parts");
            for (int j = 0; j < parts.length(); j++)
                if (parts.getJSONObject(j).optString("kind").equals("text"))
                    return parts.getJSONObject(j).optString("text");
        }
        return "";
    }

    private void drainWorker() throws Exception {
        Field worker = AIAssistantController.class.getDeclaredField("worker"); worker.setAccessible(true);
        CompletableFuture<Void> done = new CompletableFuture<>();
        ((Handler) worker.get(controller)).post(() -> done.complete(null));
        done.get(3, TimeUnit.SECONDS);
        Shadows.shadowOf(Looper.getMainLooper()).idle();
    }

    private static final class Reads implements AIAssistantController.SessionReads {
        final CountDownLatch started = new CountDownLatch(1);
        final AtomicInteger requests = new AtomicInteger();
        volatile BiConsumer<JSONObject, String> complete;
        @Override public void history(JSONObject owner, String before, int limit,
                BiConsumer<JSONObject, String> completion) {
            complete = completion; requests.incrementAndGet(); started.countDown();
        }
        @Override public String command(JSONObject owner, JSONObject request,
                BiConsumer<String, String> sent, BiConsumer<JSONObject, JSONObject> received) {
            // A metadata read can be initiated after the initial history page.
            // Keep it pending; no task or mutable command is executed by this seam.
            sent.accept("owned-metadata-read", "");
            return "owned-metadata-read";
        }
    }

    @Implements(value = AIAssistantController.class, isInAndroidSdk = false)
    public static final class OwnedNative {
        @Implementation protected void initialize() { }
        @Implementation protected static void nativeDestroy(long handle) {
            fail("No native session was created by this read-only fixture");
        }
    }
}
