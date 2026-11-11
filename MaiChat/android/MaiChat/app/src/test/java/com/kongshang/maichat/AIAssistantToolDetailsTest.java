// 文件作用：验证工具详情只读分页的来源、原调用归属和迟到回执隔离。
package com.kongshang.maichat;

import static org.junit.Assert.*;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;

public final class AIAssistantToolDetailsTest {
    private static final String REVISION = "a".repeat(64), NEW_REVISION = "b".repeat(64);
    private JSONObject owner() throws Exception {
        return new JSONObject().put("id", "session").put("device_slot", "mac")
            .put("login_session_id", "login").put("online", true);
    }
    private JSONObject part(String revision) throws Exception {
        return new JSONObject().put("id", "part").put("kind", "tool").put("tool", "shell")
            .put("callId", "call").put("state", "running").put("detailsAvailable", true)
            .put("detailRevision", revision);
    }
    private JSONObject envelope() throws Exception {
        return new JSONObject().put("session_id", "session").put("from_slot", "mac")
            .put("from_login_session_id", "login");
    }
    private JSONObject response(AIAssistantToolDetails.Ticket ticket, String text, long total, boolean more) throws Exception {
        long offset = ticket.request.getLong("offset");
        long bytes = text.getBytes(java.nio.charset.StandardCharsets.UTF_8).length;
        return new JSONObject().put("ok", true).put("requestId", ticket.request.getString("requestId"))
            .put("sessionId", "session").put("messageId", "message").put("partId", "part")
            .put("callId", "call").put("tool", "shell").put("state", "running")
            .put("field", ticket.request.getString("field")).put("detailRevision", REVISION)
            .put("offset", offset).put("sourceBytesRead", bytes).put("totalBytes", total)
            .put("nextOffset", more ? offset + bytes : 0).put("hasMore", more)
            .put("text", text).put("textIsValidUtf8", true);
    }
    private AIAssistantToolDetails selected() throws Exception {
        AIAssistantToolDetails details = new AIAssistantToolDetails();
        details.select("account", 7, "session", owner());
        return details;
    }
    private JSONObject field(AIAssistantToolDetails details) throws Exception {
        return details.snapshot().getJSONObject(AIAssistantToolDetails.key("message", "part")).getJSONObject("output");
    }

    @Test public void expandingReadsOneBoundedPageAndMoreUsesSourceBytes() throws Exception {
        AIAssistantToolDetails details = selected();
        AIAssistantToolDetails.Ticket first = details.begin("message", part(REVISION), "output", false, false);
        assertNotNull("Expanding metadata-only tools must issue a read", first);
        assertEquals(4096, first.request.getInt("limit"));
        assertEquals(0, first.request.getLong("offset"));
        details.bind(first, "command");
        assertTrue(details.accept("command", envelope(), response(first, "中文", 8, true)));
        assertEquals("中文", field(details).getString("text"));
        assertNull("Rebinding an expanded row cannot fetch the whole output", details.begin("message", part(REVISION), "output", false, false));
        AIAssistantToolDetails.Ticket next = details.begin("message", part(REVISION), "output", true, true);
        assertEquals(6, next.request.getLong("offset"));
        assertEquals(REVISION, next.request.getString("detailRevision"));
        details.bind(next, "next-command");
        assertTrue(details.accept("next-command", envelope(), response(next, "OK", 8, false)));
        assertEquals("中文OK", field(details).getString("text"));
        assertFalse(field(details).getBoolean("hasMore"));
    }

    @Test public void wrongAcknowledgmentDoesNotConsumeTheCorrectRequest() throws Exception {
        AIAssistantToolDetails details = selected();
        AIAssistantToolDetails.Ticket ticket = details.begin("message", part(REVISION), "output", false, false);
        details.bind(ticket, "command");
        JSONObject good = response(ticket, "actual output", 13, false);
        for (String name : new String[] {"requestId", "sessionId", "messageId", "partId", "callId", "tool", "field", "detailRevision"})
            assertFalse(name, details.accept("command", envelope(), new JSONObject(good.toString()).put(name, "wrong")));
        assertFalse(details.accept("other-command", envelope(), good));
        assertFalse(details.accept("command", new JSONObject(envelope().toString()).put("from_login_session_id", "new-login"), good));
        assertFalse(details.accept("command", envelope(), new JSONObject(good.toString()).put("offset", 1)));
        assertTrue("The correct ACK must still finish the original pending read", details.accept("command", envelope(), good));
        assertEquals("actual output", field(details).getString("text"));
    }

