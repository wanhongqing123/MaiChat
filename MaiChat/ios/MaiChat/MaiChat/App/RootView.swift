// 文件作用：组织登录后的主导航，并协调 IM、创作中心、联系人、远程桌面和设置页。
// 创作中心首次打开后保留页面实例；切离再返回时继续使用原会话与滚动状态。
// Agent 打开视频时，这里把播放器放在上方或横屏侧边，并把剩余空间交给原页面。
// 登录、消息和模型业务状态分别来自 RemoteIMAppState 与 AIAssistantModel。
import MaiChatCore
import SwiftUI
import UIKit

enum AppTab {
    case messages
    case cloudDrive
    case assistant
    case contacts
    case remote
    case me
}

extension RemoteIMStyle {
    static let barBackground = Color(uiColor: .systemBackground).opacity(0.9)
    static let conversationBarOpacity = 0.97
    static let barSeparator = Color.primary.opacity(0.1)
}

struct RemoteIMBarSeparator: View {
    var body: some View {
        Rectangle()
            .fill(RemoteIMStyle.barSeparator)
            .frame(height: 0.5)
            .allowsHitTesting(false)
    }
}

extension View {
    /// A stable translucent color lets scrolling content show underneath.
    /// Only the background extends into system safe areas.
    func remoteIMBarBackground(edges: Edge.Set = [], opacity: Double = 0.9) -> some View {
        background(Color(uiColor: .systemBackground).opacity(opacity), ignoresSafeAreaEdges: edges)
    }

    func remoteIMNavigationBarMaterial() -> some View {
        toolbarBackground(RemoteIMStyle.barBackground, for: .navigationBar)
            .toolbarBackground(.visible, for: .navigationBar)
    }
}

@MainActor
private enum RemoteIMBarAppearance {
    private static var installed = false

    static func install() {
        guard !installed else { return }
        installed = true
        let navigation = UINavigationBarAppearance()
        navigation.configureWithDefaultBackground()
        navigation.backgroundEffect = nil
        navigation.backgroundColor = UIColor.systemBackground.withAlphaComponent(0.9)
        navigation.shadowColor = .separator
        navigation.titleTextAttributes = [.foregroundColor: UIColor.label]
        navigation.largeTitleTextAttributes = [.foregroundColor: UIColor.label]
        let navigationBar = UINavigationBar.appearance()
        navigationBar.isTranslucent = true
        navigationBar.standardAppearance = navigation
        navigationBar.scrollEdgeAppearance = navigation
        navigationBar.compactAppearance = navigation
        navigationBar.compactScrollEdgeAppearance = navigation

        let tab = UITabBarAppearance()
        tab.configureWithDefaultBackground()
        tab.backgroundEffect = nil
        tab.backgroundColor = UIColor.systemBackground.withAlphaComponent(0.9)
        tab.shadowColor = .separator
        let tabBar = UITabBar.appearance()
        tabBar.isTranslucent = true
        tabBar.standardAppearance = tab
        tabBar.scrollEdgeAppearance = tab
    }
}

