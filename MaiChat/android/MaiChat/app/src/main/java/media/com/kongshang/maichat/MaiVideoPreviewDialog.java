// 文件作用：所有视频入口共用的独立全屏窗口；播放器仍由原有媒体内核负责。
package com.kongshang.maichat;

import android.app.Activity;
import android.app.Dialog;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
import android.os.Build;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.core.view.WindowInsetsControllerCompat;

final class MaiVideoPreviewDialog extends Dialog {
    private View insetContent;

    MaiVideoPreviewDialog(Activity activity) {
        super(activity, android.R.style.Theme_Black_NoTitleBar_Fullscreen);
        setCanceledOnTouchOutside(false);
    }

    @Override protected void onStart() {
        super.onStart();
        Window window = getWindow();
        if (window == null) return;
        // 单独窗口覆盖原页面，不能修改宿主布局的边距或重建会话来给播放器让位。
        // 可聚焦且模态：Back 先关闭预览，触摸不会落到下方草稿或导航。
        window.clearFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
            | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
            | WindowManager.LayoutParams.FLAG_DIM_BEHIND
            | WindowManager.LayoutParams.FLAG_TRANSLUCENT_STATUS
            | WindowManager.LayoutParams.FLAG_TRANSLUCENT_NAVIGATION);
        // 全屏主题只隐藏状态栏，不保证窗口覆盖刘海安全区。独立配置本窗口，
        // 避免窗口整体下移后露出 Activity 的白色系统栏；关闭时无需恢复宿主属性。
        WindowCompat.setDecorFitsSystemWindows(window, false);
        window.addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN
            | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
            | WindowManager.LayoutParams.FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS);
        WindowManager.LayoutParams params = window.getAttributes();
        if (Build.VERSION.SDK_INT >= 28)
            params.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        if (Build.VERSION.SDK_INT >= 30) params.setFitInsetsTypes(0);
        window.setAttributes(params);
        window.setStatusBarColor(Color.BLACK);
        window.setNavigationBarColor(Color.BLACK);
        WindowInsetsControllerCompat bars = new WindowInsetsControllerCompat(window, window.getDecorView());
        bars.setAppearanceLightStatusBars(false);
        bars.setAppearanceLightNavigationBars(false);
        bars.hide(WindowInsetsCompat.Type.statusBars());
        window.setBackgroundDrawable(new ColorDrawable(Color.BLACK));
        window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_HIDDEN
            | WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING);
        window.setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
        applySafeContentInsets(window);
    }

    private void applySafeContentInsets(Window window) {
        View content = window.findViewById(android.R.id.content);
        if (content == null) return;
        if (insetContent != content) {
            insetContent = content;
            final int left = content.getPaddingLeft(), top = content.getPaddingTop();
            final int right = content.getPaddingRight(), bottom = content.getPaddingBottom();
            // 黑色窗口铺满物理屏幕，交互内容避开可见导航栏与刘海，兼顾云盘贴底控制条。
            // 基于容器初始 padding 重算，旋转/重复分发/重新 show 都不能累加；子视图不改动。
            ViewCompat.setOnApplyWindowInsetsListener(content, (view, insets) -> {
                Insets safe = insets.getInsets(WindowInsetsCompat.Type.navigationBars()
                    | WindowInsetsCompat.Type.displayCutout());
                view.setPadding(left + safe.left, top + safe.top,
                    right + safe.right, bottom + safe.bottom);
                return insets;
            });
        }
        ViewCompat.requestApplyInsets(content);
    }
}
