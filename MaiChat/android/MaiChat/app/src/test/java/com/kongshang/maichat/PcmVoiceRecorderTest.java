// 文件作用：用假音源和真实线程/临时文件验证 PCM 录音停止、尾块与失败清理。
// 运行边界：不实例化 AudioRecord，不访问麦克风、Android 设备或网络。
package com.kongshang.maichat;

import static org.junit.Assert.*;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.Test;

public class PcmVoiceRecorderTest {
    private static final class FakeSource implements PcmVoiceRecorder.Source {
        final CountDownLatch entered = new CountDownLatch(1);
        final CountDownLatch ready = new CountDownLatch(1);
        final CountDownLatch stopped = new CountDownLatch(1);
        final AtomicInteger starts = new AtomicInteger(), stops = new AtomicInteger(), releases = new AtomicInteger();
        final AtomicInteger reads = new AtomicInteger();
        final byte[] tail;
        boolean failStart, failRead, zeroRead, stopUnblocks = true;

        FakeSource(byte[] tail) { this.tail = tail; }

        @Override public void start() throws Exception {
            starts.incrementAndGet();
            if (failStart) throw new IOException("private source detail");
        }

        @Override public int read(byte[] buffer) throws Exception {
            if (reads.incrementAndGet() == 1) {
                entered.countDown();
                if (!ready.await(4, TimeUnit.SECONDS)) throw new IOException("Test source was left blocked");
                if (failRead) throw new IOException("private read detail");
                if (zeroRead) return 0;
                if (tail.length > 0) {
                    System.arraycopy(tail, 0, buffer, 0, tail.length);
                    return tail.length;
                }
            }
            if (!stopped.await(4, TimeUnit.SECONDS)) throw new IOException("Test source was left running");
            return -3;
        }

        @Override public void stop() {
            stops.incrementAndGet();
            if (stopUnblocks) { ready.countDown(); stopped.countDown(); }
        }

        @Override public void release() {
            releases.incrementAndGet();
            ready.countDown();
            stopped.countDown();
        }
    }

    @Test(timeout = 5000) public void finishWritesTheInFlightTailBlockBeforeClosingResources() throws Exception {
        File file = File.createTempFile("maichat-pcm-recorder-tail-", ".pcm");
        byte[] tail = new byte[3200];
        for (int index = 0; index < tail.length; index++) tail[index] = (byte) (index * 37);
        FakeSource source = new FakeSource(tail);
        AtomicInteger failures = new AtomicInteger();
        PcmVoiceRecorder recorder = new PcmVoiceRecorder(file, failures::incrementAndGet, source);
        try {
            recorder.start();
            assertTrue(source.entered.await(2, TimeUnit.SECONDS));
            assertTrue(recorder.finish());
            assertArrayEquals(tail, Files.readAllBytes(file.toPath()));
            assertEquals(0, failures.get());
            assertEquals(1, source.starts.get());
            assertEquals(1, source.stops.get());
            assertEquals(1, source.releases.get());
        } finally { recorder.finish(); file.delete(); }
    }

    @Test(timeout = 5000) public void finishIsIdempotentAndDoesNotStopOrReleaseTwice() throws Exception {
        File file = File.createTempFile("maichat-pcm-recorder-once-", ".pcm");
        FakeSource source = new FakeSource(new byte[] {0x34, 0x12});
        PcmVoiceRecorder recorder = new PcmVoiceRecorder(file, () -> fail("Unexpected recording failure"), source);
        try {
            recorder.start();
            assertTrue(source.entered.await(2, TimeUnit.SECONDS));
            assertTrue(recorder.finish());
            assertTrue(recorder.finish());
            assertEquals(1, source.stops.get());
            assertEquals(1, source.releases.get());
            assertArrayEquals(new byte[] {0x34, 0x12}, Files.readAllBytes(file.toPath()));
        } finally { recorder.finish(); file.delete(); }
    }

