// 文件作用：Android IM 身份、消息同步与本地历史；RemoteIMNewMessageNotificationPolicy 负责输入筛选、权限判断或展示规则，本文件给出其实现。
// 主要入口：preview、aggregatedPreview、compact。
package com.kongshang.maichat;

public final class RemoteIMNewMessageNotificationPolicy {
    private RemoteIMNewMessageNotificationPolicy() {
    }

    public static boolean shouldNotify(
        boolean wasInserted,
        boolean activityInForeground
    ) {
        return shouldNotify(wasInserted, activityInForeground, false);
    }

    // DND changes presentation only. Reception, persistence and unread counts
    // stay with the session controller, including while notifications are muted.
    public static boolean shouldNotify(
        boolean wasInserted,
        boolean activityInForeground,
        boolean doNotDisturb
    ) {
        return wasInserted && !activityInForeground && !doNotDisturb;
    }

    public static boolean shouldNotify(
        boolean wasInserted,
        boolean activityInForeground,
        boolean queuedDoNotDisturb,
        long queuedEpoch,
        boolean currentDoNotDisturb,
        long currentEpoch
    ) {
        // Muted arrivals and callbacks invalidated by any mode change are never
        // replayed. Only a new arrival after unmuting may alert again.
        return shouldNotify(wasInserted, activityInForeground, currentDoNotDisturb)
            && !queuedDoNotDisturb && queuedEpoch == currentEpoch;
    }

    public static String preview(RemoteIMMessage message) {
        return preview(message, 80);
    }

    public static String aggregatedPreview(RemoteIMMessage message, int pendingCount) {
        String latest = preview(message);
        return pendingCount <= 1 ? latest : pendingCount + " 条新消息：" + latest;
    }

    static String preview(RemoteIMMessage message, int limit) {
        if (message == null) return "新消息";
        String text = compact(message.text());
        String value;
        if (message.imageAttachment() != null) {
            value = text.isEmpty() || text.startsWith("[图片消息]") ? "图片消息" : text;
        } else if (message.fileAttachment() != null) {
            value = text.isEmpty() || text.startsWith("[文件消息]")
                ? "文件消息"
                : text;
        } else if (message.videoAttachment() != null) {
            value = text.isEmpty() || text.startsWith("[视频消息") ? "视频消息" : text;
        } else if (message.voiceAttachment() != null) {
            value = "语音消息（" + message.voiceAttachment().durationSeconds() + " 秒）";
        } else {
            value = text.isEmpty() ? "新消息" : text;
        }
        int safeLimit = Math.max(1, limit);
        return value.length() > safeLimit ? value.substring(0, safeLimit) + "…" : value;
    }

    private static String compact(String value) {
        return value == null ? "" : value.trim().replaceAll("\\s+", " ");
    }
}
