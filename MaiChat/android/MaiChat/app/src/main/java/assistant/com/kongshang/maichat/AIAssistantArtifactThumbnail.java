// Bounded document jobs and in-memory thumbnails for the existing Agent file button.
package com.kongshang.maichat;

import android.graphics.Bitmap;
import android.graphics.drawable.BitmapDrawable;
import android.os.Handler;
import android.os.Looper;
import android.util.LruCache;
import android.view.View;
import android.widget.Button;
import java.io.File;
import java.io.IOException;
import java.lang.ref.WeakReference;
import java.util.Map;
import java.util.WeakHashMap;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Future;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;

/** File I/O, immutable snapshots, PDF decode and bounded Markdown raster stay off the UI thread. */
final class AIAssistantArtifactThumbnail {
    static final int MAX_MARKDOWN_BYTES = 256 * 1024;
    private static final ThreadPoolExecutor worker = createWorker();
    private final ExecutorService io;
    private final ReadObserver readObserver;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final Map<Button, Request> bindings = new WeakHashMap<>();
    private final Map<Button, Boolean> observed = new WeakHashMap<>();
    private final LruCache<String, Bitmap> cache = new LruCache<String, Bitmap>(8 * 1024 * 1024) {
        @Override protected int sizeOf(String key, Bitmap bitmap) { return bitmap.getAllocationByteCount(); }
    };

    private static ThreadPoolExecutor createWorker() {
        ThreadPoolExecutor pool = new ThreadPoolExecutor(2, 2, 30, TimeUnit.SECONDS, new ArrayBlockingQueue<>(32));
        pool.allowCoreThreadTimeOut(true);
        return pool;
    }

    interface FileResolver { File resolve() throws Exception; }
    // Private diagnostic seam verifies source replacement without changing renderer behavior.
    interface ReadObserver {
        default void beforeRender(File source) throws IOException { }
        default void afterRender(File source) throws IOException { }
    }

    AIAssistantArtifactThumbnail() { this(worker); }
    AIAssistantArtifactThumbnail(ExecutorService io) { this(io, new ReadObserver() { }); }
    AIAssistantArtifactThumbnail(ExecutorService io, ReadObserver observer) {
        this.io = io; this.readObserver = observer;
    }

    private static final class Request {
        final String bindingKey;
        final ExecutorService executor;
        final AtomicBoolean canceled = new AtomicBoolean();
        volatile String visibleIdentity;
        Future<?> future;
        Request(String key, String identity, ExecutorService executor) {
            bindingKey = key; visibleIdentity = identity; this.executor = executor;
        }
        void cancel() {
            canceled.set(true);
            if (future != null) {
                future.cancel(false);
                // Future.cancel alone leaves a canceled FutureTask in a normal pool queue.
                if (executor instanceof ThreadPoolExecutor && future instanceof Runnable)
                    ((ThreadPoolExecutor) executor).remove((Runnable) future);
            }
        }
    }

    void clear(Button button) {
        Request previous = bindings.remove(button);
        if (previous != null) previous.cancel();
        button.setCompoundDrawables(null, null, null, null);
    }

