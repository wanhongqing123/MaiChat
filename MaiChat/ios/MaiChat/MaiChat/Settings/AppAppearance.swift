// 文件作用：把共享浅色/深色令牌适配为动态系统颜色，原生导航和正文共用同一主题。
// 颜色保留动态特性，不在静态初始化时按当前 trait 固定，切换主题无需重建页面。
import MaiChatCore
import SwiftUI
import UIKit

enum AppAppearance {
    static func color(_ light: UInt32, _ dark: UInt32) -> Color {
        Color(uiColor: UIColor { traits in
            let rgb = traits.userInterfaceStyle == .dark ? dark : light
            return UIColor(red: CGFloat((rgb >> 16) & 255) / 255,
                           green: CGFloat((rgb >> 8) & 255) / 255,
                           blue: CGFloat(rgb & 255) / 255, alpha: 1)
        })
    }

    static let page = color(AppTypographyTokens.pageColor, AppTypographyTokens.pageDarkColor)
    static let panel = color(AppTypographyTokens.panelColor, AppTypographyTokens.panelDarkColor)
    static let text = color(AppTypographyTokens.textColor, AppTypographyTokens.textDarkColor)
    static let secondary = color(AppTypographyTokens.secondaryColor, AppTypographyTokens.secondaryDarkColor)
    static let border = color(AppTypographyTokens.borderColor, AppTypographyTokens.borderDarkColor)
    static let link = color(AppTypographyTokens.linkColor, AppTypographyTokens.linkDarkColor)
    static let heading = color(AppTypographyTokens.headingColor, AppTypographyTokens.headingDarkColor)
    static let code = color(AppTypographyTokens.codeColor, AppTypographyTokens.codeDarkColor)
    static let codeBackground = color(AppTypographyTokens.codeBackgroundColor, AppTypographyTokens.codeBackgroundDarkColor)
    static let inlineCodeBackground = color(AppTypographyTokens.inlineCodeBackgroundColor, AppTypographyTokens.inlineCodeBackgroundDarkColor)
    static let tableHeader = color(AppTypographyTokens.tableHeaderColor, AppTypographyTokens.tableHeaderDarkColor)
    static let tableAlternate = color(AppTypographyTokens.tableAlternateColor, AppTypographyTokens.tableAlternateDarkColor)
    static let quoteText = color(AppTypographyTokens.quoteTextColor, AppTypographyTokens.quoteTextDarkColor)
    static let quoteBackground = color(AppTypographyTokens.quoteBackgroundColor, AppTypographyTokens.quoteBackgroundDarkColor)
}
