// 文件作用：验证消息空白短按收键盘，且不抢链接、复制和滚动手势。
package com.kongshang.maichat;

import android.content.Context;
import android.app.Activity;
import android.os.SystemClock;
import android.view.MotionEvent;
import android.widget.FrameLayout;
import android.widget.TextView;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.Robolectric;
import org.robolectric.shadows.ShadowLooper;
import org.robolectric.annotation.Config;
import static org.junit.Assert.*;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, manifest = Config.NONE)
public class ChatKeyboardDismissalTest {
    private int dismissals, clicks;
    private FrameLayout surface() throws Exception {
        Class<?> type;
        try { type = Class.forName("com.kongshang.maichat.ChatKeyboardDismissal"); }
        catch (ClassNotFoundException error) {
            throw new AssertionError("Message surfaces need blank-tap keyboard dismissal", error);
        }
        Activity activity = Robolectric.buildActivity(Activity.class).setup().get();
        FrameLayout surface = (FrameLayout) type.getDeclaredConstructor(Context.class, Runnable.class)
            .newInstance(activity, (Runnable) () -> dismissals++);
        activity.setContentView(surface);
        surface.layout(0, 0, 400, 600);
        return surface;
    }
    private void touch(FrameLayout surface, int action, float x, float y) {
        long now = SystemClock.uptimeMillis();
        MotionEvent event = MotionEvent.obtain(now, now, action, x, y, 0);
        surface.dispatchTouchEvent(event);
        event.recycle();
        ShadowLooper.idleMainLooper();
    }
    @Test public void blankTapDismissesKeyboard() throws Exception {
        FrameLayout surface = surface();
        touch(surface, MotionEvent.ACTION_DOWN, 250, 300);
        touch(surface, MotionEvent.ACTION_UP, 250, 300);
        assertEquals(1, dismissals);
    }
    @Test public void scrollingAndCancellationKeepKeyboardAndReceiveEvents() throws Exception {
        FrameLayout surface = surface();
        touch(surface, MotionEvent.ACTION_DOWN, 250, 300);
        touch(surface, MotionEvent.ACTION_MOVE, 250, 100);
        touch(surface, MotionEvent.ACTION_UP, 250, 100);
        touch(surface, MotionEvent.ACTION_DOWN, 250, 300);
        touch(surface, MotionEvent.ACTION_CANCEL, 250, 300);
        assertEquals(0, dismissals);
    }
    @Test public void linksAndCopyTargetsKeepTheirOwnGestures() throws Exception {
        FrameLayout surface = surface();
        TextView text = new TextView(surface.getContext());
        text.setText("Open link");
        text.setOnClickListener(view -> clicks++);
        surface.addView(text, new FrameLayout.LayoutParams(180, 80));
        text.layout(0, 0, 180, 80);
        touch(surface, MotionEvent.ACTION_DOWN, 40, 40);
        touch(surface, MotionEvent.ACTION_UP, 40, 40);
        assertEquals(1, clicks);
        assertEquals(0, dismissals);
        text.setOnClickListener(null);
        text.setTextIsSelectable(true);
        touch(surface, MotionEvent.ACTION_DOWN, 40, 40);
        touch(surface, MotionEvent.ACTION_UP, 40, 40);
        assertEquals(0, dismissals);
    }
}
