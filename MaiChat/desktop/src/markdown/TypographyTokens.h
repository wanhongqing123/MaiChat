// 自动生成自 MaiChat/Presentation/typography.json；修改定义后运行 generate-typography.py。
#pragma once

namespace TypographyTokens {
inline constexpr double kBody = 16.0;
inline constexpr double kStandardScale = 1.0;
inline constexpr double kLargeScale = 1.125;
inline constexpr double kExtraLargeScale = 1.25;
inline constexpr double kBodyRatio = 1.0;
inline constexpr double kDetailRatio = 0.875;
inline constexpr double kMetadataRatio = 0.75;
inline constexpr double kHeading1Ratio = 1.375;
inline constexpr double kHeading2Ratio = 1.25;
inline constexpr double kHeading3Ratio = 1.125;
inline constexpr double kTitleRatio = 1.25;
inline constexpr double kSectionRatio = 1.125;
inline constexpr double kDisplayRatio = 1.75;
inline constexpr double kHeroRatio = 2.0;
inline constexpr const char* kTextColor = "#0e1525";
inline constexpr const char* kSecondaryColor = "#64758f";
inline constexpr const char* kLinkColor = "#0b67b7";
inline constexpr const char* kHeadingColor = "#1c4f8a";
inline constexpr const char* kCodeColor = "#6b3b97";
inline constexpr const char* kCodeBackgroundColor = "#f4f6f9";
inline constexpr const char* kInlineCodeBackgroundColor = "#f2edfa";
inline constexpr const char* kTableHeaderColor = "#e8f0f7";
inline constexpr const char* kTableAlternateColor = "#f5f7fa";
inline constexpr const char* kBorderColor = "#dae4f0";
inline constexpr const char* kQuoteTextColor = "#475569";
inline constexpr const char* kQuoteBackgroundColor = "#eaf3fd";
inline constexpr const char* kQuoteAccentColor = "#2873c7";
inline constexpr const char* kPageColor = "#f6f9fc";
inline constexpr const char* kGroupedPageColor = "#f2f2f7";
inline constexpr const char* kPanelColor = "#ffffff";
inline constexpr const char* kIncomingBubbleColor = "#fafcfe";
inline constexpr const char* kOutgoingBubbleColor = "#eaf4ff";
inline constexpr const char* kBlueSoftColor = "#e1f4ff";
inline constexpr const char* kGreenSoftColor = "#d8fbe6";
inline constexpr const char* kPurpleSoftColor = "#f0edff";
inline constexpr const char* kContactSoftColor = "#e7f7f1";
inline constexpr const char* kProfileSoftColor = "#e9eef5";
inline constexpr const char* kNoteColor = "#1f64b0";
inline constexpr const char* kTipColor = "#16836b";
inline constexpr const char* kImportantColor = "#7547a8";
inline constexpr const char* kWarningColor = "#9c640f";
inline constexpr const char* kCautionColor = "#ba3d40";
inline constexpr const char* kTextDarkColor = "#e8edf4";
inline constexpr const char* kSecondaryDarkColor = "#a8b4c8";
inline constexpr const char* kLinkDarkColor = "#79bfff";
inline constexpr const char* kHeadingDarkColor = "#a4caff";
inline constexpr const char* kCodeDarkColor = "#d1acf3";
inline constexpr const char* kCodeBackgroundDarkColor = "#242a34";
inline constexpr const char* kInlineCodeBackgroundDarkColor = "#332b43";
inline constexpr const char* kTableHeaderDarkColor = "#283647";
inline constexpr const char* kTableAlternateDarkColor = "#22262d";
inline constexpr const char* kBorderDarkColor = "#3a404c";
inline constexpr const char* kQuoteTextDarkColor = "#c5cfdf";
inline constexpr const char* kQuoteBackgroundDarkColor = "#1c2c3e";
inline constexpr const char* kQuoteAccentDarkColor = "#79bfff";
inline constexpr const char* kPageDarkColor = "#121214";
inline constexpr const char* kGroupedPageDarkColor = "#000000";
inline constexpr const char* kPanelDarkColor = "#1c1c1e";
inline constexpr const char* kIncomingBubbleDarkColor = "#1c1c1e";
inline constexpr const char* kOutgoingBubbleDarkColor = "#1b3049";
inline constexpr const char* kBlueSoftDarkColor = "#133044";
inline constexpr const char* kGreenSoftDarkColor = "#143524";
inline constexpr const char* kPurpleSoftDarkColor = "#282240";
inline constexpr const char* kContactSoftDarkColor = "#15332a";
inline constexpr const char* kProfileSoftDarkColor = "#232830";
inline constexpr const char* kNoteDarkColor = "#79bfff";
inline constexpr const char* kTipDarkColor = "#67d7b1";
inline constexpr const char* kImportantDarkColor = "#d1acf3";
inline constexpr const char* kWarningDarkColor = "#f1c36b";
inline constexpr const char* kCautionDarkColor = "#ff9a9f";
inline constexpr double kLineGapRatio = 0.375;
inline constexpr double kParagraphGapRatio = 1.0;
inline constexpr double kListGapRatio = 0.5;
inline constexpr double kTablePaddingHRatio = 0.75;
inline constexpr double kTablePaddingVRatio = 0.625;
inline constexpr double kContentPaddingRatio = 0.75;
inline constexpr double kRadiusRatio = 0.625;
}  // namespace TypographyTokens

namespace TypographyTokens {
struct FontFamily {
    const char* id;
    const char* title;
    const char* family;
    const char* regular;
    const char* medium;
    const char* semibold;
    const char* bold;
    bool windowsOnly;
};
inline constexpr const char* kFontFamilyPreferenceKey = "maichat.preferences.font-family";
inline constexpr FontFamily kFontFamilies[] = {
    {"system", u8"系统默认", "", "", "", "", "", false},
    {"notoSans", u8"黑体（Noto Sans SC）", "Noto Sans SC", "NotoSansSC-Regular", "NotoSansSC-Medium", "NotoSansSC-Medium", "NotoSansSC-Bold", false},
    {"notoSerif", u8"宋体（Noto Serif SC）", "Noto Serif SC", "NotoSerifSC-Regular", "NotoSerifSC-Medium", "NotoSerifSC-SemiBold", "NotoSerifSC-Bold", false},
    {"microsoftYaHei", u8"微软雅黑", "Microsoft YaHei", "", "", "", "", true},
    {"simSun", u8"宋体（Windows）", "SimSun", "", "", "", "", true},
};
}  // namespace TypographyTokens
