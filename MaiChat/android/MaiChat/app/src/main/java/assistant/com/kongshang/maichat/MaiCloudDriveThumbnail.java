// Host-owned image preparation only. Multipart transport and durable upload state stay in C++.
package com.kongshang.maichat;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.util.HashSet;
import java.util.Set;
import java.util.UUID;
import java.util.function.BooleanSupplier;
import org.json.JSONObject;

final class MaiCloudDriveThumbnail {
    private static final int EDGE = 320;
    private static final long MAX_JPEG_BYTES = 2_000_000L;
    private static final Set<String> BORROWED = new HashSet<>();

    static final class Canceled extends IOException { }

    // This lease protects only the borrowed auxiliary files before/through the
    // synchronous native call. It never tracks multipart or server upload state.
    static final class Lease implements AutoCloseable {
        private final File checkpoint;
        private final String key;
        private boolean closed;
        Lease(File checkpoint, String key) { this.checkpoint = checkpoint; this.key = key; }
        @Override public void close() {
            synchronized (MaiCloudDriveThumbnail.class) {
                if (closed) return;
                closed = true;
                cleanupOwned(checkpoint);
                BORROWED.remove(key);
            }
        }
    }

    static synchronized Lease borrow(File checkpoint) throws IOException {
        String key = checkpoint.getCanonicalPath();
        if (!BORROWED.add(key)) throw new IOException("This thumbnail is already in use");
        return new Lease(checkpoint, key);
    }

    private interface Input {
        String id() throws IOException;
        long bytes();
        InputStream open() throws IOException;
        Bitmap decode(int edge) throws IOException;
    }

    interface Decoder { Bitmap decode(String path, int width, int height) throws IOException; }
    private static native Bitmap decodeImage(String path, int width, int height);

    static File prepareFile(File checkpoint, File source, String name,
                            BooleanSupplier canceled) throws IOException {
        return prepareFileWithDecoder(checkpoint, source, name, canceled,
            MaiCloudDriveThumbnail::decodeImage);
    }

    // Ordinary preparation is independent of transport. The production decoder
    // reuses the existing bounded native reader, including HEIC and all EXIF
    // transforms; host tests may supply their platform decoder without JNI.
    static File prepareFileWithDecoder(File checkpoint, File source, String name,
                                      BooleanSupplier canceled, Decoder decoder) throws IOException {
        if (!Files.isRegularFile(source.toPath(), LinkOption.NOFOLLOW_LINKS))
            throw new IOException("The original image is unavailable");
        final long bytes = source.length();
        return prepare(checkpoint, name, canceled, new Input() {
            public String id() throws IOException { return source.getCanonicalPath(); }
            public long bytes() { return bytes; }
            public InputStream open() throws IOException { return new FileInputStream(source); }
            public Bitmap decode(int edge) throws IOException {
                return decoder.decode(source.getAbsolutePath(), edge, edge);
            }
        });
    }

    private static File jpeg(File checkpoint) { return new File(checkpoint + ".thumbnail.jpg"); }
    private static File metadata(File checkpoint) { return new File(checkpoint + ".thumbnail.meta"); }

    private static void poll(BooleanSupplier canceled) throws Canceled {
        if (canceled.getAsBoolean()) throw new Canceled();
    }

