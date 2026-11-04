// Captures only the short-lived action menu's backdrop; the popup owns its lifetime.
package com.kongshang.maichat;

import android.app.Activity;
import android.content.res.Resources;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ColorFilter;
import android.graphics.Outline;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.drawable.Drawable;
import android.view.View;

final class AIAssistantMenuBackdrop extends Drawable {
    private static final int DOWNSAMPLE = 8;
    private static final int FROST = Color.argb(232, 244, 244, 244);
    private final Paint bitmapPaint = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.DITHER_FLAG);
    private final Paint frostPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint borderPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path clip = new Path();
    private final RectF shape = new RectF();
    private final RectF bitmapDestination = new RectF();
    private final Bitmap backdrop;
    private final float radius, stroke;
    private final int capturedWidth, capturedHeight;
    private int alpha = 255;

    private AIAssistantMenuBackdrop(float density, Bitmap backdrop, int width, int height) {
        this.backdrop = backdrop;
        capturedWidth = width;
        capturedHeight = height;
        radius = 14f * density;
        stroke = .5f * density;
        frostPaint.setColor(FROST);
        borderPaint.setColor(Color.argb(20, 0, 0, 0));
        borderPaint.setStyle(Paint.Style.STROKE);
        borderPaint.setStrokeWidth(stroke);
    }

    /**
     * Call on the UI thread immediately before showing the right-aligned popup. Pass the same
     * offsets used by showAsDropDown, then retain this Drawable while inline sections expand.
     * Only the future popup region is sampled; no screenshot, View or Activity is retained.
     */
    static Drawable capture(Activity activity, View anchor, int widthPx,
                            int horizontalOffsetPx, int verticalOffsetPx) {
        Resources resources = activity != null ? activity.getResources()
            : anchor != null ? anchor.getResources() : Resources.getSystem();
        float density = resources.getDisplayMetrics().density;
        Bitmap snapshot = null;
        try {
            View decor = activity == null || activity.getWindow() == null
                ? null : activity.getWindow().getDecorView();
            if (decor == null || anchor == null || !anchor.isAttachedToWindow()
                    || widthPx <= 0 || decor.getWidth() <= 0 || decor.getHeight() <= 0) {
                return new AIAssistantMenuBackdrop(density, null, 0, 0);
            }
            int[] anchorPosition = new int[2], decorPosition = new int[2];
            anchor.getLocationOnScreen(anchorPosition);
            decor.getLocationOnScreen(decorPosition);
            Rect visible = new Rect();
            decor.getWindowVisibleDisplayFrame(visible);
            if (visible.isEmpty()) {
                visible.set(decorPosition[0], decorPosition[1],
                    decorPosition[0] + decor.getWidth(), decorPosition[1] + decor.getHeight());
            }
            int left = anchorPosition[0] + anchor.getWidth() - widthPx + horizontalOffsetPx;
            int top = anchorPosition[1] + anchor.getHeight() + verticalOffsetPx;
            int height = Math.min(visible.bottom, decorPosition[1] + decor.getHeight()) - top;
            // Limit transient bitmap and blur arrays to a real screen region, including when
            // the window is disappearing or a stale anchor reports offscreen coordinates.
            if (widthPx > decor.getWidth() || height <= 0 || height > decor.getHeight()) {
                return new AIAssistantMenuBackdrop(density, null, 0, 0);
            }
            int smallWidth = Math.max(1, (widthPx + DOWNSAMPLE - 1) / DOWNSAMPLE);
            int smallHeight = Math.max(1, (height + DOWNSAMPLE - 1) / DOWNSAMPLE);
            snapshot = Bitmap.createBitmap(smallWidth, smallHeight, Bitmap.Config.ARGB_8888);
            snapshot.eraseColor(Color.rgb(244, 244, 244));
            Canvas capture = new Canvas(snapshot);
            capture.scale(smallWidth / (float) widthPx, smallHeight / (float) height);
            capture.translate(decorPosition[0] - left, decorPosition[1] - top);
            decor.draw(capture);
            blur(snapshot, Math.max(1, Math.round(20f * density / DOWNSAMPLE)));
            return new AIAssistantMenuBackdrop(density, snapshot, widthPx, height);
        } catch (RuntimeException | OutOfMemoryError ignored) {
            // Software capture may reject a hardware-only child bitmap. The same gray material
            // remains usable without capturing another window or exposing private content.
            if (snapshot != null) snapshot.recycle();
            return new AIAssistantMenuBackdrop(density, null, 0, 0);
        }
    }

    @Override public void draw(Canvas canvas) {
        if (getBounds().isEmpty() || alpha == 0) return;
        shape.set(getBounds());
        int layer = alpha < 255 ? canvas.saveLayerAlpha(shape, alpha) : canvas.save();
        int clipped = canvas.save();
        clip.reset();
        clip.addRoundRect(shape, radius, radius, Path.Direction.CW);
        canvas.clipPath(clip);
        // If the keyboard closes after capture, the popup may grow beyond the sampled region.
        // Keep that newly exposed area gray instead of revealing sharp, unblurred live content.
        canvas.drawColor(Color.rgb(244, 244, 244));
        if (backdrop != null) {
            // Keep source pixels at their captured physical position. Resizing the popup must
            // reveal more of this one image, rather than stretching the background during expand.
            bitmapDestination.set(shape.left, shape.top,
                shape.left + capturedWidth, shape.top + capturedHeight);
            canvas.drawBitmap(backdrop, null, bitmapDestination, bitmapPaint);
        }
        canvas.drawRect(shape, frostPaint);
        canvas.restoreToCount(clipped);
        shape.inset(stroke / 2f, stroke / 2f);
        canvas.drawRoundRect(shape, Math.max(0, radius - stroke / 2f),
            Math.max(0, radius - stroke / 2f), borderPaint);
        canvas.restoreToCount(layer);
    }

    @Override public void setAlpha(int alpha) {
        this.alpha = Math.max(0, Math.min(255, alpha));
        invalidateSelf();
    }

    @Override public int getAlpha() { return alpha; }

    @Override public void setColorFilter(ColorFilter filter) {
        bitmapPaint.setColorFilter(filter);
        frostPaint.setColorFilter(filter);
        borderPaint.setColorFilter(filter);
        invalidateSelf();
    }

    @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }

    @Override public void getOutline(Outline outline) {
        outline.setRoundRect(getBounds(), radius);
        outline.setAlpha(alpha / 255f);
    }

    private static void blur(Bitmap bitmap, int radius) {
        int width = bitmap.getWidth(), height = bitmap.getHeight();
        int[] pixels = new int[width * height], scratch = new int[pixels.length];
        bitmap.getPixels(pixels, 0, width, 0, 0, width, height);
        // Three separable box passes approximate a soft ~20 dp blur on the 1/8-size capture.
        // Sliding sums keep work proportional to the small bitmap, not to radius squared.
        for (int pass = 0; pass < 3; pass++) {
            boxPass(pixels, scratch, width, height, radius, true);
            boxPass(scratch, pixels, width, height, radius, false);
        }
        bitmap.setPixels(pixels, 0, width, 0, 0, width, height);
    }

    private static void boxPass(int[] source, int[] target, int width, int height,
                                int radius, boolean horizontal) {
        int length = horizontal ? width : height;
        int lines = horizontal ? height : width;
        int step = horizontal ? 1 : width;
        int divisor = radius * 2 + 1;
        for (int line = 0; line < lines; line++) {
            int start = horizontal ? line * width : line;
            int alpha = 0, red = 0, green = 0, blue = 0;
            for (int offset = -radius; offset <= radius; offset++) {
                int pixel = source[start + Math.max(0, Math.min(length - 1, offset)) * step];
                alpha += Color.alpha(pixel);
                red += Color.red(pixel);
                green += Color.green(pixel);
                blue += Color.blue(pixel);
            }
            for (int position = 0; position < length; position++) {
                target[start + position * step] = Color.argb(alpha / divisor, red / divisor,
                    green / divisor, blue / divisor);
                int leaving = source[start + Math.max(0, position - radius) * step];
                int entering = source[start + Math.min(length - 1, position + radius + 1) * step];
                alpha += Color.alpha(entering) - Color.alpha(leaving);
                red += Color.red(entering) - Color.red(leaving);
                green += Color.green(entering) - Color.green(leaving);
                blue += Color.blue(entering) - Color.blue(leaving);
            }
        }
    }
}
