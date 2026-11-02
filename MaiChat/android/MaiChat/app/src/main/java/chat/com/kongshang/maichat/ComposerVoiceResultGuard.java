// 文件作用：在等待 ASR final 期间按编辑和提交代际保护草稿；只允许录音自己的 partial 写入。
package com.kongshang.maichat;

final class ComposerVoiceResultGuard {
    private long revision;
    private boolean writingTranscript;

    long snapshot() { return revision; }
    boolean edited() {
        if (writingTranscript) return false;
        revision++;
        return true;
    }
    void submitted() { revision++; }
    boolean accepts(long token, boolean canSubmit) { return revision == token && canSubmit; }
    void writeTranscript(Runnable edit) {
        boolean previous = writingTranscript;
        writingTranscript = true;
        try { edit.run(); }
        finally { writingTranscript = previous; }
    }
}
