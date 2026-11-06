// 文件作用：在 App 内预览并裁剪所选头像；只生成本地文件，不上传或修改账号资料。
// 生命周期：取消、宿主销毁或调用方 dismiss 后，迟到的解码与保存结果只做资源清理。
package com.kongshang.maichat;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Application;
import android.app.Dialog;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.ScaleGestureDetector;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;
import java.io.File;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Consumer;

/** Local square avatar editor. The caller owns and deletes the confirmed temporary JPEG. */
public final class AvatarCropDialog implements Application.ActivityLifecycleCallbacks {
    private final Activity activity;
    private final Context appContext;
    private final Consumer<File> onConfirmed;
    private final Runnable onCancelled;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final ExecutorService worker = Executors.newSingleThreadExecutor(
        runnable -> new Thread(runnable, "avatar-crop"));
    private final AtomicBoolean closed = new AtomicBoolean();
    private final AlertDialog dialog;
    private final CropView preview;
    private final ProgressBar progress;
    private final TextView status;
    private Bitmap shownBitmap;
    private boolean saving, delivered;

    /**
     * Opens immediately with a loading state and returns the handle used to cancel on account changes.
     * Call on the main thread. Both callbacks run on that thread, at most one is delivered.
     * Confirmation yields a private cache 512×512 JPEG at quality 88; the caller must delete it.
     * Destroyed or finishing activities receive no callbacks. Invalid activities return null.
     */
    public static Dialog show(Activity activity, Uri source, Consumer<File> onConfirmed,
                              Runnable onCancelled) {
        if (Looper.myLooper() != Looper.getMainLooper())
            throw new IllegalStateException("Avatar crop must open on the main thread");
        if (activity == null || activity.isFinishing() || activity.isDestroyed()) return null;
        if (source == null || onConfirmed == null || onCancelled == null)
            throw new IllegalArgumentException("Avatar crop requires a source and callbacks");
        AvatarCropDialog owner = new AvatarCropDialog(activity, onConfirmed, onCancelled);
        owner.open(source);
        return owner.dialog;
    }