struct RootView: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @Environment(\.scenePhase) private var scenePhase
    @ObservedObject private var systemNotificationCenter = RemoteIMSystemNotificationCenter.shared
    @ObservedObject private var videoCircle = MaiVideoCircleModel.shared
    @ObservedObject private var passwordRequests = AIAccountPasswordRequests.shared
    @ObservedObject private var pdfPasswordRequests = AIPdfPasswordRequests.shared
    @State private var selectedTab: AppTab = {
        #if DEBUG && targetEnvironment(simulator)
        if ProcessInfo.processInfo.arguments.contains("--my-settings-ui-test") { return .me }
        if ProcessInfo.processInfo.arguments.contains("--ai-image-mixed-ui-test") { return .assistant }
        if ProcessInfo.processInfo.arguments.contains("--ai-session-reveal-ui-test") { return .assistant }
        if ProcessInfo.processInfo.arguments.contains("--ai-browser-ui-test") { return .assistant }
        if ProcessInfo.processInfo.arguments.contains("--ai-portrait-ui-test") { return .assistant }
        if ProcessInfo.processInfo.environment["MAICHAT_IMAGE_ZOOM_ANCHOR_TEST"] == "1" { return .assistant }
        if ProcessInfo.processInfo.arguments.contains(
            "--cloud-drive-production-canary-normal") { return .me }
        #endif
        return .messages
    }()
    @State private var hasOpenedAssistant = false
    @State private var assistantImagePresented = false
    @State private var assistantImageSources = AIImageSourceFrameStore()
    @State private var assistantOpenGeneration = 0
    #if DEBUG && targetEnvironment(simulator)
    @State private var installedAssistantVideoLayoutFixture = false
    #endif
    @State private var activeChatContact: RemoteIMContact?
    @State private var activeChatGroup: RemoteIMGroup?
    @State private var isShowingCreateGroup = false
    @State private var pendingCreatedGroup: RemoteIMGroup?
    @State private var pendingAddFriendAfterGroup = false
    @State private var isShowingAddContact = false
    @State private var isShowingQRScanner = false
    @State private var pendingScannedFriendID: String?
    @State private var movingContact: RemoteIMContact?
    @State private var contactGroupActionName: String?
    @State private var renamingContactGroup: String?
    @State private var contactGroupRenameDraft = ""
    @State private var deletingContactGroup: String?
    @State private var isShowingNotificationPermissionPrompt = false
    @AppStorage("maichat.notification-preprompt-dismissed")
    private var notificationPermissionPromptDismissed = false

    init() {
        RemoteIMBarAppearance.install()
    }

    var body: some View {
        content
    }

    /// 主界面保持自己的布局和会话状态；视频在独立全屏模态中呈现，关闭后返回原页面。
    /// IM、创作结果和工具播放复用同一个媒体分页宿主，旋转和图库切换不重建当前播放器。
    private var content: some View {
        ZStack {
            VStack(spacing: 0) {
                if appState.shouldShowInitialLogin {
                    InitialLoginView()
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                } else {
                    Group {
                        switch selectedTab {
                        case .messages:
                            ChatView(
                                activeContact: $activeChatContact,
                                activeGroup: $activeChatGroup,
                                showRemoteDesktop: {
                                    activeChatContact = nil
                                    activeChatGroup = nil
                                    selectedTab = .remote
                                },
                                showCreateGroup: { isShowingCreateGroup = true },
                                showAddContact: {
                                    appState.newContactUserID = ""
                                    isShowingAddContact = true
                                },
                                showScanQRCode: {
                                    isShowingQRScanner = true
                                },
                                bottomBar: AnyView(RootTabBar(
                                    selectedTab: $selectedTab,
                                    remoteDesktop: appState.remoteDesktop,
                                    openAssistant: openAssistant
                                ))
                            )
                        case .cloudDrive:
                            NavigationStack {
                                MaiCloudDriveView()
                                    .remoteIMNavigationBarMaterial()
                            }
                        case .assistant:
                            Color.clear
                        case .contacts:
                            ContactsView(
                                selectedTab: $selectedTab,
                                activeContact: $activeChatContact,
                                movingContact: $movingContact,
                                groupActionName: $contactGroupActionName,
                                showAddContact: {
                                    appState.newContactUserID = ""
                                    isShowingAddContact = true
                                }
                            )
                        case .remote:
                            RemoteDesktopView()
                        case .me:
                            SettingsView()
                        }
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .safeAreaInset(edge: .bottom, spacing: 0) {
                        // Native lists extend behind this inset and keep their
                        // last row reachable. Message navigation owns its tab
                        // bar so interactive back gestures retain one layout.
                        if selectedTab != .messages && selectedTab != .assistant &&
                           activeChatContact == nil && activeChatGroup == nil {
                            RootTabBar(
                                selectedTab: $selectedTab,
                                remoteDesktop: appState.remoteDesktop,
                                openAssistant: openAssistant
                            )
                        }
                    }
                }
            }

            // 创作中心第一次打开后继续留在视图树中；切换主标签只控制可见性。
            // 保留同一页面实例可避免回来时重新加载历史或遗失当前输入。
            if !appState.shouldShowInitialLogin &&
                (selectedTab == .assistant || hasOpenedAssistant) {
                // 主导航与创作页面一起移开，目录联动与圆角轮廓由内部同一容器管理。
                AIAssistantView(isActive: selectedTab == .assistant, hostsImagePreview: false,
                    bottomBar: AnyView(RootTabBar(
                        selectedTab: $selectedTab,
                        remoteDesktop: appState.remoteDesktop,
                        openAssistant: openAssistant
                    )))
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .opacity(selectedTab == .assistant ? 1 : 0)
                    .allowsHitTesting(selectedTab == .assistant)
                    .accessibilityHidden(selectedTab != .assistant)
            }

            if isShowingAddContact, !appState.shouldShowInitialLogin {
                AddContactDialog(isPresented: $isShowingAddContact)
                    .transition(.opacity.combined(with: .scale(scale: 0.96)))
                    .zIndex(10)
            }

            if let movingContact, !appState.shouldShowInitialLogin {
                MoveContactGroupDialog(contact: movingContact) {
                    self.movingContact = nil
                }
                .transition(.opacity.combined(with: .scale(scale: 0.96)))
                .zIndex(20)
            }

            if isShowingNotificationPermissionPrompt, !appState.shouldShowInitialLogin {
                NotificationPermissionDialog(
                    later: dismissNotificationPermissionPrompt,
                    enable: enableNotifications
                )
                .transition(.opacity.combined(with: .scale(scale: 0.96)))
                .zIndex(40)
            }

            if let contactGroupActionName, !appState.shouldShowInitialLogin {
                ContactGroupActionDialog(
                    groupName: contactGroupActionName,
                    dismiss: { self.contactGroupActionName = nil },
                    rename: {
                        contactGroupRenameDraft = contactGroupActionName
                        renamingContactGroup = contactGroupActionName
                        self.contactGroupActionName = nil
                    },
                    delete: {
                        deletingContactGroup = contactGroupActionName
                        self.contactGroupActionName = nil
                    }
                )
                .transition(.opacity.combined(with: .scale(scale: 0.96)))
                .zIndex(50)
            }

            if let renamingContactGroup, !appState.shouldShowInitialLogin {
                RenameContactGroupDialog(
                    originalName: renamingContactGroup,
                    name: $contactGroupRenameDraft,
                    cancel: { self.renamingContactGroup = nil },
                    save: saveRenamedContactGroup
                )
                .transition(.opacity.combined(with: .scale(scale: 0.96)))
                .zIndex(60)
            }

            if let deletingContactGroup, !appState.shouldShowInitialLogin {
                DeleteContactGroupDialog(
                    groupName: deletingContactGroup,
                    memberCount: appState.chatState.contacts.filter {
                        $0.groupName == deletingContactGroup
                    }.count,
                    cancel: { self.deletingContactGroup = nil },
                    delete: deleteContactGroup
                )
                .transition(.opacity.combined(with: .scale(scale: 0.96)))
                .zIndex(70)
            }

        }
        .onChange(of: selectedTab) { tab in
            AppHaptics.selectionChanged()
#if DEBUG
            if tab == .cloudDrive { AIMobileScreenCapture.shared.captureCloudPageSmokeIfRequested() }
#endif
            if tab != .assistant { assistantOpenGeneration += 1 }
            if tab == .assistant { hasOpenedAssistant = true }
            #if DEBUG && targetEnvironment(simulator)
            if tab == .assistant,
               !installedAssistantVideoLayoutFixture,
               ProcessInfo.processInfo.arguments.contains("--assistant-main-tab-video-layout-test"),
               let videoPath = Bundle.main.path(forResource: "ffplay-sample", ofType: "mp4") {
                installedAssistantVideoLayoutFixture = true
                appState.agentVideoPresentation = MaiFfplayPresentation(path: videoPath)
            }
            #endif
        }
        .animation(.easeOut(duration: 0.18), value: isShowingAddContact)
        .animation(.easeOut(duration: 0.18), value: movingContact?.userID)
        .animation(.easeOut(duration: 0.18), value: isShowingNotificationPermissionPrompt)
        .animation(.easeOut(duration: 0.18), value: contactGroupActionName)
        .animation(.easeOut(duration: 0.18), value: renamingContactGroup)
        .animation(.easeOut(duration: 0.18), value: deletingContactGroup)
        .background(RemoteIMStyle.pageBackground.ignoresSafeArea())
        // 系统模态覆盖主页面，不给主页面扣除宽高；原生宿主统一负责触摸和读屏隔离。
        .fullScreenCover(item: $appState.agentVideoPresentation) { presentation in
            MaiLocalMediaPager(sequence: presentation.sequence,
                startSeconds: presentation.startSeconds, vod: presentation.vod,
                isModal: true,
                selectionChanged: { sequence in
                    // 浏览图片和视频保持同一 presentation ID，暂停、倍速和当前播放位置不重建。
                    appState.agentVideoPresentation = MaiFfplayPresentation(
                        sequence: sequence, id: presentation.id, vod: presentation.vod)
                }) {
                appState.agentVideoPresentation = nil
            }
            .id(presentation.id)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(Color.black)
            .ignoresSafeArea()
            .statusBarHidden()
        }
        // 图片覆盖整窗，保持下面页面布局不变；图视频分页继续复用相同原生容器。
        .overlay {
            if !appState.shouldShowInitialLogin && selectedTab == .assistant {
                AIAssistantImagePreviewLayer(sources: assistantImageSources)
            }
        }
        .onPreferenceChange(AIImageSourceFramesKey.self) { assistantImageSources.update($0) }
        .onPreferenceChange(AIImagePreviewVisibilityKey.self) {
            assistantImagePresented = $0
            maiLocalMediaTrace("root-image-visible=\($0)")
        }
        .statusBarHidden(assistantImagePresented)
        .task {
#if DEBUG
            AIMobileScreenCapture.shared.startFrameAcceptanceIfRequested(
                prepareAssistant: {
                    guard !appState.shouldShowInitialLogin else { return false }
                    openAssistant()
                    return true
                }, assistantIsVisible: { selectedTab == .assistant })
#endif
            if !appState.shouldShowInitialLogin {
                await appState.connectOnLaunchIfNeeded()
                await offerNotificationPermissionIfNeeded()
            }
            appState.synchronizeSystemNotificationBadge()
            schedulePendingNotificationRoute(sceneIsActive: scenePhase == .active)
        }
        .task(id: scenePhase == .active && appState.hasUnifiedAccount &&
              !appState.shouldShowInitialLogin) {
            guard scenePhase == .active, appState.hasUnifiedAccount,
                  !appState.shouldShowInitialLogin else { return }
            appState.refreshAvatarImagesForCurrentUsers()
            // 进入前台立即检查；前台每 30 秒复查。切到后台时 SwiftUI 取消该任务。
            while !Task.isCancelled && appState.hasUnifiedAccount &&
                  !appState.shouldShowInitialLogin {
                await appState.validateCurrentAccountSession()
                do {
                    try await Task.sleep(for: .seconds(30))
                } catch {
                    return
                }
            }
        }
        .onChange(of: appState.shouldShowInitialLogin) { needsLogin in
            if needsLogin {
                activeChatContact = nil
                activeChatGroup = nil
                selectedTab = .messages
                hasOpenedAssistant = false
                return
            }
            Task {
                await offerNotificationPermissionIfNeeded()
            }
        }
        .onChange(of: appState.masterUserID) { _ in
            clearSelectedChatForAccountChange()
        }
        .onChange(of: appState.remoteDiagnosticsIdentity) { _ in
            // 安全表单绑定打开时账号；切号必须关闭旧表单，不把状态或密码带入新账号。
            passwordRequests.cancelForAccountChange()
            pdfPasswordRequests.cancelAll()
        }
        .onChange(of: appState.maiChatAccountID) { _ in
            clearSelectedChatForAccountChange()
            Task { await AIMobileScreenCapture.shared.stop() }
        }
        .onChange(of: appState.groups.map(\.groupID)) { joinedGroupIDs in
            if let activeChatGroup,
               !joinedGroupIDs.contains(activeChatGroup.groupID) {
                self.activeChatGroup = nil
            }
            if let pendingCreatedGroup,
               !joinedGroupIDs.contains(pendingCreatedGroup.groupID) {
                self.pendingCreatedGroup = nil
            }
        }
        .onChange(of: systemNotificationCenter.pendingRouteRevision) { _ in
            schedulePendingNotificationRoute(sceneIsActive: scenePhase == .active)
        }
        .onChange(of: systemNotificationCenter.isDoNotDisturbEnabled) { enabled in
            if enabled { isShowingNotificationPermissionPrompt = false }
        }
        .onChange(of: scenePhase) { phase in
            AppDiagnosticLog.shared.record(
                level: .info,
                category: "app",
                event: "scene-phase-changed",
                fields: ["phase": phase.diagnosticName]
            )
            if phase == .active {
                IOSBackgroundActivityKeeper.shared.end(cause: "scene-active")
                appState.synchronizeSystemNotificationBadge()
                schedulePendingNotificationRoute(sceneIsActive: true)
                return
            }
            guard phase == .inactive || phase == .background else { return }
            if phase == .background {
                IOSBackgroundActivityKeeper.shared.beginIfNeeded()
            }
            Task { @MainActor in
                let historyFlushed = await appState.flushHistoryPersistence()
                if !historyFlushed {
                    AppDiagnosticLog.shared.record(
                        level: .error,
                        category: "remote-im",
                        event: "history-flush-failed",
                        fields: [
                            "cause": phase == .background
                                ? "scene-background"
                                : "scene-inactive",
                        ]
                    )
                }
                guard phase == .background else { return }
                await appState.stopRemoteDesktopView(cause: "scene-background")
                await AppDiagnosticLog.shared.flush()
            }
        }
        .overlay(alignment: .top) {
            if !appState.shouldShowInitialLogin, let errorMessage = appState.errorMessage {
                Text(errorMessage)
                    .font(AppTypography.detail)
                    .foregroundStyle(.white)
                    .lineLimit(2)
                    .truncationMode(.tail)
                    .accessibilityLabel(errorMessage)
                    .padding(.horizontal, 14)
                    .padding(.vertical, 9)
                    .background(.red, in: Capsule())
                    // Keep transient IM errors below the 54-point page header.
                    // A long error previously covered every top-bar action.
                    .padding(.top, 64)
                    .padding(.horizontal, 16)
                    .onTapGesture {
                        if appState.errorMessage == errorMessage {
                            appState.errorMessage = nil
                        }
                    }
                    .transition(.move(edge: .top).combined(with: .opacity))
            }
        }
        .animation(.easeOut(duration: 0.18), value: appState.errorMessage)
        // PDF 密码只在本机 sheet 内输入；私有回调一次性取走，Root 不读取或序列化密码。
        .sheet(item: Binding(get: { pdfPasswordRequests.presentation }, set: { request in
            if request == nil, let current = pdfPasswordRequests.presentation {
                pdfPasswordRequests.dismissed(current.id)
            }
        })) { request in
            NavigationStack { AIPdfPasswordView(request: request) }
                .environmentObject(appState)
        }
        // 普通设置页与工具复用同一 SecureField 表单；这里只路由展示，不接触密码内容。
        .sheet(item: Binding(get: { passwordRequests.presentation }, set: { request in
            if request == nil, let current = passwordRequests.presentation {
                passwordRequests.dismissed(current.id)
            }
        })) { request in
            NavigationStack { AccountPasswordView(request: request) }
                .environmentObject(appState)
        }
        .onAppear { passwordRequests.presenterActive = true }
        .onDisappear {
            passwordRequests.presenterActive = false
            passwordRequests.cancelForAccountChange()
            pdfPasswordRequests.cancelAll()
        }
        .sheet(isPresented: $isShowingCreateGroup, onDismiss: {
            if let pendingCreatedGroup {
                activeChatContact = nil
                activeChatGroup = pendingCreatedGroup
                self.pendingCreatedGroup = nil
            } else if pendingAddFriendAfterGroup {
                pendingAddFriendAfterGroup = false
                appState.newContactUserID = ""
                isShowingAddContact = true
            }
        }) {
            GroupCreationView(
                friends: appState.chatState.contacts.filter {
                    appState.acceptedFriendIMIDs.contains($0.userID)
                },
                recoveryGroup: appState.groupCreationRecovery,
                refreshFriends: { await appState.refreshAcceptedFriends() },
                create: { name, memberUserIDs in
                    guard let group = await appState.createGroup(
                        name: name, memberUserIDs: memberUserIDs
                    ) else { return false }
                    pendingCreatedGroup = group
                    return true
                },
                addFriend: {
                    pendingAddFriendAfterGroup = true
                    isShowingCreateGroup = false
                },
                openRecoveredGroup: {
                    guard let group = appState.groupCreationRecovery else { return }
                    appState.selectGroup(groupID: group.groupID)
                    pendingCreatedGroup = group
                    isShowingCreateGroup = false
                }
            )
        }
        .sheet(isPresented: $isShowingQRScanner, onDismiss: {
            guard let pendingScannedFriendID else { return }
            appState.newContactUserID = pendingScannedFriendID
            self.pendingScannedFriendID = nil
            isShowingAddContact = true
        }) {
            MaiChatQRScanSheet { userID in
                pendingScannedFriendID = userID
                isShowingQRScanner = false
            }
        }
        .sheet(item: $videoCircle.presentation) { presentation in
            switch presentation {
            case .circle:
                MaiVideoCircleView { videoCircle.presentation = nil }
            case let .publish(source):
                MaiVideoCirclePublishView(source: source,
                                          close: { videoCircle.presentation = nil },
                                          published: { videoCircle.presentation = .circle })
            }
        }
    }

    private func openAssistant() {
        if hasOpenedAssistant {
            logAIHistoryEvent(
                """
                open reused selected=\(AIAssistantModel.shared.selected) \
                last=\(AIAssistantModel.shared.messages.last?.id ?? "none")
                """
            )
            selectedTab = .assistant
            return
        }
        logAIHistoryEvent("open first begin tab=\(selectedTab)")
        assistantOpenGeneration += 1
        let generation = assistantOpenGeneration
        let origin = selectedTab
        Task {
            await AIAssistantModel.shared.prepareFirstPage()
            guard assistantOpenGeneration == generation,
                  selectedTab == origin,
                  !appState.shouldShowInitialLogin else { return }
            logAIHistoryEvent(
                """
                open first visible selected=\(AIAssistantModel.shared.selected) \
                count=\(AIAssistantModel.shared.messages.count) \
                last=\(AIAssistantModel.shared.messages.last?.id ?? "none")
                """
            )
            selectedTab = .assistant
        }
    }

    private func offerNotificationPermissionIfNeeded() async {
        guard !systemNotificationCenter.isDoNotDisturbEnabled else { return }
#if targetEnvironment(simulator)
        if ProcessInfo.processInfo.arguments.contains("--app-bars-layout-ui-test") ||
            ProcessInfo.processInfo.arguments.contains("--ai-session-reveal-ui-test") ||
            ProcessInfo.processInfo.arguments.contains("--ai-image-mixed-ui-test") ||
            ProcessInfo.processInfo.environment["MAICHAT_IMAGE_ZOOM_ANCHOR_TEST"] == "1" ||
            ProcessInfo.processInfo.arguments.contains("--im-group-ui-test") ||
            ProcessInfo.processInfo.arguments.contains("--im-group-activation-fail-ui-test") {
            return
        }
#endif
        await systemNotificationCenter.refreshAuthorizationStatus()
        guard !systemNotificationCenter.isDoNotDisturbEnabled,
              systemNotificationCenter.authorizationStatus == .notDetermined,
              !notificationPermissionPromptDismissed
        else { return }
        isShowingNotificationPermissionPrompt = true
    }

    private func clearSelectedChatForAccountChange() {
        activeChatContact = nil
        activeChatGroup = nil
        pendingCreatedGroup = nil
        isShowingCreateGroup = false
    }

    private func dismissNotificationPermissionPrompt() {
        notificationPermissionPromptDismissed = true
        isShowingNotificationPermissionPrompt = false
    }

    private func enableNotifications() {
        notificationPermissionPromptDismissed = true
        isShowingNotificationPermissionPrompt = false
        Task {
            await systemNotificationCenter.requestAuthorizationIfNeeded()
        }
    }

    private func saveRenamedContactGroup() {
        guard let originalName = renamingContactGroup else { return }
        if !appState.renameContactGroup(from: originalName, to: contactGroupRenameDraft) {
            appState.errorMessage = "分组名不能为空，且不能与已有分组重名"
        }
        renamingContactGroup = nil
    }

    private func deleteContactGroup() {
        guard let groupName = deletingContactGroup else { return }
        _ = appState.deleteContactGroup(name: groupName)
        deletingContactGroup = nil
    }

    private func schedulePendingNotificationRoute(sceneIsActive: Bool) {
        guard sceneIsActive else { return }
        // 通知点击到达时场景可能仍处于 inactive；推迟到主队列下一轮，
        // 避免在 UIKit 状态恢复事务中直接跳转会话。
        DispatchQueue.main.async {
            MainActor.assumeIsolated {
                guard let peerUserID = systemNotificationCenter.consumePendingPeerUserID()
                else { return }
                openNotificationConversation(peerUserID)
            }
        }
    }

    private func openNotificationConversation(_ peerUserID: String) {
        guard let contact = appState.chatState.contacts.first(where: { $0.userID == peerUserID })
        else { return }
        appState.selectContact(contact)
        selectedTab = .messages
        activeChatGroup = nil
        activeChatContact = contact
        AppDiagnosticLog.shared.record(
            level: .info,
            category: "remote-im",
            event: "notification-clicked",
            fields: [
                "peer": DiagnosticLogPrivacy.stableTag(peerUserID, prefix: "u"),
                "main_thread": Thread.isMainThread ? "true" : "false",
            ]
        )
    }
}

