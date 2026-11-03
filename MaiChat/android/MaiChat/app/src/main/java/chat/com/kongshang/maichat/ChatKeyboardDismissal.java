// 文件作用：在消息容器观察空白短按；子控件仍接收完整事件序列。
package com.kongshang.maichat;

import android.content.Context;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.FrameLayout;
import android.widget.TextView;

/** Observes at dispatch time so a list or a message child cannot hide a blank tap. */
final class ChatKeyboardDismissal extends FrameLayout {
    private final Runnable dismiss;
    private final int slop;
    private float downX, downY;
    private long downTime;
    private boolean blankTap;

    ChatKeyboardDismissal(Context context, Runnable dismiss) {
        super(context);
        this.dismiss = dismiss;
        slop = ViewConfiguration.get(context).getScaledTouchSlop();
    }
    @Override public boolean dispatchTouchEvent(MotionEvent event) {
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                downX = event.getX(); downY = event.getY(); downTime = event.getEventTime();
                blankTap = !interactiveAt(this, downX, downY);
                break;
            case MotionEvent.ACTION_MOVE:
                if (Math.abs(event.getX() - downX) > slop || Math.abs(event.getY() - downY) > slop)
                    blankTap = false;
                break;
            case MotionEvent.ACTION_POINTER_DOWN:
            case MotionEvent.ACTION_CANCEL:
                blankTap = false;
                break;
            default:
                break;
        }
        boolean observesBlank = blankTap;
        boolean handled = super.dispatchTouchEvent(event);
        if (event.getActionMasked() == MotionEvent.ACTION_UP) {
            boolean shouldDismiss = blankTap
                && event.getEventTime() - downTime < ViewConfiguration.getLongPressTimeout()
                && !interactiveAt(this, event.getX(), event.getY());
            blankTap = false;
            if (shouldDismiss) dismiss.run();
        }
        return handled || observesBlank;
    }
    private static boolean interactiveAt(View view, float x, float y) {
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int index = group.getChildCount() - 1; index >= 0; index--) {
                View child = group.getChildAt(index);
                if (child.getVisibility() != VISIBLE) continue;
                float childX = x + group.getScrollX() - child.getX();
                float childY = y + group.getScrollY() - child.getY();
                if (childX >= 0 && childY >= 0 && childX < child.getWidth()
                    && childY < child.getHeight() && interactiveAt(child, childX, childY)) return true;
            }
        }
        // AdapterView 自身可点击并不代表空白有动作；只保护实际命中的消息行入口。
        if (view instanceof AdapterView) {
            AdapterView<?> list = (AdapterView<?>) view;
            return list.getOnItemClickListener() != null || list.getOnItemLongClickListener() != null;
        }
        return view.hasOnClickListeners() || view.isLongClickable()
            || view instanceof TextView && (((TextView) view).isTextSelectable()
                || ((TextView) view).getMovementMethod() != null);
    }
}
