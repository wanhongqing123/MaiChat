// 文件作用：把 Android 16 kHz、PCM16 小端、单声道麦克风数据按 100 ms 块写入私有录音文件。
// 生命周期：宿主录音 worker 调用 start/finish；采集线程只读取和落盘，不同步回调 finish。
package com.kongshang.maichat;

import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaRecorder;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

final class PcmVoiceRecorder {
    interface Source {
        void start() throws Exception;
        int read(byte[] buffer) throws Exception;
        void stop();
        void release();
    }

    private static final int BLOCK_BYTES = 3200;
    private static final long FINISH_MILLIS = 1400;
    private final Object lifecycle = new Object();
    private final File file;
    private final Runnable failure;
    private final AtomicBoolean stopped = new AtomicBoolean(), released = new AtomicBoolean();
    private final AtomicBoolean outputClosed = new AtomicBoolean(), failureReported = new AtomicBoolean();
    private volatile Source source;
    private volatile FileOutputStream output;
    private volatile Thread capture;
    private volatile boolean stopRequested, failed;
    private boolean attempted, finished, finishResult;

    PcmVoiceRecorder(File file, Runnable failure) { this(file, failure, null); }

    // 测试只注入音源，仍使用正式采集线程、文件写入、停止和资源回收。
    PcmVoiceRecorder(File file, Runnable failure, Source source) {
        if (file == null || failure == null) throw new IllegalArgumentException("PCM output and failure callback are required");
        this.file = file;
        this.failure = failure;
        this.source = source;
    }

    void start() throws Exception {
        synchronized (lifecycle) {
            if (attempted || finished) throw new IOException("PCM recorder is already started or closed");
            attempted = true;
            try {
                if (source == null) source = new MicrophoneSource();
                output = new FileOutputStream(file);
                source.start();
                capture = new Thread(this::captureAudio, "aliyun-pcm-capture");
                capture.setDaemon(true);
                capture.start();
            } catch (Exception | Error error) {
                failed = true;
                stopRequested = true;
                cleanup();
                finished = true;
                throw error;
            }
        }
    }

    private void captureAudio() {
        byte[] block = new byte[BLOCK_BYTES];
        try {
            while (!stopRequested) {
                int count;
                try { count = source.read(block); }
                catch (Exception error) {
                    if (!stopRequested || !stopped.get()) failed = true;
                    break;
                }
                if (count <= 0) {
                    if (!stopRequested || !stopped.get()) failed = true;
                    break;
                }
                if (count > block.length || (count & 1) != 0) { failed = true; break; }
                // finish 请求期间返回的最后一个完整块仍必须落盘，不能按 stopRequested 丢弃尾音。
                output.write(block, 0, count);
            }
        } catch (IOException | RuntimeException error) {
            failed = true;
        } finally {
            cleanup();
            if (failed && failureReported.compareAndSet(false, true)) {
                try { failure.run(); }
                catch (RuntimeException ignored) { /* 宿主回调异常不能阻止录音资源回收。 */ }
            }
        }
    }

    // 不在 UI 调用。先允许当前 100 ms 读取自然结束，再 stop 解阻塞；所有 join 共用有限截止时间。
    // 必须 release 才退出的音源不能确认尾音完整，返回 false；重复 finish 返回同一结果。
    boolean finish() {
        if (Thread.currentThread() == capture) {
            stopRequested = true;
            failed = true;
            return false;
        }
        synchronized (lifecycle) {
            if (finished) return finishResult;
            stopRequested = true;
            Thread thread = capture;
            long deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(FINISH_MILLIS);
            try {
                if (thread != null) {
                    joinUntil(thread, Math.min(deadline, System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(250)));
                    if (thread.isAlive()) {
                        stopSource();
                        joinUntil(thread, Math.min(deadline, System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(900)));
                    }
                    if (thread.isAlive()) {
                        failed = true;
                        releaseSource();
                        joinUntil(thread, deadline);
                    }
                }
            } catch (InterruptedException interrupted) {
                failed = true;
                Thread.currentThread().interrupt();
            } finally {
                if (thread != null && thread.isAlive()) failed = true;
                cleanup();
                finishResult = thread != null && !thread.isAlive() && !failed;
                finished = true;
            }
            return finishResult;
        }
    }

    private static void joinUntil(Thread thread, long deadline) throws InterruptedException {
        long remaining = deadline - System.nanoTime();
        if (!thread.isAlive() || remaining <= 0) return;
        long millis = TimeUnit.NANOSECONDS.toMillis(remaining);
        thread.join(millis, (int) (remaining - TimeUnit.MILLISECONDS.toNanos(millis)));
    }

    private void stopSource() {
        Source current = source;
        if (current != null && stopped.compareAndSet(false, true)) {
            try { current.stop(); }
            catch (RuntimeException error) { failed = true; }
        }
    }

    private void releaseSource() {
        Source current = source;
        if (current != null && released.compareAndSet(false, true)) {
            try { current.release(); }
            catch (RuntimeException error) { failed = true; }
        }
    }

    private void cleanup() {
        stopSource();
        releaseSource();
        FileOutputStream current = output;
        if (current != null && outputClosed.compareAndSet(false, true)) {
            try { current.close(); }
            catch (IOException error) { failed = true; }
        }
    }

    private static final class MicrophoneSource implements Source {
        private final AudioRecord recorder;

        MicrophoneSource() throws IOException {
            int minimum = AudioRecord.getMinBufferSize(16_000, AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT);
            if (minimum <= 0) throw new IOException("PCM microphone format is unavailable");
            AudioRecord created = new AudioRecord(MediaRecorder.AudioSource.VOICE_RECOGNITION,
                16_000, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT,
                Math.max(minimum, BLOCK_BYTES * 2));
            if (created.getState() != AudioRecord.STATE_INITIALIZED) {
                created.release();
                throw new IOException("PCM microphone could not initialize");
            }
            recorder = created;
        }

        @Override public void start() throws IOException {
            recorder.startRecording();
            if (recorder.getRecordingState() != AudioRecord.RECORDSTATE_RECORDING)
                throw new IOException("PCM microphone could not start");
        }
        @Override public int read(byte[] buffer) {
            return recorder.read(buffer, 0, buffer.length, AudioRecord.READ_BLOCKING);
        }
        // stop 解除 read 阻塞后采集线程会进入清理；只串行 SDK 生命周期，不持锁等待 read。
        @Override public synchronized void stop() { recorder.stop(); }
        @Override public synchronized void release() { recorder.release(); }
    }
}