private struct NotificationPermissionDialog: View {
    let later: () -> Void
    let enable: () -> Void

    var body: some View {
        ZStack {
            Color.black.opacity(0.28)
                .ignoresSafeArea()
                .contentShape(Rectangle())
                .onTapGesture(perform: later)

            VStack(alignment: .leading, spacing: 16) {
                HStack(spacing: 12) {
                    Image(systemName: "bell.badge.fill")
                        .font(AppTypography.symbol(size: 19, weight: .semibold))
                        .foregroundStyle(RemoteIMStyle.blue)
                        .frame(width: 42, height: 42)
                        .background(RemoteIMStyle.blueSoft, in: Circle())
                    VStack(alignment: .leading, spacing: 3) {
                        Text("开启消息通知")
                            .font(AppTypography.title.weight(.bold))
                            .foregroundStyle(RemoteIMStyle.textPrimary)
                        Text("仅在 MaiChat 退到后台时提醒新消息")
                            .font(AppTypography.detail)
                            .foregroundStyle(RemoteIMStyle.textSecondary)
                    }
                }

                HStack(spacing: 12) {
                    Button("稍后") {
                        later()
                    }
                    .buttonStyle(.plain)
                    .font(AppTypography.body.weight(.semibold))
                    .foregroundStyle(RemoteIMStyle.textPrimary)
                    .frame(maxWidth: .infinity)
                    .frame(height: 46)
                    .background(
                        RemoteIMStyle.pageBackground,
                        in: RoundedRectangle(cornerRadius: 12, style: .continuous)
                    )

                    Button("开启通知") {
                        enable()
                    }
                    .buttonStyle(.plain)
                    .font(AppTypography.body.weight(.semibold))
                    .foregroundStyle(.white)
                    .frame(maxWidth: .infinity)
                    .frame(height: 46)
                    .background(
                        RemoteIMStyle.blue,
                        in: RoundedRectangle(cornerRadius: 12, style: .continuous)
                    )
                }
            }
            .padding(20)
            .frame(maxWidth: 360)
            .background(
                RemoteIMStyle.panelBackground,
                in: RoundedRectangle(cornerRadius: 18, style: .continuous)
            )
            .overlay {
                RoundedRectangle(cornerRadius: 18, style: .continuous)
                    .stroke(RemoteIMStyle.border, lineWidth: 1)
            }
            .shadow(color: Color.black.opacity(0.16), radius: 24, y: 10)
            .padding(.horizontal, 24)
        }
        .accessibilityElement(children: .contain)
        .accessibilityAction(.escape, later)
    }
}

