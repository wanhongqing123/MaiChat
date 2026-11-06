// 自动生成自 MaiChat/Presentation/typography.json；修改定义后运行 generate-typography.py。
import Foundation

public enum AppTextRole: String, CaseIterable, Sendable {
    case body, detail, metadata, heading1, heading2, heading3, title, section, display, hero

    public var points: Double {
        switch self {
        case .body: return 16.000
        case .detail: return 14.000
        case .metadata: return 12.000
        case .heading1: return 22.000
        case .heading2: return 20.000
        case .heading3: return 18.000
        case .title: return 20.000
        case .section: return 18.000
        case .display: return 28.000
        case .hero: return 32.000
        }
    }
}

public enum AppTextSize: String, CaseIterable, Sendable {
    case system, standard, large, extraLarge
    public func scale(systemScale: Double = 1) -> Double {
        switch self {
        case .system: return systemScale.isFinite && systemScale > 0 ? systemScale : 1
        case .standard: return 1.0
        case .large: return 1.125
        case .extraLarge: return 1.25
        }
    }
}

public enum AppTypographyTokens {
    public static let defaultPreset = AppTextSize.standard
    public static let preferenceKey = "maichat.preferences.text-size"
    public static func heading(_ level: Int) -> AppTextRole {
        switch level {
        case 1: return .heading1
        case 2: return .heading2
        case 3: return .heading3
        default: return .body
        }
    }
    public static let textColor: UInt32 = 0x0e1525
    public static let secondaryColor: UInt32 = 0x64758f
    public static let linkColor: UInt32 = 0x0b67b7
    public static let headingColor: UInt32 = 0x1c4f8a
    public static let codeColor: UInt32 = 0x6b3b97
    public static let codeBackgroundColor: UInt32 = 0xf4f6f9
    public static let inlineCodeBackgroundColor: UInt32 = 0xf2edfa
    public static let tableHeaderColor: UInt32 = 0xe8f0f7
    public static let tableAlternateColor: UInt32 = 0xf5f7fa
    public static let borderColor: UInt32 = 0xdae4f0
    public static let quoteTextColor: UInt32 = 0x475569
    public static let quoteBackgroundColor: UInt32 = 0xeaf3fd
    public static let quoteAccentColor: UInt32 = 0x2873c7
    public static let pageColor: UInt32 = 0xf6f9fc
    public static let groupedPageColor: UInt32 = 0xf2f2f7
    public static let panelColor: UInt32 = 0xffffff
    public static let incomingBubbleColor: UInt32 = 0xfafcfe
    public static let outgoingBubbleColor: UInt32 = 0xeaf4ff
    public static let blueSoftColor: UInt32 = 0xe1f4ff
    public static let greenSoftColor: UInt32 = 0xd8fbe6
    public static let purpleSoftColor: UInt32 = 0xf0edff
    public static let contactSoftColor: UInt32 = 0xe7f7f1
    public static let profileSoftColor: UInt32 = 0xe9eef5
    public static let noteColor: UInt32 = 0x1f64b0
    public static let tipColor: UInt32 = 0x16836b
    public static let importantColor: UInt32 = 0x7547a8
    public static let warningColor: UInt32 = 0x9c640f
    public static let cautionColor: UInt32 = 0xba3d40
    public static let textDarkColor: UInt32 = 0xe8edf4
    public static let secondaryDarkColor: UInt32 = 0xa8b4c8
    public static let linkDarkColor: UInt32 = 0x79bfff
    public static let headingDarkColor: UInt32 = 0xa4caff
    public static let codeDarkColor: UInt32 = 0xd1acf3
    public static let codeBackgroundDarkColor: UInt32 = 0x242a34
    public static let inlineCodeBackgroundDarkColor: UInt32 = 0x332b43
    public static let tableHeaderDarkColor: UInt32 = 0x283647
    public static let tableAlternateDarkColor: UInt32 = 0x22262d
    public static let borderDarkColor: UInt32 = 0x3a404c
    public static let quoteTextDarkColor: UInt32 = 0xc5cfdf
    public static let quoteBackgroundDarkColor: UInt32 = 0x1c2c3e
    public static let quoteAccentDarkColor: UInt32 = 0x79bfff
    public static let pageDarkColor: UInt32 = 0x121214
    public static let groupedPageDarkColor: UInt32 = 0x000000
    public static let panelDarkColor: UInt32 = 0x1c1c1e
    public static let incomingBubbleDarkColor: UInt32 = 0x1c1c1e
    public static let outgoingBubbleDarkColor: UInt32 = 0x1b3049
    public static let blueSoftDarkColor: UInt32 = 0x133044
    public static let greenSoftDarkColor: UInt32 = 0x143524
    public static let purpleSoftDarkColor: UInt32 = 0x282240
    public static let contactSoftDarkColor: UInt32 = 0x15332a
    public static let profileSoftDarkColor: UInt32 = 0x232830
    public static let noteDarkColor: UInt32 = 0x79bfff
    public static let tipDarkColor: UInt32 = 0x67d7b1
    public static let importantDarkColor: UInt32 = 0xd1acf3
    public static let warningDarkColor: UInt32 = 0xf1c36b
    public static let cautionDarkColor: UInt32 = 0xff9a9f
    public static let lineGapRatio: Double = 0.375
    public static let paragraphGapRatio: Double = 1.0
    public static let listGapRatio: Double = 0.5
    public static let tablePaddingHRatio: Double = 0.75
    public static let tablePaddingVRatio: Double = 0.625
    public static let contentPaddingRatio: Double = 0.75
    public static let radiusRatio: Double = 0.625
}

public enum AppFontFamily: String, CaseIterable, Sendable {
    case system, notoSans, notoSerif, microsoftYaHei, simSun
    public var title: String {
        switch self {
        case .system: return "系统默认"
        case .notoSans: return "黑体（Noto Sans SC）"
        case .notoSerif: return "宋体（Noto Serif SC）"
        case .microsoftYaHei: return "微软雅黑"
        case .simSun: return "宋体（Windows）"
        }
    }
    public var familyName: String {
        switch self {
        case .system: return ""
        case .notoSans: return "Noto Sans SC"
        case .notoSerif: return "Noto Serif SC"
        case .microsoftYaHei: return "Microsoft YaHei"
        case .simSun: return "SimSun"
        }
    }
    public var regularName: String {
        switch self {
        case .system: return ""
        case .notoSans: return "NotoSansSC-Regular"
        case .notoSerif: return "NotoSerifSC-Regular"
        case .microsoftYaHei: return ""
        case .simSun: return ""
        }
    }
    public var mediumName: String {
        switch self {
        case .system: return ""
        case .notoSans: return "NotoSansSC-Medium"
        case .notoSerif: return "NotoSerifSC-Medium"
        case .microsoftYaHei: return ""
        case .simSun: return ""
        }
    }
    public var semiboldName: String {
        switch self {
        case .system: return ""
        case .notoSans: return "NotoSansSC-Medium"
        case .notoSerif: return "NotoSerifSC-SemiBold"
        case .microsoftYaHei: return ""
        case .simSun: return ""
        }
    }
    public var boldName: String {
        switch self {
        case .system: return ""
        case .notoSans: return "NotoSansSC-Bold"
        case .notoSerif: return "NotoSerifSC-Bold"
        case .microsoftYaHei: return ""
        case .simSun: return ""
        }
    }
    public var windowsOnly: Bool { self == .microsoftYaHei || self == .simSun }
}
public enum AppFontFamilyTokens {
    public static let preferenceKey = "maichat.preferences.font-family"
    public static let defaultFamily = AppFontFamily.system
}
