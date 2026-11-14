// The real ordinary adapter runs against owned auth/certificate/JNI boundaries only.
// No Native library, account endpoint, storage URL, IM SDK or model is contacted.
package com.kongshang.maichat;

import static org.junit.Assert.*;
import android.content.Context;
import androidx.test.core.app.ApplicationProvider;
import java.io.File;
import java.io.IOException;
import java.lang.reflect.InvocationTargetException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.concurrent.CancellationException;
import java.util.function.Consumer;
import java.util.stream.Stream;
import org.json.JSONObject;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.Implementation;
import org.robolectric.annotation.Implements;
import org.robolectric.util.ReflectionHelpers;
import org.robolectric.util.ReflectionHelpers.ClassParameter;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, shadows = {MaiSessionAttachmentsTemporaryImageTest.OwnedAuth.class,
    MaiSessionAttachmentsTemporaryImageTest.OwnedNative.class,
    MaiSessionAttachmentsTemporaryImageTest.OwnedTrust.class})
public class MaiSessionAttachmentsTemporaryImageTest {
    private Path directory;
    private MaiSessionAttachments attachments;

    @Before public void setUp() throws Exception {
        directory = Files.createTempDirectory("owned-session-image-adapter");
        Context context = ApplicationProvider.getApplicationContext();
        OwnedAuth.account = "reader-account"; OwnedAuth.accessCalls = 0; OwnedAuth.duringAccess = null;
        OwnedAuth.client = ReflectionHelpers.callConstructor(MaiChatAuthClient.class,
            ClassParameter.from(Context.class, context));
        OwnedNative.reset();
        attachments = new MaiSessionAttachments(context, "reader-account");
    }

    @After public void tearDown() throws Exception {
        OwnedAuth.client = null; OwnedAuth.duringAccess = null; OwnedNative.reset();
        if (directory != null) try (Stream<Path> files = Files.walk(directory)) {
            for (Path path : (Iterable<Path>) files.sorted(Comparator.reverseOrder())::iterator)
                Files.deleteIfExists(path);
        }
    }

    @Test public void passesOriginalPublicationTupleAndReturnsExactValidatedReceipt() throws Exception {
        File target = directory.resolve("image-cache.bin").toFile();
        JSONObject receipt = download(request(), target, new AIAssistantPreparationJobs.Cancellation());
        assertEquals(List.of("original-slot", "original-login", "original-session", "message-one", "part-one"),
            OwnedNative.tuple);
        assertEquals("https://owned.example.invalid/api", OwnedNative.base);
        assertEquals(7, OwnedNative.payload.length());
        assertEquals("12345678-1234-1234-1234-123456789abc", OwnedNative.payload.getString("attachment_id"));
        assertEquals("image.png", OwnedNative.payload.getString("name"));
        assertEquals("image/png", OwnedNative.payload.getString("content_type"));
        assertEquals("image", OwnedNative.payload.getString("media_type"));
        assertEquals(3, OwnedNative.payload.getLong("size_bytes"));
        assertEquals(request().getLong("expires_at"), OwnedNative.payload.getLong("expires_at"));
        assertEquals(target.getAbsolutePath(), OwnedNative.payload.getString("output_path"));
        assertTrue(receipt.getBoolean("downloaded")); assertEquals(7, receipt.length());
        assertEquals(target.getAbsolutePath(), receipt.getString("path"));
        assertArrayEquals(new byte[]{1, 2, 3}, Files.readAllBytes(target.toPath()));
        assertEquals(List.of("create", "download", "returned", "destroy"), OwnedNative.events);
    }

    @Test public void accountMismatchBeforeOrAfterCredentialRenewalNeverReachesNative() throws Exception {
        assertThrows(IOException.class, () -> download(request().put("account_id", "another-account"),
            directory.resolve("wrong-account.bin").toFile(), null));
        assertEquals(0, OwnedAuth.accessCalls); assertEquals(0, OwnedNative.downloads);
        OwnedAuth.duringAccess = () -> OwnedAuth.account = "replacement-account";
        assertThrows(IOException.class, () -> download(request(), directory.resolve("renewed.bin").toFile(), null));
        assertEquals(1, OwnedAuth.accessCalls); assertTrue(OwnedNative.events.isEmpty());
    }