    @Test public void ownerAccountGenerationAndDetailRevisionRetirePendingPages() throws Exception {
        AIAssistantToolDetails details = selected();
        AIAssistantToolDetails.Ticket old = details.begin("message", part(REVISION), "output", false, false);
        details.bind(old, "old");
        details.observe("message", part(NEW_REVISION));
        assertFalse(details.accept("old", envelope(), response(old, "stale", 5, false)));
        assertFalse(field(details).has("text"));
        AIAssistantToolDetails.Ticket fresh = details.begin("message", part(NEW_REVISION), "output", false, false);
        assertEquals(0, fresh.request.getLong("offset"));
        details.bind(fresh, "fresh");
        details.select("account", 8, "session", owner());
        assertFalse(details.accept("fresh", envelope(), response(fresh, "stale", 5, false)));
        assertEquals(0, details.snapshot().length());
        details.select("other-account", 8, "session", owner());
        assertEquals(0, details.snapshot().length());
    }

    @Test public void failedReadKeepsCachedTextAndOnlyExplicitRetrySendsAgain() throws Exception {
        AIAssistantToolDetails details = selected();
        AIAssistantToolDetails.Ticket first = details.begin("message", part(REVISION), "output", false, false);
        details.bind(first, "first");
        assertTrue(details.accept("first", envelope(), response(first, "cached", 20, true)));
        AIAssistantToolDetails.Ticket next = details.begin("message", part(REVISION), "output", true, true);
        details.bind(next, "next");
        details.fail(next, "所属设备离线，已加载内容保留");
        assertEquals("cached", field(details).getString("text"));
        assertTrue(field(details).getString("error").contains("离线"));
        assertNull(details.begin("message", part(REVISION), "output", false, false));
        assertNotNull(details.begin("message", part(REVISION), "output", true, true));
    }

    @Test public void invalidPaginationCannotPretendToBeCompleteOrConsumePendingRead() throws Exception {
        AIAssistantToolDetails details = selected();
        AIAssistantToolDetails.Ticket ticket = details.begin("message", part(REVISION), "output", false, false);
        details.bind(ticket, "command");
        JSONObject good = response(ticket, "1234", 10, true);
        assertFalse(details.accept("command", envelope(), new JSONObject(good.toString()).put("nextOffset", 9)));
        assertFalse(details.accept("command", envelope(), new JSONObject(good.toString()).put("sourceBytesRead", 5000)));
        assertFalse(details.accept("command", envelope(), new JSONObject(good.toString()).put("hasMore", false)));
        assertTrue(details.accept("command", envelope(), good));
    }

    @Test public void ownerRequestAllowlistCannotInvokeOrConfigureAnything() throws Exception {
        AIAssistantToolDetails details = selected();
        AIAssistantToolDetails.Ticket ticket = details.begin("message", part(REVISION), "input", false, false);
        JSONObject nativeRequest = AIAssistantToolDetails.nativeRequest("session", ticket.request);
        assertEquals("tool_detail", nativeRequest.getString("op"));
        assertEquals("session", nativeRequest.getString("session"));
        assertFalse(nativeRequest.has("action"));
        for (JSONObject bad : new JSONObject[] {
                new JSONObject(ticket.request.toString()).put("command", "do not execute"),
                new JSONObject(ticket.request.toString()).put("offset", -1),
                new JSONObject(ticket.request.toString()).put("limit", 4097),
                new JSONObject(ticket.request.toString()).put("field", "text"),
                new JSONObject(ticket.request.toString()).put("offset", 4).put("detailRevision", "")}) {
            try { AIAssistantToolDetails.nativeRequest("session", bad); fail("Invalid read was accepted"); }
            catch (IllegalArgumentException expected) { }
        }
    }

