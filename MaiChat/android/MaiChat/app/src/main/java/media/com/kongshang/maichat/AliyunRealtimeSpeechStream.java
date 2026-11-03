// 文件作用：阿里 SpeechTranscriber WebSocket 直连适配；PCM 排完后 Stop，真实 Completed 才交付。
// 传输复用 native curl；token 只在固定 X-NLS-Token 头，诊断不含原文或凭据。
package com.kongshang.maichat;

import java.io.File;
import java.util.UUID;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import org.json.JSONObject;

final class AliyunRealtimeSpeechStream implements RealtimeSpeechStream {
    interface SocketFactory { RealtimeSpeechSocket open(CurlRealtimeSpeechWebSocket.Listener listener) throws Exception; }
    private static final String ENDPOINT = "wss://nls-gateway-cn-shanghai.aliyuncs.com/ws/v1";
    private final ScheduledExecutorService worker = Executors.newSingleThreadScheduledExecutor();
    private final File audio;
    private final String taskId, appKey;
    private final Listener listener;
    private final AliyunSpeechTranscript transcript;
    private RealtimeSpeechSocket socket;
    private PcmAudioReader reader;
    private ScheduledFuture<?> pumping;
    private boolean ready, audioEnded, endSent, finished, sentAudio;
    private long readyAt;
    private String previous = "";

    AliyunRealtimeSpeechStream(File audio, AliyunAsrCredential credential, Listener listener) {
        this(audio, id(), credential.appKey, listener, callback -> new CurlRealtimeSpeechWebSocket(
            ENDPOINT, credential.token, CurlRealtimeSpeechWebSocket.TokenHeader.ALIYUN, callback));
    }
    // 离线合同测试注入内存 socket，正式构造只使用上面的固定服务地址与 native curl。
    AliyunRealtimeSpeechStream(File audio, String taskId, String appKey, Listener listener, SocketFactory sockets) {
        this.audio = audio; this.taskId = taskId; this.appKey = appKey; this.listener = listener;
        transcript = new AliyunSpeechTranscript(taskId);
        worker.execute(() -> {
            try {
                listener.diagnostic("asr-aliyun-start");
                socket = sockets.open(new CurlRealtimeSpeechWebSocket.Listener() {
                    public void message(String json) { dispatch(() -> receive(json)); }
                    public void failed(int code) {
                        dispatch(() -> { listener.diagnostic("asr-aliyun-transport-failed curl=" + code); finish(null, true); });
                    }
                    public void closed() {
                        dispatch(() -> { if (!finished) { listener.diagnostic("asr-aliyun-socket-closed"); finish(null, true); } });
                    }
                });
                JSONObject payload = new JSONObject().put("format", "pcm").put("sample_rate", 16000)
                    .put("enable_intermediate_result", true).put("enable_punctuation_prediction", true)
                    .put("enable_inverse_text_normalization", true);
                if (!socket.sendText(command("StartTranscription", payload))) { finish(null, true); return; }
                worker.schedule(() -> {
                    if (!ready && !finished) { listener.diagnostic("asr-aliyun-ready-timeout"); finish(null, true); }
                }, 6, TimeUnit.SECONDS);
            } catch (Exception failure) {
                listener.diagnostic("asr-aliyun-setup-failed class=" + failure.getClass().getSimpleName());
                finish(null, true);
            }
        });
    }
    private static String id() { return UUID.randomUUID().toString().replace("-", ""); }
    private String command(String name, JSONObject payload) throws Exception {
        JSONObject value = new JSONObject().put("header", new JSONObject().put("message_id", id())
            .put("task_id", taskId).put("namespace", "SpeechTranscriber").put("name", name).put("appkey", appKey));
        if (payload != null) value.put("payload", payload);
        return value.toString();
    }
    private void dispatch(Runnable action) {
        if (worker.isShutdown()) return;
        try { worker.execute(action); } catch (java.util.concurrent.RejectedExecutionException ignored) { }
    }
    private void receive(String json) {
        if (finished) return;
        try {
            JSONObject event = new JSONObject(json);
            if (!transcript.accept(event)) return;
            if (transcript.failed()) {
                JSONObject header = event.optJSONObject("header");
                listener.diagnostic("asr-aliyun-service-failed code=" + (header == null ? -1 : header.optInt("status", -1)));
                finish(null, true); return;
            }
            if (!ready && transcript.started()) {
                ready = true; readyAt = System.nanoTime(); reader = new PcmAudioReader(audio);
                listener.diagnostic("asr-aliyun-ready");
                pumping = worker.scheduleAtFixedRate(this::pump, 0, 200, TimeUnit.MILLISECONDS);
            }
            String text = transcript.text();
            if (!text.equals(previous)) {
                previous = text; listener.diagnostic("asr-aliyun-partial chars=" + text.length()); listener.partial(text);
            }
            if (transcript.complete()) {
                listener.diagnostic("asr-aliyun-server-final chars=" + text.length());
                finish(endSent ? text : null, true);
            }
        } catch (Exception failure) {
            listener.diagnostic("asr-aliyun-response-failed class=" + failure.getClass().getSimpleName());
            finish(null, true);
        }
    }
    private void pump() {
        if (finished || !ready || endSent) return;
        try {
            long budget = ((System.nanoTime() - readyAt) / 1_000_000 + 200) * 16;
            byte[] packet = reader.readUntilSample(budget);
            if (packet.length > 0 && !socket.sendBinary(packet)) { finish(null, true); return; }
            if (packet.length > 0 && !sentAudio) {
                sentAudio = true; listener.diagnostic("asr-aliyun-audio-sent samples=" + reader.samples());
            }
            if (audioEnded && packet.length == 0 && !reader.atEnd() && budget - reader.samples() > 8192) {
                finish(null, true); return;
            }
            // read cap 保证每次至多200ms；正常停止继续按实时率排完所有完整尾样本。
            if (audioEnded && reader.atEnd()) {
                endSent = true;
                if (!socket.sendText(command("StopTranscription", null))) { finish(null, true); return; }
                listener.diagnostic("asr-aliyun-end-sent samples=" + reader.samples());
                if (pumping != null) pumping.cancel(false);
                worker.schedule(() -> {
                    if (!finished) { listener.diagnostic("asr-aliyun-end-timeout"); finish(null, true); }
                }, 10, TimeUnit.SECONDS);
            }
        } catch (Exception failure) {
            listener.diagnostic("asr-aliyun-audio-failed class=" + failure.getClass().getSimpleName());
            finish(null, true);
        }
    }
    @Override public void endAudio() { dispatch(() -> { audioEnded = true; pump(); }); }
    @Override public void cancel() { dispatch(() -> { transcript.cancel(); finish(null, false); }); }
    private void finish(String text, boolean notify) {
        if (finished) return;
        finished = true;
        if (pumping != null) pumping.cancel(false);
        if (socket != null) socket.cancel();
        if (reader != null) try { reader.close(); } catch (Exception ignored) { }
        if (notify) {
            if (text == null) listener.failed(); else listener.finished(text);
        }
        worker.shutdownNow();
    }
}
