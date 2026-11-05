// Verifies the normal APK's prepared model bytes without reading accounts or starting inference.
package com.kongshang.maichat;

import static org.junit.Assert.*;

import android.app.Instrumentation;
import android.content.Context;
import android.os.Bundle;
import android.os.Debug;
import android.os.Process;
import android.os.SystemClock;
import android.util.Log;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InterruptedIOException;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Locale;
import java.util.Map;
import java.util.Timer;
import java.util.TimerTask;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class MaiBundledPrimaryModelInstrumentedTest {
    private static final String TAG = "MaiPrimaryIntegrityTest";
    private volatile String phase = "not-entered";
    private volatile long observedBytes;

    @Test(timeout = 90_000)
    public void preparedModelMatchesThePinnedIosResourceInsideThePrivateAppRoot() throws Exception {
        long started = SystemClock.elapsedRealtime();
        Thread testThread = Thread.currentThread();
        int testTid = Process.myTid();
        stage("entered", started);
        Timer watchdog = new Timer("MaiPrimaryIntegrityWatchdog", true);
        watchdog.schedule(new TimerTask() {
            @Override public void run() {
                dumpRelatedStacks(testThread, started);
                if (SystemClock.elapsedRealtime() - started >= 90_000) {
                    cancel();
                    watchdog.cancel();
                }
            }
        }, 15_000, 15_000);
        try {
            Instrumentation instrumentation = InstrumentationRegistry.getInstrumentation();
            Context target = instrumentation.getTargetContext();
            stage("target-context-ready", started);
            stage("resolve-begin", started);
            File model = MaiBundledPrimaryModel.resolve(target);
            stage("resolve-complete", started);
            stage("private-root-check-begin", started);
            String root = target.getFilesDir().getCanonicalPath() + File.separator;
            assertTrue(model.getCanonicalPath().startsWith(root));
            assertEquals(1_396_198_496L, model.length());
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            long bytes = 0, nextProgress = 256L * 1024 * 1024;
            stage("independent-hash-begin", started);
            try (FileInputStream input = new FileInputStream(model)) {
                byte[] prefix = new byte[4];
                assertEquals(4, input.read(prefix));
                assertArrayEquals(new byte[]{'G', 'G', 'U', 'F'}, prefix);
                digest.update(prefix);
                bytes = prefix.length;
                byte[] buffer = new byte[64 * 1024];
                int count;
                while ((count = input.read(buffer)) != -1) {
                    if (Thread.currentThread().isInterrupted())
                        throw new InterruptedIOException("Integrity test was interrupted");
                    digest.update(buffer, 0, count);
                    bytes += count;
                    observedBytes = bytes;
                    if (bytes >= nextProgress) {
                        stage("independent-hash-progress", started);
                        nextProgress += 256L * 1024 * 1024;
                    }
                }
            }
            stage("independent-hash-complete", started);
            StringBuilder sha = new StringBuilder();
            for (byte value : digest.digest()) sha.append(String.format(Locale.ROOT, "%02x", value & 255));
            assertEquals("57a1085840f497d764a7fc5d346922dbde961efb54cc792ea81d694fd846a1d8", sha.toString());
            assertEquals(1_396_198_496L, bytes);
            JSONObject report = new JSONObject().put("model_id", "builtin-qwen3.5-2b-q4_k_m")
                .put("bytes", bytes).put("sha256", sha.toString()).put("filename", model.getName())
                .put("private_app_file", true).put("inference_exercised", false)
                .put("verification_ms", SystemClock.elapsedRealtime() - started)
                .put("test_started_elapsed_ms", started).put("process_id", Process.myPid())
                .put("test_java_thread_id", testThread.getId()).put("test_os_tid", testTid);
            stage("receipt-directory-begin", started);
            File directory = target.getExternalFilesDir("primary-model-validation");
            assertNotNull(directory);
            assertTrue(directory.isDirectory() || directory.mkdirs());
            File receipt = new File(directory, "installed-model-integrity.json");
            stage("receipt-write-begin", started);
            try (FileOutputStream output = new FileOutputStream(receipt)) {
                output.write(report.toString(2).getBytes(StandardCharsets.UTF_8));
                output.getFD().sync();
            }
            stage("receipt-write-complete", started);
            Bundle status = new Bundle();
            status.putString("model_integrity_receipt", receipt.getAbsolutePath());
            instrumentation.sendStatus(0, status);
            stage("complete", started);
        } catch (Exception | AssertionError failure) {
            Log.e(TAG, "failed phase=" + phase + " elapsed_ms="
                + (SystemClock.elapsedRealtime() - started) + " type=" + failure.getClass().getName());
            dumpRelatedStacks(testThread, started);
            throw failure;
        } finally {
            watchdog.cancel();
        }
    }

    private void stage(String next, long started) {
        phase = next;
        Log.i(TAG, "phase=" + next + " elapsed_ms=" + (SystemClock.elapsedRealtime() - started)
            + " cpu_ms=" + Debug.threadCpuTimeNanos() / 1_000_000 + " bytes=" + observedBytes
            + " pid=" + Process.myPid() + " tid=" + Process.myTid());
    }

    private void dumpRelatedStacks(Thread testThread, long started) {
        Log.i(TAG, "watchdog phase=" + phase + " elapsed_ms="
            + (SystemClock.elapsedRealtime() - started) + " bytes=" + observedBytes);
        try {
            for (Map.Entry<Thread, StackTraceElement[]> entry : Thread.getAllStackTraces().entrySet()) {
                Thread thread = entry.getKey();
                if (thread == Thread.currentThread() && thread != testThread) continue;
                boolean related = thread == testThread;
                for (StackTraceElement frame : entry.getValue()) {
                    if (frame.getClassName().equals(MaiBundledPrimaryModel.class.getName())) related = true;
                }
                if (!related) continue;
                // Frames contain code locations only. Do not inspect objects, local variables,
                // arbitrary thread names, account files or unrelated application thread stacks.
                Log.i(TAG, "related_java_thread_id=" + thread.getId() + " state=" + thread.getState());
                int limit = Math.min(32, entry.getValue().length);
                for (int index = 0; index < limit; index++)
                    Log.i(TAG, "  at " + entry.getValue()[index]);
            }
        } catch (RuntimeException unavailable) {
            Log.e(TAG, "related stack capture unavailable: " + unavailable.getClass().getName());
        }
    }
}
