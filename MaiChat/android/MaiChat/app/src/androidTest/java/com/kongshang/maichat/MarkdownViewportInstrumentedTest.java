// Actual Android text-engine and clipboard regression. JVM legacy shadows do
// not measure glyph wrapping reliably and are deliberately not used as proof.
package com.kongshang.maichat;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.text.Selection;
import android.text.Spannable;
import android.view.View;
import android.widget.HorizontalScrollView;
import android.widget.TextView;
import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.util.Collections;
import java.io.File;
import java.io.FileOutputStream;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import org.junit.Test;
import org.junit.runner.RunWith;
import static org.junit.Assert.*;

@RunWith(AndroidJUnit4.class)
public class MarkdownViewportInstrumentedTest {
    // Save only this isolated fixture's actual native pixels. User app windows
    // and clipboard contents never enter the evidence image.
    private static void captureFixture(Context context, View view, String name) {
        Bitmap image = Bitmap.createBitmap(view.getWidth(), view.getHeight(), Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(image);
        canvas.drawColor(android.graphics.Color.WHITE);
        view.draw(canvas);
        try (FileOutputStream output = new FileOutputStream(new File(context.getCacheDir(), name))) {
            assertTrue(image.compress(Bitmap.CompressFormat.PNG, 100, output));
        } catch (Exception error) {
            throw new AssertionError("Cannot capture native Markdown fixture", error);
        } finally {
            image.recycle();
        }
    }
    private static void prepare(String source) throws Exception {
        CountDownLatch done = new CountDownLatch(1);
        MarkdownRenderer.prepare(Collections.singletonList(source), () -> true, done::countDown);
        assertTrue("Prepared Markdown delivery", done.await(10, TimeUnit.SECONDS));
    }
    private static void narrowLayout(MarkdownContentView content) {
        content.measure(View.MeasureSpec.makeMeasureSpec(240, View.MeasureSpec.EXACTLY),
            View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED));
        content.layout(0, 0, content.getMeasuredWidth(), content.getMeasuredHeight());
    }

    @Test public void codeAppendDoesNotRestoreAnOffsetSupersededBeforeLayout() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> MarkdownRenderer.initialize(context));
        String source = "```text\nHEAD_" + String.join("", Collections.nCopies(24, "0123456789"));
        String next = source + "_APPENDED";
        prepare(source);
        prepare(next);
        Intent launch = new Intent(context, MarkdownPresentationTestActivity.class)
            .putExtra("markdown_fixture", source);
        try (ActivityScenario<MarkdownPresentationTestActivity> scenario = ActivityScenario.launch(launch)) {
            scenario.onActivity(activity -> {
                MarkdownContentView content = activity.content;
                narrowLayout(content);
                HorizontalScrollView viewport = (HorizontalScrollView) content.getChildAt(0);
                viewport.scrollTo(80, 0);
                assertEquals(80, viewport.getScrollX());
                MarkdownRenderer.bindStreaming(content, next);
                assertSame(viewport, content.getChildAt(0));
                viewport.scrollTo(180, 0);
                assertEquals(180, viewport.getScrollX());
                narrowLayout(content);
                assertEquals(180, viewport.getScrollX());
            });
        }
    }

    @Test public void narrowCodeScrollsWithoutBodyReflowAndCopiesOriginalWhitespace() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> MarkdownRenderer.initialize(context));
        String code = "    HEAD_" + String.join("", Collections.nCopies(24, "0123456789"))
            + "_TAIL_MARKER\n\n    保留原文空格：┌─┴─┐\n";
        String source = String.join("", Collections.nCopies(16, "普通正文需要正常换行。"))
            + "\n\n```text\n" + code + "```\n\nBody after";
        prepare(source);
        Intent launch = new Intent(context, MarkdownPresentationTestActivity.class)
            .putExtra("markdown_fixture", source);
        try (ActivityScenario<MarkdownPresentationTestActivity> scenario = ActivityScenario.launch(launch)) {
            HorizontalScrollView[] viewport = new HorizontalScrollView[1];
            TextView[] literal = new TextView[1];
            int[] previousOffset = new int[1];
            scenario.onActivity(activity -> {
                MarkdownContentView content = activity.content;
                narrowLayout(content);
                TextView body = (TextView) content.getChildAt(0);
                assertTrue("Normal body still wraps", body.getLayout().getLineCount() > 1);
                assertTrue(body.getWidth() <= content.getWidth());
                viewport[0] = (HorizontalScrollView) content.getChildAt(1);
                literal[0] = (TextView) viewport[0].getChildAt(0);
                assertTrue("Code owns overflow", literal[0].getWidth() > viewport[0].getWidth());
                int tail = code.indexOf("TAIL_MARKER");
                assertEquals("A single source line remains one native layout line",
                    literal[0].getLayout().getLineForOffset(4), literal[0].getLayout().getLineForOffset(tail + 10));
                assertEquals(code, literal[0].getText().toString());
                viewport[0].scrollTo(Integer.MAX_VALUE, 0);
                previousOffset[0] = viewport[0].getScrollX();
                assertTrue("Tail is accessible through a real local viewport", previousOffset[0] > 0);
                ClipboardManager clipboard = (ClipboardManager) activity.getSystemService(Context.CLIPBOARD_SERVICE);
                ClipData previous = clipboard.getPrimaryClip();
                try {
                    Selection.setSelection((Spannable) literal[0].getText(), 0, code.length());
                    assertTrue(literal[0].onTextContextMenuItem(android.R.id.copy));
                    assertNotNull(clipboard.getPrimaryClip());
                    assertEquals(code, clipboard.getPrimaryClip().getItemAt(0).coerceToText(activity).toString());
                    Selection.setSelection((Spannable) literal[0].getText(), tail, tail + 11);
                    assertTrue(literal[0].onTextContextMenuItem(android.R.id.copy));
                    assertEquals("TAIL_MARKER", clipboard.getPrimaryClip().getItemAt(0).coerceToText(activity).toString());
                } finally {
                    if (previous != null) clipboard.setPrimaryClip(previous);
                    else if (android.os.Build.VERSION.SDK_INT >= 28) clipboard.clearPrimaryClip();
                }
                Selection.setSelection((Spannable) literal[0].getText(), tail, tail + 11);
            });
            String next = source + "\n\nA new streamed body paragraph.";
            prepare(next);
            scenario.onActivity(activity -> {
                MarkdownRenderer.bindStreaming(activity.content, next);
                narrowLayout(activity.content);
                assertSame(viewport[0], activity.content.getChildAt(1));
                assertSame(literal[0], viewport[0].getChildAt(0));
                assertEquals(previousOffset[0], viewport[0].getScrollX());
                int tail = code.indexOf("TAIL_MARKER");
                assertEquals(tail, literal[0].getSelectionStart());
                assertEquals(tail + 11, literal[0].getSelectionEnd());
                assertEquals(next, activity.content.getMarkdown());
                captureFixture(activity, activity.content, "markdown-native-viewport.png");
            });
        }
    }
}
