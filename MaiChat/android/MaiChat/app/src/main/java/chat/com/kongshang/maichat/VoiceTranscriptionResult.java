// 文件作用：保存一次转写松手的三态意图；只有真实 final 可以交付一次。
package com.kongshang.maichat;

final class VoiceTranscriptionResult {
    private VoiceTranscriptionOverlay.Action action;
    private boolean terminal;

    boolean release(VoiceTranscriptionOverlay.Action requested) {
        if (terminal || action != null || requested == null) return false;
        action = requested;
        if (requested == VoiceTranscriptionOverlay.Action.CANCEL) terminal = true;
        return true;
    }
    VoiceTranscriptionOverlay.Action finalAction(String text, boolean current) {
        if (terminal || action == null) return null;
        terminal = true;
        return current && text != null && !text.trim().isEmpty() ? action : null;
    }
    void invalidate() { terminal = true; }
}
