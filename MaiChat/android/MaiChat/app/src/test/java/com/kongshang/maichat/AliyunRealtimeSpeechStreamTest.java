// 文件作用：以内存 socket 走阿里流的完整命令/音频/停止/最终结果链，不联网。
package com.kongshang.maichat;

import java.io.File;
import java.nio.file.Files;
import java.lang.reflect.Proxy;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import org.json.JSONObject;
import org.junit.Test;
import static org.junit.Assert.*;

public class AliyunRealtimeSpeechStreamTest {
    private static final String TASK = "00000000000040008000000000000001";
    private static final class Socket implements RealtimeSpeechSocket {
        final CountDownLatch start = new CountDownLatch(1), stop = new CountDownLatch(1), audio = new CountDownLatch(1), cancelled = new CountDownLatch(1);
        CurlRealtimeSpeechWebSocket.Listener listener;
        JSONObject startMessage;
        int bytes;
        public boolean sendText(String text) {
            try {
                JSONObject command = new JSONObject(text);
                String name = command.getJSONObject("header").getString("name");
                if (name.equals("StartTranscription")) { startMessage = command; start.countDown(); }
                if (name.equals("StopTranscription")) stop.countDown();
                return true;
            } catch (Exception invalid) { throw new AssertionError(invalid); }
        }
        public boolean sendBinary(byte[] packet) { bytes += packet.length; audio.countDown(); return true; }
        public void cancel() { cancelled.countDown(); }
        void event(String name, JSONObject payload, int status) throws Exception {
            JSONObject event = new JSONObject().put("header", new JSONObject().put("task_id", TASK)
                .put("namespace", "SpeechTranscriber").put("name", name).put("status", status));
            if (payload != null) event.put("payload", payload);
            listener.message(event.toString());
        }
    }
    private Object stream(File file, Socket socket, RealtimeSpeechStream.Listener listener) throws Exception {
        Class<?> type;
        try { type = Class.forName("com.kongshang.maichat.AliyunRealtimeSpeechStream"); }
        catch (ClassNotFoundException missing) { throw new AssertionError("Aliyun requires a real start/audio/stop/final adapter", missing); }
        Class<?> factory = Class.forName(type.getName() + "$SocketFactory");
        Object sockets = Proxy.newProxyInstance(factory.getClassLoader(), new Class<?>[] {factory}, (p,m,a) -> {
            socket.listener = (CurlRealtimeSpeechWebSocket.Listener) a[0]; return socket;
        });
        return type.getDeclaredConstructor(File.class, String.class, String.class, RealtimeSpeechStream.Listener.class, factory)
            .newInstance(file, TASK, "fixture-app-key", listener, sockets);
    }
    private void call(Object stream, String method) throws Exception { stream.getClass().getDeclaredMethod(method).invoke(stream); }
    @Test public void pcmIsSentOnlyAfterStartedAndFinalWaitsForCompleted() throws Exception {
        File file = File.createTempFile("maichat-aliyun-pcm", ".pcm"); Files.write(file.toPath(), new byte[3200]);
        Socket socket = new Socket(); CountDownLatch done = new CountDownLatch(1), partial = new CountDownLatch(1);
        AtomicReference<String> result = new AtomicReference<>(); AtomicInteger failures = new AtomicInteger();
        Object stream = stream(file, socket, new RealtimeSpeechStream.Listener() {
            public void partial(String text) { partial.countDown(); }
            public void finished(String text) { result.set(text); done.countDown(); }
            public void failed() { failures.incrementAndGet(); done.countDown(); }
        });
        try {
            assertTrue(socket.start.await(2, TimeUnit.SECONDS)); assertEquals(0, socket.bytes);
            JSONObject payload = socket.startMessage.getJSONObject("payload");
            assertEquals("pcm", payload.getString("format")); assertEquals(16000, payload.getInt("sample_rate"));
            assertEquals(TASK, socket.startMessage.getJSONObject("header").getString("task_id"));
            socket.event("TranscriptionStarted", null, 20000000); assertTrue(socket.audio.await(2, TimeUnit.SECONDS));
            socket.event("TranscriptionResultChanged", new JSONObject().put("index", 1).put("time", 100).put("result", "partial"), 20000000);
            assertTrue(partial.await(2, TimeUnit.SECONDS)); call(stream, "endAudio");
            assertTrue(socket.stop.await(2, TimeUnit.SECONDS)); assertEquals(3200, socket.bytes); assertNull(result.get());
            socket.event("SentenceEnd", new JSONObject().put("index", 1).put("time", 200).put("result", "final corrected"), 20000000);
            socket.event("TranscriptionCompleted", null, 20000000);
            assertTrue(done.await(2, TimeUnit.SECONDS)); assertEquals("final corrected", result.get()); assertEquals(0, failures.get());
        } finally { call(stream, "cancel"); file.delete(); }
    }
    @Test public void serviceFailureNeverPublishesPartialAsSuccess() throws Exception {
        File file = File.createTempFile("maichat-aliyun-error", ".pcm"); Socket socket = new Socket();
        CountDownLatch failed = new CountDownLatch(1); AtomicInteger finished = new AtomicInteger();
        Object stream = stream(file, socket, new RealtimeSpeechStream.Listener() {
            public void partial(String text) { }
            public void finished(String text) { finished.incrementAndGet(); }
            public void failed() { failed.countDown(); }
        });
        try {
            assertTrue(socket.start.await(2, TimeUnit.SECONDS)); socket.event("TaskFailed", null, 40000001);
            assertTrue(failed.await(2, TimeUnit.SECONDS)); assertEquals(0, finished.get());
        } finally { call(stream, "cancel"); file.delete(); }
    }
    @Test public void cancellingNeverSendsStopOrDeliversALateCompletedResult() throws Exception {
        File file = File.createTempFile("maichat-aliyun-cancel", ".pcm"); Socket socket = new Socket();
        CountDownLatch notified = new CountDownLatch(1);
        Object stream = stream(file, socket, new RealtimeSpeechStream.Listener() {
            public void partial(String text) { }
            public void finished(String text) { notified.countDown(); }
            public void failed() { notified.countDown(); }
        });
        try {
            assertTrue(socket.start.await(2, TimeUnit.SECONDS)); call(stream, "cancel");
            assertTrue(socket.cancelled.await(2, TimeUnit.SECONDS));
            socket.event("TranscriptionCompleted", null, 20000000);
            assertFalse(notified.await(50, TimeUnit.MILLISECONDS));
            assertEquals(1, socket.stop.getCount());
        } finally { call(stream, "cancel"); file.delete(); }
    }
}
