// Real bounded streams and private filesystem fixtures exercise integrity and immutable publication.
package com.kongshang.maichat;

import static org.junit.Assert.*;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import org.json.JSONObject;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

public final class MaiBundledPrimaryModelTest {
    @Rule public final TemporaryFolder temporary = new TemporaryFolder();

    @Test public void streamsVerifiedContentsBeforePublishingAndReusesTheImmutableFile() throws Exception {
        byte[] bytes = model();
        MaiBundledPrimaryModel.Descriptor expected = descriptor(bytes);
        File directory = temporary.newFolder();
        File destination = new File(directory, expected.installedFilename());
        File previous = new File(directory, "previous-version.gguf");
        Files.write(previous.toPath(), new byte[]{7, 8, 9});
        AtomicInteger opened = new AtomicInteger();
        MaiBundledPrimaryModel.Source source = source(manifest(expected), () -> {
            opened.incrementAndGet();
            return new ByteArrayInputStream(bytes) {
                @Override public synchronized int read(byte[] output, int offset, int count) {
                    assertFalse("Incomplete bytes must never appear at the final mmap path", destination.exists());
                    return super.read(output, offset, Math.min(7, count));
                }
            };
        });
        File first = MaiBundledPrimaryModel.resolve(source, directory, expected);
        assertEquals(destination.getCanonicalFile(), first.getCanonicalFile());
        assertTrue(first.getName().contains(expected.sha256));
        assertArrayEquals(bytes, Files.readAllBytes(first.toPath()));
        File again = MaiBundledPrimaryModel.resolve(source, directory, expected);
        assertEquals(first, again);
        assertEquals("A complete installed file is rehashed rather than recopied", 1, opened.get());
        assertArrayEquals(new byte[]{7, 8, 9}, Files.readAllBytes(previous.toPath()));
        assertNoStagingFiles(directory);
    }

    @Test public void failedReadRemovesOnlyItsTemporaryFileAndNeverPublishes() throws Exception {
        byte[] bytes = model();
        MaiBundledPrimaryModel.Descriptor expected = descriptor(bytes);
        File directory = temporary.newFolder();
        MaiBundledPrimaryModel.Source source = source(manifest(expected), () -> new InputStream() {
            int offset;
            @Override public int read() throws IOException {
                if (offset >= 40) throw new IOException("Injected read failure");
                return bytes[offset++] & 255;
            }
        });
        IOException failure = assertThrows(IOException.class,
            () -> MaiBundledPrimaryModel.resolve(source, directory, expected));
        assertTrue(failure.getMessage().contains("Injected read failure"));
        assertFalse(new File(directory, expected.installedFilename()).exists());
        assertNoStagingFiles(directory);
    }

    @Test public void wrongDigestAndAnExtraTrailingByteCannotBecomeInstalledFiles() throws Exception {
        byte[] bytes = model();
        MaiBundledPrimaryModel.Descriptor expected = descriptor(bytes);
        for (byte[] corrupt : new byte[][]{bytes.clone(), Arrays.copyOf(bytes, bytes.length + 1)}) {
            corrupt[50] ^= 1;
            File directory = temporary.newFolder();
            assertThrows(IOException.class, () -> MaiBundledPrimaryModel.resolve(
                source(manifest(expected), () -> new ByteArrayInputStream(corrupt)), directory, expected));
            assertFalse(new File(directory, expected.installedFilename()).exists());
            assertNoStagingFiles(directory);
        }
    }

    @Test public void manifestCannotChangeTheCompiledIdentityOrEscapeThePrivateDirectory() throws Exception {
        byte[] bytes = model();
        MaiBundledPrimaryModel.Descriptor expected = descriptor(bytes);
        File directory = temporary.newFolder();
        for (String changed : new String[]{manifest(expected).replace("fixture.gguf", "../escape.gguf"),
                manifest(expected).replace("fixture-primary", "other-model"), " ".repeat(16_385)}) {
            AtomicInteger weightOpens = new AtomicInteger();
            assertThrows(IOException.class, () -> MaiBundledPrimaryModel.resolve(source(changed, () -> {
                weightOpens.incrementAndGet();
                return new ByteArrayInputStream(bytes);
            }), directory, expected));
            assertEquals("Invalid metadata must fail before touching model bytes", 0, weightOpens.get());
        }
        assertFalse(new File(directory.getParentFile(), "escape.gguf").exists());
        assertNoStagingFiles(directory);
    }

    @Test public void anExistingCorruptContentPathIsPreservedInsteadOfOverwritten() throws Exception {
        byte[] bytes = model();
        MaiBundledPrimaryModel.Descriptor expected = descriptor(bytes);
        File directory = temporary.newFolder();
        File destination = new File(directory, expected.installedFilename());
        byte[] existing = "Existing possibly mapped data".getBytes(StandardCharsets.UTF_8);
        Files.write(destination.toPath(), existing);
        assertThrows(IOException.class, () -> MaiBundledPrimaryModel.resolve(
            source(manifest(expected), () -> new ByteArrayInputStream(bytes)), directory, expected));
        assertArrayEquals(existing, Files.readAllBytes(destination.toPath()));
    }

