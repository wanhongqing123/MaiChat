// Tests use only owned local files and the ordinary host validator. No IM SDK,
// Agent runtime, provider, or account connection may be initialized.
package com.kongshang.maichat.tools;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.assertThrows;

import android.content.Context;
import android.webkit.MimeTypeMap;
import androidx.test.core.app.ApplicationProvider;
import com.kongshang.maichat.AIAssistantController;
import com.kongshang.maichat.LocalChatHistoryStore;
import com.kongshang.maichat.LocalSettingsStore;
import com.kongshang.maichat.RemoteIMFileAttachment;
import com.kongshang.maichat.RemoteIMMediaPaths;
import com.kongshang.maichat.RemoteIMMessage;
import com.kongshang.maichat.RemoteIMSessionController;
import java.io.File;
import java.lang.reflect.Field;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Comparator;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.stream.Stream;
import org.json.JSONObject;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.Shadows;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.Implementation;
import org.robolectric.annotation.Implements;
import org.robolectric.shadows.ShadowMimeTypeMap;
import org.robolectric.util.ReflectionHelpers;
import org.robolectric.util.ReflectionHelpers.ClassParameter;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, shadows = {MaiChatSendMediaToolsTest.WorkspaceOwner.class,
    MaiChatSendMediaToolsTest.LocalMediaRoot.class})
public class MaiChatSendMediaToolsTest {
    private Path directory;
    private RemoteIMSessionController session;
    private MaiChatIMTools tools;

    @Before public void setUp() throws Exception {
        directory = Files.createTempDirectory("maichat-owned-file-tool");
        session = new RemoteIMSessionController(
            new LocalSettingsStore(directory.resolve("settings").toFile()),
            new LocalChatHistoryStore(directory.resolve("history").toFile()));
        session.login("owned-file-owner");
        session.addContact("owned-file-peer");
        // The runtime constructor, workspace lookup and app media root are faked;
        // validation, file copy and the ordinary local session sendFile path remain real.
        WorkspaceOwner.owner = ReflectionHelpers.callConstructor(AIAssistantController.class,
            ClassParameter.from(Context.class, ApplicationProvider.getApplicationContext()),
            ClassParameter.from(String.class, null));
        LocalMediaRoot.directory = directory.resolve("media").toFile();
        tools = new MaiChatIMTools(null, session);
    }

    @After public void tearDown() throws Exception {
        WorkspaceOwner.owner = null;
        WorkspaceOwner.source = null;
        LocalMediaRoot.directory = null;
        if (session != null) {
            for (String name : new String[]{"ioExecutor", "messageHistoryExecutor"}) {
                Field field = RemoteIMSessionController.class.getDeclaredField(name);
                field.setAccessible(true);
                ExecutorService executor = (ExecutorService) field.get(session);
                if (executor != null) executor.shutdownNow();
            }
        }
        if (directory != null) try (Stream<Path> files = Files.walk(directory)) {
            for (Path path : (Iterable<Path>) files.sorted(Comparator.reverseOrder())::iterator)
                Files.deleteIfExists(path);
        }
    }

    @Test public void fileTypeReachesEmptyFileValidationWithoutQueueing() throws Exception {
        WorkspaceOwner.source = Files.createFile(directory.resolve("empty.txt")).toFile();
        IllegalArgumentException error = assertThrows(IllegalArgumentException.class,
            () -> tools.execute("maichat_send_media", fileArguments()));
        assertTrue("A file must reach size validation rather than be rejected as an unknown media type",
            error.getMessage().contains("大小限制"));
        assertTrue(session.chatState().messagesWith("owned-file-peer").isEmpty());
    }

    @Test public void fileTypeKeepsExistingOneGiBLimitWithoutAllocatingLargeFile() throws Exception {
        File tiny = Files.write(directory.resolve("owned.bin"), new byte[]{1}).toFile();
        WorkspaceOwner.source = new File(tiny.getPath()) {
            @Override public long length() { return 1024L * 1024 * 1024 + 1; }
        };
        IllegalArgumentException error = assertThrows(IllegalArgumentException.class,
            () -> tools.execute("maichat_send_media", fileArguments()));
        assertTrue(error.getMessage().contains("大小限制"));
        assertEquals(1L, tiny.length());
        assertTrue(session.chatState().messagesWith("owned-file-peer").isEmpty());
    }

