// 文件作用：将共享字号契约适配到 SwiftUI 和 UIKit，文字只在呈现阶段缩放一次。
// 页面选择语义角色，不自行定义正文大小；图形符号不参与文字字号设置。
import CoreText
import MaiChatCore
import SwiftUI
import UIKit

struct AppFontStyle: Sendable {
    let role: AppTextRole
    var fontWeight: Font.Weight = .regular
    var design: Font.Design = .default
    var isItalic = false
    var usesMonospacedDigits = false

    func weight(_ value: Font.Weight) -> Self {
        var copy = self; copy.fontWeight = value; return copy
    }
    func bold() -> Self { weight(.bold) }
    func italic() -> Self { var copy = self; copy.isItalic = true; return copy }
    func monospaced() -> Self { var copy = self; copy.design = .monospaced; return copy }
    func monospacedDigit() -> Self { var copy = self; copy.usesMonospacedDigits = true; return copy }
}

// 原生按钮、列表可提供自己的默认 Font；实际标签直接解析应用语义角色，
// 避免家族或字号被控件样式覆盖。设置页与创作中心目录复用同一入口。
struct AppText: View {
    private let content: Text
    @Environment(\.appTextStyle) private var style
    @Environment(\.appTextScale) private var scale
    @Environment(\.appFontFamily) private var family
    init(_ text: String) { content = Text(verbatim: text) }
    var body: some View { content.font(AppTypography.resolved(style, scale: scale, family: family)) }
}

enum AppTypography {
    static let body = AppFontStyle(role: .body)
    static let emphasizedBody = body.weight(.semibold)
    static let detail = AppFontStyle(role: .detail)
    static let detailMonospaced = AppFontStyle(role: .detail, design: .monospaced)
    static let metadata = AppFontStyle(role: .metadata)
    static let heading = AppFontStyle(role: .heading3, fontWeight: .semibold)
    static let title = AppFontStyle(role: .title, fontWeight: .semibold)
    static let display = AppFontStyle(role: .display, fontWeight: .semibold)
    static let hero = AppFontStyle(role: .hero, fontWeight: .semibold)
    static let conversationTitle = body
    static let conversationPreview = detail
    static let conversationTime = metadata.weight(.medium)
    static let lineSpacing = CGFloat(AppTextRole.body.points * AppTypographyTokens.lineGapRatio)
    static let paragraphSpacing = CGFloat(AppTextRole.body.points * AppTypographyTokens.paragraphGapRatio)

    static func markdownHeading(_ level: Int) -> AppFontStyle {
        AppFontStyle(role: AppTypographyTokens.heading(level), fontWeight: .semibold)
    }

    static func resolved(_ style: AppFontStyle, scale: CGFloat, family: AppFontFamily? = nil) -> Font {
        let choice = AppFontAssets.resolve(family ?? AppFontAssets.current)
        let points = CGFloat(style.role.points) * scale
        var font: Font
        // Code keeps the shared fixed-cell face; its size still derives once
        // from the same preference scale as ordinary body and metadata roles.
        if style.design == .monospaced {
            font = Font.custom("Sarasa-Fixed-SC-Regular", fixedSize: points).weight(style.fontWeight)
        } else if choice == .system {
            font = Font.system(size: points, weight: style.fontWeight, design: style.design)
        } else {
            font = Font(AppFontAssets.font(choice, size: points, weight: uiWeight(style.fontWeight)))
        }
        if style.isItalic { font = font.italic() }
        if style.usesMonospacedDigits { font = font.monospacedDigit() }
        return font
    }

    static func symbol(size: CGFloat, weight: Font.Weight = .regular, design: Font.Design = .default) -> Font {
        .system(size: size, weight: weight, design: design)
    }

