// 文件作用：在已安装 App 的真实面板、Controller 与 JNI 上验证本机 Responses 工具和技能路径。
// 仅测试代码提供隔离 Context 和回环模型；不改生产配置，不访问账号数据或发送 IM 消息。
package com.kongshang.maichat;

import static org.junit.Assert.*;

import android.app.Activity;
import android.app.Application;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ApplicationInfo;
import android.graphics.Bitmap;
import android.os.Bundle;
import android.os.SystemClock;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.TextView;
import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.reflect.Field;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class AIAssistantPcParityInstrumentedTest {
    @Test public void realComposerApprovesNativeShellAndPatchWithDurableUsage() throws Exception {
        try (OwnedScope owned = new OwnedScope(); ModelServer model = new ModelServer()) {
            try (ActivityScenario<AIAssistantTestActivity> scenario = launch(owned, model)) {
                ready(scenario, owned);
                create(scenario);
                File workspace = workspace(scenario, owned);
                model.command = "mkdir -p 'owned dir'; echo 'owned parity' > 'owned dir/source.txt'; "
                    + "cp 'owned dir/source.txt' 'owned dir/copy.txt' && mv 'owned dir/copy.txt' 'owned dir/moved.txt'; "
                    + "cat 'owned dir/moved.txt' | grep -F parity; find 'owned dir' -name '*.txt'; "
                    + "read-text 'owned dir/moved.txt'; touch obsolete.txt; rm obsolete.txt";
                send(scenario, "OWNED_SHELL");
                await(scenario, panel -> permissions(panel).length() == 1);
                assertFalse(new File(workspace, "owned dir/moved.txt").exists());
                approve(scenario, "exec_command");
                await(scenario, panel -> !panel.controller.state.busy() && text(panel, "OWNED_SHELL_DONE"));
                model.check();
                assertEquals("owned parity\n", read(new File(workspace, "owned dir/moved.txt")));
                assertFalse(new File(workspace, "owned dir/copy.txt").exists());
                assertFalse(new File(workspace, "obsolete.txt").exists());
                scenario.onActivity(activity -> {
                    JSONObject part = toolPart(activity.panel, "exec_command");
                    assertNotNull(part);
                    assertEquals("completed", part.optString("state"));
                    assertTrue(part.optString("input").contains("mkdir"));
                    assertTrue(part.optString("output").contains("in_process"));
                    assertNotNull(findContaining(activity.panel, "exec_command"));
                });
                capture(scenario, owned, "shell-approved.png");
                send(scenario, "OWNED_PATCH");
                await(scenario, panel -> permissions(panel).length() == 1);
                approve(scenario, "apply_patch");
                await(scenario, panel -> !panel.controller.state.busy() && text(panel, "OWNED_PATCH_DONE"));
                assertEquals("owned patch\n", read(new File(workspace, "owned-patch.txt")));
                model.check();
                assertEquals(4, model.requests.size());
                assertSchemaContract(model.requests.get(0));
                assertFunctionReceipt(model.requests.get(1), "owned_shell", "in_process");
                assertFunctionReceipt(model.requests.get(3), "owned_patch", "owned-patch.txt");
                assertJournalFacts(owned, "owned parity", "owned_shell", model.requests.size());
                assertUsageUi(scenario, owned, 266, 532);
                capture(scenario, owned, "patch-completed.png");
                owned.evidence("completed-shell-and-patch", new JSONObject().put("requests", model.requests.size())
                    .put("usageUiVerified", true).put("usageBoundary", "loopback Responses usage reaches authoritative JSONL, native snapshot and real panel; no paid provider request"));
            }
        }
    }

    @Test public void installedWorkspaceSkillRefreshesNextInputAndSessionsRestoreRawDialogue() throws Exception {
        try (OwnedScope owned = new OwnedScope(); ModelServer model = new ModelServer()) {
            try (ActivityScenario<AIAssistantTestActivity> scenario = launch(owned, model)) {
                ready(scenario, owned);
                create(scenario);
                String first = selected(scenario);
                send(scenario, "OWNED_BEFORE_SKILL");
                await(scenario, panel -> !panel.controller.state.busy() && text(panel, "OWNED_BEFORE_DONE"));
                File workspace = workspace(scenario, owned);
                String skill = "owned-pc-" + UUID.randomUUID().toString().replace("-", "");
                String description = "Owned parity skill metadata " + skill;
                String body = "---\nname: " + skill + "\ndescription: " + description + "\n---\n"
                    + "# Owned parity instructions\nBODY_" + skill + "\nRead this local fixture only.\n";
                File folder = new File(workspace, ".agents/skills/" + skill);
                assertTrue(folder.mkdirs());
                write(new File(folder, "SKILL.md"), body);
                model.skill = skill;
                model.skillBody = body;
                send(scenario, "OWNED_SKILL $" + skill);
                await(scenario, panel -> !panel.controller.state.busy() && text(panel, "OWNED_SKILL_DONE"));
                model.check();
                assertEquals(3, model.requests.size());
                String instructions = model.requests.get(0).optString("instructions");
                assertFalse(instructions.contains(description));
                assertEquals(instructions, model.requests.get(1).optString("instructions"));
                assertEquals(instructions, model.requests.get(2).optString("instructions"));
                JSONObject skillInput = selectedSkillContext(lastUser(model.requests.get(1)));
                assertEquals("untrusted_user_context", skillInput.getString("trust"));
                assertEquals(body, skillInput.getJSONArray("skills").getJSONObject(0).getString("contents"));
                assertTrue(lastUser(model.requests.get(1)).contains(description));
                assertEquals(lastUser(model.requests.get(1)), lastUser(model.requests.get(2)));
                assertFunctionReceipt(model.requests.get(2), "owned_skill", body);
                send(scenario, "OWNED_AFTER_SKILL");
                await(scenario, panel -> !panel.controller.state.busy() && text(panel, "OWNED_AFTER_DONE"));
                assertEquals(4, model.requests.size());
                assertEquals("OWNED_AFTER_SKILL", lastUser(model.requests.get(3)));
                final String[] beforeRaw = new String[1];
                scenario.onActivity(activity -> beforeRaw[0] = toolPart(activity.panel, "skill_read").toString());
                create(scenario);
                String second = selected(scenario);
                assertNotEquals(first, second);
                scenario.onActivity(activity -> {
                    assertFalse(activity.panel.controller.state.contextUsage.text.contains("133 used"));
                    assertEquals(-1, activity.panel.controller.state.contextUsage.usedPercent);
                });
                send(scenario, "OWNED_SECOND_SESSION");
                await(scenario, panel -> !panel.controller.state.busy() && text(panel, "OWNED_SECOND_DONE"));
                scenario.onActivity(activity -> activity.panel.controller.select(first));
                await(scenario, panel -> first.equals(panel.controller.state.selected) && text(panel, "OWNED_SKILL_DONE"));
                scenario.onActivity(activity -> {
                    assertTrue(text(activity.panel, "OWNED_BEFORE_DONE"));
                    assertTrue(text(activity.panel, "OWNED_AFTER_DONE"));
                    assertFalse(text(activity.panel, "OWNED_SECOND_DONE"));
                    assertEquals(beforeRaw[0], toolPart(activity.panel, "skill_read").toString());
                });
                scenario.onActivity(activity -> activity.panel.controller.select(second));
                await(scenario, panel -> second.equals(panel.controller.state.selected) && text(panel, "OWNED_SECOND_DONE"));
                model.check();
                assertEquals(5, model.requests.size());
                assertJournalFacts(owned, "BODY_" + skill, "owned_skill", model.requests.size());
                assertUsageUi(scenario, owned, 133, 133);
                capture(scenario, owned, "second-session-restored.png");
                owned.evidence("completed-skills-and-session-restore", new JSONObject().put("requests", model.requests.size())
                    .put("usageUiVerified", true).put("usageBoundary", "native selected-session usage and visible panel restored; no paid provider request"));
            }
        }
    }

    /** 单独验证已知窗口快照的真实控件；这是明示的展示 fixture，不声称来自回环型号目录或供应商。 */
    @Test public void explicitKnownWindowPresentationSnapshotRendersPercentWithoutProviderRequests() throws Exception {
        try (OwnedScope owned = new OwnedScope(); ModelServer model = new ModelServer()) {
            try (ActivityScenario<AIAssistantTestActivity> scenario = launch(owned, model)) {
                ready(scenario, owned);
                create(scenario);
                scenario.onActivity(activity -> {
                    try {
                        AIAssistantController.State previous = activity.panel.controller.state;
                        JSONObject data = new JSONObject(previous.data.toString());
                        JSONObject latest = new JSONObject().put("input_tokens", 39000).put("input_tokens_source", "provider")
                            .put("cached_input_tokens", 35000).put("cached_input_tokens_source", "provider")
                            .put("output_tokens", 1000).put("output_tokens_source", "provider")
                            .put("total_tokens", 40000).put("total_tokens_source", "provider");
                        // 复用 AgentContextUsagePresentationTest 经 Core helper 验证的 40,000/100,000→68% 样例。
                        // 此合成窗口仅测试 UI 接线，不是任何型号的容量默认值，也不是本测试的供应商实测。
                        JSONObject usage = new JSONObject().put("version", 1).put("session_id", previous.selected)
                            .put("model", previous.model).put("sampling_valid", true).put("latest_sampling", latest)
                            .put("window_tokens", 100000).put("window_source", "configuration").put("remaining_percent", 68)
                            .put("turn_token_usage", new JSONObject().put("total_tokens", 80000).put("total_tokens_source", "provider"))
                            .put("thread_token_usage", new JSONObject().put("total_tokens", 3000000).put("total_tokens_source", "provider"));
                        data.put("contextUsage", usage);
                        activity.panel.onState(new AIAssistantController.State(data, null, previous.selected,
                            previous.baseUrl, previous.model, previous.policy, "", "", "", 0, false, true));
                        TextView label = activity.panel.findViewWithTag("ai-context-usage");
                        assertEquals("68% left (40,000 used / 100,000)", label.getText().toString());
                        android.widget.ProgressBar bar = activity.panel.findViewWithTag("ai-context-usage-bar");
                        assertEquals(View.VISIBLE, bar.getVisibility());
                        assertEquals(32, bar.getProgress());
                        assertTrue(label.getGlobalVisibleRect(new android.graphics.Rect()));
                    } catch (Exception error) { throw new AssertionError(error); }
                });
                capture(scenario, owned, "explicit-known-window-presentation.png");
                assertEquals(0, model.requests.size());
                owned.evidence("explicit-known-window-presentation", new JSONObject()
                    .put("source", "synthetic UI snapshot only; no known-window JNI/provider assertion")
                    .put("requests", model.requests.size()));
            }
        }
    }

    /** 数字取独立 SSE fixture，窗口遵守实际 endpoint/型号配置；未知窗口绝不能伪造百分比。 */
    private static void assertUsageUi(ActivityScenario<AIAssistantTestActivity> scenario, OwnedScope owned, int turnTotal, int threadTotal) {
        AtomicReference<android.app.AlertDialog> shownDialog = new AtomicReference<>();
        scenario.onActivity(activity -> {
            try {
                AIAssistantController.State state = activity.panel.controller.state;
                JSONObject usage = state.data.getJSONObject("contextUsage");
                assertEquals(state.selected, usage.getString("session_id"));
                assertTrue(usage.getBoolean("sampling_valid"));
                JSONObject latest = usage.getJSONObject("latest_sampling");
                assertEquals(111, latest.getInt("input_tokens"));
                assertEquals(22, latest.getInt("output_tokens"));
                assertEquals(133, latest.getInt("total_tokens"));
                assertEquals("provider", latest.getString("total_tokens_source"));
                assertEquals("missing", latest.getString("cached_input_tokens_source"));
                assertEquals(turnTotal, usage.getJSONObject("turn_token_usage").getInt("total_tokens"));
                assertEquals(threadTotal, usage.getJSONObject("thread_token_usage").getInt("total_tokens"));
                // 首条事实保留 provider；多个请求相加由 Core add 标为 derived，UI 必须保留“推算”标记。
                assertEquals(turnTotal == 133 ? "provider" : "derived",
                    usage.getJSONObject("turn_token_usage").getString("total_tokens_source"));
                assertEquals(threadTotal == 133 ? "provider" : "derived",
                    usage.getJSONObject("thread_token_usage").getString("total_tokens_source"));
                String turnDisplay = (turnTotal == 133 ? "" : "推算 ") + turnTotal;
                String threadDisplay = (threadTotal == 133 ? "" : "推算 ") + threadTotal;
                TextView label = activity.panel.findViewWithTag("ai-context-usage");
                assertNotNull(label);
                assertEquals(View.VISIBLE, label.getVisibility());
                assertTrue(label.getText().toString().contains("133 used / "));
                if (threadTotal != 133) assertFalse(label.getText().toString().contains(threadTotal + " used"));
                assertTrue(state.contextUsage.detail.contains("输入：111"));
                assertTrue(state.contextUsage.detail.contains("输出：22"));
                assertTrue(state.contextUsage.detail.contains("本轮累计：" + turnDisplay));
                assertTrue(state.contextUsage.detail.contains("会话累计（已记录部分）：" + threadDisplay));
                android.widget.ProgressBar bar = activity.panel.findViewWithTag("ai-context-usage-bar");
                if (usage.getLong("window_tokens") == 0) {
                    // test-model 没有目录窗口；真实计数仍可见，不凭别家型号伪造窗口或百分比。
                    assertEquals("missing", usage.getString("window_source"));
                    assertTrue(usage.isNull("remaining_percent"));
                    assertEquals("133 used / 窗口未知", label.getText().toString());
                    assertEquals(View.GONE, bar.getVisibility());
                    assertEquals(-1, state.contextUsage.usedPercent);
                } else {
                    assertEquals("configuration", usage.getString("window_source"));
                    assertTrue(label.getText().toString().contains("% left"));
                    assertEquals(View.VISIBLE, bar.getVisibility());
                    assertEquals(100 - usage.getInt("remaining_percent"), bar.getProgress());
                }
                assertTrue(label.getGlobalVisibleRect(new android.graphics.Rect()));
                assertTrue(label.performClick());
                Field dialogField = AIAssistantPanel.class.getDeclaredField("contextUsageDialog");
                dialogField.setAccessible(true);
                android.app.AlertDialog dialog = (android.app.AlertDialog) dialogField.get(activity.panel);
                assertNotNull(dialog);
                assertTrue(dialog.isShowing());
                TextView message = dialog.findViewById(android.R.id.message);
                assertTrue(message.getText().toString().contains("输入：111"));
                assertTrue(message.getText().toString().contains("输出：22"));
                assertTrue(message.getText().toString().contains("缓存命中：未提供"));
                assertTrue(message.getText().toString().contains("本轮累计：" + turnDisplay));
                assertTrue(message.getText().toString().contains("会话累计（已记录部分）：" + threadDisplay));
                shownDialog.set(dialog);
            } catch (Exception error) { throw new AssertionError(error); }
        });
        // 对真实窗口截屏（包括 AlertDialog），补充原有 panel.draw 内容图；主代理验收时检查实际像素。
        InstrumentationRegistry.getInstrumentation().waitForIdleSync();
        Bitmap screenshot = InstrumentationRegistry.getInstrumentation().getUiAutomation().takeScreenshot();
        assertNotNull(screenshot);
        try (FileOutputStream output = new FileOutputStream(new File(owned.root, "context-usage-details.png"))) {
            assertTrue(screenshot.compress(Bitmap.CompressFormat.PNG, 100, output));
        } catch (Exception error) { throw new AssertionError(error); }
        finally {
            screenshot.recycle();
            scenario.onActivity(activity -> shownDialog.get().dismiss());
        }
    }

    private static ActivityScenario<AIAssistantTestActivity> launch(OwnedScope owned, ModelServer model) {
        return ActivityScenario.launch(new Intent(owned.original, AIAssistantTestActivity.class)
            .putExtra("model_url", model.endpoint()));
    }
    private static void ready(ActivityScenario<AIAssistantTestActivity> scenario, OwnedScope owned) throws Exception {
        await(scenario, panel -> panel.controller.state.ready && panel.controller.modelSettingsReady());
        scenario.onActivity(activity -> {
            try {
                assertTrue(activity.getBaseContext() instanceof OwnedContext);
                assertEquals(owned.root.getCanonicalPath(), activity.getApplicationInfo().dataDir);
                assertEquals(owned.root.getCanonicalFile(), activity.getFilesDir().getParentFile().getCanonicalFile());
                File databaseRoot = controllerRoot(activity.panel.controller);
                assertEquals(new File(owned.root, "no_backup").getCanonicalFile(), databaseRoot.getParentFile().getCanonicalFile());
                assertTrue(databaseRoot.getName().startsWith("AIAssistantTest-"));
                assertEquals(0, activity.panel.controller.state.data.getJSONArray("sessions").length());
            } catch (Exception error) { throw new AssertionError(error); }
        });
    }
    private static File controllerRoot(AIAssistantController controller) throws Exception {
        Field field = AIAssistantController.class.getDeclaredField("root");
        field.setAccessible(true);
        return (File) field.get(controller);
    }
    private static File workspace(ActivityScenario<AIAssistantTestActivity> scenario, OwnedScope owned) {
        final File[] result = new File[1];
        scenario.onActivity(activity -> {
            try {
                result[0] = new File(controllerRoot(activity.panel.controller), "Workspace").getCanonicalFile();
                assertTrue(result[0].getPath().startsWith(owned.root.getCanonicalPath() + File.separator));
            } catch (Exception error) { throw new AssertionError(error); }
        });
        return result[0];
    }
    private static void create(ActivityScenario<AIAssistantTestActivity> scenario) throws Exception {
        String previous = selected(scenario);
        scenario.onActivity(activity -> activity.panel.controller.create());
        await(scenario, panel -> !panel.controller.state.selected.isEmpty() && !previous.equals(panel.controller.state.selected));
    }
    private static String selected(ActivityScenario<AIAssistantTestActivity> scenario) {
        final String[] value = new String[1];
        scenario.onActivity(activity -> value[0] = activity.panel.controller.state.selected);
        return value[0];
    }
    private static void send(ActivityScenario<AIAssistantTestActivity> scenario, String value) {
        scenario.onActivity(activity -> {
            activity.panel.composer.setText(value);
            assertTrue(activity.panel.send.isEnabled());
            assertTrue(activity.panel.send.performClick());
        });
    }
    private static void approve(ActivityScenario<AIAssistantTestActivity> scenario, String tool) {
        scenario.onActivity(activity -> {
            assertEquals(tool, permissions(activity.panel).optJSONObject(0).optString("tool"));
            Button button = exactButton(activity.panel, "允许一次");
            assertNotNull(button);
            assertTrue(button.isEnabled());
            assertTrue(button.performClick());
        });
    }
    private interface Check { boolean test(AIAssistantPanel panel) throws Exception; }
    private static void await(ActivityScenario<AIAssistantTestActivity> scenario, Check check) throws Exception {
        AtomicBoolean success = new AtomicBoolean();
        long deadline = SystemClock.uptimeMillis() + 30000;
        while (!success.get() && SystemClock.uptimeMillis() < deadline) {
            scenario.onActivity(activity -> {
                try { success.set(check.test(activity.panel)); }
                catch (Exception error) { throw new AssertionError(error); }
            });
            if (!success.get()) SystemClock.sleep(40);
        }
        assertTrue("Owned UI/model fixture did not reach expected state", success.get());
    }
    private static JSONArray permissions(AIAssistantPanel panel) {
        JSONArray value = panel.controller.state.data.optJSONArray("permissions");
        return value == null ? new JSONArray() : value;
    }
    private static boolean text(View view, String value) { return findContaining(view, value) != null; }
    private static TextView findContaining(View view, String value) {
        if (view instanceof TextView && ((TextView) view).getText().toString().contains(value)) return (TextView) view;
        if (view instanceof ViewGroup) for (int index = 0; index < ((ViewGroup) view).getChildCount(); ++index) {
            TextView result = findContaining(((ViewGroup) view).getChildAt(index), value);
            if (result != null) return result;
        }
        return null;
    }
    private static Button exactButton(View view, String value) {
        if (view instanceof Button && value.contentEquals(((Button) view).getText())) return (Button) view;
        if (view instanceof ViewGroup) for (int index = 0; index < ((ViewGroup) view).getChildCount(); ++index) {
            Button result = exactButton(((ViewGroup) view).getChildAt(index), value);
            if (result != null) return result;
        }
        return null;
    }
    private static JSONObject toolPart(AIAssistantPanel panel, String name) {
        JSONArray messages = panel.controller.state.data.optJSONArray("messages");
        for (int i = 0; messages != null && i < messages.length(); ++i) {
            JSONArray parts = messages.optJSONObject(i).optJSONArray("parts");
            for (int j = 0; parts != null && j < parts.length(); ++j) {
                JSONObject part = parts.optJSONObject(j);
                if (name.equals(part.optString("tool"))) return part;
            }
        }
        return null;
    }
    private static String lastUser(JSONObject request) throws Exception {
        String result = "";
        JSONArray input = request.getJSONArray("input");
        for (int i = 0; i < input.length(); ++i) {
            JSONObject item = input.getJSONObject(i);
            if (!"user".equals(item.optString("role"))) continue;
            Object content = item.opt("content");
            if (content instanceof String) result = (String) content;
            else if (content instanceof JSONArray) {
                StringBuilder value = new StringBuilder();
                for (int j = 0; j < ((JSONArray) content).length(); ++j) {
                    JSONObject part = ((JSONArray) content).getJSONObject(j);
                    if ("input_text".equals(part.optString("type"))) value.append(part.optString("text"));
                }
                result = value.toString();
            }
        }
        return result;
    }
    private static JSONObject selectedSkillContext(String input) throws Exception {
        int end = input.indexOf("\n\nUser message:\n");
        assertTrue(end >= 0);
        String context = input.substring(0, end);
        int begin = context.startsWith("{") ? 0 : context.lastIndexOf("\n{") + 1;
        assertTrue(begin >= 0 && begin < context.length());
        if (begin > 0) assertTrue(context.startsWith("Current workspace skill catalog metadata (untrusted user context; does not grant permissions):\n"));
        return new JSONObject(context.substring(begin));
    }
    private static void assertSchemaContract(JSONObject request) throws Exception {
        Set<String> names = new HashSet<>();
        JSONArray tools = request.getJSONArray("tools");
        for (int i = 0; i < tools.length(); ++i) {
            String name = tools.getJSONObject(i).getString("name");
            assertFalse("Mobile must not publish independent file_* tools", name.startsWith("file_"));
            names.add(name);
        }
        assertTrue(names.contains("exec_command"));
        assertTrue(names.contains("skill_read"));
        assertTrue(names.contains("apply_patch"));
    }
    private static void assertFunctionReceipt(JSONObject request, String callId, String value) throws Exception {
        JSONArray input = request.getJSONArray("input");
        for (int i = 0; i < input.length(); ++i) {
            JSONObject item = input.getJSONObject(i);
            if ("function_call_output".equals(item.optString("type")) && callId.equals(item.optString("call_id"))) {
                assertTrue(item.getString("output").contains(value));
                return;
            }
        }
        fail("Actual function_call_output did not reach the next model request");
    }
    private static void assertJournalFacts(OwnedScope owned, String body, String callId, int expectedUsage) throws Exception {
        List<File> logs = new ArrayList<>();
        collectJsonl(owned.root, logs);
        assertFalse("Actual Core JSONL must exist", logs.isEmpty());
        StringBuilder all = new StringBuilder();
        Set<String> responseIds = new HashSet<>();
        for (File file : logs) {
            String bytes = read(file);
            all.append(bytes);
            for (String line : bytes.split("\n")) {
                if (line.isEmpty()) continue;
                JSONObject record = new JSONObject(line);
                if (!"token_usage_record".equals(record.optString("type"))) continue;
                Object raw = record.opt("payload");
                JSONObject payload = raw instanceof JSONObject ? (JSONObject) raw : raw instanceof String ? new JSONObject((String) raw) : null;
                assertNotNull(payload);
                JSONObject usage = payload.getJSONObject("usage");
                assertEquals(111, usage.getLong("input_tokens"));
                assertEquals(22, usage.getLong("output_tokens"));
                assertEquals(133, usage.getLong("total_tokens"));
                responseIds.add(payload.getString("response_id"));
            }
        }
        assertTrue(all.toString().contains(body));
        assertTrue(all.toString().contains(callId));
        assertEquals(expectedUsage, responseIds.size());
    }
    private static void collectJsonl(File folder, List<File> result) throws Exception {
        File[] files = folder.listFiles();
        if (files == null) return;
        for (File file : files) {
            assertTrue(file.getCanonicalPath().startsWith(folder.getCanonicalPath() + File.separator));
            if (file.isDirectory()) collectJsonl(file, result);
            else if (file.getName().endsWith(".jsonl")) result.add(file);
        }
    }
    private static String read(File file) throws Exception { return new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8); }
    private static void write(File file, String value) throws Exception {
        try (FileOutputStream output = new FileOutputStream(file)) { output.write(value.getBytes(StandardCharsets.UTF_8)); }
    }
    private static void capture(ActivityScenario<AIAssistantTestActivity> scenario, OwnedScope owned, String name) throws Exception {
        InstrumentationRegistry.getInstrumentation().waitForIdleSync();
        SystemClock.sleep(150);
        scenario.onActivity(activity -> {
            Bitmap bitmap = Bitmap.createBitmap(activity.panel.getWidth(), activity.panel.getHeight(), Bitmap.Config.ARGB_8888);
            activity.panel.draw(new android.graphics.Canvas(bitmap));
            try (FileOutputStream output = new FileOutputStream(new File(owned.root, name))) {
                assertTrue(bitmap.compress(Bitmap.CompressFormat.PNG, 100, output));
            } catch (Exception error) { throw new AssertionError(error); }
            finally { bitmap.recycle(); }
        });
    }

    // 线格式逐项对应 MobileAgentAdapter/tests/MaiResponsesFixture.h：terminal-only
    // response.completed、message/output_text、function_call 和完整 response.usage。
    private static final class ModelServer implements AutoCloseable {
        final ServerSocket server = new ServerSocket(0, 8, InetAddress.getByName("127.0.0.1"));
        final List<JSONObject> requests = new CopyOnWriteArrayList<>();
        final AtomicReference<Throwable> failure = new AtomicReference<>();
        final Thread worker;
        volatile boolean closed;
        volatile String command = "", skill = "", skillBody = "";
        ModelServer() throws Exception {
            worker = new Thread(this::serve, "owned-responses-server");
            worker.start();
        }
        String endpoint() { return "http://127.0.0.1:" + server.getLocalPort(); }
        void check() { if (failure.get() != null) throw new AssertionError(failure.get()); }
        private void serve() {
            while (!closed) try (Socket socket = server.accept()) {
                socket.setSoTimeout(10000);
                InputStream in = socket.getInputStream();
                String requestLine = line(in);
                assertTrue(requestLine.startsWith("POST "));
                Map<String, String> headers = new HashMap<>();
                for (String value; !(value = line(in)).isEmpty();) {
                    int colon = value.indexOf(':');
                    assertTrue(colon > 0);
                    headers.put(value.substring(0, colon).toLowerCase(java.util.Locale.ROOT), value.substring(colon + 1).trim());
                }
                assertTrue("Only synthetic loopback credentials are accepted", "Bearer test-key".equals(headers.get("authorization")));
                int length = Integer.parseInt(headers.get("content-length"));
                assertTrue(length >= 0 && length <= 4 * 1024 * 1024);
                byte[] bytes = new byte[length];
                for (int at = 0; at < length;) {
                    int count = in.read(bytes, at, length - at);
                    if (count < 0) throw new java.io.EOFException();
                    at += count;
                }
                JSONObject request = new JSONObject(new String(bytes, StandardCharsets.UTF_8));
                requests.add(request);
                JSONArray output = response(request);
                JSONObject response = new JSONObject().put("id", "owned_response_" + requests.size()).put("status", "completed")
                    .put("output", output).put("usage", new JSONObject().put("input_tokens", 111).put("output_tokens", 22).put("total_tokens", 133));
                String sse = "data: " + new JSONObject().put("type", "response.completed").put("response", response) + "\n\n";
                byte[] body = sse.getBytes(StandardCharsets.UTF_8);
                OutputStream stream = socket.getOutputStream();
                stream.write(("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\nContent-Length: " + body.length + "\r\n\r\n").getBytes(StandardCharsets.US_ASCII));
                stream.write(body);
                stream.flush();
            } catch (Throwable error) {
                if (!closed) failure.compareAndSet(null, error);
            }
        }
        private JSONArray response(JSONObject request) throws Exception {
            String prompt = lastUser(request);
            if (prompt.contains("OWNED_SHELL")) {
                if (hasReceipt(request, "owned_shell")) return message("OWNED_SHELL_DONE");
                return function("owned_shell", "exec_command", new JSONObject().put("cmd", command));
            }
            if (prompt.contains("OWNED_PATCH")) {
                if (hasReceipt(request, "owned_patch")) return message("OWNED_PATCH_DONE");
                return function("owned_patch", "apply_patch", new JSONObject().put("patch", "*** Begin Patch\n*** Add File: owned-patch.txt\n+owned patch\n*** End Patch"));
            }
            if (prompt.contains("OWNED_BEFORE_SKILL")) return message("OWNED_BEFORE_DONE");
            if (prompt.contains("OWNED_SKILL")) {
                if (hasReceipt(request, "owned_skill")) return message("OWNED_SKILL_DONE");
                return function("owned_skill", "skill_read", new JSONObject().put("skill", skill));
            }
            if (prompt.contains("OWNED_AFTER_SKILL")) return message("OWNED_AFTER_DONE");
            if (prompt.contains("OWNED_SECOND_SESSION")) return message("OWNED_SECOND_DONE");
            throw new AssertionError("Unexpected owned fixture prompt");
        }
        private static boolean hasReceipt(JSONObject request, String id) throws Exception {
            JSONArray input = request.getJSONArray("input");
            for (int i = 0; i < input.length(); ++i)
                if ("function_call_output".equals(input.getJSONObject(i).optString("type")) && id.equals(input.getJSONObject(i).optString("call_id"))) return true;
            return false;
        }
        private static JSONArray function(String id, String name, JSONObject args) throws Exception {
            return new JSONArray().put(new JSONObject().put("type", "function_call").put("id", "fc_" + id)
                .put("call_id", id).put("name", name).put("arguments", args.toString()));
        }
        private static JSONArray message(String text) throws Exception {
            return new JSONArray().put(new JSONObject().put("type", "message").put("role", "assistant")
                .put("content", new JSONArray().put(new JSONObject().put("type", "output_text").put("text", text))));
        }
        private static String line(InputStream input) throws Exception {
            ByteArrayOutputStream value = new ByteArrayOutputStream();
            for (int next; (next = input.read()) >= 0;) {
                if (next == '\n') return value.toString("US-ASCII").replace("\r", "");
                value.write(next);
                if (value.size() > 65536) throw new java.io.IOException("Owned HTTP header exceeds budget");
            }
            throw new java.io.EOFException();
        }
        @Override public void close() throws Exception { closed = true; server.close(); worker.join(12000); assertFalse(worker.isAlive()); check(); }
    }

    private static final class OwnedScope implements AutoCloseable, Application.ActivityLifecycleCallbacks {
        final Context original = InstrumentationRegistry.getInstrumentation().getTargetContext();
        final Application application = (Application) original.getApplicationContext();
        final File root = new File(original.getNoBackupFilesDir(), "AIAssistantPcParity-" + UUID.randomUUID());
        final OwnedContext context;
        final Field authField;
        final Object previousAuth;
        final Object ownedAuth;
        final List<Thread> controllerWorkers = new CopyOnWriteArrayList<>();
        OwnedScope() throws Exception {
            assertNull("Fixture requires an instrumentation process without a live account Agent owner",
                AIAssistantController.initializedAccountToolOwner());
            assertTrue(root.mkdirs());
            context = new OwnedContext(original, root);
            authField = MaiChatAuthClient.class.getDeclaredField("shared");
            authField.setAccessible(true);
            previousAuth = authField.get(null);
            ownedAuth = new MaiChatAuthClient(context);
            authField.set(null, ownedAuth);
            application.registerActivityLifecycleCallbacks(this);
        }
        @Override public void onActivityPreCreated(Activity activity, Bundle state) {
            if (!(activity instanceof AIAssistantTestActivity)) return;
            try {
                Field base = ContextWrapper.class.getDeclaredField("mBase");
                base.setAccessible(true);
                base.set(activity, context);
            } catch (Exception error) { throw new AssertionError("Owned context must be installed before Controller creation", error); }
        }
        @Override public void onActivityPostCreated(Activity activity, Bundle state) {
            if (!(activity instanceof AIAssistantTestActivity)) return;
            try {
                Field thread = AIAssistantController.class.getDeclaredField("thread");
                thread.setAccessible(true);
                controllerWorkers.add((Thread) thread.get(((AIAssistantTestActivity) activity).panel.controller));
            } catch (Exception error) { throw new AssertionError(error); }
        }
        @Override public void onActivityCreated(Activity activity, Bundle state) {}
        void evidence(String phase, JSONObject result) throws Exception {
            write(new File(root, "receipt-" + phase + ".json"), result.put("root", root.getCanonicalPath())
                .put("preferences", "owned in-memory fixture; no delegate preferences accessed")
                .put("processUid", android.os.Process.myUid()).toString(2));
        }
        @Override public void close() throws Exception {
            application.unregisterActivityLifecycleCallbacks(this);
            for (Thread worker : controllerWorkers) {
                worker.join(15000);
                assertFalse("Controller worker must stop before restoring auth singleton; terminate this instrumentation process on failure", worker.isAlive());
            }
            authField.set(null, previousAuth);
        }
        @Override public void onActivityStarted(Activity activity) {}
        @Override public void onActivityResumed(Activity activity) {}
        @Override public void onActivityPaused(Activity activity) {}
        @Override public void onActivityStopped(Activity activity) {}
        @Override public void onActivitySaveInstanceState(Activity activity, Bundle state) {}
        @Override public void onActivityDestroyed(Activity activity) {}
    }

    private static final class OwnedContext extends ContextWrapper {
        final File root;
        final Map<String, SharedPreferences> preferences = new HashMap<>();
        OwnedContext(Context original, File root) { super(original); this.root = root; }
        private File directory(String name) {
            File value = new File(root, name);
            try { if (!value.getCanonicalPath().startsWith(root.getCanonicalPath() + File.separator)) throw new AssertionError("Fixture path escaped"); }
            catch (Exception error) { throw new AssertionError(error); }
            if (!value.isDirectory() && !value.mkdirs()) throw new AssertionError("Cannot create owned directory");
            return value;
        }
        @Override public Context getApplicationContext() { return this; }
        @Override public File getDataDir() { return root; }
        @Override public File getFilesDir() { return directory("files"); }
        @Override public File getNoBackupFilesDir() { return directory("no_backup"); }
        @Override public File getCacheDir() { return directory("cache"); }
        @Override public File getCodeCacheDir() { return directory("code_cache"); }
        @Override public File getDir(String name, int mode) { return directory("app_" + name); }
        @Override public File getExternalFilesDir(String type) { return directory("external_files/" + (type == null ? "owned" : type)); }
        @Override public File[] getExternalFilesDirs(String type) { return new File[]{getExternalFilesDir(type)}; }
        @Override public File getExternalCacheDir() { return directory("external_cache"); }
        @Override public File[] getExternalCacheDirs() { return new File[]{getExternalCacheDir()}; }
        @Override public File getDatabasePath(String name) { return new File(directory("databases"), name); }
        @Override public ApplicationInfo getApplicationInfo() {
            ApplicationInfo result = new ApplicationInfo(super.getApplicationInfo());
            try { result.dataDir = root.getCanonicalPath(); }
            catch (Exception error) { throw new AssertionError(error); }
            return result;
        }
        @Override public synchronized SharedPreferences getSharedPreferences(String name, int mode) {
            SharedPreferences value = preferences.get(name);
            if (value == null) { value = new MemoryPreferences(); preferences.put(name, value); }
            return value;
        }
    }
    private static final class MemoryPreferences implements SharedPreferences {
        final Map<String, Object> values = new HashMap<>();
        @Override public synchronized Map<String, ?> getAll() { return new HashMap<>(values); }
        @Override public synchronized String getString(String key, String fallback) { return (String) values.getOrDefault(key, fallback); }
        @Override @SuppressWarnings("unchecked") public synchronized Set<String> getStringSet(String key, Set<String> fallback) { Object value = values.get(key); return value == null ? fallback : new HashSet<>((Set<String>) value); }
        @Override public synchronized int getInt(String key, int fallback) { return (Integer) values.getOrDefault(key, fallback); }
        @Override public synchronized long getLong(String key, long fallback) { return (Long) values.getOrDefault(key, fallback); }
        @Override public synchronized float getFloat(String key, float fallback) { return (Float) values.getOrDefault(key, fallback); }
        @Override public synchronized boolean getBoolean(String key, boolean fallback) { return (Boolean) values.getOrDefault(key, fallback); }
        @Override public synchronized boolean contains(String key) { return values.containsKey(key); }
        @Override public void registerOnSharedPreferenceChangeListener(OnSharedPreferenceChangeListener listener) {}
        @Override public void unregisterOnSharedPreferenceChangeListener(OnSharedPreferenceChangeListener listener) {}
        @Override public Editor edit() { return new OwnedEditor(); }
        private final class OwnedEditor implements Editor {
            final Map<String, Object> changes = new HashMap<>();
            boolean clear;
            @Override public Editor putString(String key, String value) { changes.put(key, value); return this; }
            @Override public Editor putStringSet(String key, Set<String> value) { changes.put(key, value == null ? null : new HashSet<>(value)); return this; }
            @Override public Editor putInt(String key, int value) { changes.put(key, value); return this; }
            @Override public Editor putLong(String key, long value) { changes.put(key, value); return this; }
            @Override public Editor putFloat(String key, float value) { changes.put(key, value); return this; }
            @Override public Editor putBoolean(String key, boolean value) { changes.put(key, value); return this; }
            @Override public Editor remove(String key) { changes.put(key, null); return this; }
            @Override public Editor clear() { clear = true; return this; }
            @Override public boolean commit() { synchronized (MemoryPreferences.this) { if (clear) values.clear(); for (Map.Entry<String, Object> change : changes.entrySet()) { if (change.getValue() == null) values.remove(change.getKey()); else values.put(change.getKey(), change.getValue()); } } return true; }
            @Override public void apply() { commit(); }
        }
    }
}
