// 文件作用：统一扫描好友与电脑登录二维码；好友返回账号，电脑登录进入显式确认页。
package com.kongshang.maichat;

import android.Manifest;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.media.Image;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.TextView;

import androidx.activity.ComponentActivity;
import androidx.camera.core.CameraSelector;
import androidx.camera.core.ImageAnalysis;
import androidx.camera.core.ImageProxy;
import androidx.camera.core.Preview;
import androidx.camera.lifecycle.ProcessCameraProvider;
import androidx.camera.view.PreviewView;
import androidx.core.content.ContextCompat;

import com.google.common.util.concurrent.ListenableFuture;
import com.google.mlkit.vision.barcode.BarcodeScanner;
import com.google.mlkit.vision.barcode.BarcodeScannerOptions;
import com.google.mlkit.vision.barcode.BarcodeScanning;
import com.google.mlkit.vision.barcode.common.Barcode;
import com.google.mlkit.vision.common.InputImage;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

public final class FriendQrScanActivity extends MaiChatBaseActivity {
    private static final int CAMERA_PERMISSION_REQUEST = 1;
    private static final int DESKTOP_LOGIN_REQUEST = 2;
    private static final String KEY_DESKTOP_OPEN = "desktop_approval_open";
    private static final String SCAN_HINT = "扫描好友二维码或电脑登录二维码";
    private final ExecutorService cameraExecutor = Executors.newSingleThreadExecutor();
    private final AtomicBoolean frameInFlight = new AtomicBoolean();
    private final BarcodeScanner scanner = BarcodeScanning.getClient(
        new BarcodeScannerOptions.Builder().setBarcodeFormats(Barcode.FORMAT_QR_CODE).build());
    private PreviewView previewView;
    private TextView hint;
    private ProcessCameraProvider provider;
    private boolean providerLoading;
    private volatile boolean finished;
    private volatile boolean active;
    private volatile boolean openingDesktop;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        openingDesktop = state != null && state.getBoolean(KEY_DESKTOP_OPEN);
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(0xFF101829);
        previewView = new PreviewView(this);
        root.addView(previewView, new FrameLayout.LayoutParams(-1, -1));
        View frame = new View(this);
        frame.setBackground(MaiChatTheme.bordered(Color.TRANSPARENT, Color.WHITE, 18, this));
        FrameLayout.LayoutParams frameParams = new FrameLayout.LayoutParams(dp(250), dp(250),
            Gravity.CENTER);
        root.addView(frame, frameParams);
        TextView title = MaiChatTheme.label(this, "扫一扫", MaiChatTypographyTokens.Role.TITLE, Color.WHITE);
        title.setGravity(Gravity.CENTER);
        FrameLayout.LayoutParams titleParams = new FrameLayout.LayoutParams(dp(150), dp(50),
            Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        titleParams.topMargin = dp(12);
        root.addView(title, titleParams);
        hint = MaiChatTheme.label(this, SCAN_HINT, MaiChatTypographyTokens.Role.BODY, Color.WHITE);
        hint.setGravity(Gravity.CENTER);
        FrameLayout.LayoutParams hintParams = new FrameLayout.LayoutParams(-1, dp(58),
            Gravity.BOTTOM);
        hintParams.bottomMargin = dp(55);
        root.addView(hint, hintParams);
        TextView close = MaiChatTheme.label(this, "关闭", MaiChatTypographyTokens.Role.BODY, Color.WHITE);
        close.setGravity(Gravity.CENTER);
        close.setOnClickListener(view -> finish());
        FrameLayout.LayoutParams closeParams = new FrameLayout.LayoutParams(dp(70), dp(50),
            Gravity.TOP | Gravity.START);
        closeParams.leftMargin = dp(12);
        closeParams.topMargin = dp(12);
        root.addView(close, closeParams);
        setContentView(root);
    }