    @Test public void nativeReturnAccountChangeDeletesOnlyThisNewUnpublishedTarget() throws Exception {
        File cached = Files.write(directory.resolve("existing-cache.bin"), new byte[]{7, 8}).toFile();
        File target = directory.resolve("new-cache.bin").toFile();
        OwnedNative.duringDownload = () -> OwnedAuth.account = "replacement-account";
        assertThrows(IOException.class, () -> download(request(), target, null));
        assertFalse(target.exists()); assertArrayEquals(new byte[]{7, 8}, Files.readAllBytes(cached.toPath()));
        assertEquals("destroy", OwnedNative.events.get(OwnedNative.events.size() - 1));
    }

    @Test public void cancellationKeepsHandleAliveUntilNativeReturnsAndRemovesOnlyNewTarget() throws Exception {
        AIAssistantPreparationJobs.Cancellation cancellation = new AIAssistantPreparationJobs.Cancellation();
        File target = directory.resolve("canceled.bin").toFile();
        OwnedNative.duringDownload = cancellation::cancel;
        assertThrows(CancellationException.class, () -> download(request(), target, cancellation));
        assertFalse(target.exists());
        assertEquals(List.of("create", "download", "cancel", "returned", "destroy"), OwnedNative.events);
        cancellation.cancel(); assertEquals(5, OwnedNative.events.size());
    }

    @Test public void alreadyCanceledWorkDoesNotFetchCredentialsOrAcquireAHandle() throws Exception {
        AIAssistantPreparationJobs.Cancellation cancellation = new AIAssistantPreparationJobs.Cancellation();
        cancellation.cancel();
        assertThrows(CancellationException.class,
            () -> download(request(), directory.resolve("never-created.bin").toFile(), cancellation));
        assertEquals(0, OwnedAuth.accessCalls); assertTrue(OwnedNative.events.isEmpty());
    }

    @Test public void malformedNativeReceiptIsNotPublished() throws Exception {
        Object[][] mutations = {{"downloaded", false}, {"attachment_id", "another-id"}, {"path", "/wrong/path"},
            {"size_bytes", 2}, {"expires_at", 1L}, {"content_type", "image/jpeg"}, {"media_type", "video"}};
        for (int index = 0; index < mutations.length; index++) {
            final Object[] mutation = mutations[index];
            OwnedNative.mutateReceipt = value -> {
                try { value.put((String) mutation[0], mutation[1]); }
                catch (Exception invalid) { throw new AssertionError(invalid); }
            };
            File target = directory.resolve("malformed-" + index + ".bin").toFile();
            assertThrows(IOException.class, () -> download(request(), target, null));
            assertFalse(target.exists());
        }
    }

    @Test public void existingDestinationIsNeverOverwrittenOrDeleted() throws Exception {
        File target = Files.write(directory.resolve("existing.bin"), new byte[]{9}).toFile();
        assertThrows(IOException.class, () -> download(request(), target, null));
        assertArrayEquals(new byte[]{9}, Files.readAllBytes(target.toPath()));
        assertEquals(0, OwnedNative.downloads);
    }

    @Test public void occupiedNativePathSurvivesWhenFileFacadeReportsMissing() throws Exception {
        Path occupied = Files.write(directory.resolve("occupied-native-path.bin"), new byte[]{9});
        // File.exists follows links, so a dangling link can look missing. The facade
        // provides that false result portably without requiring Windows symlink privileges.
        File target = new File(occupied.toString()) { @Override public boolean exists() { return false; } };
        assertThrows(IOException.class, () -> download(request(), target, null));
        assertTrue("A pre-existing native path must survive rejection", Files.exists(occupied));
        assertArrayEquals(new byte[]{9}, Files.readAllBytes(occupied));
        assertEquals(0, OwnedNative.downloads);
    }

    @Test public void actualFileSizeMustMatchTheNativeReceipt() throws Exception {
        OwnedNative.fileBytes = new byte[]{1, 2};
        File target = directory.resolve("short.bin").toFile();
        assertThrows(IOException.class, () -> download(request(), target, null));
        assertFalse(target.exists());
    }

    private static JSONObject request() throws Exception {
        return new JSONObject().put("account_id", "reader-account").put("owner_slot", "original-slot")
            .put("owner_login_session_id", "original-login").put("session_id", "original-session")
            .put("message_id", "message-one").put("part_id", "part-one")
            .put("attachment_id", "12345678-1234-1234-1234-123456789abc").put("name", "image.png")
            .put("content_type", "image/png").put("size_bytes", 3).put("expires_at", 4_000_000_000_000L);
    }

