package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;

import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;

public final class AIAssistantFileDeliveryTest {
    private static JSONObject artifact() throws Exception {
        return new JSONObject().put("delivery", "current_ai_session").put("type", "file")
            .put("path", "reports/report.pdf").put("name", "report.pdf")
            .put("mime_type", "application/pdf").put("bytes", 512L * 1024 * 1024)
            .put("caption", "Owned current reply file");
    }

    private static JSONObject part(JSONObject artifact) throws Exception {
        return new JSONObject().put("kind", "tool").put("tool", "agent_send_file")
            .put("state", "completed").put("output", artifact.toString());
    }

    @Test public void currentReplyFileMetadataIsASeparatePersistentArtifact() throws Exception {
        JSONObject part = part(artifact());
        JSONObject actual = AIAssistantPanel.fileFromToolOutput(part);
        assertNotNull(actual);
        assertEquals("report.pdf", actual.getString("name"));
        assertEquals(512L * 1024 * 1024, actual.getLong("bytes"));
        assertEquals("", AIAssistantPanel.toolGroupTitle(new JSONArray().put(part)));
        assertNull(AIAssistantPanel.mediaFromToolOutput(part));
    }

    @Test public void nonFileDeliveryAndMalformedMetadataAreRejected() throws Exception {
        for (String path : new String[]{"../report.pdf", "/outside/report.pdf", "folder/../report.pdf",
                "C:/outside/report.pdf", "folder\\..\\report.pdf"}) {
            assertNull(AIAssistantPanel.fileFromToolOutput(part(artifact().put("path", path))));
        }
        for (Object bytes : new Object[]{0L, 512L * 1024 * 1024 + 1, true, 1.5}) {
            assertNull(AIAssistantPanel.fileFromToolOutput(part(artifact().put("bytes", bytes))));
        }
        assertNull(AIAssistantPanel.fileFromToolOutput(part(artifact().put("name", "spoofed.exe"))));
        assertNull(AIAssistantPanel.fileFromToolOutput(part(artifact().put("delivery", "other_session"))));
        assertNull(AIAssistantPanel.fileFromToolOutput(part(artifact()).put("tool", "another_tool")));
        assertNull(AIAssistantPanel.fileFromToolOutput(part(artifact()).put("state", "running")));
    }

    @Test public void allSixMetadataStringsAreRequiredWithoutCoercion() throws Exception {
        for (String field : new String[]{"delivery", "type", "path", "name", "mime_type", "caption"}) {
            JSONObject missing = artifact();
            missing.remove(field);
            assertNull("Missing " + field, AIAssistantPanel.fileFromToolOutput(part(missing)));
            for (Object value : new Object[]{123, true, JSONObject.NULL, new JSONObject(), new JSONArray()}) {
                JSONObject invalid = artifact().put(field, value);
                // Keep basename matching so path/name coercion cannot hide behind another guard.
                if (field.equals("path")) invalid.put("name", String.valueOf(value));
                if (field.equals("name")) invalid.put("path", String.valueOf(value));
                assertNull("Non-string " + field, AIAssistantPanel.fileFromToolOutput(part(invalid)));
            }
        }
    }

    @Test public void emptyCaptionAndUnknownFieldsKeepTheSameDeliveryContract() throws Exception {
        JSONObject actual = AIAssistantPanel.fileFromToolOutput(part(artifact()
            .put("caption", "").put("peer_id", "ignored-extra-field")));
        assertNotNull(actual);
        assertEquals("", actual.getString("caption"));
        assertEquals("current_ai_session", actual.getString("delivery"));
    }

    @Test public void captionMustBePresentEvenWhenItsValueCanBeEmpty() throws Exception {
        JSONObject missing = artifact();
        missing.remove("caption");
        assertNull(AIAssistantPanel.fileFromToolOutput(part(missing)));
    }

    @Test public void captionValuesAreNotCoercedIntoDisplayText() throws Exception {
        for (Object value : new Object[]{123, true, JSONObject.NULL, new JSONObject(), new JSONArray()})
            assertNull(AIAssistantPanel.fileFromToolOutput(part(artifact().put("caption", value))));
    }

    @Test public void byteCountsMustBeNativeIntegerValuesWithinTheLimit() throws Exception {
        JSONObject missing = artifact();
        missing.remove("bytes");
        assertNull(AIAssistantPanel.fileFromToolOutput(part(missing)));
        for (Object value : new Object[]{"1", true, JSONObject.NULL, -1, 0, 512L * 1024 * 1024 + 1})
            assertNull(AIAssistantPanel.fileFromToolOutput(part(artifact().put("bytes", value))));
        // Preserve the numeric token: JSONObject.toString() can canonicalize 1.0 into 1.
        for (String number : new String[]{"1.0", "1e0", "1.5", "1.0000000000000000000001",
                "1e309", "9223372036854775808"}) {
            String output = artifact().put("bytes", 37).toString()
                .replace("\"bytes\":37", "\"bytes\":" + number);
            assertNull("Non-integer bytes " + number,
                AIAssistantPanel.fileFromToolOutput(part(artifact()).put("output", output)));
        }
        assertNotNull(AIAssistantPanel.fileFromToolOutput(part(artifact().put("bytes", 1))));
    }
}
