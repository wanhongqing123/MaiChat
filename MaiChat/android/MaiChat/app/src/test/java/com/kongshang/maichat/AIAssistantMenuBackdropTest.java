// Native Canvas pixels verify real decor capture without starting accounts, networking or Agent.
package com.kongshang.maichat;

import static org.junit.Assert.*;

import android.app.Activity;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Outline;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;
import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;

@RunWith(RobolectricTestRunner.class)
// Match the real display viewport to Fixture's decor size. A default 320 x 470 display would
// correctly crop capture at y=470 while the test incorrectly inspected an offscreen y=518.
@Config(sdk = 34, manifest = Config.NONE, qualifiers = "w400dp-h800dp-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public final class AIAssistantMenuBackdropTest {
    @Test public void actualDecorColorsAreVisibleThroughFrostAndTheBoundaryIsSoftened() {
        Fixture fixture = new Fixture();
        Drawable backdrop = AIAssistantMenuBackdrop.capture(fixture.activity, fixture.anchor, 240, 0, 8);
        Bitmap pixels = render(backdrop, 240, 160);
        try {
            int left = pixels.getPixel(20, 60), right = pixels.getPixel(220, 60);
            assertTrue("The captured left part must retain its red source through the gray frost",
                Color.red(left) > Color.blue(left) + 4);
            assertTrue("The captured right part must retain its blue source through the gray frost",
                Color.blue(right) > Color.red(right) + 4);
            int softened = pixels.getPixel(36, 60);
            assertTrue("Blur must blend across the source's sharp red/blue boundary",
                Color.blue(softened) > Color.blue(left));
            assertTrue("The gray material must soften the source's saturated colors",
                Color.green(left) > 180 && Color.green(right) > 180);
        } finally {
            pixels.recycle();
            fixture.activity.finish();
        }
    }

    @Test public void expandingOnlyRevealsMoreBackdropWithoutStretchingExistingPixels() {
        Fixture fixture = new Fixture();
        Drawable backdrop = AIAssistantMenuBackdrop.capture(fixture.activity, fixture.anchor, 240, 0, 8);
        Bitmap collapsed = render(backdrop, 240, 160);
        Bitmap expanded = render(backdrop, 240, 500);
        try {
            assertEquals("The top backdrop must remain physically anchored when options expand",
                collapsed.getPixel(20, 40), expanded.getPixel(20, 40));
            assertEquals(collapsed.getPixel(20, 100), expanded.getPixel(20, 100));
            int top = expanded.getPixel(20, 40), bottom = expanded.getPixel(20, 450);
            assertTrue("Expansion must reveal the green lower source rather than stretch the top; "
                    + fixture.diagnostics(backdrop) + "; top=" + Integer.toHexString(top)
                    + "; bottom=" + Integer.toHexString(bottom),
                Color.green(bottom) > Color.red(bottom) + 4 && Color.red(top) > Color.green(top) + 4);
        } finally {
            collapsed.recycle();
            expanded.recycle();
            fixture.activity.finish();
        }
    }

    @Test public void unavailableWindowFallsBackToRoundedGrayWithAShadowOutline() {
        Activity activity = Robolectric.buildActivity(Activity.class).get();
        Drawable backdrop = AIAssistantMenuBackdrop.capture(activity, new View(activity), 240, 0, 8);
        Bitmap pixels = render(backdrop, 240, 160);
        try {
            assertEquals("The popup must not expose square colored corners", 0,
                Color.alpha(pixels.getPixel(0, 0)));
            int center = pixels.getPixel(120, 80);
            assertTrue(Color.alpha(center) >= 220);
            assertTrue(Color.red(center) >= 240 && Color.green(center) >= 240
                && Color.blue(center) >= 240);
            Outline outline = new Outline();
            backdrop.getOutline(outline);
            Rect bounds = new Rect();
            assertTrue("PopupWindow elevation requires a rounded rectangle outline", outline.getRect(bounds));
            assertEquals(new Rect(0, 0, 240, 160), bounds);
            assertTrue(outline.getRadius() >= 13f);
        } finally {
            pixels.recycle();
        }
    }

    private static Bitmap render(Drawable backdrop, int width, int height) {
        Bitmap result = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        backdrop.setBounds(0, 0, width, height);
        backdrop.draw(new Canvas(result));
        return result;
    }

    private static final class Fixture {
        final Activity activity = Robolectric.buildActivity(Activity.class).setup().get();
        final View anchor = new View(activity);

        Fixture() {
            FrameLayout host = new FrameLayout(activity);
            host.addView(new Pattern(activity), new FrameLayout.LayoutParams(-1, -1));
            FrameLayout.LayoutParams position = new FrameLayout.LayoutParams(40, 40, Gravity.TOP | Gravity.RIGHT);
            position.topMargin = 20;
            host.addView(anchor, position);
            activity.setContentView(host);
            View decor = activity.getWindow().getDecorView();
            decor.measure(View.MeasureSpec.makeMeasureSpec(400, View.MeasureSpec.EXACTLY),
                View.MeasureSpec.makeMeasureSpec(800, View.MeasureSpec.EXACTLY));
            decor.layout(0, 0, 400, 800);
        }

        String diagnostics(Drawable backdrop) {
            View decor = activity.getWindow().getDecorView();
            Rect visible = new Rect();
            decor.getWindowVisibleDisplayFrame(visible);
            int[] decorPosition = new int[2], anchorPosition = new int[2];
            decor.getLocationOnScreen(decorPosition);
            anchor.getLocationOnScreen(anchorPosition);
            String dimensions;
            try {
                java.lang.reflect.Field width = backdrop.getClass().getDeclaredField("capturedWidth");
                java.lang.reflect.Field height = backdrop.getClass().getDeclaredField("capturedHeight");
                java.lang.reflect.Field bitmap = backdrop.getClass().getDeclaredField("backdrop");
                width.setAccessible(true);
                height.setAccessible(true);
                bitmap.setAccessible(true);
                Bitmap capture = (Bitmap) bitmap.get(backdrop);
                dimensions = width.getInt(backdrop) + "x" + height.getInt(backdrop)
                    + ", bitmap=" + (capture == null ? "null"
                    : capture.getWidth() + "x" + capture.getHeight());
            } catch (ReflectiveOperationException failure) {
                dimensions = failure.getClass().getSimpleName();
            }
            return "visible=" + visible + ", decor=" + decorPosition[0] + "," + decorPosition[1]
                + "," + decor.getWidth() + "x" + decor.getHeight() + ", anchor="
                + anchorPosition[0] + "," + anchorPosition[1] + "," + anchor.getWidth()
                + "x" + anchor.getHeight() + ", captured=" + dimensions;
        }
    }

    private static final class Pattern extends View {
        private final Paint paint = new Paint();

        Pattern(Context context) { super(context); }

        @Override protected void onDraw(Canvas canvas) {
            paint.setColor(Color.RED);
            canvas.drawRect(0, 0, 200, getHeight(), paint);
            paint.setColor(Color.BLUE);
            canvas.drawRect(200, 0, getWidth(), getHeight(), paint);
            paint.setColor(Color.GREEN);
            canvas.drawRect(0, 300, getWidth(), getHeight(), paint);
        }
    }
}
