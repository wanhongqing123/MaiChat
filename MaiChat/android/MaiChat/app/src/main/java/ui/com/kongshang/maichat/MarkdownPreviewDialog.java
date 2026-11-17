// Shared, cancelable document preview for IM attachments and Agent-delivered Markdown.
package com.kongshang.maichat;

import android.app.Activity;
import android.app.Dialog;
import android.content.ClipData;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.view.ViewGroup;
import android.view.Gravity;
import android.view.Window;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import androidx.core.content.FileProvider;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.Collections;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.BooleanSupplier;

/** Uses the ordinary Markdown renderer; no WebView, image fetcher or alternate parser. */
final class MarkdownPreviewDialog {
    static final int MAX_BYTES = 8 * 1024 * 1024;
    private static final ExecutorService worker = Executors.newSingleThreadExecutor();
    private static final Handler main = new Handler(Looper.getMainLooper());

    private MarkdownPreviewDialog() { }

    static boolean isMarkdown(String mime, String name) {
        String type = mime == null ? "" : mime.toLowerCase(Locale.ROOT);
        String lower = name == null ? "" : name.toLowerCase(Locale.ROOT);
        return type.contains("markdown") || lower.endsWith(".md") || lower.endsWith(".markdown");
    }

    static void show(Activity activity, File file, File allowedRoot, String displayName) {
        show(activity, file, allowedRoot, displayName,
            isMarkdown("", displayName) ? "text/markdown" : "text/plain");
    }