    private AvatarCropDialog(Activity activity, Consumer<File> onConfirmed, Runnable onCancelled) {
        this.activity = activity;
        this.appContext = activity.getApplicationContext();
        this.onConfirmed = onConfirmed;
        this.onCancelled = onCancelled;
        LinearLayout content = new LinearLayout(activity);
        content.setOrientation(LinearLayout.VERTICAL);
        FrameLayout frame = new FrameLayout(activity);
        frame.setBackgroundColor(Color.rgb(15, 19, 26));
        preview = new CropView(activity);
        frame.addView(preview, new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        progress = new ProgressBar(activity);
        frame.addView(progress, new FrameLayout.LayoutParams(
            MaiChatTheme.dp(activity, 40), MaiChatTheme.dp(activity, 40), Gravity.CENTER));
        int screenHeight = activity.getResources().getDisplayMetrics().heightPixels;
        int screenWidth = activity.getResources().getDisplayMetrics().widthPixels;
        int previewHeight = Math.max(MaiChatTheme.dp(activity, 150),
            Math.min(screenWidth, (int) (screenHeight * 0.55f)));
        content.addView(frame, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, previewHeight));
        status = MaiChatTheme.text(activity, "正在读取图片…", MaiChatTypographyTokens.Role.DETAIL, MaiChatTheme.SECONDARY);
        int inset = MaiChatTheme.dp(activity, 16);
        status.setPadding(inset, inset, inset, inset);
        content.addView(status, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        dialog = new AlertDialog.Builder(activity)
            .setTitle("裁剪头像")
            .setView(content)
            .setNegativeButton("取消", null)
            .setPositiveButton("使用此头像", null)
            .create();
        dialog.setCanceledOnTouchOutside(false);
        dialog.setOnDismissListener(ignored -> close(true));
    }

    private void open(Uri source) {
        activity.getApplication().registerActivityLifecycleCallbacks(this);
        try {
            dialog.show();
            if (dialog.getWindow() != null)
                dialog.getWindow().setLayout(ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT);
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(false);
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(ignored -> save());
        } catch (RuntimeException failure) {
            if (activityValid())
                Toast.makeText(activity, "无法打开头像裁剪，请重试", Toast.LENGTH_SHORT).show();
            close(true);
            return;
        }
        worker.execute(() -> {
            try {
                // 与普通上传及模型入口共用有界解码、EXIF 修正；本层只管理用户选区。
                Bitmap decoded = MaiChatAvatarImage.decodeSelectedImage(appContext, source);
                main.post(() -> {
                    if (!active()) {
                        decoded.recycle();
                        dismissInactive();
                        return;
                    }
                    shownBitmap = decoded;
                    preview.setImage(decoded);
                    progress.setVisibility(View.GONE);
                    status.setText("拖动调整位置，双指缩放；方框内的画面将作为头像");
                    dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(true);
                });
            } catch (Exception failure) {
                main.post(() -> loadFailed("无法读取这张图片，请重新选择"));
            } catch (OutOfMemoryError failure) {
                main.post(() -> loadFailed("图片过大，请选择尺寸较小的图片"));
            }
        });
    }

    private boolean activityValid() { return !activity.isFinishing() && !activity.isDestroyed(); }
    private boolean active() { return !closed.get() && activityValid() && dialog.isShowing(); }

    private void loadFailed(String message) {
        if (!active()) { dismissInactive(); return; }
        progress.setVisibility(View.GONE);
        status.setText(message);
        dialog.getButton(AlertDialog.BUTTON_NEGATIVE).setText("关闭");
        Toast.makeText(activity, message, Toast.LENGTH_SHORT).show();
    }

    private void save() {
        if (saving || shownBitmap == null || !active()) return;
        RectF crop = preview.selectedCrop();
        if (crop == null) return;
        saving = true;
        preview.setBusy(true);
        progress.setVisibility(View.VISIBLE);
        status.setText("正在保存头像…");
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(false);
        Bitmap source = shownBitmap;
        worker.execute(() -> {
            try {
                File result = MaiChatAvatarImage.saveCrop(appContext, source, crop);
                main.post(() -> {
                    if (!active()) {
                        result.delete();
                        dismissInactive();
                        return;
                    }
                    delivered = true;
                    dialog.dismiss();
                    onConfirmed.accept(result);
                });
            } catch (Exception failure) {
                main.post(() -> saveFailed());
            } catch (OutOfMemoryError failure) {
                main.post(() -> saveFailed());
            }
        });
    }

    private void saveFailed() {
        if (!active()) { dismissInactive(); return; }
        saving = false;
        preview.setBusy(false);
        progress.setVisibility(View.GONE);
        status.setText("保存失败，可重试或取消");
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(true);
        Toast.makeText(activity, "保存头像失败，请重试", Toast.LENGTH_SHORT).show();
    }

    private void dismissInactive() {
        close(false);
        if (dialog.isShowing()) dialog.dismiss();
    }

    private void close(boolean notifyCancelled) {
        if (!closed.compareAndSet(false, true)) return;
        activity.getApplication().unregisterActivityLifecycleCallbacks(this);
        preview.setImage(null);
        Bitmap released = shownBitmap;
        shownBitmap = null;
        // 保存和释放串行排队，用户在保存中取消时不能回收工作线程仍在读取的位图。
        worker.execute(() -> { if (released != null) released.recycle(); });
        worker.shutdown();
        if (notifyCancelled && !delivered && activityValid()) onCancelled.run();
    }

    @Override public void onActivityDestroyed(Activity destroyed) {
        if (destroyed == activity) dismissInactive();
    }
    @Override public void onActivityCreated(Activity activity, Bundle state) { }
    @Override public void onActivityStarted(Activity activity) { }
    @Override public void onActivityResumed(Activity activity) { }
    @Override public void onActivityPaused(Activity activity) { }
    @Override public void onActivityStopped(Activity activity) { }
    @Override public void onActivitySaveInstanceState(Activity activity, Bundle state) { }

    private static final class CropView extends View {
        private final Paint imagePaint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
        private final Paint overlayPaint = new Paint();
        private final Paint linePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final ScaleGestureDetector pinch;
        private Bitmap image;
        private AvatarCropGeometry geometry;
        private float cropLeft, cropTop, cropSide, previousX, previousY;
        private int pointerId = -1;
        private boolean busy, dragged;

        CropView(Context context) {
            super(context);
            setContentDescription("头像裁剪预览，拖动调整位置，双指缩放");
            setFocusable(true);
            pinch = new ScaleGestureDetector(context,
                new ScaleGestureDetector.SimpleOnScaleGestureListener() {
                    @Override public boolean onScale(ScaleGestureDetector detector) {
                        if (geometry == null || busy) return false;
                        geometry.zoom(detector.getScaleFactor(), detector.getFocusX() - cropLeft,
                            detector.getFocusY() - cropTop);
                        dragged = true;
                        invalidate();
                        return true;
                    }
                });
        }

        void setImage(Bitmap image) {
            this.image = image;
            geometry = null;
            updateFrame();
            invalidate();
        }

        void setBusy(boolean busy) { this.busy = busy; }

        RectF selectedCrop() {
            if (geometry == null) return null;
            float left = geometry.sourceLeft(), top = geometry.sourceTop();
            // 浮点加法在边缘可能多出不足一个像素；编码入口仍严格拒绝真正越界的选区。
            return new RectF(left, top,
                Math.min(image.getWidth(), left + geometry.sourceSide()),
                Math.min(image.getHeight(), top + geometry.sourceSide()));
        }

        @Override protected void onSizeChanged(int width, int height, int oldWidth, int oldHeight) {
            super.onSizeChanged(width, height, oldWidth, oldHeight);
            updateFrame();
        }

        private void updateFrame() {
            if (image == null || getWidth() < 1 || getHeight() < 1) return;
            cropSide = Math.max(1, Math.min(getWidth(), getHeight()) - MaiChatTheme.dp(getContext(), 32));
            cropLeft = (getWidth() - cropSide) / 2;
            cropTop = (getHeight() - cropSide) / 2;
            if (geometry == null)
                geometry = new AvatarCropGeometry(image.getWidth(), image.getHeight(), cropSide);
            else geometry.resize(cropSide);
        }

        @Override protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            if (image == null || geometry == null) return;
            canvas.save();
            canvas.translate(cropLeft + geometry.left(), cropTop + geometry.top());
            canvas.scale(geometry.scale(), geometry.scale());
            canvas.drawBitmap(image, 0, 0, imagePaint);
            canvas.restore();
            float right = cropLeft + cropSide, bottom = cropTop + cropSide;
            overlayPaint.setColor(0xA6000000);
            canvas.drawRect(0, 0, getWidth(), cropTop, overlayPaint);
            canvas.drawRect(0, bottom, getWidth(), getHeight(), overlayPaint);
            canvas.drawRect(0, cropTop, cropLeft, bottom, overlayPaint);
            canvas.drawRect(right, cropTop, getWidth(), bottom, overlayPaint);
            linePaint.setStyle(Paint.Style.STROKE);
            linePaint.setStrokeWidth(MaiChatTheme.dp(getContext(), 1));
            linePaint.setColor(Color.WHITE);
            canvas.drawRect(cropLeft, cropTop, right, bottom, linePaint);
            linePaint.setColor(0x55FFFFFF);
            for (int index = 1; index < 3; index++) {
                float offset = cropSide * index / 3;
                canvas.drawLine(cropLeft + offset, cropTop, cropLeft + offset, bottom, linePaint);
                canvas.drawLine(cropLeft, cropTop + offset, right, cropTop + offset, linePaint);
            }
        }

        @Override public boolean onTouchEvent(MotionEvent event) {
            if (busy || geometry == null) return false;
            getParent().requestDisallowInterceptTouchEvent(true);
            pinch.onTouchEvent(event);
            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    pointerId = event.getPointerId(0);
                    previousX = event.getX();
                    previousY = event.getY();
                    dragged = false;
                    return true;
                case MotionEvent.ACTION_MOVE:
                    int pointer = event.findPointerIndex(pointerId);
                    if (event.getPointerCount() == 1 && !pinch.isInProgress() && pointer >= 0) {
                        float x = event.getX(pointer), y = event.getY(pointer);
                        geometry.pan(x - previousX, y - previousY);
                        dragged |= Math.abs(x - previousX) + Math.abs(y - previousY) > 1;
                        previousX = x;
                        previousY = y;
                        invalidate();
                    }
                    return true;
                case MotionEvent.ACTION_POINTER_UP:
                    int surviving = event.getActionIndex() == 0 ? 1 : 0;
                    pointerId = event.getPointerId(surviving);
                    previousX = event.getX(surviving);
                    previousY = event.getY(surviving);
                    return true;
                case MotionEvent.ACTION_UP:
                    if (!dragged) performClick();
                    pointerId = -1;
                    return true;
                case MotionEvent.ACTION_CANCEL:
                    pointerId = -1;
                    return true;
                default: return true;
            }
        }

        @Override public boolean performClick() { super.performClick(); return true; }
    }
}