private extension ScenePhase {
    var diagnosticName: String {
        switch self {
        case .active: return "active"
        case .inactive: return "inactive"
        case .background: return "background"
        @unknown default: return "unknown"
        }
    }
}

private struct RootTabBar: View {
    @Binding var selectedTab: AppTab
    @ObservedObject var remoteDesktop: RemoteDesktopSession
    let openAssistant: () -> Void

    var body: some View {
        if selectedTab != .remote || !remoteDesktop.state.isActive {
            CompactTabBar(selectedTab: $selectedTab, openAssistant: openAssistant)
        }
    }
}

private struct InitialLoginView: View {
    var body: some View {
        PhoneAccountLoginView()
    }
}

private struct CompactTabBar: View {
    @Binding var selectedTab: AppTab
    let openAssistant: () -> Void
    @EnvironmentObject private var appState: RemoteIMAppState

    var body: some View {
        HStack(spacing: 8) {
            TabButton(
                title: "消息",
                imageName: "NavMessages",
                accent: RemoteIMStyle.navMessages,
                selected: selectedTab == .messages,
                badgeCount: appState.totalUnreadCount
            ) {
                selectedTab = .messages
            }
            TabButton(
                title: "云盘",
                imageName: "",
                accent: RemoteIMStyle.blue,
                selected: selectedTab == .cloudDrive,
                systemImageName: "icloud"
            ) {
                selectedTab = .cloudDrive
            }
            TabButton(
                title: "创作中心",
                imageName: "NavAssistant",
                accent: RemoteIMStyle.navAssistant,
                selected: selectedTab == .assistant
            ) {
                openAssistant()
            }
            TabButton(
                title: "好友列表",
                imageName: "NavContacts",
                accent: RemoteIMStyle.navContacts,
                selected: selectedTab == .contacts
            ) {
                selectedTab = .contacts
            }
            TabButton(
                title: "我的",
                imageName: "NavProfile",
                accent: RemoteIMStyle.navProfile,
                selected: selectedTab == .me
            ) {
                selectedTab = .me
            }
        }
        .padding(.horizontal, 16)
        .padding(.top, 10)
        .padding(.bottom, 12)
        .frame(maxWidth: .infinity)
        .remoteIMBarBackground(edges: .bottom,
            opacity: selectedTab == .messages || selectedTab == .assistant
                ? RemoteIMStyle.conversationBarOpacity : 0.9)
        .overlay(alignment: .top) {
            RemoteIMBarSeparator()
        }
    }
}

