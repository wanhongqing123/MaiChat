// 文件作用：验证等待语音 final 时编辑或提交新内容会失效旧结果，ASR 自有 partial 不会。
package com.kongshang.maichat;

import org.junit.Test;
import static org.junit.Assert.*;

public class ComposerVoiceResultGuardTest {
    private static final class Guard {
        final Object value;
        final Class<?> type;
        Guard() throws Exception {
            try { type = Class.forName("com.kongshang.maichat.ComposerVoiceResultGuard"); }
            catch (ClassNotFoundException error) {
                throw new AssertionError("Pending ASR results must own the current draft and submission", error);
            }
            value = type.getDeclaredConstructor().newInstance();
        }
        long snapshot() throws Exception { return (Long) type.getDeclaredMethod("snapshot").invoke(value); }
        void edited() throws Exception { type.getDeclaredMethod("edited").invoke(value); }
        void submitted() throws Exception { type.getDeclaredMethod("submitted").invoke(value); }
        boolean accepts(long token, boolean ready) throws Exception {
            return (Boolean) type.getDeclaredMethod("accepts", long.class, boolean.class).invoke(value, token, ready);
        }
        void writeTranscript(Runnable action) throws Exception {
            type.getDeclaredMethod("writeTranscript", Runnable.class).invoke(value, action);
        }
    }
    @Test public void userEditingWhileFinalIsPendingRejectsTheOldFinal() throws Exception {
        Guard guard = new Guard();
        long recording = guard.snapshot();
        guard.edited();
        assertFalse("An old final must not replace a newer draft", guard.accepts(recording, true));
        assertTrue(guard.accepts(guard.snapshot(), true));
    }
    @Test public void manualSubmissionRejectsTheOldFinalEvenAfterTheNewRequestFinishes() throws Exception {
        Guard guard = new Guard();
        long recording = guard.snapshot();
        guard.submitted();
        assertFalse("An old final must not send again or stop a later request", guard.accepts(recording, true));
    }
    @Test public void aBusyOrUnavailableComposerCannotAcceptAnOtherwiseCurrentFinal() throws Exception {
        Guard guard = new Guard();
        long recording = guard.snapshot();
        assertFalse(guard.accepts(recording, false));
        assertTrue(guard.accepts(recording, true));
    }
    @Test public void voiceOwnedPartialKeepsItsFinalButLaterUserEditsInvalidateIt() throws Exception {
        Guard guard = new Guard();
        long recording = guard.snapshot();
        guard.writeTranscript(() -> {
            try { guard.edited(); } catch (Exception error) { throw new AssertionError(error); }
        });
        assertTrue(guard.accepts(recording, true));
        guard.edited();
        assertFalse(guard.accepts(recording, true));
    }
}
