// 文件作用：在 Android 设备的真实创作中心 Panel 中复验远端快照乱序。
// 仅替换只读 SessionReads 传输，普通 Controller、原生初始化与界面仍使用产品实现。
// 此测试必须在独立测试包中运行；不登录用户账号，不访问远端设备或触发模型任务。
package com.kongshang.maichat;

import static org.junit.Assert.*;

import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.os.Handler;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;
import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.io.FileOutputStream;
import java.lang.reflect.Field;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.function.BiConsumer;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class AIAssistantRemoteSessionSyncInstrumentedTest {
    private static final String FINAL_TEXT = "远端任务已完成，最终正文保持同步。";

    @Test public void completedPanelSurvivesDelayedHistoryLiveAndCatalog() throws Exception {
        org.junit.Assume.assumeTrue("Use an independent test application, never the user's installed App",
            BuildConfig.DEBUG && BuildConfig.APPLICATION_ID.endsWith(".remotecheck20261115"));
        Context app = InstrumentationRegistry.getInstrumentation().getTargetContext();
        Reads reads = new Reads();
        try (ActivityScenario<AIAssistantTestActivity> screen = ActivityScenario.launch(
                new Intent(app, AIAssistantTestActivity.class).putExtra("model_url", "http://127.0.0.1:18190"))) {
            screen.onActivity(activity -> {
                activity.panel.closeLocalMusic();
                activity.panel.controller.close();
                activity.panel = new AIAssistantPanel(activity,
                    new AIAssistantController(activity, "http://127.0.0.1:18190", reads),
                    null, null, activity.browser);
                activity.setContentView(activity.panel);
            });
            await(screen, panel -> panel.controller.state.ready);
            select(screen, reads);
            screen.onActivity(activity -> {
                try { activity.panel.controller.acceptRemotePayload(envelope().put("id", 10), page(false)); }
                catch (Exception invalid) { throw new AssertionError(invalid); }
            });
            drain(screen);
            reads.complete.accept(page(true), "");
            drain(screen); drain(screen);
            assertCompleted(screen);

            // Re-select to cover authoritative completed history arriving before old mailbox progress.
            select(screen, reads);
            reads.complete.accept(page(false), "");
            drain(screen); drain(screen);
            screen.onActivity(activity -> {
                try {
                    activity.panel.controller.acceptRemotePayload(envelope().put("id", 11), page(true));
                    activity.panel.controller.acceptRemoteCatalog(new JSONArray().put(owner()),
                        new JSONArray(), "android_phone");
                } catch (Exception invalid) { throw new AssertionError(invalid); }
            });
            drain(screen);
            assertCompleted(screen);
            screen.onActivity(activity -> {
                assertTrue("The test Activity must own the foreground screenshot", activity.hasWindowFocus());
                assertTrue(activity.panel.isShown());
            });
            Bitmap screenshot = InstrumentationRegistry.getInstrumentation().getUiAutomation().takeScreenshot();
            assertNotNull(screenshot);
            try (FileOutputStream stream = new FileOutputStream(new File(app.getExternalFilesDir(null),
                    "remote-session-completed-after-old-updates.png"))) {
                assertTrue(screenshot.compress(Bitmap.CompressFormat.PNG, 100, stream));
            } finally { screenshot.recycle(); }
        }
    }

    private static void select(ActivityScenario<AIAssistantTestActivity> screen, Reads reads) throws Exception {
        reads.complete = null;
        screen.onActivity(activity -> {
            try { activity.panel.controller.selectRemote(owner(), accepted -> assertTrue(accepted)); }
            catch (Exception invalid) { throw new AssertionError(invalid); }
        });
        drain(screen);
        assertNotNull("Initial history read was not issued", reads.complete);
    }
    private static void assertCompleted(ActivityScenario<AIAssistantTestActivity> screen) throws Exception {
        await(screen, panel -> !panel.controller.state.busy() && containsText(panel, FINAL_TEXT)
            && containsText(panel, "已完成") && !containsText(panel, "正在思考"));
    }
    private interface Check { boolean ready(AIAssistantPanel panel); }
    private static void await(ActivityScenario<AIAssistantTestActivity> screen, Check check) throws Exception {
        long until = System.currentTimeMillis() + 20000;
        java.util.concurrent.atomic.AtomicBoolean found = new java.util.concurrent.atomic.AtomicBoolean();
        do {
            screen.onActivity(activity -> found.set(check.ready(activity.panel)));
            if (found.get()) return;
            Thread.sleep(50);
        } while (System.currentTimeMillis() < until);
        fail("The actual remote Session panel did not reach its expected completed state");
    }
    private static void drain(ActivityScenario<AIAssistantTestActivity> screen) throws Exception {
        CompletableFuture<Void> done = new CompletableFuture<>();
        screen.onActivity(activity -> {
            try {
                Field worker = AIAssistantController.class.getDeclaredField("worker");
                worker.setAccessible(true);
                ((Handler) worker.get(activity.panel.controller)).post(() -> done.complete(null));
            } catch (Exception invalid) { throw new AssertionError(invalid); }
        });
        done.get(5, TimeUnit.SECONDS);
        InstrumentationRegistry.getInstrumentation().waitForIdleSync();
    }
    private static boolean containsText(View view, String value) {
        if (!view.isShown()) return false;
        if (view instanceof TextView && view.getGlobalVisibleRect(new android.graphics.Rect())
            && ((TextView) view).getText().toString().contains(value)) return true;
        if (view instanceof ViewGroup) for (int i = 0; i < ((ViewGroup) view).getChildCount(); i++)
            if (containsText(((ViewGroup) view).getChildAt(i), value)) return true;
        return false;
    }
    private static JSONObject owner() throws Exception {
        return new JSONObject().put("id", "owned-remote-fixture").put("device_slot", "windows")
            .put("login_session_id", "owned-fixture-login").put("online", true)
            .put("busy", true).put("turnId", "msg_002").put("title", "远端同步验证");
    }
    private static JSONObject envelope() throws Exception {
        return new JSONObject().put("session_id", "owned-remote-fixture").put("from_slot", "windows")
            .put("from_login_session_id", "owned-fixture-login");
    }
    private static JSONObject page(boolean running) throws Exception {
        JSONArray parts = new JSONArray().put(new JSONObject().put("id", "owned-tool")
            .put("kind", "tool").put("tool", "file_read").put("state", running ? "running" : "completed"));
        if (!running) parts.put(new JSONObject().put("id", "owned-final").put("kind", "text").put("text", FINAL_TEXT));
        return new JSONObject().put("ok", true).put("busy", running).put("turnId", running ? "msg_002" : "")
            .put("messages", new JSONArray().put(new JSONObject().put("id", "msg_002").put("role", "assistant")
                .put("created", 1000).put("completed", running ? 0 : 2000).put("active", running).put("parts", parts)))
            .put("permissions", new JSONArray()).put("questions", new JSONArray()).put("questionMessages", new JSONArray());
    }
    private static final class Reads implements AIAssistantController.SessionReads {
        volatile BiConsumer<JSONObject, String> complete;
        @Override public void history(JSONObject owner, String before, int limit, BiConsumer<JSONObject, String> callback) {
            complete = callback;
        }
        @Override public String command(JSONObject owner, JSONObject request,
                BiConsumer<String, String> sent, BiConsumer<JSONObject, JSONObject> received) {
            sent.accept("owned-readonly-metadata", "");
            return "owned-readonly-metadata";
        }
    }
}
