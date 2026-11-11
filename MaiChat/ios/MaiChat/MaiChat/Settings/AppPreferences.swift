// 文件作用：声明本机外观、字体、朗读、触感与消息提醒偏好，并在普通页面共用。
// 偏好不含账号凭据；外观和字体只更新环境，不重建 Root，避免丢失导航及输入草稿。
import AVFoundation
import MaiChatCore
import SwiftUI
import UIKit

enum AppPreferenceKey {
    static let theme = "maichat.preferences.theme"
    static let fontFamily = AppFontFamilyTokens.preferenceKey
    static let textSize = AppTypographyTokens.preferenceKey
    static let speechRate = "maichat.preferences.speech-rate"
    static let speechLanguage = "maichat.preferences.speech-language"
    static let haptics = "maichat.preferences.haptics"
    static let doNotDisturb = RemoteIMNotificationPreferences.defaultsKey
}

enum AppTheme: String, CaseIterable, Identifiable {
    case system, light, dark
    var id: String { rawValue }
    var title: String {
        switch self {
        case .system: return "跟随系统"
        case .light: return "浅色"
        case .dark: return "深色"
        }
    }
    var colorScheme: ColorScheme? {
        switch self {
        case .system: return nil
        case .light: return .light
        case .dark: return .dark
        }
    }
}

extension AppTextSize: @retroactive Identifiable {
    public var id: String { rawValue }
    var title: String {
        switch self {
        case .system: return "跟随系统"
        case .standard: return "标准"
        case .large: return "大"
        case .extraLarge: return "特大"
        }
    }
    var dynamicTypeSize: DynamicTypeSize? {
        switch self {
        case .system: return nil
        case .standard: return .large
        case .large: return .xLarge
        case .extraLarge: return .xxLarge
        }
    }
    var contentSizeCategory: UIContentSizeCategory? {
        switch self {
        case .system: return nil
        case .standard: return .large
        case .large: return .extraLarge
        case .extraLarge: return .extraExtraLarge
        }
    }
}

private struct AppPreferencesModifier: ViewModifier {
    @Environment(\.dynamicTypeSize) private var systemTextSize
    @AppStorage(AppPreferenceKey.theme) private var theme = AppTheme.system.rawValue
    @AppStorage(AppPreferenceKey.textSize) private var textSize = AppTypographyTokens.defaultPreset.rawValue
    @AppStorage(AppPreferenceKey.fontFamily) private var fontFamily = AppFontFamilyTokens.defaultFamily.rawValue

    func body(content: Content) -> some View {
        let family = AppFontAssets.resolve(AppFontFamily(rawValue: fontFamily) ?? AppFontFamilyTokens.defaultFamily)
        let choice = AppTextSize(rawValue: textSize) ?? AppTypographyTokens.defaultPreset
        let scale = AppTypography.scale(choice, category: AppTypography.category(systemTextSize))
        content
            .preferredColorScheme((AppTheme(rawValue: theme) ?? .system).colorScheme)
            .dynamicTypeSize(choice.dynamicTypeSize ?? systemTextSize)
            .environment(\.appTextScale, scale)
            .environment(\.appFontFamily, family)
            .font(AppTypography.resolved(.init(role: .body), scale: scale, family: family))
    }
}

extension View {
    func appPreferences() -> some View { modifier(AppPreferencesModifier()) }
}

enum AppReadingPreferences {
    /// 语速以系统默认语速为基准，设置页试听与输入框“朗读”使用同一份参数。
    static func apply(to utterance: AVSpeechUtterance) {
        let defaults = UserDefaults.standard
        let storedRate = defaults.object(forKey: AppPreferenceKey.speechRate) as? Double ?? 1
        let multiplier = min(1.25, max(0.75, storedRate.isFinite ? storedRate : 1))
        utterance.rate = AVSpeechUtteranceDefaultSpeechRate * Float(multiplier)
        let language = defaults.string(forKey: AppPreferenceKey.speechLanguage) ?? "system"
        utterance.voice = AVSpeechSynthesisVoice(language: language == "system"
            ? (Locale.preferredLanguages.first ?? "zh-CN") : language)
    }
}

@MainActor
enum AppHaptics {
    static func selectionChanged() {
        let enabled = UserDefaults.standard.object(forKey: AppPreferenceKey.haptics) as? Bool ?? true
        guard enabled else { return }
        UISelectionFeedbackGenerator().selectionChanged()
    }
}
