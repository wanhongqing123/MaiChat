// 文件作用：在真实 View 树中验证创作中心的联动侧滑、方向锁和按设备组织的会话入口。
package com.kongshang.maichat;

import static org.junit.Assert.*;

import android.app.Activity;
import android.os.Looper;
import android.view.MotionEvent;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;
import android.widget.ListView;
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
import org.robolectric.annotation.Implementation;
import org.robolectric.annotation.Implements;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w400dp-h800dp-mdpi", shadows = {
    AIAssistantSessionDrawerTest.OwnedController.class, AIAssistantSessionDrawerTest.OwnedImageLoader.class,
    AIAssistantSessionDrawerTest.OwnedMainActivity.class})
public final class AIAssistantSessionDrawerTest {
    private Activity activity;
    private AIAssistantController controller;
    private AIAssistantPanel panel;
    private long clock;

    @Before public void setup() throws Exception {
        activity = Robolectric.buildActivity(Activity.class).setup().get();
        controller = new AIAssistantController(activity, "http://owned.invalid");
        OwnedController.createdDevice = null; OwnedController.creationCompletion = null;
        OwnedController.imageLoads = 0; OwnedController.imageCompletion = null;
        OwnedController.cachedImage = null;
        OwnedController.imagePublications = 0; OwnedController.publicationCompletion = null;
        OwnedController.publicationMessage = null; OwnedController.publicationPart = null;
        OwnedImageLoader.failDecode = false;
        panel = new AIAssistantPanel(activity, controller);
        activity.setContentView(panel);
        JSONObject device = new JSONObject().put("device_slot", "desktop").put("device_id", "pc")
            .put("device_name", "我的电脑").put("online", true);
        JSONObject remote = new JSONObject(device.toString()).put("id", "remote-one")
            .put("title", "电脑上的对话").put("login_session_id", "login");
        JSONObject local = new JSONObject().put("id", "local-one").put("title", "本机对话");
        JSONObject data = new JSONObject().put("sessions", new JSONArray().put(local).put(remote))
            .put("devices", new JSONArray().put(device)).put("messages", new JSONArray());
        panel.onState(new AIAssistantController.State(data, null, "local-one", "", "deepseek-flash",
            "on-request", "", "", "", 0, false, true));
        layout();
    }

    @After public void close() throws Exception {
        panel.setForeground(false);
        Method close = AIAssistantController.class.getDeclaredMethod("close");
        close.setAccessible(true); close.invoke(controller);
    }

    @Test public void edgeDragMovesTheExistingConversationTogetherWithTheDrawer() {
        View card = named(panel, "创作中心会话页面");
        assertNotNull("The original conversation must stay in one movable card", card);
        touch(MotionEvent.ACTION_DOWN, 8, 240);
        touch(MotionEvent.ACTION_MOVE, 160, 244);
        assertEquals(152f, card.getTranslationX(), 1f);
        View drawer = named(panel, "对话列表");
        assertNotNull(drawer);
        assertEquals("The drawer must be measured at its full width before it is revealed", 312, drawer.getMeasuredWidth());
        assertEquals(152f - 400 * .78f, drawer.getTranslationX(), 1f);
        touch(MotionEvent.ACTION_UP, 280, 244);
        settle();
        assertEquals(312f, card.getTranslationX(), 1f);
        assertEquals(.95f, card.getScaleX(), .001f);
        assertNotNull(named(panel, "收起对话列表"));
    }

    @Test public void verticalAndInteriorDragsKeepTheConversationInPlace() {
        View card = named(panel, "创作中心会话页面");
        assertNotNull(card);
        touch(MotionEvent.ACTION_DOWN, 8, 240);
        touch(MotionEvent.ACTION_MOVE, 14, 340);
        touch(MotionEvent.ACTION_MOVE, 200, 345);
        touch(MotionEvent.ACTION_UP, 200, 345);
        settle();
        assertEquals(0f, card.getTranslationX(), 0f);
        touch(MotionEvent.ACTION_DOWN, 100, 240);
        touch(MotionEvent.ACTION_MOVE, 310, 240);
        touch(MotionEvent.ACTION_UP, 310, 240);
        settle();
        assertEquals(0f, card.getTranslationX(), 0f);
    }

