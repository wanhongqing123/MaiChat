package com.kongshang.maichat;

import static org.junit.Assert.*;

import android.app.Instrumentation;
import android.app.Activity;
import android.content.Context;
import android.graphics.Bitmap;
import android.os.Bundle;
import android.os.SystemClock;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.core.app.ActivityScenario;
import androidx.test.platform.app.InstrumentationRegistry;
import androidx.test.runner.lifecycle.ActivityLifecycleMonitorRegistry;
import androidx.test.runner.lifecycle.Stage;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.lang.reflect.Field;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.json.JSONObject;
import org.junit.Assume;
import org.junit.Test;
import org.junit.runner.RunWith;

/** Explicitly opted-in validation using the normal account, without replacing its session.
 * Cloud validation uses the real save confirmation and only touches its generated object.
 * IM validation requires the operator-owned peer in im_peer. Neither path logs
 * credentials, signed links, remote response bodies, or unrelated messages.
 */
@RunWith(AndroidJUnit4.class)
public final class NormalAccountLiveInstrumentedTest {
    private final Instrumentation instrumentation = InstrumentationRegistry.getInstrumentation();
    private ActivityScenario<MainActivity> launchedScenario;

    @Test public void normalAccountUploadsOwnedPngThroughUiAndDownloadsIdenticalBytes() throws Exception {
        optIn("runNormalCloudUpload");
        stage("cloud-start");
        Context context = instrumentation.getTargetContext();
        String owner = MaiChatAuthClient.shared(context).accountId();
        assertFalse("Normal account must already be signed in", owner.isEmpty());
        stage("cloud-auth-ready");
        String nonce = "normal-live-" + UUID.randomUUID();
        File source = new File(context.getCacheDir(), nonce + ".png");
        File downloaded = new File(context.getCacheDir(), nonce + "-download.png");
        File receipt = receipt(context, nonce);
        JSONObject ledger = new JSONObject().put("source_name", source.getName())
            .put("state", "prepared");
        save(receipt, ledger);
        Bitmap bitmap = Bitmap.createBitmap(16, 16, Bitmap.Config.ARGB_8888);
        bitmap.eraseColor(0xff218dcc);
        try (FileOutputStream output = new FileOutputStream(source)) {
            assertTrue(bitmap.compress(Bitmap.CompressFormat.PNG, 100, output));
        } finally { bitmap.recycle(); }
        stage("cloud-source-ready");
        MainActivity activity = launch(context);
        awaitRestoredHost(activity);
        MaiCloudDrive client = new MaiCloudDrive(context);
        AtomicReference<String> created = new AtomicReference<>();
        AtomicReference<MaiCloudDriveDialog> opened = new AtomicReference<>();
        AtomicReference<String> lastSnapshot = new AtomicReference<>("");
        boolean deleted = false;
        try {
            cleanupPreviousOwned(activity, context, owner, client);
            stage("cloud-save-dispatch");
            instrumentation.runOnMainSync(() -> {
                stage("cloud-save-enter");
                activity.showCloudSave(source);
                opened.set(activity.currentCloudDriveDialog());
                stage("cloud-save-return");
            });
            assertTrue("Cloud save confirmation was not displayed", waitUntil(10_000, () -> {
                AccessibilityNodeInfo root = instrumentation.getUiAutomation().getRootInActiveWindow();
                if (root == null) return false;
                try {
                    return !root.findAccessibilityNodeInfosByText("保存到我的云盘").isEmpty();
                } finally { root.recycle(); }
            }));
            assertTrue("Could not click the normal save confirmation", clickSave());
            stage("cloud-save-clicked");
            ledger.put("state", "upload_requested"); save(receipt, ledger);
            assertTrue("New owned cloud object did not appear in the UI", waitUntil(100_000, () -> {
                AtomicReference<String> found = new AtomicReference<>();
                AtomicReference<JSONObject> observed = new AtomicReference<>();
                instrumentation.runOnMainSync(() -> {
                    found.set(ownedId(opened.get(), source.getName()));
                    observed.set(cloudSnapshot(activity, opened.get(), source.getName()));
                });
                String snapshot = observed.get().toString();
                if (!snapshot.equals(lastSnapshot.getAndSet(snapshot))) {
                    android.util.Log.i("MaiChatLiveValidation", "cloud_ui=" + snapshot);
                    ledger.put("last_ui", observed.get()); save(receipt, ledger);
                }
                assertTrue("Owned cloud dialog closed during upload; see safe device receipt",
                    observed.get().optBoolean("showing"));
                if (found.get() == null) return false;
                created.set(found.get());
                return true;
            }));
            ledger.put("media_id", created.get()).put("state", "uploaded"); save(receipt, ledger);
            assertTrue("Owned uploaded filename did not become visible", waitUntil(10_000, () -> {
                AccessibilityNodeInfo root = instrumentation.getUiAutomation().getRootInActiveWindow();
                if (root == null) return false;
                try {
                    for (AccessibilityNodeInfo node : root.findAccessibilityNodeInfosByText(source.getName()))
                        if (node.isVisibleToUser()) return true;
                    return false;
                } finally { root.recycle(); }
            }));
            Bitmap uploadedScreen = instrumentation.getUiAutomation().takeScreenshot();
            assertNotNull("Owned uploaded cloud screenshot failed", uploadedScreen);
            File screenshot = new File(receipt.getParentFile(), nonce + "-cloud-upload.png");
            try (FileOutputStream output = new FileOutputStream(screenshot)) {
                assertTrue(uploadedScreen.compress(Bitmap.CompressFormat.PNG, 100, output));
            } finally { uploadedScreen.recycle(); }
            stage("cloud-upload-screenshot-saved");
            sameOwner(context, owner);
            MaiCloudDrive.Result download = request(client, MaiCloudDrive.DOWNLOAD_MEDIA,
                new JSONObject().put("media_id", created.get())
                    .put("output_path", downloaded.getAbsolutePath()));
            assertTrue("Owned download failed; status=" + download.status +
                " http=" + download.httpStatus, download.ok());
            assertEquals("Downloaded size differs", source.length(), downloaded.length());
            assertTrue("Downloaded SHA-256 differs", MessageDigest.isEqual(sha(source), sha(downloaded)));
            ledger.put("state", "download_sha_verified").put("bytes", downloaded.length())
                .put("sha256", hex(sha(downloaded))); save(receipt, ledger);
            sameOwner(context, owner);
            MaiCloudDrive.Result removed = request(client, MaiCloudDrive.DELETE,
                new JSONObject().put("video_id", created.get()));
            assertTrue("Owned cleanup failed; status=" + removed.status +
                " http=" + removed.httpStatus, removed.ok());
            deleted = true;
            MaiCloudDrive.Result absent = request(client, MaiCloudDrive.PLAY_LINK,
                new JSONObject().put("video_id", created.get()));
            assertEquals("Deleted owned object remains available", 404, absent.httpStatus);
            ledger.put("state", "deleted_and_unavailable"); save(receipt, ledger);
            android.util.Log.i("MaiChatLiveValidation", "cloud owned_upload=1 sha_verified=1 deleted=1");
        } finally {
            // A failed assertion still permits cleanup only after obtaining our exact new ID.
            // If the account changed, leave the receipt for the operator rather than deleting
            // using a different account. Never enumerate or delete preexisting cloud objects.
            if (created.get() == null) {
                instrumentation.runOnMainSync(() -> created.set(ownedId(opened.get(), source.getName())));
            }
            if (!deleted && created.get() != null) {
                ledger.put("media_id", created.get());
                if (owner.equals(MaiChatAuthClient.shared(context).accountId())) {
                    MaiCloudDrive.Result cleanup = request(client, MaiCloudDrive.DELETE,
                        new JSONObject().put("video_id", created.get()));
                    ledger.put("cleanup_ok", cleanup.ok());
                } else ledger.put("cleanup_ok", false);
                ledger.put("state", "failure_cleanup_attempted"); save(receipt, ledger);
            }
            source.delete(); downloaded.delete();
            instrumentation.runOnMainSync(() -> {
                MaiCloudDriveDialog dialog = activity.currentCloudDriveDialog();
                if (dialog != null && dialog.isShowing()) dialog.dismiss();
            });
            closeLaunchedScenario();
        }
    }

