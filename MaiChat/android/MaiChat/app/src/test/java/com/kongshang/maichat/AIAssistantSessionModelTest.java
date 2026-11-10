// Session projections use the native snapshot; the account default only covers an empty page.
package com.kongshang.maichat;

import static org.junit.Assert.*;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;

public final class AIAssistantSessionModelTest {
    @Test public void anUnreadRemoteModelIsUnknownRatherThanThePhonesAccountDefault() throws Exception {
        JSONObject owner = new JSONObject().put("id", "remote-a").put("device_slot", "mac")
            .put("login_session_id", "login-a");
        AIAssistantController.State shown = new AIAssistantController.State(new JSONObject(), owner,
            "remote-a", "https://api.deepseek.com", "deepseek-flash", "on-request", "", "", "", 0,
            false, true);
        assertEquals("The phone default cannot identify an unread owner model", "", shown.model);
    }

    private AIAssistantController.State state(JSONObject snapshot, String session) {
        return new AIAssistantController.State(snapshot, null, session, "https://api.deepseek.com",
            "deepseek-flash", "on-request", "", "", "", 0, false, true);
    }

    @Test public void selectingAnotherSessionRestoresItsOwnExactModel() throws Exception {
        JSONObject snapshot = new JSONObject().put("sessions", new JSONArray()
            .put(new JSONObject().put("id", "a").put("model", "glm-5.3-flash")
                .put("reasoningEffort", "max").put("wire", "chat_completions"))
            .put(new JSONObject().put("id", "b").put("model", "deepseek-v4-pro")
                .put("reasoningEffort", "low").put("wire", "responses")));
        assertEquals("glm-5.3-flash", state(snapshot, "a").model);
        assertEquals("max", state(snapshot, "a").reasoningEffort);
        assertEquals("deepseek-v4-pro", state(snapshot, "b").model);
        assertEquals("low", state(snapshot, "b").reasoningEffort);
        assertEquals("deepseek-flash", state(snapshot, "").model);
        assertEquals("The projection must not mutate the native session rows", "glm-5.3-flash",
            snapshot.getJSONArray("sessions").getJSONObject(0).getString("model"));
    }

    @Test public void selectedTopLevelModelDoesNotChangeAnotherSessionsProjection() throws Exception {
        JSONObject snapshot = new JSONObject().put("selectedModel", "glm-5.3")
            .put("sessions", new JSONArray().put(new JSONObject().put("id", "b")
                .put("model", "deepseek-v4-pro").put("reasoningEffort", "low")));
        assertEquals("deepseek-v4-pro", state(snapshot, "b").model);
    }

    @Test public void changingAnotherPageCannotRetargetAnAlreadyCapturedSelection() throws Exception {
        JSONObject change = AIAssistantSessionModels.selection("a", "glm-5.3-flash", "max");
        assertEquals("set_model", change.getString("op"));
        assertEquals("a", change.getString("session"));
        assertEquals("glm-5.3-flash", change.getString("model"));
        assertEquals("max", change.getString("reasoningEffort"));
        JSONObject other = AIAssistantSessionModels.selection("b", "deepseek-v4-pro", "low");
        assertEquals("a", change.getString("session"));
        assertEquals("b", other.getString("session"));
        assertFalse(change.has("apiKey"));
        assertFalse(change.has("baseUrl"));
    }

    @Test public void emptyPageMakesOneExplicitSessionAndOmittedEffortStaysOmitted() throws Exception {
        JSONObject created = AIAssistantSessionModels.selection("", AIAssistantSessionModels.LOCAL, null);
        assertEquals("create", created.getString("op"));
        assertEquals(AIAssistantSessionModels.LOCAL, created.getString("model"));
        assertFalse(created.has("reasoningEffort"));
        assertEquals("", AIAssistantSessionModels.selection("a", "glm-5.3", "")
            .getString("reasoningEffort"));
    }