    @Test public void cancelledDragRestoresTheOriginalEndpointAndPreservesTheDraft() {
        View card = named(panel, "创作中心会话页面");
        assertNotNull(card);
        panel.composer.setText("未发送的草稿");
        touch(MotionEvent.ACTION_DOWN, 8, 240);
        touch(MotionEvent.ACTION_MOVE, 220, 240);
        assertTrue("The cancellation fixture must first move the page", card.getTranslationX() > 0);
        touch(MotionEvent.ACTION_CANCEL, 220, 240);
        settle();
        assertEquals(0f, card.getTranslationX(), 0f);
        assertEquals("未发送的草稿", panel.composer.getText().toString());
    }

    @Test public void toolbarOpensTheSameDrawerWithCollapsibleDeviceGroupsAndDirectRemoteCreation() {
        named(panel, "打开对话列表").performClick();
        settle(); layout();
        View local = named(panel, "这台设备，本机，1 个对话");
        View desktop = named(panel, "我的电脑，在线，1 个对话");
        assertNotNull(local); assertNotNull(desktop);
        assertNotNull(named(panel, "在我的电脑上新建对话"));
        assertNotNull(text(panel, "电脑上的对话"));
        desktop.performClick();
        assertNull(text(panel, "电脑上的对话"));
        assertNotNull(text(panel, "本机对话"));
    }

    @Test public void deviceHeaderKeepsTheCatalogNameAndAggregatesSessionPresenceLikeIOS() throws Exception {
        JSONObject device = new JSONObject().put("device_slot", "windows").put("device_id", "pc")
            .put("device_name", "hongqingwan").put("online", false);
        JSONObject remote = new JSONObject(device.toString()).put("id", "remote-one")
            .put("title", "电脑上的对话").put("login_session_id", "login").put("online", true);
        JSONObject data = new JSONObject().put("devices", new JSONArray().put(device))
            .put("sessions", new JSONArray().put(remote)).put("messages", new JSONArray());
        panel.onState(new AIAssistantController.State(data, null, "", "", "deepseek-flash",
            "on-request", "", "", "", 0, false, true));
        named(panel, "打开对话列表").performClick(); settle(); layout();
        assertNotNull("The same device_name is displayed on iOS, with presence from either catalog projection",
            named(panel, "hongqingwan，在线，1 个对话"));
        assertFalse("Display aggregation must not grant a new command lease to a stale device catalog",
            named(panel, "在hongqingwan上新建对话").isEnabled());
    }

    @Test public void remoteCreateUsesTheIOSBlueActionColor() {
        named(panel, "打开对话列表").performClick(); settle(); layout();
        android.widget.ImageButton create = (android.widget.ImageButton) named(panel, "在我的电脑上新建对话");
        assertEquals(MaiChatTheme.BLUE,
            Shadows.shadowOf((android.graphics.PorterDuffColorFilter) create.getColorFilter()).getColor());
    }

