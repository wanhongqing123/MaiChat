// 文件作用：验证明确 user/turn 回执及连续空闲点击的原轮次持久绑定。
package com.kongshang.maichat;

import static org.junit.Assert.*;
import java.io.IOException;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;

public final class AIAssistantReceiptTest {
    private JSONObject accepted(String user, String turn) throws Exception {
        return new JSONObject().put("ok", true).put("id", "assistant-compatible-id")
            .put("user_message_id", user).put("turnId", turn);
    }
    private static final class Memory implements AIAssistantOutgoingQueue.Storage {
        String saved = ""; boolean writable = true;
        public String read() { return saved; }
        public boolean write(String value) { if (!writable) return false; saved = value; return true; }
    }
    private JSONObject draft(String body) throws Exception {
        return new JSONObject().put("session", "one").put("text", body)
            .put("operation", "").put("turnId", "").put("quoteMessageId", "quote")
            .put("files", new JSONArray().put(new JSONObject().put("path", "fixture.txt")));
    }
    @Test public void sendUsesExplicitUserAndTurnInsteadOfTheCompatibleAssistantId() throws Exception {
        JSONObject result = AIAssistantSubmission.receipt(accepted("user-one", "turn-one"), "send", "");
        assertTrue(result.getBoolean("ok"));
        assertEquals("user-one", result.getString("user_message_id"));
        assertEquals("turn-one", result.getString("turnId"));
        assertEquals("assistant-compatible-id", result.getString("id"));
        for (String missing : new String[]{"user_message_id", "turnId"}) {
            JSONObject incomplete = accepted("user-one", "turn-one"); incomplete.remove(missing);
            JSONObject unknown = AIAssistantSubmission.receipt(incomplete, "send", "");
            assertFalse("An incomplete success cannot release the durable capture", unknown.optBoolean("ok"));
            assertTrue(unknown.optBoolean("unknown"));
        }
    }
    @Test public void steerReceiptMustBelongToItsOriginalTurnEvenAfterANewTurnStarts() throws Exception {
        JSONObject action = new JSONObject().put("action", "steer").put("turnId", "turn-original");
        JSONObject original = AndroidSessionSyncCoordinator.commandReceipt(action,
            accepted("user-one", "turn-original"));
        assertTrue(original.getBoolean("ok"));
        JSONObject wrong = AndroidSessionSyncCoordinator.commandReceipt(action,
            accepted("user-one", "turn-successor"));
        assertFalse(wrong.optBoolean("ok")); assertTrue(wrong.optBoolean("unknown"));
        JSONObject numeric = accepted("user-one", "turn-original").put("turnId", 123);
        assertTrue(AIAssistantSubmission.receipt(numeric, "steer", "turn-original").optBoolean("unknown"));
    }
    @Test public void acceptedHeadAtomicallyBindsIdleTailsAndKeepsTheirIdentityAndAttachments() throws Exception {
        Memory storage = new Memory();
        AIAssistantOutgoingQueue queue = new AIAssistantOutgoingQueue(storage);
        String first = queue.enqueue("local:one", draft("first"));
        String second = queue.enqueue("local:one", draft("second"));
        queue.enqueue("local:other", draft("other"));
        queue.transition(first, "dispatching", null);
        queue.accept(first, "user-first", "turn-first");
        AIAssistantOutgoingQueue restored = new AIAssistantOutgoingQueue(storage);
        JSONObject tail = restored.head("local:one");
        assertEquals(second, tail.getString("id"));
        assertEquals("steer", tail.getJSONObject("payload").getString("operation"));
        assertEquals("turn-first", tail.getJSONObject("payload").getString("turnId"));
        assertEquals("quote", tail.getJSONObject("payload").getString("quoteMessageId"));
        assertEquals("fixture.txt", tail.getJSONObject("payload").getJSONArray("files").getJSONObject(0).getString("path"));
        assertEquals("", restored.head("local:other").getJSONObject("payload").getString("operation"));
        AIAssistantSubmission binding = AIAssistantSubmission.capture("one", null,
            new JSONObject().put("busy", true).put("turnId", "turn-first"));
        try { binding.bind(new JSONObject().put("busy", false)); fail("An ended initial turn must not become send"); }
        catch (IllegalStateException expected) { }
    }
    @Test public void rejectedReceiptAndFailedCommitCannotPartiallyBindOrForgetTheFifo() throws Exception {
        Memory storage = new Memory();
        AIAssistantOutgoingQueue queue = new AIAssistantOutgoingQueue(storage);
        String first = queue.enqueue("local:one", draft("first"));
        queue.enqueue("local:one", draft("second"));
        queue.transition(first, "dispatching", null);
        String saved = storage.saved;
        try { queue.accept(first, "user-first", ""); fail("Missing turn cannot release a queued initial burst"); }
        catch (IOException expected) { }
        storage.writable = false;
        try { queue.accept(first, "user-first", "turn-first"); fail("Binding and removal need one durable write"); }
        catch (IOException expected) { }
        assertEquals(saved, storage.saved);
        assertEquals(first, queue.head("local:one").getString("id"));
        assertEquals("", queue.entries().getJSONObject(1).getJSONObject("payload").getString("turnId"));
    }
}
