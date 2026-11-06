// 文件作用：Android 聊天会话的消息呈现与输入；MarkdownRenderer 负责内容渲染和资源生命周期，本文件给出其实现。
// 主要入口：cacheKey、initialize、prepare、preparePresentations。
package com.kongshang.maichat;

import android.content.Context;
import android.graphics.Typeface;
import android.os.Handler;
import android.os.Looper;
import android.text.Spanned;
import android.util.LruCache;
import android.widget.TextView;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.function.BooleanSupplier;
import io.noties.markwon.Markwon;
import io.noties.markwon.ext.tables.TablePlugin;
import io.noties.markwon.ext.tables.TableAwareMovementMethod;
import io.noties.markwon.ext.strikethrough.StrikethroughPlugin;
import io.noties.markwon.ext.tasklist.TaskListPlugin;
import io.noties.markwon.linkify.LinkifyPlugin;
import io.noties.markwon.movement.MovementMethodPlugin;

/** CommonMark/GFM parsing is serialized off the UI thread; only applying spans touches views. */
public final class MarkdownRenderer {
    private static final ExecutorService worker = Executors.newSingleThreadExecutor();
    private static final Handler main = new Handler(Looper.getMainLooper());
    private static volatile Markwon markwon;
    private static volatile Typeface codeFace;
    private static float density, scaledDensity;
    private static boolean dark;
    private static String fontFamily = "";
    private static int generation;
    static final class Presentation {
        final String text; final Long timestamp;
        Presentation(String text, Long timestamp) { this.text = text; this.timestamp = timestamp; }
    }
    private static String cacheKey(String text, Long timestamp) {
        if (timestamp == null) return generation + ":text:" + source(text);
        long now = System.currentTimeMillis();
        long day = (now + java.util.TimeZone.getDefault().getOffset(now)) / 86400000;
        return generation + ":date:" + timestamp + ":" + day + "\0" + source(text);
    }
    private static final LruCache<String, Spanned> cache = new LruCache<String, Spanned>(2 * 1024 * 1024) {
        @Override
        protected int sizeOf(String key, Spanned value) {
            return Math.max(1, (key.length() + value.length()) * 2);
        }
    };
    private MarkdownRenderer() { }
    static synchronized Typeface codeTypeface(Context context) {
        if (codeFace == null) {
            // Fixed has no programming ligatures and covers ASCII/box drawing
            // at one cell and CJK at two, without device-dependent fallback.
            codeFace = Typeface.createFromAsset(context.getApplicationContext().getAssets(),
                    "fonts/SarasaFixedSC-Regular.ttf");
        }
        return codeFace;
    }
    public static synchronized void initialize(Context context) {
        float nextDensity = context.getResources().getDisplayMetrics().density;
        float nextScale = nextDensity * context.getResources().getConfiguration().fontScale;
        if (markwon != null && density == nextDensity && scaledDensity == nextScale && dark == MaiChatTheme.isDark() && fontFamily.equals(MaiChatTypography.familyId())) return;
        density = nextDensity; scaledDensity = nextScale; dark = MaiChatTheme.isDark(); fontFamily = MaiChatTypography.familyId(); ++generation; cache.evictAll();
        Typeface fixedCode = codeTypeface(context);
        markwon = Markwon.builder(context.getApplicationContext())
            .usePlugin(new io.noties.markwon.AbstractMarkwonPlugin() {
                @Override public void configureVisitor(io.noties.markwon.MarkwonVisitor.Builder builder) {
                    // 只处理 CommonMark 的真实 HTML 节点，代码、转义和链接地址仍保留原意。
                    // 不启用整个 HTML 插件，避免其它标签改变聊天排版或请求外部资源。
                    builder.on(org.commonmark.node.HtmlInline.class, (visitor, node) -> {
                        String literal = node.getLiteral();
                        visitor.builder().append(literal.matches("(?i)<br[ \\t]*/?>") ? "\n" : literal);
                    });
                }
                @Override public void configureTheme(io.noties.markwon.core.MarkwonTheme.Builder theme) {
                    theme.linkColor(dark ? MaiChatTypographyTokens.LINK_DARK_COLOR : MaiChatTypographyTokens.LINK_COLOR)
                            .isLinkUnderlined(false)
                            .listItemColor(MaiChatTheme.BLUE)
                            .blockMargin(Math.round(24 * density))
                            .bulletWidth(Math.round(3 * density))
                            .headingTypeface(MaiChatTypography.semibold())
                            .headingBreakHeight(0)
                            .headingTextSizeMultipliers(new float[] {MaiChatTypographyTokens.headingRatio(1), MaiChatTypographyTokens.headingRatio(2), MaiChatTypographyTokens.headingRatio(3), 1, 1, 1})
                            .codeTypeface(fixedCode)
                            .codeTextSize(Math.round(MaiChatTypographyTokens.Role.DETAIL.points * scaledDensity))
                            .codeTextColor(dark ? MaiChatTypographyTokens.CODE_DARK_COLOR : MaiChatTypographyTokens.CODE_COLOR)
                            .codeBackgroundColor(dark ? MaiChatTypographyTokens.INLINECODEBACKGROUND_DARK_COLOR : MaiChatTypographyTokens.INLINECODEBACKGROUND_COLOR)
                            .codeBlockTypeface(fixedCode)
                            .codeBlockTextSize(Math.round(MaiChatTypographyTokens.Role.DETAIL.points * scaledDensity))
                            .codeBlockTextColor(MaiChatTheme.TEXT)
                            .codeBlockBackgroundColor(dark ? MaiChatTypographyTokens.CODEBACKGROUND_DARK_COLOR : MaiChatTypographyTokens.CODEBACKGROUND_COLOR)
                            .codeBlockMargin(Math.round(12 * density))
                            .blockQuoteColor(MaiChatTheme.BLUE)
                            .blockQuoteWidth(Math.round(3 * density))
                            .thematicBreakColor(MaiChatTheme.BORDER);
                }
                @Override public void configureSpansFactory(io.noties.markwon.MarkwonSpansFactory.Builder builder) {
                    builder.appendFactory(org.commonmark.node.Heading.class,
                            (configuration, props)
                                    -> new android.text.style.ForegroundColorSpan(
                                            io.noties.markwon.core.CoreProps.HEADING_LEVEL.require(props) <= 2
                                                    ? (dark ? MaiChatTypographyTokens.HEADING_DARK_COLOR : MaiChatTypographyTokens.HEADING_COLOR)
                                                    : MaiChatTheme.TEXT));
                    io.noties.markwon.SpanFactory fallback = builder.requireFactory(org.commonmark.node.ListItem.class);
                    builder.setFactory(org.commonmark.node.ListItem.class, (configuration, props) -> {
                        if (io.noties.markwon.core.CoreProps.LIST_ITEM_TYPE.require(props)
                                == io.noties.markwon.core.CoreProps.ListItemType.BULLET) {
                            return new MaiChatTypography.BlueBulletSpan(
                                    density, io.noties.markwon.core.CoreProps.BULLET_LIST_ITEM_LEVEL.require(props));
                        }
                        return fallback.getSpans(configuration, props);
                    });
                }
            })
            .usePlugin(TablePlugin.create(builder -> builder
                .tableCellPadding(Math.round(MaiChatTypographyTokens.Role.BODY.points * MaiChatTypographyTokens.TABLEPADDINGH_RATIO * scaledDensity))
                .tableBorderColor(MaiChatTheme.BORDER).tableBorderWidth(Math.max(1, Math.round(density)))
                .tableHeaderRowBackgroundColor(dark ? MaiChatTypographyTokens.TABLEHEADER_DARK_COLOR : MaiChatTypographyTokens.TABLEHEADER_COLOR)
                .tableOddRowBackgroundColor(MaiChatTheme.PANEL)
                .tableEvenRowBackgroundColor(dark ? MaiChatTypographyTokens.TABLEALTERNATE_DARK_COLOR : MaiChatTypographyTokens.TABLEALTERNATE_COLOR)))
            .usePlugin(StrikethroughPlugin.create())
            .usePlugin(TaskListPlugin.create(context.getApplicationContext()))
            .usePlugin(LinkifyPlugin.create())
            .usePlugin(MovementMethodPlugin.create(TableAwareMovementMethod.create()))
            .build();
    }
    private static String source(String value) { return value == null ? "" : value.replace("\r\n", "\n"); }
    private static Spanned prepare(String value, Long timestamp) {
        if (Looper.myLooper() == Looper.getMainLooper())
            throw new IllegalStateException("Markdown parsing on UI thread");
        String key = cacheKey(value, timestamp);
        Spanned ready = cache.get(key);
        if (ready == null) {
            try { ready = markwon.toMarkdown(source(value)); }
            catch (RuntimeException | StackOverflowError error) {
                android.util.Log.w("MaiChat.markdown", "Could not render message; showing plain text", error);
                ready = new android.text.SpannedString(source(value));
            }
            android.text.SpannableStringBuilder styled = new android.text.SpannableStringBuilder(ready);
            java.util.regex.Matcher note =
                    java.util.regex.Pattern.compile("(?m)^此消息来自\\s*imcli[。.]?\\s*$").matcher(styled);
            while (note.find()) {
                if (styled.getSpans(note.start(), note.end(), io.noties.markwon.core.spans.CodeBlockSpan.class).length
                        > 0)
                    continue;
                styled.setSpan(new android.text.style.ForegroundColorSpan(MaiChatTheme.BLUE), note.start(), note.end(),
                        Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
                styled.setSpan(new android.text.style.BackgroundColorSpan(MaiChatTheme.BLUE_SOFT), note.start(),
                        note.end(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
                styled.setSpan(new android.text.style.AbsoluteSizeSpan(Math.round(MaiChatTypographyTokens.Role.METADATA.points * scaledDensity)), note.start(),
                        note.end(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
                styled.setSpan(new MaiChatTypography.SemiboldSpan(), note.start(), note.end(),
                        Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
            }
            if (timestamp != null) {
                while (styled.length() > 0 && Character.isWhitespace(styled.charAt(styled.length() - 1)))
                    styled.delete(styled.length() - 1, styled.length());
                int start = styled.length(); styled.append("  · ").append(RemoteIMTimestampFormatter.format(timestamp));
                styled.setSpan(new android.text.style.ForegroundColorSpan(MaiChatTheme.BLUE), start, styled.length(),
                        Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
                styled.setSpan(new android.text.style.AbsoluteSizeSpan(Math.round(MaiChatTypographyTokens.Role.METADATA.points * scaledDensity)), start,
                        styled.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
                styled.setSpan(
                        new MaiChatTypography.SemiboldSpan(), start, styled.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
            }
            ready = styled;
            cache.put(key, ready);
        }
        return ready;
    }
    public static void prepare(List<String> sources, BooleanSupplier current, Runnable completion) {
        java.util.List<Presentation> items = new java.util.ArrayList<>();
        for (String text : sources) items.add(new Presentation(text, null));
        preparePresentations(items, current, completion);
    }
    static void preparePresentations(List<Presentation> sources, BooleanSupplier current, Runnable completion) {
        worker.execute(() -> {
            for (Presentation item : sources) {
                if (!current.getAsBoolean())
                    return;
                prepare(item.text, item.timestamp);
            }
            main.post(() -> { if (current.getAsBoolean()) completion.run(); });
        });
    }
    public static void bind(TextView view, String value) { bind(view, value, null); }
    // Mixed messages use local code viewports. Legacy TextView callers remain
    // suitable for bounded detail/summary text without changing their hosts.
    public static void bind(MarkdownContentView view, String value) { bind(view, value, null); }
    static void bindStreaming(MarkdownContentView view, String value) { bind(view, value, null); }
    static void bind(MarkdownContentView view, String value, Long timestamp) {
        Object token = view.beginPresentation(value);
        Spanned ready = cache.get(cacheKey(value, timestamp));
        if (ready != null) { view.applyPresentation(token, ready, markwon); return; }
        worker.execute(() -> {
            Spanned parsed = prepare(value, timestamp);
            main.post(() -> view.applyPresentation(token, parsed, markwon));
        });
    }
    static void bindStreaming(TextView view, String value) { bind(view, value, null, true); }
    static void bind(TextView view, String value, Long timestamp) { bind(view, value, timestamp, false); }
    private static void bind(TextView view, String value, Long timestamp, boolean retainPrevious) {
        initialize(view.getContext());
        String key = cacheKey(value, timestamp);
        Object token = new Object(); view.setTag(token);
        Spanned ready = cache.get(key);
        if (ready != null) { markwon.setParsedMarkdown(view, ready); return; }
        if (!retainPrevious || view.length() == 0) view.setText(value);
        worker.execute(() -> {
            Spanned parsed = prepare(value, timestamp);
            main.post(() -> { if (view.getTag() == token) markwon.setParsedMarkdown(view, parsed); });
        });
    }
}
