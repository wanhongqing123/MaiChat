// 文件作用：“我的”的主题、偏好和关于页；每个可操作入口绑定真实平台能力。
// 日志先选择时间再导出，清日志独立确认；法律链接沿用 MaiChat 的正式文档。
import AVFoundation
import MaiChatCore
import SwiftUI
import UIKit
import UserNotifications

enum AppVersion {
    static var display: String {
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "—"
        let build = Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "—"
        return "\(version) (\(build))"
    }
}

// 设置页名称保留为别名，实际语义文字与其他普通页面只维护一套解析实现。
typealias MySettingsText = AppText

struct MySettingsSection<Content: View>: View {
    let title: String
    @ViewBuilder let content: () -> Content
    init(_ title: String, @ViewBuilder content: @escaping () -> Content) {
        self.title = title; self.content = content
    }
    var body: some View {
        Section { content() } header: {
            MySettingsText(title).font(AppTypography.metadata).foregroundStyle(RemoteIMStyle.textSecondary)
        }
    }
}

struct MySettingsRow: View {
    let title: String
    let systemImage: String
    var value: String = ""

    init(_ title: String, systemImage: String, value: String = "") {
        self.title = title
        self.systemImage = systemImage
        self.value = value
    }

    var body: some View {
        HStack(spacing: 13) {
            Image(systemName: systemImage)
                .font(AppTypography.body)
                .foregroundStyle(.secondary)
                .frame(width: 23)
                .accessibilityHidden(true)
            // List/NavigationLink 会提供自己的默认字体，正文样式必须落到文字本身。
            MySettingsText(title).font(AppTypography.body).foregroundStyle(RemoteIMStyle.textPrimary)
            Spacer(minLength: 8)
            if !value.isEmpty {
                MySettingsText(value).font(AppTypography.detail).foregroundStyle(RemoteIMStyle.textSecondary)
                    .multilineTextAlignment(.trailing)
            }
        }
        .padding(.vertical, 5)
    }
}

struct MySettingsValue: View {
    let title: String
    let value: String
    init(_ title: String, value: String) { self.title = title; self.value = value }
    var body: some View {
        LabeledContent {
            MySettingsText(value).font(AppTypography.detail).foregroundStyle(RemoteIMStyle.textSecondary)
        } label: {
            MySettingsText(title).font(AppTypography.body)
        }
    }
}

struct AppThemeSettingsView: View {
    @AppStorage(AppPreferenceKey.theme) private var theme = AppTheme.system.rawValue

    var body: some View {
        List {
            ForEach(AppTheme.allCases) { option in
                Button {
                    theme = option.rawValue
                } label: {
                    HStack {
                        MySettingsText(option.title).foregroundStyle(.primary)
                        Spacer()
                        if theme == option.rawValue {
                            Image(systemName: "checkmark").foregroundStyle(RemoteIMStyle.green)
                        }
                    }
                }
                .accessibilityIdentifier("theme-\(option.rawValue)")
                .accessibilityAddTraits(theme == option.rawValue ? .isSelected : [])
            }
        }
        .listStyle(.insetGrouped)
        .font(AppTypography.body)
        .appNavigationTitle("主题色")
        .navigationBarTitleDisplayMode(.inline)
    }
}

struct AppPreferencesView: View {
    @AppStorage(AppPreferenceKey.textSize) private var textSize = AppTypographyTokens.defaultPreset.rawValue
    @AppStorage(AppPreferenceKey.fontFamily) private var fontFamily = AppFontFamilyTokens.defaultFamily.rawValue
    @AppStorage(AppPreferenceKey.haptics) private var haptics = true
    @AppStorage(SpeechRecognitionProvider.defaultsKey) private var speechProvider =
        SpeechRecognitionProvider.tencent.rawValue