    @Test public void metadataOnlyDisclosureShowsLoadingFailureAndCachedOriginalText() throws Exception {
        JSONObject metadata = part(REVISION);
        assertTrue(AIAssistantToolDetails.displayText(metadata, null).contains("参数"));
        assertTrue(AIAssistantToolDetails.displayText(metadata, null).contains("尚未加载"));
        assertTrue(AIAssistantToolDetails.displayText(metadata, new JSONObject()
            .put("output", new JSONObject().put("loading", true))).contains("正在读取"));
        JSONObject cached = new JSONObject().put("input", new JSONObject().put("text", "original command"))
            .put("output", new JSONObject().put("text", "cached output").put("error", "owner offline"))
            .put("error", new JSONObject().put("text", ""));
        String visible = AIAssistantToolDetails.displayText(metadata, cached);
        assertTrue(visible.contains("original command"));
        assertTrue(visible.contains("cached output"));
        assertTrue(visible.contains("owner offline"));
        assertTrue(visible.contains("（空）"));
        JSONObject legacy = new JSONObject().put("input", "old input").put("output", "old output").put("error", "old error");
        assertEquals("old input\nold output\nold error", AIAssistantToolDetails.displayText(legacy, null));
        assertTrue(AIAssistantToolDetails.displayText(new JSONObject(), null).contains("未提供"));
    }

    @Test public void busyInvalidatesAllFieldsAndRetriesFromZeroWithoutTheStaleRevision() throws Exception {
        AIAssistantToolDetails details = selected();
        AIAssistantToolDetails.Ticket input = details.begin("message", part(REVISION), "input", false, false);
        details.bind(input, "input");
        assertTrue(details.accept("input", envelope(), response(input, "old command", 11, false)));
        AIAssistantToolDetails.Ticket output = details.begin("message", part(REVISION), "output", false, false);
        details.bind(output, "output");
        JSONObject changed = AIAssistantToolDetails.failure("session", output.request, "busy", "detail_changed");
        assertTrue(details.accept("output", envelope(), changed));
        assertFalse(details.snapshot().getJSONObject(AIAssistantToolDetails.key("message", "part"))
            .getJSONObject("input").has("text"));
        AIAssistantToolDetails.Ticket retry = details.begin("message", part(REVISION), "output", false, true);
        assertEquals(0, retry.request.getLong("offset"));
        assertEquals("", retry.request.getString("detailRevision"));
        details.bind(retry, "retry");
        JSONObject fresh = response(retry, "new", 6, true).put("detailRevision", NEW_REVISION);
        assertFalse("An invalid first-page revision must leave the fresh read pending", details.accept("retry", envelope(),
            new JSONObject(fresh.toString()).put("detailRevision", "x")));
        assertTrue(details.accept("retry", envelope(), fresh));
        AIAssistantToolDetails.Ticket next = details.begin("message", part(REVISION), "output", true, true);
        assertEquals(NEW_REVISION, next.request.getString("detailRevision"));
        assertEquals(3, next.request.getLong("offset"));
    }

    @Test public void invalidUtf8UsesBoundOriginalBytesAndDoesNotPretendTheyAreUnicode() throws Exception {
        AIAssistantToolDetails details = selected();
        AIAssistantToolDetails.Ticket ticket = details.begin("message", part(REVISION), "output", false, false);
        details.bind(ticket, "command");
        JSONObject invalid = response(ticket, "?", 1, false).put("textIsValidUtf8", false);
        assertFalse(details.accept("command", envelope(), invalid));
        assertTrue(details.accept("command", envelope(), invalid.put("rawBase64", "/w==")));
        assertTrue(field(details).getString("text").contains("Base64"));
        assertTrue(field(details).getString("text").contains("/w=="));
    }

