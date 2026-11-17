// 文件作用：Android 跨页面复用的界面组件；PdfPreviewDialog 负责对话框呈现、确认交互和结果回传，本文件给出其实现。
// 主要入口：show、open、validate、requestPage。
package com.kongshang.maichat;

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.Matrix;
import android.graphics.pdf.PdfRenderer;
import android.os.ParcelFileDescriptor;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

/** In-app, paginated PDF preview shared by IM attachments and Agent artifacts. */
final class PdfPreviewDialog {
    private static final long MAX_BYTES = 100L * 1024 * 1024;
    private final Activity activity;
    private final File file, workspace;
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final AtomicBoolean closed = new AtomicBoolean();
    private final ImageView image;
    private final TextView pageLabel;
    private final Button previous, next;
    private final AlertDialog dialog;
    private ParcelFileDescriptor descriptor;
    private PdfRenderer renderer;
    private volatile int currentPage = -1, pageCount;
    private Bitmap shownBitmap;

    static void show(Activity activity, File file, File workspace) {
        show(activity, file, workspace, file == null ? null : file.getName());
    }

    static void show(Activity activity, File file, File workspace, String displayName) {
        new PdfPreviewDialog(activity, file, workspace, displayName).open();
    }

    private PdfPreviewDialog(Activity activity, File file, File workspace, String displayName) {
        this.activity = activity;
        this.file = file;
        this.workspace = workspace;
        LinearLayout layout = new LinearLayout(activity);
        layout.setOrientation(LinearLayout.VERTICAL);
        int padding = MaiChatTheme.dp(activity, 12);
        layout.setPadding(padding, padding, padding, padding);
        image = new ImageView(activity);
        image.setScaleType(ImageView.ScaleType.FIT_CENTER);
        layout.addView(image, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        LinearLayout controls = new LinearLayout(activity);
        controls.setGravity(Gravity.CENTER);
        previous = MaiChatTypography.button(activity);
        previous.setText("上一页");
        previous.setEnabled(false);
        previous.setOnClickListener(view -> requestPage(currentPage - 1));
        controls.addView(previous);
        pageLabel = MaiChatTypography.text(activity);
        pageLabel.setText("正在读取 PDF…");
        pageLabel.setGravity(Gravity.CENTER);
        controls.addView(pageLabel, new LinearLayout.LayoutParams(0,
            ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        next = MaiChatTypography.button(activity);
        next.setText("下一页");
        next.setEnabled(false);
        next.setOnClickListener(view -> requestPage(currentPage + 1));
        controls.addView(next);
        layout.addView(controls);
        dialog = new AlertDialog.Builder(activity)
            .setTitle(displayName == null || displayName.isEmpty() ? "PDF 预览" : displayName)
            .setView(layout)
            .setNegativeButton("关闭", null)
            .create();
        dialog.setOnDismissListener(ignored -> close());
    }

    private void open() {
        dialog.show();
        if (dialog.getWindow() != null) {
            int height = (int) (activity.getResources().getDisplayMetrics().heightPixels * 0.86f);
            dialog.getWindow().setLayout(ViewGroup.LayoutParams.MATCH_PARENT, height);
        }
        worker.execute(() -> {
            try {
                File canonical = validate(file, workspace);
                descriptor = ParcelFileDescriptor.open(canonical, ParcelFileDescriptor.MODE_READ_ONLY);
                renderer = new PdfRenderer(descriptor);
                pageCount = renderer.getPageCount();
                if (pageCount <= 0) throw new IOException("PDF 没有可预览的页面");
                renderPage(0);
            } catch (Exception failure) {
                activity.runOnUiThread(() -> {
                    if (!closed.get()) {
                        pageLabel.setText("无法预览 PDF：" + failure.getMessage());
                        pageLabel.setTextColor(Color.RED);
                    }
                });
            }
        });
    }

    static File validate(File file, File workspace) throws IOException {
        if (file == null) throw new IOException("PDF 文件不存在");
        File canonical = file.getCanonicalFile();
        if (workspace != null && !canonical.toPath().startsWith(workspace.getCanonicalFile().toPath()))
            throw new IOException("PDF 不在 AI 工作区内");
        if (!canonical.isFile() || canonical.length() < 8 || canonical.length() > MAX_BYTES)
            throw new IOException("PDF 文件不存在或大小不受支持");
        byte[] header = new byte[5];
        try (FileInputStream input = new FileInputStream(canonical)) {
            if (input.read(header) != 5
                || !"%PDF-".equals(new String(header, StandardCharsets.US_ASCII)))
                throw new IOException("文件内容不是 PDF");
        }
        return canonical;
    }

    /** Render only page zero's first viewport; descriptors/pages never escape the worker. */
    static Bitmap thumbnail(File file, File workspace, int width, int height, File cacheDirectory) throws IOException {
        try (DocumentThumbnailSnapshot snapshot = DocumentThumbnailSnapshot.pdf(file, workspace, cacheDirectory, () -> true)) {
            return renderSnapshot(snapshot, width, height);
        }
    }

    /** Consumes the descriptor of a verified, already-unlinked immutable snapshot. */
    static Bitmap renderSnapshot(DocumentThumbnailSnapshot snapshot, int width, int height) throws IOException {
        try (ParcelFileDescriptor owned = snapshot.takeDescriptor(); PdfRenderer renderer = new PdfRenderer(owned)) {
            if (renderer.getPageCount() <= 0) throw new IOException("PDF has no pages");
            try (PdfRenderer.Page page = renderer.openPage(0)) {
                return raster(page, width, height, true);
            }
        }
    }

    private static Bitmap raster(PdfRenderer.Page page, int width, int height, boolean firstViewport) {
        Bitmap bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        try {
            bitmap.eraseColor(Color.WHITE);
            Matrix scale = new Matrix();
            float x = (float) width / page.getWidth();
            // A thumbnail crops a real top viewport; it must not stretch a whole page into it.
            scale.setScale(x, firstViewport ? x : (float) height / page.getHeight());
            page.render(bitmap, null, scale, PdfRenderer.Page.RENDER_MODE_FOR_DISPLAY);
            return bitmap;
        } catch (RuntimeException | Error failure) {
            bitmap.recycle();
            throw failure;
        }
    }

    private void requestPage(int index) {
        if (closed.get() || index < 0 || index >= pageCount || index == currentPage) return;
        activity.runOnUiThread(() -> {
            if (closed.get()) return;
            previous.setEnabled(false);
            next.setEnabled(false);
            pageLabel.setText("正在读取第 " + (index + 1) + " 页…");
        });
        worker.execute(() -> renderPage(index));
    }

    private void renderPage(int index) {
        if (closed.get() || renderer == null) return;
        PdfRenderer.Page page = null;
        Bitmap bitmap = null;
        try {
            page = renderer.openPage(index);
            int width = Math.min(1600,
                Math.max(720, activity.getResources().getDisplayMetrics().widthPixels * 2));
            int height = Math.max(1, Math.min(3000,
                Math.round((float) page.getHeight() * width / page.getWidth())));
            bitmap = raster(page, width, height, false);
            Bitmap result = bitmap;
            bitmap = null;
            activity.runOnUiThread(() -> {
                if (closed.get()) { result.recycle(); return; }
                Bitmap old = shownBitmap;
                shownBitmap = result;
                image.setImageBitmap(result);
                if (old != null) old.recycle();
                currentPage = index;
                pageLabel.setText((index + 1) + " / " + pageCount);
                previous.setEnabled(index > 0);
                next.setEnabled(index + 1 < pageCount);
            });
        } catch (Exception failure) {
            activity.runOnUiThread(() -> {
                if (!closed.get()) {
                    pageLabel.setText("页面读取失败：" + failure.getMessage());
                    previous.setEnabled(currentPage > 0);
                    next.setEnabled(currentPage + 1 < pageCount);
                }
            });
        } finally {
            if (page != null) page.close();
            if (bitmap != null) bitmap.recycle();
        }
    }

    private void close() {
        if (!closed.compareAndSet(false, true)) return;
        image.setImageDrawable(null);
        if (shownBitmap != null) { shownBitmap.recycle(); shownBitmap = null; }
        worker.execute(() -> {
            if (renderer != null) renderer.close();
            if (descriptor != null) try { descriptor.close(); } catch (IOException ignored) { }
        });
        worker.shutdown();
    }
}
