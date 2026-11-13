// 文件作用：验证 IM 附件面板真实布局、动作分派及禁用状态，不打开系统或网络功能。
package com.kongshang.maichat;

import static org.junit.Assert.*;

import android.app.Activity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ImageView;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.Shadows;
import org.robolectric.annotation.Config;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "mdpi")
public class MaiIMAttachmentPanelTest {
    @Test public void showsTwoRowsOfFourColumnsWithOnlyBindingOnTheSecondRow() {
        Activity activity = Robolectric.buildActivity(Activity.class).setup().get();
        List<MaiIMAttachmentPanel.Action> actions = new ArrayList<>();
        View panel = MaiIMAttachmentPanel.create(activity, actions::add);
        panel.measure(View.MeasureSpec.makeMeasureSpec(360, View.MeasureSpec.EXACTLY),
            View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED));
        panel.layout(0, 0, 360, panel.getMeasuredHeight());
        ViewGroup rows = (ViewGroup) panel;
        assertEquals(2, rows.getChildCount());
        ViewGroup first = (ViewGroup) rows.getChildAt(0), second = (ViewGroup) rows.getChildAt(1);
        assertEquals(4, first.getChildCount());
        assertEquals(4, second.getChildCount());
        MaiIMAttachmentPanel.Action[] order = MaiIMAttachmentPanel.Action.values();
        for (int index = 0; index < 4; index++) {
            View tile = first.getChildAt(index);
            assertEquals(order[index], tile.getTag());
            assertTrue(tile.isClickable());
            assertTrue(tile.getWidth() >= 44 && tile.getHeight() >= 44);
            assertEquals(first.getChildAt(0).getWidth(), tile.getWidth());
        }
        assertEquals("SESSION_BINDING", ((MaiIMAttachmentPanel.Action) second.getChildAt(0).getTag()).name());
        for (int index = 1; index < 4; index++) assertFalse(second.getChildAt(index).isClickable());
        // Captions adapt to platform font metrics and user font scale; the
        // legacy JVM renderer gives CJK a 35px line even at 12sp/mdpi. Verify
        // non-overlap here and inspect normal-size visual height on the device.
        assertTrue(panel.getMeasuredHeight() >= 160);
        assertTrue(first.getBottom() <= second.getTop());
        assertTrue(second.getBottom() <= panel.getHeight() - panel.getPaddingBottom());
        panel.measure(View.MeasureSpec.makeMeasureSpec(360, View.MeasureSpec.EXACTLY),
            View.MeasureSpec.makeMeasureSpec(700, View.MeasureSpec.AT_MOST));
        assertTrue("Attachment panel must leave the conversation visible", panel.getMeasuredHeight() < 350);
        assertTrue(actions.isEmpty());
    }

    @Test public void bindingReplacesStandaloneReplyAndKeepsTheIosSparklesIcon() {
        Activity activity = Robolectric.buildActivity(Activity.class).setup().get();
        ViewGroup panel = (ViewGroup) MaiIMAttachmentPanel.create(activity, action -> {});
        ViewGroup tile = (ViewGroup) ((ViewGroup) panel.getChildAt(1)).getChildAt(0);
        assertEquals("绑定会话", tile.getContentDescription().toString());
        assertEquals("SESSION_BINDING", ((MaiIMAttachmentPanel.Action) tile.getTag()).name());
        assertThrows(IllegalArgumentException.class, () -> MaiIMAttachmentPanel.Action.valueOf("AI_REPLY"));
        ImageView icon = (ImageView) ((ViewGroup) tile.getChildAt(0)).getChildAt(0);
        assertEquals(R.drawable.ic_ai_sparkles, Shadows.shadowOf(icon.getDrawable()).getCreatedFromResId());
        assertEquals(25, icon.getLayoutParams().width);
        assertEquals(25, icon.getLayoutParams().height);
    }

    @Test public void independentReplyBackendAndSuggestionStateAreRemoved() {
        assertFalse(Arrays.stream(AIAssistantController.class.getDeclaredMethods())
            .anyMatch(method -> method.getName().equals("suggestReplies")));
        assertFalse(Arrays.stream(MainActivity.class.getDeclaredMethods())
            .anyMatch(method -> method.getName().equals("requestAiReplySuggestions")
                || method.getName().equals("renderAiReplySuggestions")));
        assertFalse(Arrays.stream(MainActivity.class.getDeclaredFields())
            .anyMatch(field -> field.getName().startsWith("aiReplySuggestions")));
    }

    @Test public void everyTileDispatchesItsOwnActionOnceAndDisabledPanelDispatchesNothing() {
        Activity activity = Robolectric.buildActivity(Activity.class).setup().get();
        List<MaiIMAttachmentPanel.Action> actions = new ArrayList<>();
        View panel = MaiIMAttachmentPanel.create(activity, actions::add);
        for (MaiIMAttachmentPanel.Action action : MaiIMAttachmentPanel.Action.values())
            panel.findViewWithTag(action).performClick();
        assertEquals(Arrays.asList(MaiIMAttachmentPanel.Action.values()), actions);
        panel.setEnabled(false);
        for (MaiIMAttachmentPanel.Action action : MaiIMAttachmentPanel.Action.values()) {
            View tile = panel.findViewWithTag(action);
            assertFalse(tile.isEnabled());
            tile.performClick();
        }
        assertEquals(5, actions.size());
        panel.setEnabled(true);
        assertTrue(panel.findViewWithTag(MaiIMAttachmentPanel.Action.VOICE_INPUT).isEnabled());
    }
}
