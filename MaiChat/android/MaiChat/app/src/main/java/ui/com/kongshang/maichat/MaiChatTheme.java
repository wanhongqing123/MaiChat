// 文件作用：Android 跨页面复用的界面组件；MaiChatTheme 负责界面颜色和视觉令牌，本文件给出其实现。
// 主要入口：dp、rounded、gradientAvatar、text。
package com.kongshang.maichat;

import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.widget.TextView;

public final class MaiChatTheme {
    public static int PAGE = Color.rgb(246, 249, 252);
    // 与 iOS grouped 页面共用层级；面板与正文均响应浅色/深色偏好。
    public static int GROUPED_PAGE = Color.rgb(242, 242, 247);
    public static int PANEL = Color.WHITE;
    public static int BORDER = Color.rgb(218, 228, 240);
    public static int TEXT = Color.rgb(14, 21, 37);
    public static int SECONDARY = Color.rgb(100, 117, 143);
    public static final int BLUE = Color.rgb(15, 141, 221);
    public static int BLUE_DARK = Color.rgb(9, 96, 170);
    public static int BLUE_SOFT = Color.rgb(225, 244, 255);
    public static final int GREEN = Color.rgb(16, 152, 83);
    public static int GREEN_SOFT = Color.rgb(216, 251, 230);
    public static int YELLOW_SOFT = Color.rgb(255, 251, 233);
    public static final int YELLOW_BORDER = Color.rgb(253, 207, 88);
    public static final int RED = Color.rgb(220, 38, 38);
    public static final int NAV_MESSAGES = Color.rgb(20, 135, 212);
    public static int NAV_MESSAGES_SOFT = Color.rgb(230, 244, 255);
    public static final int NAV_ASSISTANT = Color.rgb(111, 98, 199);
    public static int NAV_ASSISTANT_SOFT = Color.rgb(240, 237, 255);
    public static final int NAV_CONTACTS = Color.rgb(25, 140, 115);
    public static int NAV_CONTACTS_SOFT = Color.rgb(231, 247, 241);
    public static final int NAV_REMOTE = Color.rgb(83, 111, 158);
    public static int NAV_REMOTE_SOFT = Color.rgb(235, 240, 248);
    public static final int NAV_PROFILE = Color.rgb(83, 101, 122);
    public static int NAV_PROFILE_SOFT = Color.rgb(233, 238, 245);

    private static boolean dark;
    public static boolean isDark() { return dark; }
    public static void configure(Context context) {
        dark = (context.getResources().getConfiguration().uiMode
            & android.content.res.Configuration.UI_MODE_NIGHT_MASK)
            == android.content.res.Configuration.UI_MODE_NIGHT_YES;
        PAGE = dark ? MaiChatTypographyTokens.PAGE_DARK_COLOR : MaiChatTypographyTokens.PAGE_COLOR;
        GROUPED_PAGE = dark ? MaiChatTypographyTokens.GROUPEDPAGE_DARK_COLOR : MaiChatTypographyTokens.GROUPEDPAGE_COLOR;
        PANEL = dark ? MaiChatTypographyTokens.PANEL_DARK_COLOR : MaiChatTypographyTokens.PANEL_COLOR;
        BORDER = dark ? MaiChatTypographyTokens.BORDER_DARK_COLOR : MaiChatTypographyTokens.BORDER_COLOR;
        TEXT = dark ? MaiChatTypographyTokens.TEXT_DARK_COLOR : MaiChatTypographyTokens.TEXT_COLOR;
        SECONDARY = dark ? MaiChatTypographyTokens.SECONDARY_DARK_COLOR : MaiChatTypographyTokens.SECONDARY_COLOR;
        BLUE_SOFT = dark ? MaiChatTypographyTokens.BLUESOFT_DARK_COLOR : MaiChatTypographyTokens.BLUESOFT_COLOR;
        GREEN_SOFT = dark ? MaiChatTypographyTokens.GREENSOFT_DARK_COLOR : MaiChatTypographyTokens.GREENSOFT_COLOR;
        YELLOW_SOFT = dark ? Color.rgb(57,49,22) : Color.rgb(255,251,233);
        BLUE_DARK = dark ? MaiChatTypographyTokens.LINK_DARK_COLOR : Color.rgb(9,96,170);
        NAV_MESSAGES_SOFT = BLUE_SOFT;
        NAV_ASSISTANT_SOFT = dark ? MaiChatTypographyTokens.PURPLESOFT_DARK_COLOR : MaiChatTypographyTokens.PURPLESOFT_COLOR;
        NAV_CONTACTS_SOFT = dark ? MaiChatTypographyTokens.CONTACTSOFT_DARK_COLOR : MaiChatTypographyTokens.CONTACTSOFT_COLOR;
        NAV_REMOTE_SOFT = dark ? Color.rgb(31,39,55) : Color.rgb(235,240,248);
        NAV_PROFILE_SOFT = dark ? MaiChatTypographyTokens.PROFILESOFT_DARK_COLOR : MaiChatTypographyTokens.PROFILESOFT_COLOR;
    }

