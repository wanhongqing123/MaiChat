// 文件作用：在本机假模型上验证正常输入栏问答和持久历史，不调用付费模型。
package com.kongshang.maichat;

import static org.junit.Assert.*;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.PixelCopy;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.TextView;
import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.io.FileOutputStream;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class AIAssistantQuestionConversationInstrumentedTest {
    @Test public void questionAndAnswerUseOnlyTheNormalComposerAndSurviveTranscriptReload() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        Intent launch = new Intent(context, AIAssistantTestActivity.class).putExtra("model_url", "http://127.0.0.1:18197");
        try (ActivityScenario<AIAssistantTestActivity> scenario = ActivityScenario.launch(launch)) {
            await(scenario, panel -> panel.controller.state.ready && panel.controller.modelSettingsReady());
            scenario.onActivity(activity -> {
                activity.panel.composer.setText("ask-normal-question");
                assertTrue(activity.panel.send.isEnabled());
                activity.panel.send.performClick();
            });
            await(scenario, panel -> questions(panel).length() == 1 && visible(panel, "请选择工作目录"));
            scenario.onActivity(activity -> {
                assertEquals("A question must not create an extra answer field", 1, editors(activity.panel));
                assertFalse("Options are transcript text, not choice buttons", optionButton(activity.panel, "当前目录"));
                assertFalse(optionButton(activity.panel, "项目目录"));
                assertTrue(visible(activity.panel, "当前目录"));
                assertTrue(visible(activity.panel, "项目目录"));
            });
            capture(scenario, "android-question-normal-transcript.png");
            scenario.onActivity(activity -> {
                // 旧目录可能未带 turn/configured；有效 question ID 的回答不能被新任务配置门槛挡住。
                AIAssistantController.State original = activity.panel.controller.state;
                try {
                    JSONObject data = new JSONObject(original.data.toString()).put("configured", false);
                    JSONArray sessions = data.optJSONArray("sessions");
                    for (int i = 0; sessions != null && i < sessions.length(); i++) {
                        JSONObject session = sessions.optJSONObject(i);
                        if (session != null && original.selected.equals(session.optString("id")))
                            session.put("busy", true).put("turnId", "");
                    }
                    activity.panel.onState(new AIAssistantController.State(data, original.remoteSession, original.selected,
                        original.baseUrl, original.model, original.policy, original.error, original.syncStatus,
                        original.restoreDraft, original.restoreGeneration, original.remoteCommandPending, true));
                } catch (Exception invalid) { throw new AssertionError(invalid); }
                activity.panel.composer.setText("使用项目目录，继续处理");
                activity.panel.send.performClick();
                assertEquals("", activity.panel.composer.getText().toString());
            });
            await(scenario, panel -> questions(panel).length() == 0 && !panel.controller.state.busy()
                && visible(panel, "使用项目目录，继续处理") && visible(panel, "已按你的回答继续处理"));
            scenario.onActivity(activity -> {
                assertEquals(1, editors(activity.panel));
                assertEquals(0, activity.panel.controller.state.data.optJSONArray("outgoing").length());
                View copy = findText(activity.panel, "复制");
                assertNotNull(copy); copy.performClick();
                android.content.ClipboardManager clipboard = (android.content.ClipboardManager)
                    activity.getSystemService(Context.CLIPBOARD_SERVICE);
                String copied = clipboard.getPrimaryClip().getItemAt(0).getText().toString();
                assertTrue(copied.contains("请选择工作目录"));
                assertTrue(copied.contains("使用项目目录，继续处理"));
            });
            // A force snapshot rebinds from persisted question tool input/output, not a transient answer card.
            scenario.onActivity(activity -> activity.panel.controller.action("rename", json("title", "Question fixture"), null));
            await(scenario, panel -> visible(panel, "请选择工作目录") && visible(panel, "使用项目目录，继续处理"));
            capture(scenario, "android-question-answered-transcript.png");
        }
    }
    @Test public void childQuestionIsInterleavedBetweenItsParentsRealTextParts() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        Intent launch = new Intent(context, AIAssistantTestActivity.class).putExtra("model_url", "http://127.0.0.1:18197");
        try (ActivityScenario<AIAssistantTestActivity> scenario = ActivityScenario.launch(launch)) {
            await(scenario, panel -> panel.controller.state.ready);
            final JSONObject[] fixture = new JSONObject[1];
            scenario.onActivity(activity -> {
                activity.panel.controller.clearListener(activity.panel);
                AIAssistantController.State original = activity.panel.controller.state;
                try {
                    JSONObject before = new JSONObject().put("id", "prt_000000000A00000000000").put("created", 0)
                        .put("kind", "text").put("text", "父任务先检查工作目录。");
                    JSONObject after = new JSONObject().put("id", "prt_000000000Y00000000000")
                        .put("kind", "text").put("text", "父任务继续处理。");
                    JSONObject parent = new JSONObject().put("id", "parent-message").put("role", "assistant")
                        .put("created", 1).put("completed", 35).put("parts", new JSONArray().put(before).put(after));
                    JSONObject question = new JSONObject().put("id", "prt_000000000M00000000000").put("kind", "tool").put("tool", "question").put("state", "completed")
                        .put("input", new JSONObject().put("question", "子任务请选择工作目录")
                            .put("options", new JSONArray().put("当前目录").put("项目目录")).toString())
                        .put("output", "使用项目目录。");
                    JSONObject child = new JSONObject().put("id", "child-message").put("sessionId", "child-session")
                        .put("role", "assistant").put("questionProjection", true).put("created", 2).put("completed", 35)
                        .put("parts", new JSONArray().put(question));
                    JSONObject data = new JSONObject().put("messages", new JSONArray().put(parent))
                        .put("questionMessages", new JSONArray().put(child)).put("questions", new JSONArray())
                        .put("sessions", new JSONArray().put(new JSONObject().put("id", "fixture-parent").put("busy", false)))
                        .put("configured", true).put("outgoing", new JSONArray());
                    fixture[0] = data;
                    activity.panel.onState(new AIAssistantController.State(data, null, "fixture-parent",
                        original.baseUrl, original.model, original.policy, "", "", "", 0, false, true));
                } catch (Exception invalid) { throw new AssertionError(invalid); }
            });
            await(scenario, panel -> visible(panel, "父任务继续处理。"));
            scenario.onActivity(activity -> {
                View before = findText(activity.panel, "父任务先检查工作目录。");
                View question = findText(activity.panel, "子任务请选择工作目录");
                View answer = findText(activity.panel, "使用项目目录。");
                View after = findText(activity.panel, "父任务继续处理。");
                assertNotNull(before); assertNotNull(question); assertNotNull(answer); assertNotNull(after);
                assertTrue(top(before) < top(question));
                assertTrue(top(question) < top(answer));
                assertTrue(top(answer) < top(after));
            });
            capture(scenario, "android-child-question-interleaved.png");
            scenario.onActivity(activity -> {
                AIAssistantController.State original = activity.panel.controller.state;
                try {
                    JSONObject data = new JSONObject(fixture[0].toString());
                    JSONArray updates = data.getJSONArray("messages");
                    JSONObject recent = new JSONObject().put("id", "recent-input").put("role", "user").put("created", 15)
                        .put("parts", new JSONArray().put(new JSONObject().put("id", "recent-input-text")
                            .put("kind", "text").put("text", "新的补充消息")));
                    data.put("messages", new JSONArray().put(recent)).put("messageDisplayUpdates", updates);
                    activity.panel.onState(new AIAssistantController.State(data, null, "fixture-parent", original.baseUrl,
                        original.model, original.policy, "", "", "", 0, false, true));
                } catch (Exception invalid) { throw new AssertionError(invalid); }
            });
            await(scenario, panel -> visible(panel, "新的补充消息") && visible(panel, "父任务继续处理。"));
            scenario.onActivity(activity -> {
                assertNull("Old preamble must not be replayed by an out-of-window update", findText(activity.panel, "父任务先检查工作目录。"));
                assertTrue(top(findText(activity.panel, "新的补充消息")) < top(findText(activity.panel, "子任务请选择工作目录")));
                assertTrue(top(findText(activity.panel, "子任务请选择工作目录")) < top(findText(activity.panel, "父任务继续处理。")));
            });
            capture(scenario, "android-old-root-update-recent-parts.png");
        }
    }
    private static View findText(View root, String value) {
        if (root instanceof TextView && ((TextView) root).getText().toString().contains(value)) return root;
        if (root instanceof ViewGroup) for (int i = 0; i < ((ViewGroup) root).getChildCount(); i++) {
            View found = findText(((ViewGroup) root).getChildAt(i), value);
            if (found != null) return found;
        }
        return null;
    }
    private static int top(View view) { int[] point = new int[2]; view.getLocationOnScreen(point); return point[1]; }

    private interface Check { boolean test(AIAssistantPanel panel) throws Exception; }
    private void await(ActivityScenario<AIAssistantTestActivity> scenario, Check check) throws Exception {
        AtomicBoolean done = new AtomicBoolean();
        long deadline = SystemClock.uptimeMillis() + 30000;
        while (!done.get() && SystemClock.uptimeMillis() < deadline) {
            scenario.onActivity(activity -> {
                try { done.set(check.test(activity.panel)); }
                catch (Exception failure) { throw new AssertionError(failure); }
            });
            if (!done.get()) SystemClock.sleep(50);
        }
        assertTrue("Question fixture did not reach its expected state", done.get());
    }
    private static JSONArray questions(AIAssistantPanel panel) {
        JSONArray value = panel.controller.state.data.optJSONArray("questions");
        return value == null ? new JSONArray() : value;
    }
    private static JSONObject json(String key, String value) {
        try { return new JSONObject().put(key, value); }
        catch (Exception failure) { throw new AssertionError(failure); }
    }
    private static int editors(View root) {
        int count = root instanceof EditText ? 1 : 0;
        if (root instanceof ViewGroup) for (int i = 0; i < ((ViewGroup) root).getChildCount(); i++)
            count += editors(((ViewGroup) root).getChildAt(i));
        return count;
    }
    private static boolean optionButton(View root, String value) {
        if (root instanceof Button && value.contentEquals(((Button) root).getText())) return true;
        if (root instanceof ViewGroup) for (int i = 0; i < ((ViewGroup) root).getChildCount(); i++)
            if (optionButton(((ViewGroup) root).getChildAt(i), value)) return true;
        return false;
    }
    private static boolean visible(View root, String value) {
        android.graphics.Rect bounds = new android.graphics.Rect();
        if (root instanceof TextView && ((TextView) root).getText().toString().contains(value)
                && root.getGlobalVisibleRect(bounds) && !bounds.isEmpty()) return true;
        if (root instanceof ViewGroup) for (int i = 0; i < ((ViewGroup) root).getChildCount(); i++)
            if (visible(((ViewGroup) root).getChildAt(i), value)) return true;
        return false;
    }
    private static void capture(ActivityScenario<AIAssistantTestActivity> scenario, String name) throws Exception {
        InstrumentationRegistry.getInstrumentation().waitForIdleSync();
        SystemClock.sleep(200);
        CountDownLatch copied = new CountDownLatch(1);
        AtomicInteger result = new AtomicInteger(-1);
        scenario.onActivity(activity -> {
            View decor = activity.getWindow().getDecorView();
            Bitmap bitmap = Bitmap.createBitmap(decor.getWidth(), decor.getHeight(), Bitmap.Config.ARGB_8888);
            PixelCopy.request(activity.getWindow(), bitmap, code -> {
                result.set(code);
                if (code == PixelCopy.SUCCESS) try (FileOutputStream stream = new FileOutputStream(new File(activity.getCacheDir(), name))) {
                    bitmap.compress(Bitmap.CompressFormat.PNG, 100, stream);
                } catch (Exception failure) { result.set(-2); }
                bitmap.recycle(); copied.countDown();
            }, new Handler(Looper.getMainLooper()));
        });
        assertTrue(copied.await(10, TimeUnit.SECONDS));
        assertEquals(PixelCopy.SUCCESS, result.get());
    }
}
