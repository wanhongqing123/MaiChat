// 自动生成自 MaiChat/Presentation/typography.json；修改定义后运行 generate-typography.py。
package com.kongshang.maichat;

public final class MaiChatTypographyTokens {
    private MaiChatTypographyTokens() {}
    public enum Role {
        BODY(16.000f), DETAIL(14.000f), METADATA(12.000f), HEADING1(22.000f), HEADING2(20.000f), HEADING3(18.000f), TITLE(20.000f), SECTION(18.000f), DISPLAY(28.000f), HERO(32.000f);
        public final float points;
        Role(float points) { this.points = points; }
    }
    public static final String DEFAULT_PRESET = "standard";
    public static float scale(String preset, float systemScale) {
        switch (preset) {
            case "system": return Float.isFinite(systemScale) && systemScale > 0 ? systemScale : 1;
            case "standard": return 1.0f;
            case "large": return 1.125f;
            case "extraLarge": return 1.25f;
            default: return 1;
        }
    }
    public static float headingRatio(int level) {
        return level == 1 ? 1.375f : level == 2 ? 1.25f : level == 3 ? 1.125f : 1f;
    }
    public static final int TEXT_COLOR = 0xff0e1525;
    public static final int SECONDARY_COLOR = 0xff64758f;
    public static final int LINK_COLOR = 0xff0b67b7;
    public static final int HEADING_COLOR = 0xff1c4f8a;
    public static final int CODE_COLOR = 0xff6b3b97;
    public static final int CODEBACKGROUND_COLOR = 0xfff4f6f9;
    public static final int INLINECODEBACKGROUND_COLOR = 0xfff2edfa;
    public static final int TABLEHEADER_COLOR = 0xffe8f0f7;
    public static final int TABLEALTERNATE_COLOR = 0xfff5f7fa;
    public static final int BORDER_COLOR = 0xffdae4f0;
    public static final int QUOTETEXT_COLOR = 0xff475569;
    public static final int QUOTEBACKGROUND_COLOR = 0xffeaf3fd;
    public static final int QUOTEACCENT_COLOR = 0xff2873c7;
    public static final int PAGE_COLOR = 0xfff6f9fc;
    public static final int GROUPEDPAGE_COLOR = 0xfff2f2f7;
    public static final int PANEL_COLOR = 0xffffffff;
    public static final int INCOMINGBUBBLE_COLOR = 0xfffafcfe;
    public static final int OUTGOINGBUBBLE_COLOR = 0xffeaf4ff;
    public static final int BLUESOFT_COLOR = 0xffe1f4ff;
    public static final int GREENSOFT_COLOR = 0xffd8fbe6;
    public static final int PURPLESOFT_COLOR = 0xfff0edff;
    public static final int CONTACTSOFT_COLOR = 0xffe7f7f1;
    public static final int PROFILESOFT_COLOR = 0xffe9eef5;
    public static final int NOTE_COLOR = 0xff1f64b0;
    public static final int TIP_COLOR = 0xff16836b;
    public static final int IMPORTANT_COLOR = 0xff7547a8;
    public static final int WARNING_COLOR = 0xff9c640f;
    public static final int CAUTION_COLOR = 0xffba3d40;
    public static final int TEXT_DARK_COLOR = 0xffe8edf4;
    public static final int SECONDARY_DARK_COLOR = 0xffa8b4c8;
    public static final int LINK_DARK_COLOR = 0xff79bfff;
    public static final int HEADING_DARK_COLOR = 0xffa4caff;
    public static final int CODE_DARK_COLOR = 0xffd1acf3;
    public static final int CODEBACKGROUND_DARK_COLOR = 0xff242a34;
    public static final int INLINECODEBACKGROUND_DARK_COLOR = 0xff332b43;
    public static final int TABLEHEADER_DARK_COLOR = 0xff283647;
    public static final int TABLEALTERNATE_DARK_COLOR = 0xff22262d;
    public static final int BORDER_DARK_COLOR = 0xff3a404c;
    public static final int QUOTETEXT_DARK_COLOR = 0xffc5cfdf;
    public static final int QUOTEBACKGROUND_DARK_COLOR = 0xff1c2c3e;
    public static final int QUOTEACCENT_DARK_COLOR = 0xff79bfff;
    public static final int PAGE_DARK_COLOR = 0xff121214;
    public static final int GROUPEDPAGE_DARK_COLOR = 0xff000000;
    public static final int PANEL_DARK_COLOR = 0xff1c1c1e;
    public static final int INCOMINGBUBBLE_DARK_COLOR = 0xff1c1c1e;
    public static final int OUTGOINGBUBBLE_DARK_COLOR = 0xff1b3049;
    public static final int BLUESOFT_DARK_COLOR = 0xff133044;
    public static final int GREENSOFT_DARK_COLOR = 0xff143524;
    public static final int PURPLESOFT_DARK_COLOR = 0xff282240;
    public static final int CONTACTSOFT_DARK_COLOR = 0xff15332a;
    public static final int PROFILESOFT_DARK_COLOR = 0xff232830;
    public static final int NOTE_DARK_COLOR = 0xff79bfff;
    public static final int TIP_DARK_COLOR = 0xff67d7b1;
    public static final int IMPORTANT_DARK_COLOR = 0xffd1acf3;
    public static final int WARNING_DARK_COLOR = 0xfff1c36b;
    public static final int CAUTION_DARK_COLOR = 0xffff9a9f;
    public static final float LINEGAP_RATIO = 0.375f;
    public static final float PARAGRAPHGAP_RATIO = 1.0f;
    public static final float LISTGAP_RATIO = 0.5f;
    public static final float TABLEPADDINGH_RATIO = 0.75f;
    public static final float TABLEPADDINGV_RATIO = 0.625f;
    public static final float CONTENTPADDING_RATIO = 0.75f;
    public static final float RADIUS_RATIO = 0.625f;
}

final class MaiChatFontFamilyTokens {
    private MaiChatFontFamilyTokens() {}
    static final String PREFERENCE_KEY = "maichat.preferences.font-family";
    enum Family {
        SYSTEM("system", "系统默认", "", "", "", "", "", false),
        NOTOSANS("notoSans", "黑体（Noto Sans SC）", "Noto Sans SC", "NotoSansSC-Regular", "NotoSansSC-Medium", "NotoSansSC-Medium", "NotoSansSC-Bold", false),
        NOTOSERIF("notoSerif", "宋体（Noto Serif SC）", "Noto Serif SC", "NotoSerifSC-Regular", "NotoSerifSC-Medium", "NotoSerifSC-SemiBold", "NotoSerifSC-Bold", false),
        MICROSOFTYAHEI("microsoftYaHei", "微软雅黑", "Microsoft YaHei", "", "", "", "", true),
        SIMSUN("simSun", "宋体（Windows）", "SimSun", "", "", "", "", true);
        final String id, title, family, regular, medium, semibold, bold;
        final boolean windowsOnly;
        Family(String id, String title, String family, String regular, String medium, String semibold, String bold, boolean windowsOnly) {
            this.id = id; this.title = title; this.family = family; this.regular = regular; this.medium = medium; this.semibold = semibold; this.bold = bold; this.windowsOnly = windowsOnly;
        }
        static Family fromId(String id) { for (Family family : values()) if (family.id.equals(id)) return family; return SYSTEM; }
    }
}
