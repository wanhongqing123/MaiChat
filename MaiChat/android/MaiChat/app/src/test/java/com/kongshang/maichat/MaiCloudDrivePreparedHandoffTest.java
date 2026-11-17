package com.kongshang.maichat;

import static org.junit.Assert.*;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.os.Looper;
import androidx.test.core.app.ApplicationProvider;
import java.io.File;
import java.io.FileOutputStream;
import java.lang.reflect.Constructor;
import java.nio.charset.StandardCharsets;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.Shadows;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;
import org.robolectric.annotation.Implementation;
import org.robolectric.annotation.Implements;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, shadows = {MaiCloudDrivePreparedHandoffTest.OwnedAuth.class,
    MaiCloudDrivePreparedHandoffTest.OwnedNativeLibrary.class,
    MaiCloudDrivePreparedHandoffTest.OwnedNativeCalls.class})
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public final class MaiCloudDrivePreparedHandoffTest {
    @Implements(value = MaiChatAuthClient.class, isInAndroidSdk = false)
    public static class OwnedAuth {
        @Implementation protected String accountId() { return "owned-handoff"; }
        @Implementation protected MaiChatAuthClient.CloudDriveAccess cloudDriveAccess() {
            return new MaiChatAuthClient.CloudDriveAccess("https://owned.invalid", "owned-local-token");
        }
    }
    @Implements(className = "com.kongshang.maichat.MaiCloudDrive$NativeLibrary", isInAndroidSdk = false)
    public static class OwnedNativeLibrary {
        @Implementation protected static void __staticInitializer__() { }
        @Implementation protected static void ensureLoaded() { }
    }
    @Implements(value = MaiCloudDrive.class, isInAndroidSdk = false, looseSignatures = true)
    public static class OwnedNativeCalls {
        static final AtomicReference<String> nativeSource = new AtomicReference<>();
        static final AtomicReference<String> nativeThumbnail = new AtomicReference<>();
        @Implementation protected byte[] trustPem() { return new byte[0]; }
        @Implementation protected static long nativeCreateCancel() { return 123; }
        @Implementation protected static void nativeDestroyCancel(long handle) { }
        @Implementation protected static void nativeRequestCancel(long handle) { }
        @Implementation protected static MaiCloudDrive.Result nativeUploadFile(Object base, Object token,
                Object pem, Object suffix, Object source, Object checkpoint, Object name, Object type,
                Object thumbnail, Object cancel, Object progress) {
            nativeSource.set((String) source);
            nativeThumbnail.set((String) thumbnail);
            return new MaiCloudDrive.Result(0, 200, 0, "{}".getBytes(StandardCharsets.UTF_8));
        }
    }

    @Test public void preparedPairIsTransferredOnceWithoutCopyingOrDecodingAgain() throws Exception {
        Context context = ApplicationProvider.getApplicationContext();
        MaiCloudDrive client = new MaiCloudDrive(context);
        File checkpoint = new File(client.accountDirectory(), UUID.randomUUID() + ".maicloud.json");
        File snapshot = new File(checkpoint + ".source");
        Bitmap pixels = Bitmap.createBitmap(96, 64, Bitmap.Config.ARGB_8888);
        pixels.eraseColor(0xff2d90db);
        try (FileOutputStream stream = new FileOutputStream(snapshot)) {
            assertTrue(pixels.compress(Bitmap.CompressFormat.PNG, 100, stream));
        } finally { pixels.recycle(); }
        MaiCloudDriveThumbnail.Lease lease = MaiCloudDriveThumbnail.borrow(checkpoint);
        File thumbnail = MaiCloudDriveThumbnail.prepareFileWithDecoder(checkpoint, snapshot,
            "original.png", () -> false, (path, width, height) -> BitmapFactory.decodeFile(path));
        Class<?> preparedType = Class.forName("com.kongshang.maichat.MaiCloudDrive$PreparedImage");
        Constructor<?> preparedConstructor = preparedType.getDeclaredConstructor(File.class, File.class,
            File.class, String.class, MaiCloudDriveThumbnail.Lease.class);
        preparedConstructor.setAccessible(true);
        Object pair = preparedConstructor.newInstance(checkpoint, snapshot, thumbnail, "owned-handoff", lease);
        Constructor<MaiCloudDrive.Source> sourceConstructor = MaiCloudDrive.Source.class.getDeclaredConstructor(
            android.net.Uri.class, File.class, String.class, String.class, long.class, preparedType);
        sourceConstructor.setAccessible(true);
        File noLongerNeededOriginal = new File(context.getCacheDir(), "original-no-longer-present.png");
        MaiCloudDrive.Source source = sourceConstructor.newInstance(null, noLongerNeededOriginal,
            "original.png", "image/png", snapshot.length(), pair);
        OwnedNativeCalls.nativeSource.set(null);
        OwnedNativeCalls.nativeThumbnail.set(null);
        AtomicReference<MaiCloudDrive.Result> completed = new AtomicReference<>();
        client.upload(source, checkpoint, (done, total, preparing) -> { }, completed::set);
        long deadline = System.currentTimeMillis() + 5000;
        while (completed.get() == null && System.currentTimeMillis() < deadline) {
            Shadows.shadowOf(Looper.getMainLooper()).idle();
            Thread.sleep(10);
        }
        assertNotNull("Prepared upload must settle without requiring the removed original", completed.get());
        assertTrue("status=" + completed.get().status + " nativeSource=" + OwnedNativeCalls.nativeSource.get(),
            completed.get().ok());
        assertEquals(snapshot.getAbsolutePath(), OwnedNativeCalls.nativeSource.get());
        assertEquals(thumbnail.getAbsolutePath(), OwnedNativeCalls.nativeThumbnail.get());
        assertFalse(snapshot.exists());
        assertFalse(thumbnail.exists());
        assertFalse(new File(checkpoint + ".thumbnail.meta").exists());
    }
}
