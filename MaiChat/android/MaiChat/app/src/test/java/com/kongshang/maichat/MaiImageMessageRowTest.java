// 文件作用：通过真实 View 测量验证图片消息包裹自身、窄屏等比收缩及日期和状态不出界。
package com.kongshang.maichat;

import static org.junit.Assert.*;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.drawable.BitmapDrawable;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "mdpi", manifest = Config.NONE)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class MaiImageMessageRowTest {
    private static final class Fixture {
        final MaiImageMessageRow row;
        final LinearLayout bubble;
        final MaiImageThumbnailView image;
        final Bitmap bitmap;
        final TextView date;
        final TextView status;

        Fixture(boolean outgoing, int imageWidth, int imageHeight, String timestamp, String caption) {
            Activity activity = Robolectric.buildActivity(Activity.class).setup().get();
            row = new MaiImageMessageRow(activity);
            row.setGravity((outgoing ? Gravity.END : Gravity.START) | Gravity.BOTTOM);
            bubble = new LinearLayout(activity);
            bubble.setOrientation(LinearLayout.VERTICAL);
            LinearLayout imageBox = new LinearLayout(activity);
            imageBox.setOrientation(LinearLayout.VERTICAL);
            image = new MaiImageThumbnailView(activity);
            bitmap = Bitmap.createBitmap(imageWidth, imageHeight, Bitmap.Config.ARGB_8888);
            image.setImageBitmap(bitmap);
            imageBox.addView(image, new LinearLayout.LayoutParams(220, 180));
            bubble.addView(imageBox, new LinearLayout.LayoutParams(-2, -2));
            if (caption != null) {
                TextView captionView = new TextView(activity);
                captionView.setTextSize(16);
                captionView.setText(caption);
                bubble.addView(captionView, new LinearLayout.LayoutParams(-1, -2));
            }
            row.addView(bubble, new LinearLayout.LayoutParams(-2, -2));
            date = new TextView(activity);
            date.setTextSize(11);
            date.setText(timestamp);
            LinearLayout.LayoutParams dateParams = new LinearLayout.LayoutParams(-2, -2);
            dateParams.leftMargin = 6;
            row.addView(date, dateParams);
            status = outgoing ? new TextView(activity) : null;
            if (status != null) {
                status.setText("\u2713");
                LinearLayout.LayoutParams statusParams = new LinearLayout.LayoutParams(18, 24);
                statusParams.leftMargin = 8;
                row.addView(status, statusParams);
            }
        }

        void layout(int width) {
            row.forceLayout();
            row.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
                View.MeasureSpec.makeMeasureSpec(1200, View.MeasureSpec.AT_MOST));
            row.layout(0, 0, row.getMeasuredWidth(), row.getMeasuredHeight());
        }

        void assertContained() {
            assertTrue("Image must fit the message bubble", image.getWidth() <= bubble.getWidth());
            assertTrue("Date must follow the bubble", date.getLeft() >= bubble.getRight());
            assertTrue("Date must fit the row", date.getRight() <= row.getWidth());
            if (status != null) {
                assertTrue("Status must follow the date", status.getLeft() >= date.getRight());
                assertTrue("Status must fit the row", status.getRight() <= row.getWidth());
            }
        }

        void assertAspect(float aspect) {
            assertTrue("Image must retain a nonempty fitted size", image.getWidth() > 0 && image.getHeight() > 0);
            assertEquals("Image must retain its aspect ratio", image.getWidth(), image.getHeight() * aspect,
                Math.max(1f, aspect));
        }
    }

    @Test public void outgoingPortraitWrapsItsImageAndAlignsTheWholeGroupRight() {
        Fixture fixture = new Fixture(true, 100, 300, "\u00b7 23:13", null);
        fixture.layout(360);
        assertEquals(60, fixture.image.getWidth());
        assertEquals(180, fixture.image.getHeight());
        assertEquals("Bubble must not reserve an empty media column", fixture.image.getWidth(), fixture.bubble.getWidth());
        assertTrue(fixture.bubble.getLeft() > 0);
        assertEquals(fixture.row.getWidth(), fixture.status.getRight());
        fixture.assertContained();
    }

    @Test public void incomingPortraitWrapsItsImageAndStartsAtTheLeadingEdge() {
        Fixture fixture = new Fixture(false, 100, 300, "\u00b7 23:13", null);
        fixture.layout(360);
        assertEquals(0, fixture.bubble.getLeft());
        assertEquals(fixture.image.getWidth(), fixture.bubble.getWidth());
        fixture.assertContained();
    }

    @Test public void wideImageShrinksForALongDateAndKeepsTheStatusVisible() {
        Fixture fixture = new Fixture(true, 400, 100, "\u00b7 2026/11/04 23:13", null);
        fixture.layout(280);
        assertTrue("Image must yield room to metadata: image=" + fixture.image.getWidth()
            + " date=" + fixture.date.getWidth() + " status=" + fixture.status.getWidth()
            + " bubble=" + fixture.bubble.getWidth(), fixture.image.getWidth() < 220);
        fixture.assertAspect(4f);
        assertEquals(fixture.image.getWidth(), fixture.bubble.getWidth());
        fixture.assertContained();
    }

    @Test public void longCaptionWrapsWithinTheWidthLeftByMetadata() {
        Fixture fixture = new Fixture(true, 400, 200, "\u00b7 23:13",
            "A long caption with enough words to wrap on a narrow conversation pane.");
        fixture.layout(240);
        TextView caption = (TextView) fixture.bubble.getChildAt(1);
        assertTrue("Caption must wrap, rather than push the metadata out: lines=" + caption.getLineCount()
            + " width=" + caption.getWidth() + " textWidth=" + caption.getPaint().measureText(caption.getText().toString()),
            caption.getLineCount() > 1);
        fixture.assertAspect(2f);
        fixture.assertContained();
    }

    @Test public void quoteAndNestedPaddingAreIncludedInTheImageWidthBudget() {
        Fixture fixture = new Fixture(true, 400, 200, "\u00b7 23:13", null);
        fixture.bubble.setPadding(3, 0, 5, 0);
        TextView quote = new TextView(fixture.row.getContext());
        quote.setText("Quoted text must wrap within the same constrained bubble width.");
        fixture.bubble.addView(quote, 0, new LinearLayout.LayoutParams(-1, -2));
        fixture.layout(180);
        fixture.assertAspect(2f);
        fixture.assertContained();
        assertTrue(fixture.image.getWidth() + 8 <= fixture.bubble.getWidth());
        assertTrue(quote.getRight() <= fixture.bubble.getWidth() - fixture.bubble.getPaddingRight());
    }

    @Test public void expandingTheRowRestoresTheThumbnailMaximumWithoutLosingItsBitmap() {
        Fixture fixture = new Fixture(true, 400, 100, "\u00b7 23:13", null);
        fixture.layout(160);
        int narrowWidth = fixture.image.getWidth();
        fixture.layout(420);
        assertTrue(narrowWidth < fixture.image.getWidth());
        assertEquals(220, fixture.image.getWidth());
        assertEquals(55, fixture.image.getHeight());
        assertSame(fixture.bitmap, ((BitmapDrawable) fixture.image.getDrawable()).getBitmap());
        fixture.assertContained();
    }
}