    @Test public void newCapabilityReadsAllThreeFieldsEvenWhenCurlKeepsASafeSummary() throws Exception {
        AIAssistantToolDetails details = selected();
        JSONObject curl = part(REVISION).put("tool", "curl").put("error", "curl request failed");
        assertFalse("A summary is not the advertised read-only detail body", AIAssistantToolDetails.hasInlineDetails(curl));
        String visible = AIAssistantToolDetails.displayText(curl, null);
        assertTrue(visible.contains("curl request failed"));
        assertTrue(visible.contains("参数"));
        assertTrue(visible.contains("尚未加载"));
        java.util.Set<String> reads = new java.util.HashSet<>();
        for (String name : AIAssistantToolDetails.FIELDS) {
            AIAssistantToolDetails.Ticket ticket = details.begin("message", curl, name, false, false);
            assertNotNull(ticket);
            reads.add(ticket.request.getString("field"));
        }
        assertEquals(3, reads.size());
    }

    @Test public void omittedOversizedSnapshotCallIdDoesNotRejectTheExactOriginalReply() throws Exception {
        for (int bytes : new int[] {129, 50 * 1024}) {
            AIAssistantToolDetails details = selected();
            JSONObject metadata = part(REVISION); metadata.remove("callId");
            String actualCall = "c".repeat(bytes);
            AIAssistantToolDetails.Ticket first = details.begin("message", metadata, "output", false, false);
            details.bind(first, "first");
            assertTrue("The message/part/revision-bound original call remains readable: " + bytes,
                details.accept("first", envelope(), response(first, "abcd", 8, true).put("callId", actualCall)));
            AIAssistantToolDetails.Ticket next = details.begin("message", metadata, "output", true, true);
            assertEquals("A verified original call ID binds subsequent pages", actualCall, next.call);
            details.bind(next, "next");
            assertFalse(details.accept("next", envelope(), response(next, "efgh", 8, false).put("callId", "wrong")));
            assertTrue(details.accept("next", envelope(), response(next, "efgh", 8, false).put("callId", actualCall)));
            assertEquals("abcdefgh", field(details).getString("text"));
        }
    }

    @Test public void omittedPrefixBeforeEofHasAnExplicitIndependentRestart() throws Exception {
        AIAssistantToolDetails details = selected();
        JSONObject metadata = part(REVISION);
        for (int index = 0; index < 17; index++) {
            AIAssistantToolDetails.Ticket ticket = details.begin("message", metadata, "output", index > 0, index > 0);
            details.bind(ticket, "page-" + index);
            assertTrue(details.accept("page-" + index, envelope(), response(ticket, "x".repeat(4096), 18 * 4096, true)));
        }
        assertTrue(field(details).getBoolean("hasMore"));
        assertTrue("The disclosure must offer restart independently of nextOffset", field(details).optBoolean("prefixOmitted"));
        AIAssistantToolDetails.Ticket restart = details.begin("message", metadata, "output", false, true);
        assertEquals(0, restart.request.getLong("offset"));
        details.bind(restart, "restart");
        assertTrue(details.accept("restart", envelope(), response(restart, "head", 18 * 4096, true)));
        assertEquals("head", field(details).getString("text"));
        assertFalse(field(details).optBoolean("prefixOmitted"));
    }