    @Test public void failedOrWrongSessionReceiptsDoNotConfirmTheSelection() throws Exception {
        for (JSONObject receipt : new JSONObject[]{
                new JSONObject().put("ok", false).put("session", "a").put("model", "glm-5.3"),
                new JSONObject().put("ok", true).put("session", "b").put("model", "glm-5.3"),
                new JSONObject().put("ok", true).put("session", "a").put("model", "deepseek-flash")}) {
            try {
                AIAssistantSessionModels.validateReceipt(receipt, "a", "glm-5.3");
                fail("A failed or unrelated receipt cannot commit a choice");
            } catch (IllegalStateException expected) { }
        }
        AIAssistantSessionModels.validateReceipt(new JSONObject().put("ok", true)
            .put("session", "a").put("model", "glm-5.3"), "a", "glm-5.3");
    }

    @Test public void sessionHistoryProjectionPreservesLocalAndEmptyEffortInheritsOnlyDefault() throws Exception {
        JSONObject snapshot = new JSONObject().put("defaultReasoningEffort", "max")
            .put("sessions", new JSONArray().put(new JSONObject().put("id", "local")
                .put("model", AIAssistantSessionModels.LOCAL).put("reasoningEffort", "")));
        assertEquals(AIAssistantSessionModels.LOCAL, state(snapshot, "local").model);
        assertEquals("max", state(snapshot, "local").reasoningEffort);
        assertEquals("deepseek-flash", state(snapshot, "").model);
    }

    private JSONObject owner(String slot, String login, String session) throws Exception {
        return new JSONObject().put("id", session).put("device_slot", slot)
            .put("login_session_id", login).put("online", true);
    }

    private JSONObject receipt(String session, String model, String effort) throws Exception {
        return new JSONObject().put("ok", true).put("session", session).put("model", model)
            .put("reasoningEffort", effort).put("wire", model.startsWith("glm-")
                ? "chat_completions" : "responses");
    }

    @Test public void remoteSelectionWaitsForOwnerAckAndFailureRetainsTheCommittedModel() throws Exception {
        AIAssistantSessionModels.Remote cache = new AIAssistantSessionModels.Remote();
        JSONObject owner = owner("mac", "login-a", "a");
        assertFalse(cache.project(owner).has("model"));
        assertTrue(cache.accept(cache.begin(owner, null), receipt("a", "glm-5.3-flash", "max")));
        AIAssistantSessionModels.Remote.Ticket pending = cache.begin(owner, "deepseek-v4-pro");
        assertTrue(cache.changing(owner));
        assertEquals("glm-5.3-flash", cache.project(owner).getString("model"));
        try {
            cache.accept(pending, new JSONObject().put("ok", false).put("error", "busy"));
            fail("The forwarding receipt cannot confirm an owner mutation");
        } catch (IllegalStateException expected) { }
        assertFalse(cache.changing(owner));
        assertEquals("glm-5.3-flash", cache.project(owner).getString("model"));
        assertTrue(cache.accept(cache.begin(owner, "deepseek-v4-pro"), receipt("a", "deepseek-v4-pro", "low")));
        assertEquals("deepseek-v4-pro", cache.project(owner).getString("model"));
        assertEquals("low", cache.project(owner).getString("reasoningEffort"));
    }

