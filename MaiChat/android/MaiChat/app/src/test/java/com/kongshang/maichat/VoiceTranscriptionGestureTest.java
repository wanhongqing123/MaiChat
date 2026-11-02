// 文件作用：验证语音浮层原始坐标命中、iOS 拖动阈值和 Unicode 预览边界。
package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import org.junit.Test;

public class VoiceTranscriptionGestureTest {
    private final VoiceTranscriptionGesture.Bounds cancel =
        new VoiceTranscriptionGesture.Bounds(100, 500, 152, 552);
    private final VoiceTranscriptionGesture.Bounds edit =
        new VoiceTranscriptionGesture.Bounds(244, 500, 296, 552);

    @Test public void visibleTargetsWinRegardlessOfFingerOrigin() {
        VoiceTranscriptionGesture gesture = new VoiceTranscriptionGesture(false, 1, 320, 700);
        assertEquals(VoiceTranscriptionGesture.Target.EDIT, gesture.move(270, 520, cancel, edit));
        assertEquals(VoiceTranscriptionGesture.Target.CANCEL, gesture.move(130, 520, cancel, edit));
    }

    @Test public void expandsEachVisibleTargetBySixteenDp() {
        VoiceTranscriptionGesture gesture = new VoiceTranscriptionGesture(false, 2, 200, 700);
        assertEquals(VoiceTranscriptionGesture.Target.CANCEL, gesture.move(69, 480, cancel, edit));
        assertEquals(VoiceTranscriptionGesture.Target.SEND, gesture.move(67, 480, cancel, edit));
    }

    @Test public void assistantAndImUseDifferentLeftDragThresholds() {
        VoiceTranscriptionGesture im = new VoiceTranscriptionGesture(false, 2, 400, 700);
        VoiceTranscriptionGesture ai = new VoiceTranscriptionGesture(true, 2, 400, 700);
        assertEquals(VoiceTranscriptionGesture.Target.CANCEL, im.move(220, 700, null, null));
        assertEquals(VoiceTranscriptionGesture.Target.SEND, ai.move(220, 700, null, null));
        assertEquals(VoiceTranscriptionGesture.Target.CANCEL, ai.move(159, 700, null, null));
    }

    @Test public void upwardMotionAloneSendsAndRightUpwardMotionEdits() {
        VoiceTranscriptionGesture gesture = new VoiceTranscriptionGesture(false, 1, 200, 700);
        assertEquals(VoiceTranscriptionGesture.Target.SEND, gesture.move(200, 500, null, null));
        assertEquals(VoiceTranscriptionGesture.Target.SEND, gesture.move(270, 664, null, null));
        assertEquals(VoiceTranscriptionGesture.Target.SEND, gesture.move(271, 665, null, null));
        assertEquals(VoiceTranscriptionGesture.Target.EDIT, gesture.move(271, 664, null, null));
    }

    @Test public void previewKeepsLast180WholeCodePoints() {
        String source = "prefix" + "😀".repeat(181);
        String preview = VoiceTranscriptionGesture.preview(source);
        assertEquals(180, preview.codePointCount(0, preview.length()));
        assertEquals("😀".repeat(180), preview);
        assertEquals("", VoiceTranscriptionGesture.preview(null));
    }
}
