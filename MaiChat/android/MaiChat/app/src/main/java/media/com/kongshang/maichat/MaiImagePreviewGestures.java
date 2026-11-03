// Image gestures follow MaiLocalMediaPager: single tap closes, double tap zooms,
// and a pinch never dismisses. Coordinates belong to the fixed viewport, not the scaled image.
package com.kongshang.maichat;

import android.animation.ValueAnimator;
import android.view.GestureDetector;
import android.view.MotionEvent;
import android.view.ScaleGestureDetector;
import android.view.View;
import android.view.animation.AccelerateDecelerateInterpolator;
import java.util.function.IntConsumer;

final class MaiImagePreviewGestures implements View.OnTouchListener {
    private final View viewport;
    private final View image;
    private final GestureDetector taps;
    private final ScaleGestureDetector pinch;
    private final IntConsumer neighbor;
    private float scale = 1f;
    private float downX, downY, lastX, lastY;
    private boolean multiple;
    private boolean enabled = true;
    private ValueAnimator zoomAnimation;

    MaiImagePreviewGestures(View viewport, View image, Runnable close, IntConsumer neighbor) {
        this.viewport = viewport;
        this.image = image;
        this.neighbor = neighbor;
        taps = new GestureDetector(viewport.getContext(),
            new GestureDetector.SimpleOnGestureListener() {
                @Override public boolean onDown(MotionEvent event) { return true; }
                @Override public boolean onSingleTapConfirmed(MotionEvent event) {
                    if (enabled) close.run();
                    return true;
                }
                @Override public boolean onDoubleTap(MotionEvent event) {
                    if (enabled) toggleZoom(event.getX(), event.getY());
                    return true;
                }
            });
        pinch = new ScaleGestureDetector(viewport.getContext(),
            new ScaleGestureDetector.SimpleOnScaleGestureListener() {
                @Override public boolean onScaleBegin(ScaleGestureDetector detector) {
                    cancelZoom();
                    multiple = true;
                    return enabled;
                }
                @Override public boolean onScale(ScaleGestureDetector detector) {
                    float next = Math.max(1f, Math.min(4f, scale * detector.getScaleFactor()));
                    float ratio = next / scale;
                    float x = detector.getFocusX() - viewport.getWidth() / 2f;
                    float y = detector.getFocusY() - viewport.getHeight() / 2f;
                    apply(next, x - ratio * (x - image.getTranslationX()),
                        y - ratio * (y - image.getTranslationY()));
                    return true;
                }
            });
        pinch.setQuickScaleEnabled(false);
    }

    void setEnabled(boolean value) {
        enabled = value;
        if (!value) cancelZoom();
    }

    void reset() {
        cancelZoom();
        apply(1f, 0f, 0f);
    }

    @Override public boolean onTouch(View ignored, MotionEvent event) {
        if (!enabled) return true;
        if (event.getActionMasked() == MotionEvent.ACTION_DOWN) {
            multiple = false;
            downX = lastX = event.getX();
            downY = lastY = event.getY();
        }
        if (event.getPointerCount() > 1) multiple = true;
        pinch.onTouchEvent(event);
        taps.onTouchEvent(event);
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_MOVE:
                if (!pinch.isInProgress() && event.getPointerCount() == 1 && scale > 1.01f) {
                    cancelZoom();
                    apply(scale, image.getTranslationX() + event.getX() - lastX,
                        image.getTranslationY() + event.getY() - lastY);
                }
                lastX = event.getX();
                lastY = event.getY();
                break;
            case MotionEvent.ACTION_POINTER_UP:
                int remaining = event.getActionIndex() == 0 ? 1 : 0;
                lastX = event.getX(remaining);
                lastY = event.getY(remaining);
                break;
            case MotionEvent.ACTION_UP:
                float dx = event.getX() - downX;
                float dy = event.getY() - downY;
                float threshold = 72f * viewport.getResources().getDisplayMetrics().density;
                if (!multiple && scale <= 1.01f && Math.abs(dx) >= threshold
                        && Math.abs(dx) > Math.abs(dy) * 1.4f) neighbor.accept(dx < 0 ? 1 : -1);
                break;
            default:
                break;
        }
        return true;
    }

    private void toggleZoom(float x, float y) {
        cancelZoom();
        float start = scale;
        float end = start > 1.01f ? 1f : 2.5f;
        float fromX = image.getTranslationX(), fromY = image.getTranslationY();
        // UIScrollView.zoom(to:) centers the tapped image point (within the image edges).
        float toX = end == 1f ? 0f : -(x - viewport.getWidth() / 2f) * end;
        float toY = end == 1f ? 0f : -(y - viewport.getHeight() / 2f) * end;
        zoomAnimation = ValueAnimator.ofFloat(0f, 1f);
        zoomAnimation.setDuration(300);
        zoomAnimation.setInterpolator(new AccelerateDecelerateInterpolator());
        zoomAnimation.addUpdateListener(animation -> {
            float t = (float) animation.getAnimatedValue();
            apply(start + (end - start) * t, fromX + (toX - fromX) * t,
                fromY + (toY - fromY) * t);
        });
        zoomAnimation.start();
    }

    private void apply(float value, float x, float y) {
        scale = value;
        image.setScaleX(value);
        image.setScaleY(value);
        // Letterboxed axes stay centered until the zoomed image exceeds the viewport.
        float maxX = Math.max(0f, (image.getWidth() * value - viewport.getWidth()) / 2f);
        float maxY = Math.max(0f, (image.getHeight() * value - viewport.getHeight()) / 2f);
        image.setTranslationX(Math.max(-maxX, Math.min(maxX, x)));
        image.setTranslationY(Math.max(-maxY, Math.min(maxY, y)));
    }

    private void cancelZoom() {
        if (zoomAnimation != null) zoomAnimation.cancel();
        zoomAnimation = null;
    }
}
