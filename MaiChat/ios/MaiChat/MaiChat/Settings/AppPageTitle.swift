// 文件作用：原生导航栏与自定义页头共用可响应字体偏好的标题。
// 导航栏默认标题会被系统样式重新赋予字体，因此使用显式的 principal 内容。
import MaiChatCore
import SwiftUI

struct AppPageTitle: View {
    let title: String
    @AppStorage(AppFontFamilyTokens.preferenceKey) private var storedFamily = AppFontFamilyTokens.defaultFamily.rawValue
    @AppStorage(AppTypographyTokens.preferenceKey) private var storedSize = AppTypographyTokens.defaultPreset.rawValue
    @Environment(\.dynamicTypeSize) private var dynamicTypeSize
    init(_ title: String) { self.title = title }
    var body: some View {
        let family = AppFontAssets.resolve(AppFontFamily(rawValue: storedFamily) ?? .system)
        let scale = AppTypography.scale(AppTextSize(rawValue: storedSize) ?? .standard,
                                        category: AppTypography.category(dynamicTypeSize))
        Text(title)
            .font(AppTypography.resolved(AppTypography.title, scale: scale, family: family))
            .foregroundStyle(AppAppearance.text)
            .lineLimit(1)
            .truncationMode(.middle)
    }
}

extension View {
    func appNavigationTitle(_ title: String) -> some View {
        navigationTitle(title)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .principal) { AppPageTitle(title) } }
    }
}
