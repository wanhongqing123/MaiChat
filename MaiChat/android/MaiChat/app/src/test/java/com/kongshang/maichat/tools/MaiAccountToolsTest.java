// 文件作用：验证账号工具公开输出、工作区边界，以及并发取消早于请求登记的行为。
package com.kongshang.maichat.tools;

import com.kongshang.maichat.RemoteIMSessionController;
import org.json.JSONObject;
import org.junit.Test;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.concurrent.atomic.AtomicInteger;

import static org.junit.Assert.*;

public class MaiAccountToolsTest {
    @Test public void loggedInProfileHasOnlySixPublicFieldsAndKeepsNullValues() throws Exception {
        JSONObject result = MaiChatIMTools.currentAccountJson(
            new RemoteIMSessionController.CurrentAccount(true, "public-42", "im-73",
                null, "138****1234", null));
        assertEquals(6, result.length());
        assertTrue(result.getBoolean("logged_in"));
        assertEquals("public-42", result.getString("account_id"));
        assertEquals("im-73", result.getString("im_user_id"));
        assertTrue(result.isNull("nickname"));
        assertTrue(result.isNull("avatar_url"));
        assertEquals("138****1234", result.getString("phone_masked"));
    }

    @Test public void loggedOutProfileDoesNotExposePreviousAccountFields() throws Exception {
        JSONObject result = MaiChatIMTools.currentAccountJson(
            new RemoteIMSessionController.CurrentAccount(false, "old-public", "old-im",
                "old nickname", "138****1234", "https://example.test/old.jpg"));
        assertEquals(1, result.length());
        assertFalse(result.getBoolean("logged_in"));
    }

    @Test public void rejectsSiblingWorkspaceAndSymlinkEscapes() throws Exception {
        Path root = Files.createTempDirectory("maichat-avatar-scope");
        Path workspace = Files.createDirectory(root.resolve("workspace"));
        Path sibling = Files.createDirectory(root.resolve("workspace-other"));
        Path own = Files.write(workspace.resolve("own.png"), new byte[]{1});
        Path other = Files.write(sibling.resolve("other.png"), new byte[]{1});
        assertEquals(own.toFile().getCanonicalFile(), MaiChatIMTools.avatarWorkspaceFile(
            own.toString(), workspace.toString()));
        assertThrows(IOException.class, () -> MaiChatIMTools.avatarWorkspaceFile(
            other.toString(), workspace.toString()));
        Path link = Files.createSymbolicLink(workspace.resolve("link.png"), other);
        assertThrows(IOException.class, () -> MaiChatIMTools.avatarWorkspaceFile(
            link.toString(), workspace.toString()));
        Path ownLink = Files.createSymbolicLink(workspace.resolve("own-link.png"), own);
        assertEquals(own.toRealPath().toFile(), MaiChatIMTools.avatarWorkspaceFile(
            ownLink.toString(), workspace.toString()));
        Path outsideDirectory = Files.createSymbolicLink(workspace.resolve("outside-directory"), sibling);
        assertThrows(IOException.class, () -> MaiChatIMTools.avatarWorkspaceFile(
            outsideDirectory.resolve("other.png").toString(), workspace.toString()));
    }

    @Test public void remembersCancelBeforeRequestAndBoundsPendingIds() throws Exception {
        MaiChatIMTools.AvatarRequests requests = new MaiChatIMTools.AvatarRequests();
        requests.cancel("canceled-first");
        assertThrows(IOException.class, () -> requests.begin("canceled-first"));
        for (int i = 0; i < 257; ++i) requests.cancel("expired-" + i);
        assertNotNull(requests.begin("expired-0"));
        assertThrows(IOException.class, () -> requests.begin("expired-256"));
    }

    @Test public void cancelBeforeTaskBindingReachesTaskAndRetiredRequestIsUntouched() throws Exception {
        MaiChatIMTools.AvatarRequests requests = new MaiChatIMTools.AvatarRequests();
        MaiChatIMTools.AvatarRequest request = requests.begin("active");
        requests.cancel("active");
        AtomicInteger canceled = new AtomicInteger();
        request.bind(canceled::incrementAndGet);
        assertTrue(request.isCanceled());
        assertEquals(1, canceled.get());
        requests.finish("active", request);
        requests.cancel("active");
        assertEquals(1, canceled.get());
    }
}
