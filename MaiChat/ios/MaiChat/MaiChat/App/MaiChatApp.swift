// 文件作用：创建 iOS App 场景，初始化日志、通知、账号状态与全局共享服务。
// 窗口最外层承载 IM 和创作中心共用的语音转写悬浮窗；悬浮窗只负责覆盖显示。
// 启动、后台和返回前台的状态流在此接线，具体业务页面由 RootView 组织。
// 模拟器专用入口用于不触发付费服务的真实界面验收，不进入真机发布路径。
import MaiChatCore
import SwiftUI
import UserNotifications
import UIKit
#if DEBUG && targetEnvironment(simulator)
import PDFKit
#endif

@MainActor
final class IOSBackgroundActivityKeeper {
    static let shared = IOSBackgroundActivityKeeper()

    private var taskIdentifier: UIBackgroundTaskIdentifier = .invalid

    private init() {
    }

    /// 请求 iOS 授予普通 App 的有限后台执行窗口。
    /// 不播放静音音频，也不虚报定位或 VoIP 模式；这些都不是通用 IM 的合法保活方式，
    /// 进程仍会被 iOS 结束。
    func beginIfNeeded() {
        guard taskIdentifier == .invalid else { return }
        taskIdentifier = UIApplication.shared.beginBackgroundTask(
            withName: "MaiChat.IMBackgroundKeepAlive"
        ) { [weak self] in
            Task { @MainActor in
                self?.end(cause: "system-expired")
            }
        }
        let started = taskIdentifier != .invalid
        AppDiagnosticLog.shared.record(
            level: started ? .info : .warning,
            category: "app",
            event: started ? "background-keepalive-started" : "background-keepalive-unavailable",
            fields: [
                "remaining_seconds": remainingSecondsText(),
            ]
        )
    }

    func end(cause: String) {
        guard taskIdentifier != .invalid else { return }
        let identifier = taskIdentifier
        taskIdentifier = .invalid
        UIApplication.shared.endBackgroundTask(identifier)
        AppDiagnosticLog.shared.record(
            level: .info,
            category: "app",
            event: "background-keepalive-ended",
            fields: ["cause": cause]
        )
    }

    private func remainingSecondsText() -> String {
        let remaining = UIApplication.shared.backgroundTimeRemaining
        guard remaining.isFinite, remaining >= 0, remaining <= Double(Int.max) else {
            return "unknown"
        }
        return String(Int(remaining.rounded(.down)))
    }
}

@MainActor
final class RemoteIMSystemNotificationCenter: NSObject, ObservableObject, UNUserNotificationCenterDelegate {
    static let shared = RemoteIMSystemNotificationCenter()

    private let center = UNUserNotificationCenter.current()
    @Published private(set) var pendingRouteRevision: UInt64 = 0
    @Published private(set) var authorizationStatus: UNAuthorizationStatus = .notDetermined
    private(set) var pendingPeerUserID: String?

    func install() {
        center.delegate = self
    }

    func requestAuthorizationIfNeeded() async {
        let settings = await center.notificationSettings()
        authorizationStatus = settings.authorizationStatus
        guard settings.authorizationStatus == .notDetermined else { return }
        _ = try? await center.requestAuthorization(options: [.alert, .sound, .badge])
        await refreshAuthorizationStatus()
    }

    func refreshAuthorizationStatus() async {
        authorizationStatus = await center.notificationSettings().authorizationStatus
    }

    func post(peerUserID: String, title: String, body: String, badgeCount: Int) async -> Bool {
        let settings = await center.notificationSettings()
        guard settings.authorizationStatus == .authorized
            || settings.authorizationStatus == .provisional
            || settings.authorizationStatus == .ephemeral else { return false }
        let identifier = notificationIdentifier(peerUserID)
        center.removePendingNotificationRequests(withIdentifiers: [identifier])
        center.removeDeliveredNotifications(withIdentifiers: [identifier])

        let content = UNMutableNotificationContent()
        content.title = title
        content.body = body
        content.sound = .default
        content.badge = NSNumber(value: RemoteIMNewMessageNotificationPolicy.systemBadgeCount(
            totalUnreadCount: badgeCount
        ))
        content.threadIdentifier = "remote-im-\(peerUserID)"
        content.userInfo = ["peerUserID": peerUserID]
        do {
            try await center.add(UNNotificationRequest(
                identifier: identifier,
                content: content,
                trigger: nil
            ))
            return true
        } catch {
            return false
        }
    }

