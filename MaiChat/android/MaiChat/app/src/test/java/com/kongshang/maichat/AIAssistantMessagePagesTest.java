// 文件作用：用合成消息验证原生真分页请求、旧页保留、迟到回执隔离和流式更新。
package com.kongshang.maichat;

import static org.junit.Assert.*;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;

public final class AIAssistantMessagePagesTest {
    private static JSONObject message(int number, String text) throws Exception {
        return new JSONObject().put("id", String.format(java.util.Locale.ROOT, "msg_%04d", number))
            .put("role", "assistant").put("completed", 1)
            .put("parts", new JSONArray().put(new JSONObject().put("id", "part_" + number)
                .put("kind", "text").put("text", text)));
    }
    private static JSONArray descending(int newest, int count) throws Exception {
        JSONArray result = new JSONArray();
        for (int i = newest; i > newest - count; i--) result.put(message(i, "text " + i));
        return result;
    }
    private static JSONObject snapshot(int newest, int count) throws Exception {
        return new JSONObject().put("messages", descending(newest, count));
    }
    @Test public void snapshotAndOlderRequestsUseFiniteNativePagesAndFixedMetadata() throws Exception {
        AIAssistantMessagePages pages = new AIAssistantMessagePages();
        AIAssistantMessagePages.Ticket ticket = pages.select("local:one");
        JSONObject request = pages.snapshotRequest("one", true);
        assertEquals("snapshot", request.getString("op"));
        assertEquals(8, request.getInt("messageLimit"));
        assertTrue(request.getBoolean("includeMessageUpdates"));
        assertFalse(request.has("retainedMessageIds"));
        assertTrue(pages.acceptSnapshot(ticket, snapshot(48, 8)));
        AIAssistantMessagePages.Ticket older = pages.beginOlder();
        JSONObject page = pages.pageRequest("one", older);
        assertEquals("messages_page", page.getString("op"));
        assertEquals("msg_0041", page.getString("before"));
        assertEquals(20, page.getInt("limit"));
        assertNull("Only one older page can be outstanding", pages.beginOlder());
        assertTrue(pages.acceptOlder(older, descending(40, 20)));
        assertEquals("Request metadata must not grow with loaded history", request.length(),
            pages.snapshotRequest("one", true).length());
    }
    @Test public void olderPagesReachTheEarliestAndSurviveLatestWindowRefreshes() throws Exception {
        AIAssistantMessagePages pages = new AIAssistantMessagePages();
        AIAssistantMessagePages.Ticket current = pages.select("local:one");
        pages.acceptSnapshot(current, snapshot(48, 8));
        assertTrue(pages.hasOlder());
        pages.acceptOlder(pages.beginOlder(), descending(40, 20));
        pages.acceptOlder(pages.beginOlder(), descending(20, 20));
        assertEquals(48, pages.messages().length());
        pages.acceptOlder(pages.beginOlder(), new JSONArray());
        assertFalse(pages.hasOlder());
        pages.acceptSnapshot(current, snapshot(49, 8));
        assertEquals("msg_0001", pages.messages().getJSONObject(0).getString("id"));
        assertEquals(49, pages.messages().length());
        assertFalse("A full latest page does not imply unvisited old history", pages.hasOlder());
    }
    @Test public void loadedOldAssistantReceivesUpdatesAfterItLeavesTheLatestWindow() throws Exception {
        AIAssistantMessagePages pages = new AIAssistantMessagePages();
        AIAssistantMessagePages.Ticket current = pages.select("local:one");
        pages.acceptSnapshot(current, snapshot(8, 8));
        long original = pages.messages().getJSONObject(0).getLong("uiRevision");
        pages.acceptSnapshot(current, snapshot(18, 8).put("messageUpdates",
            new JSONArray().put(message(1, "late streaming text"))));
        JSONObject oldest = pages.messages().getJSONObject(0);
        assertEquals("late streaming text", oldest.getJSONArray("parts").getJSONObject(0).getString("text"));
        assertNotEquals(original, oldest.getLong("uiRevision"));
        long updated = oldest.getLong("uiRevision");
        pages.acceptSnapshot(current, new JSONObject().put("messages", new JSONArray())
            .put("messageUpdates", new JSONArray().put(message(1, "late streaming text"))));
        assertEquals("Identical snapshots must not reparse a row's tool detail", updated,
            pages.messages().getJSONObject(0).getLong("uiRevision"));
    }
    @Test public void pageRepliesCannotCrossOwnersOrReturnAfterLeavingAndReentering() throws Exception {
        AIAssistantMessagePages pages = new AIAssistantMessagePages();
        AIAssistantMessagePages.Ticket first = pages.select("local:one");
        pages.acceptSnapshot(first, snapshot(30, 8));
        AIAssistantMessagePages.Ticket oldRequest = pages.beginOlder();
        AIAssistantMessagePages.Ticket second = pages.select("remote:phone:two");
        pages.acceptSnapshot(second, snapshot(50, 3));
        assertFalse(pages.acceptOlder(oldRequest, descending(22, 20)));
        assertEquals(3, pages.messages().length());
        AIAssistantMessagePages.Ticket returned = pages.select("local:one");
        assertFalse(pages.acceptSnapshot(first, snapshot(31, 8)));
        assertTrue(pages.acceptSnapshot(returned, snapshot(32, 8)));
        assertEquals(8, pages.messages().length());
    }
    @Test public void pageFailureKeepsLoadedRowsAndAllowsTheSameCursorToRetry() throws Exception {
        AIAssistantMessagePages pages = new AIAssistantMessagePages();
        AIAssistantMessagePages.Ticket current = pages.select("local:one");
        pages.acceptSnapshot(current, snapshot(30, 8));
        AIAssistantMessagePages.Ticket failed = pages.beginOlder();
        String before = pages.pageRequest("one", failed).getString("before");
        pages.failOlder(failed);
        assertFalse(pages.loading());
        assertEquals(8, pages.messages().length());
        assertEquals(before, pages.pageRequest("one", pages.beginOlder()).getString("before"));
        pages.reset();
        assertFalse(pages.acceptSnapshot(current, snapshot(31, 8)));
    }
    @Test public void aBroadcastBeforeTheInitialHistoryReplyDoesNotHideOlderHistory() throws Exception {
        AIAssistantMessagePages pages = new AIAssistantMessagePages();
        AIAssistantMessagePages.Ticket current = pages.select("remote:one");
        pages.acceptSnapshot(current, snapshot(40, 1));
        assertFalse(pages.hasOlder());
        pages.acceptSnapshot(current, snapshot(39, 8).put("initialHistoryPage", true));
        assertTrue(pages.hasOlder());
        assertEquals(9, pages.messages().length());
        pages.acceptOlder(pages.beginOlder(), new JSONArray());
        pages.acceptSnapshot(current, snapshot(40, 8).put("initialHistoryPage", true));
        assertFalse("An initial-page retry must not reset the older-page cursor", pages.hasOlder());
    }
    @Test public void anOldActiveMessageUpdateDoesNotMoveTheContiguousPageCursor() throws Exception {
        AIAssistantMessagePages pages = new AIAssistantMessagePages();
        AIAssistantMessagePages.Ticket current = pages.select("local:one");
        pages.acceptSnapshot(current, snapshot(20, 8).put("messageUpdates",
            new JSONArray().put(message(5, "updated old assistant"))));
        assertEquals("msg_0005", pages.messages().getJSONObject(0).getString("id"));
        AIAssistantMessagePages.Ticket older = pages.beginOlder();
        assertEquals("Updates outside the latest window must not skip the unvisited gap",
            "msg_0013", pages.pageRequest("one", older).getString("before"));
        pages.acceptOlder(older, descending(12, 12));
        assertEquals(20, pages.messages().length());
        for (int i = 0; i < 20; i++)
            assertEquals(String.format(java.util.Locale.ROOT, "msg_%04d", i + 1),
                pages.messages().getJSONObject(i).getString("id"));
        assertEquals("A historical row must not overwrite a newer dirty update", "updated old assistant",
            pages.messages().getJSONObject(4).getJSONArray("parts").getJSONObject(0).getString("text"));
        assertFalse(pages.hasOlder());
    }
    @Test public void onlyHistoricalPagesAdvanceTheCursorAcrossFurtherDirtyUpdates() throws Exception {
        AIAssistantMessagePages pages = new AIAssistantMessagePages();
        AIAssistantMessagePages.Ticket current = pages.select("local:one");
        pages.acceptSnapshot(current, snapshot(48, 8));
        pages.acceptSnapshot(current, new JSONObject().put("messages", new JSONArray())
            .put("messageUpdates", new JSONArray().put(message(5, "old streaming"))));
        AIAssistantMessagePages.Ticket first = pages.beginOlder();
        assertEquals("msg_0041", pages.pageRequest("one", first).getString("before"));
        pages.acceptOlder(first, descending(40, 20));
        pages.acceptSnapshot(current, snapshot(49, 8).put("messageUpdates",
            new JSONArray().put(message(1, "older streaming"))));
        AIAssistantMessagePages.Ticket second = pages.beginOlder();
        assertEquals("msg_0021", pages.pageRequest("one", second).getString("before"));
        pages.acceptOlder(second, descending(20, 20));
        assertEquals(49, pages.messages().length());
        assertEquals("msg_0001", pages.pageRequest("one", pages.beginOlder()).getString("before"));
    }
    @Test public void remoteOldMessageUpdateUsesABoundedDisplayLaneUntilItsRealHistoryPageArrives() throws Exception {
        AIAssistantMessagePages pages = new AIAssistantMessagePages(true);
        AIAssistantMessagePages.Ticket current = pages.select("remote:one");
        pages.acceptSnapshot(current, snapshot(20, 8).put("initialHistoryPage", true));
        pages.acceptSnapshot(current, new JSONObject().put("messages", new JSONArray().put(message(5, "new answer"))));
        assertEquals(8, pages.messages().length());
        assertEquals("msg_0005", pages.displayUpdates().getJSONObject(0).getString("id"));
        AIAssistantMessagePages.Ticket older = pages.beginOlder();
        assertEquals("msg_0013", pages.pageRequest("one", older).getString("before"));
        pages.acceptOlder(older, descending(12, 12));
        assertEquals(20, pages.messages().length());
        assertEquals(0, pages.displayUpdates().length());
        assertEquals("new answer", pages.messages().getJSONObject(4).getJSONArray("parts").getJSONObject(0).getString("text"));
    }
    @Test public void remoteUnknownOldUpdatesNeverGrowTheContinuousRootWindow() throws Exception {
        AIAssistantMessagePages pages = new AIAssistantMessagePages(true);
        AIAssistantMessagePages.Ticket current = pages.select("remote:one");
        pages.acceptSnapshot(current, snapshot(1000, 8));
        for (int i = 1; i <= 25; i++) pages.acceptSnapshot(current,
            new JSONObject().put("messages", new JSONArray().put(message(i, "old owner update"))));
        assertEquals(8, pages.messages().length());
        assertEquals(20, pages.displayUpdates().length());
        assertEquals("msg_0993", pages.pageRequest("one", pages.beginOlder()).getString("before"));
        pages.select("remote:two");
        assertEquals(0, pages.displayUpdates().length());
    }

    @Test public void initialRemotePageMovesAnEarlierOldOwnerBroadcastToTheDisplayLane() throws Exception {
        AIAssistantMessagePages pages = new AIAssistantMessagePages(true);
        AIAssistantMessagePages.Ticket current = pages.select("remote:one");
        pages.acceptSnapshot(current, new JSONObject().put("messages", new JSONArray().put(message(5, "old owner broadcast"))));
        pages.acceptSnapshot(current, snapshot(20, 8).put("initialHistoryPage", true));
        assertEquals(8, pages.messages().length());
        assertEquals(1, pages.displayUpdates().length());
        assertEquals("msg_0013", pages.pageRequest("one", pages.beginOlder()).getString("before"));
    }

}
