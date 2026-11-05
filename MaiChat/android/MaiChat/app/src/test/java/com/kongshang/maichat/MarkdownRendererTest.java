// 文件作用：Android 本地测试；MarkdownRendererTest 负责覆盖对应类的状态和数据行为，本文件给出其验证。
// 运行边界：仅供自动化验证使用，不作为正式 App 或 Agent 的用户功能入口。
// 覆盖场景：backgroundPreparationHandlesNestedMarkdownAndPreservesCodeEscapes。
package com.kongshang.maichat;

import android.os.Looper;
import android.widget.TextView;
import android.widget.HorizontalScrollView;
import android.view.View;
import android.text.Spanned;
import android.text.Spannable;
import android.text.Selection;
import android.text.style.URLSpan;
import androidx.test.core.app.ApplicationProvider;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.Shadows;
import org.robolectric.annotation.Config;
import java.util.Collections;
import java.util.concurrent.atomic.AtomicBoolean;
import static org.junit.Assert.*;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class MarkdownRendererTest {
    @Test public void codeUpdateYieldsToNewScrollBeforeItsPendingLayout() throws Exception {
        MarkdownRenderer.initialize(ApplicationProvider.getApplicationContext());
        String source = "```text\nAn open fenced code block", next = source + " with appended text";
        prepareReady(source);
        prepareReady(next);
        MarkdownContentView content = new MarkdownContentView(ApplicationProvider.getApplicationContext());
        MarkdownRenderer.bind(content, source);
        HorizontalScrollView viewport = (HorizontalScrollView) content.getChildAt(0);
        TextView code = (TextView) viewport.getChildAt(0);
        // Explicit geometry makes this a scroll-state test, independent of the
        // legacy Robolectric glyph layout that cannot prove code wrapping.
        code.setMinimumWidth(2400);
        final Runnable layout = () -> {
            content.measure(View.MeasureSpec.makeMeasureSpec(240, View.MeasureSpec.EXACTLY),
                View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED));
            content.layout(0, 0, content.getMeasuredWidth(), content.getMeasuredHeight());
        };
        layout.run();
        assertTrue(code.getWidth() > viewport.getWidth());
        viewport.scrollTo(80, 0);
        assertEquals(80, viewport.getScrollX());
        MarkdownRenderer.bindStreaming(content, next); // Captures 80 for pending layout.
        assertSame(viewport, content.getChildAt(0));
        viewport.scrollTo(180, 0); // New explicit intent wins over the cached position.
        assertEquals(180, viewport.getScrollX());
        layout.run();
        assertEquals(180, viewport.getScrollX());
    }

    @Test public void codeUsesLocalViewportAndRetainsSourceSelectionAndChildren() throws Exception {
        MarkdownRenderer.initialize(ApplicationProvider.getApplicationContext());
        String code = "    HEAD_" + String.join("", Collections.nCopies(20, "0123456789"))
            + "_TAIL_MARKER\n\n    保留代码空格 ┌─┴─┐\n";
        String source = "Body `inline` and [site](https://example.com)\n\n```text\n" + code + "```\n\nBody after";
        prepareReady(source);
        MarkdownContentView content = new MarkdownContentView(ApplicationProvider.getApplicationContext());
        MarkdownRenderer.bind(content, source);
        assertEquals(source, content.getMarkdown());
        assertEquals(3, content.getChildCount());
        assertTrue(content.getChildAt(0) instanceof TextView);
        assertTrue(content.getChildAt(1) instanceof HorizontalScrollView);
        assertTrue(content.getChildAt(2) instanceof TextView);
        TextView body = (TextView) content.getChildAt(0);
        assertTrue(body.isTextSelectable());
        Spanned bodyText = (Spanned) body.getText();
        assertEquals(1, bodyText.getSpans(0, bodyText.length(), URLSpan.class).length);
        assertEquals(1, bodyText.getSpans(0, bodyText.length(), io.noties.markwon.core.spans.CodeSpan.class).length);
        HorizontalScrollView viewport = (HorizontalScrollView) content.getChildAt(1);
        TextView literal = (TextView) viewport.getChildAt(0);
        assertTrue(literal.isTextSelectable());
        assertEquals(code, literal.getText().toString()); // No synthesized padding or rewritten spaces/LF.
        int start = code.indexOf("TAIL_MARKER");
        Selection.setSelection((Spannable) literal.getText(), start, start + 11);
        String next = source + "\n\nA streamed follow-up.";
        prepareReady(next);
        MarkdownRenderer.bindStreaming(content, next);
        assertSame(viewport, content.getChildAt(1));
        assertSame(literal, viewport.getChildAt(0));
        assertEquals(start, literal.getSelectionStart());
        assertEquals(start + 11, literal.getSelectionEnd());
        assertEquals("TAIL_MARKER", literal.getText().subSequence(
            literal.getSelectionStart(), literal.getSelectionEnd()).toString());
        assertEquals(next, content.getMarkdown());
    }

    @Test public void localCodeDoesNotLoseBodyTableQuoteOrTimestampSpans() throws Exception {
        MarkdownRenderer.initialize(ApplicationProvider.getApplicationContext());
        String source = "> Quoted body\n\n| A | B |\n|---|---|\n|one|two|\n\n```\ncode\n```";
        prepareReady(source);
        MarkdownContentView content = new MarkdownContentView(ApplicationProvider.getApplicationContext());
        MarkdownRenderer.bind(content, source);
        assertTrue(content.getChildAt(0) instanceof TextView);
        TextView body = (TextView) content.getChildAt(0);
        Spanned text = (Spanned) body.getText();
        assertEquals(1, text.getSpans(0, text.length(), io.noties.markwon.core.spans.BlockQuoteSpan.class).length);
        boolean table = false;
        for (Object span : text.getSpans(0, text.length(), Object.class))
            table |= span.getClass().getName().contains(".tables.");
        assertTrue("Body retains the table renderer spans", table);
        long timestamp = 1793880000000L;
        AtomicBoolean done = new AtomicBoolean();
        MarkdownRenderer.preparePresentations(Collections.singletonList(
            new MarkdownRenderer.Presentation(source, timestamp)), () -> true, () -> done.set(true));
        await(done);
        MarkdownRenderer.bind(content, source, timestamp);
        assertTrue(content.getChildAt(content.getChildCount() - 1) instanceof TextView);
        TextView note = (TextView) content.getChildAt(content.getChildCount() - 1);
        assertTrue(note.getText().toString().contains(RemoteIMTimestampFormatter.format(timestamp)));
        assertEquals(source, content.getMarkdown());
    }

    @Test public void obsoleteAsyncPresentationCannotOverwriteReboundDocument() throws Exception {
        MarkdownRenderer.initialize(ApplicationProvider.getApplicationContext());
        MarkdownContentView content = new MarkdownContentView(ApplicationProvider.getApplicationContext());
        String old = "old owner\n\n```\nold code\n```", next = "new owner\n\n```\nnew code\n```";
        MarkdownRenderer.bind(content, old);
        MarkdownRenderer.bind(content, next);
        prepareReady(next); // Runs after both enqueued parses and flushes their main-thread deliveries.
        assertEquals(next, content.getMarkdown());
        assertFalse(content.getText().toString().contains("old owner"));
        assertTrue(content.getText().toString().contains("new owner"));
    }

    private static void prepareReady(String source) throws Exception {
        AtomicBoolean completed = new AtomicBoolean();
        MarkdownRenderer.prepare(Collections.singletonList(source), () -> true, () -> completed.set(true));
        await(completed);
    }
    private static void await(AtomicBoolean completed) throws Exception {
        long deadline = System.nanoTime() + 3_000_000_000L;
        while (!completed.get() && System.nanoTime() < deadline) {
            Shadows.shadowOf(Looper.getMainLooper()).idle(); Thread.sleep(5);
        }
        assertTrue("Preparation must deliver its result", completed.get());
    }

    @Test public void backgroundPreparationHandlesNestedMarkdownAndPreservesCodeEscapes() throws Exception {
        MarkdownRenderer.initialize(ApplicationProvider.getApplicationContext());
        String source = "# Heading\n\n- **bold** and [site](https://example.com)\n  - child\n\n`literal "
                        + "\\n`\n\n~~removed~~\n\n| A | B |\n|---|---|\n|one|two|";
        AtomicBoolean completed = new AtomicBoolean();
        MarkdownRenderer.prepare(Collections.singletonList(source), () -> true, () -> completed.set(true));
        long deadline = System.nanoTime() + 3_000_000_000L;
        while (!completed.get() && System.nanoTime() < deadline) {
            Shadows.shadowOf(Looper.getMainLooper()).idle(); Thread.sleep(5);
        }
        assertTrue("Preparation must deliver its result", completed.get());
        TextView text = new TextView(ApplicationProvider.getApplicationContext());
        MarkdownRenderer.bind(text, source);
        String rendered = text.getText().toString();
        assertTrue(rendered.contains("Heading")); assertTrue(rendered.contains("child"));
        assertTrue(rendered.contains("literal \\n")); assertFalse(rendered.contains("**bold**"));
        assertFalse(rendered.contains("[site](https://example.com)"));
        assertFalse(rendered.contains("|---|---|"));
    }
}