    static func uiFont(_ role: AppTextRole, weight: UIFont.Weight = .regular,
                       scale: CGFloat? = nil, monospaced: Bool = false, monospacedDigits: Bool = false,
                       family: AppFontFamily? = nil) -> UIFont {
        let points = CGFloat(role.points) * (scale ?? currentScale)
        if monospaced {
            return UIFont(name: "Sarasa-Fixed-SC-Regular", size: points)
                ?? UIFont.monospacedSystemFont(ofSize: points, weight: weight)
        }
        var font = AppFontAssets.font(family ?? AppFontAssets.current, size: points, weight: weight)
        if monospacedDigits {
            let features: [[UIFontDescriptor.FeatureKey: Any]] = [[
                .type: kNumberSpacingType, .selector: kMonospacedNumbersSelector]]
            font = UIFont(descriptor: font.fontDescriptor.addingAttributes([.featureSettings: features]), size: points)
        }
        return font
    }

    private static func uiWeight(_ weight: Font.Weight) -> UIFont.Weight {
        switch weight {
        case .ultraLight: return .ultraLight
        case .thin: return .thin
        case .light: return .light
        case .medium: return .medium
        case .semibold: return .semibold
        case .bold: return .bold
        case .heavy: return .heavy
        case .black: return .black
        default: return .regular
        }
    }

    static var currentScale: CGFloat {
        let choice = AppTextSize(rawValue: UserDefaults.standard.string(forKey: AppTypographyTokens.preferenceKey) ?? "")
            ?? AppTypographyTokens.defaultPreset
        return scale(choice, category: nil)
    }

    static func scale(_ choice: AppTextSize, category: UIContentSizeCategory?) -> CGFloat {
        let standard = UITraitCollection(preferredContentSizeCategory: .large)
        let current = category.map { UITraitCollection(preferredContentSizeCategory: $0) }
        let reference = UIFont.preferredFont(forTextStyle: .body, compatibleWith: standard).pointSize
        let system = UIFont.preferredFont(forTextStyle: .body, compatibleWith: current).pointSize / reference
        return CGFloat(choice.scale(systemScale: Double(system)))
    }

    static func category(_ value: DynamicTypeSize) -> UIContentSizeCategory {
        switch value {
        case .xSmall: return .extraSmall
        case .small: return .small
        case .medium: return .medium
        case .large: return .large
        case .xLarge: return .extraLarge
        case .xxLarge: return .extraExtraLarge
        case .xxxLarge: return .extraExtraExtraLarge
        case .accessibility1: return .accessibilityMedium
        case .accessibility2: return .accessibilityLarge
        case .accessibility3: return .accessibilityExtraLarge
        case .accessibility4: return .accessibilityExtraExtraLarge
        case .accessibility5: return .accessibilityExtraExtraExtraLarge
        @unknown default: return .large
        }
    }

    static var bodyPointSize: CGFloat { uiFont(.body).pointSize }
    static var editor: UIFont { uiFont(.body) }
}

private struct AppTextStyleKey: EnvironmentKey { static let defaultValue = AppTypography.body }
private struct AppFontFamilyKey: EnvironmentKey { static let defaultValue = AppFontFamilyTokens.defaultFamily }
private struct AppTextScaleKey: EnvironmentKey { static let defaultValue: CGFloat = 1 }
extension EnvironmentValues {
    var appTextStyle: AppFontStyle {
        get { self[AppTextStyleKey.self] }
        set { self[AppTextStyleKey.self] = newValue }
    }
    var appFontFamily: AppFontFamily {
        get { self[AppFontFamilyKey.self] }
        set { self[AppFontFamilyKey.self] = newValue }
    }
    var appTextScale: CGFloat {
        get { self[AppTextScaleKey.self] }
        set { self[AppTextScaleKey.self] = newValue }
    }
}

private struct AppFontModifier: ViewModifier {
    @Environment(\.appTextScale) private var scale
    @Environment(\.appFontFamily) private var family
    let style: AppFontStyle
    func body(content: Content) -> some View {
        content.font(AppTypography.resolved(style, scale: scale, family: family))
            .environment(\.appTextStyle, style)
    }
}

extension View {
    func font(_ style: AppFontStyle) -> some View { modifier(AppFontModifier(style: style)) }
}

// 原有共用入口只保留别名，避免 IM 与创作中心各自维护第二套字号。
typealias ChatTypography = AppTypography
typealias AssistantMessageFont = AppTypography
