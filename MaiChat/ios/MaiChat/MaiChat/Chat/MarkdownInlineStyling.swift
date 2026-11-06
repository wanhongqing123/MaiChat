// 文件作用：为聊天中的 Markdown 行内元素提供一致的 SwiftUI 字体和颜色样式。
// 为消息正文中的代码、链接和强调片段指定稳定配色。
// 属性修改前先固定文本区间，避免遍历中改变富文本 run 边界。
import SwiftUI

// The original shared face is registered by UIAppFonts. Keep code separate from
// body typography: ASCII/box drawing use one cell, CJK two, without ligatures.
enum MarkdownCodeTypography {
    static let postScriptName = "Sarasa-Fixed-SC-Regular"

    static func font(size: CGFloat) -> Font {
        .custom(postScriptName, fixedSize: size)
    }
}

enum MarkdownInlineStyling {
    // 语义解析缓存不存字体；字号在绘制时计算，保证改字号后代码文字也立即生效。
    static let codeForeground = AppAppearance.code
    static var codeFont: Font { MarkdownCodeTypography.font(size: AppTypography.uiFont(.detail).pointSize) }

    static func apply(to source: AttributedString, scale: CGFloat = 1) -> AttributedString {
        var styled = source
        // 写入富文本属性会改变片段边界；修改前先保存字符范围，不能遍历正在变化的片段视图。
        let codeRanges = styled.runs
            .filter { $0.inlinePresentationIntent?.contains(.code) == true }
            .map(\.range)
        for range in codeRanges {
            styled[range].font = MarkdownCodeTypography.font(size: CGFloat(AppTypography.detail.role.points) * scale)
            styled[range].foregroundColor = codeForeground
        }
        return styled
    }
}
