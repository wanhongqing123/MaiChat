// Verifies the normal APK's prepared model bytes without reading accounts or starting inference.
package com.kongshang.maichat;

import static org.junit.Assert.*;

import android.app.Instrumentation;
import android.content.Context;
import android.os.Bundle;
import android.os.SystemClock;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Locale;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class MaiBundledPrimaryModelInstrumentedTest {
    @Test public void preparedModelMatchesThePinnedIosResourceInsideThePrivateAppRoot() throws Exception {
        Instrumentation instrumentation = InstrumentationRegistry.getInstrumentation();
        Context target = instrumentation.getTargetContext();
        long started = SystemClock.elapsedRealtime();
        File model = MaiBundledPrimaryModel.resolve(target);
        String root = target.getFilesDir().getCanonicalPath() + File.separator;
        assertTrue(model.getCanonicalPath().startsWith(root));
        assertEquals(1_396_198_496L, model.length());
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        long bytes = 0;
        try (FileInputStream input = new FileInputStream(model)) {
            byte[] prefix = new byte[4];
            assertEquals(4, input.read(prefix));
            assertArrayEquals(new byte[]{'G', 'G', 'U', 'F'}, prefix);
            digest.update(prefix);
            bytes = prefix.length;
            byte[] buffer = new byte[64 * 1024];
            int count;
            while ((count = input.read(buffer)) != -1) {
                digest.update(buffer, 0, count);
                bytes += count;
            }
        }
        StringBuilder sha = new StringBuilder();
        for (byte value : digest.digest()) sha.append(String.format(Locale.ROOT, "%02x", value & 255));
        assertEquals("57a1085840f497d764a7fc5d346922dbde961efb54cc792ea81d694fd846a1d8", sha.toString());
        assertEquals(1_396_198_496L, bytes);
        JSONObject report = new JSONObject().put("model_id", "builtin-qwen3.5-2b-q4_k_m")
            .put("bytes", bytes).put("sha256", sha.toString()).put("filename", model.getName())
            .put("private_app_file", true).put("inference_exercised", false)
            .put("verification_ms", SystemClock.elapsedRealtime() - started);
        File directory = target.getExternalFilesDir("primary-model-validation");
        assertNotNull(directory);
        assertTrue(directory.isDirectory() || directory.mkdirs());
        File receipt = new File(directory, "installed-model-integrity.json");
        try (FileOutputStream output = new FileOutputStream(receipt)) {
            output.write(report.toString(2).getBytes(StandardCharsets.UTF_8));
            output.getFD().sync();
        }
        Bundle status = new Bundle();
        status.putString("model_integrity_receipt", receipt.getAbsolutePath());
        instrumentation.sendStatus(0, status);
    }
}
