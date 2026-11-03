// 文件作用：空编辑器长按转写；非空草稿及短按继续交给系统编辑器处理。
package com.kongshang.maichat;

import android.annotation.SuppressLint;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.widget.EditText;

/** Shares the empty-composer gesture without making either chat depend on the other. */
final class ComposerVoiceInput {
    interface Listener {
        boolean start(float rawX, float rawY);
        void move(float rawX, float rawY);
        void finish(float rawX, float rawY, boolean cancelled);
        default void touched() { }
        default void detached() { }
    }
    private final EditText editor;
    private final Listener listener;
    private final int slop;
    private float downX, downY, lastX, lastY;
    private boolean pending, active, claimed;
    private final Runnable hold = this::beginHold;

    @SuppressLint("ClickableViewAccessibility")
    static void attach(EditText editor, Listener listener) {
        new ComposerVoiceInput(editor, listener);
    }
    private ComposerVoiceInput(EditText editor, Listener listener) {
        this.editor = editor;
        this.listener = listener;
        slop = ViewConfiguration.get(editor.getContext()).getScaledTouchSlop();
        editor.setOnLongClickListener(view -> {
            // 350ms 已进入转写后，系统稍后的原生 long click 只能消费，不能二次录音或弹粘贴。
            return claimed || beginHold();
        });
        editor.setOnTouchListener((view, event) -> touch(event));
        editor.addOnAttachStateChangeListener(new View.OnAttachStateChangeListener() {
            @Override public void onViewAttachedToWindow(View view) { }
            @Override public void onViewDetachedFromWindow(View view) {
                // 旋转或页面重建不保证旧编辑器收到 ACTION_CANCEL，离窗必须结束麦克风会话。
                stopPending();
                finish(lastX, lastY, true);
                listener.detached();
            }
        });
    }
    private boolean touch(MotionEvent event) {
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                downX = lastX = event.getRawX(); downY = lastY = event.getRawY();
                pending = editor.length() == 0; claimed = false;
                if (pending) editor.postDelayed(hold, 350);
                listener.touched();
                return false;
            case MotionEvent.ACTION_MOVE:
                lastX = event.getRawX(); lastY = event.getRawY();
                if (!active) {
                    if (Math.abs(event.getRawX() - downX) > slop
                        || Math.abs(event.getRawY() - downY) > slop) stopPending();
                    return false;
                }
                listener.move(lastX, lastY);
                return true;
            case MotionEvent.ACTION_POINTER_DOWN:
            case MotionEvent.ACTION_CANCEL:
                stopPending();
                return finish(lastX, lastY, true);
            case MotionEvent.ACTION_UP:
                stopPending();
                // 三态选择属于共用浮层；手势层只透传屏幕坐标，不能把上移一律当取消。
                return finish(event.getRawX(), event.getRawY(), false);
            default:
                return active;
        }
    }
    private boolean beginHold() {
        if (!pending || !editor.isEnabled() || editor.length() != 0) return false;
        stopPending();
        claimed = true;
        editor.cancelLongPress();
        active = listener.start(downX, downY);
        if (active && editor.getParent() != null)
            editor.getParent().requestDisallowInterceptTouchEvent(true);
        return true;
    }
    private void stopPending() { pending = false; editor.removeCallbacks(hold); }
    private boolean finish(float rawX, float rawY, boolean cancelled) {
        boolean handled = claimed;
        claimed = false;
        if (handled) editor.setPressed(false);
        if (!active) return handled;
        active = false;
        if (editor.getParent() != null) editor.getParent().requestDisallowInterceptTouchEvent(false);
        listener.finish(rawX, rawY, cancelled);
        return true;
    }
}
