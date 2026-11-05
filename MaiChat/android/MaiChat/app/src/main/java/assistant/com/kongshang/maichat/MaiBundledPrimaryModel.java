// Resolves the pinned APK resource into an immutable private file for the shared local runtime.
package com.kongshang.maichat;

import android.content.Context;
import android.content.res.AssetManager;
import android.os.Looper;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InterruptedIOException;
import java.nio.channels.Channels;
import java.nio.channels.FileChannel;
import java.nio.channels.FileLock;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.StandardOpenOption;
import java.nio.file.attribute.BasicFileAttributes;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Objects;
import org.json.JSONException;
import org.json.JSONObject;

final class MaiBundledPrimaryModel {
    static final String MODEL_ID = "builtin-qwen3.5-2b-q4_k_m";
    static final String MODEL_FILENAME = "Qwen_Qwen3.5-2B-Q4_K_M.gguf";
    static final long MODEL_BYTES = 1_396_198_496L;
    static final String MODEL_SHA256 = "57a1085840f497d764a7fc5d346922dbde961efb54cc792ea81d694fd846a1d8";
    static final String ASSET_DIRECTORY = "MaiLocalModels/Primary";
    private static final Object INSTALL_LOCK = new Object();
    private static final int BUFFER_BYTES = 64 * 1024;
    private static final Descriptor PRIMARY = new Descriptor(1, MODEL_ID, "Qwen3.5 2B · Q4_K_M",
        MODEL_FILENAME, MODEL_BYTES, MODEL_SHA256, "7d26695454df6de5fbcce2e58681e62dae06ce43");

    interface Source {
        InputStream open(String filename) throws IOException;
    }

    static final class Descriptor {
        final int version;
        final String id, name, filename, sha256, revision;
        final long bytes;

        Descriptor(int version, String id, String name, String filename, long bytes,
                   String sha256, String revision) {
            this.version = version;
            this.id = id;
            this.name = name;
            this.filename = filename;
            this.bytes = bytes;
            this.sha256 = sha256;
            this.revision = revision;
        }

        String installedFilename() {
            return filename.substring(0, filename.length() - 5) + "-" + sha256 + ".gguf";
        }
    }

    private MaiBundledPrimaryModel() { }

    /**
     * Worker-thread only: resolves APK assets without a runtime download, import catalog or projector.
     * Every resolution checks the compiled manifest identity and rehashes the installed content.
     * A corrupt existing content path is preserved, including files that may already be mmap'ed.
     */
    static File resolve(Context context) throws IOException {
        if (context == null) throw new IOException("Bundled primary model context is unavailable");
        if (Looper.myLooper() == Looper.getMainLooper())
            throw new IOException("Bundled primary model preparation requires a worker thread");
        File files = context.getFilesDir();
        if (files == null) throw new IOException("Private application files are unavailable");
        // Android's /data/user/0 alias may itself resolve through a system symlink. Canonicalize
        // that trusted application root first, then reject symlinks in our own model directories.
        File models = new File(files.getCanonicalFile(), "MaiLocalModels");
        prepareDirectory(models);
        AssetManager assets = context.getAssets();
        return resolve(filename -> assets.open(ASSET_DIRECTORY + "/" + filename,
            AssetManager.ACCESS_STREAMING), new File(models, "Primary"), PRIMARY);
    }