    void bind(Button button, String owner, String name, String mime, long expectedBytes,
              File allowedRoot, FileResolver resolver, BooleanSupplier ownerCurrent,
              Consumer<Boolean> availability) {
        WeakReference<Button> target = new WeakReference<>(button);
        android.content.Context context = button.getContext().getApplicationContext();
        int inset = button.getPaddingLeft() + button.getPaddingRight();
        int available = button.getWidth() > inset ? button.getWidth() - inset
            : context.getResources().getDisplayMetrics().widthPixels - MaiChatTheme.dp(context, 64);
        int width = Math.max(1, Math.min(720, Math.min(MaiChatTheme.dp(context, 320), available)));
        int height = Math.max(1, Math.min(480, MaiChatTheme.dp(context, 180)));
        String style = width + ":" + height + ":" + MaiChatTheme.isDark() + ":"
            + context.getResources().getConfiguration().fontScale + ":" + MaiChatTypography.familyId();
        String bindingKey = owner + "\n" + name + "\n" + mime + "\n" + expectedBytes + "\n" + style;
        Request previous = bindings.remove(button);
        if (previous != null) previous.cancel();
        boolean retain = previous != null && previous.bindingKey.equals(bindingKey);
        if (!retain) button.setCompoundDrawables(null, null, null, null);
        Request request = new Request(bindingKey, retain ? previous.visibleIdentity : null, io);
        bindings.put(button, request);
        if (!observed.containsKey(button)) {
            observed.put(button, true);
            button.addOnAttachStateChangeListener(new View.OnAttachStateChangeListener() {
                @Override public void onViewAttachedToWindow(View view) { }
                @Override public void onViewDetachedFromWindow(View view) { clear((Button) view); }
            });
        }
        try {
            request.future = io.submit(() -> {
                if (request.canceled.get()) return;
                File file;
                try {
                    file = MarkdownPreviewDialog.validate(resolver.resolve(), allowedRoot);
                    if (!file.getName().equals(name) || file.length() != expectedBytes)
                        throw new IOException("delivered file identity changed");
                } catch (Exception failure) {
                    post(target, request, ownerCurrent, () -> {
                        Button live = target.get();
                        if (live != null) live.setCompoundDrawables(null, null, null, null);
                        availability.accept(false);
                    });
                    return;
                }
                post(target, request, ownerCurrent, () -> availability.accept(true));
                boolean markdown = MarkdownPreviewDialog.isMarkdown(mime, name);
                boolean pdf = "application/pdf".equalsIgnoreCase(mime);
                if ((!markdown && !pdf) || (markdown && expectedBytes > MAX_MARKDOWN_BYTES)
                    || (pdf && expectedBytes > DocumentThumbnailSnapshot.MAX_PDF_BYTES)) return;
                Bitmap bitmap = null;
                try (DocumentThumbnailSnapshot snapshot = markdown
                        ? DocumentThumbnailSnapshot.markdown(file, allowedRoot, MAX_MARKDOWN_BYTES,
                            () -> !request.canceled.get())
                        : DocumentThumbnailSnapshot.pdf(file, allowedRoot, context.getCacheDir(),
                            () -> !request.canceled.get())) {
                    if (request.canceled.get()) return;
                    String identity = snapshot.canonicalPath + "\n" + snapshot.digest;
                    String key = owner + "\n" + identity + "\n" + style + "\n" + mime;
                    if (request.visibleIdentity != null && !identity.equals(request.visibleIdentity))
                        post(target, request, ownerCurrent, () -> {
                            Button live = target.get();
                            if (live != null) live.setCompoundDrawables(null, null, null, null);
                        });
                    Bitmap ready = cache.get(key);
                    if (ready != null) { publish(target, request, ownerCurrent, identity, ready); return; }
                    readObserver.beforeRender(file);
                    // Reuse the serialized parser/styles without adding documents to the
                    // normal chat renderer queue. This task already owns a bounded worker slot.
                    bitmap = markdown ? MarkdownRenderer.thumbnail(context, snapshot.markdown, width, height,
                        () -> !request.canceled.get())
                        : PdfPreviewDialog.renderSnapshot(snapshot, width, height);
                    readObserver.afterRender(file);
                    if (bitmap == null || request.canceled.get()) return;
                    cache.put(key, bitmap);
                    publish(target, request, ownerCurrent, identity, bitmap);
                    bitmap = null;
                } catch (IOException | RuntimeException | OutOfMemoryError failure) {
                    // Failure preserves the file card and complete preview/share action.
                } finally {
                    if (bitmap != null && !bitmap.isRecycled()) bitmap.recycle();
                }
            });
        } catch (RejectedExecutionException busy) {
            // At capacity retain the card/action; an ordinary later bind can retry.
            // Never move document I/O onto the caller or an unbounded deferred queue.
            request.cancel();
        }
    }

    private void publish(WeakReference<Button> target, Request request, BooleanSupplier current,
                         String identity, Bitmap bitmap) {
        post(target, request, current, () -> {
            Button button = target.get();
            if (button == null) return;
            BitmapDrawable drawable = new BitmapDrawable(button.getResources(), bitmap);
            drawable.setBounds(0, 0, bitmap.getWidth(), bitmap.getHeight());
            button.setCompoundDrawablePadding(MaiChatTheme.dp(button.getContext(), 10));
            button.setCompoundDrawables(null, drawable, null, null);
            request.visibleIdentity = identity;
        });
    }

    private void post(WeakReference<Button> target, Request request, BooleanSupplier current, Runnable result) {
        main.post(() -> {
            Button button = target.get();
            if (!request.canceled.get() && button != null && bindings.get(button) == request
                && current.getAsBoolean()) result.run();
        });
    }
}
