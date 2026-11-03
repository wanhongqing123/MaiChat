// 文件作用：通过实际 Android 图像编解码验证头像规范化及输入上限；不上传、不修改账号。
package com.kongshang.maichat;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.file.Files;

import static org.junit.Assert.*;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class MaiAccountAvatarTest {
    @Test public void normalizesTransparentImageTo512JpegWithoutChangingSource() throws Exception {
        File source = Files.createTempFile("maichat-avatar", ".png").toFile();
        Bitmap bitmap = Bitmap.createBitmap(1024, 512, Bitmap.Config.ARGB_8888);
        bitmap.eraseColor(Color.TRANSPARENT);
        try (FileOutputStream output = new FileOutputStream(source)) {
            assertTrue(bitmap.compress(Bitmap.CompressFormat.PNG, 100, output));
        } finally { bitmap.recycle(); }
        byte[] original = Files.readAllBytes(source.toPath());
        byte[] jpeg = MaiAccountAvatar.prepare(source);
        assertTrue(jpeg.length > 0 && jpeg.length <= 2_000_000);
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
    }

    @Test public void rejectsOversizedAndUndecodableInputsBeforeUpload() throws Exception {
        File oversized = Files.createTempFile("maichat-avatar-large", ".png").toFile();
        try (RandomAccessFile file = new RandomAccessFile(oversized, "rw")) {
            file.setLength(20L * 1024 * 1024 + 1);
        }
        assertThrows(IOException.class, () -> MaiAccountAvatar.prepare(oversized));
        File corrupt = Files.createTempFile("maichat-avatar-invalid", ".jpg").toFile();
        Files.write(corrupt.toPath(), new byte[]{1, 2, 3, 4});
        assertThrows(IOException.class, () -> MaiAccountAvatar.prepare(corrupt));
    }
}
