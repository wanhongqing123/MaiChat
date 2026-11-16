// 文件作用：用真实主导航与创作页面验证底栏在抽屉手势中属于同一移动页面。
package com.kongshang.maichat;

import static org.junit.Assert.*;

import android.app.Activity;
import android.os.Looper;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import java.io.File;
import java.lang.reflect.Method;
import java.time.Duration;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.Shadows;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;
import org.robolectric.util.ReflectionHelpers;

@RunWith(RobolectricTestRunner.class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
@Config(sdk = 34, qualifiers = "w400dp-h800dp-mdpi", shadows = {
    AIAssistantSessionDrawerTest.OwnedController.class,
    AIAssistantSessionDrawerTest.OwnedImageLoader.class})
public final class AIAssistantMainNavigationTest {
    private Activity context;
    private MainActivity host;
    private AIAssistantPanel panel;
    private AIAssistantController controller;
    private long clock;

    @Before public void setup() throws Exception {
        context = Robolectric.buildActivity(Activity.class).setup().get();
        host = Robolectric.buildActivity(MainActivity.class).get();
        LocalSettingsStore settings = new LocalSettingsStore(new File(context.getCacheDir(), "owned-navigation-settings"));
        settings.save(new RemoteIMSettings("owned-navigation"));
        RemoteIMSessionController session = new RemoteIMSessionController(settings,
            new LocalChatHistoryStore(new File(context.getCacheDir(), "owned-navigation-history")));
        ReflectionHelpers.setField(host, "session", session);
        ReflectionHelpers.setField(host, "displayedOwner", "owned-navigation");
        ReflectionHelpers.setField(host, "activeTab", RemoteIMTab.ASSISTANT);
        controller = new AIAssistantController(context, "http://owned.invalid");
        panel = new AIAssistantPanel(context, controller);
        panel.onState(new AIAssistantController.State(new JSONObject().put("sessions", new JSONArray())
            .put("devices", new JSONArray()).put("messages", new JSONArray()), null, "", "", "deepseek-flash",
            "on-request", "", "", "", 0, false, true));
        ReflectionHelpers.setField(host, "aiAssistant", panel);
        Method render = MainActivity.class.getDeclaredMethod("render");
        render.setAccessible(true); render.invoke(host);
        // MainActivity's ordinary render is exercised without starting its SDK lifecycle.
        // Attach that real tree to the already-resumed fixture window so touch clicks run.
        LinearLayout root = ReflectionHelpers.getField(host, "root");
        if (root.getParent() instanceof ViewGroup) ((ViewGroup) root.getParent()).removeView(root);
        context.setContentView(root);
        layout();
    }

    @After public void close() throws Exception {
        panel.setForeground(false);
        Method close = AIAssistantController.class.getDeclaredMethod("close");
        close.setAccessible(true); close.invoke(controller);
    }

    @Test public void bottomNavigationMovesWithTheExistingPageAndRestoresAfterCancel() {
        View bar = ReflectionHelpers.getField(host, "renderedTabBar");
        View card = named(panel, "创作中心会话页面");
        assertNotNull(bar); assertNotNull(card);
        int[] initial = position(bar);
        touch(MotionEvent.ACTION_DOWN, 8, 240);
        touch(MotionEvent.ACTION_MOVE, 160, 244);
        assertEquals(152f, card.getTranslationX(), 1f);
        assertEquals("The original bottom navigation must move with the conversation, as on iOS",
            152, position(bar)[0] - initial[0], 1);
        touch(MotionEvent.ACTION_CANCEL, 160, 244); settle();
        assertSame("The gesture must retain the same navigation instance", bar,
            ReflectionHelpers.getField(host, "renderedTabBar"));
        assertArrayEquals(initial, position(bar));
    }

    @Test public void expandedPageNavigationOnlyClosesTheDrawerAndOrdinaryTabsStillWork() {
        named(panel, "打开对话列表").performClick(); settle(); layout();
        View bar = ReflectionHelpers.getField(host, "renderedTabBar");
        int[] location = position(bar);
        // The visible first tab sits on the shifted page. This is an actual touch,
        // so the drawer return surface must consume it before a navigation action.
        float x = location[0] + 38;
        float y = location[1] + 32;
        touch(MotionEvent.ACTION_DOWN, x, y); touch(MotionEvent.ACTION_UP, x, y); settle();
        assertEquals(RemoteIMTab.ASSISTANT, ReflectionHelpers.getField(host, "activeTab"));
        assertEquals(0f, named(panel, "创作中心会话页面").getTranslationX(), .01f);
        named(bar, "消息").performClick(); layout();
        assertEquals(RemoteIMTab.MESSAGES, ReflectionHelpers.getField(host, "activeTab"));
        LinearLayout root = ReflectionHelpers.getField(host, "root");
        View current = ReflectionHelpers.getField(host, "renderedTabBar");
        assertSame("Other primary tabs retain the ordinary root bottom navigation", root, current.getParent());
        assertEquals(1, countNamed(root, "消息"));
    }

    @Test public void starterGridListsToolsWithoutDuplicatingOrdinaryChat() {
        ViewGroup welcome = panel.findViewWithTag("ai-welcome");
        assertNotNull("An empty creation conversation still shows the ordinary starter grid", welcome);
        assertNull("Ordinary chat is a base capability, not another creation tool card",
            welcome.findViewWithTag("ai-welcome-ask"));
        assertNull("Disabled image generation must not be advertised as an available creation tool",
            welcome.findViewWithTag("ai-welcome-image"));
        assertNull("Disabled video generation must not be advertised as an available creation tool",
            welcome.findViewWithTag("ai-welcome-video"));
        String[] tools = {"file", "screen", "browser", "schedule", "calendar", "qr",
            "location", "media", "camera", "music", "gallery", "drive", "messages", "contacts", "phone", "devices"};
        assertEquals("Removing disabled generation cards must retain the existing usable tool cards", tools.length,
            countStarterCards(welcome));
        for (String tool : tools) {
            View card = welcome.findViewWithTag("ai-welcome-" + tool);
            assertNotNull("Missing tool card: " + tool, card);
            assertFalse("The welcome catalog remains read-only", card.isClickable());
        }
        View file = welcome.findViewWithTag("ai-welcome-file");
        ViewGroup firstRow = (ViewGroup) file.getParent();
        ViewGroup grid = (ViewGroup) firstRow.getParent();
        assertEquals(8, grid.getChildCount());
        assertEquals("ai-welcome-file", firstRow.getChildAt(0).getTag());
        assertEquals("ai-welcome-screen", firstRow.getChildAt(2).getTag());
        for (int row = 0; row < grid.getChildCount(); row++) {
            ViewGroup pair = (ViewGroup) grid.getChildAt(row);
            assertNotNull("Every remaining row has two capability cards", pair.getChildAt(0).getTag());
            assertNotNull("Removing the seed must not leave a trailing placeholder", pair.getChildAt(2).getTag());
        }
    }

    private void layout() {
        LinearLayout root = ReflectionHelpers.getField(host, "root");
        root.measure(View.MeasureSpec.makeMeasureSpec(400, View.MeasureSpec.EXACTLY),
            View.MeasureSpec.makeMeasureSpec(800, View.MeasureSpec.EXACTLY));
        root.layout(0, 0, 400, 800);
    }
    private int[] position(View view) {
        // Dispatch below uses Panel coordinates, independent of the fixture window's insets.
        android.graphics.Matrix matrix = new android.graphics.Matrix();
        view.transformMatrixToGlobal(matrix);
        float[] point = {0, 0}; matrix.mapPoints(point);
        android.graphics.Matrix panelToGlobal = new android.graphics.Matrix();
        panel.transformMatrixToGlobal(panelToGlobal);
        android.graphics.Matrix globalToPanel = new android.graphics.Matrix();
        assertTrue(panelToGlobal.invert(globalToPanel)); globalToPanel.mapPoints(point);
        return new int[]{Math.round(point[0]), Math.round(point[1])};
    }
    private void touch(int action, float x, float y) {
        clock += 24;
        MotionEvent event = MotionEvent.obtain(0, clock, action, x, y, 0);
        panel.dispatchTouchEvent(event); event.recycle();
    }
    private void settle() { Shadows.shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(350)); }
    private static View named(View view, String name) {
        if (name.contentEquals(view.getContentDescription() == null ? "" : view.getContentDescription())) return view;
        if (view instanceof ViewGroup) for (int i = 0; i < ((ViewGroup) view).getChildCount(); i++) {
            View found = named(((ViewGroup) view).getChildAt(i), name); if (found != null) return found;
        }
        return null;
    }
    private static int countNamed(View view, String name) {
        int count = name.contentEquals(view.getContentDescription() == null ? "" : view.getContentDescription()) ? 1 : 0;
        if (view instanceof ViewGroup) for (int i = 0; i < ((ViewGroup) view).getChildCount(); i++)
            count += countNamed(((ViewGroup) view).getChildAt(i), name);
        return count;
    }
    private static int countStarterCards(View view) {
        int count = view.getTag() instanceof String && ((String) view.getTag()).startsWith("ai-welcome-") ? 1 : 0;
        if (view instanceof ViewGroup) for (int i = 0; i < ((ViewGroup) view).getChildCount(); i++)
            count += countStarterCards(((ViewGroup) view).getChildAt(i));
        return count;
    }
}