    // The same file pipeline accepts a small descriptor/source in isolated stream tests only.
    static File resolve(Source source, File directory, Descriptor descriptor) throws IOException {
        validateDescriptor(descriptor);
        synchronized (INSTALL_LOCK) {
            prepareDirectory(directory);
            File lock = new File(directory, ".primary-model.lock");
            if (Files.exists(lock.toPath(), LinkOption.NOFOLLOW_LINKS)
                    && !Files.isRegularFile(lock.toPath(), LinkOption.NOFOLLOW_LINKS))
                throw new IOException("Bundled primary model installation lock is invalid");
            // The Java lock serializes same-process callers; this file lock also covers a second
            // application process. Publication never replaces a cooperating installer's mmap path.
            try (FileChannel channel = FileChannel.open(lock.toPath(), StandardOpenOption.CREATE,
                    StandardOpenOption.WRITE, LinkOption.NOFOLLOW_LINKS);
                 FileLock held = channel.lock()) {
                checkInterrupted();
                validateManifest(source, descriptor);
                File destination = new File(directory, descriptor.installedFilename());
                if (Files.exists(destination.toPath(), LinkOption.NOFOLLOW_LINKS)) {
                    verifyInstalled(destination, descriptor);
                    return destination;
                }
                File temporary = File.createTempFile(".primary-model-", ".part", directory);
                IOException pending = null;
                try {
                    try (InputStream input = source.open(descriptor.filename);
                         FileOutputStream output = new FileOutputStream(temporary)) {
                        transferAndVerify(input, output, descriptor);
                        output.flush();
                        output.getFD().sync();
                    }
                    checkInterrupted();
                    // Recheck after copying: an independently appearing target must be verified
                    // and reused or preserved on failure, never removed to make rename succeed.
                    if (Files.exists(destination.toPath(), LinkOption.NOFOLLOW_LINKS)) {
                        verifyInstalled(destination, descriptor);
                        return destination;
                    }
                    // Both files are on the same private filesystem. rename is one atomic
                    // publication, under the install locks, and avoids Android hard-link failures.
                    if (!temporary.renameTo(destination)) {
                        if (Files.exists(destination.toPath(), LinkOption.NOFOLLOW_LINKS)) {
                            verifyInstalled(destination, descriptor);
                            return destination;
                        }
                        throw new IOException("Could not publish the complete bundled primary model");
                    }
                    return destination;
                } catch (IOException failure) {
                    pending = failure;
                    throw failure;
                } finally {
                    try {
                        Files.deleteIfExists(temporary.toPath());
                    } catch (IOException cleanup) {
                        if (pending != null) pending.addSuppressed(cleanup);
                        else throw cleanup;
                    }
                }
            }
        }
    }

    private static void prepareDirectory(File directory) throws IOException {
        if (directory == null) throw new IOException("Bundled primary model directory is unavailable");
        if (Files.exists(directory.toPath(), LinkOption.NOFOLLOW_LINKS)
                && !Files.isDirectory(directory.toPath(), LinkOption.NOFOLLOW_LINKS))
            throw new IOException("Bundled primary model directory is not a private directory");
        Files.createDirectories(directory.toPath());
        if (!Files.isDirectory(directory.toPath(), LinkOption.NOFOLLOW_LINKS))
            throw new IOException("Bundled primary model directory is invalid");
    }

    private static void validateDescriptor(Descriptor expected) throws IOException {
        if (expected == null || expected.version != 1 || expected.id == null || expected.name == null
                || expected.filename == null || expected.sha256 == null || expected.revision == null
                || !expected.filename.matches("[A-Za-z0-9_-][A-Za-z0-9_.-]*\\.gguf")
                || expected.filename.contains("..") || expected.filename.length() > 160
                || expected.bytes < 32 || expected.bytes > 16L * 1024 * 1024 * 1024
                || !expected.sha256.matches("[a-f0-9]{64}")
                || !expected.revision.matches("[a-f0-9]{40}"))
            throw new IOException("Bundled primary model compiled identity is invalid");
    }