    func clear(peerUserID: String, badgeCount: Int) {
        let identifier = notificationIdentifier(peerUserID)
        center.removePendingNotificationRequests(withIdentifiers: [identifier])
        center.removeDeliveredNotifications(withIdentifiers: [identifier])
        if #available(iOS 16.0, *) {
            center.setBadgeCount(RemoteIMNewMessageNotificationPolicy.systemBadgeCount(
                totalUnreadCount: badgeCount
            ))
        }
    }

    func updateBadgeCount(_ badgeCount: Int) {
        if #available(iOS 16.0, *) {
            center.setBadgeCount(RemoteIMNewMessageNotificationPolicy.systemBadgeCount(
                totalUnreadCount: badgeCount
            ))
        }
    }

    func consumePendingPeerUserID() -> String? {
        defer { pendingPeerUserID = nil }
        return pendingPeerUserID
    }

    nonisolated func userNotificationCenter(
        _ center: UNUserNotificationCenter,
        willPresent notification: UNNotification
    ) async -> UNNotificationPresentationOptions {
        // 这个回调只会在 App 位于前台时触发。前台已经有会话列表和未读红点，
        // 不再叠一层系统横幅；退到后台后由系统按通知内容正常展示。
        []
    }

    nonisolated func userNotificationCenter(
        _ center: UNUserNotificationCenter,
        didReceive response: UNNotificationResponse,
        withCompletionHandler completionHandler: @escaping () -> Void
    ) {
        guard let peerUserID = response.notification.request.content.userInfo["peerUserID"] as? String,
              !peerUserID.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
        else {
            completionHandler()
            return
        }
        completionHandler()
        // 真机上的异步 UNUserNotificationCenterDelegate 入口运行在协作队列。
        // 从自动生成的异步桥接中调用 UIKit，曾在进入 MainActor 方法前就崩溃。
        // 改由带完成回调的入口显式切到 GCD 主队列，越过真实线程边界后再断言 actor 隔离。
        DispatchQueue.main.async { [weak self] in
            MainActor.assumeIsolated {
                self?.queueNotificationConversation(peerUserID)
            }
        }
    }

    private func queueNotificationConversation(_ peerUserID: String) {
        pendingPeerUserID = peerUserID
        pendingRouteRevision &+= 1
        AppDiagnosticLog.shared.record(
            level: .info,
            category: "remote-im",
            event: "notification-route-queued",
            fields: [
                "peer": DiagnosticLogPrivacy.stableTag(peerUserID, prefix: "u"),
                "main_thread": Thread.isMainThread ? "true" : "false",
                "revision": String(pendingRouteRevision),
            ]
        )
    }

    private func notificationIdentifier(_ peerUserID: String) -> String {
        "remote-im-message-\(peerUserID)"
    }
}

@MainActor
private final class MaiChatBootstrap: ObservableObject {
    @Published private(set) var appState: RemoteIMAppState?
    @Published private(set) var errorMessage: String?
    private var loading = false
    func load() async {
        guard appState == nil, !loading else { return }
        loading = true
        errorMessage = nil
        defer { loading = false }
        do {
            let loaded = try await RemoteIMBackgroundWork.file {
                _ = RemoteIMBackgroundWork.audioQueue
                try RemoteIMMediaStorage.prepareDirectories()
                let settings = LocalSettingsStore()
                return (settings, settings.load())
            }
            guard !Task.isCancelled else { return }
            let state = RemoteIMAppState(
                settingsStore: loaded.0,
                loadedSettings: loaded.1
            )
            AIMobileHostToolProvider.shared.appState = state
            appState = state
            if state.hasUnifiedAccount {
                await MainActor.run {
                    AIAssistantModel.shared.startSessionSyncForAccount()
                }
            }
        } catch {
            if !Task.isCancelled { errorMessage = error.localizedDescription }
        }
    }
}

@main
struct MaiChatApp: App {
    @StateObject private var bootstrap = MaiChatBootstrap()
    @StateObject private var voiceOverlay = VoiceTranscriptionOverlayState.shared

    init() {
        AppDiagnosticLog.shared.install()
        MaiCloudDriveAgentPlaybackCache.shared.cleanupOrphans()
        Task.detached(priority: .utility) {
            // 模型凭据由当前账号领取；启动时预取并按有效期刷新，后续请求仍由客户端直连。
            _ = try? await AICloudCredentialSync.refreshAccountModelKeysIfAvailable()
            try? await AICloudCredentialSync.refreshSeedanceIfNeeded()
            try? await AICloudCredentialSync.refreshBailianIfNeeded()
        }
        RemoteIMSystemNotificationCenter.shared.install()
        #if DEBUG && !targetEnvironment(simulator)
        AIPrimaryModelDeviceProbe.startIfRequested()
        #endif
    }

    var body: some Scene {
        WindowGroup {
            Group {
                #if DEBUG
                if let testEntry = MaiChatUITestSupport.entryView() {
                    testEntry
                } else {
                    appContent
                }
                #else
                appContent
                #endif
            }
            .appPreferences()
            .overlay {
                // IM 和创作中心共用的语音悬浮层放在窗口最外侧，始终盖过视频播放器。
                if let presentation = voiceOverlay.activePresentation {
                    VoiceTranscriptionHighlightHost(presentation: presentation)
                        .ignoresSafeArea()
                }
            }
            .task {
                #if DEBUG
                if MaiChatUITestSupport.bypassesBootstrap { return }
                #endif
                await bootstrap.load()
                #if DEBUG && targetEnvironment(simulator)
                await MaiChatUITestSupport.configureAfterBootstrap(bootstrap.appState)
                #endif
            }
        }
    }

    @ViewBuilder private var appContent: some View {
        if let appState = bootstrap.appState {
            RootView().environmentObject(appState)
                .overlay(alignment: .topTrailing) {
                    #if DEBUG
                    MaiChatUITestSupport.overlayProbes(appState: appState)
                    #endif
                }
        } else if let error = bootstrap.errorMessage {
            VStack(spacing: 12) {
                Text("加载失败")
                Text(error).font(AppTypography.detail)
                Button("重试") { Task { await bootstrap.load() } }
            }.padding()
        } else {
            ProgressView("正在加载")
        }
    }
}
