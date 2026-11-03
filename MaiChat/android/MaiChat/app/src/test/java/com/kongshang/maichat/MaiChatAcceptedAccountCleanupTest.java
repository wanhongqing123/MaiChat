// 文件作用：仅在显式临时目录验证账号清理边界，保留其他账号、Guest、回执及越界文件。
package com.kongshang.maichat;

import static org.junit.Assert.*;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import org.junit.Assume;
import org.junit.Test;

public class MaiChatAcceptedAccountCleanupTest {
    private Path fixture() throws Exception { return Files.createTempDirectory("maichat-account-cleanup-fixture-"); }
    private Path content(Path path) throws Exception { Files.createDirectories(path.getParent()); Files.write(path, new byte[]{1}); return path; }

    @Test public void removesOnlyTheCapturedHashedWorkspaceAndKeepsOtherAccountGuestAndReceipt() throws Exception {
        Path root = fixture();
        try {
            Path base = root.resolve("AIAssistant");
            Path own = content(base.resolve("Accounts").resolve(MaiChatAcceptedAccountCleanup.accountKey("owner-a")).resolve("Workspace/photo.jpg"));
            Path other = content(base.resolve("Accounts").resolve(MaiChatAcceptedAccountCleanup.accountKey("owner-b")).resolve("Workspace/other.txt"));
            Path guest = content(base.resolve("Guest/guest.txt"));
            Path receipt = content(root.resolve("account-deletion-receipt.json"));
            MaiChatAcceptedAccountCleanup.removeWorkspace(root.toFile(), "owner-a");
            assertFalse(Files.exists(own)); assertTrue(Files.exists(other)); assertTrue(Files.exists(guest)); assertTrue(Files.exists(receipt));
        } finally { removeFixture(root); }
    }

    @Test public void legacyOwnershipDeletesOnlyLegacyChildrenAndPreservesAccountContainers() throws Exception {
        Path root = fixture();
        try {
            Path base = root.resolve("AIAssistant");
            Path database = content(base.resolve("sessions.sqlite"));
            Path workspace = content(base.resolve("Workspace/file.txt"));
            Path other = content(base.resolve("Accounts/other-account/keep.txt"));
            Path guest = content(base.resolve("Guest/keep.txt"));
            Files.write(base.resolve("legacy-owner-user-id.txt"), "owner-a".getBytes(StandardCharsets.UTF_8));
            MaiChatAcceptedAccountCleanup.removeWorkspace(root.toFile(), "owner-b");
            assertTrue(Files.exists(database)); assertTrue(Files.exists(workspace));
            MaiChatAcceptedAccountCleanup.removeWorkspace(root.toFile(), "owner-a");
            assertFalse(Files.exists(database)); assertFalse(Files.exists(workspace)); assertTrue(Files.exists(other)); assertTrue(Files.exists(guest));
        } finally { removeFixture(root); }
    }

    @Test public void mediaDeletionRefusesAbsoluteParentAndSiblingPaths() throws Exception {
        Path root = fixture();
        try {
            Path media = root.resolve("RemoteIMMedia"); Files.createDirectories(media);
            Path outside = content(root.resolve("outside.txt"));
            for (String candidate : new String[]{"../outside.txt", outside.toString(), "Incoming/../../outside.txt"}) {
                try { MaiChatAcceptedAccountCleanup.deleteMedia(media.toFile(), candidate); fail("Out of root deletion accepted"); }
                catch (IOException expected) { }
                assertTrue(Files.exists(outside));
            }
            Path owned = content(media.resolve("Incoming/Images/owned.jpg"));
            MaiChatAcceptedAccountCleanup.deleteMedia(media.toFile(), "Incoming/Images/owned.jpg");
            assertFalse(Files.exists(owned));
        } finally { removeFixture(root); }
    }

