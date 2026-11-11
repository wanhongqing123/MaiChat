// 文件作用：普通设置页的本机外观和通知偏好；不承载账号、模型或计费状态。
package com.kongshang.maichat;

import android.content.Context;
import android.content.SharedPreferences;
import android.content.res.Configuration;

public final class MaiChatPreferences {
    private MaiChatPreferences() { }
    private static final Object notificationLock = new Object();
    private static long notificationEpoch;
    private static SharedPreferences store(Context context) {
        return context.getSharedPreferences("maichat.appearance", Context.MODE_PRIVATE);
    }
    public static String theme(Context context) { return store(context).getString("theme", "system"); }
    public static String font(Context context) { return store(context).getString("font", MaiChatTypographyTokens.DEFAULT_PRESET); }
    public static String fontFamily(Context context) { return store(context).getString(MaiChatFontFamilyTokens.PREFERENCE_KEY, "system"); }
    public static String fontFamilyTitle(Context context) { return MaiChatFontFamilyTokens.Family.fromId(fontFamily(context)).title; }
    public static boolean haptics(Context context) { return store(context).getBoolean("haptics", true); }
    // Keep this device's notification choice outside account or session sync.
    // Ordinary Android OS backup/migration retains its existing app policy.
    private static SharedPreferences notificationStore(Context context) {
        return context.getSharedPreferences("maichat.notifications", Context.MODE_PRIVATE);
    }
    public static boolean doNotDisturb(Context context) {
        return messageNotificationState(context).doNotDisturb;
    }
    public static void doNotDisturb(Context context, boolean enabled) {
        synchronized (notificationLock) {
            SharedPreferences settings = notificationStore(context);
            if (settings.getBoolean("maichat.preferences.do-not-disturb", false) == enabled) return;
            settings.edit().putBoolean("maichat.preferences.do-not-disturb", enabled).apply();
            ++notificationEpoch;
        }
    }
    public static final class MessageNotificationState {
        public final boolean doNotDisturb;
        public final long epoch;
        private MessageNotificationState(boolean doNotDisturb, long epoch) {
            this.doNotDisturb = doNotDisturb;
            this.epoch = epoch;
        }
    }
    public static MessageNotificationState messageNotificationState(Context context) {
        // Capture mode and process-local generation together. A mode round trip
        // invalidates old UI callbacks even when its final boolean is unchanged.
        synchronized (notificationLock) {
            return new MessageNotificationState(notificationStore(context)
                .getBoolean("maichat.preferences.do-not-disturb", false), notificationEpoch);
        }
    }
    public static void set(Context context, String key, String value) {
        store(context).edit().putString(key, value).apply();
    }
    public static void haptics(Context context, boolean enabled) {
        store(context).edit().putBoolean("haptics", enabled).apply();
    }
    public static String themeTitle(Context context) {
        String value = theme(context);
        return "dark".equals(value) ? "深色" : "light".equals(value) ? "浅色" : "跟随系统";
    }
    public static String fontTitle(Context context) {
        String value = font(context);
        return "extraLarge".equals(value) ? "特大" : "large".equals(value) ? "大" : "standard".equals(value) ? "标准" : "跟随系统";
    }
    // Configuration 传给系统控件；自绘和 Markdown 也读取这个倍率，使用同一套线性比例。
    public static Context wrap(Context context) {
        Configuration config = new Configuration(context.getResources().getConfiguration());
        String theme = theme(context), font = font(context);
        if (!"system".equals(theme)) config.uiMode = (config.uiMode & ~Configuration.UI_MODE_NIGHT_MASK)
            | ("dark".equals(theme) ? Configuration.UI_MODE_NIGHT_YES : Configuration.UI_MODE_NIGHT_NO);
        config.fontScale = MaiChatTypographyTokens.scale(font, config.fontScale);
        return context.createConfigurationContext(config);
    }
}
