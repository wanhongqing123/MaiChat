// Mixed Markdown keeps normal body wrapping while each code block owns its
// horizontal viewport. IM and Agent use the same stable child-view lifecycle.
package com.kongshang.maichat;

import android.content.Context;
import android.graphics.Color;
import android.text.Selection;
import android.text.Spannable;
import android.text.SpannableStringBuilder;
import android.text.Spanned;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.TextView;
import io.noties.markwon.Markwon;
import io.noties.markwon.core.spans.CodeBlockSpan;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.List;

public final class MarkdownContentView extends LinearLayout {
    static final class Chunk {
        final boolean code;
        final Spanned text;
        Chunk(boolean code, Spanned text) { this.code = code; this.text = text; }
    }

    private static final class CodeViewport extends HorizontalScrollView {
        private int pendingScroll = -1;
        private boolean layingOut;
        CodeViewport(Context context) {
            super(context);
            setFillViewport(true);
            setFocusable(false);
            setHorizontalScrollBarEnabled(true);
            setScrollbarFadingEnabled(false);
            setBackground(MaiChatTheme.bordered(Color.rgb(244, 246, 249), MaiChatTheme.BORDER, 8, context));
        }
        void preservePositionForLayout() { if (pendingScroll < 0) pendingScroll = getScrollX(); }
        @Override protected void onSizeChanged(int width, int height, int oldWidth, int oldHeight) {
            // ViewGroup.layout is final in the Android SDK. Guard the native
            // resize/focus and layout callbacks separately so their own scrolls
            // cannot replace a pending user intent.
            boolean previous = layingOut;
            layingOut = true;
            try { super.onSizeChanged(width, height, oldWidth, oldHeight); }
            finally { layingOut = previous; }
        }
        @Override protected void onScrollChanged(int left, int top, int oldLeft, int oldTop) {
            // A gesture, fling or explicit scroll before layout supersedes the
            // captured offset. Still restore this newest position after native
            // layout clamping/focus, rather than replaying the earlier offset.
            if (!layingOut && pendingScroll >= 0 && left != oldLeft) pendingScroll = left;
            super.onScrollChanged(left, top, oldLeft, oldTop);
        }
        @Override protected void onLayout(boolean changed, int left, int top, int right, int bottom) {
            boolean previous = layingOut;
            layingOut = true;
            try {
                super.onLayout(changed, left, top, right, bottom);
                if (pendingScroll >= 0) {
                    // No posted callback can later overwrite a newer gesture or
                    // recycled message. Native scrollTo also clamps after resize.
                    scrollTo(pendingScroll, 0);
                    pendingScroll = -1;
                }
            } finally { layingOut = previous; }
        }
    }

