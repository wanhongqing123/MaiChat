// 文件作用：Android 媒体播放、预览和编辑；VoiceRecordingController 负责流程编排、异步回调和状态转换，本文件给出其实现。
// 主要入口：finishOnWorker、deliver。
package com.kongshang.maichat;

import android.media.MediaRecorder;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.SystemClock;
import java.io.File;
import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.atomic.AtomicBoolean;

/** One microphone per clip; Tencent/direct voice keep AAC, Aliyun transcription captures PCM. */
final class VoiceRecordingController {
    interface AsrTicketProvider {
        MaiChatAuthClient.RealtimeAsrTicket issue() throws Exception;
    }
    interface AliyunCredentialProvider { AliyunAsrCredential issue() throws Exception; }
    interface Completion {
        void finished(File file, int seconds);
        void failed();
        default void partial(String text) { }
        default void recognized(String text, File file, int seconds) { finished(file, seconds); }
    }
    private static final class Clip {
        final File file; final Completion completion;
        final SpeechRecordingPlan plan;
        RealtimeSpeechStream stream;
        String text;
        int seconds;
        boolean released, resultReady, asrFailed, cancelled, delivered;
        Clip(File file, Completion completion, SpeechRecordingPlan plan) {
            this.file = file; this.completion = completion; this.plan = plan;
        }
    }
    private final HandlerThread thread = new HandlerThread("MaiChat-audio");
    private final Handler worker;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final ExecutorService ticketWorker = Executors.newSingleThreadExecutor(action ->
        new Thread(action, "MaiChat-asr-ticket"));
    private final RemoteIMMediaStore store;
    private final java.util.function.Consumer<String> diagnostic;
    private final AtomicBoolean active = new AtomicBoolean();
    private final Set<Clip> clips = new HashSet<>();
    private volatile boolean closed;
    private MediaRecorder recorder;
    private PcmVoiceRecorder pcmRecorder;
    private volatile java.util.function.Supplier<SpeechRecognitionProvider> providerSelection =
        () -> SpeechRecognitionProvider.TENCENT;
    private volatile AliyunCredentialProvider aliyunCredentials;
    private Clip recording;
    private long started;
    VoiceRecordingController(RemoteIMMediaStore store) { this(store, event -> { }); }
    VoiceRecordingController(RemoteIMMediaStore store, java.util.function.Consumer<String> diagnostic) {
        this.diagnostic = diagnostic;
        this.store = store; thread.start(); worker = new Handler(thread.getLooper());
    }
    void configureTranscription(java.util.function.Supplier<SpeechRecognitionProvider> selection,
                                AliyunCredentialProvider credentialProvider) {
        providerSelection = selection == null ? () -> SpeechRecognitionProvider.TENCENT : selection;
        aliyunCredentials = credentialProvider;
    }
    void start(Completion callback) { tryStart(callback); }
    boolean tryStart(Completion callback) {
        return tryStart(callback, null);
    }
    boolean tryStart(Completion callback, AsrTicketProvider ticketProvider) {
        // 快照属于这次按住；设置修改不会替换正在录音或等待final的服务。
        SpeechRecordingPlan plan = new SpeechRecordingPlan(
            ticketProvider == null ? SpeechRecognitionProvider.TENCENT : providerSelection.get(), ticketProvider != null);
        AliyunCredentialProvider selectedAliyun = aliyunCredentials;
        if (closed || !active.compareAndSet(false, true)) return false;
        recordDiagnostic("asr-recorder-queued");
        worker.post(() -> {
            if (closed) { active.set(false); return; }
            try {
                Clip clip = new Clip(plan.usesPcm() ? store.createPcmVoiceRecordingFile()
                    : store.createRealtimeVoiceRecordingFile(), callback, plan);
                recording = clip; clips.add(clip);
                if (plan.usesPcm()) {
                    pcmRecorder = new PcmVoiceRecorder(clip.file, () -> worker.post(() -> {
                        if (closed || clip.cancelled || recording != clip) return;
                        recordDiagnostic("asr-pcm-capture-failed");
                        finishOnWorker(true);
                        main.post(() -> { if (!closed) callback.failed(); });
                    }));
                    pcmRecorder.start();
                } else {
                    recorder = new MediaRecorder();
                    recorder.setAudioSource(MediaRecorder.AudioSource.MIC);
                    recorder.setOutputFormat(MediaRecorder.OutputFormat.AAC_ADTS);
                    recorder.setAudioEncoder(MediaRecorder.AudioEncoder.AAC);
                    recorder.setAudioSamplingRate(16000);
                    recorder.setAudioChannels(1);
                    recorder.setAudioEncodingBitRate(32000);
                    recorder.setOutputFile(clip.file.getAbsolutePath());
                    recorder.prepare(); recorder.start();
                }
                started = SystemClock.elapsedRealtime();
                recordDiagnostic("asr-recorder-started");
                if (ticketProvider != null) {
                    recordDiagnostic(plan.usesPcm() ? "asr-provider-aliyun" : "asr-provider-tencent");
                    requestAsrTicket(clip, ticketProvider, selectedAliyun);
                }
                else clip.resultReady = true;
            } catch (Exception error) {
                recordDiagnostic("asr-recorder-failed class=" + error.getClass().getSimpleName());
                finishOnWorker(true);
                main.post(() -> { if (!closed) callback.failed(); });
            }
        });
        return true;
    }
    private void requestAsrTicket(Clip clip, AsrTicketProvider provider, AliyunCredentialProvider aliyunProvider) {
        recordDiagnostic("asr-ticket-request");
        try {
            ticketWorker.execute(() -> {
                MaiChatAuthClient.RealtimeAsrTicket ticket = null;
                AliyunAsrCredential aliyun = null;
                try {
                    if (clip.plan.usesPcm()) {
                        if (aliyunProvider == null) throw new IllegalStateException("Aliyun credential provider is missing");
                        aliyun = aliyunProvider.issue();
                    } else ticket = provider.issue();
                    recordDiagnostic("asr-ticket-ready");
                }
                catch (Exception error) {
                    recordDiagnostic("asr-ticket-failed class=" + error.getClass().getSimpleName());
                }
                MaiChatAuthClient.RealtimeAsrTicket issued = ticket;
                AliyunAsrCredential issuedAliyun = aliyun;
                if (!worker.post(() -> {
                    if (closed || clip.cancelled || !clips.contains(clip)) return;
                    if (clip.plan.usesPcm() ? issuedAliyun == null : issued == null) {
                        clip.asrFailed = true; clip.resultReady = true;
                        deliver(clip);
                        return;
                    }
                    try {
                        TencentRealtimeSpeechStream.Listener listener = new TencentRealtimeSpeechStream.Listener() {
                                @Override public void diagnostic(String event) { recordDiagnostic(event); }
                                @Override public void partial(String text) {
                                    main.post(() -> {
                                        if (!closed && !clip.cancelled) clip.completion.partial(text);
                                    });
                                }
                                @Override public void finished(String text) {
                                    worker.post(() -> {
                                        if (clip.cancelled || closed) return;
                                        clip.text = text; clip.resultReady = true; deliver(clip);
                                    });
                                }
                                @Override public void failed() {
                                    // 空 final 仍是正常无文字；网络/服务失败单独交给界面提示。
                                    worker.post(() -> {
                                        if (clip.cancelled || closed) return;
                                        clip.asrFailed = true; clip.resultReady = true; deliver(clip);
                                    });
                                }
                            };
                        clip.stream = clip.plan.usesPcm()
                            ? new AliyunRealtimeSpeechStream(clip.file, issuedAliyun, listener)
                            : new TencentRealtimeSpeechStream(clip.file, issued, listener);
                        if (clip.released) clip.stream.endAudio();
                    } catch (RuntimeException error) {
                        recordDiagnostic("asr-stream-start-failed class=" + error.getClass().getSimpleName());
                        clip.asrFailed = true; clip.resultReady = true;
                        deliver(clip);
                    }
                })) recordDiagnostic("asr-worker-stopped");
            });
        } catch (RejectedExecutionException error) {
            clip.asrFailed = true; clip.resultReady = true;
            deliver(clip);
        }
    }
    void finish(boolean cancel) { if (!closed) worker.post(() -> finishOnWorker(cancel)); }
    private void recordDiagnostic(String event) {
        // Release 现场只记录固定阶段、数字码和异常类型；严禁响应原文、URL、凭据和转写文字。
        if (event == null || !event.matches("asr-[a-z-]+(?: (?:curl|code|chars|samples)=-?[0-9]+| class=[A-Za-z0-9_$]+)?"))
            return;
        diagnostic.accept(event);
        android.util.Log.i("MaiChatASR", event);
    }
    private void finishOnWorker(boolean cancel) {
        Clip clip = recording;
        boolean succeeded = recorder != null || pcmRecorder != null;
        if (pcmRecorder != null) {
            succeeded = pcmRecorder.finish(); pcmRecorder = null;
        }
        if (recorder != null) {
            try { recorder.stop(); } catch (RuntimeException error) { succeeded = false; }
            recorder.release(); recorder = null;
        }
        recording = null; active.set(false);
        if (clip == null) return;
        clip.seconds = Math.max(1, (int) ((SystemClock.elapsedRealtime() - started) / 1000));
        clip.released = true;
        if (cancel || closed || !succeeded) {
            recordDiagnostic(cancel || closed ? "asr-recording-cancelled" : "asr-recorder-stop-failed");
            clip.cancelled = true;
            if (clip.stream != null) clip.stream.cancel();
            clip.file.delete(); clips.remove(clip);
            if (!cancel && !closed) main.post(clip.completion::failed);
            return;
        }
        if (clip.stream != null) clip.stream.endAudio();
        deliver(clip);
    }
    private void deliver(Clip clip) {
        if (!clip.released || !clip.resultReady || clip.delivered || clip.cancelled || closed) return;
        clip.delivered = true; clips.remove(clip);
        if (clip.asrFailed) {
            clip.file.delete();
            main.post(() -> { if (!closed) clip.completion.failed(); });
            return;
        }
        main.post(() -> {
            if (closed || clip.cancelled) return;
            if (clip.text != null && !clip.text.trim().isEmpty())
                clip.completion.recognized(clip.text.trim(), clip.file, clip.seconds);
            else clip.completion.finished(clip.file, clip.seconds);
        });
    }
    void close() {
        closed = true;
        ticketWorker.shutdownNow();
        worker.post(() -> {
            finishOnWorker(true);
            for (Clip clip : clips) {
                clip.cancelled = true;
                if (clip.stream != null)
                    clip.stream.cancel();
                clip.file.delete();
            }
            clips.clear(); thread.quitSafely();
        });
    }
}
