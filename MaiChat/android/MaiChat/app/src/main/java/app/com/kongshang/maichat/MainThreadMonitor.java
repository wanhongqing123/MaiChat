// 文件作用：在后台串行采样主线程卡顿，记录、按事件时间导出和清理本服务的诊断日志。
// 主要入口：record、export、clear；不读取或清理聊天、附件及其他模块的数据。
package com.kongshang.maichat;

import android.os.Debug;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.SystemClock;
import android.view.FrameMetrics;
import android.view.Window;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.function.Consumer;

/** Bounded diagnostics written by a background thread, including a stalled main-thread stack. */
final class MainThreadMonitor {
    private static final String[] LOG_NAMES = {"ui.previous.log", "ui.log"};
    private final HandlerThread thread = new HandlerThread("MaiChat-diagnostics");
    private final Handler worker;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final Window window;
    private final File directory;
    private volatile boolean foreground = true, closed, pending;
    private volatile long pulse;
    private final java.util.concurrent.atomic.AtomicLong pulseGeneration = new java.util.concurrent.atomic.AtomicLong();
    private volatile String context = "startup";
    private long lastStall, lastMemory;
    private final Window.OnFrameMetricsAvailableListener frames = (ignored, metrics, dropped) -> {
        long total = metrics.getMetric(FrameMetrics.TOTAL_DURATION) / 1_000_000;
        if (!closed && foreground && total >= 32) write("frame totalMs=" + total
            + " layoutMs=" + metrics.getMetric(FrameMetrics.LAYOUT_MEASURE_DURATION) / 1_000_000
            + " drawMs=" + metrics.getMetric(FrameMetrics.DRAW_DURATION) / 1_000_000 + " dropped=" + dropped);
    };
    MainThreadMonitor(Window window, File directory) {
        this.window = window; this.directory = directory;
        thread.start(); worker = new Handler(thread.getLooper());
        window.addOnFrameMetricsAvailableListener(frames, worker);
        worker.post(tick);
    }
    void record(String event) { if (!closed) worker.post(() -> write(event)); }
    void context(String value) { context = value; }
    void foreground(boolean value) {
        foreground = value; pending = false; pulseGeneration.incrementAndGet();
        worker.removeCallbacks(tick); worker.post(tick);
    }
    private final Runnable tick = new Runnable() {
        @Override public void run() {
            if (closed) return;
            long now = SystemClock.uptimeMillis();
            if (foreground) {
                if (!pending) {
                    pulse = now; pending = true; long generation = pulseGeneration.incrementAndGet();
                    main.post(() -> { if (pulseGeneration.get() == generation) pending = false; });
                } else if (now - pulse >= 500 && now - lastStall >= 2000) {
                    lastStall = now;
                    StringBuilder stack = new StringBuilder("main-stall delayMs=" + (now - pulse));
                    for (StackTraceElement item : Looper.getMainLooper().getThread().getStackTrace())
                        stack.append("\n  at ").append(item);
                    write(stack.toString());
                }
                if (now - lastMemory >= 30000) {
                    lastMemory = now; Debug.MemoryInfo info = new Debug.MemoryInfo(); Debug.getMemoryInfo(info);
                    write("memory pssKiB=" + info.getTotalPss() + " javaBytes="
                        + (Runtime.getRuntime().totalMemory() - Runtime.getRuntime().freeMemory())
                        + " nativeBytes=" + Debug.getNativeHeapAllocatedSize());
                }
            }
            worker.postDelayed(this, foreground ? 100 : 2000);
        }
    };
    private void write(String event) {
        try {
            directory.mkdirs(); File log = new File(directory, "ui.log");
            if (log.length() > 2 * 1024 * 1024) {
                File previous = new File(directory, "ui.previous.log"); previous.delete(); log.renameTo(previous);
            }
            try (FileOutputStream out = new FileOutputStream(log, true)) {
                out.write((System.currentTimeMillis() + " " + context + " " + event + "\n")
                                .getBytes(StandardCharsets.UTF_8));
            }
        } catch (Exception ignored) { }
    }
    void export(File report, String header, Consumer<File> success, Runnable failure) {
        export(report, header, 0, success, failure);
    }
    /** sinceMs <= 0 retains the existing all-history byte export; a range ends at this request's time. */
    void export(File report, String header, long sinceMs, Consumer<File> success, Runnable failure) {
        long untilMs = System.currentTimeMillis();
        worker.post(() -> {
            try {
                exportLogs(directory, report, header, sinceMs, untilMs);
                main.post(() -> { if (!closed) success.accept(report); });
            } catch (Exception error) { main.post(() -> { if (!closed) failure.run(); }); }
        });
    }
    /** Writes and rotation share this worker, so clearing cannot race an open log write. */
    void clear(Runnable success, Runnable failure) {
        worker.post(() -> {
            try {
                clearLogs(directory);
                main.post(() -> { if (!closed) success.run(); });
            } catch (Exception error) { main.post(() -> { if (!closed) failure.run(); }); }
        });
    }