private struct TabButton: View {
    let title: String
    let imageName: String
    let accent: Color
    let selected: Bool
    var badgeCount = 0
    var systemImageName: String? = nil
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            ZStack(alignment: .topTrailing) {
                if let systemImageName {
                    Image(systemName: selected ? systemImageName + ".fill" : systemImageName)
                        .font(AppTypography.symbol(size: 23))
                        .frame(width: 23, height: 23)
                } else {
                    Image(selected ? imageName + "Filled" : imageName)
                        .renderingMode(.template)
                        .resizable()
                        .scaledToFit()
                        .frame(width: 23, height: 23)
                }
                if badgeCount > 0 {
                    Circle()
                        .fill(Color(red: 1.0, green: 0.235, blue: 0.188))
                        .frame(width: 8, height: 8)
                        .offset(x: 6, y: -2)
                }
            }
            .frame(maxWidth: .infinity, minHeight: 44)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .foregroundStyle(selected ? accent : Color.secondary)
        .accessibilityLabel(title)
        .accessibilityValue(accessibilityValue)
    }

    private var accessibilityValue: String {
        [selected ? "已选择" : nil, badgeCount > 0 ? "\(badgeCount) 条未读" : nil]
            .compactMap { $0 }
            .joined(separator: "，")
    }
}
