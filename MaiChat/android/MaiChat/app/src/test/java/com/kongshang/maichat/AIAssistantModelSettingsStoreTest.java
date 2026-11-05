// Exercises real temporary files and the same transaction that the Controller uses around JNI.
package com.kongshang.maichat;

import static org.junit.Assert.*;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.junit.rules.TemporaryFolder;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, manifest = Config.NONE)
public final class AIAssistantModelSettingsStoreTest {
    @Rule public TemporaryFolder temporary = new TemporaryFolder();

    @Test public void persistenceFailureRestoresThePreviouslyConfiguredCore() throws Exception {
        File blockingParent = temporary.newFile("cannot-be-a-directory");
        File settings = new File(blockingParent, "settings.json");
        List<String> configured = new ArrayList<>();
        try {
            AIAssistantModelSettingsStore.apply(settings, AIAssistantModelSettings.defaults(), () -> true,
                () -> configured.add("proposed-core"), () -> configured.add("previous-core"));
            fail("Writing settings below a regular file must fail");
        } catch (IOException expected) { }
        assertEquals(List.of("proposed-core", "previous-core"), configured);
        assertTrue(blockingParent.isFile());
        assertFalse(settings.exists());
    }

    @Test public void staleAccountCannotConfigureOrOverwriteItsSavedSettings() throws Exception {
        File settings = temporary.newFile("settings.json");
        Files.write(settings.toPath(), "original-settings".getBytes(StandardCharsets.UTF_8));
        List<String> configured = new ArrayList<>();
        try {
            AIAssistantModelSettingsStore.apply(settings, AIAssistantModelSettings.defaults(), () -> false,
                () -> configured.add("proposed"), () -> configured.add("previous"));
            fail("An expired account must be rejected before configuring");
        } catch (IOException expected) { }
        assertTrue(configured.isEmpty());
        assertEquals("original-settings", new String(Files.readAllBytes(settings.toPath()), StandardCharsets.UTF_8));
    }

    @Test public void failedAtomicRenameCannotReportSuccessOrRetainTheProposedCore() throws Exception {
        // AtomicFile can open settings.json.new here, but its final rename cannot replace a
        // nonempty directory. SDK 34 finishWrite logs that failure and returns normally.
        File settings = temporary.newFolder("settings.json");
        File retained = new File(settings, "retained.txt");
        Files.write(retained.toPath(), "keep".getBytes(StandardCharsets.UTF_8));
        List<String> configured = new ArrayList<>();
        try {
            AIAssistantModelSettingsStore.apply(settings, AIAssistantModelSettings.defaults(), () -> true,
                () -> configured.add("proposed"), () -> configured.add("previous"));
            fail("AtomicFile.finishWrite returning normally does not prove the rename committed");
        } catch (IOException expected) { }
        assertEquals(List.of("proposed", "previous"), configured);
        assertEquals("keep", new String(Files.readAllBytes(retained.toPath()), StandardCharsets.UTF_8));
    }

    @Test public void accountClosedDuringConfigureCannotPublishLateSettingsOrReconfigureAgain() throws Exception {
        File settings = temporary.newFile("settings.json");
        Files.write(settings.toPath(), "original-settings".getBytes(StandardCharsets.UTF_8));
        AtomicBoolean active = new AtomicBoolean(true);
        List<String> configured = new ArrayList<>();
        try {
            AIAssistantModelSettingsStore.apply(settings, AIAssistantModelSettings.defaults(), active::get,
                () -> { configured.add("proposed"); active.set(false); },
                () -> configured.add("must-not-resurrect-closed-account"));
            fail("Late configuration must not publish settings for a closed account");
        } catch (IOException expected) { }
        assertEquals(List.of("proposed"), configured);
        assertEquals("original-settings", new String(Files.readAllBytes(settings.toPath()), StandardCharsets.UTF_8));
    }
}