    var body: some View {
        List {
            Section {
                MySettingsValue("语言", value: "简体中文")
                    .padding(.vertical, 5)
                NavigationLink("消息通知") { AppNotificationSettingsView() }
                    .font(AppTypography.body)
                    .padding(.vertical, 5)
                    .accessibilityIdentifier("preferences-notifications")
                NavigationLink {
                    AppFontFamilySettingsView()
                } label: {
                    MySettingsValue("字体", value: AppFontAssets.resolve(
                        AppFontFamily(rawValue: fontFamily) ?? .system).title)
                }
                .padding(.vertical, 5)
                .accessibilityIdentifier("preferences-font-family")
                NavigationLink {
                    AppTextSizeSettingsView()
                } label: {
                    MySettingsValue("字体大小", value: (AppTextSize(rawValue: textSize) ?? AppTypographyTokens.defaultPreset).title)
                }
                .padding(.vertical, 5)
                .accessibilityIdentifier("preferences-font-size")
                NavigationLink("朗读设置") { AppReadingSettingsView() }
                    .font(AppTypography.body)
                    .padding(.vertical, 5)
                    .accessibilityIdentifier("preferences-reading")
                Toggle("触感反馈", isOn: $haptics)
                    .font(AppTypography.body)
                    .padding(.vertical, 5)
                    .accessibilityIdentifier("preferences-haptics")
            } footer: {
                MySettingsText("当前界面提供简体中文。触感反馈用于切换底部栏目，支持情况由设备决定。")
            }
            MySettingsSection("语音输入") {
                Picker("语音识别服务", selection: $speechProvider) {
                    ForEach(SpeechRecognitionProvider.allCases, id: \.rawValue) { provider in
                        MySettingsText(provider.title).tag(provider.rawValue)
                    }
                }
                .font(AppTypography.body)
                .accessibilityIdentifier("speech-recognition-provider")
            }
        }
        .listStyle(.insetGrouped)
        .font(AppTypography.body)
        .appNavigationTitle("偏好设置")
        .navigationBarTitleDisplayMode(.inline)
    }
}

private struct AppFontFamilySettingsView: View {
    @AppStorage(AppPreferenceKey.fontFamily) private var fontFamily = AppFontFamilyTokens.defaultFamily.rawValue
    @Environment(\.appTextScale) private var textScale
    var body: some View {
        List {
            Section {
                ForEach(AppFontAssets.available, id: \.rawValue) { option in
                    Button {
                        fontFamily = option.rawValue
                        AppHaptics.selectionChanged()
                    } label: {
                        HStack {
                            VStack(alignment: .leading, spacing: 5) {
                                MySettingsText(option.title).font(AppTypography.body).foregroundStyle(.primary)
                                Text("你好，Hello MaiChat")
                                    .font(AppTypography.resolved(AppTypography.detail, scale: textScale, family: option))
                                    .foregroundStyle(.secondary)
                            }
                            Spacer()
                            if AppFontAssets.resolve(AppFontFamily(rawValue: fontFamily) ?? .system) == option {
                                Image(systemName: "checkmark").foregroundStyle(RemoteIMStyle.green)
                            }
                        }
                    }
                    .accessibilityIdentifier("font-family-\(option.rawValue)")
                    .accessibilityAddTraits(fontFamily == option.rawValue ? .isSelected : [])
                }
            } footer: {
                MySettingsText("应用界面和正文使用所选字体；Markdown 代码保留等宽字体。")
                    .font(AppTypography.detail)
            }
            MySettingsSection("预览") {
                MarkdownLikeText("你好，Hello MaiChat。这是正文。\n\n# 标题 Heading\n\n**加粗 Bold**、*斜体 Italic* 与 `code`。")
                    .padding(.vertical, 12)
                    .accessibilityIdentifier("font-family-preview")
            }
        }
        .appNavigationTitle("字体")
        .navigationBarTitleDisplayMode(.inline)
    }
}

private struct AppNotificationSettingsView: View {
    @ObservedObject private var notifications = RemoteIMSystemNotificationCenter.shared
    @Environment(\.scenePhase) private var scenePhase
    @Environment(\.openURL) private var openURL

    var body: some View {
        List {
            Section {
                Toggle(isOn: Binding(
                    get: { notifications.isDoNotDisturbEnabled },
                    set: { _ = notifications.setDoNotDisturb($0) }
                )) {
                    MySettingsText("免打扰模式").font(AppTypography.body)
                }
                .tint(RemoteIMStyle.green)
                .accessibilityIdentifier("notification-do-not-disturb")
            } footer: {
                MySettingsText("开启后不弹出新消息通知；消息仍会正常接收，未读数量保留。")
            }
            Section {
                LabeledContent("系统通知", value: statusText)
                if notifications.authorizationStatus == .notDetermined {
                    Button("开启消息通知") {
                        Task { await notifications.requestAuthorizationIfNeeded() }
                    }
                } else {
                    Button("前往系统设置") {
                        if let url = URL(string: UIApplication.openSettingsURLString) { openURL(url) }
                    }
                }
            } footer: {
                MySettingsText("消息通知由 iOS 管理。应用在前台时不重复弹出系统横幅；通知不影响聊天中的未读提示。")
            }
        }
        .appNavigationTitle("消息通知")
        .navigationBarTitleDisplayMode(.inline)
        .task { await notifications.refreshAuthorizationStatus() }
        .onChange(of: scenePhase) { phase in
            if phase == .active { Task { await notifications.refreshAuthorizationStatus() } }
        }
    }