    @Test public void ownerEarlyFailureCannotReflectUnboundedOrMalformedRequestBindings() throws Exception {
        JSONObject invalid = new JSONObject().put("requestId", "r".repeat(65400))
            .put("messageId", new JSONArray().put("private")).put("partId", 7)
            .put("field", "arbitrary").put("offset", "a".repeat(65400));
        JSONObject failed = AIAssistantToolDetails.failure("session", invalid, "invalid_input", "invalid request");
        assertTrue("Owner rejection must stay below the control-message budget",
            failed.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8).length < 64 * 1024);
        assertEquals("", failed.getString("requestId"));
        assertEquals("", failed.getString("messageId"));
        assertEquals("", failed.getString("partId"));
        assertEquals("", failed.getString("field"));
        assertEquals(0, failed.getLong("offset"));
        JSONObject valid = new JSONObject().put("requestId", "request").put("messageId", "message")
            .put("partId", "part").put("field", "output").put("offset", 4096);
        JSONObject bound = AIAssistantToolDetails.failure("session", valid, "not_found", "not found");
        assertEquals("request", bound.getString("requestId"));
        assertEquals("output", bound.getString("field"));
        assertEquals(4096, bound.getLong("offset"));
        assertEquals(0, AIAssistantToolDetails.failure("session", new JSONObject(valid.toString())
            .put("offset", Long.MAX_VALUE), "invalid_input", "invalid").getLong("offset"));
    }

    @Test public void onlyBusyAllowsAdoptingAChangedStateAndTheWholeCallKeepsThatState() throws Exception {
        AIAssistantToolDetails details = selected();
        JSONObject metadata = part(REVISION);
        AIAssistantToolDetails.Ticket first = details.begin("message", metadata, "output", false, false);
        details.bind(first, "first");
        assertFalse("An ordinary ACK cannot change metadata state", details.accept("first", envelope(),
            response(first, "wrong", 5, false).put("state", "completed")));
        assertTrue("Wrong state must not consume the original request", details.accept("first", envelope(),
            response(first, "running output", 14, false)));
        AIAssistantToolDetails.Ticket read = details.begin("message", metadata, "output", false, true);
        details.bind(read, "read");
        assertTrue(details.accept("read", envelope(), AIAssistantToolDetails.failure("session", read.request, "busy", "detail_changed")));
        AIAssistantToolDetails.Ticket retry = details.begin("message", metadata, "output", false, true);
        details.bind(retry, "retry");
        JSONObject completed = response(retry, "completed output", 16, false)
            .put("detailRevision", NEW_REVISION).put("state", "completed");
        assertFalse("An unknown state cannot claim the pending fresh read", details.accept("retry", envelope(),
            new JSONObject(completed.toString()).put("state", "bogus")));
        assertFalse("A non-SHA revision cannot claim the pending fresh read", details.accept("retry", envelope(),
            new JSONObject(completed.toString()).put("detailRevision", "x")));
        assertTrue("The legitimate page still owns the original pending request", details.accept("retry", envelope(), completed));
        JSONObject cached = details.snapshot().getJSONObject(AIAssistantToolDetails.key("message", "part"));
        assertEquals("completed", cached.getString("state"));
        assertEquals("completed", AIAssistantToolDetails.visiblePart(metadata, cached).getString("state"));
        assertEquals("The old history object is not rewritten", "running", metadata.getString("state"));
        AIAssistantToolDetails.Ticket input = details.begin("message", metadata, "input", false, true);
        details.bind(input, "input");
        assertFalse("Another field cannot claim another state for the adopted revision", details.accept("input", envelope(),
            response(input, "args", 4, false).put("detailRevision", NEW_REVISION)));
        assertTrue(details.accept("input", envelope(), response(input, "args", 4, false)
            .put("detailRevision", NEW_REVISION).put("state", "completed")));
        assertEquals("completed", details.snapshot().getJSONObject(AIAssistantToolDetails.key("message", "part")).getString("state"));
    }
    @Test public void optionalOuterRequestAliasCannotConflictOrConsumeTheBoundFlatReply() throws Exception {
        AIAssistantToolDetails details = selected();
        AIAssistantToolDetails.Ticket ticket = details.begin("message", part(REVISION), "output", false, false);
        details.bind(ticket, "command");
        JSONObject payload = new JSONObject().put("type", "command_result").put("command_id", "command")
            .put("result", response(ticket, "output", 6, false));
        assertFalse(details.acceptCommandResult(envelope(), new JSONObject(payload.toString()).put("request_id", "wrong")));
        assertTrue("Correct alias must still finish the pending request", details.acceptCommandResult(envelope(),
            new JSONObject(payload.toString()).put("request_id", ticket.request.getString("requestId"))));
        AIAssistantToolDetails.Ticket input = details.begin("message", part(REVISION), "input", false, false);
        details.bind(input, "input");
        assertTrue("Only command_id is required by the protocol", details.acceptCommandResult(envelope(),
            new JSONObject().put("type", "command_result").put("command_id", "input").put("result", response(input, "args", 4, false))));
    }
}