    private static final class Row {
        final boolean code;
        final TextView text;
        final View root;
        final CodeViewport viewport;
        Row(Context context, boolean code) {
            this.code = code;
            text = MaiChatTypography.body(context);
            text.setGravity(Gravity.TOP | Gravity.START);
            if (code) {
                text.setTypeface(MarkdownRenderer.codeTypeface(context));
                text.setTextSize(12);
                text.setLineSpacing(0, 1.4f);
                text.setLetterSpacing(0);
                text.setFontFeatureSettings("'kern' 0, 'liga' 0, 'clig' 0, 'calt' 0");
                text.setHorizontallyScrolling(true);
                int padding = MaiChatTheme.dp(context, 12);
                text.setPadding(padding, padding, padding, padding);
                viewport = new CodeViewport(context);
                viewport.addView(text, new HorizontalScrollView.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));
                root = viewport;
            } else {
                viewport = null;
                root = text;
            }
        }
    }

    private final List<Row> rows = new ArrayList<>();
    private String markdown = "";
    private CharSequence rendered = "";
    private Object binding;

    public MarkdownContentView(Context context) {
        super(context);
        setOrientation(VERTICAL);
        setClipToPadding(false);
    }

    /** Original Markdown, including code whitespace, remains available for raw copy. */
    public String getMarkdown() { return markdown; }
    public CharSequence getText() { return rendered; }

    Object beginPresentation(String source) {
        String next = source == null ? "" : source;
        // Appending a streamed answer retains children, code offsets and native
        // selections. Reusing the container for another document resets them.
        if (!next.startsWith(markdown)) {
            rows.clear();
            removeAllViews();
            rendered = "";
        }
        markdown = next;
        binding = new Object();
        return binding;
    }

    boolean isCurrentPresentation(Object token) { return binding == token; }

    void applyPresentation(Object token, Spanned presentation, Markwon markwon) {
        if (!isCurrentPresentation(token)) return;
        rendered = presentation;
        List<Chunk> chunks = split(presentation);
        for (int index = 0; index < chunks.size(); ++index) {
            Chunk chunk = chunks.get(index);
            Row row = index < rows.size() ? rows.get(index) : null;
            if (row == null || row.code != chunk.code) {
                if (row != null) { removeView(row.root); rows.remove(index); }
                row = new Row(getContext(), chunk.code);
                rows.add(index, row);
                addView(row.root, index, new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            }
            // Code has no inline styling; skip an identical block entirely so
            // a later body delta cannot dismiss its active selection handles.
            if (row.code && android.text.TextUtils.equals(row.text.getText(), chunk.text)) continue;
            int anchor = row.text.getSelectionStart(), cursor = row.text.getSelectionEnd();
            if (row.viewport != null) row.viewport.preservePositionForLayout();
            // Code is literal: native selectable-text movement must not be
            // replaced by the body plugin's link/table movement handler.
            if (row.code) row.text.setText(chunk.text, TextView.BufferType.SPANNABLE);
            else markwon.setParsedMarkdown(row.text, chunk.text);
            if (anchor >= 0 && cursor >= 0 && row.text.getText() instanceof Spannable) {
                int length = row.text.length();
                Selection.setSelection((Spannable) row.text.getText(), Math.min(anchor, length), Math.min(cursor, length));
            }
        }
        while (rows.size() > chunks.size()) removeView(rows.remove(rows.size() - 1).root);
    }

    static List<Chunk> split(Spanned presentation) {
        List<Chunk> chunks = new ArrayList<>();
        CodeBlockSpan[] code = presentation.getSpans(0, presentation.length(), CodeBlockSpan.class);
        Arrays.sort(code, Comparator.comparingInt(presentation::getSpanStart));
        int cursor = 0;
        for (CodeBlockSpan span : code) {
            int start = presentation.getSpanStart(span), end = presentation.getSpanEnd(span);
            if (start < cursor || end <= start) continue;
            if (cursor < start) chunks.add(new Chunk(false,
                new android.text.SpannedString(presentation.subSequence(cursor, start))));
            int literalStart = start, literalEnd = end;
            // Markwon 4.6.2 CorePlugin.visitCodeBlock inserts NBSP+LF before the
            // literal and NBSP after it to paint padding inside one TextView.
            // The local viewport supplies that padding instead. Remove only
            // those exact sentinels, never the user's code spaces or newlines.
            if (literalEnd - literalStart >= 2 && presentation.charAt(literalStart) == '\u00a0'
                    && presentation.charAt(literalStart + 1) == '\n') literalStart += 2;
            if (literalEnd > literalStart && presentation.charAt(literalEnd - 1) == '\u00a0') --literalEnd;
            SpannableStringBuilder literal = new SpannableStringBuilder(
                presentation.subSequence(literalStart, literalEnd));
            for (CodeBlockSpan decoration : literal.getSpans(0, literal.length(), CodeBlockSpan.class))
                literal.removeSpan(decoration);
            chunks.add(new Chunk(true, new android.text.SpannedString(literal)));
            cursor = end;
        }
        if (cursor < presentation.length()) chunks.add(new Chunk(false,
            new android.text.SpannedString(presentation.subSequence(cursor, presentation.length()))));
        return chunks;
    }
}