    @Test(timeout = 5000) public void sourceReadFailureReportsOnceAndFinishReturnsFalse() throws Exception {
        File file = File.createTempFile("maichat-pcm-recorder-read-fail-", ".pcm");
        FakeSource source = new FakeSource(new byte[0]); source.failRead = true;
        AtomicInteger failures = new AtomicInteger();
        CountDownLatch failed = new CountDownLatch(1);
        PcmVoiceRecorder recorder = new PcmVoiceRecorder(file, () -> {
            failures.incrementAndGet(); failed.countDown();
        }, source);
        try {
            recorder.start();
            assertTrue(source.entered.await(2, TimeUnit.SECONDS));
            source.ready.countDown();
            assertTrue(failed.await(2, TimeUnit.SECONDS));
            assertFalse(recorder.finish());
            assertFalse(recorder.finish());
            assertEquals(1, failures.get());
            assertEquals(1, source.stops.get());
            assertEquals(1, source.releases.get());
            assertEquals(0, file.length());
        } finally { recorder.finish(); file.delete(); }
    }

    @Test(timeout = 5000) public void startupFailureReleasesTheInitializedSourceAndClosesTheFile() throws Exception {
        File file = File.createTempFile("maichat-pcm-recorder-start-fail-", ".pcm");
        FakeSource source = new FakeSource(new byte[0]); source.failStart = true;
        AtomicInteger failures = new AtomicInteger();
        PcmVoiceRecorder recorder = new PcmVoiceRecorder(file, failures::incrementAndGet, source);
        try {
            try { recorder.start(); fail("Source startup failure was accepted"); }
            catch (Exception expected) { }
            assertFalse(recorder.finish());
            assertEquals(1, source.starts.get());
            assertEquals(1, source.releases.get());
            assertEquals(0, failures.get());
            assertTrue("Startup failure retained an open output file", file.delete());
        } finally { recorder.finish(); file.delete(); }
    }

    @Test(timeout = 5000) public void cancellingAQuietSourceStopsTheBlockingReadWithoutReportingAnError() throws Exception {
        File file = File.createTempFile("maichat-pcm-recorder-cancel-", ".pcm");
        FakeSource source = new FakeSource(new byte[0]);
        AtomicInteger failures = new AtomicInteger();
        PcmVoiceRecorder recorder = new PcmVoiceRecorder(file, failures::incrementAndGet, source);
        try {
            recorder.start();
            assertTrue(source.entered.await(2, TimeUnit.SECONDS));
            long began = System.nanoTime();
            assertTrue(recorder.finish());
            assertTrue(TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - began) < 2000);
            assertEquals(0, failures.get());
            assertEquals(1, source.stops.get());
            assertEquals(1, source.releases.get());
            assertTrue("Finished recorder retained an open output file", file.delete());
        } finally { recorder.finish(); file.delete(); }
    }

    @Test(timeout = 5000) public void releaseUnblocksASourceThatIgnoresStopAndMarksTheRecordingUnconfirmed() throws Exception {
        File file = File.createTempFile("maichat-pcm-recorder-stuck-", ".pcm");
        FakeSource source = new FakeSource(new byte[0]); source.stopUnblocks = false;
        PcmVoiceRecorder recorder = new PcmVoiceRecorder(file, () -> { }, source);
        try {
            recorder.start();
            assertTrue(source.entered.await(2, TimeUnit.SECONDS));
            long began = System.nanoTime();
            assertFalse(recorder.finish());
            assertTrue(TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - began) < 2000);
            assertEquals(1, source.stops.get());
            assertEquals(1, source.releases.get());
        } finally { recorder.finish(); file.delete(); }
    }

    @Test(timeout = 5000) public void anUnexpectedEmptyReadFailsInsteadOfSpinningOrPublishingASuccessfulRecording() throws Exception {
        File file = File.createTempFile("maichat-pcm-recorder-zero-", ".pcm");
        FakeSource source = new FakeSource(new byte[0]); source.zeroRead = true;
        CountDownLatch failed = new CountDownLatch(1);
        AtomicInteger failures = new AtomicInteger();
        PcmVoiceRecorder recorder = new PcmVoiceRecorder(file, () -> {
            failures.incrementAndGet(); failed.countDown();
        }, source);
        try {
            recorder.start();
            assertTrue(source.entered.await(2, TimeUnit.SECONDS));
            source.ready.countDown();
            assertTrue(failed.await(2, TimeUnit.SECONDS));
            assertFalse(recorder.finish());
            assertEquals(1, source.reads.get());
            assertEquals(1, failures.get());
            assertEquals(1, source.releases.get());
        } finally { recorder.finish(); file.delete(); }
    }
}