    private MaiChatTheme() {
    }

    public static int dp(Context context, float value) {
        return Math.round(value * context.getResources().getDisplayMetrics().density);
    }

    public static GradientDrawable rounded(int color, float radiusDp, Context context) {
        GradientDrawable drawable = new GradientDrawable();
        // 旧页面的浅中性面板跟随主题；品牌色、透明覆盖层和白色文字保持各自语义。
        boolean neutralSurface = Color.alpha(color) == 255 && Color.red(color) >= 230
            && Color.green(color) >= 230 && Color.blue(color) >= 230
            && Math.max(Color.red(color), Math.max(Color.green(color), Color.blue(color)))
                - Math.min(Color.red(color), Math.min(Color.green(color), Color.blue(color))) <= 15;
        drawable.setColor(dark && neutralSurface ? PANEL : color);
        drawable.setCornerRadius(dp(context, radiusDp));
        return drawable;
    }

    public static GradientDrawable bordered(
        int fill,
        int stroke,
        float radiusDp,
        Context context
    ) {
        GradientDrawable drawable = rounded(fill, radiusDp, context);
        drawable.setStroke(dp(context, 1), stroke);
        return drawable;
    }

    public static GradientDrawable gradientAvatar(boolean outgoing, Context context) {
        int[] colors = outgoing
            ? new int[]{Color.rgb(91, 155, 255), Color.rgb(30, 64, 175)}
            : new int[]{Color.rgb(45, 212, 191), Color.rgb(15, 118, 110)};
        GradientDrawable drawable = new GradientDrawable(
            GradientDrawable.Orientation.TL_BR,
            colors
        );
        drawable.setCornerRadius(dp(context, 10));
        return drawable;
    }

    public static TextView text(Context context, String value, MaiChatTypographyTokens.Role role, int color) {
        TextView view = new TextView(context);
        view.setText(value);
        MaiChatTypography.apply(view, role);
        view.setTextColor(color);
        view.setGravity(Gravity.CENTER_VERTICAL);
        view.setIncludeFontPadding(false);
        return view;
    }

    public static TextView label(Context context, String value, MaiChatTypographyTokens.Role role, int color) {
        TextView view = text(context, value, role, color);
        if (role.points > MaiChatTypographyTokens.Role.BODY.points) view.setTypeface(MaiChatTypography.semibold());
        return view;
    }

    /** Physical icon glyphs keep their geometry when the user's text preference changes. */
    public static TextView symbol(Context context, String value, float points, int color) {
        TextView view = new TextView(context); view.setText(value);
        view.setTextSize(android.util.TypedValue.COMPLEX_UNIT_PX, dp(context, points));
        view.setTextColor(color); view.setGravity(Gravity.CENTER_VERTICAL);
        view.setIncludeFontPadding(false); return view;
    }

}
