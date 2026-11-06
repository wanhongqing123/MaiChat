// 文件作用：用确定性存储验证持久接收先于清稿、未知收据不重播、会话 FIFO 和内容快照。
package com.kongshang.maichat;

import static org.junit.Assert.*;
import java.io.IOException;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;

public final class AIAssistantOutgoingQueueTest {
    private static final class MemoryStorage implements AIAssistantOutgoingQueue.Storage {
        String saved = "";
        boolean writable = true;
        public String read() { return saved; }
        public boolean write(String value) { if (!writable) return false; saved = value; return true; }
    }
    private JSONObject payload(String text) throws Exception {
        return new JSONObject().put("session", "session-one").put("text", text)
            .put("quoteMessageId", "quote-one").put("turnId", "turn-one")
            .put("operation", "steer").put("files", new JSONArray().put(
                new JSONObject().put("path", "private/input.jpg").put("mimeType", "image/jpeg")));
    }
    @Test public void failedCommitDoesNotAcceptOrForgetTheDraft() throws Exception {
        MemoryStorage storage = new MemoryStorage();
        AIAssistantOutgoingQueue queue = new AIAssistantOutgoingQueue(storage);
        storage.writable = false;
        try { queue.enqueue("owner-one", payload("first")); fail("A failed write must reject intake"); }
        catch (IOException expected) { }
        assertEquals(0, queue.entries().length());
    }
    @Test public void reloadKeepsUnknownDeliveryAndBlocksOnlyItsOriginalOwner() throws Exception {
        MemoryStorage storage = new MemoryStorage();
        AIAssistantOutgoingQueue queue = new AIAssistantOutgoingQueue(storage);
        String first = queue.enqueue("owner-one", payload("first"));
        String second = queue.enqueue("owner-one", payload("second"));
        String other = queue.enqueue("owner-two", payload("other"));
        queue.transition(first, "dispatching", null);
        AIAssistantOutgoingQueue restored = new AIAssistantOutgoingQueue(storage);
        assertEquals("unknown", restored.head("owner-one").getString("state"));
        assertEquals(first, restored.head("owner-one").getString("id"));
        assertEquals(other, restored.head("owner-two").getString("id"));
        assertFalse(second.equals(restored.head("owner-one").getString("id")));
    }
    @Test public void acceptedReceiptAdvancesFifoAndCaptureCannotBeMutated() throws Exception {
        AIAssistantOutgoingQueue queue = new AIAssistantOutgoingQueue(new MemoryStorage());
        JSONObject original = payload("first");
        String first = queue.enqueue("owner-one", original);
        String second = queue.enqueue("owner-one", payload("second"));
        original.put("text", "changed").getJSONArray("files").getJSONObject(0).put("path", "changed.jpg");
        JSONObject entry = queue.head("owner-one");
        assertEquals("first", entry.getJSONObject("payload").getString("text"));
        assertEquals("private/input.jpg", entry.getJSONObject("payload").getJSONArray("files")
            .getJSONObject(0).getString("path"));
        assertEquals("quote-one", entry.getJSONObject("payload").getString("quoteMessageId"));
        assertNotEquals(first, second);
        queue.transition(first, "dispatching", null);
        queue.accept(first, "message-one", "turn-one");
        assertEquals(second, queue.head("owner-one").getString("id"));
    }
    @Test public void firstSessionCreationRetargetsAllUnsentClicksTogether() throws Exception {
        AIAssistantOutgoingQueue queue = new AIAssistantOutgoingQueue(new MemoryStorage());
        JSONObject first = payload("first").put("session", "");
        String firstId = queue.enqueue("local:", first);
        queue.enqueue("local:", payload("second").put("session", ""));
        queue.retarget("local:", "local:created", "created");
        assertNull(queue.head("local:"));
        assertEquals(firstId, queue.head("local:created").getString("id"));
        assertEquals("created", queue.entries().getJSONObject(1).getJSONObject("payload").getString("session"));
    }
    @Test public void failedStateWriteKeepsThePreviouslySavedCapture() throws Exception {
        MemoryStorage storage = new MemoryStorage();
        AIAssistantOutgoingQueue queue = new AIAssistantOutgoingQueue(storage);
        String id = queue.enqueue("owner-one", payload("first"));
        String saved = storage.saved;
        storage.writable = false;
        try { queue.transition(id, "dispatching", null); fail("Dispatch must not precede its durable binding"); }
        catch (IOException expected) { }
        assertEquals(saved, storage.saved);
        assertEquals("queued", queue.head("owner-one").getString("state"));
    }
    @Test public void unknownCannotBeRestoredAsANewSendButExplicitRejectionCan() throws Exception {
        AIAssistantOutgoingQueue queue = new AIAssistantOutgoingQueue(new MemoryStorage());
        String id = queue.enqueue("owner-one", payload("first"));
        queue.transition(id, "dispatching", null); queue.transition(id, "unknown", null);
        try { queue.restore(id); fail("Unknown delivery must never be copied into a new send"); }
        catch (IOException expected) { }
        assertEquals(id, queue.head("owner-one").getString("id"));
        String failed = queue.enqueue("owner-two", payload("rejected"));
        queue.transition(failed, "failed", null);
        assertEquals("rejected", queue.restore(failed).getString("text"));
        assertNull(queue.head("owner-two"));
    }
    @Test public void oversizedCaptureIsRejectedBeforeTheDraftCanBeDetached() throws Exception {
        AIAssistantOutgoingQueue queue = new AIAssistantOutgoingQueue(new MemoryStorage());
        try { queue.enqueue("owner-one", payload("x".repeat(128 * 1024))); fail("Bounded intake must reject oversized payload"); }
        catch (IOException expected) { }
        assertEquals(0, queue.entries().length());
    }
    @Test public void interruptedPreparationIsStillUndispatchedAndCanResumeWithItsOriginalBinding() throws Exception {
        MemoryStorage storage = new MemoryStorage();
        AIAssistantOutgoingQueue queue = new AIAssistantOutgoingQueue(storage);
        String id = queue.enqueue("owner-one", payload("first"));
        JSONArray saved = new JSONArray(storage.saved);
        saved.getJSONObject(0).put("state", "preparing"); storage.saved = saved.toString();
        AIAssistantOutgoingQueue restored = new AIAssistantOutgoingQueue(storage);
        assertEquals("queued", restored.head("owner-one").getString("state"));
        assertEquals(id, restored.head("owner-one").getString("id"));
        assertEquals("turn-one", restored.head("owner-one").getJSONObject("payload").getString("turnId"));
    }
    @Test public void onlyFirstRapidSendClaimsAPendingQuestionAndLaterTextKeepsItsTurn() throws Exception {
        AIAssistantOutgoingQueue queue = new AIAssistantOutgoingQueue(new MemoryStorage());
        JSONObject answer = payload("Use folder A").put("operation", "answer").put("questionId", "question-one");
        String first = queue.enqueue("owner-one", answer);
        String second = queue.enqueue("owner-one", answer);
        queue.enqueue("owner-two", answer);
        JSONArray items = queue.entries();
        assertEquals("answer", items.getJSONObject(0).getJSONObject("payload").getString("operation"));
        assertEquals("steer", items.getJSONObject(1).getJSONObject("payload").getString("operation"));
        assertEquals("", items.getJSONObject(1).getJSONObject("payload").optString("questionId"));
        assertEquals("turn-one", items.getJSONObject(1).getJSONObject("payload").getString("turnId"));
        assertEquals("answer", items.getJSONObject(2).getJSONObject("payload").getString("operation"));
        queue.transition(first, "dispatching", null);
        queue.acceptAnswer(first, "question-one");
        assertEquals(second, queue.head("owner-one").getString("id"));
        queue.enqueue("owner-one", answer);
        JSONObject lateClick = queue.entries().getJSONObject(queue.entries().length() - 1).getJSONObject("payload");
        assertEquals("An ACK processed before the UI snapshot must still reserve this question", "steer", lateClick.getString("operation"));
    }
    @Test public void answerAcknowledgmentMustMatchTheCapturedQuestionAndDurablyRemoveOnlyItsHead() throws Exception {
        MemoryStorage storage = new MemoryStorage();
        AIAssistantOutgoingQueue queue = new AIAssistantOutgoingQueue(storage);
        String id = queue.enqueue("owner-one", payload("answer").put("operation", "answer").put("questionId", "question-one"));
        queue.transition(id, "dispatching", null);
        try { queue.acceptAnswer(id, "question-two"); fail("A different question cannot acknowledge this text"); }
        catch (IOException expected) { }
        assertEquals(id, queue.head("owner-one").getString("id"));
        storage.writable = false;
        try { queue.acceptAnswer(id, "question-one"); fail("Failed durable removal must keep the original answer"); }
        catch (IOException expected) { }
        assertEquals(id, queue.head("owner-one").getString("id"));
    }

    @Test public void parallelQuestionsAreClaimedInOrderBeforeFurtherTextBecomesSteering() throws Exception {
        AIAssistantOutgoingQueue queue = new AIAssistantOutgoingQueue(new MemoryStorage());
        JSONObject answer = payload("answer").put("operation", "answer").put("questionId", "question-one")
            .put("questionCandidates", new JSONArray().put(new JSONObject().put("id", "question-one").put("partId", "part-one"))
                .put(new JSONObject().put("id", "question-two").put("partId", "part-two")));
        queue.enqueue("owner-one", answer); queue.enqueue("owner-one", answer); queue.enqueue("owner-one", answer);
        JSONArray rows = queue.entries();
        assertEquals("question-one", rows.getJSONObject(0).getJSONObject("payload").getString("questionId"));
        assertEquals("question-two", rows.getJSONObject(1).getJSONObject("payload").getString("questionId"));
        assertEquals("part-two", rows.getJSONObject(1).getJSONObject("payload").getJSONObject("question").getString("partId"));
        assertEquals("steer", rows.getJSONObject(2).getJSONObject("payload").getString("operation"));
    }

}
