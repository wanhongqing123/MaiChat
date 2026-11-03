// 文件作用：验证空编辑器长按录音与普通编辑手势的边界，不连接账号或语音服务。
package com.kongshang.maichat;

import android.content.Context;
import android.app.Activity;
import android.os.SystemClock;
import android.os.Looper;
import android.view.MotionEvent;
import android.widget.EditText;
import android.widget.FrameLayout;
import java.lang.reflect.Proxy;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.Robolectric;
import org.robolectric.annotation.Config;
import java.util.concurrent.TimeUnit;
import static org.robolectric.Shadows.shadowOf;
import static org.junit.Assert.*;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, manifest = Config.NONE)
public class ComposerVoiceInputTest {
    private static final class TrackingEditor extends EditText {
        int nativeLongClicks;
        TrackingEditor(Context context) { super(context); }
        @Override public boolean performLongClick() { nativeLongClicks++; return super.performLongClick(); }
    }
    private int starts, moves, finishes;
    private boolean cancelled;
    private float startX = Float.NaN, startY = Float.NaN, endX = Float.NaN, endY = Float.NaN;
    private int detached;

    // 反射让缺少绑定本身成为明确的断言失败，便于在修复前运行同一回归用例。
    private EditText editor() throws Exception {
        Activity activity = Robolectric.buildActivity(Activity.class).setup().get();
        EditText editor = new TrackingEditor(activity);
        activity.setContentView(editor);
        editor.layout(0, 0, 400, 80);
        Class<?> binding;
        try { binding = Class.forName("com.kongshang.maichat.ComposerVoiceInput"); }
        catch (ClassNotFoundException error) {
            throw new AssertionError("Empty composers need a hold-to-transcribe binding", error);
        }
        Class<?> listener = Class.forName("com.kongshang.maichat.ComposerVoiceInput$Listener");
        Object callback = Proxy.newProxyInstance(listener.getClassLoader(), new Class<?>[] {listener},
            (proxy, method, args) -> {
                switch (method.getName()) {
                    case "start":
                        starts++;
                        if (args != null && args.length == 2) { startX = (Float) args[0]; startY = (Float) args[1]; }
                        return true;
                    case "move": moves++; return null;
                    case "finish":
                        finishes++;
                        if (args.length == 3) {
                            endX = (Float) args[0]; endY = (Float) args[1]; cancelled = (Boolean) args[2];
                        } else cancelled = (Boolean) args[0];
                        return null;
                    case "detached": detached++; return null;
                    default: return null;
                }
            });
        binding.getDeclaredMethod("attach", EditText.class, listener).invoke(null, editor, callback);
        return editor;
    }
    private void touch(EditText editor, int action, float x, float y) {
        long now = SystemClock.uptimeMillis();
        MotionEvent event = MotionEvent.obtain(now, now, action, x, y, 0);
        editor.dispatchTouchEvent(event);
        event.recycle();
    }
    @Test public void emptyEditorHoldPassesRawCoordinatesWithoutChoosingTheOverlayAction() throws Exception {
        EditText editor = editor();
        touch(editor, MotionEvent.ACTION_DOWN, 100, 40);
        assertTrue(editor.performLongClick());
        assertEquals(1, starts);
        touch(editor, MotionEvent.ACTION_MOVE, 100, -200);
        touch(editor, MotionEvent.ACTION_UP, 100, -200);
        assertEquals(1, finishes);
        assertFalse("The overlay chooses SEND/CANCEL/EDIT; vertical movement is not a cancellation", cancelled);
        assertEquals(100, startX, 0);
        assertEquals(40, startY, 0);
        assertEquals(100, endX, 0);
        assertEquals(-200, endY, 0);
    }
    @Test public void releaseInsideFinishesOnceWithoutCancelling() throws Exception {
        EditText editor = editor();
        touch(editor, MotionEvent.ACTION_DOWN, 100, 40);
        editor.performLongClick();
        touch(editor, MotionEvent.ACTION_UP, 100, 40);
        assertEquals(1, finishes);
        assertFalse(cancelled);
    }
    @Test public void nonemptyDraftKeepsNativeLongPressAndDoesNotRecord() throws Exception {
        EditText editor = editor();
        editor.setText("Existing draft");
        touch(editor, MotionEvent.ACTION_DOWN, 100, 40);
        editor.performLongClick();
        touch(editor, MotionEvent.ACTION_UP, 100, 40);
        assertEquals(0, starts);
        assertEquals(0, finishes);
        assertEquals("Existing draft", editor.getText().toString());
    }
    @Test public void shortTapAndScrollBeforeHoldDoNotStartRecording() throws Exception {
        EditText editor = editor();
        touch(editor, MotionEvent.ACTION_DOWN, 100, 40);
        touch(editor, MotionEvent.ACTION_UP, 100, 40);
        assertEquals(0, starts);
        touch(editor, MotionEvent.ACTION_DOWN, 100, 40);
        touch(editor, MotionEvent.ACTION_MOVE, 200, 40);
        editor.performLongClick();
        touch(editor, MotionEvent.ACTION_CANCEL, 200, 40);
        assertEquals(0, starts);
    }
    @Test public void systemCancellationNeverCompletesARecording() throws Exception {
        EditText editor = editor();
        touch(editor, MotionEvent.ACTION_DOWN, 100, 40);
        editor.performLongClick();
        touch(editor, MotionEvent.ACTION_CANCEL, 100, 40);
        assertEquals(1, finishes);
        assertTrue(cancelled);
    }
    @Test public void removingAnActiveEditorCancelsWithoutAReleaseEvent() throws Exception {
        EditText editor = editor();
        touch(editor, MotionEvent.ACTION_DOWN, 100, 40);
        editor.performLongClick();
        assertEquals(1, starts);
        ((Activity) editor.getContext()).setContentView(new FrameLayout(editor.getContext()));
        assertEquals("Removing the input must cancel its microphone session", 1, finishes);
        assertTrue(cancelled);
    }
    @Test public void detachingAfterReleaseAlsoInvalidatesThePendingFinal() throws Exception {
        EditText editor = editor();
        touch(editor, MotionEvent.ACTION_DOWN, 100, 40);
        editor.performLongClick();
        touch(editor, MotionEvent.ACTION_UP, 100, 40);
        ((Activity) editor.getContext()).setContentView(new FrameLayout(editor.getContext()));
        assertEquals(1, detached);
    }
    @Test public void emptyHoldStartsAt350msAndNativeLongClickCannotStartItTwice() throws Exception {
        EditText editor = editor();
        touch(editor, MotionEvent.ACTION_DOWN, 100, 40);
        shadowOf(Looper.getMainLooper()).idleFor(350, TimeUnit.MILLISECONDS);
        assertEquals(1, starts);
        assertTrue(editor.performLongClick());
        assertEquals(1, starts);
        touch(editor, MotionEvent.ACTION_UP, 100, 40);
        assertEquals(1, finishes);
    }
    @Test public void releaseBeforeNativeTimeoutLeavesNoLateNativeLongClick() throws Exception {
        TrackingEditor editor = (TrackingEditor) editor();
        touch(editor, MotionEvent.ACTION_DOWN, 100, 40);
        shadowOf(Looper.getMainLooper()).idleFor(400, TimeUnit.MILLISECONDS);
        touch(editor, MotionEvent.ACTION_UP, 100, 40);
        shadowOf(Looper.getMainLooper()).idleFor(400, TimeUnit.MILLISECONDS);
        assertEquals("The native long-click callback must be removed when custom hold takes ownership", 0, editor.nativeLongClicks);
        assertEquals(1, starts);
        assertEquals(1, finishes);
    }
}
