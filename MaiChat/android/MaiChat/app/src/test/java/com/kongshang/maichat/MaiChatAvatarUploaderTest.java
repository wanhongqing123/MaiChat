// 文件作用：本地假签发/上传验证头像归一化、签名地址约束和切号取消，不连接任何网络。
package com.kongshang.maichat;

import static org.junit.Assert.*;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.media.ExifInterface;
import android.net.Uri;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, manifest = Config.NONE)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class MaiChatAvatarUploaderTest {
    private static final String PATH = "/avatars/fixture/2026-11-03/123e4567-e89b-42d3-a456-426614174000.jpg";
    private static final String AVATAR = "https://ichat.life/maichat" + PATH;

    private File picture(int width, int height) throws Exception {
        File file = File.createTempFile("avatar-fixture-", ".jpg");
        Bitmap bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);
        canvas.drawColor(Color.RED);
        android.graphics.Paint paint = new android.graphics.Paint(); paint.setColor(Color.BLUE);
        canvas.drawRect(width / 4f, 0, width * 3 / 4f, height, paint);
        try (FileOutputStream output = new FileOutputStream(file)) { bitmap.compress(Bitmap.CompressFormat.JPEG, 100, output); }
        bitmap.recycle();
        return file;
    }

    private JSONObject signed() throws Exception {
        return new JSONObject().put("upload_url", "https://fixture.oss-cn-hangzhou.aliyuncs.com" + PATH + "?signature=fixture")
            .put("avatar_url", AVATAR).put("upload_headers", new JSONObject().put("Content-Type", "image/jpeg"));
    }

    private MaiChatAvatarUploader.Response response(int status, JSONObject body) {
        return new MaiChatAvatarUploader.Response(status,
            body == null ? new byte[0] : body.toString().getBytes(StandardCharsets.UTF_8));
    }

    private MaiChatAuthClient.MediaServiceAccess access() {
        return new MaiChatAuthClient.MediaServiceAccess("https://ichat.life/maichat", "fixture-account-token");
    }

    @Test public void normalizesLargeLandscapeTo512SquareWithoutDistortingCenter() throws Exception {
        File source = picture(4096, 2048);
        try {
            byte[] data = MaiChatAvatarImage.encode(source);
            Bitmap result = BitmapFactory.decodeByteArray(data, 0, data.length);
            assertEquals(512, result.getWidth()); assertEquals(512, result.getHeight());
            assertTrue(data.length > 0 && data.length <= 2_000_000);
            assertTrue(Color.blue(result.getPixel(20, 20)) > 220);
            assertTrue(Color.blue(result.getPixel(490, 490)) > 220);
            result.recycle();
        } finally { source.delete(); }
    }

    @Test public void appliesExifRotationBeforeSquareCrop() throws Exception {
        File source = picture(100, 50);
        try {
            Bitmap original = Bitmap.createBitmap(100, 50, Bitmap.Config.ARGB_8888);
            Canvas canvas = new Canvas(original); canvas.drawColor(Color.RED);
            android.graphics.Paint paint = new android.graphics.Paint(); paint.setColor(Color.BLUE);
            canvas.drawRect(0, 25, 100, 50, paint);
            try (FileOutputStream output = new FileOutputStream(source)) { original.compress(Bitmap.CompressFormat.JPEG, 100, output); }
            original.recycle();
            ExifInterface exif = new ExifInterface(source.getAbsolutePath());
            exif.setAttribute(ExifInterface.TAG_ORIENTATION, Integer.toString(ExifInterface.ORIENTATION_ROTATE_90));
            exif.saveAttributes();
            byte[] data = MaiChatAvatarImage.encode(source);
            Bitmap result = BitmapFactory.decodeByteArray(data, 0, data.length);
            // 顺时针旋转把原图下半部蓝色移到左侧，而非忽略 EXIF 仍保留上下分色。
            assertTrue(Color.blue(result.getPixel(20, 256)) > 220);
            assertTrue(Color.red(result.getPixel(490, 256)) > 220);
            result.recycle();
        } finally { source.delete(); }
    }

    @Test public void selectedUriProducesTemporaryJpegThroughTheSameImageCore() throws Exception {
        File source = picture(1200, 600);
        File prepared = null;
        try {
            prepared = MaiChatAvatarUploader.prepareSelectedImage(RuntimeEnvironment.getApplication(), Uri.fromFile(source));
            assertTrue(prepared.getName().endsWith(".jpg"));
            Bitmap result = BitmapFactory.decodeFile(prepared.getAbsolutePath());
            assertEquals(512, result.getWidth()); assertEquals(512, result.getHeight());
            result.recycle();
        } finally { source.delete(); if (prepared != null) prepared.delete(); }
    }

    @Test public void signsThenPutsNormalizedImageWithoutForwardingAccountBearerToOss() throws Exception {
        File source = picture(1000, 500);
        AtomicInteger calls = new AtomicInteger();
        try {
            String result = MaiChatAvatarUploader.upload(source, access(), () -> true,
                (url, method, body, bearer, contentType, current) -> {
                    if (calls.getAndIncrement() == 0) {
                        assertEquals("https://ichat.life/maichat/sign-upload", url);
                        assertEquals("POST", method); assertEquals("fixture-account-token", bearer);
                        JSONObject payload = new JSONObject(new String(body, StandardCharsets.UTF_8));
                        assertEquals("im_avatar", payload.getString("purpose"));
                        assertTrue(payload.getString("filename").endsWith(".jpg"));
                        return response(200, signed());
                    }
                    assertEquals("PUT", method); assertEquals("", bearer);
                    assertEquals("image/jpeg", contentType);
                    Bitmap uploaded = BitmapFactory.decodeByteArray(body, 0, body.length);
                    assertEquals(512, uploaded.getWidth()); uploaded.recycle();
                    return response(204, null);
                });
            assertEquals(AVATAR, result); assertEquals(2, calls.get());
        } finally { source.delete(); }
    }

    @Test public void rejectsWrongUploadHostAndMismatchedStablePathBeforePut() throws Exception {
        File source = picture(40, 40);
        try {
            for (String field : new String[] {"upload_url", "avatar_url"}) {
                AtomicInteger calls = new AtomicInteger();
                JSONObject bad = signed().put(field, field.equals("upload_url")
                    ? "https://aliyuncs.com.attacker.test" + PATH : AVATAR.replace("fixture/", "another/"));
                try {
                    MaiChatAvatarUploader.upload(source, access(), () -> true,
                        (url, method, body, bearer, contentType, current) -> {
                            calls.incrementAndGet(); return response(200, bad);
                        });
                    fail("Mismatched avatar signing response was accepted");
                } catch (IOException expected) { assertEquals(1, calls.get()); }
            }
        } finally { source.delete(); }
    }

    @Test public void accountChangeAfterSigningPreventsPutAndLatePutCannotReturnSuccess() throws Exception {
        File source = picture(40, 40);
        try {
            for (int staleStage : new int[] {1, 2}) {
                AtomicBoolean current = new AtomicBoolean(true); AtomicInteger calls = new AtomicInteger();
                try {
                    MaiChatAvatarUploader.upload(source, access(), current::get,
                        (url, method, body, bearer, contentType, valid) -> {
                            int stage = calls.incrementAndGet();
                            if (stage == staleStage) current.set(false);
                            return stage == 1 ? response(200, signed()) : response(200, null);
                        });
                    fail("A previous account upload returned success");
                } catch (IOException expected) { assertEquals(staleStage, calls.get()); }
            }
        } finally { source.delete(); }
    }
}