    @Test public void ordinaryFilesUseFileAttachmentsWithTheirOwnMimeTypes() throws Exception {
        String[][] files = {
            {"report.PDF", "application/pdf"},
            {"report.doc", "application/msword"},
            {"report.docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
            {"slides.ppt", "application/vnd.ms-powerpoint"},
            {"slides.pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
            {"archive.zip", "application/zip"},
            {"owned.unknown-extension", "application/octet-stream"},
            {"README", "application/octet-stream"}
        };
        byte[] content = new byte[]{1, 2, 3};
        int sent = 0;
        // Robolectric's platform registry starts empty; supply known Android MIME entries.
        // Leave the unknown/no-extension cases absent to verify the binary fallback.
        ShadowMimeTypeMap mimeTypes = Shadows.shadowOf(MimeTypeMap.getSingleton());
        for (String[] file : files) {
            if (!file[1].equals("application/octet-stream"))
                mimeTypes.addExtensionMimeTypeMapping(
                    file[0].substring(file[0].lastIndexOf('.') + 1).toLowerCase(Locale.ROOT), file[1]);
            WorkspaceOwner.source = Files.write(directory.resolve(file[0]), content).toFile();
            JSONObject result = (JSONObject) tools.execute("maichat_send_media",
                fileArguments().put("caption", ""));
            assertTrue(result.getBoolean("queued"));
            assertEquals("file", result.getString("type"));
            assertEquals("file", result.getString("message_kind"));
            assertEquals("owned-file-peer", result.getString("peer_id"));
            RemoteIMMessage message = session.chatState().messagesWith("owned-file-peer").get(sent++);
            assertEquals(result.getString("message_id"), message.id());
            RemoteIMFileAttachment attachment = message.fileAttachment();
            assertNotNull(attachment);
            assertEquals(file[0], attachment.fileName());
            assertEquals(file[1], attachment.mimeType());
            assertEquals(content.length, attachment.sizeBytes());
            assertArrayEquals(content, Files.readAllBytes(new File(attachment.localPath()).toPath()));
            assertArrayEquals(content, Files.readAllBytes(WorkspaceOwner.source.toPath()));
        }
        assertEquals(files.length, session.chatState().messagesWith("owned-file-peer").size());
    }

    @Test public void fileTypeStillRejectsNonFriendsBeforeQueueing() throws Exception {
        WorkspaceOwner.source = Files.write(directory.resolve("report.pdf"), new byte[]{1}).toFile();
        IllegalArgumentException error = assertThrows(IllegalArgumentException.class,
            () -> tools.execute("maichat_send_media", fileArguments().put("peer_id", "not-a-friend")));
        assertTrue(error.getMessage().contains("peer_id"));
        assertTrue(session.chatState().messagesWith("not-a-friend").isEmpty());
        assertTrue(session.chatState().messagesWith("owned-file-peer").isEmpty());
    }

    private static JSONObject fileArguments() throws Exception {
        return new JSONObject().put("peer_id", "owned-file-peer").put("file_path", "owned.bin")
            .put("type", "file").put("caption", "Owned attachment");
    }

    @Implements(value = AIAssistantController.class, isInAndroidSdk = false)
    public static class WorkspaceOwner {
        static AIAssistantController owner;
        static File source;

        @Implementation protected void __constructor__(Context ignored, String endpoint) {
            // No constructor body: no authentication, native runtime, model, or SDK.
        }
        @Implementation public static AIAssistantController shared(Context ignored) { return owner; }
        @Implementation public File workspaceFile(String ignored) { return source; }
    }

    @Implements(value = RemoteIMMediaPaths.class, isInAndroidSdk = false)
    public static class LocalMediaRoot {
        static File directory;

        @Implementation public static RemoteIMMediaPaths forApp(Context ignored) {
            // Keep every copy under this test's private root without starting an Activity.
            return ReflectionHelpers.callConstructor(RemoteIMMediaPaths.class,
                ClassParameter.from(File.class, directory));
        }
    }
}
