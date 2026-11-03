// 文件作用：在设备实际 native 库上验证头像 JNI、取消句柄及 Android 图像准备。
// 运行边界：不启动 Activity，不读取真实账号；上传参数始终预取消或包含无效 URL，禁止发网络。
package com.kongshang.maichat;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import org.junit.BeforeClass;
import org.junit.Test;
import org.junit.runner.RunWith;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

@RunWith(AndroidJUnit4.class)
public final class AccountProfileInstrumentedTest {
    // 即使预取消发生回归，这个带空格的无效 URL 也会在共享校验和 libcurl 解析阶段被拒绝。
    private static final byte[] INVALID_URL = bytes("invalid avatar upload URL");
    private static final byte[] TOKEN = bytes("synthetic-avatar-smoke-token");
    private static final byte[] ACCOUNT = bytes("avatar_fixture");
    private static final byte[] INVALID_CA = bytes("synthetic-not-a-certificate");
    private static final byte[] JPEG_HEADER = {(byte) 0xff, (byte) 0xd8, (byte) 0xff};

    @BeforeClass public static void loadActualNativeLibrary() {
        System.loadLibrary("maichat_agent");
    }

    private static byte[] bytes(String value) { return value.getBytes(StandardCharsets.UTF_8); }

    private static Method nativeMethod(String name, Class<?>... types) throws Exception {
        Method method = MaiAccountAvatar.class.getDeclaredMethod(name, types);
        method.setAccessible(true);
        return method;
    }

    private static long createCancel() throws Exception {
        long handle = (Long) nativeMethod("nativeCreateCancel").invoke(null);
        assertTrue("Native avatar cancellation handle was not created", handle != 0);
        return handle;
    }

    private static void requestCancel(long handle) throws Exception {
        nativeMethod("nativeRequestCancel", long.class).invoke(null, handle);
    }

    private static void destroyCancel(long handle) throws Exception {
        nativeMethod("nativeDestroyCancel", long.class).invoke(null, handle);
    }

    private static IOException uploadFailure(long handle, byte[] image) throws Exception {
        Method upload = nativeMethod("nativeUpload", byte[].class, byte[].class,
            byte[].class, byte[].class, byte[].class, long.class);
        try {
            upload.invoke(null, INVALID_URL, TOKEN, ACCOUNT, INVALID_CA, image, handle);
        } catch (InvocationTargetException failure) {
            assertTrue("Expected a safe IOException from JNI", failure.getCause() instanceof IOException);
            return (IOException) failure.getCause();
        }
        fail("Invalid or canceled avatar upload was reported as successful");
        throw new AssertionError();
    }

    @Test(timeout = 5000) public void nativePreCanceledUploadReportsCanceledBeforeUrlValidation() throws Exception {
        long handle = createCancel();
        try {
            requestCancel(handle);
            // 直接进入实际 JNI/C ABI，避免 Java Scope 的提前检查掩盖原生取消回归。
            assertEquals("Avatar upload canceled", uploadFailure(handle, JPEG_HEADER).getMessage());
        } finally { destroyCancel(handle); }
    }

    @Test(timeout = 5000) public void nativeInvalidInputsAreRejectedWithoutAnyUsableNetworkUrl() throws Exception {
        assertEquals("Avatar upload input is invalid", uploadFailure(0, JPEG_HEADER).getMessage());
        long handle = createCancel();
        try {
            assertEquals("Avatar upload input is invalid", uploadFailure(handle, new byte[0]).getMessage());
            // 非空 JPEG 头穿过 JNI 输入校验，共享层必须拒绝格式无效的 URL，不触发签名请求。
            assertEquals("Avatar upload was not confirmed", uploadFailure(handle, JPEG_HEADER).getMessage());
            requestCancel(handle);
            assertEquals("Avatar upload canceled", uploadFailure(handle, JPEG_HEADER).getMessage());
        } finally { destroyCancel(handle); }
    }

    @Test(timeout = 10000) public void realBitmapBecomes512JpegAndOriginalBytesStayUnchanged() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        File source = File.createTempFile("account-avatar-smoke-", ".png", context.getCacheDir());
        Bitmap bitmap = Bitmap.createBitmap(1024, 600, Bitmap.Config.ARGB_8888);
        bitmap.eraseColor(Color.TRANSPARENT);
        try {
            try (FileOutputStream output = new FileOutputStream(source)) {
                assertTrue(bitmap.compress(Bitmap.CompressFormat.PNG, 100, output));
            }
            byte[] original = Files.readAllBytes(source.toPath());
            byte[] jpeg = MaiAccountAvatar.prepare(source);
            assertTrue(jpeg.length > 3 && jpeg.length <= 2_000_000);
            assertEquals(0xff, jpeg[0] & 0xff);
            assertEquals(0xd8, jpeg[1] & 0xff);
            Bitmap decoded = BitmapFactory.decodeByteArray(jpeg, 0, jpeg.length);
            try {
                assertNotNull(decoded);
                assertEquals(512, decoded.getWidth());
                assertEquals(512, decoded.getHeight());
                assertEquals(Color.WHITE, decoded.getPixel(256, 256));
            } finally { if (decoded != null) decoded.recycle(); }
            assertArrayEquals(original, Files.readAllBytes(source.toPath()));
        } finally {
            bitmap.recycle();
            Files.deleteIfExists(source.toPath());
        }
    }
}