    private var statusText: String {
        switch notifications.authorizationStatus {
        case .notDetermined: return "未设置"
        case .denied: return "已关闭"
        case .authorized: return "已开启"
        case .provisional: return "静默通知"
        case .ephemeral: return "临时授权"
        @unknown default: return "请查看系统设置"
        }
    }
}

private struct AppTextSizeSettingsView: View {
    @AppStorage(AppPreferenceKey.textSize) private var textSize = AppTypographyTokens.defaultPreset.rawValue

    var body: some View {
        List {
            Section {
                ForEach(AppTextSize.allCases) { option in
                    Button { textSize = option.rawValue } label: {
                        HStack {
                            MySettingsText(option.title).foregroundStyle(.primary)
                            Spacer()
                            if textSize == option.rawValue {
                                Image(systemName: "checkmark").foregroundStyle(RemoteIMStyle.green)
                            }
                        }
                    }
                    .accessibilityIdentifier("font-size-\(option.rawValue)")
                    .accessibilityAddTraits(textSize == option.rawValue ? .isSelected : [])
                }
            } footer: {
                MySettingsText("调整应用文字、聊天正文和输入框；选择“跟随系统”可使用 iOS 的辅助功能字号。")
            }
            MySettingsSection("预览") {
                MarkdownLikeText("你好，Hello MaiChat。这是正文。\n\n# 大标题 Heading\n\n## 小标题 Subheading\n\n**加粗 Bold**、*斜体 Italic* 与 `code` 保留语义样式。", bodyFont: AppTypography.body)
                    .padding(.vertical, 12)
                    .accessibilityIdentifier("font-size-preview")
            }
        }
        .appNavigationTitle("字体大小")
        .navigationBarTitleDisplayMode(.inline)
    }
}

@MainActor
private final class ReadingPreview: ObservableObject {
    private let synthesizer = AVSpeechSynthesizer()
    func play() {
        synthesizer.stopSpeaking(at: .immediate)
        let utterance = AVSpeechUtterance(string: "你好，欢迎使用 MaiChat。Hello, welcome to MaiChat.")
        AppReadingPreferences.apply(to: utterance)
        synthesizer.speak(utterance)
    }
    func stop() { synthesizer.stopSpeaking(at: .immediate) }
}

private struct AppReadingSettingsView: View {
    @AppStorage(AppPreferenceKey.speechRate) private var rate = 1.0
    @AppStorage(AppPreferenceKey.speechLanguage) private var language = "system"
    @StateObject private var preview = ReadingPreview()

    var body: some View {
        List {
            Section {
                Picker("语速", selection: $rate) {
                    MySettingsText("慢速").tag(0.75)
                    MySettingsText("标准").tag(1.0)
                    MySettingsText("快速").tag(1.25)
                }
                Picker("朗读语言", selection: $language) {
                    MySettingsText("跟随系统").tag("system")
                    MySettingsText("普通话").tag("zh-CN")
                    MySettingsText("英语").tag("en-US")
                }
                Button("试听朗读") { preview.play() }
                    .accessibilityIdentifier("reading-preview")
            } footer: {
                MySettingsText("用于聊天和创作中心输入框编辑菜单中的“朗读”。语音由系统提供，未安装的语音可在 iOS 设置中下载。")
            }
        }
        .appNavigationTitle("朗读设置")
        .navigationBarTitleDisplayMode(.inline)
        .onDisappear { preview.stop() }
    }
}

private enum DiagnosticExportRange: String, CaseIterable, Identifiable {
    case hour, day, week, all
    var id: String { rawValue }
    var title: String {
        switch self {
        case .hour: return "最近 1 小时"
        case .day: return "最近 24 小时"
        case .week: return "最近 7 天"
        case .all: return "全部保留日志"
        }
    }
    var since: Date? {
        let seconds: TimeInterval
        switch self {
        case .hour: seconds = 3600
        case .day: seconds = 24 * 3600
        case .week: seconds = 7 * 24 * 3600
        case .all: return nil
        }
        return Date().addingTimeInterval(-seconds)
    }
}

