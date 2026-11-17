// Owned bounded-read fixtures; no account, model or device access.
package com.kongshang.maichat;

import static org.junit.Assert.*;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.concurrent.atomic.AtomicBoolean;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public final class MarkdownPreviewDialogTest {
    @Rule public TemporaryFolder temporary = new TemporaryFolder();

    @Test public void completeUtf8MarkdownRetainsItsTailAndReplacementDecoding() throws Exception {
        File file = temporary.newFile("owned.md");
        byte[] source = "# 中文标题\n\n完整末尾😀".getBytes(StandardCharsets.UTF_8);
        Files.write(file.toPath(), source);
        assertEquals(new String(source, StandardCharsets.UTF_8),
            MarkdownPreviewDialog.read(file, temporary.getRoot(), 1024, () -> true));
        Files.write(file.toPath(), new byte[]{'A', (byte) 0xff, 'B'});
        assertEquals("A\ufffdB", MarkdownPreviewDialog.read(file, temporary.getRoot(), 1024, () -> true));
    }

    @Test public void canonicalAllowedRootRejectsSiblingAndParentTraversal() throws Exception {
        File allowed = temporary.newFolder("workspace");
        File sibling = temporary.newFolder("workspace-other");
        File outside = new File(sibling, "owned.md");
        Files.write(outside.toPath(), "outside".getBytes(StandardCharsets.UTF_8));
        assertThrows(IOException.class, () -> MarkdownPreviewDialog.read(outside, allowed, 1024, () -> true));
        assertThrows(IOException.class, () -> MarkdownPreviewDialog.read(
            new File(allowed, "../workspace-other/owned.md"), allowed, 1024, () -> true));
    }

    @Test public void sizeLimitIsCheckedBeforeReadAndAgainIfFileGrowsDuringRead() throws Exception {
        File file = temporary.newFile("owned.md");
        Files.write(file.toPath(), new byte[129]);
        assertThrows(MarkdownPreviewDialog.TooLarge.class,
            () -> MarkdownPreviewDialog.read(file, temporary.getRoot(), 128, () -> true));
        Files.write(file.toPath(), new byte[64]);
        AtomicBoolean append = new AtomicBoolean(true);
        assertThrows(MarkdownPreviewDialog.TooLarge.class, () -> MarkdownPreviewDialog.read(file,
            temporary.getRoot(), 128, () -> {
                if (append.getAndSet(false)) {
                    try (FileOutputStream output = new FileOutputStream(file, true)) { output.write(new byte[128]); }
                    catch (IOException failure) { throw new AssertionError(failure); }
                }
                return true;
            }));
    }

    @Test public void cancellationStopsTheReadAndLargeThumbnailInputIsNeverSilentlyTruncated() throws Exception {
        File file = temporary.newFile("owned.md");
        Files.write(file.toPath(), "# Owned".getBytes(StandardCharsets.UTF_8));
        assertThrows(IOException.class, () -> MarkdownPreviewDialog.read(file, temporary.getRoot(), 1024, () -> false));
        Files.write(file.toPath(), new byte[AIAssistantArtifactThumbnail.MAX_MARKDOWN_BYTES + 1]);
        assertThrows(MarkdownPreviewDialog.TooLarge.class, () -> MarkdownPreviewDialog.read(file,
            temporary.getRoot(), AIAssistantArtifactThumbnail.MAX_MARKDOWN_BYTES, () -> true));
        assertEquals(8 * 1024 * 1024, MarkdownPreviewDialog.MAX_BYTES);
    }
}