    @Test public void expandedDrawerClosesByDraggingTheConversationAndByAndroidBack() {
        named(panel, "打开对话列表").performClick(); settle(); layout();
        View card = named(panel, "创作中心会话页面");
        touch(MotionEvent.ACTION_DOWN, 350, 260);
        touch(MotionEvent.ACTION_MOVE, 120, 260);
        assertTrue(card.getTranslationX() < 100);
        touch(MotionEvent.ACTION_UP, 100, 260); settle();
        assertEquals(0, card.getTranslationX(), .01f);
        named(panel, "打开对话列表").performClick(); settle();
        assertTrue(panel.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BACK)));
        settle(); assertEquals(0, card.getTranslationX(), .01f);
    }

    @Test public void grabbingAnAnimatingDrawerDoesNotApplyTheOldAnimationEndpoint() {
        named(panel, "打开对话列表").performClick();
        Shadows.shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(80));
        View card = named(panel, "创作中心会话页面");
        touch(MotionEvent.ACTION_DOWN, 350, 260);
        touch(MotionEvent.ACTION_MOVE, 300, 260);
        float distance = card.getTranslationX();
        Shadows.shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(32));
        assertEquals("An old completion must not jump the card during a new drag", distance, card.getTranslationX(), .01f);
        touch(MotionEvent.ACTION_CANCEL, 300, 260); settle();
        assertEquals(312f, card.getTranslationX(), .01f);
    }

    @Test public void dismissingTheKeyboardDuringAnEdgeDragKeepsTheSameDrawerProgress() {
        View card = named(panel, "创作中心会话页面");
        touch(MotionEvent.ACTION_DOWN, 8, 240);
        touch(MotionEvent.ACTION_MOVE, 160, 240);
        assertEquals(152f, card.getTranslationX(), .01f);
        panel.measure(View.MeasureSpec.makeMeasureSpec(400, View.MeasureSpec.EXACTLY),
            View.MeasureSpec.makeMeasureSpec(600, View.MeasureSpec.EXACTLY));
        panel.layout(0, 0, 400, 600);
        assertEquals("A keyboard-only height change must preserve the active drag", 152f, card.getTranslationX(), .01f);
        assertEquals(300f, card.getPivotY(), .01f);
        touch(MotionEvent.ACTION_MOVE, 220, 240);
        assertEquals(212f, card.getTranslationX(), .01f);
        touch(MotionEvent.ACTION_UP, 280, 240); settle();
        assertEquals(312f, card.getTranslationX(), .01f);
    }

    @Test public void systemBackKeepsTheBoundedDrawerSwipeAreaAndReleasesItInBackground() {
        assertEquals(1, panel.getSystemGestureExclusionRects().size());
        android.graphics.Rect rect = panel.getSystemGestureExclusionRects().get(0);
        assertEquals(0, rect.left); assertEquals(32, rect.right);
        assertEquals(200, rect.height()); assertEquals(400, rect.centerY());
        panel.setForeground(false);
        assertTrue(panel.getSystemGestureExclusionRects().isEmpty());
    }

    @Test public void hostBackClosesTheDrawerBeforeLeavingCreationAndThenKeepsItsOriginalNavigation() throws Exception {
        MainActivity host = Robolectric.buildActivity(MainActivity.class).get();
        LocalSettingsStore settings = new LocalSettingsStore(new java.io.File(activity.getCacheDir(), "owned-host-settings"));
        settings.save(new RemoteIMSettings("owned-host-user"));
        RemoteIMSessionController session = new RemoteIMSessionController(settings,
            new LocalChatHistoryStore(new java.io.File(activity.getCacheDir(), "owned-host-history")));
        org.robolectric.util.ReflectionHelpers.setField(host, "session", session);
        org.robolectric.util.ReflectionHelpers.setField(host, "activeTab", RemoteIMTab.ASSISTANT);
        org.robolectric.util.ReflectionHelpers.setField(host, "aiAssistant", panel);
        named(panel, "打开对话列表").performClick(); settle();
        host.onBackPressed(); settle();
        assertEquals(RemoteIMTab.ASSISTANT, org.robolectric.util.ReflectionHelpers.getField(host, "activeTab"));
        assertEquals(0, named(panel, "创作中心会话页面").getTranslationX(), .01f);
        host.onBackPressed();
        assertEquals(RemoteIMTab.MESSAGES, org.robolectric.util.ReflectionHelpers.getField(host, "activeTab"));
    }

    @Test public void remoteCreationShowsItsDeviceAndStaysInTheDrawerUntilCompletion() {
        named(panel, "打开对话列表").performClick(); settle(); layout();
        named(panel, "在我的电脑上新建对话").performClick();
        assertNotNull(OwnedController.createdDevice);
        assertEquals("pc", OwnedController.createdDevice.optString("device_id"));
        assertNotNull(text(panel, "正在我的电脑上新建对话…"));
        assertFalse(named(panel, "在我的电脑上新建对话").isEnabled());
        assertEquals(312f, named(panel, "创作中心会话页面").getTranslationX(), .01f);
        OwnedController.creationCompletion.accept(true); settle();
        assertEquals(0, named(panel, "创作中心会话页面").getTranslationX(), .01f);
    }

    @Test public void readyRemoteImageLoadsOnceAndFailureOffersAnExplicitRetry() throws Exception {
        View row = remoteImageRow("ready", "image-one");
        assertNotNull(text(row, "正在加载图片…"));
        assertEquals(1, OwnedController.imageLoads);
        row = remoteImageRow("ready", "image-one");
        assertEquals("Repeated visible binds must share the same automatic load", 1, OwnedController.imageLoads);
        OwnedController.imageCompletion.accept(null, "图片下载失败，请重试");
        assertNotNull(text(row, "图片下载失败，请重试"));
        View retry = named(row, "会话图片"); assertNotNull(retry); assertTrue(retry.isClickable());
        retry.performClick();
        assertEquals(1, OwnedController.imagePublications);
        assertNotNull(text(row, "正在加载图片…"));
    }

    @Test public void imageRetryUsesTheCardWithoutAnOwnerDeviceReloadPrompt() throws Exception {
        View row = remoteImageRow("ready", "plain-retry");
        OwnedController.imageCompletion.accept(null, "图片下载失败，请重试");
        assertNull("Do not expose a device reload button in the conversation", named(row, "从所属设备重新加载"));
        View card = named(row, "会话图片"); assertNotNull(card);
        assertTrue("Image retry remains available on the image placeholder", card.isClickable());
        card.performClick();
        assertEquals(1, OwnedController.imagePublications);
        assertNotNull(text(row, "正在加载图片…"));
        assertNull(text(row, "正在从所属设备重新加载…"));
    }

    @Test public void visibleQuestionAndItsPersistedUserAnswerHaveOnlyOneAnswerBubble() throws Exception {
        String answer = "Use this text";
        JSONObject owner = new JSONObject().put("id", "remote-one").put("device_slot", "windows")
            .put("device_id", "pc").put("login_session_id", "login").put("online", true);
        JSONObject question = new JSONObject().put("id", "question-part").put("kind", "tool")
            .put("tool", "question").put("state", "completed").put("created", 20)
            .put("input", new JSONObject().put("question", "Which text should I use?").toString()).put("output", answer);
        JSONObject assistant = new JSONObject().put("id", "assistant-one").put("role", "assistant")
            .put("sessionId", "remote-one").put("created", 10).put("completed", 25)
            .put("parts", new JSONArray().put(question));
        JSONObject user = new JSONObject().put("id", "user-one").put("role", "user").put("sessionId", "remote-one")
            .put("created", 30).put("completed", 30).put("parts", new JSONArray().put(new JSONObject()
                .put("id", "user-text").put("kind", "text").put("text", answer)));
        JSONObject data = new JSONObject().put("sessions", new JSONArray().put(owner))
            .put("messages", new JSONArray().put(assistant).put(user));
        panel.onState(new AIAssistantController.State(data, owner, "remote-one", "", "deepseek-flash",
            "on-request", "", "", "", 0, false, true));
        layout();
        assertEquals("The actual visible list must not repeat the answer", 1, visibleTextCount(list(panel), answer));
        assertEquals("The canonical user message keeps its original parts", answer,
            user.getJSONArray("parts").getJSONObject(0).getString("text"));
    }

    private static int visibleTextCount(View view, String value) {
        if (view.getVisibility() != View.VISIBLE) return 0;
        int count = view instanceof TextView && value.contentEquals(((TextView) view).getText()) ? 1 : 0;
        if (view instanceof ViewGroup) for (int i = 0; i < ((ViewGroup) view).getChildCount(); i++)
            count += visibleTextCount(((ViewGroup) view).getChildAt(i), value);
        return count;
    }

    @Test public void completedRemoteImageReusesTheExistingImagePreviewCard() throws Exception {
        java.io.File file = new java.io.File(activity.getCacheDir(), "owned-image.png");
        java.nio.file.Files.write(file.toPath(), java.util.Base64.getDecoder().decode(
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Wl6xXQAAAAASUVORK5CYII="));
        OwnedController.cachedImage = file.getAbsolutePath();
        View row = remoteImageRow("ready", "image-cached");
        View card = named(row, "会话图片"); assertNotNull(card);
        assertTrue("The completed file must create an actual thumbnail", hasImage(card));
        assertEquals(0, OwnedController.imageLoads);
    }

    @Test public void undecodableRemoteImageShowsFailureAndCanBeRetried() throws Exception {
        java.io.File file = new java.io.File(activity.getCacheDir(), "broken-image.png");
        java.nio.file.Files.write(file.toPath(), new byte[]{1, 2, 3});
        OwnedController.cachedImage = file.getAbsolutePath(); OwnedImageLoader.failDecode = true;
        View row = remoteImageRow("ready", "image-broken");
        Shadows.shadowOf(Looper.getMainLooper()).idle();
        assertNotNull(text(row, "图片无法显示，请重试"));
        assertTrue(named(row, "会话图片").isClickable());
        assertNull(named(row, "从所属设备重新加载"));
    }

    @Test public void pendingAndExpiredRemoteImagesHaveVisibleStatusWithoutDownloadLoops() throws Exception {
        assertNotNull(text(remoteImageRow("pending", "image-pending"), "图片同步中…"));
        assertNotNull(text(remoteImageRow("expired", "image-expired"), "图片已过期，未保留本地副本"));
        assertEquals("Retryable receipts only check their completed local cache once", 2, OwnedController.imageLoads);
    }

    @Test public void legacyRemoteImageOffersAnExplicitSinglePartRequestWithoutAutomaticPublication() throws Exception {
        View row = remoteImageRow(null, "legacy-one");
        assertNotNull(text(row, "图片尚未同步到这台设备"));
        View action = named(row, "会话图片"); assertNotNull(action); assertTrue(action.isClickable());
        assertEquals(0, OwnedController.imageLoads); assertEquals(0, OwnedController.imagePublications);
        action.performClick();
        assertEquals(1, OwnedController.imagePublications);
        assertEquals("message-one", OwnedController.publicationMessage);
        assertEquals("legacy-one", OwnedController.publicationPart.optString("id"));
        assertFalse(OwnedController.publicationPart.has("path"));
        assertFalse(OwnedController.publicationPart.has("url"));
    }

    @Test public void publicationInFlightDisablesDuplicateRequestsAndFailureRequiresAnotherClick() throws Exception {
        View row = remoteImageRow("failed", "failed-one");
        View action = named(row, "会话图片"); assertNotNull(action);
        action.performClick(); action.performClick();
        assertEquals(1, OwnedController.imagePublications);
        assertNotNull(text(row, "正在加载图片…"));
        assertFalse(named(row, "会话图片").isClickable());
        OwnedController.publicationCompletion.accept(null, "原设备图片已移动，请检查后重试");
        assertNotNull(text(row, "原设备图片已移动，请检查后重试"));
        View retry = named(row, "会话图片"); assertTrue(retry.isClickable());
        assertEquals(1, OwnedController.imagePublications);
        retry.performClick(); assertEquals(2, OwnedController.imagePublications);
    }

    @Test public void offlineLegacyImageHasNoDevicePromptAndRetryFollowsConnectivity() throws Exception {
        View row = remoteImageRow(null, "offline-one", false, 3);
        View action = named(row, "会话图片"); assertNotNull(action);
        assertFalse(action.isClickable());
        assertNull(named(row, "从所属设备重新加载"));
        assertNull(text(row, "所属设备离线，联网后可重新加载"));
        action.performClick(); assertEquals(0, OwnedController.imagePublications);
        row = remoteImageRow(null, "offline-one", true, 3);
        assertTrue(named(row, "会话图片").isClickable());
    }

    @Test public void completeExpiredCacheRemainsPreviewableWhenTheOwnerIsOffline() throws Exception {
        java.io.File file = new java.io.File(activity.getCacheDir(), "owned-offline-image.png");
        java.nio.file.Files.write(file.toPath(), new byte[]{1, 2, 3});
        OwnedController.cachedImage = file.getAbsolutePath();
        View row = remoteImageRow("expired", "offline-cached", false, 3);
        assertTrue(hasImage(named(row, "会话图片")));
        assertNull(named(row, "从所属设备重新加载"));
        assertEquals(0, OwnedController.imageLoads); assertEquals(0, OwnedController.imagePublications);
    }

    @Test public void offlinePendingAndFailedReceiptsCheckCompleteLocalCopiesWithoutRequestingPublication() throws Exception {
        for (String status : new String[]{"pending", "failed"}) {
            OwnedController.cachedImage = null;
            int before = OwnedController.imageLoads;
            View row = remoteImageRow(status, "offline-cache-" + status, false, 3);
            assertEquals("A retryable receipt must check its local disk copy once", before + 1, OwnedController.imageLoads);
            assertEquals(0, OwnedController.imagePublications);
            java.io.File file = new java.io.File(activity.getCacheDir(), "cached-" + status + ".png");
            java.nio.file.Files.write(file.toPath(), new byte[]{1, 2, 3});
            OwnedController.cachedImage = file.getAbsolutePath();
            OwnedController.imageCompletion.accept(file, "");
            assertTrue(hasImage(named(row, "会话图片")));
            assertNull(named(row, "从所属设备重新加载"));
        }
    }

    @Test public void theLegacyCardSurvivesNewReceiptPublicationWithoutReplacingItsContainer() throws Exception {
        View row = remoteImageRow(null, "stable-one");
        View card = named(row, "会话图片"); assertNotNull(card);
        card.performClick();
        row = remoteImageRow("ready", "stable-one");
        assertSame(card, named(row, "会话图片"));
        assertEquals("The explicit publication already owns the following download", 0, OwnedController.imageLoads);
        assertFalse(card.isClickable());
        OwnedController.publicationCompletion.accept(null, "重新发布失败，请重试");
        assertNotNull(text(row, "重新发布失败，请重试"));
        assertTrue(card.isClickable());
        card.performClick();
        java.io.File file = new java.io.File(activity.getCacheDir(), "republished-image.png");
        java.nio.file.Files.write(file.toPath(), new byte[]{1, 2, 3});
        OwnedController.cachedImage = file.getAbsolutePath();
        OwnedController.publicationCompletion.accept(file, "");
        assertSame(card, named(row, "会话图片"));
        assertTrue(hasImage(card));
        assertNull(named(row, "从所属设备重新加载"));
        assertFalse("Loaded images must drop the placeholder's old retry listener", card.isClickable());
        card.performClick(); assertEquals(2, OwnedController.imagePublications);
    }

    @Test public void legacyUserImageKeepsTheSameSinglePartRecoveryEntry() throws Exception {
        View row = remoteImageRow(null, "legacy-user", true, 3, "user");
        View action = named(row, "会话图片"); assertNotNull(action);
        action.performClick();
        assertEquals(1, OwnedController.imagePublications);
        assertEquals("legacy-user", OwnedController.publicationPart.optString("id"));
        assertEquals("message-one", OwnedController.publicationMessage);
    }

    @Test public void stalePublicationFailureCannotReplaceTheCurrentImageOrChangeTheDrawerAndDraft() throws Exception {
        View row = remoteImageRow(null, "stale-one");
        View action = named(row, "会话图片"); assertNotNull(action); action.performClick();
        java.util.function.BiConsumer<java.io.File, String> old = OwnedController.publicationCompletion;
        row = remoteImageRow(null, "stale-one", true, 4);
        panel.composer.setText("继续保留的草稿");
        named(panel, "打开对话列表").performClick(); settle();
        old.accept(null, "旧请求失败");
        assertNull(text(row, "旧请求失败"));
        assertNotNull(text(row, "图片尚未同步到这台设备"));
        assertEquals("继续保留的草稿", panel.composer.getText().toString());
        assertEquals(312f, named(panel, "创作中心会话页面").getTranslationX(), .01f);
    }

    private View remoteImageRow(String status, String partID) throws Exception {
        return remoteImageRow(status, partID, true, 3);
    }
    private View remoteImageRow(String status, String partID, boolean online, int generation) throws Exception {
        return remoteImageRow(status, partID, online, generation, "assistant");
    }
    private View remoteImageRow(String status, String partID, boolean online, int generation, String role) throws Exception {
        JSONObject owner = new JSONObject().put("id", "remote-one").put("device_slot", "windows")
            .put("device_id", "pc").put("login_session_id", "login").put("online", online);
        JSONObject image = new JSONObject().put("kind", "image").put("id", partID);
        if (status != null) image.put("remoteImage", new JSONObject().put("state", status));
        JSONObject message = new JSONObject().put("id", "message-one").put("role", role)
            .put("parts", new JSONArray().put(image));
        JSONObject data = new JSONObject().put("sessions", new JSONArray().put(owner))
            .put("messages", new JSONArray().put(message)).put("remoteSelectionGeneration", generation);
        panel.onState(new AIAssistantController.State(data, owner, "remote-one", "", "deepseek-flash",
            "on-request", "", "", "", 0, false, true));
        layout();
        ListView messages = list(panel);
        View row = messages.getChildAt(messages.getHeaderViewsCount() - messages.getFirstVisiblePosition());
        assertNotNull("The image fixture must be an actually visible message row", row);
        return row;
    }
    private static ListView list(View view) {
        if (view instanceof ListView) return (ListView) view;
        if (view instanceof ViewGroup) for (int i = 0; i < ((ViewGroup) view).getChildCount(); i++) {
            ListView found = list(((ViewGroup) view).getChildAt(i)); if (found != null) return found;
        }
        return null;
    }
    private static boolean hasImage(View view) {
        if (view instanceof android.widget.ImageView) return true;
        if (view instanceof ViewGroup) for (int i = 0; i < ((ViewGroup) view).getChildCount(); i++)
            if (hasImage(((ViewGroup) view).getChildAt(i))) return true;
        return false;
    }

    private void layout() {
        panel.measure(View.MeasureSpec.makeMeasureSpec(400, View.MeasureSpec.EXACTLY),
            View.MeasureSpec.makeMeasureSpec(800, View.MeasureSpec.EXACTLY));
        panel.layout(0, 0, 400, 800);
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
    private static View text(View view, String value) {
        if (view instanceof TextView && value.contentEquals(((TextView) view).getText())) return view;
        if (view instanceof ViewGroup) for (int i = 0; i < ((ViewGroup) view).getChildCount(); i++) {
            View found = text(((ViewGroup) view).getChildAt(i), value); if (found != null) return found;
        }
        return null;
    }
    @Implements(value = AIAssistantController.class, isInAndroidSdk = false)
    public static class OwnedController {
        static JSONObject createdDevice;
        static java.util.function.Consumer<Boolean> creationCompletion;
        static int imageLoads;
        static String cachedImage;
        static java.util.function.BiConsumer<java.io.File, String> imageCompletion;
        static int imagePublications;
        static String publicationMessage;
        static JSONObject publicationPart;
        static java.util.function.BiConsumer<java.io.File, String> publicationCompletion;
        @Implementation protected void initialize() { /* UI fixture never initializes JNI or models. */ }
        @Implementation protected void setListener(AIAssistantController.Listener listener) { }
        @Implementation protected void clearListener(AIAssistantController.Listener listener) { }
        @Implementation protected boolean modelSettingsReady() { return true; }
        @Implementation protected void createOnDevice(JSONObject device, String workspace,
                java.util.function.Consumer<Boolean> completion) {
            createdDevice = device; creationCompletion = completion;
        }
        @Implementation protected String cachedRemoteImagePath(AIAssistantController.State owner,
                String messageID, JSONObject part) { return cachedImage; }
        @Implementation protected void loadRemoteImage(AIAssistantController.State owner,
                String messageID, JSONObject part, java.util.function.BiConsumer<java.io.File, String> completion) {
            imageLoads++; imageCompletion = completion;
        }
        @Implementation protected void requestRemoteImagePublication(AIAssistantController.State owner,
                String messageID, JSONObject part, java.util.function.BiConsumer<java.io.File, String> completion) {
            imagePublications++; publicationMessage = messageID; publicationPart = part; publicationCompletion = completion;
        }
    }
    @Implements(value = MessageImageLoader.class, isInAndroidSdk = false)
    public static class OwnedImageLoader {
        static boolean failDecode;
        @Implementation protected static void __staticInitializer__() { /* No JNI image decoder in this UI fixture. */ }
        @Implementation protected static void load(String path, int width, int height,
                android.widget.ImageView target, MessageImageLoader.MissingHandler failure) {
            if (failDecode) new android.os.Handler(Looper.getMainLooper()).post(failure::onMissing);
        }
    }
    @Implements(value = MainActivity.class, isInAndroidSdk = false)
    public static class OwnedMainActivity extends org.robolectric.shadows.ShadowActivity {
        @Implementation protected void render() { /* The test exercises host Back routing without SDK startup. */ }
    }
}
