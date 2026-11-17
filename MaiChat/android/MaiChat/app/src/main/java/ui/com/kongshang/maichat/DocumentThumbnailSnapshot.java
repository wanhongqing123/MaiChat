// One bounded document snapshot owns both the content digest and the bytes used by its renderer.
package com.kongshang.maichat;

import android.os.ParcelFileDescriptor;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.function.BooleanSupplier;

/** PDF copies exist only for this call in the existing app cache; no disk thumbnail cache. */
final class DocumentThumbnailSnapshot implements AutoCloseable {
    static final long MAX_PDF_BYTES = 100L * 1024 * 1024;
    final String canonicalPath, digest, markdown;
    private ParcelFileDescriptor descriptor;

    private DocumentThumbnailSnapshot(String path, String digest, String markdown, ParcelFileDescriptor descriptor) {
        this.canonicalPath = path; this.digest = digest; this.markdown = markdown; this.descriptor = descriptor;
    }

    static DocumentThumbnailSnapshot markdown(File source, File root, int limit, BooleanSupplier current)
            throws IOException {
        File canonical = validate(source, root);
        byte[] bytes = readBytes(canonical, root, limit, current);
        return new DocumentThumbnailSnapshot(canonical.getPath(), hex(sha256().digest(bytes)),
            new String(bytes, StandardCharsets.UTF_8), null);
    }

    static DocumentThumbnailSnapshot pdf(File source, File root, File cacheDirectory, BooleanSupplier current)
            throws IOException {
        File canonical = PdfPreviewDialog.validate(source, root);
        if (!current.getAsBoolean()) throw new IOException("thumbnail canceled");
        File cache = cacheDirectory == null ? null : cacheDirectory.getCanonicalFile();
        if (cache == null || !cache.isDirectory()) throw new IOException("app cache unavailable");
        File temporary = File.createTempFile("mai-document-snapshot-", ".pdf", cache);
        ParcelFileDescriptor opened = null;
        try {
            MessageDigest digest = sha256();
            long total = 0;
            try (FileInputStream input = new FileInputStream(canonical);
                 FileOutputStream output = new FileOutputStream(temporary)) {
                byte[] buffer = new byte[16 * 1024];
                int count;
                while ((count = input.read(buffer)) != -1) {
                    if (!current.getAsBoolean()) throw new IOException("thumbnail canceled");
                    total += count;
                    if (total > MAX_PDF_BYTES) throw new IOException("PDF snapshot too large");
                    output.write(buffer, 0, count);
                    digest.update(buffer, 0, count);
                }
            }
            if (total < 8 || !current.getAsBoolean()) throw new IOException("PDF snapshot unavailable");
            // The descriptor owns these copied bytes. Removing the random name immediately
            // prevents a later source rewrite (even on the same inode) from changing them.
            opened = ParcelFileDescriptor.open(temporary, ParcelFileDescriptor.MODE_READ_ONLY);
            Files.delete(temporary.toPath());
            DocumentThumbnailSnapshot snapshot = new DocumentThumbnailSnapshot(canonical.getPath(),
                hex(digest.digest()), null, opened);
            opened = null;
            return snapshot;
        } finally {
            try { if (opened != null) opened.close(); }
            finally { Files.deleteIfExists(temporary.toPath()); }
        }
    }

    static File validate(File source, File root) throws IOException {
        if (source == null) throw new IOException("document unavailable");
        File canonical = source.getCanonicalFile();
        if (root != null && !canonical.toPath().startsWith(root.getCanonicalFile().toPath()))
            throw new IOException("document outside allowed root");
        if (!canonical.isFile() || !canonical.canRead()) throw new IOException("document unavailable");
        return canonical;
    }

    static byte[] readBytes(File source, File root, int limit, BooleanSupplier current) throws IOException {
        File canonical = validate(source, root);
        if (canonical.length() > limit) throw new TooLarge();
        try (FileInputStream input = new FileInputStream(canonical);
             ByteArrayOutputStream output = new ByteArrayOutputStream((int) Math.min(canonical.length(), limit))) {
            byte[] buffer = new byte[8192];
            int count, total = 0;
            while ((count = input.read(buffer)) != -1) {
                if (!current.getAsBoolean() || Thread.currentThread().isInterrupted())
                    throw new IOException("document preview canceled");
                if (count > limit - total) throw new TooLarge();
                output.write(buffer, 0, count);
                total += count;
            }
            return output.toByteArray();
        }
    }

    static final class TooLarge extends IOException { }

    ParcelFileDescriptor takeDescriptor() throws IOException {
        if (descriptor == null) throw new IOException("PDF snapshot already consumed");
        ParcelFileDescriptor owned = descriptor;
        descriptor = null;
        return owned;
    }

    @Override public void close() throws IOException {
        if (descriptor != null) { descriptor.close(); descriptor = null; }
    }

    private static MessageDigest sha256() throws IOException {
        try { return MessageDigest.getInstance("SHA-256"); }
        catch (NoSuchAlgorithmException impossible) { throw new IOException(impossible); }
    }

    private static String hex(byte[] bytes) {
        char[] digits = "0123456789abcdef".toCharArray();
        char[] output = new char[bytes.length * 2];
        for (int i = 0; i < bytes.length; i++) {
            output[i * 2] = digits[(bytes[i] & 255) >>> 4];
            output[i * 2 + 1] = digits[bytes[i] & 15];
        }
        return new String(output);
    }
}
