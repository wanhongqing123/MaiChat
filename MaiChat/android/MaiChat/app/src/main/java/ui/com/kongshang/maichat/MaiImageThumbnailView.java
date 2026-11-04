// 文件作用：与 iOS 图片气泡一致地完整显示缩略图，保持原 Bitmap 并按最大边界等比排版。
package com.kongshang.maichat;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Outline;
import android.graphics.drawable.BitmapDrawable;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewOutlineProvider;
import android.widget.ImageView;

public final class MaiImageThumbnailView extends ImageView {
    private final int maximumWidth;
    private final int maximumHeight;
    private int availableWidth = Integer.MAX_VALUE;

    public MaiImageThumbnailView(Context context) { this(context, 220, 180); }

    public MaiImageThumbnailView(Context context, float maximumWidthDp, float maximumHeightDp) {
        super(context);
        if (!Float.isFinite(maximumWidthDp) || !Float.isFinite(maximumHeightDp)
            || maximumWidthDp <= 0 || maximumHeightDp <= 0)
            throw new IllegalArgumentException("Thumbnail bounds must be finite and positive");
        maximumWidth = Math.max(1, MaiChatTheme.dp(context, maximumWidthDp));
        maximumHeight = Math.max(1, MaiChatTheme.dp(context, maximumHeightDp));
        setScaleType(ScaleType.FIT_CENTER);
        setClipToOutline(true);
        setOutlineProvider(new ViewOutlineProvider() {
            @Override public void getOutline(View view, Outline outline) {
                outline.setRoundRect(0, 0, view.getWidth(), view.getHeight(), MaiChatTheme.dp(getContext(), 8));
            }
        });
        setLayoutParams(new ViewGroup.LayoutParams(maximumWidth, maximumHeight));
    }

    @Override public void setImageBitmap(Bitmap bitmap) {
        super.setImageBitmap(bitmap);
        ViewGroup.LayoutParams params = getLayoutParams();
        if (params == null) params = new ViewGroup.LayoutParams(maximumWidth, maximumHeight);
        setLayoutParams(params);
        invalidateOutline();
    }

    @Override public void setLayoutParams(ViewGroup.LayoutParams params) {
        // RECENT 缓存可能在 addView 之前先交图；父容器随后提供边距时也不能盖回固定裁切尺寸。
        if (params != null && maximumWidth > 0 && maximumHeight > 0) {
            fitLayoutParams(params);
        }
        super.setLayoutParams(params);
    }

    // The enclosing media row calls this during measure, after reserving metadata. Mutate the
    // existing dimensions synchronously: posting/requesting another layout would leave one
    // oversized frame visible and could loop while the window changes orientation or width.
    void setAvailableWidth(int pixels) {
        int next = Math.max(0, pixels);
        if (availableWidth == next) return;
        availableWidth = next;
        ViewGroup.LayoutParams params = getLayoutParams();
        if (params != null) fitLayoutParams(params);
        invalidateOutline();
    }

    private void fitLayoutParams(ViewGroup.LayoutParams params) {
        int width = Math.min(maximumWidth, availableWidth);
        Bitmap bitmap = getDrawable() instanceof BitmapDrawable
            ? ((BitmapDrawable) getDrawable()).getBitmap() : null;
        if (width == 0) {
            params.width = 0;
            params.height = 0;
        } else if (bitmap != null && !bitmap.isRecycled()) {
            float scale = Math.min((float) width / bitmap.getWidth(),
                (float) maximumHeight / bitmap.getHeight());
            params.width = Math.max(1, Math.round(bitmap.getWidth() * scale));
            params.height = Math.max(1, Math.round(bitmap.getHeight() * scale));
        } else {
            params.width = width;
            params.height = maximumHeight;
        }
    }
}