    // The file operations stay independent of Android callbacks for ordinary use and focused
    // fixture tests. A complete snapshot replaces the requested report only after encoding succeeds.
    static void exportLogs(File directory, File report, String header, long sinceMs, long untilMs)
            throws IOException {
        File destination = report.getAbsoluteFile(), parent = destination.getParentFile();
        if (parent == null || (!parent.isDirectory() && !parent.mkdirs() && !parent.isDirectory()))
            throw new IOException("Cannot create diagnostics export directory");
        for (String name : LOG_NAMES)
            if (destination.getCanonicalFile().equals(new File(directory, name).getCanonicalFile()))
                throw new IOException("Diagnostics exports must not replace an active log");
        File temporary = File.createTempFile(".diagnostics-", ".log", parent);
        try {
            try (FileOutputStream out = new FileOutputStream(temporary)) {
                out.write(header.getBytes(StandardCharsets.UTF_8));
                boolean selected = false;
                for (String name : LOG_NAMES) {
                    File source = new File(directory, name);
                    if (!source.isFile()) continue;
                    if (sinceMs <= 0) {
                        out.write(("\n--- " + name + " ---\n").getBytes(StandardCharsets.UTF_8));
                        try (FileInputStream in = new FileInputStream(source)) {
                            byte[] buffer = new byte[8192]; int count;
                            while ((count = in.read(buffer)) != -1) out.write(buffer, 0, count);
                        }
                        selected = true;
                    } else {
                        // A stack is one timestamped event followed by untimestamped lines.
                        // Do not infer its age from file mtime or leak an excluded event's stack.
                        boolean includeRecord = false, sectionWritten = false;
                        try (BufferedReader in = new BufferedReader(
                                new InputStreamReader(new FileInputStream(source), StandardCharsets.UTF_8))) {
                            String line;
                            while ((line = in.readLine()) != null) {
                                long timestamp = recordTimestamp(line);
                                if (timestamp >= 0) includeRecord = timestamp >= sinceMs && timestamp <= untilMs;
                                if (!includeRecord) continue;
                                if (!sectionWritten) {
                                    out.write(("\n--- " + name + " ---\n").getBytes(StandardCharsets.UTF_8));
                                    sectionWritten = true;
                                }
                                out.write(line.getBytes(StandardCharsets.UTF_8));
                                out.write('\n');
                                selected = true;
                            }
                        }
                    }
                }
                if (sinceMs > 0 && !selected)
                    out.write("\nNo diagnostic log entries are available for the selected time range.\n"
                        .getBytes(StandardCharsets.UTF_8));
                out.getFD().sync();
            }
            if (!temporary.renameTo(destination)) throw new IOException("Cannot publish diagnostics export");
        } finally { temporary.delete(); }
    }

    private static long recordTimestamp(String line) {
        int separator = line.indexOf(' ');
        if (separator <= 0) return -1;
        for (int index = 0; index < separator; ++index)
            if (line.charAt(index) < '0' || line.charAt(index) > '9') return -1;
        try { return Long.parseLong(line.substring(0, separator)); }
        catch (NumberFormatException malformed) { return -1; }
    }

    /** Only the two exact log filenames are owned here; never recurse into other diagnostics. */
    static void clearLogs(File directory) throws IOException {
        for (String name : LOG_NAMES) {
            File log = new File(directory, name);
            if (!log.exists()) continue;
            if (!log.isFile() || !log.delete()) throw new IOException("Cannot clear diagnostics log: " + name);
        }
    }
    void close() {
        closed = true; window.removeOnFrameMetricsAvailableListener(frames);
        worker.removeCallbacks(tick); thread.quitSafely();
    }
}