    @Test public void normalAccountSendsToOwnedCliAndReceivesExactNonce() throws Exception {
        optIn("runNormalImExchange");
        stage("im-start");
        String nonce = InstrumentationRegistry.getArguments().getString("im_nonce", "");
        assertTrue("Provide a fresh ASCII im_nonce", nonce.matches("live-[A-Za-z0-9-]{12,80}"));
        // Require an explicit operator-owned destination for each run; source carries no live ID.
        String peer = InstrumentationRegistry.getArguments().getString("im_peer");
        assertNotNull("Provide the operator-owned IM destination via im_peer", peer);
        assertTrue("im_peer must be nonempty without surrounding whitespace",
            !peer.isEmpty() && peer.equals(peer.trim()));
        // The real imcli transport appends this fixed footer to every text send.
        // Require the complete body, including its run nonce, rather than a substring.
        String expectedReply = nonce + "-cli\n\n此消息来自 imcli";
        Context context = instrumentation.getTargetContext();
        MainActivity activity = launch(context);
        try {
            RemoteIMSessionController session = session(activity);
            assertTrue("Normal IM did not connect", waitUntil(30_000, () -> onMain(() ->
                session.connectionState() == TencentIMClient.ConnectionState.CONNECTED)));
            AtomicReference<String> owner = new AtomicReference<>();
            instrumentation.runOnMainSync(() -> owner.set(session.chatState().ownerUserId()));
            assertTrue("Normal IM identity is unavailable",
                owner.get() != null && !owner.get().isEmpty());
            assertFalse("Cannot exchange with the same IM identity", peer.equals(owner.get()));
            JSONObject ledger = new JSONObject().put("state", "ready").put("peer", peer)
                .put("owner_im_id", owner.get()).put("nonce", nonce);
            File receipt = receipt(context, nonce + "-im"); save(receipt, ledger);
            android.util.Log.i("MaiChatLiveValidation", "im ready authenticated=1 nonce=" + nonce);
            AtomicReference<RemoteIMMessage> sent = new AtomicReference<>();
            instrumentation.runOnMainSync(() -> {
                try { sent.set(session.sendTextMessageTo(peer, nonce + "-android", null)); }
                catch (Exception error) { throw new AssertionError("Owned IM send failed", error); }
            });
            assertTrue("Owned IM send was not acknowledged", waitUntil(30_000, () -> onMain(() ->
                sent.get().status() == RemoteIMMessage.Status.SENT && !sent.get().remoteId().isEmpty())));
            ledger.put("state", "send_acknowledged"); save(receipt, ledger);
            android.util.Log.i("MaiChatLiveValidation", "im send_ack=1 nonce=" + nonce);
            // Inspect only exact run nonce matches in the current in-memory state. Do not
            // load history pages, create friendships, or print any other message contents.
            assertTrue("Exact owned CLI reply was not received", waitUntil(150_000, () -> onMain(() -> {
                int count = 0;
                for (RemoteIMMessage message : session.chatState().messages()) {
                    if (peer.equals(message.fromUserId()) && owner.get().equals(message.toUserId())
                        && expectedReply.equals(message.text())
                        && message.direction() == RemoteIMMessage.Direction.INCOMING
                        && !message.remoteId().isEmpty()) count++;
                }
                return count == 1;
            })));
            ledger.put("state", "send_acknowledged_reply_received"); save(receipt, ledger);
            android.util.Log.i("MaiChatLiveValidation", "im send_ack=1 exact_reply=1 nonce=" + nonce);
        } finally { closeLaunchedScenario(); }
    }

