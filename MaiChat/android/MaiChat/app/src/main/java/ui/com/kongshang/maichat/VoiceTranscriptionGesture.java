// 文件作用：用窗口原始坐标解析 iOS 同源的语音发送、取消和编辑目标；不依赖 Android View。
package com.kongshang.maichat;

final class VoiceTranscriptionGesture {
    enum Target { SEND, CANCEL, EDIT }

    static final class Bounds {
        final float left, top, right, bottom;
        Bounds(float left, float top, float right, float bottom) {
            this.left = left; this.top = top; this.right = right; this.bottom = bottom;
        }
        boolean contains(float x, float y, float margin) {
            return right > left && bottom > top && x >= left - margin && x < right + margin
                && y >= top - margin && y < bottom + margin;
        }
    }

    private final boolean assistant;
    private final float density, originX, originY;

    VoiceTranscriptionGesture(boolean assistant, float density, float originX, float originY) {
        this.assistant = assistant; this.density = density;
        this.originX = originX; this.originY = originY;
    }

    Target move(float rawX, float rawY, Bounds cancel, Bounds edit) {
        // 可见按钮的扩大命中区域优先，不能让从右侧开始的手指误触取消兜底。
        if (cancel != null && cancel.contains(rawX, rawY, 16 * density)) return Target.CANCEL;
        if (edit != null && edit.contains(rawX, rawY, 16 * density)) return Target.EDIT;
        float dx = (rawX - originX) / density, dy = (rawY - originY) / density;
        if (dx < -(assistant ? 120 : 70)) return Target.CANCEL;
        if (dx > 70 && dy < -35) return Target.EDIT;
        return Target.SEND;
    }

    static String preview(String text) {
        if (text == null) return "";
        int count = text.codePointCount(0, text.length());
        return count > 180 ? text.substring(text.offsetByCodePoints(0, count - 180)) : text;
    }
}