private struct DiagnosticShare: Identifiable {
    let id = UUID()
    let url: URL
}

struct AppAboutView: View {
    @State private var showExportRange = false
    @State private var confirmClear = false
    @State private var busy = false
    @State private var share: DiagnosticShare?
    @State private var message: String?

    var body: some View {
        List {
            Section {
                Button { showExportRange = true } label: {
                    HStack {
                        MySettingsText("分享日志").foregroundStyle(.primary)
                        Spacer()
                        MySettingsText("按时间范围导出").font(AppTypography.detail).foregroundStyle(.secondary)
                        if busy { ProgressView() } else {
                            Image(systemName: "chevron.right").font(AppTypography.metadata.weight(.semibold)).foregroundStyle(.tertiary)
                        }
                    }
                    .padding(.vertical, 5)
                }
                .accessibilityIdentifier("about-export-logs")
                Button { confirmClear = true } label: {
                    MySettingsText("清除日志").foregroundStyle(.primary).padding(.vertical, 5)
                }
                .accessibilityIdentifier("about-clear-logs")
                Link("意见反馈", destination: URL(string: "mailto:1925271463@kongshang.com")!)
                    .padding(.vertical, 5)
            } footer: {
                MySettingsText("日志用于排查运行问题，最多保留 7 天、50 MB。导出后请先检查内容，再选择分享对象。")
            }
            .disabled(busy)
            Section {
                Link("用户协议", destination: URL(string: "https://ichat.life/legal/terms.html")!)
                Link("隐私政策", destination: URL(string: "https://ichat.life/legal/privacy.html")!)
                Link("儿童隐私保护声明", destination: URL(string: "https://ichat.life/legal/children.html")!)
            }
            Section {
                HStack {
                    Spacer()
                    VStack(spacing: 8) {
                        MySettingsText("MaiChat").font(AppTypography.heading.weight(.semibold))
                        MySettingsText("版本 \(AppVersion.display)").font(AppTypography.detail).foregroundStyle(.secondary)
                    }
                    Spacer()
                }
                .padding(.vertical, 20)
                .listRowBackground(Color.clear)
            }
        }
        .listStyle(.insetGrouped)
        .font(AppTypography.body)
        .appNavigationTitle("关于 MaiChat")
        .navigationBarTitleDisplayMode(.inline)
        .confirmationDialog("选择日志时间范围", isPresented: $showExportRange, titleVisibility: .visible) {
            ForEach(DiagnosticExportRange.allCases) { range in
                Button(range.title) { export(range) }
            }
            Button("取消", role: .cancel) {}
        }
        .confirmationDialog("清除本机诊断日志？", isPresented: $confirmClear, titleVisibility: .visible) {
            Button("清除日志", role: .destructive) { clearLogs() }
            Button("取消", role: .cancel) {}
        } message: {
            MySettingsText("清除后无法恢复。不会删除聊天、附件或已分享给其他应用的日志副本。之后产生的新日志仍会正常记录。")
        }
        .sheet(item: $share) { item in
            DiagnosticShareSheet(url: item.url)
        }
        .alert("诊断日志", isPresented: Binding(get: { message != nil }, set: { if !$0 { message = nil } })) {
            Button("好", role: .cancel) {}
        } message: { MySettingsText(message ?? "") }
    }

    private func export(_ range: DiagnosticExportRange) {
        guard !busy else { return }
        busy = true
        Task { @MainActor in
            defer { busy = false }
            do { share = DiagnosticShare(url: try await AppDiagnosticLog.shared.makeExportSnapshot(since: range.since)) }
            catch { message = "导出失败，请稍后重试。" }
        }
    }

    private func clearLogs() {
        guard !busy else { return }
        busy = true
        Task { @MainActor in
            defer { busy = false }
            do {
                try await AppDiagnosticLog.shared.clearLogs()
                message = "本机诊断日志已清除。"
            } catch { message = "清除未完成，请稍后重试。" }
        }
    }
}

private struct DiagnosticShareSheet: UIViewControllerRepresentable {
    let url: URL
    func makeUIViewController(context: Context) -> UIActivityViewController {
        UIActivityViewController(activityItems: [url], applicationActivities: nil)
    }
    func updateUIViewController(_ controller: UIActivityViewController, context: Context) {}
}
