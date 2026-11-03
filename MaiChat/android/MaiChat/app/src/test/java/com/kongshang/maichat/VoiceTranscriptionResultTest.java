// 文件作用：验证三态松手后的真实 final 交付；修改只交给草稿，取消不交付。
package com.kongshang.maichat;

import org.junit.Test;
import static org.junit.Assert.*;

public class VoiceTranscriptionResultTest {
    private Object result() throws Exception {
        try { return Class.forName("com.kongshang.maichat.VoiceTranscriptionResult").getDeclaredConstructor().newInstance(); }
        catch (ClassNotFoundException error) { throw new AssertionError("Three-action transcription needs final-result ownership", error); }
    }
    private Object action(String name) throws Exception {
        for (Object value : Class.forName("com.kongshang.maichat.VoiceTranscriptionOverlay$Action").getEnumConstants())
            if (value.toString().equals(name)) return value;
        throw new AssertionError(name);
    }
    private boolean release(Object result, String name) throws Exception {
        Object action = action(name);
        return (Boolean) result.getClass().getDeclaredMethod("release", action.getClass()).invoke(result, action);
    }
    private String finish(Object result, String text, boolean current) throws Exception {
        Object action = result.getClass().getDeclaredMethod("finalAction", String.class, boolean.class)
            .invoke(result, text, current);
        return action == null ? null : action.toString();
    }
    @Test public void releasingSendWaitsForARealFinalAndDeliversOnlyOnce() throws Exception {
        Object result = result();
        assertNull(finish(result, "partial", true));
        assertTrue(release(result, "SEND"));
        assertEquals("SEND", finish(result, "Final text", true));
        assertNull(finish(result, "Repeated final", true));
    }
    @Test public void editNeverReturnsTheSendAction() throws Exception {
        Object result = result();
        assertTrue(release(result, "EDIT"));
        assertEquals("EDIT", finish(result, "Draft text", true));
    }
    @Test public void cancelDiscardsPartialAndAnyLateFinal() throws Exception {
        Object result = result();
        assertTrue(release(result, "CANCEL"));
        assertNull(finish(result, "Late text", true));
        assertFalse(release(result, "SEND"));
    }
    @Test public void emptyFinalAndStaleDraftNeverDeliver() throws Exception {
        Object empty = result(); release(empty, "SEND");
        assertNull(finish(empty, "  ", true));
        Object stale = result(); release(stale, "EDIT");
        assertNull(finish(stale, "Old final", false));
        assertNull(finish(stale, "Another final", true));
    }
    @Test public void leavingThePageInvalidatesThePendingEdit() throws Exception {
        Object result = result(); release(result, "EDIT");
        result.getClass().getDeclaredMethod("invalidate").invoke(result);
        assertNull(finish(result, "Old final", true));
    }
}