    @Test public void symlinkOrJunctionEscapeIsRejectedBeforeRemovingWorkspaceData() throws Exception {
        Path root = fixture(), outside = fixture(), link = null;
        try {
            Path sentinel = content(outside.resolve("user-project.txt"));
            Path account = root.resolve("AIAssistant/Accounts").resolve(MaiChatAcceptedAccountCleanup.accountKey("owner-a"));
            Path own = content(account.resolve("Workspace/own.txt"));
            link = account.resolve("outside");
            try { Files.createSymbolicLink(link, outside); }
            catch (Exception unavailable) {
                Assume.assumeTrue(System.getProperty("os.name").startsWith("Windows"));
                Process process = new ProcessBuilder("cmd", "/c", "mklink", "/J", link.toString(), outside.toString()).redirectErrorStream(true).start();
                process.getInputStream().readAllBytes(); Assume.assumeTrue(process.waitFor() == 0);
            }
            try { MaiChatAcceptedAccountCleanup.removeWorkspace(root.toFile(), "owner-a"); fail("Linked external workspace was traversed"); }
            catch (IOException expected) { }
            assertTrue(Files.exists(sentinel)); assertTrue(Files.exists(own));
        } finally { if (link != null) Files.deleteIfExists(link); removeFixture(root); removeFixture(outside); }
    }

    @Test public void pendingProofIsOwnerBoundAndCompletionDoesNotRemoveReceipt() throws Exception {
        Path root = fixture();
        try {
            Path receipt = content(root.resolve("account-deletion-receipt.json"));
            MaiChatAcceptedAccountCleanup.Pending pending = MaiChatAcceptedAccountCleanup.markPending(root.toFile(),
                "owner-a", "account-a", "account-a:7", "123e4567-e89b-42d3-a456-426614174000");
            assertEquals("owner-a", MaiChatAcceptedAccountCleanup.pending(root.toFile()).get(0).owner);
            MaiChatAcceptedAccountCleanup.finishPending(root.toFile(), pending);
            assertTrue(MaiChatAcceptedAccountCleanup.pending(root.toFile()).isEmpty()); assertTrue(Files.exists(receipt));
        } finally { removeFixture(root); }
    }

    @Test public void interruptedLegacyDeletionRetainsOwnerMarkerAndCanRetryRemainingChildren() throws Exception {
        Assume.assumeTrue(System.getProperty("os.name").startsWith("Windows"));
        Path root = fixture(), blocked = null;
        try {
            Path base = root.resolve("AIAssistant"); Files.createDirectories(base);
            Path marker = base.resolve("legacy-owner-user-id.txt");
            Files.write(marker, "owner-a".getBytes(StandardCharsets.UTF_8));
            blocked = content(base.resolve("zzz-blocked.sqlite"));
            Assume.assumeTrue(blocked.toFile().setWritable(false));
            try { MaiChatAcceptedAccountCleanup.removeWorkspace(root.toFile(), "owner-a"); fail("Read-only file should interrupt fixture deletion"); }
            catch (IOException expected) { }
            assertTrue("Legacy ownership must survive an interrupted deletion", Files.exists(marker));
            assertTrue(blocked.toFile().setWritable(true));
            MaiChatAcceptedAccountCleanup.removeWorkspace(root.toFile(), "owner-a");
            assertFalse(Files.exists(blocked)); assertFalse(Files.exists(marker));
        } finally { if (blocked != null) blocked.toFile().setWritable(true); removeFixture(root); }
    }

    private static void removeFixture(Path root) throws Exception {
        // Windows 测试清理也先核对最终绝对目标，禁止把计算出的任意路径用于递归删除。
        Path resolved = root.toRealPath();
        Path temporary = new File(System.getProperty("java.io.tmpdir")).getCanonicalFile().toPath();
        if (!resolved.getParent().equals(temporary) || !resolved.getFileName().toString().startsWith("maichat-account-cleanup-fixture-"))
            throw new IOException("Fixture cleanup escaped the explicit temporary directory");
        try (java.util.stream.Stream<Path> entries = Files.walk(resolved)) {
            for (Path item : entries.sorted(java.util.Comparator.reverseOrder()).collect(java.util.stream.Collectors.toList())) Files.deleteIfExists(item);
        }
    }
}
