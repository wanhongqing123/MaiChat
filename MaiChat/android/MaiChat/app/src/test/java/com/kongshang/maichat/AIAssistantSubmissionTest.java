// 文件作用：验证忙时补充的不可变所有者、稳定请求编号，以及轮次结束后禁止改发新任务。
package com.kongshang.maichat;

import static org.junit.Assert.*;
import org.json.JSONObject;
import org.json.JSONArray;
import org.junit.Test;

public final class AIAssistantSubmissionTest {
    private JSONObject runtime(boolean busy, String turn) throws Exception {
        return new JSONObject().put("busy", busy).put("turnId", turn);
    }

    @Test public void busySubmissionKeepsItsTurnAndCannotBecomeANewPrompt() throws Exception {
        AIAssistantSubmission binding = AIAssistantSubmission.capture("session-one", null,
            runtime(true, "turn-one"));
        assertEquals("steer", binding.bind(runtime(true, "turn-one")));
        String id = binding.requestId;
        assertEquals("steer", binding.bind(runtime(true, "turn-one")));
        assertEquals(id, binding.requestId);
        for (JSONObject state : new JSONObject[] {runtime(false, ""), runtime(true, "turn-two")}) {
            try { binding.bind(state); fail("An ended/replaced turn must preserve the unsent draft"); }
            catch (IllegalStateException expected) { }
        }
    }

    @Test public void unboundIdleClickCanJoinTheFirstAcceptedClickInOrder() throws Exception {
        AIAssistantSubmission binding = AIAssistantSubmission.capture("session-one", null,
            runtime(false, ""));
        assertEquals("steer", binding.bind(runtime(true, "turn-first")));
        assertEquals("turn-first", binding.turnId());
        try { binding.bind(runtime(false, "")); fail("A once-bound supplement must not fall back"); }
        catch (IllegalStateException expected) { }
    }

    @Test public void captureCopiesTheRemoteOwnerAndSeparatesEqualTextClicks() throws Exception {
        JSONObject owner = new JSONObject().put("id", "session-one").put("device_slot", "phone")
            .put("login_session_id", "login-one").put("busy", true).put("turnId", "turn-one");
        AIAssistantSubmission first = AIAssistantSubmission.capture("session-one", owner, owner);
        AIAssistantSubmission second = AIAssistantSubmission.capture("session-one", owner, owner);
        owner.put("id", "session-two").put("turnId", "turn-two");
        assertEquals("session-one", first.remoteOwner().getString("id"));
        assertEquals("turn-one", first.turnId());
        assertNotEquals(first.requestId, second.requestId);
        JSONObject copy = first.remoteOwner(); copy.put("id", "mutated");
        assertEquals("session-one", first.remoteOwner().getString("id"));
    }

    @Test public void stopTargetsOnlyTheCapturedActiveTurn() throws Exception {
        AIAssistantSubmission binding = AIAssistantSubmission.capture("session-one", null,
            runtime(true, "turn-one"));
        assertTrue(binding.canStop(runtime(true, "turn-one")));
        assertFalse(binding.canStop(runtime(true, "turn-two")));
        assertFalse(binding.canStop(runtime(false, "")));
    }

    @Test public void nativeDispatchCarriesDocumentAndAudioPathsAlongsideVisualInputs() throws Exception {
        JSONObject payload = new JSONObject().put("session", "session-one").put("text", "Summarize these files")
            .put("quoteMessageId", "quote-one").put("files", new JSONArray()
                .put(new JSONObject().put("path", "docs/notes.txt").put("mimeType", "text/plain"))
                .put(new JSONObject().put("path", "audio/meeting.wav").put("mimeType", "audio/wav"))
                .put(new JSONObject().put("path", "photos/chart.jpg").put("mimeType", "image/jpeg").put("image", true))
                .put(new JSONObject().put("path", "clips/demo.mp4").put("mimeType", "video/mp4")));
        JSONObject request = AIAssistantSubmission.localRequest(payload, "steer", "turn-one", "request-one");
        assertTrue("The actual model prompt must retain the document's usable workspace path",
            request.getString("text").contains("docs/notes.txt"));
        assertTrue("Audio needs an ordinary file/tool input path too", request.getString("text").contains("audio/meeting.wav"));
        assertTrue(request.getString("text").startsWith("Summarize these files"));
        assertEquals("photos/chart.jpg", request.getJSONArray("images").getJSONObject(0).getString("path"));
        assertEquals("clips/demo.mp4", request.getJSONArray("videos").getJSONObject(0).getString("path"));
        assertEquals("quote-one", request.getString("quoteMessageId"));
        assertEquals("turn-one", request.getString("turnId"));
        assertEquals("request-one", request.getString("requestId"));
    }
}
