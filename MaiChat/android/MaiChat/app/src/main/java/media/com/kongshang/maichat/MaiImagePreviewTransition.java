// Temporary full-image overlay for the same 380 ms aspect-fit transition as the iOS pager.
// Graphics keeps ownership of the final image; this host view only animates its presentation.
package com.kongshang.maichat;

import android.animation.Animator;
import android.animation.AnimatorListenerAdapter;
import android.animation.ValueAnimator;
import android.graphics.Bitmap;
import android.graphics.Outline;
import android.graphics.Rect;
import android.graphics.RectF;
import android.view.View;
import android.view.ViewOutlineProvider;
import android.view.animation.PathInterpolator;
import android.widget.FrameLayout;
import android.widget.ImageView;

final class MaiImagePreviewTransition {
    private final FrameLayout viewport;
    private final View page;
    private final View image;
    private ImageView floating;
    private ValueAnimator animator;
    private boolean disposed;
    private boolean closing;
    private float radius;

    MaiImagePreviewTransition(FrameLayout viewport, View page, View image) {
        this.viewport = viewport;
        this.page = page;
        this.image = image;
    }

    static RectF fitted(int imageWidth, int imageHeight, int width, int height) {
        if (imageWidth <= 0 || imageHeight <= 0 || width <= 0 || height <= 0) return new RectF();
        float factor = Math.min((float) width / imageWidth, (float) height / imageHeight);
        float w = imageWidth * factor, h = imageHeight * factor;
        return new RectF((width - w) / 2f, (height - h) / 2f,
            (width + w) / 2f, (height + h) / 2f);
    }

    static RectF sourceFrame(View source, View viewport) {
        if (source == null || !source.isAttachedToWindow() || source.getWidth() <= 0
                || source.getHeight() <= 0 || !source.getGlobalVisibleRect(new Rect())) return null;
        int[] origin = new int[2], host = new int[2];
        source.getLocationOnScreen(origin);
        if (viewport != null) viewport.getLocationOnScreen(host);
        // Visibility validates the source; use its entire bounds, not the clipped visible rect.
        RectF bounds = new RectF(origin[0] - host[0], origin[1] - host[1],
            origin[0] - host[0] + source.getWidth(), origin[1] - host[1] + source.getHeight());
        if (source instanceof ImageView && ((ImageView) source).getDrawable() != null) {
            ImageView thumbnail = (ImageView) source;
            RectF pixels = new RectF(thumbnail.getDrawable().getBounds());
            thumbnail.getImageMatrix().mapRect(pixels);
            pixels.offset(bounds.left + source.getPaddingLeft(), bounds.top + source.getPaddingTop());
            // FIT_CENTER cards may include letterboxing; animate the image, not its empty padding.
            if (pixels.intersect(bounds)) return pixels;
        }
        return bounds;
    }

    boolean running() { return animator != null; }

    void open(Bitmap bitmap, RectF source, Runnable finished) {
        if (disposed || closing || running()) return;
        if (bitmap == null || source == null || source.isEmpty()) {
            page.setAlpha(1f);
            finished.run();
            return;
        }
        RectF destination = fitted(bitmap.getWidth(), bitmap.getHeight(),
            viewport.getWidth(), viewport.getHeight());
        animate(bitmap, source, destination, true, finished);
    }

    void close(Bitmap bitmap, RectF destination, Runnable finished) {
        if (disposed || closing || running()) return;
        closing = true;
        if (bitmap == null || destination == null || destination.isEmpty()) {
            page.animate().alpha(0f).setDuration(220).withEndAction(finished).start();
            return;
        }
        animate(bitmap, fitted(bitmap.getWidth(), bitmap.getHeight(),
            viewport.getWidth(), viewport.getHeight()), destination, false, finished);
    }

    private void animate(Bitmap bitmap, RectF from, RectF to, boolean opening, Runnable finished) {
        floating = new ImageView(viewport.getContext());
        floating.setImageBitmap(bitmap);
        floating.setScaleType(ImageView.ScaleType.FIT_CENTER);
        floating.setClipToOutline(true);
        floating.setOutlineProvider(new ViewOutlineProvider() {
            @Override public void getOutline(View view, Outline outline) {
                outline.setRoundRect(0, 0, view.getWidth(), view.getHeight(), radius);
            }
        });
        viewport.addView(floating, new FrameLayout.LayoutParams(1, 1));
        image.setAlpha(0f);
        page.setAlpha(opening ? 0f : 1f);
        float corner = 8f * viewport.getResources().getDisplayMetrics().density;
        animator = ValueAnimator.ofFloat(0f, 1f);
        animator.setDuration(380);
        // UIKit curveEaseInOut, rather than Android's default cosine interpolation.
        animator.setInterpolator(new PathInterpolator(.42f, 0f, .58f, 1f));
        animator.addUpdateListener(value -> {
            float t = (float) value.getAnimatedValue();
            layoutFloating(from, to, t);
            radius = corner * (opening ? 1f - t : t);
            floating.invalidateOutline();
            page.setAlpha(opening ? t : 1f - t);
        });
        animator.addListener(new AnimatorListenerAdapter() {
            private boolean cancelled;
            @Override public void onAnimationCancel(Animator ignored) { cancelled = true; }
            @Override public void onAnimationEnd(Animator ignored) {
                animator = null;
                if (floating != null) viewport.removeView(floating);
                floating = null;
                if (!cancelled && !disposed) {
                    if (opening) image.setAlpha(1f);
                    finished.run();
                }
            }
        });
        layoutFloating(from, to, 0f);
        radius = opening ? corner : 0f;
        animator.start();
    }

    private void layoutFloating(RectF from, RectF to, float t) {
        float left = from.left + (to.left - from.left) * t;
        float top = from.top + (to.top - from.top) * t;
        int width = Math.max(1, Math.round(from.width() + (to.width() - from.width()) * t));
        int height = Math.max(1, Math.round(from.height() + (to.height() - from.height()) * t));
        FrameLayout.LayoutParams params = (FrameLayout.LayoutParams) floating.getLayoutParams();
        params.width = width;
        params.height = height;
        floating.setLayoutParams(params);
        floating.setX(left);
        floating.setY(top);
    }

    void dispose() {
        disposed = true;
        if (animator != null) animator.cancel();
        page.animate().cancel();
        if (floating != null) viewport.removeView(floating);
        floating = null;
    }
}
