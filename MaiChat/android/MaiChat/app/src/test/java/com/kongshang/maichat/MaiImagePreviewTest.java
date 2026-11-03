// Geometry and input regression coverage; final animation acceptance still uses device recording.
package com.kongshang.maichat;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.RectF;
import android.os.Looper;
import android.view.MotionEvent;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.ImageView;
import java.time.Duration;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.Shadows;
import org.robolectric.annotation.Config;
import static org.junit.Assert.*;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, manifest = Config.NONE)
public class MaiImagePreviewTest {
    @Test public void fittedImagePreservesPortraitAndLandscapeAspectRatios() {
        assertEquals(new RectF(400, 0, 600, 400), MaiImagePreviewTransition.fitted(200, 400, 1000, 400));
        assertEquals(new RectF(0, 400, 400, 600), MaiImagePreviewTransition.fitted(400, 200, 400, 1000));
        assertTrue(MaiImagePreviewTransition.fitted(0, 200, 400, 1000).isEmpty());
    }

    @Test public void sourceUsesDisplayedImageAndConvertsBetweenWindowOrigins() {
        Activity activity = Robolectric.buildActivity(Activity.class).setup().get();
        FrameLayout root = new FrameLayout(activity);
        activity.setContentView(root);
        ImageView source = new ImageView(activity);
        source.setImageBitmap(Bitmap.createBitmap(100, 200, Bitmap.Config.ARGB_8888));
        source.setScaleType(ImageView.ScaleType.FIT_CENTER);
        FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(200, 200);
        params.leftMargin = 80; params.topMargin = 100;
        root.addView(source, params);
        FrameLayout viewport = new FrameLayout(activity);
        FrameLayout.LayoutParams overlay = new FrameLayout.LayoutParams(400, 600);
        overlay.leftMargin = 20; overlay.topMargin = 40;
        root.addView(viewport, overlay);
        layout(root, 600, 900);
        assertEquals(new RectF(110, 60, 210, 260), MaiImagePreviewTransition.sourceFrame(source, viewport));
    }

    @Test public void originOnlyReturnsTheOriginalItemAndAccount() {
        Activity activity = Robolectric.buildActivity(Activity.class).setup().get();
        FrameLayout root = new FrameLayout(activity);
        View source = new View(activity);
        root.addView(source, new FrameLayout.LayoutParams(100, 80));
        activity.setContentView(root);
        layout(root, 400, 800);
        MaiImagePreviewOrigin origin = new MaiImagePreviewOrigin("first.png", source, "account", 0);
        assertNotNull(origin.frame("first.png", "account", 0, root));
        assertNull(origin.frame("next.png", "account", 0, root));
        assertNull(origin.frame("first.png", "other", 0, root));
        root.removeView(source);
        assertNotNull(origin.frame("first.png", "account", 0, root));
        assertNull(origin.frame("first.png", "account", 1, root));
        origin.configurationChanged();
        assertNull(origin.frame("first.png", "account", 0, root));
    }

    @Test public void lateReadyImageCannotReopenACancelledLoadingPreview() {
        Activity activity = Robolectric.buildActivity(Activity.class).setup().get();
        FrameLayout viewport = new FrameLayout(activity);
        FrameLayout page = new FrameLayout(activity);
        View image = new View(activity);
        viewport.addView(page, new FrameLayout.LayoutParams(-1, -1));
        page.addView(image, new FrameLayout.LayoutParams(-1, -1));
        activity.setContentView(viewport);
        layout(viewport, 400, 800);
        MaiImagePreviewTransition transition = new MaiImagePreviewTransition(viewport, page, image);
        AtomicInteger closes = new AtomicInteger(), opens = new AtomicInteger();
        transition.close(null, null, closes::incrementAndGet);
        transition.open(Bitmap.createBitmap(100, 200, Bitmap.Config.ARGB_8888),
            new RectF(10, 20, 110, 220), opens::incrementAndGet);
        Shadows.shadowOf(Looper.getMainLooper()).idleFor(Duration.ofSeconds(1));
        assertEquals(1, closes.get());
        assertEquals(0, opens.get());
        assertEquals(0f, page.getAlpha(), .001f);
        assertEquals(1, viewport.getChildCount());
    }

    @Test public void singleTapClosesButDoubleTapZoomsWithoutClosing() {
        Activity activity = Robolectric.buildActivity(Activity.class).setup().get();
        FrameLayout viewport = new FrameLayout(activity);
        View image = new View(activity);
        viewport.addView(image, new FrameLayout.LayoutParams(-1, -1));
        activity.setContentView(viewport);
        layout(viewport, 400, 800);
        AtomicInteger closes = new AtomicInteger(), pages = new AtomicInteger();
        MaiImagePreviewGestures gestures = new MaiImagePreviewGestures(viewport, image,
            closes::incrementAndGet, pages::addAndGet);
        long now = android.os.SystemClock.uptimeMillis();
        tap(gestures, viewport, now, 200, 400);
        Shadows.shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(500));
        assertEquals(1, closes.get());
        now = android.os.SystemClock.uptimeMillis();
        tap(gestures, viewport, now, 100, 300);
        tap(gestures, viewport, now + 100, 100, 300);
        Shadows.shadowOf(Looper.getMainLooper()).idleFor(Duration.ofSeconds(1));
        assertEquals(1, closes.get());
        assertEquals(2.5f, image.getScaleX(), .01f);
        assertTrue(image.getTranslationX() > 0f);
        assertEquals(0, pages.get());
        gestures.reset();
        assertEquals(1f, image.getScaleX(), .001f);
        assertEquals(0f, image.getTranslationX(), .001f);
    }

    private static void tap(MaiImagePreviewGestures gestures, View viewport, long time, float x, float y) {
        MotionEvent down = MotionEvent.obtain(time, time, MotionEvent.ACTION_DOWN, x, y, 0);
        MotionEvent up = MotionEvent.obtain(time, time + 30, MotionEvent.ACTION_UP, x, y, 0);
        gestures.onTouch(viewport, down); gestures.onTouch(viewport, up);
        down.recycle(); up.recycle();
    }

    private static void layout(View view, int width, int height) {
        view.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
            View.MeasureSpec.makeMeasureSpec(height, View.MeasureSpec.EXACTLY));
        view.layout(0, 0, width, height);
    }
}