    @Override protected void onResume() {
        super.onResume();
        active = true;
        if (finished || openingDesktop) return;
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA)
            == PackageManager.PERMISSION_GRANTED) {
            ensureCamera();
        } else {
            hint.setText("请允许 MaiChat 使用相机扫码");
            requestPermissions(new String[]{Manifest.permission.CAMERA}, CAMERA_PERMISSION_REQUEST);
        }
    }

    @Override protected void onPause() {
        active = false;
        if (provider != null) provider.unbindAll();
        super.onPause();
    }

    @Override protected void onSaveInstanceState(Bundle state) {
        state.putBoolean(KEY_DESKTOP_OPEN, openingDesktop);
        super.onSaveInstanceState(state);
    }

    @Override protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != DESKTOP_LOGIN_REQUEST || !openingDesktop || finished) return;
        openingDesktop = false;
        if (resultCode == DesktopLoginApprovalActivity.RESULT_SCAN_AGAIN) {
            hint.setText(SCAN_HINT);
            // 只恢复实际已回到前台的父页面；通常由随后的 onResume 打开相机。
            if (active) ensureCamera();
        } else {
            // 登录结果不是好友结果，不能把子页面的 RESULT_OK/extra 传给添加好友入口。
            finished = true;
            setResult(RESULT_CANCELED);
            finish();
        }
    }

    @Override public void onRequestPermissionsResult(int requestCode, String[] permissions,
                                                      int[] results) {
        super.onRequestPermissionsResult(requestCode, permissions, results);
        if (requestCode != CAMERA_PERMISSION_REQUEST) return;
        if (results.length > 0 && results[0] == PackageManager.PERMISSION_GRANTED) {
            ensureCamera();
        } else {
            hint.setText("相机权限未开启，请到系统设置允许 MaiChat 使用相机");
        }
    }

    private void ensureCamera() {
        if (!active || finished || openingDesktop || providerLoading) return;
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA)
            != PackageManager.PERMISSION_GRANTED) return;
        if (provider != null) { bindCamera(); return; }
        providerLoading = true;
        ListenableFuture<ProcessCameraProvider> future = ProcessCameraProvider.getInstance(this);
        future.addListener(() -> {
            providerLoading = false;
            if (!active || finished || openingDesktop || isFinishing() || isDestroyed()) return;
            try {
                provider = future.get();
                bindCamera();
            } catch (Exception failure) { hint.setText("相机不可用"); }
        }, ContextCompat.getMainExecutor(this));
    }

    private void bindCamera() {
        if (!active || finished || openingDesktop || provider == null || isFinishing()) return;
        try {
            Preview preview = new Preview.Builder().build();
            preview.setSurfaceProvider(previewView.getSurfaceProvider());
            ImageAnalysis analysis = new ImageAnalysis.Builder()
                .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST).build();
            analysis.setAnalyzer(cameraExecutor, this::scanFrame);
            provider.unbindAll();
            provider.bindToLifecycle(this, CameraSelector.DEFAULT_BACK_CAMERA, preview, analysis);
            hint.setText(SCAN_HINT);
        } catch (Exception failure) { hint.setText("相机不可用"); }
    }

    @androidx.camera.core.ExperimentalGetImage
    private void scanFrame(ImageProxy frame) {
        if (!active || finished || openingDesktop || !frameInFlight.compareAndSet(false, true)) {
            frame.close();
            return;
        }
        Image image = frame.getImage();
        if (image == null) { frameInFlight.set(false); frame.close(); return; }
        InputImage input = InputImage.fromMediaImage(image,
            frame.getImageInfo().getRotationDegrees());
        scanner.process(input).addOnSuccessListener(codes -> {
            for (Barcode code : codes) {
                if (acceptQrUri(code.getRawValue())) return;
            }
        }).addOnCompleteListener(task -> {
            frameInFlight.set(false);
            frame.close();
        });
    }

    // 相机和普通调用共用严格识别入口；登录码只读取请求，不携带批准决定。
    boolean acceptQrUri(String raw) {
        if (!active || finished || openingDesktop || isFinishing() || isDestroyed()) return false;
        String userId = FriendQrPolicy.userId(raw);
        if (userId.isEmpty()) {
            if (DesktopLoginQrUri.requestId(raw).isEmpty()) {
                hint.setText("这不是 MaiChat 好友或电脑登录二维码");
                return false;
            }
            openingDesktop = true;
            if (provider != null) provider.unbindAll();
            startActivityForResult(new Intent(this, DesktopLoginApprovalActivity.class)
                .putExtra(DesktopLoginApprovalActivity.EXTRA_QR_URI, raw), DESKTOP_LOGIN_REQUEST);
            return true;
        }
        finished = true;
        Intent result = new Intent().putExtra("friend_user_id", userId);
        setResult(RESULT_OK, result);
        finish();
        return true;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    @Override protected void onDestroy() {
        active = false;
        finished = true;
        if (provider != null) provider.unbindAll();
        scanner.close();
        cameraExecutor.shutdownNow();
        super.onDestroy();
    }
}
