// 文件作用：图片消息行保留图片的实际宽度，并先为日期和发送状态预留显示空间。
package com.kongshang.maichat;

import android.content.Context;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;

public final class MaiImageMessageRow extends LinearLayout {
    private int bubbleWidthLimit = -1;

    public MaiImageMessageRow(Context context) {
        super(context);
        setOrientation(HORIZONTAL);
    }

    @Override protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        View bubble = getChildCount() == 0 ? null : getChildAt(0);
        if (bubble == null || MeasureSpec.getMode(widthMeasureSpec) == MeasureSpec.UNSPECIFIED) {
            if (bubble != null) constrainImages(bubble, Integer.MAX_VALUE);
            super.onMeasure(widthMeasureSpec, heightMeasureSpec);
            return;
        }

        // Metadata is measured first, starting with the fixed trailing status. A long date or
        // caption must not push the status outside the row, especially in a narrow split pane.
        int tailsWidth = 0;
        for (int index = getChildCount() - 1; index > 0; index--) {
            View tail = getChildAt(index);
            if (tail.getVisibility() == GONE) continue;
            super.measureChildWithMargins(tail, widthMeasureSpec, tailsWidth, heightMeasureSpec, 0);
            LayoutParams params = (LayoutParams) tail.getLayoutParams();
            tailsWidth += tail.getMeasuredWidth() + params.leftMargin + params.rightMargin;
        }
        LayoutParams params = (LayoutParams) bubble.getLayoutParams();
        bubbleWidthLimit = Math.max(0, MeasureSpec.getSize(widthMeasureSpec)
            - getPaddingLeft() - getPaddingRight() - tailsWidth - params.leftMargin - params.rightMargin);
        constrainImages(bubble, bubbleWidthLimit);
        try {
            // Keep LinearLayout's normal gravity and baseline/layout behavior. Only the first
            // child's width budget changes; its WRAP_CONTENT width remains its actual content.
            super.onMeasure(widthMeasureSpec, heightMeasureSpec);
        } finally {
            bubbleWidthLimit = -1;
        }
    }

    @Override protected void measureChildWithMargins(View child, int parentWidthMeasureSpec,
            int widthUsed, int parentHeightMeasureSpec, int heightUsed) {
        if (bubbleWidthLimit >= 0 && getChildCount() > 0 && child == getChildAt(0)) {
            LayoutParams params = (LayoutParams) child.getLayoutParams();
            int size = bubbleWidthLimit + getPaddingLeft() + getPaddingRight()
                + params.leftMargin + params.rightMargin;
            parentWidthMeasureSpec = MeasureSpec.makeMeasureSpec(size, MeasureSpec.AT_MOST);
            widthUsed = 0;
        }
        super.measureChildWithMargins(child, parentWidthMeasureSpec, widthUsed,
            parentHeightMeasureSpec, heightUsed);
    }

    private static void constrainImages(View view, int availableWidth) {
        if (view instanceof MaiImageThumbnailView) {
            ((MaiImageThumbnailView) view).setAvailableWidth(availableWidth);
        } else if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            int contentWidth = Math.max(0, availableWidth - group.getPaddingLeft() - group.getPaddingRight());
            for (int index = 0; index < group.getChildCount(); index++) {
                View child = group.getChildAt(index);
                if (child.getVisibility() == GONE) continue;
                ViewGroup.LayoutParams params = child.getLayoutParams();
                int childWidth = contentWidth;
                if (params instanceof ViewGroup.MarginLayoutParams) {
                    ViewGroup.MarginLayoutParams margins = (ViewGroup.MarginLayoutParams) params;
                    childWidth = Math.max(0, childWidth - margins.leftMargin - margins.rightMargin);
                }
                constrainImages(child, childWidth);
            }
        }
    }
}
