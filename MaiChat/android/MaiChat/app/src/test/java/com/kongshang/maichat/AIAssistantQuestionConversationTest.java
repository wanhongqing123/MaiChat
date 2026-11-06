// Question tools share the transcript rendering path without adding model messages.
package com.kongshang.maichat;

import static org.junit.Assert.*;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;

public final class AIAssistantQuestionConversationTest {
    private JSONObject question(String state) throws Exception {
        return new JSONObject().put("id", "part-question").put("kind", "tool")
            .put("tool", "question").put("state", state).put("input", new JSONObject()
                .put("question", "Which workspace should I use?")
                .put("options", new JSONArray().put("Keep current").put("Use project folder")).toString());
    }
    @Test public void pendingQuestionAndOptionsAreOrdinaryTextInsteadOfCollapsedToolControls() throws Exception {
        JSONObject source = question("running");
        JSONArray visible = AIAssistantQuestionConversation.parts(new JSONArray().put(source));
        assertEquals(1, visible.length());
        JSONObject text = visible.getJSONObject(0);
        assertEquals("text", text.getString("kind"));
        assertEquals("part-question", text.getString("id"));
        assertEquals("Which workspace should I use?\n\n- Keep current\n- Use project folder", text.getString("text"));
        assertEquals("tool", source.getString("kind"));
        assertFalse(source.has("text"));
    }
    @Test public void completedAnswerIsReplayedOnceAtItsOriginalQuestionPosition() throws Exception {
        JSONObject source = question("completed").put("output", "Use the app folder");
        JSONObject after = new JSONObject().put("id", "after").put("kind", "text").put("text", "Continuing now");
        JSONArray visible = AIAssistantQuestionConversation.parts(new JSONArray().put(source).put(after));
        assertEquals(3, visible.length());
        assertEquals("part-question:answer", visible.getJSONObject(1).getString("id"));
        assertEquals("question_answer", visible.getJSONObject(1).getString("kind"));
        assertEquals("Use the app folder", visible.getJSONObject(1).getString("text"));
        assertEquals("after", visible.getJSONObject(2).getString("id"));
        assertEquals(visible.toString(), AIAssistantQuestionConversation.parts(new JSONArray().put(source).put(after)).toString());
    }
    @Test public void canceledOrPartialQuestionNeverInventsAUserAnswer() throws Exception {
        JSONArray canceled = AIAssistantQuestionConversation.parts(new JSONArray().put(question("failed").put("output", "ignored")));
        assertEquals(1, canceled.length());
        JSONObject partial = question("running").put("input", "{\"question\":");
        assertEquals("tool", AIAssistantQuestionConversation.parts(new JSONArray().put(partial)).getJSONObject(0).getString("kind"));
    }
    @Test public void childQuestionHistoryMergesWithoutChangingRootPagingOrDuplicatingPendingText() throws Exception {
        JSONObject root = new JSONObject().put("id", "root").put("created", 10).put("parts", new JSONArray());
        JSONObject child = new JSONObject().put("id", "child").put("sessionId", "child-session")
            .put("questionProjection", true).put("created", 20).put("parts", new JSONArray().put(question("running")));
        JSONArray roots = new JSONArray().put(root);
        JSONArray pending = new JSONArray().put(new JSONObject().put("id", "question-one").put("partId", "part-question")
            .put("question", "Which workspace should I use?").put("asked", 30));
        JSONArray merged = AIAssistantQuestionConversation.messages(roots, new JSONArray().put(child), pending);
        assertEquals(2, merged.length());
        assertEquals("root", merged.getJSONObject(0).getString("id"));
        assertEquals("child", merged.getJSONObject(1).getString("id"));
        assertEquals(1, roots.length());
        assertFalse(root.has("questionProjection"));
    }
    @Test public void pendingQuestionStillAppearsAsNormalTextBeforeItsOwnerMessageArrives() throws Exception {
        JSONObject pending = new JSONObject().put("id", "question-one").put("partId", "part-question")
            .put("sessionId", "child-session").put("question", "Choose a folder")
            .put("options", new JSONArray().put("Current")).put("asked", 30);
        JSONArray merged = AIAssistantQuestionConversation.messages(new JSONArray(), new JSONArray(), new JSONArray().put(pending));
        assertEquals(1, merged.length());
        JSONObject row = merged.getJSONObject(0);
        assertTrue(row.getBoolean("questionProjection"));
        assertEquals("assistant", row.getString("role"));
        assertEquals("text", row.getJSONArray("parts").getJSONObject(0).getString("kind"));
        assertEquals("Choose a folder\n\n- Current", row.getJSONArray("parts").getJSONObject(0).getString("text"));
    }