    private JSONObject download(JSONObject request, File target, AIAssistantPreparationJobs.Cancellation cancel)
            throws Exception {
        try {
            return (JSONObject) MaiSessionAttachments.class.getDeclaredMethod("downloadTemporaryImage",
                JSONObject.class, File.class, AIAssistantPreparationJobs.Cancellation.class)
                .invoke(attachments, request, target, cancel);
        } catch (NoSuchMethodException missing) {
            throw new AssertionError("Ordinary shared temporary-image adapter is missing", missing);
        } catch (InvocationTargetException wrapped) {
            if (wrapped.getCause() instanceof Exception) throw (Exception) wrapped.getCause();
            if (wrapped.getCause() instanceof Error) throw (Error) wrapped.getCause();
            throw new AssertionError(wrapped.getCause());
        }
    }

    @Implements(value = MaiChatAuthClient.class, isInAndroidSdk = false)
    public static class OwnedAuth {
        static MaiChatAuthClient client; static String account; static int accessCalls; static Runnable duringAccess;
        @Implementation protected void __constructor__(Context ignored) { }
        @Implementation public static MaiChatAuthClient shared(Context ignored) { return client; }
        @Implementation public String accountId() { return account; }
        @Implementation public MaiChatAuthClient.CloudDriveAccess cloudDriveAccess() {
            accessCalls++; if (duringAccess != null) duringAccess.run();
            return new MaiChatAuthClient.CloudDriveAccess("https://owned.example.invalid/api", "owned-fixture-token");
        }
    }

    @Implements(value = SystemCertificateBundle.class, isInAndroidSdk = false)
    public static class OwnedTrust {
        @Implementation protected static byte[] currentPem() { return new byte[]{42}; }
    }

    @Implements(value = MaiSessionAttachments.class, isInAndroidSdk = false)
    public static class OwnedNative {
        static final List<String> events = new ArrayList<>();
        static int downloads; static boolean active; static String base; static List<String> tuple;
        static JSONObject payload; static Runnable duringDownload; static Consumer<JSONObject> mutateReceipt;
        static byte[] fileBytes;
        static void reset() {
            events.clear(); downloads = 0; active = false; tuple = null; payload = null;
            duringDownload = null; mutateReceipt = null; fileBytes = new byte[]{1, 2, 3};
        }
        @Implementation protected static long createCancel() { return nativeCreateCancel(); }
        @Implementation protected static long nativeCreateCancel() { events.add("create"); return 77L; }
        @Implementation protected static void nativeRequestCancel(long handle) {
            assertEquals(77L, handle); assertTrue(active); events.add("cancel");
        }
        @Implementation protected static void nativeDestroyCancel(long handle) {
            assertEquals(77L, handle); assertFalse("Do not free a borrowed in-flight handle", active); events.add("destroy");
        }
        @Implementation protected static MaiCloudDrive.Result nativeDownloadTemporaryImage(String authBase, String token,
                byte[] pem, String raw, String slot, String login, String session, String message, String part,
                long cancel) throws Exception {
            assertEquals("owned-fixture-token", token); assertArrayEquals(new byte[]{42}, pem); assertEquals(77L, cancel);
            base = authBase; tuple = List.of(slot, login, session, message, part); payload = new JSONObject(raw);
            downloads++; active = true; events.add("download");
            try {
                Path target = new File(payload.getString("output_path")).toPath();
                // Preserve the shared new-file core's refusal to replace occupied paths.
                if (Files.exists(target, java.nio.file.LinkOption.NOFOLLOW_LINKS))
                    return new MaiCloudDrive.Result(5, 0, 0, new byte[0]);
                Files.write(target, fileBytes);
                if (duringDownload != null) duringDownload.run();
                JSONObject receipt = new JSONObject().put("downloaded", true)
                    .put("attachment_id", payload.get("attachment_id")).put("path", payload.get("output_path"))
                    .put("size_bytes", payload.get("size_bytes")).put("expires_at", payload.get("expires_at"))
                    .put("content_type", payload.get("content_type")).put("media_type", "image");
                if (mutateReceipt != null) mutateReceipt.accept(receipt);
                return new MaiCloudDrive.Result(0, 200, 0, receipt.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
            } finally { active = false; events.add("returned"); }
        }
    }
}