    private static JSONObject jsonFile(File file, long maximum) throws IOException {
        if (!Files.isRegularFile(file.toPath(), LinkOption.NOFOLLOW_LINKS) ||
            file.length() <= 0 || file.length() > maximum) throw new IOException("Invalid thumbnail identity");
        try { return new JSONObject(new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8)); }
        catch (Exception invalid) { throw new IOException("Invalid thumbnail identity", invalid); }
    }

    static boolean legacyCheckpoint(File checkpoint) throws IOException {
        if (!checkpoint.exists()) return false;
        JSONObject record = jsonFile(checkpoint, 2L * 1024 * 1024);
        if (!record.has("thumbnail")) return true;
        if (record.optJSONObject("thumbnail") == null) throw new IOException("Invalid upload checkpoint");
        return false;
    }

    private static String digest(Input input, BooleanSupplier canceled) throws IOException {
        if (input.bytes() <= 0) throw new IOException("The original image size is unavailable");
        try {
            MessageDigest hash = MessageDigest.getInstance("SHA-256");
            byte[] block = new byte[64 * 1024];
            long count = 0;
            try (InputStream stream = input.open()) {
                for (;;) {
                    poll(canceled);
                    int bytes = stream.read(block);
                    if (bytes < 0) break;
                    if (bytes == 0) continue;
                    if (count > input.bytes() - bytes) throw new IOException("Original image changed");
                    count += bytes;
                    hash.update(block, 0, bytes);
                }
            }
            if (count != input.bytes()) throw new IOException("Original image changed");
            StringBuilder text = new StringBuilder();
            for (byte value : hash.digest())
                text.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
            return text.toString();
        } catch (java.security.NoSuchAlgorithmException impossible) {
            throw new IOException("SHA-256 is unavailable", impossible);
        }
    }

    private static String digestFile(File file, BooleanSupplier canceled) throws IOException {
        return digest(new Input() {
            public String id() { return file.getAbsolutePath(); }
            public long bytes() { return file.length(); }
            public InputStream open() throws IOException { return new FileInputStream(file); }
            public Bitmap decode(int ignored) { return null; }
        }, canceled);
    }

    private static void validateJpeg(File file) throws IOException {
        if (!Files.isRegularFile(file.toPath(), LinkOption.NOFOLLOW_LINKS) ||
            file.length() <= 0 || file.length() > MAX_JPEG_BYTES) throw new IOException("Thumbnail is unavailable");
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeFile(file.getAbsolutePath(), bounds);
        if (!"image/jpeg".equals(bounds.outMimeType) || bounds.outWidth <= 0 || bounds.outHeight <= 0 ||
            bounds.outWidth > EDGE || bounds.outHeight > EDGE) throw new IOException("Invalid thumbnail dimensions");
    }

    private static synchronized File prepare(File checkpoint, String name, BooleanSupplier canceled,
                                              Input input) throws IOException {
        if (legacyCheckpoint(checkpoint)) return null; // Do not attach a new resource to an in-flight legacy upload.
        poll(canceled);
        File target = jpeg(checkpoint), identity = metadata(checkpoint);
        String sourceSha = digest(input, canceled);
        if (checkpoint.exists()) {
            // The original still has to match its retained identity. Whether
            // the server already completed both resources is the core's job:
            // missing/corrupt local JPEGs must not block that authoritative query.
            JSONObject record = jsonFile(checkpoint, 2L * 1024 * 1024);
            // The core permits a stable-ID spelling/rebinding after matching
            // full content. Android absolute /data/user/0 and canonical
            // /data/data paths may name the same immutable snapshot.
            if (!name.equals(record.optString("name")) ||
                input.bytes() != record.optLong("file_size", -1) ||
                !sourceSha.equals(record.optString("file_sha256")))
                throw new IOException("The retained upload does not match its original");
            return Files.isRegularFile(target.toPath(), LinkOption.NOFOLLOW_LINKS) ? target : null;
        }
        if (target.exists() || identity.exists()) {
            // Never regenerate an incomplete upload's retained JPEG. Its exact
            // content is bound into the native checkpoint and server resource.
            validateJpeg(target);
            JSONObject saved = jsonFile(identity, 16 * 1024);
            if (saved.optInt("version") != 1 || !name.equals(saved.optString("name")) ||
                !input.id().equals(saved.optString("source_id")) ||
                saved.optLong("source_bytes", -1) != input.bytes() ||
                !sourceSha.equals(saved.optString("source_sha256")) ||
                saved.optLong("thumbnail_bytes", -1) != target.length() ||
                !digestFile(target, canceled).equals(saved.optString("thumbnail_sha256")))
                throw new IOException("The retained thumbnail does not match its original");
            return target;
        }
        File temporary = new File(target + ".prepare-" + UUID.randomUUID());
        File temporaryIdentity = new File(identity + ".prepare-" + UUID.randomUUID());
        Bitmap decoded = null, scaled = null, opaque = null;
        try {
            poll(canceled);
            decoded = input.decode(EDGE);
            if (decoded == null) throw new IOException("The image cannot be decoded");
            double ratio = Math.min(1, Math.min((double) EDGE / decoded.getWidth(),
                                              (double) EDGE / decoded.getHeight()));
            scaled = Bitmap.createScaledBitmap(decoded, Math.max(1, (int) Math.round(decoded.getWidth() * ratio)),
                Math.max(1, (int) Math.round(decoded.getHeight() * ratio)), true);
            // JPEG has no alpha. Match the other host producers by compositing
            // only this bounded derivative onto white; never rewrite the source.
            opaque = Bitmap.createBitmap(scaled.getWidth(), scaled.getHeight(), Bitmap.Config.ARGB_8888);
            Canvas canvas = new Canvas(opaque);
            canvas.drawColor(Color.WHITE);
            canvas.drawBitmap(scaled, 0, 0, null);
            try (FileOutputStream output = new FileOutputStream(temporary)) {
                if (!opaque.compress(Bitmap.CompressFormat.JPEG, 85, output))
                    throw new IOException("JPEG encoding failed");
                output.getFD().sync();
            }
            validateJpeg(temporary);
            if (!sourceSha.equals(digest(input, canceled))) throw new IOException("Original image changed");
            JSONObject record = new JSONObject().put("version", 1).put("name", name)
                .put("source_id", input.id()).put("source_bytes", input.bytes()).put("source_sha256", sourceSha)
                .put("thumbnail_bytes", temporary.length()).put("thumbnail_sha256", digestFile(temporary, canceled));
            try (FileOutputStream output = new FileOutputStream(temporaryIdentity)) {
                output.write(record.toString().getBytes(StandardCharsets.UTF_8));
                output.getFD().sync();
            }
            poll(canceled);
            Files.move(temporary.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE);
            Files.move(temporaryIdentity.toPath(), identity.toPath(), StandardCopyOption.ATOMIC_MOVE);
            return target;
        } catch (Canceled canceledPreparation) {
            throw canceledPreparation;
        } catch (OutOfMemoryError | RuntimeException failure) {
            throw new IOException("Thumbnail preparation failed", failure);
        } catch (org.json.JSONException impossible) {
            throw new IOException("Thumbnail identity encoding failed", impossible);
        } finally {
            if (opaque != null) opaque.recycle();
            if (scaled != null && scaled != decoded) scaled.recycle();
            if (decoded != null) decoded.recycle();
            temporary.delete();
            temporaryIdentity.delete();
        }
    }

    static synchronized void cleanupIfNoCheckpoint(File checkpoint) {
        try { if (BORROWED.contains(checkpoint.getCanonicalPath())) return; }
        catch (IOException invalid) { return; }
        cleanupOwned(checkpoint);
    }

    private static void cleanupOwned(File checkpoint) {
        if (checkpoint.exists()) return;
        for (File file : new File[]{jpeg(checkpoint), metadata(checkpoint)}) {
            if (Files.isRegularFile(file.toPath(), LinkOption.NOFOLLOW_LINKS)) file.delete();
        }
    }
}
