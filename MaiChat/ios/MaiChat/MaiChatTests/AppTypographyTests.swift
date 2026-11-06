// 文件作用：验证原生字体与 Markdown 共用字号比例，以及深浅主题的可读配色。
import XCTest
import UIKit
import SwiftUI
import MaiChatCore

final class AppTypographyTests: XCTestCase {
    func testBundledFamilyAppliesToBodyDetailsAndEditorsWithoutChangingScale() {
        XCTAssertTrue(AppFontAssets.available.contains(.notoSans))
        XCTAssertTrue(AppFontAssets.available.contains(.notoSerif))
        for family in [AppFontFamily.notoSans, .notoSerif] {
            for (preset, body) in [(AppTextSize.standard, 16.0), (.large, 18.0), (.extraLarge, 20.0)] {
                let scale = AppTypography.scale(preset, category: .large)
                let font = AppTypography.uiFont(.body, scale: scale, family: family)
                XCTAssertEqual(font.fontName, family.regularName)
                XCTAssertEqual(font.pointSize, body, accuracy: 0.01)
                let detail = AppTypography.uiFont(.detail, scale: scale, family: family)
                XCTAssertEqual(detail.familyName, font.familyName)
                XCTAssertEqual(detail.pointSize, body * 0.875, accuracy: 0.01)
                XCTAssertNotEqual(AppTypography.uiFont(.detail, scale: scale, monospaced: true, family: family).familyName,
                                  font.familyName)
            }
        }
    }

    func testPresetsApplyOnceToAllTextRoles() {
        for (preset, body) in [(AppTextSize.standard, 16.0), (.large, 18.0), (.extraLarge, 20.0)] {
            let scale = AppTypography.scale(preset, category: .accessibilityExtraExtraExtraLarge)
            XCTAssertEqual(AppTypography.uiFont(.body, scale: scale).pointSize, body, accuracy: 0.01)
            XCTAssertEqual(AppTypography.uiFont(.detail, scale: scale, monospaced: true).pointSize,
                           body * 0.875, accuracy: 0.01)
            XCTAssertEqual(AppTypography.uiFont(AppTypographyTokens.heading(1), scale: scale).pointSize,
                           body * 1.375, accuracy: 0.01)
        }
        XCTAssertGreaterThan(AppTypography.scale(.system, category: .accessibilityExtraExtraExtraLarge), 1)
    }

    func testTextAndMarkdownColorsRemainReadableInBothThemes() {
        for mode in [UIUserInterfaceStyle.light, .dark] {
            let traits = UITraitCollection(userInterfaceStyle: mode)
            let panel = UIColor(AppAppearance.panel).resolvedColor(with: traits)
            let codePanel = UIColor(AppAppearance.codeBackground).resolvedColor(with: traits)
            XCTAssertGreaterThan(contrast(UIColor(AppAppearance.text).resolvedColor(with: traits), panel), 7)
            XCTAssertGreaterThan(contrast(UIColor(AppAppearance.secondary).resolvedColor(with: traits), panel), 4.5)
            XCTAssertGreaterThan(contrast(UIColor(AppAppearance.code).resolvedColor(with: traits), codePanel), 4.5)
        }
    }

    private func contrast(_ first: UIColor, _ second: UIColor) -> Double {
        func luminance(_ color: UIColor) -> Double {
            var r: CGFloat = 0, g: CGFloat = 0, b: CGFloat = 0, a: CGFloat = 0
            color.getRed(&r, green: &g, blue: &b, alpha: &a)
            func linear(_ channel: CGFloat) -> Double {
                let v = Double(channel); return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4)
            }
            return linear(r) * 0.2126 + linear(g) * 0.7152 + linear(b) * 0.0722
        }
        let a = luminance(first), b = luminance(second)
        return (max(a, b) + 0.05) / (min(a, b) + 0.05)
    }
}
