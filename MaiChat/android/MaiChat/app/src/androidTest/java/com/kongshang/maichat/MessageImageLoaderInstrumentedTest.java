// 文件作用：在实际 native 解码器验证成功回调及迟到请求隔离；只使用合成图片，不访问账号或网络。
package com.kongshang.maichat;

import static org.junit.Assert.*;

import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.drawable.BitmapDrawable;
import android.os.Looper;
import android.widget.ImageView;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.io.FileOutputStream;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class MessageImageLoaderInstrumentedTest {
    @Test public void nativeDecodedBitmapIsDeliveredUncroppedOnMainToTheCurrentTarget() throws Exception {
        android.app.Instrumentation instrumentation = InstrumentationRegistry.getInstrumentation();
        File file = File.createTempFile("message-image-loader-", ".png", instrumentation.getTargetContext().getCacheDir());
        Bitmap source = Bitmap.createBitmap(80, 40, Bitmap.Config.ARGB_8888);
        source.eraseColor(Color.BLUE);
        try {
            try (FileOutputStream output = new FileOutputStream(file)) {
                assertTrue(source.compress(Bitmap.CompressFormat.PNG, 100, output));
            }
            CountDownLatch done = new CountDownLatch(1);
            AtomicReference<Bitmap> loaded = new AtomicReference<>();
            AtomicReference<ImageView> target = new AtomicReference<>();
            AtomicBoolean onMain = new AtomicBoolean(), missing = new AtomicBoolean();
            instrumentation.runOnMainSync(() -> {
                target.set(new ImageView(instrumentation.getTargetContext()));
                MessageImageLoader.load(file.getPath(), 220, 180, target.get(),
                    () -> { missing.set(true); done.countDown(); }, bitmap -> {
                        loaded.set(bitmap); onMain.set(Looper.myLooper() == Looper.getMainLooper()); done.countDown();
                    });
            });
            assertTrue(done.await(8, TimeUnit.SECONDS));
            assertFalse(missing.get());
            assertTrue(onMain.get());
            assertNotNull(loaded.get());
            assertEquals(2.0, (double) loaded.get().getWidth() / loaded.get().getHeight(), 0.02);
            instrumentation.runOnMainSync(() -> assertSame(loaded.get(),
                ((BitmapDrawable) target.get().getDrawable()).getBitmap()));
        } finally {
            source.recycle();
            assertTrue(file.delete() || !file.exists());
        }
    }

    @Test public void queuedSuccessNotifiesOnlyTheStillCurrentRequest() throws Exception {
        android.app.Instrumentation instrumentation = InstrumentationRegistry.getInstrumentation();
        File first = imageFixture(instrumentation.getTargetContext(), Color.RED);
        File second = imageFixture(instrumentation.getTargetContext(), Color.BLUE);
        AtomicInteger oldCalls = new AtomicInteger(), newCalls = new AtomicInteger();
        AtomicReference<ImageView> target = new AtomicReference<>();
        AtomicReference<Bitmap> loaded = new AtomicReference<>();
        AtomicBoolean missing = new AtomicBoolean();
        CountDownLatch done = new CountDownLatch(1);
        try {
            instrumentation.runOnMainSync(() -> {
                target.set(new ImageView(instrumentation.getTargetContext()));
                // Both requests bind before MAIN may consume either queued result.
                MessageImageLoader.load(first.getPath(), 220, 180, target.get(), null,
                    bitmap -> oldCalls.incrementAndGet());
                MessageImageLoader.load(second.getPath(), 220, 180, target.get(),
                    () -> { missing.set(true); done.countDown(); }, bitmap -> {
                        loaded.set(bitmap); newCalls.incrementAndGet(); done.countDown();
                    });
            });
            assertTrue(done.await(8, TimeUnit.SECONDS));
            instrumentation.waitForIdleSync();
            assertFalse(missing.get());
            assertEquals(0, oldCalls.get());
            assertEquals(1, newCalls.get());
            assertNotNull(loaded.get());
            assertEquals(Color.BLUE, loaded.get().getPixel(0, 0));
            instrumentation.runOnMainSync(() -> assertSame(loaded.get(),
                ((BitmapDrawable) target.get().getDrawable()).getBitmap()));
        } finally {
            assertTrue(first.delete() || !first.exists());
            assertTrue(second.delete() || !second.exists());
        }
    }

    private static File imageFixture(android.content.Context context, int color) throws Exception {
        File file = File.createTempFile("message-image-rebind-", ".png", context.getCacheDir());
        Bitmap bitmap = Bitmap.createBitmap(80, 40, Bitmap.Config.ARGB_8888);
        bitmap.eraseColor(color);
        try (FileOutputStream output = new FileOutputStream(file)) {
            assertTrue(bitmap.compress(Bitmap.CompressFormat.PNG, 100, output));
        } finally { bitmap.recycle(); }
        return file;
    }
}
