// 文件作用：注册共用字体资产并解析已保存的字体；不下载字体，不替换代码的等宽语义。
// 主 App 和单元测试从各自 bundle 读取同一资源目录，字体注册由 Swift 静态初始化保证只做一次。
import CoreText
import MaiChatCore
import UIKit

private final class AppFontBundleMarker: NSObject {}

enum AppFontAssets {
    private static let registered: Void = {
        let bundles = [Bundle.main, Bundle(for: AppFontBundleMarker.self)]
        let names = Set(AppFontFamily.allCases.flatMap {
            [$0.regularName, $0.mediumName, $0.semiboldName, $0.boldName]
        }.filter { !$0.isEmpty })
        for name in names {
            guard let url = bundles.compactMap({
                $0.url(forResource: name, withExtension: "otf", subdirectory: "fonts")
            }).first else { continue }
            CTFontManagerRegisterFontsForURL(url as CFURL, .process, nil)
        }
    }()

    static let available: [AppFontFamily] = {
        _ = registered
        return AppFontFamily.allCases.filter {
            $0 == .system || (!$0.windowsOnly && UIFont(name: $0.regularName, size: 16) != nil)
        }
    }()

    static func resolve(_ family: AppFontFamily) -> AppFontFamily {
        available.contains(family) ? family : .system
    }

    static var current: AppFontFamily {
        resolve(AppFontFamily(rawValue: UserDefaults.standard.string(
            forKey: AppFontFamilyTokens.preferenceKey) ?? "") ?? AppFontFamilyTokens.defaultFamily)
    }

    static func font(_ family: AppFontFamily, size: CGFloat, weight: UIFont.Weight) -> UIFont {
        let family = resolve(family)
        guard family != .system else { return .systemFont(ofSize: size, weight: weight) }
        let name = weight.rawValue >= UIFont.Weight.bold.rawValue ? family.boldName : weight.rawValue >= UIFont.Weight.semibold.rawValue ? family.semiboldName
            : weight.rawValue >= UIFont.Weight.medium.rawValue ? family.mediumName : family.regularName
        return UIFont(name: name, size: size) ?? .systemFont(ofSize: size, weight: weight)
    }
}
