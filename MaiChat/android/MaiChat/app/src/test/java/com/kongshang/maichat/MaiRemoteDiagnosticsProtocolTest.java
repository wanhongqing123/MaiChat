// Protocol parity and disclosure boundaries; synthetic data never contacts IM.
package com.kongshang.maichat;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.ArrayList;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import static org.junit.Assert.*;

public class MaiRemoteDiagnosticsProtocolTest {
    static final String ID = "65de6748-3a4e-4d08-8ffd-bc78e1804ff9";
    private byte[] bytes(String value) { return value.getBytes(StandardCharsets.UTF_8); }
    private Path fixture(String name) {
        Path cursor = Path.of("").toAbsolutePath();
        while (cursor != null) {
            Path candidate = cursor.resolve("tests/fixtures/" + name);
            if (Files.isRegularFile(candidate)) return candidate;
            cursor = cursor.getParent();
        }
        throw new AssertionError("Missing repository fixture " + name);
    }

    @Test public void diagnosticFileNameAndLimitAreExact() {
        String name = "remote-diagnostics-" + ID + ".json";
        assertEquals("/diagnostics " + ID, MaiRemoteDiagnosticsProtocol.command(ID));
        assertEquals(name, MaiRemoteDiagnosticsProtocol.reportName(ID));
        assertEquals(2 * 1024 * 1024, MaiRemoteDiagnosticsProtocol.attachmentLimit(name, 512L << 20));
        for (String ordinary : new String[]{name.toUpperCase(), name.replace(ID, ID.toUpperCase()),
                name.replace(ID, ID.substring(1)), "../" + name, name + ".bak", "notes.json"})
            assertEquals(512L << 20, MaiRemoteDiagnosticsProtocol.attachmentLimit(ordinary, 512L << 20));
    }

    @Test public void sharedIosFixtureKeepsUsefulMetadata() throws Exception {
        JSONObject report = MaiRemoteDiagnosticsProtocol.sanitized(
            Files.readAllBytes(fixture("remote-diagnostics-report.json")), ID);
        JSONObject event = report.getJSONArray("files").getJSONObject(0).getJSONArray("events").getJSONObject(0);
        assertEquals("aicli:event-forwarded", event.getString("event"));
        assertEquals("call-async", event.getJSONObject("detail").getString("messageId"));
        assertEquals(12.5, event.getJSONObject("detail").getDouble("duration_ms"), 0);
        assertEquals("unavailable", report.getJSONObject("sourceCoverage").getString("uiPerformance"));
    }

    @Test public void sharedFailureReceiptsRequireTheCurrentNonce() throws Exception {
        JSONObject fixture = new JSONObject(new String(Files.readAllBytes(
            fixture("remote-diagnostics-failure-receipts.json")), StandardCharsets.UTF_8));
        JSONArray cases = fixture.getJSONArray("cases");
        for (int i = 0; i < cases.length(); i++) {
            JSONObject item = cases.getJSONObject(i);
            assertEquals(item.getString("category"), item.getBoolean("recognized"),
                MaiRemoteDiagnosticsProtocol.failureReceipt(item.getString("text"), ID) != null);
        }
    }

    @Test public void remoteBodiesCredentialsUnknownFieldsAndUnsafeStringsAreDropped() throws Exception {
        JSONObject raw = new JSONObject().put("schemaVersion", 1).put("requestId", ID)
            .put("token", "SECRET_TOKEN").put("files", new JSONArray().put(new JSONObject()
                .put("source", "runtime.log").put("body", "PRIVATE_BODY")
                .put("events", new JSONArray().put(new JSONObject().put("event", "safe-event")
                    .put("text", "PRIVATE_TEXT").put("taskId", "unsafe secret")
                    .put("detail", new JSONObject().put("password", "SECRET_PASSWORD").put("ok", true))))));
        String cleaned = MaiRemoteDiagnosticsProtocol.sanitized(bytes(raw.toString()), ID).toString();
        assertTrue(cleaned.contains("safe-event"));
        for (String secret : new String[]{"SECRET", "PRIVATE", "unsafe secret", "password", "token"})
            assertFalse(cleaned, cleaned.contains(secret));
    }

    @Test public void wrongSchemaNonceSizeAndExcessiveDepthAreRejected() throws Exception {
        for (String invalid : new String[]{"{}", "{\"schemaVersion\":true,\"requestId\":\"" + ID + "\",\"files\":[]}",
            "{\"schemaVersion\":1,\"requestId\":\"other\",\"files\":[]}", "[".repeat(80) + "0" + "]".repeat(80)}) {
            try { MaiRemoteDiagnosticsProtocol.sanitized(bytes(invalid), ID); fail(invalid); }
            catch (IOException expected) { }
        }
        try { MaiRemoteDiagnosticsProtocol.sanitized(new byte[(2 << 20) + 1], ID); fail("oversize"); }
        catch (IOException expected) { }
    }

