// 文件作用：验证独立视频窗口覆盖原页面，不重建或缩小原编辑器与会话滚动区域。
package com.kongshang.maichat;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Rect;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import androidx.core.graphics.Insets;
import androidx.core.view.DisplayCutoutCompat;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowInsetsCompat;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.Collections;

import static org.junit.Assert.*;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, manifest = Config.NONE)
public class MaiVideoPreviewDialogTest {
    @Test public void independentWindowLeavesTheOriginalPageScrollAndDraftIntact() {
        Activity activity = Robolectric.buildActivity(Activity.class).setup().get();
        FrameLayout page = new FrameLayout(activity);
        LinearLayout conversation = new LinearLayout(activity);
        conversation.setOrientation(LinearLayout.VERTICAL);
        ScrollView scroll = new ScrollView(activity);
        TextView history = new TextView(activity);
        history.setText("Previous message\n".repeat(300));
        scroll.addView(history);
        conversation.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));
        EditText draft = new EditText(activity);
        draft.setText("Unsent draft");
        conversation.addView(draft, new LinearLayout.LayoutParams(-1, 80));
        page.addView(conversation, new FrameLayout.LayoutParams(-1, -1));
        activity.setContentView(page);
        layout(page, 720, 1280);
        scroll.scrollTo(0, 260);
        int originalScroll = scroll.getScrollY();
        int originalHeight = conversation.getHeight();

        MaiVideoPreviewDialog dialog = new MaiVideoPreviewDialog(activity);
        dialog.setContentView(new FrameLayout(activity));
        dialog.show();
        layout(page, 720, 1280);
        assertEquals(originalHeight, conversation.getHeight());
        assertSame(page, conversation.getParent());
        assertEquals(1, page.getChildCount());
        FrameLayout.LayoutParams params = (FrameLayout.LayoutParams) conversation.getLayoutParams();
        assertEquals(0, params.topMargin);
        assertEquals(0, params.leftMargin);
        assertEquals(originalScroll, scroll.getScrollY());
        dialog.dismiss();
        assertSame(draft, conversation.getChildAt(1));
        assertEquals("Unsent draft", draft.getText().toString());
        assertEquals(originalScroll, scroll.getScrollY());
    }

    @Test public void videoCoversTheViewportAndOwnsBackInsteadOfPassingInputToThePage() {
        Activity activity = Robolectric.buildActivity(Activity.class).setup().get();
        MaiVideoPreviewDialog dialog = new MaiVideoPreviewDialog(activity);
        AtomicInteger dismissed = new AtomicInteger();
        dialog.setOnDismissListener(ignored -> dismissed.incrementAndGet());
        dialog.show();
        WindowManager.LayoutParams params = dialog.getWindow().getAttributes();
        assertEquals(ViewGroup.LayoutParams.MATCH_PARENT, params.width);
        assertEquals(ViewGroup.LayoutParams.MATCH_PARENT, params.height);
        assertEquals(0, params.flags & WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE);
        assertEquals(0, params.flags & WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL);
        assertEquals(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING,
            params.softInputMode & WindowManager.LayoutParams.SOFT_INPUT_MASK_ADJUST);
        dialog.onBackPressed();
        org.robolectric.Shadows.shadowOf(android.os.Looper.getMainLooper()).idle();
        assertFalse(dialog.isShowing());
        assertEquals(1, dismissed.get());
    }

    @Test public void previewOwnsCutoutAndDarkSystemBarsWithoutChangingTheActivityWindow() {
        Activity activity = Robolectric.buildActivity(Activity.class).setup().get();
        activity.getWindow().setStatusBarColor(Color.WHITE);
        activity.getWindow().setNavigationBarColor(Color.WHITE);
        activity.getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR
            | View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR);
        int activityFlags = activity.getWindow().getAttributes().flags;
        MaiVideoPreviewDialog dialog = new MaiVideoPreviewDialog(activity);
        dialog.getWindow().setStatusBarColor(Color.WHITE);
        dialog.getWindow().setNavigationBarColor(Color.WHITE);
        dialog.getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR
            | View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR);
        dialog.show();
        WindowManager.LayoutParams params = dialog.getWindow().getAttributes();
        assertEquals(WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES,
            params.layoutInDisplayCutoutMode);
        assertTrue((params.flags & WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN) != 0);
        assertTrue((params.flags & WindowManager.LayoutParams.FLAG_FULLSCREEN) != 0);
        assertEquals(0, params.getFitInsetsTypes());
        assertEquals(Color.BLACK, dialog.getWindow().getStatusBarColor());
        assertEquals(Color.BLACK, dialog.getWindow().getNavigationBarColor());
        int appearance = dialog.getWindow().getDecorView().getSystemUiVisibility();
        assertEquals(0, appearance & View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR);
        assertEquals(0, appearance & View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR);
        dialog.dismiss();
        assertEquals(Color.WHITE, activity.getWindow().getStatusBarColor());
        assertEquals(Color.WHITE, activity.getWindow().getNavigationBarColor());
        assertEquals(activityFlags, activity.getWindow().getAttributes().flags);
        assertEquals(View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR | View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR,
            activity.getWindow().getDecorView().getSystemUiVisibility());
    }

    @Test public void contentAvoidsVisibleNavigationAndCutoutWithoutAccumulatingOrChangingChildPadding() {
        Activity activity = Robolectric.buildActivity(Activity.class).setup().get();
        MaiVideoPreviewDialog dialog = new MaiVideoPreviewDialog(activity);
        FrameLayout player = new FrameLayout(activity);
        player.setPadding(21, 23, 25, 27);
        dialog.setContentView(player);
        View content = dialog.getWindow().findViewById(android.R.id.content);
        content.setPadding(7, 9, 11, 13);
        dialog.show();
        WindowInsetsCompat portrait = new WindowInsetsCompat.Builder()
            .setInsets(WindowInsetsCompat.Type.navigationBars(), Insets.of(0, 0, 0, 48))
            .setDisplayCutout(new DisplayCutoutCompat(new Rect(12, 35, 0, 0),
                Collections.singletonList(new Rect(0, 0, 12, 35)))).build();
        ViewCompat.dispatchApplyWindowInsets(content, portrait);
        assertPadding(content, 19, 44, 11, 61);
        ViewCompat.dispatchApplyWindowInsets(content, portrait);
        assertPadding(content, 19, 44, 11, 61);
        WindowInsetsCompat landscape = new WindowInsetsCompat.Builder()
            .setInsets(WindowInsetsCompat.Type.navigationBars(), Insets.of(0, 0, 42, 0))
            .setDisplayCutout(new DisplayCutoutCompat(new Rect(0, 0, 20, 0),
                Collections.singletonList(new Rect(680, 0, 720, 40)))).build();
        ViewCompat.dispatchApplyWindowInsets(content, landscape);
        assertPadding(content, 7, 9, 53, 13);
        assertPadding(player, 21, 23, 25, 27);
        dialog.dismiss();
    }

    private static void assertPadding(View view, int left, int top, int right, int bottom) {
        assertEquals(left, view.getPaddingLeft());
        assertEquals(top, view.getPaddingTop());
        assertEquals(right, view.getPaddingRight());
        assertEquals(bottom, view.getPaddingBottom());
    }

    private static void layout(View view, int width, int height) {
        view.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
            View.MeasureSpec.makeMeasureSpec(height, View.MeasureSpec.EXACTLY));
        view.layout(0, 0, width, height);
    }
}
