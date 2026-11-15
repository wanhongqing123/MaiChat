// 文件作用：用真实创作中心媒体卡片检查图片尺寸与后续正文位置。
// 只替换图像解码交付和 Controller 初始化；普通 View 测量与卡片方法保持产品实现。
package com.kongshang.maichat;

import static org.junit.Assert.*;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.drawable.BitmapDrawable;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.List;
import java.io.File;
import java.lang.reflect.Method;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;
import org.robolectric.annotation.Implementation;
import org.robolectric.annotation.Implements;

@RunWith(RobolectricTestRunner.class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
@Config(sdk = 34, qualifiers = "w400dp-h800dp-mdpi", shadows = {
    AIAssistantSessionDrawerTest.OwnedController.class,
    AIAssistantMediaCardLayoutTest.OwnedImages.class})
public final class AIAssistantMediaCardLayoutTest {
    private Activity activity;
    private AIAssistantController controller;
    private AIAssistantPanel panel;
    private LinearLayout root, card;
    private TextView following;
    private int availableWidth = 400;

    @Before public void setup() throws Exception {
        activity = Robolectric.buildActivity(Activity.class).setup().get();
        controller = new AIAssistantController(activity, "http://owned.invalid");
        panel = new AIAssistantPanel(activity, controller);
        root = new LinearLayout(activity);
        root.setOrientation(LinearLayout.VERTICAL);
        card = new LinearLayout(activity);
        card.setOrientation(LinearLayout.VERTICAL);
        root.addView(card, new LinearLayout.LayoutParams(-1, -2));
        following = new TextView(activity);
        following.setText("Owned following reply");
        root.addView(following, new LinearLayout.LayoutParams(-1, -2));
        activity.setContentView(root);
        OwnedImages.immediate = null;
        OwnedImages.target = null;
    }

    @After public void close() {
        panel.setForeground(false);
        controller.close();
        OwnedImages.immediate = null;
        OwnedImages.target = null;
    }

    private void render(String type) throws Exception {
        Method method = AIAssistantPanel.class.getDeclaredMethod("showAgentMediaCard",
            LinearLayout.class, File.class, String.class, String.class);
        method.setAccessible(true);
        method.invoke(panel, card, new File(activity.getCacheDir(), "owned-card.png"), type, "");
        layout();
    }

    private void layout() {
        root.measure(View.MeasureSpec.makeMeasureSpec(availableWidth, View.MeasureSpec.EXACTLY),
            View.MeasureSpec.makeMeasureSpec(2000, View.MeasureSpec.AT_MOST));
        root.layout(0, 0, availableWidth, root.getMeasuredHeight());
    }

    @Test public void landscapeScreenshotDoesNotReserveBlankHeightBeforeTheFollowingText() throws Exception {
        Bitmap screenshot = Bitmap.createBitmap(1600, 1000, Bitmap.Config.ARGB_8888);
        OwnedImages.immediate = screenshot;
        render("image");
        ViewGroup preview = (ViewGroup) card.getChildAt(0);
        assertEquals(220, preview.getWidth());
        assertEquals("A 16:10 screenshot must not reserve the old fixed 240dp frame", 138, preview.getHeight());
        assertEquals(138, following.getTop());
        assertSame(screenshot, ((BitmapDrawable) OwnedImages.target.getDrawable()).getBitmap());
        assertEquals(ImageView.ScaleType.FIT_CENTER, OwnedImages.target.getScaleType());
        assertTrue(preview.isClickable());
    }

    @Test public void portraitSquareAndWideImagesKeepTheirOwnAspectRatio() throws Exception {
        int[][] cases = {{1600, 900, 220, 124}, {100, 400, 60, 240},
            {100, 100, 220, 220}, {400, 100, 220, 55}};
        for (int[] item : cases) {
            OwnedImages.immediate = Bitmap.createBitmap(item[0], item[1], Bitmap.Config.ARGB_8888);
            render("image");
            assertEquals(item[2], card.getChildAt(0).getWidth());
            assertEquals(item[3], card.getChildAt(0).getHeight());
            assertEquals(item[3], following.getTop());
        }
    }

    @Test public void asynchronousImageDeliveryMeasuresTheSameViewsWithoutLeavingPlaceholderSpace() throws Exception {
        render("image");
        View preview = card.getChildAt(0);
        ImageView image = OwnedImages.target;
        Bitmap screenshot = Bitmap.createBitmap(1600, 1000, Bitmap.Config.ARGB_8888);
        image.setImageBitmap(screenshot);
        layout();
        assertSame(preview, card.getChildAt(0));
        assertSame(image, ((ViewGroup) preview).getChildAt(0));
        assertEquals(138, preview.getHeight());
        assertEquals(138, following.getTop());
        assertSame(screenshot, ((BitmapDrawable) image.getDrawable()).getBitmap());
    }

    @Test public void narrowAndExpandedParentsMeasureTheSameImageWithoutClippingOrBlankHeight() throws Exception {
        Bitmap screenshot = Bitmap.createBitmap(1600, 1000, Bitmap.Config.ARGB_8888);
        OwnedImages.immediate = screenshot;
        render("image");
        ViewGroup preview = (ViewGroup) card.getChildAt(0);
        ImageView image = OwnedImages.target;
        for (int width : new int[] {120, 400, 0, 90, 400}) {
            availableWidth = width;
            layout();
            int expectedWidth = Math.min(220, width);
            int expectedHeight = Math.round(expectedWidth * 1000f / 1600);
            assertEquals(expectedWidth, preview.getWidth());
            assertEquals(expectedWidth, image.getWidth());
            assertEquals(expectedHeight, preview.getHeight());
            assertEquals(expectedHeight, following.getTop());
            assertSame(preview, card.getChildAt(0));
            assertSame(image, preview.getChildAt(0));
            assertSame(screenshot, ((BitmapDrawable) image.getDrawable()).getBitmap());
        }
    }

    @Test public void videoKeepsItsExistingFallbackFrameAndPlaybackTarget() throws Exception {
        render("video");
        View preview = card.getChildAt(0);
        assertEquals(280, preview.getWidth());
        assertEquals(181, preview.getHeight());
        assertTrue(preview.isClickable());
        assertTrue(((FrameLayout) preview).getChildCount() > 1);
    }

    @Test public void adjacentAssistantImagesAndReplyUseOneIOS14PointGap() throws Exception {
        List<View> images = assistantImages(false, false);
        assertEquals(2, images.size());
        View after = described(panel, "Owned after images");
        assertNotNull(after);
        assertSame(images.get(0).getParent(), images.get(1).getParent());
        assertEquals(14, images.get(1).getTop() - images.get(0).getBottom());
        assertEquals(14, after.getTop() - images.get(1).getBottom());
        assertEquals("Spacing belongs outside the measured image", 138, images.get(0).getHeight());
    }

    @Test public void textBeforeAnImageDoesNotStackItsExistingMarginWithTheIOSGap() throws Exception {
        List<View> images = assistantImages(true, false);
        View before = described(panel, "Owned before images");
        View after = described(panel, "Owned after images");
        assertNotNull(before); assertNotNull(after);
        assertEquals(14, images.get(0).getTop() - before.getBottom());
        assertEquals(14, images.get(1).getTop() - images.get(0).getBottom());
        assertEquals(14, after.getTop() - images.get(1).getBottom());
    }

    @Test public void aLastImageDoesNotAddAnotherGapOutsideTheMessage() throws Exception {
        List<View> images = assistantImages(false, true);
        assertEquals(14, images.get(1).getTop() - images.get(0).getBottom());
        assertEquals(0, ((LinearLayout.LayoutParams) images.get(1).getLayoutParams()).bottomMargin);
    }

    @Test public void anImageArtifactCaptionUsesIOS8PointInternalSpacing() throws Exception {
        OwnedImages.immediate = Bitmap.createBitmap(1600, 1000, Bitmap.Config.ARGB_8888);
        Method method = AIAssistantPanel.class.getDeclaredMethod("showAgentMediaCard",
            LinearLayout.class, File.class, String.class, String.class);
        method.setAccessible(true);
        method.invoke(panel, card, new File(activity.getCacheDir(), "owned-caption.png"), "image", "Owned image caption");
        layout();
        assertEquals(8, card.getChildAt(1).getTop() - card.getChildAt(0).getBottom());
    }

    private List<View> assistantImages(boolean leadingText, boolean lastImage) throws Exception {
        OwnedImages.immediate = Bitmap.createBitmap(1600, 1000, Bitmap.Config.ARGB_8888);
        AIAssistantSessionDrawerTest.OwnedController.cachedImage =
            new File(activity.getCacheDir(), "owned-remote-image.png").getAbsolutePath();
        JSONObject owner = new JSONObject().put("id", "owned-room").put("device_slot", "windows")
            .put("device_id", "owned-device").put("login_session_id", "owned-login").put("online", true);
        JSONArray parts = new JSONArray();
        if (leadingText) parts.put(new JSONObject().put("id", "before").put("kind", "text").put("text", "Owned before images"));
        for (int index = 0; index < 2; index++) parts.put(new JSONObject().put("id", "image-" + index)
            .put("kind", "image").put("remoteImage", new JSONObject().put("state", "ready")));
        if (!lastImage) parts.put(new JSONObject().put("id", "after").put("kind", "text").put("text", "Owned after images"));
        JSONObject message = new JSONObject().put("id", "owned-message").put("role", "assistant")
            .put("created", 1).put("completed", 2).put("fragmentContinues", lastImage).put("parts", parts);
        JSONObject data = new JSONObject().put("sessions", new JSONArray().put(owner))
            .put("messages", new JSONArray().put(message)).put("remoteSelectionGeneration", 3);
        activity.setContentView(panel);
        panel.onState(new AIAssistantController.State(data, owner, "owned-room", "", "deepseek-flash",
            "on-request", "", "", "", 0, false, true));
        panel.measure(View.MeasureSpec.makeMeasureSpec(400, View.MeasureSpec.EXACTLY),
            View.MeasureSpec.makeMeasureSpec(800, View.MeasureSpec.EXACTLY));
        panel.layout(0, 0, 400, 800);
        List<View> result = new ArrayList<>();
        collectImages(panel, result);
        return result;
    }

    private static void collectImages(View view, List<View> result) {
        if ("会话图片".contentEquals(view.getContentDescription() == null ? "" : view.getContentDescription())) result.add(view);
        else if (view instanceof ViewGroup) for (int index = 0; index < ((ViewGroup) view).getChildCount(); index++)
            collectImages(((ViewGroup) view).getChildAt(index), result);
    }
    private static View described(View view, String text) {
        if (text.contentEquals(view.getContentDescription() == null ? "" : view.getContentDescription())) return view;
        if (view instanceof ViewGroup) for (int index = 0; index < ((ViewGroup) view).getChildCount(); index++) {
            View result = described(((ViewGroup) view).getChildAt(index), text);
            if (result != null) return result;
        }
        return null;
    }

    @Implements(value = MessageImageLoader.class, isInAndroidSdk = false)
    public static final class OwnedImages {
        static Bitmap immediate;
        static ImageView target;
        @Implementation protected static void __staticInitializer__() { }
        @Implementation protected static void load(String path, int width, int height,
                ImageView view, MessageImageLoader.MissingHandler missing) {
            target = view;
            if (immediate != null) view.setImageBitmap(immediate);
        }
    }
}