    @Test public void remoteOldReadAndOldLoginCannotReplaceTheNewModel() throws Exception {
        AIAssistantSessionModels.Remote cache = new AIAssistantSessionModels.Remote();
        JSONObject first = owner("mac", "old-login", "same-id");
        JSONObject next = owner("mac", "new-login", "same-id");
        AIAssistantSessionModels.Remote.Ticket oldRead = cache.begin(first, null);
        AIAssistantSessionModels.Remote.Ticket current = cache.begin(first, "deepseek-v4-pro");
        assertTrue(cache.accept(current, receipt("same-id", "deepseek-v4-pro", "low")));
        assertFalse(cache.accept(oldRead, receipt("same-id", "glm-5.3", "max")));
        assertEquals("deepseek-v4-pro", cache.project(first).getString("model"));
        assertFalse(cache.project(next).has("model"));
        assertTrue(cache.accept(cache.begin(next, null), receipt("same-id", "glm-5.3-flash", "high")));
        assertEquals("glm-5.3-flash", cache.project(next).getString("model"));
        assertEquals("deepseek-v4-pro", cache.project(first).getString("model"));
        JSONObject envelope = new JSONObject().put("session_id", "same-id").put("from_slot", "mac")
            .put("from_login_session_id", "old-login");
        assertTrue(AIAssistantSessionModels.Remote.matchesOwner(first, envelope));
        assertFalse(AIAssistantSessionModels.Remote.matchesOwner(next, envelope));
        assertFalse(AIAssistantSessionModels.Remote.matchesOwner(owner("windows", "old-login", "same-id"), envelope));
        assertFalse(AIAssistantSessionModels.Remote.matchesOwner(owner("mac", "old-login", "other-id"), envelope));
        assertFalse(AIAssistantSessionModels.Remote.matchesOwner(new JSONObject(), new JSONObject()));
    }

    @Test public void remoteCatalogRefreshAndOfflineViewRetainOnlyThatOwnersAck() throws Exception {
        AIAssistantSessionModels.Remote cache = new AIAssistantSessionModels.Remote();
        JSONObject first = owner("mac", "login-a", "a");
        cache.accept(cache.begin(first, null), receipt("a", "glm-5.3", "max"));
        JSONObject refreshed = owner("mac", "login-a", "a").put("title", "changed").put("online", false);
        JSONObject projected = cache.project(refreshed);
        assertEquals("glm-5.3", projected.getString("model"));
        assertEquals("changed", projected.getString("title"));
        assertFalse(projected.getBoolean("online"));
        assertFalse("Catalog input stays an unmodified owner record", refreshed.has("model"));
        AIAssistantSessionModels.Remote.Ticket request = cache.begin(first, "deepseek-flash");
        cache.finish(request);
        assertFalse(cache.accept(request, receipt("a", "deepseek-flash", "low")));
        assertEquals("glm-5.3", cache.project(first).getString("model"));
    }

    @Test public void remoteWireHasOnlySelectionFieldsAndNeverLocalPreparation() throws Exception {
        JSONObject selection = AIAssistantSessionModels.Remote.request("glm-5.3-flash", "max");
        assertEquals(3, selection.length());
        assertEquals("set_model", selection.getString("action"));
        assertEquals("glm-5.3-flash", selection.getString("model"));
        assertEquals("max", selection.getString("reasoningEffort"));
        assertEquals(2, AIAssistantSessionModels.Remote.request("deepseek-v4-pro", null).length());
        assertEquals(1, AIAssistantSessionModels.Remote.request(null, null).length());
        try {
            AIAssistantSessionModels.Remote.request(AIAssistantSessionModels.LOCAL, "high");
            fail("Remote viewer must not send local preparation or paths");
        } catch (IllegalArgumentException expected) { }
    }

    @Test public void remoteWrongEffortOrIncompleteWireCannotReplaceCommittedConfiguration() throws Exception {
        AIAssistantSessionModels.Remote cache = new AIAssistantSessionModels.Remote();
        JSONObject owner = owner("mac", "login-a", "a");
        cache.accept(cache.begin(owner, null), receipt("a", "glm-5.3", "high"));
        try {
            cache.accept(cache.begin(owner, "glm-5.3", "max"), receipt("a", "glm-5.3", "low"));
            fail("A different actual effort does not confirm this selection");
        } catch (IllegalStateException expected) { }
        assertEquals("high", cache.project(owner).getString("reasoningEffort"));
        try {
            cache.accept(cache.begin(owner, "deepseek-flash"),
                receipt("a", "deepseek-flash", "low").put("wire", "mixed"));
            fail("A route catalog is not an actual per-model wire");
        } catch (IllegalStateException expected) { }
        assertEquals("glm-5.3", cache.project(owner).getString("model"));
        assertFalse("A different account gets a distinct cache instance",
            new AIAssistantSessionModels.Remote().project(owner).has("model"));
    }
}