    private static void validateManifest(Source source, Descriptor expected) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (InputStream input = source.open("manifest.json")) {
            byte[] buffer = new byte[4096];
            int read;
            while ((read = input.read(buffer)) != -1) {
                checkInterrupted();
                if (read == 0) continue;
                if (bytes.size() + read > 16 * 1024)
                    throw new IOException("Bundled primary model manifest exceeds its size limit");
                bytes.write(buffer, 0, read);
            }
        }
        try {
            JSONObject actual = new JSONObject(new String(bytes.toByteArray(), StandardCharsets.UTF_8));
            if (actual.length() != 7 || !integerEquals(actual.get("version"), expected.version)
                    || !integerEquals(actual.get("bytes"), expected.bytes)
                    || !expected.id.equals(actual.get("id")) || !expected.name.equals(actual.get("name"))
                    || !expected.filename.equals(actual.get("filename"))
                    || !expected.sha256.equals(actual.get("sha256"))
                    || !expected.revision.equals(actual.get("revision")))
                throw new IOException("Bundled primary model manifest differs from its compiled identity");
        } catch (JSONException failure) {
            throw new IOException("Bundled primary model manifest is invalid", failure);
        }
    }

    private static boolean integerEquals(Object actual, long expected) {
        return (actual instanceof Integer || actual instanceof Long)
            && ((Number) actual).longValue() == expected;
    }

    private static void verifyInstalled(File file, Descriptor expected) throws IOException {
        BasicFileAttributes before = regularFile(file, expected.bytes);
        try (InputStream input = Channels.newInputStream(FileChannel.open(file.toPath(),
                StandardOpenOption.READ, LinkOption.NOFOLLOW_LINKS))) {
            transferAndVerify(input, null, expected);
        }
        BasicFileAttributes after = regularFile(file, expected.bytes);
        if (!before.lastModifiedTime().equals(after.lastModifiedTime())
                || !Objects.equals(before.fileKey(), after.fileKey()))
            throw new IOException("Installed bundled primary model changed while being verified");
    }

    private static BasicFileAttributes regularFile(File file, long bytes) throws IOException {
        BasicFileAttributes attributes = Files.readAttributes(file.toPath(), BasicFileAttributes.class,
            LinkOption.NOFOLLOW_LINKS);
        if (!attributes.isRegularFile() || attributes.size() != bytes)
            throw new IOException("Installed bundled primary model is invalid; existing file was preserved");
        return attributes;
    }

    private static void transferAndVerify(InputStream input, FileOutputStream output,
                                          Descriptor expected) throws IOException {
        MessageDigest fingerprint;
        try {
            fingerprint = MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException unavailable) {
            throw new IOException("SHA-256 is unavailable", unavailable);
        }
        byte[] buffer = new byte[BUFFER_BYTES], prefix = new byte[8];
        long count = 0;
        int prefixBytes = 0, read;
        while ((read = input.read(buffer)) != -1) {
            checkInterrupted();
            if (read == 0) continue;
            count += read;
            if (count > expected.bytes)
                throw new IOException("Bundled primary model exceeds the pinned byte count");
            int prefixPart = Math.min(read, prefix.length - prefixBytes);
            System.arraycopy(buffer, 0, prefix, prefixBytes, prefixPart);
            prefixBytes += prefixPart;
            fingerprint.update(buffer, 0, read);
            if (output != null) output.write(buffer, 0, read);
        }
        checkInterrupted();
        if (count != expected.bytes || prefixBytes != 8 || prefix[0] != 'G' || prefix[1] != 'G'
                || prefix[2] != 'U' || prefix[3] != 'F' || (prefix[4] != 2 && prefix[4] != 3)
                || prefix[5] != 0 || prefix[6] != 0 || prefix[7] != 0
                || !expected.sha256.equals(hex(fingerprint.digest())))
            throw new IOException("Bundled primary model failed its pinned integrity check");
    }

    private static String hex(byte[] bytes) {
        char[] digits = "0123456789abcdef".toCharArray(), result = new char[bytes.length * 2];
        for (int index = 0; index < bytes.length; index++) {
            result[index * 2] = digits[(bytes[index] & 255) >>> 4];
            result[index * 2 + 1] = digits[bytes[index] & 15];
        }
        return new String(result);
    }

    private static void checkInterrupted() throws InterruptedIOException {
        if (Thread.currentThread().isInterrupted())
            throw new InterruptedIOException("Bundled primary model preparation was interrupted");
    }
}
