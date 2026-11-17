package com.kongshang.maichat;

import static org.junit.Assert.*;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.lang.reflect.InvocationTargetException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.function.BooleanSupplier;
import org.json.JSONObject;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public final class MaiCloudDriveThumbnailTest {
    @Rule public final TemporaryFolder directory = new TemporaryFolder();

    private File prepare(File checkpoint, File source, BooleanSupplier canceled) throws Exception {
        return MaiCloudDriveThumbnail.prepareFileWithDecoder(checkpoint, source,
            source.getName(), canceled, (path, width, height) -> BitmapFactory.decodeFile(path));
    }

    private void cleanup(File checkpoint) throws Exception {
        Class.forName("com.kongshang.maichat.MaiCloudDriveThumbnail")
            .getDeclaredMethod("cleanupIfNoCheckpoint", File.class).invoke(null, checkpoint);
    }

    private File source() throws Exception {
        File source = directory.newFile("owned.png");
        Bitmap image = Bitmap.createBitmap(960, 360, Bitmap.Config.ARGB_8888);
        image.eraseColor(0xff2d90db);
        try (FileOutputStream output = new FileOutputStream(source)) {
            assertTrue(image.compress(Bitmap.CompressFormat.PNG, 100, output));
        } finally { image.recycle(); }
        return source;
    }

    private static String sha(File file) throws Exception {
        byte[] digest = MessageDigest.getInstance("SHA-256").digest(Files.readAllBytes(file.toPath()));
        StringBuilder text = new StringBuilder();
        for (byte value : digest) text.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
        return text.toString();
    }

    private static void paused(File checkpoint) throws Exception {
        JSONObject identity = new JSONObject(new String(Files.readAllBytes(
            new File(checkpoint + ".thumbnail.meta").toPath()), StandardCharsets.UTF_8));
        JSONObject record = new JSONObject().put("version", 1)
            .put("name", identity.getString("name")).put("source_id", identity.getString("source_id"))
            .put("file_size", identity.getLong("source_bytes"))
            .put("file_sha256", identity.getString("source_sha256"))
            .put("thumbnail", new JSONObject().put("size_bytes", identity.getLong("thumbnail_bytes"))
                .put("file_sha256", identity.getString("thumbnail_sha256")));
        Files.write(checkpoint.toPath(), record.toString().getBytes(StandardCharsets.UTF_8));
    }

    @Test public void ordinaryImageMakesBoundedJpegWithoutChangingOriginal() throws Exception {
        File source = source(), checkpoint = new File(directory.getRoot(), "owned.maicloud.json");
        String original = sha(source);
        File thumbnail = prepare(checkpoint, source, () -> false);
        assertNotNull(thumbnail);
        assertEquals(new File(checkpoint + ".thumbnail.jpg").getCanonicalFile(), thumbnail.getCanonicalFile());
        assertTrue(thumbnail.length() > 0 && thumbnail.length() <= 2_000_000L);
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeFile(thumbnail.getAbsolutePath(), bounds);
        assertEquals("image/jpeg", bounds.outMimeType);
        assertEquals(320, bounds.outWidth);
        assertEquals(120, bounds.outHeight);
        JSONObject metadata = new JSONObject(new String(Files.readAllBytes(
            new File(checkpoint + ".thumbnail.meta").toPath()), StandardCharsets.UTF_8));
        assertEquals(original, metadata.getString("source_sha256"));
        assertEquals(sha(thumbnail), metadata.getString("thumbnail_sha256"));
        assertEquals(source.getName(), metadata.getString("name"));
        assertEquals(source.getCanonicalPath(), metadata.getString("source_id"));
        assertEquals(original, sha(source));
    }

    @Test public void pausedUploadReusesExactThumbnailAndRejectsChangedOriginal() throws Exception {
        File source = source(), checkpoint = new File(directory.getRoot(), "owned.maicloud.json");
        File thumbnail = prepare(checkpoint, source, () -> false);
        paused(checkpoint);
        String before = sha(thumbnail);
        long modified = thumbnail.lastModified();
        assertEquals(thumbnail, prepare(checkpoint, source, () -> false));
        assertEquals(before, sha(thumbnail));
        assertEquals(modified, thumbnail.lastModified());
        Files.write(source.toPath(), "changed original".getBytes(StandardCharsets.UTF_8));
        try { prepare(checkpoint, source, () -> false); fail("Changed source cannot borrow an old thumbnail"); }
        catch (IOException expected) { }
        assertEquals(before, sha(thumbnail));
    }

    @Test public void transparentOriginalIsWhiteCompositedOnlyInTheDerivedJpeg() throws Exception {
        File source = directory.newFile("transparent.png");
        Bitmap image = Bitmap.createBitmap(64, 64, Bitmap.Config.ARGB_8888);
        image.eraseColor(Color.TRANSPARENT);
        Paint paint = new Paint();
        paint.setColor(0xff2d90db);
        new Canvas(image).drawRect(20, 20, 44, 44, paint);
        try (FileOutputStream output = new FileOutputStream(source)) {
            assertTrue(image.compress(Bitmap.CompressFormat.PNG, 100, output));
        } finally { image.recycle(); }
        String original = sha(source);
        File thumbnail = prepare(new File(directory.getRoot(), "alpha.maicloud.json"), source, () -> false);
        Bitmap jpeg = BitmapFactory.decodeFile(thumbnail.getAbsolutePath());
        int corner = jpeg.getPixel(0, 0);
        jpeg.recycle();
        assertTrue("Transparent pixels must become white, not black", Color.red(corner) > 235 &&
            Color.green(corner) > 235 && Color.blue(corner) > 235);
        assertEquals(original, sha(source));
    }

    @Test public void legacyCheckpointDoesNotAcquireAThumbnailMidUpload() throws Exception {
        File source = source(), checkpoint = new File(directory.getRoot(), "owned.maicloud.json");
        Files.write(checkpoint.toPath(), "{\"version\":1}".getBytes(StandardCharsets.UTF_8));
        assertNull(prepare(checkpoint, source, () -> false));
        assertFalse(new File(checkpoint + ".thumbnail.jpg").exists());
        assertFalse(new File(checkpoint + ".thumbnail.meta").exists());
    }

    @Test public void declaredThumbnailMissingLocalAuxiliariesDelegatesToTheOrdinaryCore() throws Exception {
        File source = source(), checkpoint = new File(directory.getRoot(), "owned.maicloud.json");
        File thumbnail = prepare(checkpoint, source, () -> false);
        paused(checkpoint);
        String sourceBefore = sha(source);
        assertTrue(thumbnail.delete());
        assertTrue(new File(checkpoint + ".thumbnail.meta").delete());
        try {
            assertNull("Only the core can decide whether the remote object is already ready",
                prepare(checkpoint, source, () -> false));
        } catch (IOException rejectedBeforeCore) {
            fail("Missing local auxiliaries must not prevent the authoritative-ready query");
        }
        assertEquals(sourceBefore, sha(source));
        assertFalse(thumbnail.exists());
        assertFalse(new File(checkpoint + ".thumbnail.meta").exists());
    }

    @Test public void durableCheckpointSourcePathAliasIsReboundByTheCoreAfterContentIdentity() throws Exception {
        File original = source();
        File aliasDirectory = new File(directory.getRoot(), "owned-path-alias");
        assertTrue(aliasDirectory.mkdir());
        File alias = new File(aliasDirectory, ".." + File.separator + original.getName());
        assertTrue(alias.isFile());
        assertEquals(original.getCanonicalPath(), alias.getCanonicalPath());
        assertNotEquals(alias.getAbsolutePath(), alias.getCanonicalPath());
        File checkpoint = new File(directory.getRoot(), "alias.maicloud.json");
        File thumbnail = prepare(checkpoint, alias, () -> false);
        paused(checkpoint);
        JSONObject retained = new JSONObject(new String(Files.readAllBytes(checkpoint.toPath()),
            StandardCharsets.UTF_8));
        // The ordinary native file adapter stores its actual absolute spelling;
        // Android /data/user/0 -> /data/data is the real device counterpart.
        retained.put("source_id", alias.getAbsolutePath());
        Files.write(checkpoint.toPath(), retained.toString().getBytes(StandardCharsets.UTF_8));
        String before = sha(thumbnail);
        try { assertEquals(thumbnail, prepare(checkpoint, alias, () -> false)); }
        catch (IOException rejectedAlias) { fail("Equal original content must reach core source-ID rebinding"); }
        assertEquals(before, sha(thumbnail));
    }

    @Test public void invalidOrCanceledInputNeverPublishesAuxiliaryFiles() throws Exception {
        File source = directory.newFile("invalid.png");
        Files.write(source.toPath(), "not an image".getBytes(StandardCharsets.UTF_8));
        File checkpoint = new File(directory.getRoot(), "owned.maicloud.json");
        try { prepare(checkpoint, source, () -> false); fail("Undecodable input must fail before remote init"); }
        catch (IOException expected) { }
        try { prepare(checkpoint, source, () -> true); fail("Canceled preparation must not publish"); }
        catch (IOException expected) { }
        assertFalse(new File(checkpoint + ".thumbnail.jpg").exists());
        assertFalse(new File(checkpoint + ".thumbnail.meta").exists());
    }

    @Test public void cleanupKeepsPausedDataAndRemovesOnlyOwnedCompletedAuxiliaries() throws Exception {
        File source = source(), checkpoint = new File(directory.getRoot(), "owned.maicloud.json");
        File thumbnail = prepare(checkpoint, source, () -> false);
        paused(checkpoint);
        cleanup(checkpoint);
        assertTrue(thumbnail.isFile());
        assertTrue(new File(checkpoint + ".thumbnail.meta").isFile());
        assertTrue(checkpoint.delete());
        cleanup(checkpoint);
        assertFalse(thumbnail.exists());
        assertFalse(new File(checkpoint + ".thumbnail.meta").exists());
        assertTrue(source.isFile());
    }

    @Test public void borrowedAuxiliariesCannotBeCleanedByAnotherCaller() throws Exception {
        File source = source(), checkpoint = new File(directory.getRoot(), "owned.maicloud.json");
        File thumbnail = prepare(checkpoint, source, () -> false);
        Class<?> type = Class.forName("com.kongshang.maichat.MaiCloudDriveThumbnail");
        AutoCloseable lease = (AutoCloseable) type.getDeclaredMethod("borrow", File.class)
            .invoke(null, checkpoint);
        try {
            cleanup(checkpoint);
            assertTrue(thumbnail.exists());
            try {
                type.getDeclaredMethod("borrow", File.class).invoke(null, checkpoint);
                fail("A second owner must not replace or discard borrowed files");
            } catch (InvocationTargetException expected) {
                assertTrue(expected.getCause() instanceof IOException);
            }
        } finally { lease.close(); }
        assertFalse(thumbnail.exists());
        assertTrue(source.exists());
    }
}