    @Test public void localEvidenceUsesOnlyBoundedMetadataAndStableTags() throws Exception {
        long now = 1_800_000_000_000L;
        JSONObject local = MaiRemoteDiagnosticsProtocol.localEvidence("2.2.7", "16", "private-owner", "private-peer",
            true, now, Arrays.asList(
                new MaiRemoteDiagnosticsProtocol.MessageMetadata("private-id", "private-remote", "incoming", "received", now, "file"),
                new MaiRemoteDiagnosticsProtocol.MessageMetadata("old-id", "", "outgoing", "sent", now - 1_800_001, "message")));
        assertEquals(1, local.getJSONArray("messages").length());
        assertEquals(1, local.getInt("excludedMessageCount"));
        assertEquals("unavailable", local.getJSONObject("performanceCoverage").getString("status"));
        String encoded = new String(MaiRemoteDiagnosticsProtocol.mergedReport(ID, local, null, "远端现场缺失"), StandardCharsets.UTF_8);
        assertTrue(encoded.contains("远端现场缺失"));
        assertTrue(encoded.contains("Android MaiChat"));
        assertFalse(encoded.contains("private-"));
        assertEquals(MaiRemoteDiagnosticsProtocol.tag("private-id", "m"),
            local.getJSONArray("messages").getJSONObject(0).getString("id"));
    }

    @Test public void fileAndCollectionBoundsMatchIos() throws Exception {
        JSONArray events = new JSONArray();
        for (int i = 0; i < 1001; i++) events.put(new JSONObject().put("event", "event-" + i));
        JSONArray sessions = new JSONArray();
        for (int i = 0; i < 33; i++) sessions.put(new JSONObject().put("sessionId", "session-" + i));
        JSONObject raw = new JSONObject().put("schemaVersion", 1).put("requestId", ID)
            .put("activeSessions", sessions).put("files", new JSONArray().put(new JSONObject().put("events", events)));
        JSONObject clean = MaiRemoteDiagnosticsProtocol.sanitized(bytes(raw.toString()), ID);
        JSONObject file = clean.getJSONArray("files").getJSONObject(0);
        assertEquals(1000, file.getJSONArray("events").length());
        assertTrue(file.getBoolean("truncated"));
        assertEquals(32, clean.getJSONArray("activeSessions").length());
        JSONArray files = new JSONArray();
        for (int i = 0; i < 17; i++) files.put(new JSONObject());
        raw.put("files", files);
        expectInvalid(bytes(raw.toString()));
        raw.put("files", new JSONArray().put("invalid-file"));
        expectInvalid(bytes(raw.toString()));
    }

    @Test public void metadataDepthAndTypesCannotDiscloseNestedContent() throws Exception {
        JSONObject nested = new JSONObject().put("event", "level-4");
        for (int i = 3; i >= 0; i--) nested = new JSONObject().put("event", "level-" + i).put("detail", nested);
        nested.put("duration_ms", true).put("ok", "true").put("taskId", "x".repeat(201));
        JSONObject raw = new JSONObject().put("schemaVersion", 1).put("requestId", ID)
            .put("sourceCoverage", new JSONObject().put("uiPerformance", "SECRET").put("token", "SECRET"))
            .put("files", new JSONArray().put(new JSONObject().put("invalidLines", -2).put("events", new JSONArray().put(nested))));
        JSONObject clean = MaiRemoteDiagnosticsProtocol.sanitized(bytes(raw.toString()), ID);
        JSONObject event = clean.getJSONArray("files").getJSONObject(0).getJSONArray("events").getJSONObject(0);
        assertFalse(event.has("duration_ms"));
        assertFalse(event.has("ok"));
        assertFalse(event.has("taskId"));
        assertEquals(0, clean.getJSONArray("files").getJSONObject(0).getInt("invalidLines"));
        assertFalse(clean.toString().contains("level-4"));
        assertFalse(clean.toString().contains("SECRET"));
    }

    @Test public void reportReaderChecksDeclaredSizeAndActualStreamBound() throws Exception {
        Path directory = Path.of(".cache/remote-diagnostics-tests");
        Files.createDirectories(directory);
        Path valid = directory.resolve("valid-report.json");
        Files.write(valid, Files.readAllBytes(fixture("remote-diagnostics-report.json")));
        assertEquals(ID, MaiRemoteDiagnosticsProtocol.readReport(valid.toFile(), ID).getString("requestId"));
        Path large = directory.resolve("oversize-report.json");
        Files.write(large, new byte[MaiRemoteDiagnosticsProtocol.MAX_BYTES + 1]);
        for (File file : new File[]{large.toFile(), new File(large.toString()) {
            @Override public long length() { return 0; }
        }}) {
            try { MaiRemoteDiagnosticsProtocol.readReport(file, ID); fail("bounded file read"); }
            catch (IOException expected) { }
        }
    }

