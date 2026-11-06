// 文件作用：真实生产“我的”渲染与普通存储 JNI 的本地夹具回归，不访问线上账号内容。
package com.kongshang.maichat;

import android.content.Context;
import android.content.res.Configuration;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;
import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.io.FileOutputStream;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.function.BooleanSupplier;
import org.json.JSONObject;
import org.json.JSONArray;
import org.junit.Test;
import org.junit.runner.RunWith;
import static org.junit.Assert.*;

@RunWith(AndroidJUnit4.class)
public class MySettingsInstrumentedTest {
    @Test public void creationToolRunsKeepTextOrderAndIndependentExpansion() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        File screenshots = new File(context.getFilesDir(), "ToolOrderTest-visual");
        try (ActivityScenario<MainActivity> scenario = ActivityScenario.launch(MainActivity.class)) {
            on(scenario, activity -> {
                ((RemoteIMSessionController) get(activity, "session")).destroy();
                File directory = new File(activity.getFilesDir(), "ToolOrderTest-" + System.nanoTime());
                assertTrue(directory.mkdirs());
                RemoteIMSessionController fixture = new RemoteIMSessionController(
                    new LocalSettingsStore(new File(directory, "settings")), new LocalChatHistoryStore(new File(directory, "history")));
                fixture.login("tool-order-fixture");
                set(activity, "session", fixture); set(activity, "showInitialLogin", false);
                // 先走正常账号页面切换；首次 render 会释放上个账号的旧创作页。
                set(activity, "activeTab", RemoteIMTab.ME); render(activity);
                // 原生 MainActivity/Panel 使用本地快照，不执行工具或访问付费模型。
                AIAssistantController controller = new AIAssistantController(activity, "http://127.0.0.1:9");
                AIAssistantPanel panel = new AIAssistantPanel(activity, controller);
                controller.close();
                set(activity, "aiAssistant", panel); set(activity, "activeTab", RemoteIMTab.ASSISTANT);
                render(activity); panel.onState(toolOrderState(0));
            });
            capture(screenshots, "01-initial");
            on(scenario, activity -> {
                View root = activity.getWindow().getDecorView();
                assertOrder(root, "先检查文件，再执行处理。", "⌘  工具调用 2 次，已完成 · file_read ×1 · file_stat ×1  ›",
                    "A 段说明：文件检查已经完成。");
                assertTrue(description(root, "工具调用 2 次，已完成 · file_read ×1 · file_stat ×1").performClick());
            });
            InstrumentationRegistry.getInstrumentation().waitForIdleSync();
            on(scenario, activity -> ((AIAssistantPanel) get(activity, "aiAssistant")).onState(toolOrderState(1)));
            capture(screenshots, "02-next-run-streaming");
            on(scenario, activity -> {
                View root = activity.getWindow().getDecorView();
                assertOrder(root, "A 段说明：文件检查已经完成。", "⌘  工具调用 1 次，运行中 · shell ×1  ›");
                assertTrue(exactText(root, "已完成 file_read  ›").isShown());
                ((AIAssistantPanel) get(activity, "aiAssistant")).onState(toolOrderState(2));
            });
            capture(screenshots, "02b-current-run-completed");
            on(scenario, activity -> {
                // 片段 ID 不变时，也必须刷新本次快照的工具状态。
                assertNotNull(description(activity.getWindow().getDecorView(), "工具调用 1 次，已完成 · shell ×1"));
                ((AIAssistantPanel) get(activity, "aiAssistant")).onState(toolOrderState(3));
            });
            capture(screenshots, "03-complete");
            on(scenario, activity -> {
                View root = activity.getWindow().getDecorView();
                assertOrder(root, "A 段说明：文件检查已经完成。", "⌘  工具调用 1 次，已完成 · shell ×1  ›",
                    "B 段说明：处理结果已经核对。", "⌘  工具调用 2 次，已完成 · file_write ×1 · shell ×1  ›",
                    "C 段说明：全部完成。");
                assertTrue(description(root, "工具调用 1 次，已完成 · shell ×1").performClick());
            });
            InstrumentationRegistry.getInstrumentation().waitForIdleSync();
            on(scenario, activity -> {
                View root = activity.getWindow().getDecorView();
                assertTrue(description(root, "工具调用 2 次，已完成 · file_read ×1 · file_stat ×1").performClick());
            });
            InstrumentationRegistry.getInstrumentation().waitForIdleSync();
            on(scenario, activity -> {
                View root = activity.getWindow().getDecorView();
                assertFalse(exactText(root, "已完成 file_read  ›").isShown());
                assertTrue(exactText(root, "已完成 shell  ›").isShown());
            });
            capture(screenshots, "04-independent-groups");
            on(scenario, activity -> {
                View root = activity.getWindow().getDecorView();
                assertTrue(description(root, "对话列表").performClick());
            });
            capture(screenshots, "05-drawer-body-font");
            on(scenario, activity -> {
                AIAssistantPanel panel = (AIAssistantPanel) get(activity, "aiAssistant");
                TextView title = (TextView) exactText((View) get(panel, "sessionDrawerList"), "会话字体 Hello");
                TextView input = (TextView) get(panel, "composer");
                assertNotNull(title);
                assertEquals("会话标题与正文输入框字号必须相同", input.getTextSize(), title.getTextSize(), .01f);
                assertEquals("会话标题必须采用所选正文字体", input.getTypeface(), title.getTypeface());
            });
        }
    }

    private static AIAssistantController.State toolOrderState(int stage) throws Exception {
        JSONArray parts = new JSONArray()
            .put(transcriptText("intro", "先检查文件，再执行处理。"))
            .put(transcriptTool("tool-a1", "file_read", false)).put(transcriptTool("tool-a2", "file_stat", false))
            .put(transcriptText("answer-a", "A 段说明：文件检查已经完成。"));
        if (stage > 0) parts.put(transcriptTool("tool-b1", "shell", stage == 1));
        if (stage > 2) parts.put(transcriptText("answer-b", "B 段说明：处理结果已经核对。"))
            .put(transcriptTool("tool-c1", "file_write", false)).put(transcriptTool("tool-c2", "shell", false))
            .put(transcriptText("answer-c", "C 段说明：全部完成。"));
        long created = System.currentTimeMillis() - 4000;
        JSONObject message = new JSONObject().put("id", "tool-order-message").put("role", "assistant")
            .put("created", created).put("completed", stage > 2 ? created + 4000 : 0)
            .put("active", stage < 3).put("parts", parts);
        JSONObject data = new JSONObject().put("ok", true).put("configured", true)
            .put("messages", new JSONArray().put(message)).put("sessions", new JSONArray()
                .put(new JSONObject().put("id", "tool-order-fixture").put("title", "会话字体 Hello").put("busy", stage < 3)));
        return new AIAssistantController.State(data, null, "tool-order-fixture", "", "test-model", "", "", "", "", 0, false, true);
    }
    private static JSONObject transcriptText(String id, String text) throws Exception {
        return new JSONObject().put("id", id).put("kind", "text").put("text", text);
    }
    private static JSONObject transcriptTool(String id, String name, boolean running) throws Exception {
        return new JSONObject().put("id", id).put("kind", "tool").put("tool", name)
            .put("state", running ? "running" : "completed").put("input", "{}").put("output", "执行结果");
    }
    private static View exactText(View root, String text) {
        if (root instanceof TextView && ((TextView) root).getText().toString().equals(text)) return root;
        if (root instanceof ViewGroup) for (int i = 0; i < ((ViewGroup) root).getChildCount(); i++) {
            View found = exactText(((ViewGroup) root).getChildAt(i), text); if (found != null) return found;
        }
        return null;
    }
    private static void assertOrder(View root, String... texts) {
        int previousBottom = Integer.MIN_VALUE;
        for (String text : texts) {
            View view = exactText(root, text); assertNotNull(text, view);
            int[] position = new int[2]; view.getLocationOnScreen(position);
            assertTrue(text + " must follow previous content", position[1] >= previousBottom);
            previousBottom = position[1] + view.getHeight();
        }
    }

    @Test public void fontPreferenceDialogSwitchesTheRunningApplication() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String previousFamily = MaiChatPreferences.fontFamily(context);
        File screenshots = new File(context.getFilesDir(), "FontFamilyDialogTest-visual");
        try {
            MaiChatPreferences.set(context, MaiChatFontFamilyTokens.PREFERENCE_KEY, "system");
            try (ActivityScenario<MainActivity> scenario = ActivityScenario.launch(MainActivity.class)) {
                on(scenario, activity -> {
                    ((RemoteIMSessionController) get(activity, "session")).destroy();
                    File directory = new File(activity.getFilesDir(), "FontFamilyDialogTest-" + System.nanoTime());
                    assertTrue(directory.mkdirs());
                    RemoteIMSessionController fixture = new RemoteIMSessionController(
                        new LocalSettingsStore(new File(directory, "settings")), new LocalChatHistoryStore(new File(directory, "history")));
                    fixture.login("font-dialog-fixture");
                    set(activity, "session", fixture); set(activity, "showInitialLogin", false);
                    set(activity, "activeTab", RemoteIMTab.ME); render(activity);
                });
                click(scenario, "偏好设置"); click(scenario, "字体");
                capture(screenshots, "font-choice-open");
                on(scenario, activity -> {
                    android.widget.ListView choices = null;
                    for (View window : android.view.inspector.WindowInspector.getGlobalWindowViews()) {
                        choices = listView(window); if (choices != null) break;
                    }
                    assertNotNull("The actual font choice dialog must be visible", choices);
                    assertEquals(3, choices.getAdapter().getCount());
                    assertTrue(choices.performItemClick(choices.getChildAt(2), 2, choices.getAdapter().getItemId(2)));
                });
                InstrumentationRegistry.getInstrumentation().waitForIdleSync();
                on(scenario, activity -> {
                    assertEquals("notoSerif", MaiChatPreferences.fontFamily(activity));
                    assertEquals("notoSerif", MaiChatTypography.familyId());
                    // 字体弹窗必须经过真实 recreate 路径。本地内存账号没有可恢复的线上
                    // 鉴权会话，不把该替身的登录页状态当作字体切换导致账号退出的证据。
                    assertEquals("notoSerif", MaiChatPreferences.fontFamily(activity.getApplicationContext()));
                });
                capture(screenshots, "font-choice-applied");
            }
        } finally { MaiChatPreferences.set(context, MaiChatFontFamilyTokens.PREFERENCE_KEY, previousFamily); }
    }

    private static android.widget.ListView listView(View root) {
        if (root instanceof android.widget.ListView) return (android.widget.ListView) root;
        if (root instanceof ViewGroup) for (int index = 0; index < ((ViewGroup) root).getChildCount(); ++index) {
            android.widget.ListView found = listView(((ViewGroup) root).getChildAt(index)); if (found != null) return found;
        }
        return null;
    }

    @Test public void selectedFontFamilyReachesMenuEditorAndMarkdownWithoutChangingBodySize() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String previousFamily = MaiChatPreferences.fontFamily(context), previousSize = MaiChatPreferences.font(context);
        File screenshots = new File(context.getFilesDir(), "FontFamilyTest-visual");
        try {
            MaiChatPreferences.set(context, "font", "standard");
            for (String family : new String[]{"system", "notoSans", "notoSerif"}) {
                MaiChatPreferences.set(context, MaiChatFontFamilyTokens.PREFERENCE_KEY, family);
                try (ActivityScenario<MainActivity> scenario = ActivityScenario.launch(MainActivity.class)) {
                    on(scenario, activity -> {
                        assertEquals(family, MaiChatTypography.familyId());
                        ((RemoteIMSessionController) get(activity, "session")).destroy();
                        File directory = new File(activity.getFilesDir(), "FontFamilyTest-" + System.nanoTime());
                        assertTrue(directory.mkdirs());
                        RemoteIMSessionController fixture = new RemoteIMSessionController(
                            new LocalSettingsStore(new File(directory, "settings")),
                            new LocalChatHistoryStore(new File(directory, "history")));
                        set(fixture, "remoteDesktop", new RemoteDesktopController(activity, null, () -> {}));
                        fixture.login("font-family-fixture");
                        fixture.chatState().upsertContact(new RemoteIMContact("font-family-peer", "Hello 你好"));
                        fixture.chatState().receiveText("Hello 你好，**Bold 加粗** *Italic* `code`\n\n# Heading 标题\n\n| Case | Value |\n| --- | --- |\n| 预期 Expected | 320x180<br>details |", "font-family-peer");
                        set(activity, "session", fixture); set(activity, "showInitialLogin", false);
                        set(activity, "activeTab", RemoteIMTab.ME); render(activity);
                        assertTrue(assertFontRoles(activity.getWindow().getDecorView()) > 5);
                    });
                    capture(screenshots, family + "-my");
                    on(scenario, activity -> {
                        set(activity, "activeTab", RemoteIMTab.MESSAGES);
                        set(activity, "activeChatUserId", "font-family-peer");
                        ((RemoteIMSessionController) get(activity, "session")).chatState().selectPeer("font-family-peer");
                        render(activity);
                        TextView input = (TextView) get(activity, "messageInput");
                        assertEquals(MaiChatTypography.normal(), input.getTypeface());
                        assertEquals(16f * activity.getResources().getDisplayMetrics().density, input.getTextSize(), .01f);
                    });
                    capture(screenshots, family + "-im-markdown");
                }
            }
        } finally {
            MaiChatPreferences.set(context, MaiChatFontFamilyTokens.PREFERENCE_KEY, previousFamily);
            MaiChatPreferences.set(context, "font", previousSize);
        }
    }

    @Test public void fontPresetsReachProductionPagesInputsAndMarkdown() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String previousTheme = MaiChatPreferences.theme(context), previousFont = MaiChatPreferences.font(context);
        File screenshots = new File(context.getFilesDir(), "TypographyTest-visual");
        try {
            for (String preset : new String[]{"standard", "large", "extraLarge"}) {
                MaiChatPreferences.set(context, "theme", "dark");
                MaiChatPreferences.set(context, "font", preset);
                try (ActivityScenario<MainActivity> scenario = ActivityScenario.launch(MainActivity.class)) {
                    on(scenario, activity -> {
                        ((RemoteIMSessionController) get(activity, "session")).destroy();
                        File directory = new File(activity.getFilesDir(), "TypographyTest-" + System.nanoTime());
                        assertTrue(directory.mkdirs());
                        RemoteIMSessionController fixture = new RemoteIMSessionController(
                            new LocalSettingsStore(new File(directory, "settings")),
                            new LocalChatHistoryStore(new File(directory, "history")));
                        set(fixture, "remoteDesktop", new RemoteDesktopController(activity, null, () -> {}));
                        fixture.login("typography-fixture");
                        fixture.chatState().upsertContact(new RemoteIMContact("typography-peer", "Hello 你好 Typography"));
                        fixture.chatState().receiveText("# Heading 标题\n\nHello **Bold 加粗** and *Italic* `code`\n\n- List 正文\n\n> Quote 引用\n\n```java\nString greeting = \"Hello 你好\";\n```", "typography-peer");
                        set(activity, "session", fixture); set(activity, "showInitialLogin", false);
                        set(activity, "activeTab", RemoteIMTab.ME); render(activity);
                        assertTrue(MaiChatTheme.isDark());
                        assertTrue(assertFontRoles(activity.getWindow().getDecorView()) > 5);
                    });
                    capture(screenshots, preset + "-dark-my");
                    on(scenario, activity -> {
                        set(activity, "activeTab", RemoteIMTab.MESSAGES);
                        set(activity, "activeChatUserId", "typography-peer");
                        ((RemoteIMSessionController) get(activity, "session")).chatState().selectPeer("typography-peer");
                        render(activity);
                        TextView input = (TextView) get(activity, "messageInput");
                        assertNotNull(input);
                        float body = MaiChatTypographyTokens.Role.BODY.points
                            * MaiChatTypographyTokens.scale(preset, 1);
                        assertEquals(body * activity.getResources().getDisplayMetrics().density, input.getTextSize(), .01f);
                        assertTrue(assertFontRoles(activity.getWindow().getDecorView()) > 1);
                    });
                    capture(screenshots, preset + "-dark-im-markdown");
                    on(scenario, activity -> {
                        // 放大后的长标题不能把返回、远控和更多操作挤出窗口。
                        for (String action : new String[]{"返回会话列表", "远程控制", "更多聊天操作"}) {
                            View button = description(activity.getWindow().getDecorView(), action);
                            assertNotNull(action, button);
                            android.graphics.Rect visible = new android.graphics.Rect();
                            assertTrue(preset + ": " + action + " must remain visible", button.getGlobalVisibleRect(visible));
                            assertTrue(preset + ": " + action + " hit area clipped: " + visible,
                                visible.width() >= MaiChatTheme.dp(activity, 35));
                            int windowWidth = activity.getWindowManager().getCurrentWindowMetrics().getBounds().width();
                            assertTrue(preset + ": " + action + " outside window: " + visible + "/" + windowWidth,
                                visible.left >= 0 && visible.right <= windowWidth);
                        }
                    });
                }
            }
        } finally {
            MaiChatPreferences.set(context, "theme", previousTheme);
            MaiChatPreferences.set(context, "font", previousFont);
        }
    }

    private static int assertFontRoles(View root) {
        int count = 0;
        Object role = root.getTag(R.id.typography_role);
        if (root instanceof TextView && role instanceof MaiChatTypographyTokens.Role) {
            assertEquals(MaiChatTypography.pixels(root.getContext(), (MaiChatTypographyTokens.Role) role),
                ((TextView) root).getTextSize(), .01f);
            count++;
        }
        if (root instanceof ViewGroup) for (int i = 0; i < ((ViewGroup) root).getChildCount(); i++)
            count += assertFontRoles(((ViewGroup) root).getChildAt(i));
        return count;
    }

    private static TextView tableText(View root) {
        if (root instanceof TextView && ((TextView) root).getText() instanceof android.text.Spanned) {
            android.text.Spanned text = (android.text.Spanned) ((TextView) root).getText();
            if (text.getSpans(0, text.length(), io.noties.markwon.ext.tables.TableRowSpan.class).length > 0)
                return (TextView) root;
        }
        if (root instanceof ViewGroup) for (int i = 0; i < ((ViewGroup) root).getChildCount(); i++) {
            TextView found = tableText(((ViewGroup) root).getChildAt(i)); if (found != null) return found;
        }
        return null;
    }

    @Test public void productionImMarkdownTableKeepsCellBreaksAndLiteralCode() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String source = "| Case | Result |\n| --- | --- |\n"
            + "| **Expected**:<br>320x180<br><br>details | Actual:<br/>320x240<br />next<BR>last |\n\n`literal <br>`";
        // 正常聊天也从后台准备缓存；测试等待明确回执，不在 UI 线程解析 Markdown。
        MarkdownRenderer.initialize(context);
        java.util.concurrent.CountDownLatch prepared = new java.util.concurrent.CountDownLatch(1);
        MarkdownRenderer.prepare(java.util.Collections.singletonList(source), () -> true, prepared::countDown);
        assertTrue(prepared.await(15, java.util.concurrent.TimeUnit.SECONDS));
        try (ActivityScenario<MainActivity> scenario = ActivityScenario.launch(MainActivity.class)) {
            on(scenario, activity -> {
                ((RemoteIMSessionController) get(activity, "session")).destroy();
                File directory = new File(activity.getCacheDir(), "MarkdownTest-" + System.nanoTime());
                assertTrue(directory.mkdirs());
                RemoteIMSessionController fixture = new RemoteIMSessionController(
                    new LocalSettingsStore(new File(directory, "settings")),
                    new LocalChatHistoryStore(new File(directory, "history")));
                // 旧纯数据宿主不创建远控对象；正常聊天页头需要它的只读 idle 状态。
                // 本用例不点击远控、不登录 SDK，也不建立远控或网络连接。
                set(fixture, "remoteDesktop", new RemoteDesktopController(activity, null, () -> {}));
                fixture.login("markdown-fixture");
                fixture.chatState().upsertContact(new RemoteIMContact("markdown-peer", "Markdown 表格"));
                fixture.chatState().receiveText(source, "markdown-peer");
                fixture.chatState().selectPeer("markdown-peer");
                set(activity, "session", fixture); set(activity, "showInitialLogin", false);
                set(activity, "activeTab", RemoteIMTab.MESSAGES);
                set(activity, "activeChatUserId", "markdown-peer"); render(activity);
            });
            java.util.concurrent.CountDownLatch visible = new java.util.concurrent.CountDownLatch(1);
            long deadline = System.nanoTime() + 10_000_000_000L;
            on(scenario, activity -> activity.getWindow().getDecorView().post(new Runnable() {
                @Override public void run() {
                    if (tableText(activity.getWindow().getDecorView()) != null) visible.countDown();
                    else if (System.nanoTime() < deadline)
                        activity.getWindow().getDecorView().postDelayed(this, 25);
                }
            }));
            assertTrue("Production IM must finish background table preparation",
                visible.await(15, java.util.concurrent.TimeUnit.SECONDS));
            on(scenario, activity -> {
                TextView view = tableText(activity.getWindow().getDecorView());
                assertNotNull("Production IM must render the table span", view);
                android.text.Spanned text = (android.text.Spanned) view.getText();
                io.noties.markwon.ext.tables.TableRowSpan[] rows = text.getSpans(0, text.length(),
                    io.noties.markwon.ext.tables.TableRowSpan.class);
                assertEquals(2, rows.length);
                Field cellsField = io.noties.markwon.ext.tables.TableRowSpan.class.getDeclaredField("cells");
                cellsField.setAccessible(true);
                boolean found = false;
                for (io.noties.markwon.ext.tables.TableRowSpan row : rows) {
                    java.util.List<?> cells = (java.util.List<?>) cellsField.get(row);
                    String first = ((io.noties.markwon.ext.tables.TableRowSpan.Cell) cells.get(0)).text().toString();
                    if ("Expected:\n320x180\n\ndetails".equals(first)) found = true;
                }
                assertTrue("The body cell must contain actual line breaks", found);
                assertTrue(text.toString().contains("literal <br>"));
            });
            capture(new File(context.getFilesDir(), "MarkdownTest-visual"), "markdown-im-table-breaks");
        }
    }

    private static Object get(Object owner, String name) throws Exception {
        Field field = owner.getClass().getDeclaredField(name); field.setAccessible(true); return field.get(owner);
    }
    private static void set(Object owner, String name, Object value) throws Exception {
        Field field = owner.getClass().getDeclaredField(name); field.setAccessible(true); field.set(owner, value);
    }
    private static void render(MainActivity activity) throws Exception {
        Method method = MainActivity.class.getDeclaredMethod("render"); method.setAccessible(true); method.invoke(activity);
    }
    private static View description(View root, String value) {
        if (value.contentEquals(root.getContentDescription() == null ? "" : root.getContentDescription())) return root;
        if (root instanceof ViewGroup) for (int i = 0; i < ((ViewGroup) root).getChildCount(); i++) {
            View found = description(((ViewGroup) root).getChildAt(i), value); if (found != null) return found;
        }
        return null;
    }
    private static boolean text(View root, String value) {
        if (root instanceof TextView && ((TextView) root).getText().toString().contains(value)) return true;
        if (root instanceof ViewGroup) for (int i = 0; i < ((ViewGroup) root).getChildCount(); i++)
            if (text(((ViewGroup) root).getChildAt(i), value)) return true;
        return false;
    }
    private interface Action { void run(MainActivity activity) throws Exception; }
    private static void on(ActivityScenario<MainActivity> scenario, Action action) {
        scenario.onActivity(activity -> { try { action.run(activity); } catch (Exception error) { throw new AssertionError(error); } });
    }
    private static void click(ActivityScenario<MainActivity> scenario, String title) {
        on(scenario, activity -> {
            View view = description(activity.getWindow().getDecorView(), title);
            assertNotNull(title, view); assertTrue(view.performClick());
        });
    }
    private static void capture(File directory, String name) throws Exception {
        if (!"true".equals(InstrumentationRegistry.getArguments().getString("maichat.capture_my_ui"))) return;
        InstrumentationRegistry.getInstrumentation().waitForIdleSync();
        android.os.SystemClock.sleep(1000);
        android.graphics.Bitmap bitmap = InstrumentationRegistry.getInstrumentation().getUiAutomation().takeScreenshot();
        assertNotNull(bitmap); assertTrue(directory.isDirectory() || directory.mkdirs());
        File file = new File(directory, name + ".png");
        try (FileOutputStream out = new FileOutputStream(file)) { assertTrue(bitmap.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, out)); }
        finally { bitmap.recycle(); }
        System.out.println("MY_SETTINGS_QA_SCREENSHOT " + file.getAbsolutePath());
    }
    @Test public void productionMyRoutesUseRealCapabilitiesAndReturnHome() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        // 验收截图不放缓存：低存储模拟器可能在测试结束前回收 cache，导致证据丢失。
        File screenshots = new File(context.getFilesDir(), "MySettingsTest-visual-" + System.nanoTime());
        try (ActivityScenario<MainActivity> scenario = ActivityScenario.launch(MainActivity.class)) {
            on(scenario, activity -> {
                ((RemoteIMSessionController) get(activity, "session")).destroy();
                File directory = new File(activity.getCacheDir(), "MySettingsTest-" + System.nanoTime());
                assertTrue(directory.mkdirs());
                RemoteIMSessionController fixture = new RemoteIMSessionController(
                    new LocalSettingsStore(new File(directory, "settings")), new LocalChatHistoryStore(new File(directory, "history")));
                fixture.login("my-settings-fixture");
                set(activity, "session", fixture); set(activity, "showInitialLogin", false);
                set(activity, "activeTab", RemoteIMTab.ME); render(activity);
                View root = activity.getWindow().getDecorView();
                for (String title : new String[]{"个人资料", "主题", "偏好设置", "存储空间", "账号与安全", "关于 MaiChat", "当前版本", "退出登录"})
                    assertNotNull(title, description(root, title));
                assertFalse(text(root, "500")); assertFalse(text(root, "积分")); assertFalse(text(root, "升级套餐")); assertFalse(text(root, "记忆库"));
            });
            capture(screenshots, "01-my");
            click(scenario, "偏好设置");
            on(scenario, activity -> {
                View root = activity.getWindow().getDecorView();
                for (String title : new String[]{"语言", "消息通知", "字体大小", "触感反馈", "语音识别服务"}) assertNotNull(title, description(root, title));
                assertTrue(text(root, "简体中文"));
            });
            capture(screenshots, "02-preferences");
            click(scenario, "返回"); click(scenario, "关于 MaiChat");
            on(scenario, activity -> {
                View root = activity.getWindow().getDecorView();
                for (String title : new String[]{"分享诊断日志", "清除诊断日志", "意见反馈", "用户协议", "隐私政策", "儿童隐私保护政策"}) assertNotNull(title, description(root, title));
                assertFalse(text(root, "腾讯"));
            });
            capture(screenshots, "03-about");
            click(scenario, "返回"); click(scenario, "账号与安全");
            on(scenario, activity -> {
                View root = activity.getWindow().getDecorView();
                for (String title : new String[]{"设置或修改登录密码", "注销账号", "切换账号", "我的举报"}) assertNotNull(title, description(root, title));
            });
            capture(screenshots, "04-account-security");
            click(scenario, "返回"); click(scenario, "存储空间");
            long deadline = android.os.SystemClock.uptimeMillis() + 10000;
            java.util.concurrent.atomic.AtomicBoolean ready = new java.util.concurrent.atomic.AtomicBoolean();
            while (!ready.get() && android.os.SystemClock.uptimeMillis() < deadline) {
                on(scenario, activity -> ready.set(text(activity.getWindow().getDecorView(), "所列目录占用")));
                if (!ready.get()) android.os.SystemClock.sleep(50);
            }
            assertTrue("Storage must display the ordinary service result", ready.get());
            capture(screenshots, "05-storage");
            click(scenario, "返回");
            on(scenario, activity -> assertNotNull(description(activity.getWindow().getDecorView(), "个人资料")));
        }
    }
    @Test public void ordinaryStorageJniUsesExactPreviewAndKeepsRecentAndWorkspace() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        File directory = new File(context.getCacheDir(), "StorageJniTest-" + System.nanoTime());
        File temporary = new File(directory, "temporary"), cache = new File(directory, "cache"), workspace = new File(directory, "workspace");
        for (File root : new File[]{temporary, cache, workspace}) assertTrue(root.mkdirs());
        File old = new File(cache, "old.bin"), fresh = new File(cache, "fresh.bin"), original = new File(workspace, "original.png");
        for (File file : new File[]{old, fresh, original}) try (FileOutputStream out = new FileOutputStream(file)) { out.write(new byte[]{1, 2, 3}); }
        assertTrue(old.setLastModified(System.currentTimeMillis() - 48L * 3600000));
        Constructor<AIAssistantController.StorageRoots> constructor = AIAssistantController.StorageRoots.class
            .getDeclaredConstructor(File.class, File.class, File.class, BooleanSupplier.class);
        constructor.setAccessible(true);
        AIAssistantController.StorageRoots roots = constructor.newInstance(temporary, cache, workspace, (BooleanSupplier) () -> true);
        try (MaiAppStorageClient client = new MaiAppStorageClient(roots)) {
            JSONObject app = client.request(new JSONObject().put("action", "preview").put("target", "app"));
            assertFalse(app.getBoolean("cleanup_allowed")); assertFalse(app.getBoolean("workspace_cleanup_allowed"));
            JSONObject preview = client.request(new JSONObject().put("action", "preview").put("target", "cache"));
            assertEquals(1, preview.getLong("candidate_files")); assertEquals(3, preview.getLong("candidate_bytes"));
            File added = new File(cache, "added-after-preview.bin");
            try (FileOutputStream out = new FileOutputStream(added)) { out.write(4); }
            assertTrue(added.setLastModified(System.currentTimeMillis() - 48L * 3600000));
            JSONObject cleaned = client.request(new JSONObject().put("action", "cleanup").put("preview_id", preview.getString("preview_id")));
            assertEquals(1, cleaned.getLong("removed_files")); assertEquals(3, cleaned.getLong("reclaimed_bytes"));
            assertFalse(old.exists()); assertTrue(fresh.isFile()); assertTrue(added.isFile()); assertTrue(original.isFile());
            assertFalse(cleaned.has("paths"));
        }
    }
    @Test public void appearancePreferencesChangeActualConfiguration() {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String previousTheme = MaiChatPreferences.theme(context), previousFont = MaiChatPreferences.font(context);
        try {
            MaiChatPreferences.set(context, "theme", "dark"); MaiChatPreferences.set(context, "font", "large");
            Context wrapped = MaiChatPreferences.wrap(context);
            assertEquals(Configuration.UI_MODE_NIGHT_YES, wrapped.getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK);
            assertEquals(1.125f, wrapped.getResources().getConfiguration().fontScale, .001f);
            MaiChatPreferences.set(context, "theme", "light"); MaiChatPreferences.set(context, "font", "standard");
            Configuration config = MaiChatPreferences.wrap(context).getResources().getConfiguration();
            assertEquals(Configuration.UI_MODE_NIGHT_NO, config.uiMode & Configuration.UI_MODE_NIGHT_MASK); assertEquals(1f, config.fontScale, .001f);
        } finally { MaiChatPreferences.set(context, "theme", previousTheme); MaiChatPreferences.set(context, "font", previousFont); }
    }
}