    @Test public void publishedStateRetainsSeparateChildQuestionMessagesWithoutAddingRootRows() throws Exception {
        JSONArray root = new JSONArray().put(new JSONObject().put("id", "root"));
        JSONArray projected = new JSONArray().put(new JSONObject().put("id", "child").put("questionProjection", true));
        JSONObject page = new JSONObject().put("messages", root).put("questionMessages", projected)
            .put("questions", new JSONArray()).put("permissions", new JSONArray()).put("busy", true).put("turnId", "turn-one");
        JSONObject publication = AndroidSessionSyncCoordinator.publishPayload("session-one", root, page);
        assertEquals(root.toString(), publication.getJSONArray("messages").toString());
        assertEquals(projected.toString(), publication.getJSONArray("questionMessages").toString());
        assertEquals(1, root.length());
        assertTrue(publication.getBoolean("busy"));
        assertEquals("turn-one", publication.getString("turnId"));
        JSONObject legacy = AndroidSessionSyncCoordinator.publishPayload("session-one", root, new JSONObject());
        assertEquals(0, legacy.getJSONArray("questionMessages").length());
    }

    @Test public void acknowledgedAnswerStaysVisibleUntilCanonicalToolOutputTakesOverTheSameDisplayId() throws Exception {
        JSONObject pending = new JSONObject().put("id", "question-one").put("partId", "part-question")
            .put("question", "Which workspace should I use?").put("asked", 20);
        AIAssistantQuestionEchoes echoes = new AIAssistantQuestionEchoes();
        JSONObject payload = new JSONObject().put("question", pending).put("questionId", "question-one")
            .put("text", "Use the app folder");
        echoes.acknowledge("owner-one", payload);
        JSONArray roots = new JSONArray().put(new JSONObject().put("id", "root").put("parts", new JSONArray().put(question("running"))));
        JSONArray waiting = echoes.visible("owner-one", roots, new JSONArray());
        assertEquals(1, waiting.length());
        JSONArray parts = AIAssistantQuestionConversation.parts(roots.getJSONObject(0).getJSONArray("parts"), waiting);
        assertEquals("part-question:answer", parts.getJSONObject(1).getString("id"));
        assertEquals("Use the app folder", parts.getJSONObject(1).getString("text"));
        assertEquals(0, echoes.visible("owner-two", roots, new JSONArray()).length());
        JSONArray completed = new JSONArray().put(new JSONObject().put("id", "root")
            .put("parts", new JSONArray().put(question("completed").put("output", "Use the app folder"))));
        assertEquals(0, echoes.visible("owner-one", completed, new JSONArray()).length());
        JSONArray canonical = AIAssistantQuestionConversation.parts(completed.getJSONObject(0).getJSONArray("parts"));
        assertEquals(parts.getJSONObject(1).getString("id"), canonical.getJSONObject(1).getString("id"));
    }