    @Test public void jsonDepthScanHandlesEscapedQuotesAndRejectsMalformedUtf8OrTrailingJson() throws Exception {
        String raw = "{\"schemaVersion\":1,\"requestId\":\"" + ID + "\",\"files\":[],\"ignored\":\"\\\"" + "[".repeat(80) + "\"}";
        assertEquals(ID, MaiRemoteDiagnosticsProtocol.sanitized(bytes(raw), ID).getString("requestId"));
        expectInvalid(bytes("{\"schemaVersion\":1,\"requestId\":\"" + ID + "\",\"files\":[]} {}"));
        byte[] malformed = bytes("{\"schemaVersion\":1,\"requestId\":\"" + ID + "\",\"files\":[],\"ignored\":\"x\"}");
        malformed[malformed.length - 3] = (byte) 0xff;
        expectInvalid(malformed);
    }

    @Test public void localWindowAndCapExcludeFutureOrOldMetadataAndUnknownStatus() throws Exception {
        long now = 1_800_000_000_000L;
        ArrayList<MaiRemoteDiagnosticsProtocol.MessageMetadata> messages = new ArrayList<>();
        messages.add(new MaiRemoteDiagnosticsProtocol.MessageMetadata("future", "", "incoming", "received", now + 1, "file"));
        for (int i = 0; i < 1001; i++) messages.add(new MaiRemoteDiagnosticsProtocol.MessageMetadata("id-" + i,
            "remote-" + i, "PRIVATE_DIRECTION", "PRIVATE_STATUS", now, "PRIVATE_KIND"));
        JSONObject clean = MaiRemoteDiagnosticsProtocol.localEvidence("2.2.7", "16", "owner", "peer", false, now, messages);
        assertEquals(1000, clean.getJSONArray("messages").length());
        assertEquals(2, clean.getInt("excludedMessageCount"));
        assertFalse(clean.toString().contains("PRIVATE"));
        assertEquals(MaiRemoteDiagnosticsProtocol.tag("id-1", "m"), clean.getJSONArray("messages").getJSONObject(0).getString("id"));
        assertTrue(clean.getJSONObject("performanceCoverage").getString("interpretation").contains("latency"));
    }

    @Test public void mergedReportSanitizesRemoteAgainAndStableTagMatchesIos() throws Exception {
        JSONObject local = MaiRemoteDiagnosticsProtocol.localEvidence("2.2.7", "16", "owner", "peer", false, 0, Arrays.asList());
        JSONObject remote = new JSONObject().put("schemaVersion", 1).put("requestId", ID).put("files", new JSONArray())
            .put("password", "PRIVATE_PASSWORD");
        String text = new String(MaiRemoteDiagnosticsProtocol.mergedReport(ID, local, remote, null), StandardCharsets.UTF_8);
        assertFalse(text.contains("PRIVATE"));
        assertTrue(text.contains(ID));
        assertEquals("m#2cf24dba5fb0a30e", MaiRemoteDiagnosticsProtocol.tag(" hello \n", "m"));
        assertEquals("m#2cf24dba5fb0a30e", MaiRemoteDiagnosticsProtocol.tag("\u3000hello\u0085", "m"));
        assertEquals("<empty>", MaiRemoteDiagnosticsProtocol.tag(" \n", "m"));
        assertNull(MaiRemoteDiagnosticsProtocol.failureReceipt("unrelated " + ID, ID));
    }

    @Test public void nonstandardJsonCannotBypassTheDepthBoundary() throws Exception {
        String prefix = "{\"schemaVersion\":1,\"requestId\":\"" + ID + "\",\"files\":[],";
        for (String invalid : new String[]{
                "{'schemaVersion':1,'requestId':'" + ID + "','files':[]}",
                prefix + "\"ignored\":NaN}", prefix + "\"ignored\":01}",
                prefix + "\"ignored\":\"raw\nnewline\"}", prefix + "\"ignored\":[1,]}",
                prefix + "\"ignored\":0/*comment*/}", prefix + "unquoted:1}"}) {
            expectInvalid(bytes(invalid));
        }
        String valid = prefix + "\"ignored\":[true,false,null,-1.2e+3,{\"unicode\":\"\\u4e00\"}]}";
        assertEquals(ID, MaiRemoteDiagnosticsProtocol.sanitized(bytes(valid), ID).getString("requestId"));
    }

    private void expectInvalid(byte[] data) throws Exception {
        try { MaiRemoteDiagnosticsProtocol.sanitized(data, ID); fail("invalid report"); }
        catch (IOException expected) { }
    }
}