    @Test public void aPreviouslyValidFileIsRehashedOnEveryResolution() throws Exception {
        byte[] bytes = model();
        MaiBundledPrimaryModel.Descriptor expected = descriptor(bytes);
        File directory = temporary.newFolder();
        AtomicInteger opened = new AtomicInteger();
        MaiBundledPrimaryModel.Source source = source(manifest(expected), () -> {
            opened.incrementAndGet();
            return new ByteArrayInputStream(bytes);
        });
        File installed = MaiBundledPrimaryModel.resolve(source, directory, expected);
        byte[] changed = bytes.clone();
        changed[50] ^= 1;
        Files.write(installed.toPath(), changed);
        assertThrows(IOException.class, () -> MaiBundledPrimaryModel.resolve(source, directory, expected));
        assertArrayEquals("A changed possibly mapped file must remain untouched", changed,
            Files.readAllBytes(installed.toPath()));
        assertEquals("Integrity failure must not silently overwrite it from the APK", 1, opened.get());
    }

    @Test public void aDestinationAppearingDuringCopyIsNeverReplaced() throws Exception {
        byte[] bytes = model();
        MaiBundledPrimaryModel.Descriptor expected = descriptor(bytes);
        File directory = temporary.newFolder();
        File destination = new File(directory, expected.installedFilename());
        byte[] existing = new byte[]{42, 43, 44};
        MaiBundledPrimaryModel.Source source = source(manifest(expected), () -> {
            Files.write(destination.toPath(), existing);
            return new ByteArrayInputStream(bytes);
        });
        assertThrows(IOException.class, () -> MaiBundledPrimaryModel.resolve(source, directory, expected));
        assertArrayEquals(existing, Files.readAllBytes(destination.toPath()));
        assertNoStagingFiles(directory);
    }

    @Test public void concurrentResolversShareOneCompletedContentFile() throws Exception {
        byte[] bytes = model();
        MaiBundledPrimaryModel.Descriptor expected = descriptor(bytes);
        File directory = temporary.newFolder();
        AtomicInteger opened = new AtomicInteger();
        MaiBundledPrimaryModel.Source source = source(manifest(expected), () -> {
            opened.incrementAndGet();
            return new ByteArrayInputStream(bytes);
        });
        ExecutorService executor = Executors.newFixedThreadPool(2);
        try {
            Future<File> first = executor.submit(() -> MaiBundledPrimaryModel.resolve(source, directory, expected));
            Future<File> second = executor.submit(() -> MaiBundledPrimaryModel.resolve(source, directory, expected));
            assertEquals(first.get(5, TimeUnit.SECONDS), second.get(5, TimeUnit.SECONDS));
            assertEquals(1, opened.get());
        } finally {
            executor.shutdownNow();
            assertTrue(executor.awaitTermination(5, TimeUnit.SECONDS));
        }
        assertNoStagingFiles(directory);
    }

    private interface OpenWeight { InputStream open() throws IOException; }

    private static MaiBundledPrimaryModel.Source source(String manifest, OpenWeight weight) {
        return filename -> filename.equals("manifest.json")
            ? new ByteArrayInputStream(manifest.getBytes(StandardCharsets.UTF_8)) : weight.open();
    }

    private static byte[] model() {
        byte[] bytes = new byte[128];
        for (int index = 8; index < bytes.length; index++) bytes[index] = (byte) index;
        bytes[0] = 'G'; bytes[1] = 'G'; bytes[2] = 'U'; bytes[3] = 'F'; bytes[4] = 3;
        return bytes;
    }

    private static MaiBundledPrimaryModel.Descriptor descriptor(byte[] bytes) throws Exception {
        StringBuilder hash = new StringBuilder();
        for (byte value : MessageDigest.getInstance("SHA-256").digest(bytes))
            hash.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
        return new MaiBundledPrimaryModel.Descriptor(1, "fixture-primary", "Fixture", "fixture.gguf",
            bytes.length, hash.toString(), "0123456789abcdef0123456789abcdef01234567");
    }

    private static String manifest(MaiBundledPrimaryModel.Descriptor descriptor) throws Exception {
        return new JSONObject().put("version", descriptor.version).put("id", descriptor.id)
            .put("name", descriptor.name).put("filename", descriptor.filename).put("bytes", descriptor.bytes)
            .put("sha256", descriptor.sha256).put("revision", descriptor.revision).toString();
    }

    private static void assertNoStagingFiles(File directory) {
        File[] leftovers = directory.listFiles((parent, name) -> name.startsWith(".primary-model-"));
        assertNotNull(leftovers);
        assertEquals("Failed or completed copies must remove their own temporary file", 0, leftovers.length);
    }
}