    static void show(Activity activity, File file, File allowedRoot, String displayName, String mimeType) {
        AtomicBoolean canceled = new AtomicBoolean();
        BooleanSupplier current = () -> !canceled.get() && !activity.isFinishing()
            && !activity.isDestroyed();
        java.util.concurrent.atomic.AtomicReference<Future<?>> sharing = new java.util.concurrent.atomic.AtomicReference<>();
        TextView text = MaiChatTheme.text(activity, "正在打开…", MaiChatTypographyTokens.Role.DETAIL,
            MaiChatTheme.TEXT);
        text.setTextIsSelectable(true);
        text.setPadding(MaiChatTheme.dp(activity, 16), MaiChatTheme.dp(activity, 14),
            MaiChatTheme.dp(activity, 16), MaiChatTheme.dp(activity, 18));
        ScrollView scroll = new ScrollView(activity);
        scroll.addView(text, new ViewGroup.LayoutParams(-1, -2));
        // Preserve the existing IM document frame, dimensions and close action when
        // extracting it for creation cards; only the bounded read lifecycle changes.
        Dialog dialog = new Dialog(activity);
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
        LinearLayout frame = new LinearLayout(activity);
        frame.setId(android.R.id.content);
        frame.setOrientation(LinearLayout.VERTICAL);
        frame.setBackgroundColor(MaiChatTheme.PANEL);
        TextView header = MaiChatTheme.label(activity, displayName,
            MaiChatTypographyTokens.Role.BODY, MaiChatTheme.TEXT);
        header.setPadding(MaiChatTheme.dp(activity, 16), 0, MaiChatTheme.dp(activity, 16), 0);
        frame.addView(header, new LinearLayout.LayoutParams(-1, MaiChatTheme.dp(activity, 52)));
        frame.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));
        LinearLayout actions = new LinearLayout(activity);
        Button share = actionButton(activity, "分享");
        share.setOnClickListener(view -> {
            if (!current.getAsBoolean()) return;
            share.setEnabled(false);
            sharing.set(worker.submit(() -> {
                try {
                    // Sharing does not read document contents and remains available even if
                    // the full preview is over its byte limit or cannot render the text.
                    File canonical = validate(file, allowedRoot);
                    main.post(() -> {
                        if (!current.getAsBoolean()) return;
                        try {
                            Uri uri = FileProvider.getUriForFile(activity,
                                activity.getPackageName() + ".files", canonical);
                            Intent send = new Intent(Intent.ACTION_SEND);
                            send.setType(mimeType);
                            send.putExtra(Intent.EXTRA_STREAM, uri);
                            send.setClipData(ClipData.newUri(activity.getContentResolver(), canonical.getName(), uri));
                            send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                            activity.startActivity(Intent.createChooser(send, "分享文件"));
                        } catch (RuntimeException failure) {
                            Toast.makeText(activity, "无法分享文件", Toast.LENGTH_SHORT).show();
                        } finally { share.setEnabled(true); }
                    });
                } catch (IOException failure) {
                    main.post(() -> {
                        if (current.getAsBoolean()) {
                            share.setEnabled(true);
                            Toast.makeText(activity, "文件暂不可分享", Toast.LENGTH_SHORT).show();
                        }
                    });
                }
            }));
        });
        actions.addView(share, new LinearLayout.LayoutParams(0, -1, 1));
        Button close = actionButton(activity, "关闭");
        close.setOnClickListener(view -> dialog.dismiss());
        LinearLayout.LayoutParams closeAction = new LinearLayout.LayoutParams(0, -1, 1);
        closeAction.leftMargin = MaiChatTheme.dp(activity, 8);
        actions.addView(close, closeAction);
        LinearLayout.LayoutParams closeLayout = new LinearLayout.LayoutParams(-1, MaiChatTheme.dp(activity, 46));
        closeLayout.setMargins(MaiChatTheme.dp(activity, 16), MaiChatTheme.dp(activity, 8),
            MaiChatTheme.dp(activity, 16), MaiChatTheme.dp(activity, 12));
        frame.addView(actions, closeLayout);
        dialog.setContentView(frame);
        MarkdownRenderer.initialize(activity);
        dialog.show();
        if (dialog.getWindow() != null) dialog.getWindow().setLayout(
            (int) (activity.getResources().getDisplayMetrics().widthPixels * .94f),
            (int) (activity.getResources().getDisplayMetrics().heightPixels * .88f));
        Future<?> reading = worker.submit(() -> {
            try {
                String source = read(file, allowedRoot, MAX_BYTES, current);
                if (!current.getAsBoolean()) return;
                MarkdownRenderer.prepare(Collections.singletonList(source), current, () -> {
                    if (current.getAsBoolean() && dialog.isShowing()) MarkdownRenderer.bind(text, source);
                });
            } catch (TooLarge failure) {
                main.post(() -> { if (current.getAsBoolean()) text.setText("文件过大，无法预览"); });
            } catch (IOException failure) {
                // Keep the existing IM text-preview error/UTF-8 replacement semantics.
                main.post(() -> { if (current.getAsBoolean()) text.setText("文件暂不可预览"); });
            }
        });
        dialog.setOnDismissListener(ignored -> {
            canceled.set(true);
            reading.cancel(true);
            Future<?> pendingShare = sharing.get();
            if (pendingShare != null) pendingShare.cancel(true);
        });
    }

    private static Button actionButton(Activity activity, String title) {
        Button button = MaiChatTypography.button(activity);
        button.setText(title);
        MaiChatTypography.apply(button, MaiChatTypographyTokens.Role.DETAIL);
        button.setTextColor(MaiChatTheme.TEXT);
        button.setAllCaps(false);
        button.setGravity(Gravity.CENTER);
        button.setPadding(MaiChatTheme.dp(activity, 12), 0, MaiChatTheme.dp(activity, 12), 0);
        button.setBackground(MaiChatTheme.bordered(Color.rgb(245, 247, 250), MaiChatTheme.BORDER, 9, activity));
        return button;
    }

    static File validate(File file, File allowedRoot) throws IOException {
        return DocumentThumbnailSnapshot.validate(file, allowedRoot);
    }

    /** Read the complete bounded file. Never label a silently truncated prefix as a full preview. */
    static String read(File file, File allowedRoot, int limit, BooleanSupplier current) throws IOException {
        return new String(readBytes(file, allowedRoot, limit, current), StandardCharsets.UTF_8);
    }

    static byte[] readBytes(File file, File allowedRoot, int limit, BooleanSupplier current) throws IOException {
        try { return DocumentThumbnailSnapshot.readBytes(file, allowedRoot, limit, current); }
        catch (DocumentThumbnailSnapshot.TooLarge tooLarge) { throw new TooLarge(); }
    }

    static final class TooLarge extends IOException { }
}