    @Test public void childRuntimeChangesPublishTheirActualRootRoom() throws Exception {
        JSONObject child = new JSONObject().put("rootSessionId", "root-session");
        assertEquals("root-session", AIAssistantController.publishedSessionId("child-session", child));
        assertEquals("root-session", AIAssistantController.publishedSessionId("root-session", new JSONObject()));
    }
    @Test public void synchronizedHistoryRetainsOnlyQuestionToolTextForOrdinaryTranscriptReplay() throws Exception {
        JSONObject question = question("completed").put("output", "Project");
        JSONObject shell = new JSONObject().put("kind", "tool").put("tool", "shell")
            .put("input", "private-command").put("output", "private-output").put("error", "private-error");
        JSONObject history = new JSONObject().put("messages", new JSONArray().put(new JSONObject()
            .put("parts", new JSONArray().put(question).put(shell))));
        AIAssistantController.sanitizeHistoryForSync(history);
        assertTrue(question.has("input"));
        assertEquals("Project", question.getString("output"));
        assertFalse(shell.has("input")); assertFalse(shell.has("output")); assertFalse(shell.has("error"));
    }

    @Test public void largeRootPublicationTrimsOnlyOldestQuestionProjectionsToFitTheFrame() throws Exception {
        String body = "x".repeat(47 * 1024);
        JSONArray roots = new JSONArray().put(new JSONObject().put("id", "root").put("text", body));
        JSONArray projection = new JSONArray().put(new JSONObject().put("id", "old").put("text", "y".repeat(24 * 1024)))
            .put(new JSONObject().put("id", "new").put("text", "z".repeat(20 * 1024)));
        JSONObject page = new JSONObject().put("questionMessages", projection)
            .put("questions", new JSONArray().put(new JSONObject().put("question", "q".repeat(15 * 1024))));
        JSONObject publication = AndroidSessionSyncCoordinator.publishPayload("session-one", roots, page);
        assertTrue(publication.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8).length <= 95 * 1024);
        assertEquals(body, publication.getJSONArray("messages").getJSONObject(0).getString("text"));
        assertEquals(1, publication.getJSONArray("questionMessages").length());
        assertEquals("new", publication.getJSONArray("questionMessages").getJSONObject(0).getString("id"));
        assertEquals(2, projection.length());
        // The immediate state-only frame can restore the full child projection without altering root paging.
        JSONObject stateOnly = AndroidSessionSyncCoordinator.publishPayload("session-one", new JSONArray(), page);
        assertEquals(2, stateOnly.getJSONArray("questionMessages").length());
    }

    @Test public void childQuestionKeepsItsPositionBetweenParentPartsWithoutChangingMessageOwnership() throws Exception {
        JSONObject before = new JSONObject().put("id", "part-before").put("created", 10).put("kind", "text").put("text", "Before child");
        JSONObject after = new JSONObject().put("id", "part-after").put("created", 30).put("kind", "text").put("text", "After child");
        JSONObject root = new JSONObject().put("id", "root-message").put("created", 1).put("role", "assistant")
            .put("parts", new JSONArray().put(before).put(after));
        JSONObject child = new JSONObject().put("id", "child-message").put("created", 2).put("role", "assistant")
            .put("questionProjection", true).put("parts", new JSONArray().put(question("completed").put("created", 20).put("output", "Use folder A")));
        JSONArray rows = AIAssistantQuestionConversation.messages(new JSONArray().put(root), new JSONArray().put(child), new JSONArray());
        assertEquals(3, rows.length());
        assertEquals("part-before", rows.getJSONObject(0).getJSONArray("parts").getJSONObject(0).getString("id"));
        assertEquals("child-message", rows.getJSONObject(1).getString("id"));
        assertEquals("part-after", rows.getJSONObject(2).getJSONArray("parts").getJSONObject(0).getString("id"));
        assertEquals("root-message", rows.getJSONObject(2).getString("id"));
        assertEquals(2, root.getJSONArray("parts").length());
        assertNotEquals(rows.getJSONObject(0).optString("displayId"), rows.getJSONObject(2).optString("displayId"));
        JSONArray questionAndAnswer = AIAssistantQuestionConversation.parts(rows.getJSONObject(1).getJSONArray("parts"));
        assertEquals("question_answer", questionAndAnswer.getJSONObject(1).getString("kind"));
    }

    @Test public void waitingQuestionSuppressesThinkingMetadataOnlyOnItsOwnFragment() throws Exception {
        JSONObject pending = new JSONObject().put("id", "question-one").put("partId", "part-question");
        JSONObject own = new JSONObject().put("parts", new JSONArray().put(question("running")));
        JSONObject other = new JSONObject().put("parts", new JSONArray().put(new JSONObject().put("id", "other-part")));
        assertTrue(AIAssistantQuestionConversation.waiting(own, new JSONArray().put(pending)));
        assertFalse(AIAssistantQuestionConversation.waiting(other, new JSONArray().put(pending)));
        assertFalse(AIAssistantQuestionConversation.waiting(own, new JSONArray()));
    }

    @Test public void copyReplyIncludesQuestionOptionsAndTheRealAnswer() throws Exception {
        JSONObject message = new JSONObject().put("parts", new JSONArray().put(question("completed").put("output", "Use folder A")));
        String copied = AIAssistantQuestionConversation.copyText(message);
        assertTrue(copied.contains("Which workspace should I use?"));
        assertTrue(copied.contains("Keep current"));
        assertTrue(copied.endsWith("Use folder A"));
    }

    @Test public void ordinaryStreamedTextCopyKeepsItsExistingExactConcatenation() throws Exception {
        JSONObject message = new JSONObject().put("parts", new JSONArray()
            .put(new JSONObject().put("id", "text-one").put("kind", "text").put("text", "Hel"))
            .put(new JSONObject().put("id", "text-two").put("kind", "text").put("text", "lo")));
        assertEquals("Hello", AIAssistantQuestionConversation.copyText(message));
    }

    @Test public void legacyCoreTwentyOneCharacterIdsRecoverPartOrderWhenCreatedIsZeroOrMissing() throws Exception {
        String beforeId = "prt_" + "000000000A" + "000" + "00000000";
        String questionId = "prt_" + "000000000M" + "000" + "00000000";
        String afterId = "prt_" + "000000000Y" + "000" + "00000000";
        JSONObject before = new JSONObject().put("id", beforeId).put("created", 0).put("kind", "text").put("text", "Before child");
        JSONObject after = new JSONObject().put("id", afterId).put("kind", "text").put("text", "After child");
        JSONObject root = new JSONObject().put("id", "root-message").put("role", "assistant").put("created", 1)
            .put("parts", new JSONArray().put(before).put(after));
        JSONObject child = new JSONObject().put("id", "child-message").put("role", "assistant").put("created", 2)
            .put("questionProjection", true).put("parts", new JSONArray().put(question("running").put("id", questionId)));
        JSONArray rows = AIAssistantQuestionConversation.messages(new JSONArray().put(root), new JSONArray().put(child), new JSONArray());
        assertEquals(3, rows.length());
        assertEquals(beforeId, rows.getJSONObject(0).getJSONArray("parts").getJSONObject(0).getString("id"));
        assertEquals(questionId, rows.getJSONObject(1).getJSONArray("parts").getJSONObject(0).getString("id"));
        assertEquals(afterId, rows.getJSONObject(2).getJSONArray("parts").getJSONObject(0).getString("id"));
    }

    @Test public void unknownOldRootDisplayUpdateShowsOnlyRecentPartsAndItsNewQuestionAnswer() throws Exception {
        JSONArray roots = new JSONArray().put(new JSONObject().put("id", "recent-user").put("role", "user")
            .put("created", 15).put("parts", new JSONArray().put(new JSONObject().put("kind", "text").put("text", "Recent input"))));
        JSONObject old = new JSONObject().put("id", "old-root").put("role", "assistant").put("created", 1)
            .put("parts", new JSONArray().put(new JSONObject().put("id", "old-text").put("created", 10)
                .put("kind", "text").put("text", "Do not replay this old preamble"))
                .put(question("completed").put("created", 20).put("output", "Use folder A"))
                .put(new JSONObject().put("id", "new-text").put("created", 30).put("kind", "text").put("text", "New progress")));
        JSONArray rows = AIAssistantQuestionConversation.messages(roots, new JSONArray(), new JSONArray(), new JSONArray(), new JSONArray().put(old));
        assertEquals(2, rows.length());
        assertEquals("recent-user", rows.getJSONObject(0).getString("id"));
        assertEquals("old-root", rows.getJSONObject(1).getString("id"));
        assertEquals(2, rows.getJSONObject(1).getJSONArray("parts").length());
        assertEquals(3, old.getJSONArray("parts").length());
        assertFalse(rows.getJSONObject(1).getJSONArray("parts").toString().contains("Do not replay"));
        assertTrue(AIAssistantQuestionConversation.copyText(rows.getJSONObject(1)).contains("Use folder A"));
    }

    @Test public void producerPublishesDirtyOldOwnerUpdatesWithoutUsingThemAsPagingRows() throws Exception {
        JSONObject page = new JSONObject().put("messages", new JSONArray().put(new JSONObject().put("id", "msg_new").put("text", "page")))
            .put("messageUpdates", new JSONArray().put(new JSONObject().put("id", "msg_old").put("text", "dirty"))
                .put(new JSONObject().put("id", "msg_new").put("text", "newer dirty")));
        JSONArray publications = AndroidSessionSyncCoordinator.publicationMessages(page);
        assertEquals(2, publications.length());
        assertEquals("newer dirty", publications.getJSONObject(0).getString("text"));
        assertEquals("msg_old", publications.getJSONObject(1).getString("id"));
        assertEquals(1, page.getJSONArray("messages").length());
        assertEquals("page", page.getJSONArray("messages").getJSONObject(0).getString("text"));
    }

    @Test public void knownRootQuestionKeepsItsNewPartsAfterInterveningUserSteering() throws Exception {
        JSONObject root = new JSONObject().put("id", "root").put("role", "assistant").put("created", 1)
            .put("parts", new JSONArray().put(new JSONObject().put("id", "before").put("kind", "text").put("created", 10).put("text", "Before"))
                .put(question("running").put("created", 20)));
        JSONObject user = new JSONObject().put("id", "user").put("role", "user").put("created", 15)
            .put("parts", new JSONArray().put(new JSONObject().put("id", "input").put("kind", "text").put("text", "New input")));
        JSONArray rows = AIAssistantQuestionConversation.messages(new JSONArray().put(root).put(user), new JSONArray(), new JSONArray());
        assertEquals(3, rows.length());
        assertEquals("root", rows.getJSONObject(0).getString("id"));
        assertEquals("user", rows.getJSONObject(1).getString("id"));
        assertEquals("part-question", rows.getJSONObject(2).getJSONArray("parts").getJSONObject(0).getString("id"));
    }

    @Test public void canonicalAnswerInTheOldOwnerDisplayLaneAlsoRetiresItsAcknowledgedEcho() throws Exception {
        AIAssistantQuestionEchoes echoes = new AIAssistantQuestionEchoes();
        JSONObject pending = new JSONObject().put("id", "question-one").put("partId", "part-question");
        echoes.acknowledge("owner", new JSONObject().put("question", pending).put("questionId", "question-one").put("text", "Answer"));
        JSONArray updates = new JSONArray().put(new JSONObject().put("id", "old-owner")
            .put("parts", new JSONArray().put(question("completed").put("output", "Answer"))));
        assertEquals(0, echoes.visible("owner", new JSONArray(), new JSONArray(), updates).length());
    }

    @Test public void failedPublishRetainsTheAlreadyConsumedOldOwnerUpdateUntilTheSamePageIsAccepted() throws Exception {
        AIAssistantPublishPages pages = new AIAssistantPublishPages();
        JSONObject captured = new JSONObject().put("messages", new JSONArray())
            .put("messageUpdates", new JSONArray().put(new JSONObject().put("id", "old-owner").put("text", "Exact update")));
        pages.remember("session-one", "", captured);
        captured.getJSONArray("messageUpdates").getJSONObject(0).put("text", "changed elsewhere");
        assertEquals("Exact update", pages.page("session-one", "").getJSONArray("messageUpdates").getJSONObject(0).getString("text"));
        assertNull(pages.page("session-two", ""));
        assertNull(pages.page("session-one", "old-cursor"));
        pages.complete("session-one", "");
        assertNull(pages.page("session-one", ""));
    }

}
