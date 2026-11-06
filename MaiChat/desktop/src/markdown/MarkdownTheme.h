// 文件作用：桌面端Markdown 内容解析与呈现；MarkdownTheme 负责界面颜色和视觉令牌，本文件给出其声明。
// 对外边界：本文件给出 MarkdownTheme 的类型和函数声明；参数、返回值和对象生命周期以此为准。
// 公开入口：standard。
// 直接依赖：markdown/MarkdownDocument.h；实现与这些接口协作。
#pragma once

#include <QColor>
#include <QString>

#include "markdown/MarkdownDocument.h"
#include "markdown/TypographyTokens.h"

// Markdown 的配色和尺寸，**两端共用一套**。
//
// 字号、间距和语义颜色来源于 Presentation/typography.json 的生成令牌。
// 这里适配 Qt 的字体和绘制 API；自绘不受 QTextDocument 的 CSS 子集限制。
// 基础角色在共同契约中调整，agent 和 IM 使用相同主题与字号倍率。
// 调用方不能另设正文、表格和列表的字号。
//
// 尺寸都是**逻辑像素**，standard(zoom) 会按倍率缩好，调用方直接用。
//
// 倍率是**必填**而不是默认 1.0：这一层刻意不认识 UiZoom——认了它就得依赖
// remote_im_desktop_ui，而那一层反过来要用这里的绘制组件，成环。参数不给默认值，
// 调用方就没法忘。界面里一律传 UiZoom::factor()。

struct MarkdownCalloutStyle {
    QColor accent;
    QColor background;
    QString title;
};

struct MarkdownTheme {
    // 无序列表按层级循环使用实心圆、空心圆和方块，与浏览器默认的
    // disc / circle / square 层级一致。绘制层不要把所有深度重新压成一种标记。
    QString listBulletForDepth(int depth) const;

    // ── 字体 ────────────────────────────────────────────────
    // 空表示跟随应用默认字体。等宽字体按平台给，不写浏览器式的回退列表。
    QString bodyFamily;
    QString codeFamily;
    // 正文、列表、引用和表格同基准；代码等宽且按 detail 比例，标题保留语法层级。
    // 默认值也引用生成令牌，standard() 只做配色与一次最终倍率换算。
    int bodyPixelSize = static_cast<int>(TypographyTokens::kBody);
    int codePixelSize = static_cast<int>(TypographyTokens::kBody * TypographyTokens::kDetailRatio);
    int codeBlockPixelSize = static_cast<int>(TypographyTokens::kBody * TypographyTokens::kDetailRatio);
    int tablePixelSize = static_cast<int>(TypographyTokens::kBody);
    int listMarkerPixelSize = static_cast<int>(TypographyTokens::kBody);
    int listBulletPixelSize = static_cast<int>(TypographyTokens::kBody);
    int calloutTitlePixelSize = static_cast<int>(TypographyTokens::kBody * TypographyTokens::kDetailRatio);
    int headingPixelSize[6] = {
        static_cast<int>(TypographyTokens::kBody * TypographyTokens::kHeading1Ratio),
        static_cast<int>(TypographyTokens::kBody * TypographyTokens::kHeading2Ratio),
        static_cast<int>(TypographyTokens::kBody * TypographyTokens::kHeading3Ratio),
        static_cast<int>(TypographyTokens::kBody), static_cast<int>(TypographyTokens::kBody),
        static_cast<int>(TypographyTokens::kBody)};
    qreal lineHeightRatio = 1.6;

    // ── 颜色 ────────────────────────────────────────────────
    QColor text;
    QColor strong;
    QColor emphasis;
    QColor strike;
    QColor link;
    QColor headingColor[6];
    QColor inlineCodeText;
    QColor codeText;
    QColor codeBackground;
    QColor codeBorder;
    QColor quoteAccent;
    QColor quoteBackground;
    QColor quoteText;
    QColor tableHeaderBackground;
    QColor tableHeaderText;
    QColor tableRow;
    QColor tableRowAlternate;
    QColor tableLine;
    QColor divider;
    // 项目符号和序号是**同一个颜色**（iOS 两者都用主蓝），不再分开。
    QColor listMarker;
    QColor checkboxOn;
    QColor checkboxOff;
    QColor tableSeparator;
    QColor selection;
    QColor selectionText;