    private void optIn(String operation) {
        Assume.assumeTrue("Explicit normal-account operator opt-in required",
            "Rootrunyes".equals(InstrumentationRegistry.getArguments().getString(operation)));
        assertTrue("Normal debug application without local UI auth required", BuildConfig.DEBUG
            && !BuildConfig.USE_LOCAL_UI_AUTH
            && "com.kongshang.maichat".equals(BuildConfig.APPLICATION_ID));
    }
    private MainActivity launch(Context context) {
        stage("activity-resolve-enter");
        AtomicReference<MainActivity> resumed = new AtomicReference<>();
        instrumentation.runOnMainSync(() -> {
            for (Activity activity : ActivityLifecycleMonitorRegistry.getInstance()
                    .getActivitiesInStage(Stage.RESUMED)) {
                if (activity instanceof MainActivity && !activity.isFinishing()
                    && !activity.isDestroyed()) resumed.set((MainActivity) activity);
            }
        });
        if (resumed.get() != null) {
            stage("activity-resumed-reused");
            return resumed.get();
        }
        // startActivitySync waits without a deadline for a newly created Activity.
        // A NEW_TASK launch that reuses the foreground task may never create one.
        // Reuse the current resumed host above; otherwise let ActivityScenario
        // monitor lifecycle transitions with its own bounded launch wait.
        stage("activity-scenario-launch");
        launchedScenario = ActivityScenario.launch(MainActivity.class);
        launchedScenario.onActivity(resumed::set);
        assertNotNull("Normal MainActivity did not resume", resumed.get());
        stage("activity-scenario-ready");
        return resumed.get();
    }
    private void closeLaunchedScenario() {
        if (launchedScenario != null) {
            launchedScenario.close();
            launchedScenario = null;
        }
    }
    private static void stage(String value) {
        android.util.Log.i("MaiChatLiveValidation", "stage=" + value);
    }
    private interface Check { boolean okay() throws Exception; }
    private boolean waitUntil(long timeout, Check check) throws Exception {
        long deadline = SystemClock.elapsedRealtime() + timeout;
        do { if (check.okay()) return true; Thread.sleep(150); }
        while (SystemClock.elapsedRealtime() < deadline);
        return false;
    }
    private boolean onMain(Check check) {
        AtomicReference<Boolean> value = new AtomicReference<>(false);
        instrumentation.runOnMainSync(() -> {
            try { value.set(check.okay()); }
            catch (Exception error) { throw new AssertionError("Main-thread live check failed", error); }
        });
        return value.get();
    }
    private boolean clickSave() {
        AccessibilityNodeInfo root = instrumentation.getUiAutomation().getRootInActiveWindow();
        if (root == null) return false;
        try {
            for (AccessibilityNodeInfo node : root.findAccessibilityNodeInfosByText("保存")) {
                if ("保存".contentEquals(node.getText()) && node.isClickable()
                    && "android.widget.Button".contentEquals(node.getClassName()))
                    return node.performAction(AccessibilityNodeInfo.ACTION_CLICK);
            }
            return false;
        } finally { root.recycle(); }
    }
    private static RemoteIMSessionController session(MainActivity activity) throws Exception {
        Field field = MainActivity.class.getDeclaredField("session"); field.setAccessible(true);
        return (RemoteIMSessionController) field.get(activity);
    }
    private void awaitRestoredHost(MainActivity activity) throws Exception {
        stage("host-restore-wait");
        RemoteIMSessionController session = session(activity);
        assertTrue("Normal account host did not finish restoring", waitUntil(30_000, () -> onMain(() -> {
            Field displayed = MainActivity.class.getDeclaredField("displayedOwner");
            displayed.setAccessible(true);
            return !session.isRestoringAccount() && !session.requiresLogin()
                && session.connectionState() == TencentIMClient.ConnectionState.CONNECTED
                && session.settings().loginUserId().equals(displayed.get(activity));
        })));
        stage("host-restore-ready");
    }
    private void cleanupPreviousOwned(MainActivity activity, Context context, String owner,
                                      MaiCloudDrive client) throws Exception {
        String names = InstrumentationRegistry.getArguments().getString("cleanupPreviousOwnedNames",
            InstrumentationRegistry.getArguments().getString("cleanupPreviousOwnedName", ""));
        if (names.isEmpty()) return;
        String[] requested = names.split(",", -1);
        assertTrue("Bound previous owned cleanup to at most two named test assets", requested.length <= 2);
        java.util.HashSet<String> seen = new java.util.HashSet<>();
        for (String name : requested) {
            assertTrue("Repeated previous owned name", seen.add(name));
            cleanupPreviousOwnedName(activity, context, owner, client, name);
        }
    }
    private void cleanupPreviousOwnedName(MainActivity activity, Context context, String owner,
                                          MaiCloudDrive client, String name) throws Exception {
        assertTrue("Only a generated live-validation PNG may be cleaned",
            name.matches("normal-live-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\.png"));
        File previous = receipt(context, name.substring(0, name.length() - 4));
        assertTrue("Previous test ownership receipt is required", previous.isFile() && previous.length() < 8192);
        JSONObject ledger;
        try (FileInputStream input = new FileInputStream(previous)) {
            byte[] bytes = new byte[(int) previous.length()];
            int offset = 0, count;
            while (offset < bytes.length && (count = input.read(bytes, offset, bytes.length - offset)) > 0)
                offset += count;
            assertEquals(bytes.length, offset);
            ledger = new JSONObject(new String(bytes, StandardCharsets.UTF_8));
        }
        assertEquals("Previous receipt does not own this generated name", name, ledger.optString("source_name"));
        stage("previous-owned-cloud-open");
        instrumentation.runOnMainSync(() -> {
            View tab = description(activity.getWindow().getDecorView(), "云盘");
            assertNotNull("Normal cloud navigation tab is unavailable", tab);
            tab.performClick();
        });
        assertTrue("Previous owned cloud lookup did not finish", waitUntil(30_000, () -> onMain(() -> {
            MaiCloudDriveDialog dialog = activity.currentCloudDriveDialog();
            return dialog != null && dialog.isShowing() && !(Boolean) field(dialog, "loading")
                && !(Boolean) field(dialog, "folderLoading");
        })));
        AtomicReference<String> id = new AtomicReference<>();
        instrumentation.runOnMainSync(() -> {
            MaiCloudDriveDialog dialog = activity.currentCloudDriveDialog();
            try {
                assertTrue("Previous owned lookup failed", ((String) field(dialog, "listError")).isEmpty());
                id.set(ownedId(dialog, name));
                if (id.get() == null) assertEquals("Previous name not found on first page; operator review required",
                    "", field(dialog, "cursor"));
            } catch (ReflectiveOperationException error) { throw new AssertionError(error); }
        });
        sameOwner(context, owner);
        if (id.get() != null) {
            ledger.put("media_id", id.get()); save(previous, ledger);
            MaiCloudDrive.Result removed = request(client, MaiCloudDrive.DELETE,
                new JSONObject().put("video_id", id.get()));
            assertTrue("Previous exact owned object cleanup failed", removed.ok());
            MaiCloudDrive.Result absent = request(client, MaiCloudDrive.PLAY_LINK,
                new JSONObject().put("video_id", id.get()));
            assertEquals("Previous owned object remains available", 404, absent.httpStatus);
            ledger.put("previous_object", "deleted_and_unavailable");
        } else ledger.put("previous_object", "not_found_complete_ui_page");
        save(previous, ledger);
        // Native checkpoint listing exposes only public resume metadata, never signing
        // credentials. Only the exact name proven by this test's old receipt is aborted.
        CountDownLatch listed = new CountDownLatch(1);
        AtomicReference<MaiCloudDrive.Result> checkpoints = new AtomicReference<>();
        client.checkpoints(value -> { checkpoints.set(value); listed.countDown(); });
        assertTrue("Owned checkpoint lookup timed out", listed.await(20, TimeUnit.SECONDS));
        assertTrue("Owned checkpoint lookup failed", checkpoints.get() != null && checkpoints.get().ok());
        org.json.JSONArray uploads = checkpoints.get().json().optJSONArray("uploads");
        int aborted = 0;
        if (uploads != null) for (int i = 0; i < uploads.length(); i++) {
            JSONObject item = uploads.optJSONObject(i);
            if (item == null || !name.equals(item.optString("name"))) continue;
            assertEquals("Unexpected duplicate checkpoint for exact owned name", 0, aborted);
            sameOwner(context, owner);
            CountDownLatch done = new CountDownLatch(1);
            AtomicReference<MaiCloudDrive.Result> removed = new AtomicReference<>();
            client.abort(new File(item.getString("checkpoint_path")), value -> {
                removed.set(value); done.countDown();
            });
            assertTrue("Previous owned upload abort timed out", done.await(45, TimeUnit.SECONDS));
            assertTrue("Previous owned upload abort failed", removed.get() != null && removed.get().ok());
            aborted++;
        }
        ledger.put("previous_checkpoints_aborted", aborted); save(previous, ledger);
        stage("previous-owned-cleanup-complete");
        instrumentation.runOnMainSync(() -> {
            View tab = description(activity.getWindow().getDecorView(), "消息");
            assertNotNull("Normal messages navigation tab is unavailable", tab);
            tab.performClick();
        });
    }
    private static View description(View view, String text) {
        if (view.getContentDescription() != null && text.contentEquals(view.getContentDescription())) return view;
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) {
                View found = description(group.getChildAt(i), text);
                if (found != null) return found;
            }
        }
        return null;
    }
    private static String ownedId(MainActivity activity, String name) {
        return ownedId(activity.currentCloudDriveDialog(), name);
    }
    private static String ownedId(MaiCloudDriveDialog dialog, String name) {
        if (dialog == null) return null;
        try {
            Field field = MaiCloudDriveDialog.class.getDeclaredField("media"); field.setAccessible(true);
            @SuppressWarnings("unchecked") List<JSONObject> media = (List<JSONObject>) field.get(dialog);
            String found = null;
            for (JSONObject item : media) {
                if (!name.equals(item.optString("name"))) continue;
                assertNull("Fresh UUID name unexpectedly matched multiple objects", found);
                found = item.optString("id", item.optString("video_id", ""));
                assertTrue("Owned media ID is invalid", found.matches("[0-9a-fA-F-]{36}"));
            }
            return found;
        } catch (ReflectiveOperationException error) { throw new AssertionError("Cloud UI inspection failed", error); }
    }
    private static Object field(Object owner, String name) throws ReflectiveOperationException {
        Field field = owner.getClass().getDeclaredField(name); field.setAccessible(true);
        return field.get(owner);
    }
    private static JSONObject cloudSnapshot(MainActivity activity, MaiCloudDriveDialog dialog, String name) {
        try {
            JSONObject result = new JSONObject().put("present", dialog != null)
                .put("same_dialog", activity.currentCloudDriveDialog() == dialog)
                .put("showing", dialog != null && dialog.isShowing());
            if (dialog == null) return result;
            result.put("uploading", field(dialog, "uploading"))
                .put("loading", field(dialog, "loading"))
                .put("folder_loading", field(dialog, "folderLoading"))
                .put("list_error", !((String) field(dialog, "listError")).isEmpty());
            String status = ((TextView) field(dialog, "status")).getText().toString();
            result.put("status", safeStatus(status));
            @SuppressWarnings("unchecked") List<JSONObject> media = (List<JSONObject>) field(dialog, "media");
            org.json.JSONArray matches = new org.json.JSONArray();
            String stem = name.substring(0, name.length() - 4);
            for (JSONObject item : media) {
                if (!item.optString("name").contains(stem) && !item.optString("title").contains(stem)) continue;
                JSONObject match = new JSONObject();
                for (String key : new String[]{"id", "video_id", "name", "title"})
                    if (item.has(key)) match.put(key, item.optString(key));
                matches.put(match);
            }
            return result.put("nonce_matches", matches);
        } catch (Exception error) { throw new AssertionError("Safe cloud state inspection failed", error); }
    }
    private static String safeStatus(String status) {
        if (status.isEmpty()) return "empty";
        if (status.startsWith("准备上传 ")) return "preparing_upload";
        if (status.startsWith("正在准备文件 ")) return "preparing_file";
        if (status.startsWith("正在上传 ")) return "uploading";
        String[] allowed = {"已保存到我的云盘", "已暂停，可稍后续传", "设备空间不足，无法暂存这个文件",
            "无法生成或恢复图片缩略图，请重新选择可解码的原图", "暂存文件与上传记录不一致，请放弃旧上传后重选",
            "请重新登录 MaiChat", "云盘暂未开放", "上传期限已过，请重新选择文件",
            "文件超过大小限制或云盘剩余容量不足", "请求太频繁，请稍后重试", "网络或服务暂不可用，请重试",
            "暂时无法读取文件夹，请重试", "文件夹已不存在，请刷新云盘", "云盘分页未前进，请重试"};
        for (int i = 0; i < allowed.length; i++) if (allowed[i].equals(status)) return "known_" + i;
        return "other_redacted";
    }
    private static void sameOwner(Context context, String owner) {
        assertTrue("Account changed during owned cloud validation",
            owner.equals(MaiChatAuthClient.shared(context).accountId()));
    }
    private static MaiCloudDrive.Result request(MaiCloudDrive client, int action, JSONObject payload)
            throws Exception {
        CountDownLatch done = new CountDownLatch(1);
        AtomicReference<MaiCloudDrive.Result> result = new AtomicReference<>();
        client.request(action, payload, value -> { result.set(value); done.countDown(); });
        assertTrue("Owned cloud operation timed out", done.await(60, TimeUnit.SECONDS));
        assertNotNull("Owned cloud operation returned no result", result.get());
        return result.get();
    }
    private static File receipt(Context context, String nonce) {
        File directory = new File(context.getFilesDir(), "normal-live-validation");
        assertTrue(directory.mkdirs() || directory.isDirectory());
        return new File(directory, nonce + ".json");
    }
    private static void save(File receipt, JSONObject ledger) throws Exception {
        try (FileOutputStream output = new FileOutputStream(receipt)) {
            output.write(ledger.toString().getBytes(StandardCharsets.UTF_8));
        }
    }
    private static byte[] sha(File file) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (FileInputStream input = new FileInputStream(file)) {
            byte[] buffer = new byte[4096]; int count;
            while ((count = input.read(buffer)) != -1) digest.update(buffer, 0, count);
        }
        return digest.digest();
    }
    private static String hex(byte[] bytes) {
        StringBuilder value = new StringBuilder();
        for (byte item : bytes) value.append(String.format(java.util.Locale.ROOT, "%02x", item & 255));
        return value.toString();
    }
}
