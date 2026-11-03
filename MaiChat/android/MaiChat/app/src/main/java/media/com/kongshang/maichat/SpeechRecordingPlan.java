// 文件作用：录音开始时捕获服务和音频格式；设置变化仅影响下一轮，直接语音保持 AAC。
package com.kongshang.maichat;

final class SpeechRecordingPlan {
    private final SpeechRecognitionProvider provider;
    private final boolean transcribe;
    SpeechRecordingPlan(SpeechRecognitionProvider selected, boolean transcribe) {
        this.transcribe = transcribe;
        provider = transcribe && selected == SpeechRecognitionProvider.ALIYUN
            ? SpeechRecognitionProvider.ALIYUN : SpeechRecognitionProvider.TENCENT;
    }
    SpeechRecognitionProvider provider() { return provider; }
    boolean usesPcm() { return transcribe && provider == SpeechRecognitionProvider.ALIYUN; }
}
