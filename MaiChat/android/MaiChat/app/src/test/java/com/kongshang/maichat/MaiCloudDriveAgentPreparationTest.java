package com.kongshang.maichat;

import static org.junit.Assert.*;

import android.content.Context;
import android.graphics.BitmapFactory;
import androidx.test.core.app.ApplicationProvider;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Map;
import java.util.UUID;
import java.util.function.Consumer;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;
import org.robolectric.annotation.Implementation;
import org.robolectric.annotation.Implements;
import org.robolectric.util.ReflectionHelpers;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, shadows = MaiCloudDriveAgentPreparationTest.OwnedClient.class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public final class MaiCloudDriveAgentPreparationTest {
    @Implements(MaiCloudDrive.class)
    public static class OwnedClient {
        static boolean uploadScheduled;
        @Implementation protected MaiCloudDrive.Source prepareImageForUploadBlocking(
                MaiCloudDrive.Source source, File checkpoint) throws IOException {
            // Exercise the real bounded preparation with a host decoder. Only
            // JNI/transport is replaced; an invalid owned image really fails.
            MaiCloudDriveThumbnail.prepareFileWithDecoder(checkpoint, source.file,
                source.name, () -> false, (path, width, height) -> BitmapFactory.decodeFile(path));
            return source;
        }
        @Implementation protected void upload(MaiCloudDrive.Source source, File checkpoint,
                MaiCloudDrive.Progress progress, Consumer<MaiCloudDrive.Result> complete, Runnable settled) {
            uploadScheduled = true;
            settled.run();
        }
    }

    @Test public void undecodableImageFailsBeforeAgentAcknowledgesOrQueuesItsUpload() throws Exception {
        Context context = ApplicationProvider.getApplicationContext();
        File sourceFile = new File(context.getCacheDir(), "owned-invalid-image.png");
        Files.write(sourceFile.toPath(), "not a decodable image".getBytes(StandardCharsets.UTF_8));
        File checkpoint = new File(context.getCacheDir(), UUID.randomUUID() + ".maicloud.json");
        MaiCloudDrive.Source source = new MaiCloudDrive.Source(null, sourceFile,
            sourceFile.getName(), "image/png", sourceFile.length());
        String account = "owned-preparation-account";
        OwnedClient.uploadScheduled = false;
        try {
            ReflectionHelpers.callStaticMethod(MaiCloudDriveAgentTools.class, "startUpload",
                ReflectionHelpers.ClassParameter.from(MaiCloudDrive.class, new MaiCloudDrive(context)),
                ReflectionHelpers.ClassParameter.from(String.class, account),
                ReflectionHelpers.ClassParameter.from(MaiCloudDrive.Source.class, source),
                ReflectionHelpers.ClassParameter.from(File.class, checkpoint));
            fail("An undecodable image must not be acknowledged as an accepted upload");
        } catch (RuntimeException wrapped) {
            Throwable failure = wrapped;
            while (failure.getCause() != null) failure = failure.getCause();
            assertTrue(failure.toString(), failure instanceof MaiCloudDriveAgentTools.Failure);
            assertEquals("invalid_input", ((MaiCloudDriveAgentTools.Failure) failure).code);
        }
        assertFalse(OwnedClient.uploadScheduled);
        Map<?, ?> active = ReflectionHelpers.getStaticField(MaiCloudDriveAgentTools.class, "active");
        assertFalse(active.containsKey(account + ":" + checkpoint.getName().replace(".maicloud.json", "")));
        assertFalse(checkpoint.exists());
        assertFalse(new File(checkpoint + ".thumbnail.jpg").exists());
    }
}
