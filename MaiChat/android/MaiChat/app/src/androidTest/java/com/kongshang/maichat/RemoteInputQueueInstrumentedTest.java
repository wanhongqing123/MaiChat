// Normal MainActivity regression against the local no-model session-sync fixture.
package com.kongshang.maichat;

import static org.junit.Assert.*;
import android.content.Context;
import android.graphics.Bitmap;
import android.view.View;
import android.view.ViewGroup;
import android.widget.EditText;
import android.widget.TextView;
import androidx.test.core.app.ActivityScenario;
import androidx.test.core.app.ApplicationProvider;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.lang.reflect.Field;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.HashSet;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BooleanSupplier;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class RemoteInputQueueInstrumentedTest {
    @Test public void normalAppForwardsThreeInputsBeforeBusyOwnerReceipts() throws Exception {
        assertTrue(BuildConfig.DEBUG && BuildConfig.USE_LOCAL_UI_AUTH &&
            BuildConfig.APPLICATION_ID.endsWith(".isolated"));
        Context context = ApplicationProvider.getApplicationContext();
        byte[] certificate = request("/test/cert");
        request("/test/mac-input-control?hold=1&busy=1");
        MaiChatAuthClient.shared(context).loginWithPassword("sync-ui-test", "local sync UI test passphrase", true);
        AtomicReference<AIAssistantController> owner = new AtomicReference<>();
        AtomicReference<AIAssistantPanel> panel = new AtomicReference<>();
        File roots = null;
        byte[] originalRoots = null;
        try (ActivityScenario<MainActivity> screen = ActivityScenario.launch(MainActivity.class)) {
            screen.onActivity(activity -> {
                View tab = description(activity.getWindow().getDecorView(), "创作中心");
                assertNotNull(tab); tab.performClick();
                owner.set(AIAssistantController.shared(activity));
                panel.set(findPanel(activity.getWindow().getDecorView()));
                assertNotNull(panel.get());
            });
            waitFor(() -> owner.get().state.ready, "Agent configuration never became ready");
            // Trust the fixture only in this isolated test account's private root.
            // Production TLS validation and the emulator system store stay intact.
            roots = new File(owner.get().storageRootsForHost().workspace.getParentFile(), "trusted-roots.pem");
            originalRoots = Files.readAllBytes(roots.toPath());
            try (FileOutputStream file = new FileOutputStream(roots)) { file.write(certificate); }
            AtomicReference<JSONObject> remote = new AtomicReference<>();
            waitFor(() -> {
                JSONArray sessions = owner.get().state.data.optJSONArray("sessions");
                if (sessions != null) for (int i = 0; i < sessions.length(); i++) {
                    JSONObject value = sessions.optJSONObject(i);
                    if (value != null && value.optString("id").equals("ses_mock_mac") && value.optBoolean("online")) {
                        remote.set(value); return true;
                    }
                }
                return false;
            }, "The normal app did not discover the local Mac session");
            screen.onActivity(activity -> description(activity.getWindow().getDecorView(), "对话列表").performClick());
            Thread.sleep(300);
            screen.onActivity(activity -> {
                try {
                    Field list = AIAssistantPanel.class.getDeclaredField("sessionDrawerList");
                    list.setAccessible(true);
                    View row = text((View) list.get(panel.get()), remote.get().optString("title"));
                    assertNotNull(row);
                    while (!row.isClickable() && row.getParent() instanceof View) row = (View) row.getParent();
                    assertTrue(row.performClick());
                } catch (ReflectiveOperationException failure) { throw new AssertionError(failure); }
            });
            waitFor(() -> owner.get().state.remoteSession != null &&
                owner.get().state.remoteSession.optString("id").equals("ses_mock_mac"), "Remote session was not selected");
            String prefix = "android-queue-" + java.util.UUID.randomUUID().toString().substring(0, 6);
            String[] texts = {prefix + "-A", prefix + "-B", prefix + "-A"};
            for (String value : texts) {
                screen.onActivity(activity -> {
                    EditText composer = (EditText) description(panel.get(), "创作中心消息输入框");
                    assertNotNull(composer); composer.setText(value);
                    View send = description(panel.get(), "发送给 AI");
                    assertNotNull(send); assertTrue(send.isEnabled()); send.performClick();
                });
                Thread.sleep(150);
            }
            AtomicReference<JSONArray> received = new AtomicReference<>();
            waitFor(() -> {
                try {
                    JSONArray all = new JSONObject(new String(request("/test/status"), StandardCharsets.UTF_8))
                        .getJSONArray("receivedInputs");
                    JSONArray matches = new JSONArray();
                    for (int i = 0; i < all.length(); i++) if (all.getJSONObject(i).optString("text").startsWith(prefix))
                        matches.put(all.getJSONObject(i));
                    received.set(matches); return matches.length() >= 3;
                } catch (Exception failure) { throw new AssertionError(failure); }
            }, "Later inputs waited for a busy owner's receipt");
            assertEquals(3, received.get().length());
            HashSet<String> ids = new HashSet<>();
            for (int i = 0; i < 3; i++) {
                JSONObject input = received.get().getJSONObject(i);
                assertEquals(texts[i], input.getString("text"));
                assertFalse(input.getBoolean("has_turn_id")); ids.add(input.getString("request_id"));
            }
            assertEquals(3, ids.size());
            screenshot(context, "remote-queue-before-receipts.png");
            request("/test/mac-input-control?hold=0&busy=0");
            waitFor(() -> owner.get().state.data.optJSONArray("outgoing") == null ||
                owner.get().state.data.optJSONArray("outgoing").length() == 0, "Owner receipts did not settle the inputs");
            java.util.concurrent.atomic.AtomicBoolean rendered = new java.util.concurrent.atomic.AtomicBoolean();
            waitFor(() -> {
                // 历史节点可能已挂载，而旧 outbox 仍在屏幕底部；截图必须等待二者一起收敛。
                screen.onActivity(activity -> rendered.set(countText(panel.get(), texts[0]) == 2 &&
                    countText(panel.get(), texts[1]) == 1 &&
                    !hasTextPrefix(panel.get(), "已发出，等待所属设备接收")));
                return rendered.get();
            }, "All three accepted messages must be visible as formal history");
            screenshot(context, "remote-queue-owner-confirmed.png");
        } finally {
            request("/test/mac-input-control?hold=0&busy=0");
            if (roots != null && originalRoots != null)
                try (FileOutputStream file = new FileOutputStream(roots)) { file.write(originalRoots); }
        }
    }
    private static void waitFor(BooleanSupplier predicate, String message) throws Exception {
        long deadline = System.currentTimeMillis() + 30_000;
        while (!predicate.getAsBoolean() && System.currentTimeMillis() < deadline) Thread.sleep(150);
        assertTrue(message, predicate.getAsBoolean());
    }
    private static View description(View root, String description) {
        if (root == null) return null;
        if (root.getContentDescription() != null && description.contentEquals(root.getContentDescription())) return root;
        if (root instanceof ViewGroup) for (int i = 0; i < ((ViewGroup) root).getChildCount(); i++) {
            View child = description(((ViewGroup) root).getChildAt(i), description); if (child != null) return child;
        }
        return null;
    }
    private static View text(View root, String text) {
        if (root instanceof TextView && text.contentEquals(((TextView) root).getText())) return root;
        if (root instanceof ViewGroup) for (int i = 0; i < ((ViewGroup) root).getChildCount(); i++) {
            View child = text(((ViewGroup) root).getChildAt(i), text); if (child != null) return child;
        }
        return null;
    }
    private static boolean hasTextPrefix(View root, String prefix) {
        if (root instanceof TextView && ((TextView) root).getText().toString().startsWith(prefix)) return true;
        if (root instanceof ViewGroup) for (int i = 0; i < ((ViewGroup) root).getChildCount(); i++)
            if (hasTextPrefix(((ViewGroup) root).getChildAt(i), prefix)) return true;
        return false;
    }
    private static int countText(View root, String value) {
        int count = root instanceof TextView && value.contentEquals(((TextView) root).getText()) ? 1 : 0;
        if (root instanceof ViewGroup) for (int i = 0; i < ((ViewGroup) root).getChildCount(); i++)
            count += countText(((ViewGroup) root).getChildAt(i), value);
        return count;
    }
    private static AIAssistantPanel findPanel(View root) {
        if (root instanceof AIAssistantPanel) return (AIAssistantPanel) root;
        if (root instanceof ViewGroup) for (int i = 0; i < ((ViewGroup) root).getChildCount(); i++) {
            AIAssistantPanel child = findPanel(((ViewGroup) root).getChildAt(i)); if (child != null) return child;
        }
        return null;
    }
    private static byte[] request(String path) throws Exception {
        HttpURLConnection connection = (HttpURLConnection) new URL("http://10.0.2.2:18190" + path).openConnection();
        connection.setConnectTimeout(5000); connection.setReadTimeout(5000);
        try (InputStream input = connection.getInputStream(); ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] bytes = new byte[4096]; int count;
            while ((count = input.read(bytes)) != -1) output.write(bytes, 0, count);
            return output.toByteArray();
        } finally { connection.disconnect(); }
    }
    private static void screenshot(Context context, String name) throws Exception {
        Bitmap image = InstrumentationRegistry.getInstrumentation().getUiAutomation().takeScreenshot();
        assertNotNull(image);
        try (FileOutputStream output = new FileOutputStream(new File(context.getExternalFilesDir(null), name))) {
            assertTrue(image.compress(Bitmap.CompressFormat.PNG, 100, output));
        } finally { image.recycle(); }
    }
}