    // ── 间距 ────────────────────────────────────────────────
    // 段间距按正文比例；三端使用同一个令牌。
    int blockSpacing = static_cast<int>(TypographyTokens::kBody * TypographyTokens::kParagraphGapRatio);
    // 标题上下的留白。和 CSS 的 margin 一样会**合并**：相邻两块之间取两者的大值，
    // 不是相加——不然标题跟在段落后面时会空出一大块。
    // 在正文段间距之上，h1/h2 加 4，其他标题加 2。
    int headingSpacingAbove[6] = {20, 20, 18, 18, 18, 18};
    int headingSpacingBelow[6] = {16, 16, 16, 16, 16, 16};
    // 列表的三个横向量是分开的，别拿一个数兼着用：
    //   listIndent       每嵌套一层往右挪多少
    //   listMarkerWidth  标记列的宽度，标记在里面**右对齐**
    //   listMarkerGap    标记列到正文的间隙
    // 12px 在桌面宽屏上几乎看不出层级；提高层级步进，标记列和正文间距不变。
    int listIndent = 18;
    int listMarkerWidth = 16;
    int listMarkerGap = 8;
    // 缩进封顶。再深就不往右挪了，否则窄一点的列宽下正文没地方站。
    int listMaxDepth = 4;
    int listItemSpacing = static_cast<int>(TypographyTokens::kBody * TypographyTokens::kListGapRatio);
    int codePadding = 12;
    int codeRadius = 10;
    int quoteBarWidth = 3;
    int quotePadding = 12;
    int quoteRadius = 8;
    // 分割线只保留少量留白，其余使用正文段间距。
    int dividerSpacing = 3;
    // 单元格的横竖内边距不一样：横向要宽一点，列之间才分得开。
    int tableCellPaddingH = 12;
    int tableCellPaddingV = 10;
    // 表格整体圆角。表头和末行要**跟着裁**，不然方角会戳出圆角外面。
    int tableRadius = 10;
    int checkboxSize = 13;

    // ── 消息列表（MarkdownView 用）────────────────────────
    //
    // 气泡、提示行这些不属于 Markdown 本身，但**属于同一套外观**：
    // 放在这里，agent 和 IM 换皮肤时只改一个地方。
    QColor bubbleBackground;
    QColor bubbleText;
    QColor noticeText;
    QColor errorText;
    QColor viewBackground;
    int viewMargin = 16;
    int scrollBarWidth = 8;
    QColor scrollBarHandle;
    QColor scrollBarHandleHover;
    int itemSpacing = 14;
    int bubblePadding = 10;
    int bubbleRadius = 10;
    // 气泡最宽占可用宽度的多少。人发的消息短，拉满一行反而难读。
    qreal bubbleMaxWidthRatio = 0.72;

    // 造这份皮肤用的缩放倍率。与 bodyFamily/codeFamily 一起标识排版：
    // MarkdownLayoutCache 同时校验字体家族，倍率相同但字体不同仍需重排。
    //
    // 产品主题均由 standard(zoom, family) 构造。若调用方另外修改其他字段却
    // 没更新排版键，仍可能命中旧结果；新增影响字形的字段时应同步扩展缓存键。
    qreal zoom = 1.0;

    // 标准主题。zoom 传 UiZoom::factor()；非界面场景（比如测量）传 1.0。
    static MarkdownTheme standard(qreal zoom, const QString& bodyFamily = QStringLiteral("Inter"));

    // 提示框（> [!NOTE] 之类）的配色和中文标题。None 返回普通引用的样式。
    MarkdownCalloutStyle calloutStyle(MarkdownCallout callout) const;
};
