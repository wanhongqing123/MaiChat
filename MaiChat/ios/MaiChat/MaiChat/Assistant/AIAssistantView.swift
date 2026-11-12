// 文件作用：组织会话目录与创作页面的联动侧滑，以及消息、附件、输入栏和语音交互。
// 消息列表只渲染模型已经加载的页面；切 Session 后定位到新会话真实的最新消息。
// 用户上滑阅读旧页时保留当前位置；新消息、键盘或旋转只在需要跟随底部时校正。
// 会话读写与 Agent 任务由 AIAssistantModel 管理；本文件负责视图生命周期和操作反馈。
import AVFoundation
import CoreTransferable
import MaiChatCore
import MediaPlayer
import Photos
import PhotosUI
import QuickLook
import StoreKit
import SwiftUI
import UIKit
import UniformTypeIdentifiers

struct AIImageSourceFramesKey: PreferenceKey {
    static let defaultValue: [UUID: CGRect] = [:]
    static func reduce(value: inout [UUID: CGRect], nextValue: () -> [UUID: CGRect]) {
        value.merge(nextValue(), uniquingKeysWith: { _, latest in latest })
    }
}

@MainActor
final class AIImageSourceFrameStore: ObservableObject {
    @Published private(set) var frames: [UUID: CGRect] = [:]
    func update(_ value: [UUID: CGRect]) {
        if frames != value { frames = value }
    }
}

struct AIImagePreviewVisibilityKey: PreferenceKey {
    static let defaultValue = false
    static func reduce(value: inout Bool, nextValue: () -> Bool) { value = value || nextValue() }
}

private struct AIImagePreviewSourceKey: EnvironmentKey {
    static let defaultValue: UUID? = nil
}

private struct AIConversationActionsAllowedKey: EnvironmentKey {
    // 仅媒体按钮读取实时检查；普通消息行不依赖此环境，避免拖动时重绘整段 Markdown。
    static let defaultValue: @MainActor () -> Bool = { true }
}

private extension EnvironmentValues {
    var aiImagePreviewSourceID: UUID? {
        get { self[AIImagePreviewSourceKey.self] }
        set { self[AIImagePreviewSourceKey.self] = newValue }
    }
}

extension EnvironmentValues {
    var aiConversationActionsAllowed: @MainActor () -> Bool {
        get { self[AIConversationActionsAllowedKey.self] }
        set { self[AIConversationActionsAllowedKey.self] = newValue }
    }
}

private struct AIRemoteDeviceGroup: Identifiable {
    let id: String
    let name: String
    let device: AIRemoteDevice
    var online: Bool
    var sessions: [AIRemoteSession]
}

/// 单次侧滑的方向锁和连续位移；稳定状态必须保留到 onEnded 提交完成。
private struct AISessionDrawerDrag {
    var horizontal: Bool?
    var translation: CGFloat = 0
}


struct AIAssistantView: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    let isActive: Bool
    let hostsImagePreview: Bool
    private let bottomBar: AnyView?
    @ObservedObject private var model = AIAssistantModel.shared
    @ObservedObject private var videoCircle = MaiVideoCircleModel.shared
    @ObservedObject private var cloudDrive = MaiCloudDriveModel.shared
    @ObservedObject private var browser = AIMobileBrowserTools.shared
    @Environment(\.scenePhase) private var scenePhase
    @State private var showSessions = false
    @State private var showActions = false
    @State private var confirmClear = false
    @State private var imageSources = AIImageSourceFrameStore()
    @State private var confirmRemoteDelete = false
    @State private var remoteDeleteTarget: AIRemoteSession?
    // GestureState 会在 onEnded 前清零，不能用它保存待提交的方向/位移。
    // 仅用它通知中断；拖动数据保留到显式提交，取消时再归还稳定端点。
    @State private var sessionDrawerDrag = AISessionDrawerDrag()
    @GestureState private var sessionDrawerGestureActive = false
    // 命中禁用不能阻止已经按下的行在松手时回调；动作本身也须读这个实时互锁。
    @State private var sessionActionsLocked = false
    @State private var sessionMotionGeneration: UInt64 = 0
    @State private var showsWideSidebar = true
    @State private var collapsedSessionDevices: Set<String> = []
    @State private var isAttachmentPanelPresented = false
    @State private var composerFocusController = AIComposerFocusController()
    @State private var transcriptionPresentation = VoiceTranscriptionPresentation()

    init(isActive: Bool = true, hostsImagePreview: Bool = true, bottomBar: AnyView? = nil) {
        self.isActive = isActive
        self.hostsImagePreview = hostsImagePreview
        self.bottomBar = bottomBar
    }

    private var usesStandaloneUITestFixture: Bool {
        #if DEBUG && targetEnvironment(simulator)
        return AIAssistantUITestRouting.usesStandaloneFixture
        #else
        return false
        #endif
    }

    var body: some View {
        GeometryReader { viewport in
            content(viewportSize: viewport.size, safeAreaInsets: viewport.safeAreaInsets)
        }
    }

    /// 根据横竖屏尺寸装配标题、消息列表、输入栏和弹层。
    /// 会话数据仍由原页面持有；全屏视频不改变主页面尺寸，弹层不参与消息区测量。
    private func content(viewportSize: CGSize, safeAreaInsets: EdgeInsets) -> some View {
        let wideLayout = viewportSize.width >= 700 && viewportSize.width > viewportSize.height
        let width = min(360, viewportSize.width * 0.78)
        let distance = wideLayout ? 0 : max(0, min(width,
            (showSessions ? width : 0) + sessionDrawerDrag.translation))
        let progress = width > 0 ? distance / width : 0
        // 参考用户录屏：430 pt 窗口展开约 336 pt；原页面缩至约 95%，目录从左侧联动进入。
        // 起拖时即显露大圆角，收回最后 12 pt 时才贴回窗口，避免交界一直像直角挡板。
        let card = RoundedRectangle(cornerRadius: 50 * min(1, distance / 12), style: .circular)
        let backdropOpacity = wideLayout ? 0 : 0.22 * (1 - progress)
        return ZStack(alignment: .topTrailing) {
            if !wideLayout {
                // 两页沿同一个位移联动；保持目录完整排版，只平移，不再按已揭示宽度裁切。
                // 录屏中目录在展开途中逐渐变亮；轮廓与投影跟随右侧移动页面。
                sessionList(wideLayout: false)
                    .frame(width: width)
                    .padding(.top, safeAreaInsets.top)
                    .padding(.bottom, safeAreaInsets.bottom)
                    .background(Color(uiColor: .systemBackground))
                    .overlay {
                        Color.black.opacity(backdropOpacity).allowsHitTesting(false)
                    }
                    .offset(x: distance - width)
                    .padding(.top, -safeAreaInsets.top)
                    .padding(.bottom, -safeAreaInsets.bottom)
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .leading)
                    // 防止窄屏视频分区中的目录平移绘制到相邻播放器，竖向仍覆盖系统安全区。
                    .mask {
                        Rectangle().padding(.top, -safeAreaInsets.top)
                            .padding(.bottom, -safeAreaInsets.bottom)
                    }
                    .allowsHitTesting(showSessions)
                    .accessibilityHidden(!showSessions)
                    .zIndex(0)
            }
            HStack(spacing: 0) {
                if wideLayout && showsWideSidebar {
                    // 宽屏固定展示会话列表；窄屏在同一容器内连续平移两个页面。
                    sessionList(wideLayout: true)
                        .frame(width: min(280, viewportSize.width * 0.3))
                    Divider()
                }
                VStack(spacing: 0) {
                    // 首次准备期间显示明确进度；已有会话的刷新继续展示旧消息，
                    // 待读取完成后才在相同容器内更新可见页。
                    if !model.ready {
                        ProgressView("正在打开创作中心")
                            .frame(maxWidth: .infinity, maxHeight: .infinity)
                    } else {
                        if let pending = model.pendingRemoteSelection {
                            HStack(spacing: 8) {
                                ProgressView()
                                Text("正在读取 \(pending.deviceName) 的对话…")
                                    .font(AppTypography.metadata)
                                    .foregroundStyle(.secondary)
                            }
                            .frame(maxWidth: .infinity)
                            .padding(.vertical, 8)
                            .accessibilityIdentifier("ai-remote-session-loading")
                        }
                        AIAssistantMessageList(
                            model: model,
                            focusController: composerFocusController,
                            isAttachmentPanelPresented: $isAttachmentPanelPresented,
                            isActive: isActive,
                            blocksVerticalScrolling: !wideLayout &&
                                (showSessions || sessionActionsLocked ||
                                 sessionDrawerDrag.horizontal == true)
                        )
                        .frame(maxWidth: 900)
                        .frame(maxWidth: .infinity)
                    }
                }
                .safeAreaInset(edge: .top, spacing: 0) {
                    AIHeader(
                        openSessions: {
                            composerFocusController.dismiss()
                            isAttachmentPanelPresented = false
                            showActions = false
                            openSessionDrawer(wideLayout: wideLayout)
                        },
                        openActions: {
                            #if DEBUG
                            AISessionDrawerDiagnostics.menuTap(blocked: conversationActionsBlocked,
                                                               shown: showActions)
                            #endif
                            guard !conversationActionsBlocked else { return }
                            composerFocusController.dismiss()
                            isAttachmentPanelPresented = false
                            showActions.toggle()
                        },
                        openVideoCircle: videoCircle.availability == .available ? {
                            guard !conversationActionsBlocked else { return }
                            showActions = false
                            videoCircle.presentation = .circle
                        } : nil
                    )
                }
                .safeAreaInset(edge: .bottom, spacing: 0) {
                    VStack(spacing: 0) {
                    if !model.error.isEmpty {
                        HStack(alignment: .top) {
                            Image(systemName: "exclamationmark.circle")
                            Text(model.error).font(AppTypography.metadata).textSelection(.enabled)
                            Spacer(minLength: 0)
                            Button { model.error = "" } label: { Image(systemName: "xmark") }
                        }.foregroundStyle(.red).padding(12).background(Color.red.opacity(0.05))
                    }
                    // 输入栏使用独立草稿和焦点状态；转写悬浮层由窗口顶层呈现，
                    // 不把转写卡片高度写回消息滚动视口。
                    AIComposer(
                        model: model,
                        snapshot: AIComposerSnapshot(
                            ready: model.ready,
                            busy: model.busy,
                            isSubmitting: model.isSubmitting,
                            uploadStatus: model.remoteUploadStatus,
                            isCreatingConversation: model.isCreatingConversation,
                            selected: model.selected,
                            quotedMessage: model.quotedMessage,
                            attachmentPanelPresented: isAttachmentPanelPresented,
                            attachments: model.attachments(for: model.selected),
                            workspacePath: model.workspacePath,
                            remoteRestoreText: model.remoteRestoreText,
                            remoteRestoreGeneration: model.remoteRestoreGeneration,
                            screenFrameImportRevision: model.screenFrameImportRevision,
                            screenFrameAccountID: model.screenFrameCurrentAccountID,
                            screenFrameOwnerID: model.selectedRemote?.id ?? model.selected
                        ),
                        focusController: composerFocusController,
                        transcriptionPresentation: transcriptionPresentation,
                        isAttachmentPanelPresented: $isAttachmentPanelPresented
                    )
                    .equatable()
                    .frame(maxWidth: 900)
                    .frame(maxWidth: .infinity)
                    }
                    // RootTab 独占 home 安全区；composer 只画自身，不能再次向底部叠加半透明层。
                    .remoteIMBarBackground(opacity: RemoteIMStyle.conversationBarOpacity)
                    .overlay(alignment: .top) { RemoteIMBarSeparator() }
                }
                .background(Color(uiColor: .systemBackground), ignoresSafeAreaEdges: [])
            }
            .safeAreaInset(edge: .bottom, spacing: 0) {
                if let bottomBar { bottomBar }
            }
            .overlay(alignment: .topTrailing) {
                if showActions {
                    // 右上角操作面板只处理模型、权限与会话清理；
                    // 遮罩与面板共用明确的右上角容器，避免多根 overlay 的默认居中布局。
                    // 容器跟随当前创作页面尺寸，旋转后仍紧贴该页标题栏。
                    ZStack(alignment: .topTrailing) {
                        Color.black.opacity(0.001).ignoresSafeArea().contentShape(Rectangle())
                            .onTapGesture { showActions = false }
                        AIActionPanel(
                            maximumHeight: max(100, viewportSize.height - safeAreaInsets.top - safeAreaInsets.bottom - 68),
                            selectedModel: model.selectedRemote != nil && model.selectedRemote?.model == nil
                                ? "所属设备未提供配置" : model.settings.displayName,
                            selectedModelID: model.settings.model,
                            isLocalModel: model.settings.isLocalModel,
                            allowLocalModel: model.selectedRemote == nil,
                            changingModelSource: model.changingModelSource,
                            selectedEffort: model.selectedRemote != nil && model.selectedRemote?.model == nil
                                ? "unavailable" : model.settings.effectiveReasoningEffort,
                            selectedPolicy: model.settings.policy,
                            canChangeModel: model.canChangeSessionModel,
                            canChangeEffort: model.canChangeSessionReasoningEffort,
                            canChangeSettings: model.selectedRemote == nil && model.configured &&
                                !model.busy && !model.isSubmitting && !model.changingModelSource,
                            canClear: model.selectedRemote == nil && !model.busy &&
                                !model.selected.isEmpty,
                            selectEffort: { selected in
                                guard selected != model.settings.effectiveReasoningEffort else { return }
                                Task { await model.selectReasoningEffort(selected) }
                            },
                            selectPolicy: { policy in
                                guard policy != model.settings.policy else { return }
                                var next = model.settings
                                next.policy = policy
                                Task { _ = await model.save(next) }
                            },
                            selectModel: { selected in
                                Task { await model.selectPrimaryModel(selected) }
                            },
                            clear: { showActions = false; confirmClear = true }
                        )
                        .padding(.top, 54).padding(.trailing, 14)
                        .transition(.scale(scale: 0.96, anchor: .topTrailing).combined(with: .opacity))
                    }
                }
            }
            // 右页的消息、输入和底部入口共用互锁；返回目录的透明按钮在禁用范围之外。
            // 这里只锁横向拖动、回弹及目录展开态，不能因普通纵向阅读而禁用 ScrollView。
            .environment(\.aiConversationActionsAllowed, { !conversationActionsBlocked })
            .disabled(!wideLayout && conversationActionsBlocked)
            .allowsHitTesting(wideLayout || !conversationActionsBlocked)
            .accessibilityElement(children: .contain)
            .accessibilityIdentifier("ai-conversation-card")
            .accessibilityHidden(!wideLayout && showSessions)
            .overlay {
                if !wideLayout && showSessions {
                    // 移出的创作页面仍保留原会话画面，点击只收起目录，不能误触发送或导航。
                    Button {
                        guard !sessionActionsBlocked else { return }
                        closeSessionDrawer()
                    } label: {
                        Color.clear.contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel("收起对话列表")
                    .accessibilityIdentifier("ai-session-return")
                }
            }
            // 录屏里的圆角轮廓属于移动页面；统一包住标题、消息、输入和导航再缩放，
            // 不改变消息视口的排版尺寸，投影只由页面轮廓产生，不分散到原生子视图。
            .padding(.top, safeAreaInsets.top)
            .padding(.bottom, safeAreaInsets.bottom)
            .background(Color(uiColor: .systemBackground))
            .clipShape(card)
            .compositingGroup()
            .background {
                card.fill(Color(uiColor: .systemBackground))
                    .shadow(color: .black.opacity(0.15 * progress), radius: 18, x: -5, y: 0)
            }
            .scaleEffect(1 - 0.05 * progress, anchor: .leading)
            .offset(x: distance)
            .padding(.top, -safeAreaInsets.top)
            .padding(.bottom, -safeAreaInsets.bottom)
            .zIndex(10)
            if hostsImagePreview {
                // 独立宿主复用同一预览；正常 App 由 Root 在主页面上方整窗覆盖。
                AIAssistantImagePreviewLayer(sources: imageSources).zIndex(30)
            }
        }
        .environment(\.aiImagePreviewSourceID, model.previewImage?.sourceID)
        .onPreferenceChange(AIImageSourceFramesKey.self) { if hostsImagePreview { imageSources.update($0) } }
        .statusBarHidden(hostsImagePreview && model.previewImage != nil)
        .onChange(of: model.previewImage?.filePath) { path in
            MaiCloudDriveAgentPlaybackCache.shared.previewChanged(to: path)
        }
        .background {
            Color(uiColor: .systemBackground)
                .overlay(Color.black.opacity(backdropOpacity))
                .ignoresSafeArea()
        }
        .coordinateSpace(name: "ai-session-reveal")
        // 网页在不遮挡聊天的背景宿主中保留渲染；只有明确点击任务入口才移交全屏页面。
        .background {
            AIBrowserWebContainer(browser: browser, presentation: false)
                .allowsHitTesting(false).accessibilityHidden(true)
        }
        .fullScreenCover(isPresented: Binding(
            get: { browser.isPresented },
            set: { if !$0 { browser.dismissPage() } }
        )) {
            AIAssistantBrowserScreen(browser: browser)
        }
        .simultaneousGesture(sessionDrawerGesture(wideLayout: wideLayout, width: width))
        .onChange(of: sessionDrawerGestureActive) { active in
            if !active && sessionDrawerDrag.horizontal != nil {
                if sessionDrawerDrag.horizontal == true {
                    settleSessionDrawer(expanded: showSessions)
                } else {
                    sessionDrawerDrag = AISessionDrawerDrag()
                }
            }
        }
        .onChange(of: wideLayout) { wide in
            if wide {
                sessionMotionGeneration &+= 1
                sessionActionsLocked = false
                sessionDrawerDrag = AISessionDrawerDrag()
                showSessions = false
            }
        }
        .confirmationDialog("清空当前对话的所有消息？", isPresented: $confirmClear, titleVisibility: .visible) {
            Button("清空消息", role: .destructive) { Task { await model.action("clear") } }
        }
        .alert("删除远端对话", isPresented: $confirmRemoteDelete) {
            Button("取消", role: .cancel) { remoteDeleteTarget = nil }
            Button("删除", role: .destructive) {
                guard let target = remoteDeleteTarget else { return }
                remoteDeleteTarget = nil
                Task { _ = await model.deleteRemoteSession(target) }
            }
        } message: {
            Text("删除“\(remoteDeleteTarget?.displayTitle ?? "对话")”及其中的聊天记录？此操作无法撤销。")
        }
        .task(id: isActive) {
            guard isActive && !usesStandaloneUITestFixture else { return }
            await videoCircle.probe()
            cloudDrive.activate(accountID: appState.maiChatAccountID)
            await cloudDrive.probe()
        }
        .onAppear {
            browser.setPageVisible(isActive)
            logAIHistoryEvent("page-appeared session=\(model.selected) active=\(isActive)")
            if isActive && !usesStandaloneUITestFixture {
                model.appear()
            }
        }
        .onChange(of: isActive) { active in
            browser.setPageVisible(active)
            if !active { browser.dismissPage() }
            if !active { AIMobileLocalMediaRequests.shared.invalidateManualPhoto() }
            guard !usesStandaloneUITestFixture else { return }
            if active {
                model.setPageActive(true)
            } else {
                composerFocusController.dismiss()
                isAttachmentPanelPresented = false
                transcriptionPresentation.onCancel?()
                model.setPageActive(false)
            }
        }
        .onDisappear {
            // 全屏网页仍属于当前会话；呈现它不应让工具误判为已经离开创作中心。
            if !browser.isPresented { browser.setPageVisible(false) }
            logAIHistoryEvent("page-disappeared session=\(model.selected) active=\(isActive)")
            if !usesStandaloneUITestFixture && !browser.isPresented {
                model.disappear()
            }
        }
        .onChange(of: scenePhase) { phase in
            browser.setPageVisible(phase == .active && isActive)
            guard !usesStandaloneUITestFixture else { return }
            if phase == .active { model.appear() }
            else { model.disappear() }
        }
        .onChange(of: model.selected) { selected in
            if browser.ownerSessionID != selected { browser.leaveSession() }
        }
        .onChange(of: model.selectedRemote?.id) { remote in
            if remote != nil { browser.leaveSession() }
        }
        .onChange(of: appState.maiChatAccountID) { _ in
            browser.close()
        }
    }

    private static let drawerSettleDuration: Double = 0.24

    private var sessionActionsBlocked: Bool {
        sessionActionsLocked || sessionDrawerGestureActive || sessionDrawerDrag.horizontal == true
    }

    private var conversationActionsBlocked: Bool {
        showSessions || sessionActionsLocked || sessionDrawerDrag.horizontal == true
    }

    /// 覆盖拖动、松手及回弹；代次检查防止旧动画完成时解除新一轮拖动的互锁。
    private func settleSessionDrawer(expanded: Bool,
                                     duration: Double = Self.drawerSettleDuration) {
        #if DEBUG
        AISessionDrawerDiagnostics.begin("settling")
        #endif
        sessionMotionGeneration &+= 1
        let generation = sessionMotionGeneration
        sessionActionsLocked = true
        let updates = {
            showSessions = expanded
            sessionDrawerDrag = AISessionDrawerDrag()
        }
        // 跟手阶段仍是无动画的 1:1 位移；松手用有界减速收尾，不能等弹簧的长尾才恢复点击。
        let animation = Animation.easeOut(duration: duration)
        if #available(iOS 17.0, *) {
            withAnimation(animation, completionCriteria: .removed, updates) {
                releaseSessionActions(generation: generation)
            }
        } else {
            withAnimation(animation, updates)
            // iOS 16 没有完成回调；覆盖同一有限时长动画及两帧余量，再释放 touch-up 互锁。
            DispatchQueue.main.asyncAfter(deadline: .now() + duration + 0.04) {
                releaseSessionActions(generation: generation)
            }
        }
    }

    private func releaseSessionActions(generation: UInt64) {
        // 即使目标没变、动画立即结束，也等本轮 touch-up 回调处理完再恢复按钮。
        DispatchQueue.main.async {
            guard generation == sessionMotionGeneration else { return }
            sessionActionsLocked = false
            #if DEBUG
            AISessionDrawerDiagnostics.finish()
            #endif
        }
    }

    /// 开合共用一个本地坐标手势。先锁定横/纵方向，纵向滚动目录或消息时不横移页面；
    /// 反向拖回与系统取消均使用同一进度，避免两层分别动画导致交界跳开。
    private func sessionDrawerGesture(wideLayout: Bool, width: CGFloat) -> some Gesture {
        DragGesture(minimumDistance: 12, coordinateSpace: .named("ai-session-reveal"))
            .updating($sessionDrawerGestureActive) { _, active, _ in active = true }
            .onChanged { value in
                guard !wideLayout, showSessions || value.startLocation.x <= 32 else { return }
                if sessionDrawerDrag.horizontal == nil {
                    sessionDrawerDrag.horizontal = abs(value.translation.width) > abs(value.translation.height)
                    if sessionDrawerDrag.horizontal == true {
                        #if DEBUG
                        AISessionDrawerDiagnostics.begin("tracking")
                        #endif
                        sessionMotionGeneration &+= 1
                        sessionActionsLocked = true
                        // 页面准备只做一次；后续每帧只提交位移，避免反复调用 UIKit 收键盘。
                        composerFocusController.dismiss()
                        isAttachmentPanelPresented = false
                        showActions = false
                    }
                }
                guard sessionDrawerDrag.horizontal == true else { return }
                #if DEBUG
                AISessionDrawerDiagnostics.update()
                #endif
                let origin = showSessions ? width : 0
                var transaction = Transaction()
                transaction.disablesAnimations = true
                withTransaction(transaction) {
                    sessionDrawerDrag.translation = max(-origin, min(width - origin, value.translation.width))
                }
            }
            .onEnded { value in
                guard !wideLayout, sessionDrawerDrag.horizontal == true else { return }
                let origin = showSessions ? width : 0
                let destination = origin + value.predictedEndTranslation.width
                let expanded = destination > width * 0.5
                let current = max(0, min(width, origin + sessionDrawerDrag.translation))
                let remaining = abs((expanded ? width : 0) - current) / max(1, width)
                // 已接近端点的松手只需短收尾，快速甩动仍按预计终点决定开合。
                let duration = max(0.1, Self.drawerSettleDuration * remaining)
                settleSessionDrawer(expanded: expanded, duration: duration)
            }
    }

    private func openSessionDrawer(wideLayout: Bool) {
        guard !sessionActionsBlocked else { return }
        logAIHistoryEvent("drawer-open-request session=\(model.selected) wide=\(wideLayout)")
        if wideLayout {
            showsWideSidebar.toggle()
            return
        }
        settleSessionDrawer(expanded: true)
    }

    private func closeSessionDrawer() {
        logAIHistoryEvent("drawer-close-request session=\(model.selected)")
        settleSessionDrawer(expanded: false)
    }

    private var remoteDeviceGroups: [AIRemoteDeviceGroup] {
        var groups: [AIRemoteDeviceGroup] = []
        var positions: [String: Int] = [:]
        for device in model.remoteDevices {
            positions[device.id] = groups.count
            groups.append(AIRemoteDeviceGroup(
                id: device.id, name: device.deviceName,
                device: device, online: device.online, sessions: []))
        }
        for session in model.remoteSessions {
            let key = "\(session.deviceSlot):\(session.deviceID)"
            if let position = positions[key] {
                groups[position].sessions.append(session)
                groups[position].online = groups[position].online || session.online
            } else {
                let device = AIRemoteDevice(
                    deviceSlot: session.deviceSlot, deviceName: session.deviceName,
                    deviceID: session.deviceID, loginSessionID: session.loginSessionID,
                    online: session.online)
                positions[key] = groups.count
                groups.append(AIRemoteDeviceGroup(
                    id: key, name: session.deviceName,
                    device: device, online: session.online, sessions: [session]
                ))
            }
        }
        return groups
    }

    private func sessionDeviceHeader(_ title: String, id: String,
                                     count: Int, online: Bool? = nil,
                                     createOn device: AIRemoteDevice? = nil) -> some View {
        let collapsed = collapsedSessionDevices.contains(id)
        let status = online.map { $0 ? "在线" : "离线" }
        return HStack(spacing: 0) {
            Button {
                guard !sessionActionsBlocked else { return }
                withAnimation(.easeInOut(duration: 0.18)) {
                    if collapsed { collapsedSessionDevices.remove(id) }
                    else { collapsedSessionDevices.insert(id) }
                }
            } label: {
                HStack(spacing: 8) {
                    Image(systemName: collapsed ? "chevron.right" : "chevron.down")
                        .font(AppTypography.symbol(size: 10, weight: .semibold))
                        .frame(width: 14)
                    AppText(title)
                        .font(AppTypography.metadata.weight(.semibold))
                        .lineLimit(1)
                        .truncationMode(.middle)
                    Spacer(minLength: 4)
                    if let status {
                        HStack(spacing: 4) {
                            Circle().fill(online == true ? RemoteIMStyle.green : Color.gray)
                                .frame(width: 5, height: 5)
                            AppText(status).font(AppTypography.metadata)
                        }
                    }
                    if count > 0 { AppText("\(count)").font(AppTypography.metadata) }
                }
                .foregroundStyle(RemoteIMStyle.textSecondary)
                .padding(.leading, 12).padding(.trailing, device == nil ? 12 : 2)
                .frame(maxWidth: .infinity, minHeight: 34)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel("\(title)，\(status ?? "本机")，\(count) 个对话")
            .accessibilityIdentifier("ai-session-device-\(id)")
            if let device {
                Button {
                    guard !sessionActionsBlocked else { return }
                    Task {
                        if await model.createRemoteSession(on: device) != nil {
                            closeSessionDrawer()
                        }
                    }
                } label: {
                    Image(systemName: "plus")
                        .font(AppTypography.symbol(size: 15, weight: .semibold))
                        .frame(width: 34, height: 34)
                }
                .buttonStyle(.plain)
                .foregroundStyle(RemoteIMStyle.blue)
                .disabled(!device.online || model.remoteSessionMutationInProgress)
                .accessibilityLabel("在\(title)上新建对话")
                .accessibilityIdentifier("ai-remote-create-\(id)")
            }
        }
        .frame(height: 34)
    }

    private func emptyDraftRow() -> some View {
        Button {
            guard !sessionActionsBlocked else { return }
            closeSessionDrawer()
            Task { await model.selectConversation("") }
        } label: {
            HStack(spacing: 10) {
                Image(systemName: "square.and.pencil")
                    .font(AppTypography.symbol(size: 14)).foregroundStyle(Color.blue)
                AppText("未发送草稿").font(AppTypography.body)
                Spacer(minLength: 8)
                if model.selected.isEmpty && model.selectedRemote == nil {
                    Image(systemName: "checkmark").foregroundStyle(Color.blue)
                }
            }
            .frame(maxWidth: .infinity, minHeight: 48, alignment: .leading)
            .padding(.leading, 34).padding(.trailing, 12)
            .background(model.selected.isEmpty && model.selectedRemote == nil
                        ? RemoteIMStyle.blueSoft : Color.clear,
                        in: RoundedRectangle(cornerRadius: 10))
        }
        .buttonStyle(.plain)
        .foregroundStyle(Color.primary)
        .accessibilityIdentifier("ai-conversation-empty-draft")
    }

    private func localSessionRow(_ session: AISession) -> some View {
        let selected = model.selectedRemote == nil && session.id == model.selected
        return HStack(spacing: 4) {
            Button {
                guard !sessionActionsBlocked else { return }
                closeSessionDrawer()
                Task { await model.selectConversation(session.id) }
            } label: {
                HStack(spacing: 8) {
                    // 会话名称属于普通主文字，和正文共用所选家族及基准字号。
                    AppText(session.displayTitle)
                        .font(AppTypography.conversationTitle)
                        .lineLimit(1)
                    Spacer(minLength: 8)
                    if session.busy { ProgressView().controlSize(.small) }
                    else if selected { Image(systemName: "checkmark").foregroundStyle(Color.blue) }
                }
                .frame(maxWidth: .infinity, minHeight: 48, alignment: .leading)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityIdentifier("ai-conversation-\(session.id)")
            Menu {
                Button(role: .destructive) {
                    guard !sessionActionsBlocked else { return }
                    Task {
                        guard !model.isSubmitting, !model.isCreatingConversation else { return }
                        await model.selectConversation(session.id)
                        guard model.selected == session.id,
                              !model.isSubmitting, !model.isCreatingConversation else { return }
                        await model.action("delete")
                    }
                } label: {
                    Label("删除对话", systemImage: "trash")
                }
            } label: {
                Image(systemName: "ellipsis")
                    .font(AppTypography.symbol(size: 15, weight: .medium))
                    .foregroundStyle(.secondary)
                    .frame(width: 36, height: 44)
            }
            .disabled(session.busy || model.isSubmitting || model.isCreatingConversation)
            .accessibilityLabel("更多操作：\(session.displayTitle)")
        }
        .foregroundStyle(Color.primary)
        .padding(.leading, 34).padding(.trailing, 4)
        .background(selected ? RemoteIMStyle.blueSoft : Color.clear,
                    in: RoundedRectangle(cornerRadius: 10))
    }

    private func remoteSessionRow(_ session: AIRemoteSession) -> some View {
        let selected = model.selectedRemote?.id == session.id
        return HStack(spacing: 4) {
            Button {
                guard !sessionActionsBlocked else { return }
                closeSessionDrawer()
                Task { await model.selectRemoteConversation(session) }
            } label: {
                HStack(spacing: 8) {
                    // 会话名称属于普通主文字，和正文共用所选家族及基准字号。
                    AppText(session.displayTitle)
                        .font(AppTypography.conversationTitle)
                        .lineLimit(1)
                    Spacer(minLength: 8)
                    if let updatedAt = session.updatedAt, updatedAt > 0 {
                        AppText(remoteUpdatedLabel(updatedAt))
                            .font(AppTypography.metadata)
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                    }
                    if session.online && session.busy { ProgressView().controlSize(.small) }
                    else if selected { Image(systemName: "checkmark").foregroundStyle(Color.blue) }
                }
                .frame(maxWidth: .infinity, minHeight: 48, alignment: .leading)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityIdentifier("ai-remote-conversation-\(session.id)")
            Menu {
                Button(role: .destructive) {
                    guard !sessionActionsBlocked else { return }
                    remoteDeleteTarget = session
                    confirmRemoteDelete = true
                } label: {
                    Label("删除对话", systemImage: "trash")
                }
            } label: {
                Image(systemName: "ellipsis")
                    .font(AppTypography.symbol(size: 15, weight: .medium))
                    .foregroundStyle(.secondary)
                    .frame(width: 36, height: 44)
            }
            .disabled(!session.online || session.busy || model.remoteSessionMutationInProgress)
            .accessibilityLabel("更多操作：\(session.displayTitle)")
            .accessibilityIdentifier("ai-remote-delete-\(session.id)")
        }
        .foregroundStyle(Color.primary)
        .padding(.leading, 34).padding(.trailing, 4)
        .background(selected ? RemoteIMStyle.blueSoft : Color.clear,
                    in: RoundedRectangle(cornerRadius: 10))
        .opacity(session.online ? 1 : 0.8)
    }

    private func remoteUpdatedLabel(_ milliseconds: Int64) -> String {
        let date = Date(timeIntervalSince1970: Double(milliseconds) / 1000)
        return Calendar.current.isDateInToday(date)
            ? date.formatted(date: .omitted, time: .shortened)
            : date.formatted(.dateTime.month().day())
    }

    /// 显示已存在的会话入口，并把点击操作交给模型先读取目标页再切换。
    /// 宽屏固定侧栏、窄屏联动目录共用同一列表，删除操作必须确认目标已选中。
    private func sessionList(wideLayout: Bool) -> some View {
            ScrollView {
                LazyVStack(spacing: 16) {
                    VStack(spacing: 2) {
                        sessionDeviceHeader(
                            UIDevice.current.userInterfaceIdiom == .pad
                                ? "这台 iPad" : "这台 iPhone",
                            id: "local",
                            count: model.sessions.count + (model.hasEmptyDraft ? 1 : 0)
                        )
                        if !collapsedSessionDevices.contains("local"),
                           model.hasEmptyDraft || !model.sessions.isEmpty {
                            VStack(spacing: 0) {
                                if model.hasEmptyDraft { emptyDraftRow() }
                                ForEach(model.sessions) { session in
                                    localSessionRow(session)
                                }
                            }
                        } else if !collapsedSessionDevices.contains("local") {
                            AppText("暂无本机对话")
                                .font(AppTypography.detail)
                                .foregroundStyle(.secondary)
                                .frame(maxWidth: .infinity, minHeight: 44, alignment: .leading)
                                .padding(.leading, 34)
                        }
                    }
                    ForEach(remoteDeviceGroups) { group in
                        VStack(spacing: 2) {
                            sessionDeviceHeader(group.name, id: group.id,
                                                count: group.sessions.count, online: group.online,
                                                createOn: group.device)
                            if !collapsedSessionDevices.contains(group.id) {
                                VStack(spacing: 0) {
                                    ForEach(group.sessions) { session in
                                        remoteSessionRow(session)
                                    }
                                    if group.sessions.isEmpty {
                                        AppText("暂无对话")
                                            .font(AppTypography.detail)
                                            .foregroundStyle(.secondary)
                                            .frame(maxWidth: .infinity, minHeight: 44,
                                                   alignment: .leading)
                                            .padding(.leading, 34)
                                    }
                                }
                            }
                        }
                    }
                }
                .padding(.horizontal, 12).padding(.vertical, 16)
                // 只禁用目录控件，ScrollView 与外层侧滑识别仍保留，纵向浏览不会被中断。
                .disabled(sessionActionsBlocked)
            }
            .safeAreaInset(edge: .top, spacing: 0) {
                HStack(spacing: 16) {
                    AppText("对话").font(AppTypography.title)
                    Spacer()
                    Button {
                        guard !sessionActionsBlocked else { return }
                        closeSessionDrawer()
                        Task { await model.create() }
                    } label: {
                        Image(systemName: "square.and.pencil")
                            .font(AppTypography.symbol(size: 18, weight: .medium))
                            .frame(width: 32, height: 32)
                    }
                    .disabled(model.isSubmitting || model.isCreatingConversation)
                    .accessibilityLabel("新对话")
                    if wideLayout {
                        Button("收起") {
                            guard !sessionActionsBlocked else { return }
                            showsWideSidebar = false
                        }
                            .font(AppTypography.detail.weight(.medium))
                    }
                }
                .buttonStyle(.plain)
                .disabled(sessionActionsBlocked)
                .foregroundStyle(.primary)
                .padding(.horizontal, 20).frame(height: 60)
                .remoteIMBarBackground(edges: .top, opacity: RemoteIMStyle.conversationBarOpacity)
                .overlay(alignment: .bottom) { RemoteIMBarSeparator() }
            }
        .background(Color(uiColor: .systemBackground), ignoresSafeAreaEdges: [])
        .accessibilityIdentifier("ai-session-drawer")
    }
}

private final class AIMessageViewportLog {
    private let state = MessageScrollViewportState()
    private var session = ""
    private var visibleHeight = -1
    private var lastReportedNearBottom: Bool?
    var nearBottom: Bool { state.nearBottom }

    func record(session: String, content: CGFloat, visible: CGFloat,
                offset: CGFloat, remaining: CGFloat, nearBottom: Bool) {
        let height = Int(visible.rounded())
        state.record(visible: visible, nearBottom: nearBottom)
        guard self.session != session || visibleHeight != height ||
            lastReportedNearBottom != nearBottom else {
            return
        }
        self.session = session
        visibleHeight = height
        lastReportedNearBottom = nearBottom
        logAIHistoryEvent(
            """
            viewport session=\(session) content=\(Int(content)) visible=\(height) offset=\(Int(offset)) \
            remaining=\(Int(remaining)) nearBottom=\(nearBottom)
            """
        )
    }

    func nearBottomBeforeKeyboard(opening: Bool) -> Bool {
        state.nearBottomBeforeKeyboard(opening: opening)
    }

    func finishKeyboardTransition() { state.finishKeyboardTransition() }

    func reset() {
        state.reset()
        session = ""
        visibleHeight = -1
        lastReportedNearBottom = nil
    }
}

// 管理创作中心消息视口：数据刷新只提供稳定消息 ID，
// 键盘尺寸变化和流式片段都不替换用户当前的阅读位置意图。
private struct AIAssistantMessageList: View {
    @ObservedObject var model: AIAssistantModel
    @ObservedObject private var videoCircle = MaiVideoCircleModel.shared
    @ObservedObject private var browser = AIMobileBrowserTools.shared
    let focusController: AIComposerFocusController
    @Binding var isAttachmentPanelPresented: Bool
    let isActive: Bool
    // 横向侧滑只移动外层页面，消息列表不能同时消费手指的纵向分量或产生惯性滚动。
    let blocksVerticalScrolling: Bool
    @State private var followsLatest = true
    @State private var prependAnchorID: String?
    @State private var viewportLog = AIMessageViewportLog()
    @State private var keyboardFollowIntent: Bool?
    @State private var keyboardTransitionOpening: Bool?
    @State private var bottomCorrectionRequest = 0

    var body: some View {
        #if DEBUG
        let _ = AISessionDrawerDiagnostics.listBody()
        #endif
        GeometryReader { geometry in
            ScrollViewReader { proxy in
                ScrollView {
                    VStack(alignment: .leading, spacing: 24) {
                        if model.messages.isEmpty && model.visibleOutgoingRows.isEmpty && model.historyLoaded {
                            welcome
                        }
                        if hasBrowserEntryInCurrentSession && browser.sourceMessageID.isEmpty {
                            // 没有前置消息的普通 App 调用留在时间线起点，不随后来消息移动。
                            browserEntry
                        }
                        if !model.messages.isEmpty {
                            if model.hasOlderVisibleMessages {
                                Button("查看更早消息", action: requestOlderMessages)
                                    .font(AppTypography.detail.weight(.medium))
                                    .foregroundStyle(.secondary)
                                    .frame(maxWidth: .infinity)
                                    .buttonStyle(.plain)
                            }
                            // 只把已开放的一页交给视图，由真实行高决定可滚动范围。
                            VStack(alignment: .leading, spacing: 24) {
                                ForEach(model.visibleMessages, id: \.displayIdentity) { message in
                                    let hasInlineBrowser = hasBrowserEntryInCurrentSession &&
                                        browser.sourceMessageID == message.id && !browser.sourcePartID.isEmpty
                                    VStack(alignment: .leading, spacing: 16) {
                                        AIMessageRow(message: message,
                                                     workspacePath: model.selectedRemote == nil ? model.workspacePath : "",
                                                     remoteImageOwner: model.remoteImageOwner,
                                                     remoteToolStates: model.remoteToolDetailStates(for: message),
                                                     questionAnswerEchoes: model.questionAnswerEchoes(for: message),
                                                     quote: {
                                                         guard message.questionProjection != true else { return }
                                                         model.quotedMessage = model.messages.first { $0.id == message.id } ?? message
                                                         focusController.focus()
                                                     },
                                                     canPublishVideo:
                                                        videoCircle.availability == .available &&
                                                        videoCircle.canPublishFromThisBuild,
                                                     imagePreviewSourceID: model.previewImage?.sourceID,
                                                     browserSessionID: hasInlineBrowser ? model.selected : nil,
                                                     browserPartID: hasInlineBrowser ? browser.sourcePartID : nil,
                                                     browserNeedsConfirmation: hasInlineBrowser &&
                                                        (!model.permissions.isEmpty || !model.questions.isEmpty),
                                                     openBrowser: openBrowser)
                                            .equatable()
                                        if hasBrowserEntryInCurrentSession && browser.sourceMessageID == message.id &&
                                            browser.sourcePartID.isEmpty && message.isLastDisplayFragment {
                                            // 普通 App 调用没有工具片段，挂在调用当时的消息之后。
                                            browserEntry
                                        }
                                    }
                                    .id(message.displayIdentity)
                                }
                            }
                        }
                        ForEach(model.visibleOutgoingRows) { row in
                            AIOutgoingMessageRowView(row: row) { model.retryOutgoingMessage(row.id) }
                                .id("outgoing-" + row.entry.requestID)
                        }
                        ForEach(model.permissions) { permission in
                            AIPermissionCard(permission: permission, model: model)
                                .id(permission.id)
                        }
                    }
                    .padding(.horizontal, 24)
                    .padding(.top, 18)
                    .padding(.bottom, 24)
                    .frame(
                        minHeight: geometry.size.height,
                        alignment: model.messages.isEmpty && model.historyLoaded &&
                            model.permissions.isEmpty && model.questions.isEmpty ? .center : .bottom
                    )
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(alignment: .bottom) {
                        MessageScrollPositionReader(
                            restoreInitialScrollableHistory: isActive && followsLatest &&
                                !model.visibleMessages.isEmpty,
                            allowsBottomFollowing: isActive && followsLatest &&
                                !model.messages.isEmpty,
                            bottomCorrectionRequest: bottomCorrectionRequest,
                            userScrollAwayThreshold: 60,
                            minimumOlderDragDistance: 24,
                            onUserScroll: {
                                logAIHistoryEvent("user-scroll session=\(model.selected)")
                                followsLatest = false
                            },
                            onUserReachedTop: {
                                requestOlderMessages()
                            },
                            onViewportMeasured: { content, viewport, offset, remaining, nearBottom in
                                viewportLog.record(session: model.selected, content: content,
                                                   visible: viewport, offset: offset,
                                                   remaining: remaining, nearBottom: nearBottom)
                            }
                        ) { nearBottom in
                            if nearBottom && keyboardFollowIntent != false {
                                followsLatest = true
                            }
                        }
                        .frame(height: 1)
                    }
                }
                .scrollDisabled(blocksVerticalScrolling)
                .modifier(MessageInitialScrollAnchor(
                    startsAtLatest: !model.messages.isEmpty))
                .contentShape(Rectangle())
                .onTapGesture {
                    focusController.dismiss()
                    isAttachmentPanelPresented = false
                }
                .onAppear {
                    if followsLatest && browser.isOpen && browser.ownerSessionID == model.selected {
                        bottomCorrectionRequest += 1
                    }
                    logAIHistoryEvent(
                        """
                        message-list appear session=\(model.selected) count=\(model.visibleMessages.count) \
                        first=\(model.visibleMessages.first?.displayIdentity ?? "none") \
                        last=\(model.visibleMessages.last?.displayIdentity ?? "none")
                        """
                    )
                }
                .onChange(of: browser.isPresented) { presented in
                    guard !presented, followsLatest, browser.isOpen,
                          browser.ownerSessionID == model.selected else { return }
                    // 全屏页返回后，按包含工具入口的完整消息高度重新布局。
                    // 只在跟随最新时校正底部；阅读历史中的入口不改变原来的滚动意图。
                    bottomCorrectionRequest += 1
                }
                .onChange(of: model.visibleMessages.first?.displayIdentity) { _ in
                    if let anchor = prependAnchorID {
                        prependAnchorID = nil
                        position(at: anchor, proxy: proxy, anchor: .top)
                    }
                }
                .onChange(of: model.visibleMessages.last?.displayIdentity) { latestID in
                    guard isActive, followsLatest, prependAnchorID == nil,
                          let latestID else { return }
                    // 新的用户或助手消息在当前页完成布局后再定位；
                    // 不能沿用发消息前的像素偏移，否则小视口可能停在空白区域。
                    DispatchQueue.main.async {
                        guard isActive, followsLatest,
                              model.visibleMessages.last?.displayIdentity == latestID else { return }
                        position(at: latestID, proxy: proxy, anchor: .bottom)
                        bottomCorrectionRequest += 1
                    }
                }
                .onChange(of: model.scrollRequest) { _ in
                    followsLatest = true
                    // 会话切换先拿最新一条、再补前面的历史；这里显式定位最新消息，
                    // 之后的布局修正继续跟随底部，不能继承上一个会话的阅读位置。
                    if let latestID = model.visibleMessages.last?.displayIdentity {
                        position(at: latestID, proxy: proxy, anchor: .bottom)
                    }
                    bottomCorrectionRequest += 1
                    logAIHistoryEvent("follow-latest session=\(model.selected)")
                }
                .onChange(of: model.selected) { _ in
                    prependAnchorID = nil
                    followsLatest = true
                    keyboardFollowIntent = nil
                    keyboardTransitionOpening = nil
                    viewportLog.reset()
                }
                .onChange(of: geometry.size.height) { height in
                    // 键盘和旋转会改变消息视口。保留用户主动上滑的阅读位置；
                    // 原本跟随最新消息时，在新的可见高度完成布局后重新校正底部。
                    AppDiagnosticLog.shared.record(
                        level: .info,
                        category: "assistant-viewport",
                        event: "resized",
                        fields: [
                            "height": String(format: "%.1f", height),
                            "message_count": String(model.messages.count),
                            "visible_count": String(model.visibleMessages.count),
                            "history_loaded": String(model.historyLoaded),
                            "follows_latest": String(followsLatest)
                        ]
                    )
                    guard isActive, followsLatest,
                          let latestID = model.visibleMessages.last?.displayIdentity else { return }
                    // 视口收缩或恢复后定位真实消息行；不能只按旧的像素偏移
                    // 推算底部，否则播放器与键盘改变高度时可能落进空白。
                    DispatchQueue.main.async {
                        guard isActive, followsLatest,
                              model.visibleMessages.last?.displayIdentity == latestID else { return }
                        position(at: latestID, proxy: proxy, anchor: .bottom)
                        bottomCorrectionRequest += 1
                    }
                }
                .onChange(of: isActive) { active in
                    guard active else { return }
                    followsLatest = true
                }
                .onReceive(NotificationCenter.default.publisher(
                    for: UIResponder.keyboardWillShowNotification)) { _ in
                    captureKeyboardFollowIntent(opening: true)
                }
                .onReceive(NotificationCenter.default.publisher(
                    for: UIResponder.keyboardWillHideNotification)) { _ in
                    captureKeyboardFollowIntent(opening: false)
                }
                .onReceive(NotificationCenter.default.publisher(
                    for: UIResponder.keyboardDidShowNotification)) { _ in
                    if keyboardFollowIntent == true { bottomCorrectionRequest += 1 }
                    viewportLog.finishKeyboardTransition()
                    keyboardFollowIntent = nil
                    keyboardTransitionOpening = nil
                }
                .onReceive(NotificationCenter.default.publisher(
                    for: UIResponder.keyboardDidHideNotification)) { _ in
                    if keyboardFollowIntent == true { bottomCorrectionRequest += 1 }
                    viewportLog.finishKeyboardTransition()
                    keyboardFollowIntent = nil
                    keyboardTransitionOpening = nil
                }
            }
        }
    }

    private var browserEntry: some View {
        AIBrowserTaskEntry(sessionID: model.selected, isLocal: model.selectedRemote == nil,
                           needsConfirmation: !model.permissions.isEmpty || !model.questions.isEmpty,
                           open: openBrowser)
        .id("ai-browser-task-entry-\(browser.sourceMessageID)-\(browser.sourcePartID)")
    }

    private func openBrowser() {
        focusController.dismiss()
        isAttachmentPanelPresented = false
        browser.presentPage(sessionID: model.selected)
    }

    private var hasBrowserEntryInCurrentSession: Bool {
        browser.isOpen && browser.ownerSessionID == model.selected && model.selectedRemote == nil
    }

    private func position(at id: String, proxy: ScrollViewProxy, anchor: UnitPoint) {
        logAIHistoryEvent(
            """
            scroll-request session=\(model.selected) target=\(id) \
            first=\(model.visibleMessages.first?.displayIdentity ?? "none") \
            last=\(model.visibleMessages.last?.displayIdentity ?? "none")
            """
        )
        var transaction = Transaction(animation: nil)
        transaction.disablesAnimations = true
        withTransaction(transaction) { proxy.scrollTo(id, anchor: anchor) }
    }

    private func requestOlderMessages() {
        guard prependAnchorID == nil, model.hasOlderVisibleMessages,
              let anchor = model.visibleMessages.first?.displayIdentity else { return }
        prependAnchorID = anchor
        Task {
            await model.loadOlderMessages()
            if model.visibleMessages.first?.displayIdentity == anchor { prependAnchorID = nil }
        }
    }

    private func captureKeyboardFollowIntent(opening: Bool) {
        guard isActive else { return }
        if keyboardTransitionOpening == opening { return }
        keyboardTransitionOpening = opening
        let nearBottom = viewportLog.nearBottomBeforeKeyboard(opening: opening)
        keyboardFollowIntent = nearBottom
        followsLatest = nearBottom
        logAIHistoryEvent("keyboard session=\(model.selected) opening=\(opening) nearBottomBefore=\(nearBottom)")
    }

    private var welcome: some View {
        VStack(alignment: .center, spacing: 0) {
            Image(systemName: "sparkles")
                .font(AppTypography.symbol(size: 24, weight: .medium))
                .foregroundStyle(RemoteIMStyle.blue)
                .frame(width: 48, height: 48)
                .background(RemoteIMStyle.blueSoft, in: RoundedRectangle(cornerRadius: 15))
            Text("从这里开始创作")
                .font(AppTypography.display.weight(.bold))
                .foregroundStyle(RemoteIMStyle.textPrimary)
                .padding(.top, 18)

            LazyVGrid(columns: [GridItem(.flexible(), spacing: 10),
                                GridItem(.flexible(), spacing: 10)], spacing: 10) {
                welcomeFeature("AI 问答", icon: "text.bubble")
                welcomeFeature("图片创作", icon: "photo")
                welcomeFeature("视频创作", icon: "play.rectangle")
                welcomeFeature("文件处理", icon: "doc.text")
                if aiSupportsWholeDeviceScreenCapture() {
                    welcomeFeature("整屏截屏", icon: "rectangle.on.rectangle")
                }
                welcomeFeature("浏览器操作", icon: "globe")
                welcomeFeature("会话定时任务", icon: "clock")
                welcomeFeature("系统日历", icon: "calendar")
                welcomeFeature("二维码生成", icon: "qrcode")
                welcomeFeature("当前位置", icon: "location")
                welcomeFeature("本地媒体处理", icon: "slider.horizontal.3")
                // 空态只说明能力；实际处理和训练由用户在会话中提出需求。
                welcomeFeature("人像美颜", icon: "face.smiling")
                welcomeFeature("美体塑形", icon: "figure.stand")
                welcomeFeature("个人模型训练", icon: "sparkles")
                welcomeFeature("拍照", icon: "camera")
                welcomeFeature("音乐播放", icon: "music.note")
                welcomeFeature("相册管理", icon: "photo.on.rectangle")
                welcomeFeature("我的云盘", icon: "icloud")
                welcomeFeature("MaiChat 消息", icon: "bubble.left.and.bubble.right")
                welcomeFeature("账号资料与头像", icon: "person.crop.circle")
                welcomeFeature("手机通讯录", icon: "person.2")
                welcomeFeature("拨号与发短信", icon: "phone.arrow.up.right")
                welcomeFeature("跨设备会话", icon: "iphone.and.arrow.forward")
            }
            .padding(.top, 28)
            if !model.configured {
                Text(model.settings.isLocalModel
                    ? "内置本地模型暂时不可用，请重试；若仍失败，请更新 App"
                    : "登录后可使用云端主模型，也可在“更多→主模型”中选择本地 GGUF")
                    .font(AppTypography.detail).foregroundStyle(.secondary)
                    .padding(.top, 18)
                Button("重试连接") {
                    Task {
                        if !model.settings.isLocalModel {
                            _ = try? await AICloudCredentialSync.refreshAccountModelKeysIfAvailable()
                        }
                        _ = await model.save(model.settings)
                    }
                }
                    .buttonStyle(.plain).font(AppTypography.detail.weight(.semibold))
                    .foregroundStyle(Color.white).padding(.horizontal, 16).frame(height: 38)
                    .background(Color.blue, in: RoundedRectangle(cornerRadius: 10))
                    .padding(.top, 10)
            }
        }
        .frame(maxWidth: 520)
        .frame(maxWidth: .infinity)
        .padding(.vertical, 26)
        .accessibilityIdentifier("ai-welcome")
    }

    private func welcomeFeature(_ title: String, icon: String) -> some View {
        HStack(spacing: 10) {
            Image(systemName: icon)
                .font(AppTypography.symbol(size: 17, weight: .medium))
                .foregroundStyle(RemoteIMStyle.blue)
                .frame(width: 22)
            Text(title).font(AppTypography.detail.weight(.semibold))
                .foregroundStyle(RemoteIMStyle.textPrimary)
        }
        .frame(maxWidth: .infinity, minHeight: 58)
        .background(Color(uiColor: .secondarySystemGroupedBackground),
                    in: RoundedRectangle(cornerRadius: 13))
        .overlay(RoundedRectangle(cornerRadius: 13)
            .stroke(RemoteIMStyle.border.opacity(0.6), lineWidth: 0.5))
        .accessibilityElement(children: .combine)
    }
}

private struct AIHeader: View {
    let openSessions: () -> Void
    let openActions: () -> Void
    let openVideoCircle: (() -> Void)?

    var body: some View {
        HStack(spacing: 8) {
            AIHeaderButton(systemImage: "sidebar.left", label: "对话列表", action: openSessions)
            Spacer()
            if let openVideoCircle {
                AIHeaderButton(systemImage: "play.rectangle.on.rectangle",
                               label: "视频圈", action: openVideoCircle)
                    .accessibilityIdentifier("ai-video-circle-entry")
            }
            AIHeaderButton(systemImage: "ellipsis", label: "更多", action: openActions)
        }
        .overlay { AppPageTitle("创作中心").allowsHitTesting(false) }
        .padding(.horizontal, 16).frame(height: 54)
        .foregroundStyle(.primary)
        .remoteIMBarBackground(edges: .top, opacity: RemoteIMStyle.conversationBarOpacity)
        .overlay(alignment: .bottom) { RemoteIMBarSeparator() }
    }
}

private struct AIHeaderButton: View {
    let systemImage: String
    let label: String
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: systemImage).font(AppTypography.symbol(size: 16, weight: .semibold))
                .frame(width: 34, height: 34)
                .background(Color(uiColor: .secondarySystemBackground), in: RoundedRectangle(cornerRadius: 10))
                .frame(width: 44, height: 44)
                .contentShape(Rectangle())
        }.buttonStyle(.plain).foregroundStyle(Color.primary).accessibilityLabel(label)
    }
}

private struct AIActionPanelHeightKey: PreferenceKey {
    static let defaultValue: CGFloat = 0
    static func reduce(value: inout CGFloat, nextValue: () -> CGFloat) { value = nextValue() }
}

private struct AIActionPanel: View {
    let maximumHeight: CGFloat
    let selectedModel: String
    let selectedModelID: String
    let isLocalModel: Bool
    let allowLocalModel: Bool
    let changingModelSource: Bool
    let selectedEffort: String
    let selectedPolicy: String
    let canChangeModel: Bool
    let canChangeEffort: Bool
    let canChangeSettings: Bool
    let canClear: Bool
    let selectEffort: (String) -> Void
    let selectPolicy: (String) -> Void
    let selectModel: (String) -> Void
    let clear: () -> Void
    @State private var expandedSection: Section?
    @State private var contentHeight: CGFloat = 208

    private enum Section: Equatable {
        case model
        case effort
        case policy
    }

    var body: some View {
        ScrollView(.vertical) {
            VStack(spacing: 0) {
                // 模型来源与思考强度使用同一内联展开交互，点选后直接保存。
                // 本地权重随包提供；此入口不再创建设置 sheet、文件管理或应用确认页。
                expandableAction(title: "主模型", value: changingModelSource ? "正在切换…" : selectedModel,
                                 image: "cpu", section: .model, enabled: canChangeModel)
                .accessibilityIdentifier("ai-primary-model-settings")
                if expandedSection == .model {
                    optionGroup(identifier: "ai-primary-model-menu") {
                        option("DeepSeek Flash", selected: selectedModelID == "deepseek-flash") { selectModel("deepseek-flash") }
                            .accessibilityIdentifier("ai-primary-model-deepseek-flash")
                        option("DeepSeek V4 Pro", selected: selectedModelID == "deepseek-v4-pro") { selectModel("deepseek-v4-pro") }
                            .accessibilityIdentifier("ai-primary-model-deepseek-v4-pro")
                        option("GLM-5.3", selected: selectedModelID == "glm-5.3") { selectModel("glm-5.3") }
                            .accessibilityIdentifier("ai-primary-model-glm-5.3")
                        option("GLM-5.3-Flash", selected: selectedModelID == "glm-5.3-flash") { selectModel("glm-5.3-flash") }
                            .accessibilityIdentifier("ai-primary-model-glm-5.3-flash")
                        if allowLocalModel {
                            option("本地模型", selected: isLocalModel) { selectModel("local:" + AILocalModelStore.bundledPrimaryID) }
                                .accessibilityIdentifier("ai-primary-model-local")
                        }
                    }
                    .disabled(!canChangeModel)
                }
                Divider().padding(.leading, 42)
                expandableAction(
                    title: "思考强度",
                    value: effortTitle(selectedEffort),
                    image: "sparkles",
                    section: .effort,
                    enabled: canChangeEffort
                )
                if expandedSection == .effort {
                    optionGroup(identifier: "ai-effort-menu") {
                        effortOption("简单", value: "low")
                        effortOption("标准", value: "high")
                        effortOption("深度", value: "max")
                    }
                    .disabled(!canChangeEffort)
                }
                Divider().padding(.leading, 42)
                expandableAction(
                    title: "操作权限",
                    value: policyTitle(selectedPolicy),
                    image: "shield",
                    section: .policy
                )
                if expandedSection == .policy {
                    optionGroup(identifier: "ai-policy-menu") {
                        policyOption("请求批准", value: "on-request")
                        policyOption("帮我批准", value: "unless-trusted")
                        policyOption("完全访问", value: "never")
                        Text("完全访问免除一般操作确认；付费任务仍需批准。")
                            .font(AppTypography.metadata)
                            .foregroundStyle(.secondary)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .padding(.horizontal, 14)
                            .padding(.bottom, 6)
                    }
                }
                Divider().padding(.leading, 42)
                action("清空当前对话", "trash", canClear ? Color.red : Color.secondary, clear)
                    .disabled(!canClear)
            }
            .background(GeometryReader { geometry in
                Color.clear.preference(key: AIActionPanelHeightKey.self, value: geometry.size.height)
            })
        }
            // Measure the unbounded content, then size the scroll viewport itself.
            // fixedSize on a ScrollView would let its content draw outside the panel.
            .frame(width: 264, height: min(maximumHeight, contentHeight))
            .onPreferenceChange(AIActionPanelHeightKey.self) { height in
                if height.isFinite && height > 0 { contentHeight = height }
            }
            .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 14))
            .clipShape(RoundedRectangle(cornerRadius: 14))
            .overlay(RoundedRectangle(cornerRadius: 14).stroke(Color.black.opacity(0.08)))
            .shadow(color: Color.black.opacity(0.14), radius: 20, y: 8)
            .accessibilityElement(children: .contain)
            .accessibilityIdentifier("ai-action-panel")
    }

    private func expandableAction(
        title: String,
        value: String,
        image: String,
        section: Section,
        enabled: Bool? = nil
    ) -> some View {
        Button {
            withAnimation(.easeOut(duration: 0.14)) {
                expandedSection = expandedSection == section ? nil : section
            }
        } label: {
            HStack(spacing: 12) {
                Image(systemName: image).frame(width: 18)
                VStack(alignment: .leading, spacing: 2) {
                    Text(title).font(AppTypography.detail.weight(.medium))
                    Text(value).font(AppTypography.metadata).foregroundStyle(Color.secondary)
                        .lineLimit(1)
                }
                Spacer(minLength: 6)
                Image(systemName: "chevron.right")
                    .font(AppTypography.symbol(size: 11, weight: .semibold))
                    .rotationEffect(.degrees(expandedSection == section ? 90 : 0))
            }
            .foregroundStyle(Color.primary)
            .padding(.horizontal, 14)
            .frame(height: 52)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(!(enabled ?? canChangeSettings))
        .opacity((enabled ?? canChangeSettings) ? 1 : 0.5)
        .accessibilityLabel(title)
        .accessibilityValue(value)
    }

    private func optionGroup<Content: View>(
        identifier: String,
        @ViewBuilder content: () -> Content
    ) -> some View {
        VStack(spacing: 0) { content() }
            .padding(.vertical, 4)
            .background(Color(uiColor: .secondarySystemBackground))
            .accessibilityElement(children: .contain)
            .accessibilityIdentifier(identifier)
    }

    private func effortOption(_ title: String, value: String) -> some View {
        option(title, selected: selectedEffort == value) { selectEffort(value) }
    }

    private func effortTitle(_ value: String) -> String {
        switch value {
        case "low": return "简单"
        case "max": return "深度"
        case "unavailable": return "未提供"
        default: return "标准"
        }
    }

    private func policyOption(_ title: String, value: String) -> some View {
        option(title, selected: selectedPolicy == value) { selectPolicy(value) }
    }

    private func option(_ title: String, selected: Bool, select: @escaping () -> Void) -> some View {
        Button(action: select) {
            HStack {
                Text(title).font(AppTypography.detail.weight(.medium))
                Spacer()
                if selected {
                    Image(systemName: "checkmark")
                        .font(AppTypography.symbol(size: 11, weight: .bold))
                        .foregroundStyle(Color.blue)
                }
            }
            .foregroundStyle(Color.primary)
            .padding(.horizontal, 44)
            .frame(height: 38)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }

    private func policyTitle(_ policy: String) -> String {
        switch policy {
        case "never": return "完全访问"
        case "unless-trusted": return "帮我批准"
        default: return "请求批准"
        }
    }

    private func action(_ title: String, _ image: String, _ color: Color,
                        _ handler: @escaping () -> Void) -> some View {
        Button(action: handler) {
            HStack(spacing: 12) {
                Image(systemName: image).frame(width: 18)
                Text(title).font(AppTypography.detail.weight(.medium))
                Spacer()
            }.foregroundStyle(color).padding(.horizontal, 14).frame(height: 44)
        }.buttonStyle(.plain)
    }
}
// 每个工具组以首片段标识，后续正文/工具追加不会重建较早的折叠组。
private enum AIAssistantDisplayBlock: Identifiable {
    case tools([AIPart])
    case part(AIPart)
    var id: String {
        switch self {
        case .tools(let parts): "tools:" + (parts.first?.id ?? "")
        case .part(let part): "part:" + part.id
        }
    }
}

struct AIMessageRow: View, Equatable {
    @EnvironmentObject private var appState: RemoteIMAppState
    @ObservedObject private var cloudDrive = MaiCloudDriveModel.shared
    let message: AIMessage
    let workspacePath: String
    var remoteImageOwner: AIRemoteImageOwner? = nil
    // Accepted detail receipts may advance state before the next snapshot arrives.
    // This display-only map also invalidates the equatable row/group header.
    var remoteToolStates: [String: String] = [:]
    var questionAnswerEchoes: [String: String] = [:]
    let quote: () -> Void
    var canPublishVideo = false
    // 缩略图隐藏/恢复也是行的可见状态；不能再依赖逐帧更换环境闭包偶然使 equatable 行刷新。
    var imagePreviewSourceID: UUID? = nil
    // 活动网页属于创建它的工具片段，保留同一助手消息中工具前后正文的原有顺序。
    var browserSessionID: String? = nil
    var browserPartID: String? = nil
    var browserNeedsConfirmation = false
    var openBrowser: () -> Void = {}
    @State private var pdfPreview: AIPDFPreviewItem?
    @State private var pdfPreviewError = false
    @State private var videoPreview: AIVideoPreviewItem?
    @State private var mediaSaveError: String?
    @State private var cloudUploadNotice: String?
    @State private var remoteDownloaded: [String: URL] = [:]
    @State private var remoteDownloading: Set<String> = []
    @State private var remoteDownloadError: String?
    nonisolated static func == (lhs: Self, rhs: Self) -> Bool {
        lhs.message == rhs.message && lhs.workspacePath == rhs.workspacePath &&
            lhs.remoteImageOwner == rhs.remoteImageOwner &&
            lhs.remoteToolStates == rhs.remoteToolStates &&
            lhs.questionAnswerEchoes == rhs.questionAnswerEchoes &&
            lhs.canPublishVideo == rhs.canPublishVideo &&
            lhs.imagePreviewSourceID == rhs.imagePreviewSourceID &&
            lhs.browserSessionID == rhs.browserSessionID && lhs.browserPartID == rhs.browserPartID &&
            lhs.browserNeedsConfirmation == rhs.browserNeedsConfirmation
    }
    // 仅解释已经结束、没有可显示内容的整条助手消息；不伪造答复或改动复制/模型上下文。
    // 最后一个 steering 片段也要检查完整源消息，不能把前段已有正文误报为空回复。
    private var showsEmptyCompletionNotice: Bool {
        guard message.role == "assistant", !message.active, message.completed > 0,
              message.isLastDisplayFragment, message.questionProjection != true else { return false }
        return (message.sourceParts ?? message.parts).allSatisfy { part in
            guard part.attachmentID == nil, part.remoteImage == nil, part.path == nil else { return false }
            switch part.kind {
            case "reasoning": return true
            case "text": return (part.text ?? "").trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
            // 工具、问答、媒体及未来片段的能力未知，保守保留原来的显示路径。
            default: return false
            }
        }
    }
    var body: some View {
        #if DEBUG
        let _ = AISessionDrawerDiagnostics.rowBody()
        #endif
        let inlineMedia = textMediaMentions
        return VStack(alignment: .leading, spacing: 14) {
            if message.role == "user" {
                HStack {
                    Spacer(minLength: 30)
                    VStack(alignment: .leading, spacing: 10) {
                        ForEach(message.parts.filter { $0.kind == "quote" }) { part in
                            Label(part.preview ?? "已引用消息", systemImage: "arrowshape.turn.up.left")
                                .font(AppTypography.metadata)
                                .foregroundStyle(.secondary)
                                .lineLimit(2)
                        }
                        if message.parts.contains(where: { $0.remoteImage != nil }) {
                            // Remote text/image parts keep their original order;
                            // combining all text first would move a screenshot.
                            ForEach(message.parts) { part in
                                if part.kind == "text", let text = part.text, !text.isEmpty {
                                    userTextBubble(text)
                                } else if part.kind == "image", part.remoteImage != nil {
                                    remoteImageCard(part)
                                }
                            }
                        } else {
                            if !userDisplayText.isEmpty { userTextBubble(userDisplayText) }
                            ForEach(Array(imagePaths.enumerated()), id: \.offset) { _, path in
                                AIWorkspaceImage(filePath: path) { image, sourceFrame, sourceID in
                                    openImage(path, thumbnail: image, sourceFrame: sourceFrame, sourceID: sourceID)
                                }
                            }
                            ForEach(videoPaths, id: \.self) { path in
                                AIWorkspaceVideoCard(filePath: path) {
                                    openVideo(path)
                                }
                            }
                        }
                        ForEach(inlineMedia) { mention in
                            fileMentionCard(mention)
                        }
                        ForEach(message.parts.filter { $0.attachmentID != nil }) { part in
                            remoteAttachmentCard(part)
                        }
                        if let remoteDownloadError {
                            Text(remoteDownloadError).font(AppTypography.detail).foregroundStyle(.red)
                        }
                        Button(action: quote) {
                            Label("引用", systemImage: "arrowshape.turn.up.left")
                        }
                        .buttonStyle(.plain)
                        .font(AssistantMessageFont.metadata)
                        .foregroundStyle(.secondary)
                    }
                }
            } else {
                // Each stable segment stays at its original position as streaming parts grow.
                let artifacts = mediaArtifacts
                ForEach(orderedPartGroups) { group in
                    if group.isToolGroup { toolGroup(group.parts) }
                    ForEach(group.parts) { part in
                        if let questionText = part.questionText {
                            // 问题是正常对话正文，选项只展示文字；回答使用页面原有输入栏。
                            MarkdownLikeText(questionText, retainsPreviousWhilePreparing: true,
                                             bodyFont: ChatTypography.body, assistantTypography: true)
                                .foregroundStyle(RemoteIMStyle.textPrimary)
                            if let answer = part.questionAnswer ?? questionAnswerEchoes[part.id] {
                                HStack {
                                    Spacer(minLength: 30)
                                    Text(answer)
                                        .font(ChatTypography.body)
                                        .lineSpacing(ChatTypography.lineSpacing)
                                        .foregroundStyle(RemoteIMStyle.textPrimary)
                                        .textSelection(.enabled)
                                        .padding(14)
                                        .background(Color(uiColor: .secondarySystemBackground),
                                                    in: RoundedRectangle(cornerRadius: 18))
                                }
                            }
                        } else if part.kind == "tool", part.id == browserPartID, let sessionID = browserSessionID {
                            AIBrowserTaskEntry(sessionID: sessionID, isLocal: true,
                                               needsConfirmation: browserNeedsConfirmation, open: openBrowser)
                        } else if part.kind == "image", part.remoteImage != nil {
                            remoteImageCard(part)
                        } else if part.kind == "image", part.attachmentID != nil {
                            remoteAttachmentCard(part)
                        } else if part.kind == "text", let text = part.text, !text.isEmpty {
                            if let failure = specialistFailureText(text) {
                                Label(failure, systemImage: "exclamationmark.triangle.fill")
                                    .font(ChatTypography.body)
                                    .foregroundStyle(.red)
                                    .frame(maxWidth: .infinity, alignment: .leading)
                                    .padding(12)
                                    .background(Color.red.opacity(0.08),
                                                in: RoundedRectangle(cornerRadius: 12))
                                    .textSelection(.enabled)
                            } else {
                                MarkdownLikeText(text, retainsPreviousWhilePreparing: true,
                                                 bodyFont: ChatTypography.body, assistantTypography: true)
                                    .foregroundStyle(RemoteIMStyle.textPrimary)
                            }
                        }
                    }
                    ForEach(mediaArtifacts(in: group.parts, from: artifacts)) { artifact in
                        mediaArtifactView(artifact)
                    }
                }
                ForEach(inlineMedia) { mention in
                    fileMentionCard(mention)
                }
                if showsEmptyCompletionNotice {
                    Text("本次未生成可显示的回复")
                        .font(ChatTypography.body)
                        .foregroundStyle(.secondary)
                }
                if message.active && toolParts.isEmpty && artifacts.isEmpty &&
                    !hasVisibleAnswerText {
                    AIThinkingIndicator()
                }
                if !message.active && message.isLastDisplayFragment {
                    HStack {
                        Text(
                            message.completed == 0
                                ? "已中断" : "用时 \(max(0, (message.completed - message.created) / 1000)) 秒"
                        ).font(AssistantMessageFont.metadata)
                        Button { RemoteIMClipboard.writeText(message.copyText) } label: {
                            Image(systemName: "doc.on.doc")
                        }
                            .accessibilityLabel("复制回复")
                        if message.questionProjection != true {
                            Button(action: quote) {
                                Label("引用", systemImage: "arrowshape.turn.up.left")
                            }
                            .accessibilityLabel("引用这条消息")
                        }
                    }.foregroundStyle(.secondary).font(AssistantMessageFont.metadata)
                }
            }
        }.frame(maxWidth: .infinity, alignment: .leading)
            .sheet(item: $pdfPreview) { item in
                AIPDFPreviewScreen(item: item) { pdfPreview = nil }
            }
            .fullScreenCover(item: $videoPreview) { item in
                MaiFfplayVideoScreen(path: item.path) { videoPreview = nil }
                    .ignoresSafeArea()
                    .statusBarHidden()
            }
            .alert("无法预览 PDF", isPresented: $pdfPreviewError) {
                Button("知道了", role: .cancel) {}
            } message: {
                Text("文件不存在、已移出 AI 工作区，或内容不是 PDF。")
            }
            .alert("无法保存媒体", isPresented: Binding(
                get: { mediaSaveError != nil },
                set: { if !$0 { mediaSaveError = nil } }
            )) {
                Button("知道了", role: .cancel) { mediaSaveError = nil }
            } message: {
                Text(mediaSaveError ?? "请检查相册权限与媒体文件。")
            }
    }

    @ViewBuilder private func toolGroup(_ toolParts: [AIPart]) -> some View {
        let displayTools = toolParts.map { original in
            var copy = original
            if let state = remoteToolStates[original.id] { copy.state = state }
            return copy
        }
        if let stage = seedanceStage(in: displayTools) {
            Label {
                VStack(alignment: .leading, spacing: 3) {
                    Text(stage.title).font(AppTypography.detail.weight(.semibold))
                    if !stage.detail.isEmpty {
                        Text(stage.detail).font(AppTypography.metadata)
                    }
                }
            } icon: {
                Image(systemName: stage.failed
                      ? "exclamationmark.triangle.fill" : "person.crop.rectangle")
            }
            .foregroundStyle(stage.failed ? Color.red : Color.blue)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(12)
            .background((stage.failed ? Color.red : Color.blue).opacity(0.08),
                        in: RoundedRectangle(cornerRadius: 12))
        }
        AIExpandableBlock(title: toolDisclosureTitle(for: displayTools),
                          identifier: "agent-tool-group-" + (toolParts.first?.id ?? "")) {
            VStack(alignment: .leading, spacing: 10) {
                ForEach(toolParts) { tool in
                    VStack(alignment: .leading, spacing: 6) {
                        Text("\(tool.tool ?? "工具") · \(toolStatus(remoteToolStates[tool.id] ?? tool.state))")
                            .font(AssistantMessageFont.detail.weight(.semibold))
                        if let owner = remoteImageOwner, tool.usesRemoteToolDetails {
                            if toolParts.count == 1 {
                                AIRemoteToolDetailPane(owner: owner, messageID: message.id, part: tool)
                            } else {
                                // Opening a large group does not read every tool. Only the
                                // selected item's disclosure mounts its three first pages.
                                AIExpandableBlock(title: "参数、输出和错误",
                                                  identifier: "agent-tool-detail-" + tool.id) {
                                    AIRemoteToolDetailPane(owner: owner, messageID: message.id, part: tool)
                                }
                            }
                        } else {
                            Text(tool.input ?? "")
                                .font(AssistantMessageFont.detailMonospaced)
                            if let output = tool.output, !output.isEmpty {
                                Text(output).font(AssistantMessageFont.detailMonospaced)
                            }
                            if let error = tool.error, !error.isEmpty {
                                Text(error).font(AssistantMessageFont.detail).foregroundStyle(.red)
                            }
                        }
                    }
                    if tool.id != toolParts.last?.id { Divider() }
                }
            }
            .textSelection(.enabled).padding(10)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Color(uiColor: .secondarySystemBackground),
                        in: RoundedRectangle(cornerRadius: 8))
        }
        ForEach(toolParts) { tool in
            if let path = pdfArtifactPath(for: tool) {
                AIPDFArtifactButton(path: path) {
                    if let url = validatedPDFURL(path: path) {
                        pdfPreview = AIPDFPreviewItem(url: url)
                    } else {
                        pdfPreviewError = true
                    }
                }
            }
            if let url = diagnosticArtifactURL(for: tool) {
                ShareLink(item: url) {
                    Label("分享排障日志 · \(url.lastPathComponent)",
                          systemImage: "doc.text")
                        .font(AppTypography.detail.weight(.medium))
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(12)
                        .background(Color.blue.opacity(0.07),
                                    in: RoundedRectangle(cornerRadius: 10))
                }
                .accessibilityIdentifier("agentDiagnosticShareButton")
            }
        }
    }

    @ViewBuilder private func mediaArtifactView(_ artifact: AIAgentMediaArtifact) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            if artifact.type == "image" {
                AIWorkspaceImage(filePath: artifact.filePath) { image, sourceFrame, sourceID in
                    openImage(artifact.filePath, thumbnail: image,
                              sourceFrame: sourceFrame, sourceID: sourceID)
                }
            } else if artifact.type == "video" {
                AIWorkspaceVideoCard(filePath: artifact.filePath) {
                    openVideo(artifact.filePath)
                }
            } else {
                AIWorkspaceAudioCard(filePath: artifact.filePath)
            }
            if !artifact.caption.isEmpty {
                Text(artifact.caption)
                    .font(ChatTypography.body)
                    .foregroundStyle(RemoteIMStyle.textPrimary)
            }
            if canPublishVideo, let source = publishSource(for: artifact) {
                Button {
                    MaiVideoCircleModel.shared.presentation = .publish(source)
                } label: {
                    Label("发布到视频圈", systemImage: "square.and.arrow.up")
                        .font(AppTypography.detail.weight(.semibold))
                }
                .buttonStyle(.plain)
                .foregroundStyle(RemoteIMStyle.blue)
                .accessibilityIdentifier("ai-video-circle-publish-\(artifact.id)")
            }
            if cloudDrive.availability == .available,
               artifact.type == "video",
               FileManager.default.fileExists(atPath: artifact.filePath) {
                Button {
                    let account = appState.maiChatAccountID
                    guard !account.isEmpty else {
                        cloudUploadNotice = "请先登录 MaiChat"
                        return
                    }
                    let drive = MaiCloudDriveModel.shared
                    drive.activate(accountID: account)
                    Task {
                        await drive.addAgentVideo(
                            URL(fileURLWithPath: artifact.filePath),
                            expectedAccountID: account)
                        cloudUploadNotice = drive.localTasks.contains {
                            $0.sourcePath == artifact.filePath
                        } ? "已加入云盘上传" : drive.notice
                    }
                } label: {
                    Label("保存到云盘", systemImage: "icloud.and.arrow.up")
                        .font(AppTypography.detail.weight(.semibold))
                }
                .buttonStyle(.plain)
                .foregroundStyle(RemoteIMStyle.blue)
                .accessibilityIdentifier("ai-cloud-drive-save-\(artifact.id)")
            }
            if let cloudUploadNotice {
                Text(cloudUploadNotice).font(AppTypography.metadata)
                    .foregroundStyle(RemoteIMStyle.textSecondary)
            }
        }
        .contextMenu {
            Button(action: quote) {
                Label("引用这条消息", systemImage: "arrowshape.turn.up.left")
            }
            if artifact.type != "audio" {
                Button {
                    Task { await saveMediaArtifact(artifact) }
                } label: {
                    Label("保存到相册", systemImage: "square.and.arrow.down")
                }
            }
            ShareLink(item: URL(fileURLWithPath: artifact.filePath)) {
                Label("转发或分享", systemImage: "square.and.arrow.up")
            }
        }
    }

    private func userTextBubble(_ text: String) -> some View {
        Text(text)
            .font(ChatTypography.body)
            .lineSpacing(ChatTypography.lineSpacing)
            .foregroundStyle(RemoteIMStyle.textPrimary)
            .textSelection(.enabled)
            .padding(14)
            .background(Color(uiColor: .secondarySystemBackground), in: RoundedRectangle(cornerRadius: 18))
    }

    private func remoteImageCard(_ part: AIPart) -> some View {
        AIRemoteSessionImage(owner: remoteImageOwner, messageID: message.id, part: part) { path, image, frame, source in
            guard AIAssistantModel.shared.remoteImageOwner == remoteImageOwner else { return }
            openImage(path, thumbnail: image, sourceFrame: frame, sourceID: source)
        }
    }

    @ViewBuilder private func remoteAttachmentCard(_ part: AIPart) -> some View {
        let title = part.name ?? part.text ?? "附件"
        let kindLabel = part.kind == "image" ? "图片" :
            part.kind == "video" ? "视频" :
            part.kind == "audio" ? "音频" : "文件"
        if let file = remoteDownloaded[part.id] {
            if part.kind == "image" {
                AIWorkspaceImage(filePath: file.path) { image, frame, sourceID in
                    openImage(file.path, thumbnail: image, sourceFrame: frame, sourceID: sourceID)
                }
            } else if part.kind == "video" {
                AIWorkspaceVideoCard(filePath: file.path) { openVideo(file.path) }
            } else if part.kind == "audio" {
                AIWorkspaceAudioCard(filePath: file.path)
            } else {
                ShareLink(item: file) {
                    Label("打开或保存 · \(title)", systemImage: "doc")
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
        } else {
            Button {
                guard !remoteDownloading.contains(part.id) else { return }
                remoteDownloading.insert(part.id)
                remoteDownloadError = nil
                Task {
                    do {
                        let result = try await AIAssistantModel.shared
                            .downloadRemoteAttachment(part)
                        remoteDownloaded[part.id] = result.fileURL
                    } catch { remoteDownloadError = error.localizedDescription }
                    remoteDownloading.remove(part.id)
                }
            } label: {
                Label(remoteDownloading.contains(part.id)
                      ? "正在下载 · \(title)" : "下载\(kindLabel) · \(title)",
                      systemImage: "arrow.down.circle")
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .disabled(remoteDownloading.contains(part.id))
        }
    }
    private func specialistFailureText(_ text: String) -> String? {
        if text.hasPrefix("Video task failed. ") {
            return "视频任务失败：" + String(text.dropFirst("Video task failed. ".count))
        }
        if text.hasPrefix("Image task failed. ") {
            return "图片任务失败：" + String(text.dropFirst("Image task failed. ".count))
        }
        if text.hasPrefix("Video task status unavailable. ") {
            return "视频状态查询失败，云端结果未确认：" +
                String(text.dropFirst("Video task status unavailable. ".count))
        }
        if text.hasPrefix("Image task status unavailable. ") {
            return "图片状态查询失败，云端结果未确认：" +
                String(text.dropFirst("Image task status unavailable. ".count))
        }
        if text.hasPrefix("Video output unavailable. ") {
            return "视频产物获取失败：" + String(text.dropFirst("Video output unavailable. ".count))
        }
        if text.hasPrefix("Image output unavailable. ") {
            return "图片产物获取失败：" + String(text.dropFirst("Image output unavailable. ".count))
        }
        return nil
    }
    private struct PartGroup: Identifiable {
        let id: String
        let isToolGroup: Bool
        let canJoinNextTool: Bool
        var parts: [AIPart]
    }
    // Adjacent tools share a disclosure; text and media end the run. The first
    // part's identity keeps a disclosure expanded while later stream parts arrive.
    private var orderedPartGroups: [PartGroup] {
        var groups: [PartGroup] = []
        for part in message.parts {
            if part.kind == "reasoning" { continue }
            let hasArtifact = mediaArtifact(for: part) != nil
            let isTool = part.kind == "tool" && part.questionText == nil &&
                !(part.tool == "agent_send_media" && hasArtifact)
            // A capture keeps its receipt, but its visible card ends the run.
            let canJoinNextTool = isTool && !hasArtifact
            if canJoinNextTool, let last = groups.indices.last, groups[last].canJoinNextTool {
                groups[last].parts.append(part)
            } else {
                groups.append(PartGroup(id: part.id, isToolGroup: isTool,
                                        canJoinNextTool: canJoinNextTool, parts: [part]))
            }
        }
        return groups
    }
    // Keep the existing canonical media deduplication, while displaying each card
    // next to its owning tool instead of moving all cards below the final text.
    private func mediaArtifacts(in parts: [AIPart], from artifacts: [AIAgentMediaArtifact]) -> [AIAgentMediaArtifact] {
        let ids = Set(parts.map(\.id))
        return artifacts.filter { ids.contains($0.id) }
    }
    private var hasVisibleAnswerText: Bool {
        message.parts.contains { part in
            (part.kind == "text" && !(part.text ?? "").isEmpty) || part.questionText != nil
        }
    }
    private func isDisclosureTool(_ part: AIPart) -> Bool {
        part.kind == "tool" && part.questionText == nil
            && !(part.tool == "agent_send_media" && mediaArtifact(for: part) != nil)
    }
    private var toolParts: [AIPart] {
        // Captures keep their execution receipt in the disclosure even when an
        // image card is present. agent_send_media retains its existing card-only UI.
        message.parts.filter(isDisclosureTool)
    }
    private var mediaArtifacts: [AIAgentMediaArtifact] {
        var result: [AIAgentMediaArtifact] = []
        var imageIndexes: [String: Int] = [:]
        for part in message.sourceParts ?? message.parts {
            guard let artifact = mediaArtifact(for: part) else { continue }
            if artifact.type == "image" {
                // A model may explicitly deliver the photo after capture. Keep
                // one canonical image card and prefer that delivery's caption.
                if let index = imageIndexes[artifact.filePath] {
                    if part.tool == "agent_send_media" { result[index] = artifact }
                    continue
                }
                imageIndexes[artifact.filePath] = result.count
            }
            result.append(artifact)
        }
        // A later delivery owns the canonical card. Keep the row and its media
        // playlist in that owner's original part order after capture deduplication.
        let partOrder = Dictionary(uniqueKeysWithValues: (message.sourceParts ?? message.parts).enumerated().map {
            ($0.element.id, $0.offset)
        })
        return result.sorted {
            (partOrder[$0.id] ?? Int.max) < (partOrder[$1.id] ?? Int.max)
        }
    }
    private var textMediaMentions: [AIAssistantMediaMention] {
        // 普通消息的文字附件原本只在整条正文之后展示一次；分片不能重复生成相同的媒体卡。
        guard message.isLastDisplayFragment else { return [] }
        guard message.role != "assistant" || (!message.active && message.completed > 0) else {
            return []
        }
        let alreadyShown = Set(mediaArtifacts.map(\.filePath) + videoPaths)
        return AIAssistantMediaMentionPolicy.cards(in: message.sourceParts == nil ? message.text : message.copyText,
                                                   workspacePath: workspacePath)
            .filter { !alreadyShown.contains($0.url.path) }
    }
    private var displayedMediaItems: [MaiMediaItem] {
        if let owner = remoteImageOwner {
            return message.parts.compactMap { part in
                guard let path = AIAssistantModel.shared.cachedRemoteImagePath(
                    owner: owner, messageID: message.id, part: part) else { return nil }
                return MaiMediaItem(kind: .image, path: path)
            }
        }
        let mentions = textMediaMentions.filter { $0.kind == .video }.map {
            MaiMediaItem(kind: .video, path: $0.url.path)
        }
        if message.role == "user" {
            return imagePaths.map { MaiMediaItem(kind: .image, path: $0) }
                + videoPaths.map { MaiMediaItem(kind: .video, path: $0) }
                + mentions
        }
        let artifacts: [MaiMediaItem] = mediaArtifacts.compactMap { artifact in
            let kind: MaiMediaKind
            if artifact.type == "image" { kind = .image }
            else if artifact.type == "video" { kind = .video }
            else { return nil }
            return MaiMediaItem(kind: kind, path: artifact.filePath)
        }
        return artifacts + mentions
    }
    private func fullMediaItems(model: AIAssistantModel,
                                session: String) async -> [MaiMediaItem] {
        let history = await model.messagesForMediaPlaylist(in: session)
        var items: [MaiMediaItem] = []
        for (index, entry) in history.enumerated() {
            items += Self(message: entry, workspacePath: workspacePath,
                          remoteImageOwner: remoteImageOwner,
                          quote: {}).displayedMediaItems
            if index.isMultiple(of: 40) { await Task.yield() }
        }
        return items
    }

    private func openImage(_ path: String, thumbnail: UIImage, sourceFrame: CGRect, sourceID: UUID) {
        let model = AIAssistantModel.shared
        let session = model.selected
        let items = model.messages.flatMap {
            Self(message: $0, workspacePath: workspacePath,
                 remoteImageOwner: remoteImageOwner, quote: {}).displayedMediaItems
        }
        let sequence = MaiMediaSequence(
            selected: MaiMediaItem(kind: .image, path: path), items: items)
        let initial = AIImagePreview(filePath: path, imageSize: thumbnail.size,
                                     sourceFrame: sourceFrame, sourceID: sourceID, sequence: sequence,
                                     thumbnail: thumbnail)
        model.previewImage = initial
        Task {
            let allItems = await fullMediaItems(model: model, session: session)
            guard !allItems.isEmpty, model.selected == session,
                  let current = model.previewImage,
                  current.sequence?.groupID == sequence.groupID else { return }
            model.previewImage = AIImagePreview(
                filePath: current.filePath, imageSize: current.imageSize,
                sourceFrame: current.sourceFrame,
                sourceID: current.sourceID,
                sequence: current.sequence?.updatingItems(allItems), thumbnail: current.thumbnail,
                id: current.id)
        }
    }

    private func openVideo(_ path: String) {
        // 与 IM 和工具播放使用同一个全屏模态；关闭后回到原创作会话和输入草稿。
        if let appState = AIMobileHostToolProvider.shared.appState {
            let model = AIAssistantModel.shared
            let session = model.selected
            let items = model.messages.flatMap {
                Self(message: $0, workspacePath: workspacePath,
                     remoteImageOwner: remoteImageOwner, quote: {}).displayedMediaItems
            }
            let sequence = MaiMediaSequence(
                selected: MaiMediaItem(kind: .video, path: path), items: items)
            let initial = MaiFfplayPresentation(sequence: sequence)
            appState.agentVideoPresentation = initial
            Task {
                let allItems = await fullMediaItems(model: model, session: session)
                guard !allItems.isEmpty, model.selected == session,
                      appState.agentVideoPresentation?.groupID == initial.groupID else { return }
                guard model.selected == session,
                      let current = appState.agentVideoPresentation,
                      current.groupID == initial.groupID else { return }
                appState.agentVideoPresentation = current.updatingMediaItems(allItems)
            }
        } else {
            videoPreview = AIVideoPreviewItem(path: path)
        }
    }

    @ViewBuilder private func fileMentionCard(_ mention: AIAssistantMediaMention) -> some View {
        switch mention.kind {
        case .video:
            // 正文提到的本地视频和附件使用同一张缩略图卡片，避免只显示文件名。
            AIWorkspaceVideoCard(filePath: mention.url.path,
                                 identifier: "agent-text-video-bubble") {
                openVideo(mention.url.path)
            }
        case .audio:
            AIWorkspaceAudioCard(filePath: mention.url.path)
        }
    }
    private func mediaArtifact(for part: AIPart) -> AIAgentMediaArtifact? {
        if part.kind == "tool",
           let fileURL = AIAssistantPhotoArtifactPolicy.photoURL(
            tool: part.tool, state: part.state, output: part.output, workspacePath: workspacePath) {
            // The camera result already exists on disk. Rendering this card does
            // not attach image bytes to the model or automatically open fullscreen.
            return AIAgentMediaArtifact(id: part.id, filePath: fileURL.path, type: "image",
                caption: "", sourceTaskID: nil, sourceTool: nil, outputJSON: part.output ?? "")
        }
        guard part.kind == "tool", part.tool == "agent_send_media",
              part.state == "completed",
              let output = part.output?.data(using: .utf8),
              let object = (try? JSONSerialization.jsonObject(with: output)) as? [String: Any],
              object["delivery"] as? String == "current_ai_session",
              let relative = object["path"] as? String,
              let type = object["type"] as? String,
              ["image", "video", "audio"].contains(type),
              let mime = object["mime_type"] as? String,
              mime.hasPrefix(type + "/"),
              let fileURL = AIAssistantPathPolicy.resolve(relative,
                                                           workspacePath: workspacePath)
        else { return nil }
        let provenance = type == "video"
            ? MaiVideoCircleClient.inspectCompletedCard(outputJSON: part.output ?? "") : nil
        return AIAgentMediaArtifact(
            id: part.id, filePath: fileURL.path, type: type,
            caption: object["caption"] as? String ?? "",
            sourceTaskID: provenance?.sourceTaskId,
            sourceTool: provenance?.sourceTool,
            outputJSON: part.output ?? "")
    }

    private func publishSource(for artifact: AIAgentMediaArtifact)
        -> MaiVideoCirclePublishSource? {
        guard artifact.type == "video",
              let taskID = artifact.sourceTaskID, !taskID.isEmpty,
              let tool = artifact.sourceTool,
              FileManager.default.fileExists(atPath: artifact.filePath)
        else { return nil }
        return MaiVideoCirclePublishSource(
            id: artifact.id, fileURL: URL(fileURLWithPath: artifact.filePath),
            outputJSON: artifact.outputJSON, caption: artifact.caption,
            sourceTaskID: taskID, sourceTool: tool)
    }
    private func saveMediaArtifact(_ artifact: AIAgentMediaArtifact) async {
        let saveRequestedAt = Date()
        let url = URL(fileURLWithPath: artifact.filePath)
        do {
            guard await MaiPhotoLibraryMediaWriter.requestAddAccess() else {
                mediaSaveError = "请允许添加作品到相册"
                return
            }
            if artifact.type == "image" {
                try await MaiPhotoLibraryMediaWriter.saveImage(at: url, createdAt: saveRequestedAt)
            } else {
                try await MaiPhotoLibraryMediaWriter.saveVideo(at: url, createdAt: saveRequestedAt)
            }
        } catch {
            mediaSaveError = error.localizedDescription
        }
    }
    private func toolSummary(for tools: [AIPart]) -> String {
        var counts: [(name: String, count: Int)] = []
        for part in tools {
            let name = part.tool ?? "工具"
            if let index = counts.firstIndex(where: { $0.name == name }) {
                counts[index].count += 1
            } else {
                counts.append((name, 1))
            }
        }
        return counts.map { "\($0.name) ×\($0.count)" }.joined(separator: " · ")
    }
    private func toolDisclosureTitle(for tools: [AIPart]) -> String {
        let summary = "工具调用 \(tools.count) 次，\(toolGroupStatus(for: tools))"
        let names = toolSummary(for: tools)
        return names.isEmpty ? summary : "\(summary) · \(names)"
    }
    private func toolGroupStatus(for tools: [AIPart]) -> String {
        let states = tools.map(\.state)
        if states.contains("running") { return "运行中" }
        if states.contains("pending") { return "等待授权" }
        if states.contains("error") { return "有失败" }
        if states.allSatisfy({ $0 == "canceled" }) { return "已取消" }
        if states.contains("canceled") { return "部分取消" }
        return "已完成"
    }
    private func seedanceStage(in toolParts: [AIPart]) -> (title: String, detail: String, failed: Bool)? {
        guard let part = toolParts.last(where: { $0.tool == "seedance_video" }),
              let inputText = part.input, let inputData = inputText.data(using: .utf8),
              let input = try? JSONSerialization.jsonObject(with: inputData) as? [String: Any]
        else { return nil }
        let action = input["action"] as? String ?? ""
        if part.state == "error" {
            return ("人物替换 · 当前阶段失败", part.error ?? "请查看工具详情后重试当前阶段", true)
        }
        var output: [String: Any] = [:]
        if let outputText = part.output, let data = outputText.data(using: .utf8) {
            output = (try? JSONSerialization.jsonObject(with: data) as? [String: Any]) ?? [:]
        }
        if action == "prepare" {
            if part.state == "running" {
                return ("人物替换 · 正在准备素材", "上传 OSS、登记 Ark 并等待 Active", false)
            }
            if output["status"] as? String == "processing" {
                let count = (output["pending_asset_ids"] as? [String])?.count ?? 0
                return ("人物替换 · 素材处理中", count > 0 ? "还有 \(count) 个素材等待 Active" :
                        "等待 Ark 完成素材预处理", false)
            }
            if output["status"] as? String == "ready" {
                return ("人物替换 · 素材已 Active", "下一步仅提交资产 ID，并等待付费确认", false)
            }
        }
        if action == "delegate" || action == "revise" {
            if part.state == "pending" {
                return ("人物替换 · 等待付费确认", "确认后才会提交 Seedance 生成任务", false)
            }
            let status = output["status"] as? String ?? ""
            if status == "queued" { return ("人物替换 · 云端排队中", "已保存任务 ID，不会重复提交", false) }
            if status == "running" { return ("人物替换 · 云端生成中", "生成完成后会自动下载", false) }
            if status == "succeeded" { return ("人物替换 · 已完成", "成片已下载到工作区", false) }
            return ("人物替换 · 正在提交", "使用已 Active 的资产 ID", false)
        }
        if action == "continue" {
            let status = output["status"] as? String ?? ""
            if status == "queued" { return ("人物替换 · 云端排队中", "正在等待供应商调度", false) }
            if status == "running" { return ("人物替换 · 云端生成中", "正在生成视频", false) }
            if status == "succeeded" { return ("人物替换 · 已完成", "成片已下载到工作区", false) }
        }
        return nil
    }
    private func pdfArtifactPath(for part: AIPart) -> String? {
        guard part.tool == "generate_pdf", part.state == "completed",
              let output = part.output else { return nil }
        if let data = output.data(using: .utf8),
           let object = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
           object["mime_type"] as? String == "application/pdf",
           let path = object["path"] as? String,
           path.lowercased().hasSuffix(".pdf") { return path }
        if output.hasPrefix("Created "), output.hasSuffix(" bytes)."),
           let end = output.range(of: " (", options: .backwards)?.lowerBound {
            let start = output.index(output.startIndex, offsetBy: 8)
            if end > start {
                let path = String(output[start..<end])
                if path.lowercased().hasSuffix(".pdf") { return path }
            }
        }
        return nil
    }
    private func diagnosticArtifactURL(for part: AIPart) -> URL? {
        guard part.tool == "mobile_export_diagnostics", part.state == "completed",
              let output = part.output, let data = output.data(using: .utf8),
              let artifact = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
              artifact["mime_type"] as? String == "text/plain",
              let name = artifact["path"] as? String,
              name == (name as NSString).lastPathComponent,
              name.hasPrefix("maichat-diagnostics-"),
              name.lowercased().hasSuffix(".txt"),
              let path = AIAssistantPathPolicy.resolve(name, workspacePath: workspacePath),
              let size = (try? path.resourceValues(forKeys: [.fileSizeKey]))?.fileSize,
              size > 0, size <= 20 * 1024 * 1024 else { return nil }
        return path
    }
    private func validatedPDFURL(path: String) -> URL? {
        guard let candidate = AIAssistantPathPolicy.resolve(path, workspacePath: workspacePath),
              candidate.pathExtension.lowercased() == "pdf",
              let attributes = try? FileManager.default.attributesOfItem(atPath: candidate.path),
              let size = attributes[.size] as? NSNumber, size.int64Value >= 8,
              size.int64Value <= 100 * 1024 * 1024,
              let handle = try? FileHandle(forReadingFrom: candidate) else { return nil }
        defer { try? handle.close() }
        guard let header = try? handle.read(upToCount: 5) else { return nil }
        return header == Data("%PDF-".utf8) ? candidate : nil
    }
    private var imagePaths: [String] {
        guard !workspacePath.isEmpty else { return [] }
        let root = URL(fileURLWithPath: workspacePath, isDirectory: true).standardizedFileURL
        let rootPrefix = root.path.hasSuffix("/") ? root.path : root.path + "/"
        let stored = message.parts.compactMap { part -> String? in
            guard part.kind == "image", part.mimeType?.hasPrefix("image/") == true else {
                return nil
            }
            return part.path
        }
        let prefixes = ["图片附件（工作区相对路径）：", "请查看文件："]
        let legacy = message.text.split(whereSeparator: \.isNewline).compactMap { line -> String? in
            let value = String(line).trimmingCharacters(in: .whitespacesAndNewlines)
            guard let prefix = prefixes.first(where: value.hasPrefix) else { return nil }
            return String(value.dropFirst(prefix.count))
                .trimmingCharacters(in: .whitespacesAndNewlines)
        }
        var seen = Set<String>()
        return (stored + legacy).compactMap { path in
            guard !(path as NSString).isAbsolutePath else { return nil }
            let candidate = root.appendingPathComponent(path).standardizedFileURL
            guard candidate.path.hasPrefix(rootPrefix), Self.isImage(candidate),
                  seen.insert(candidate.path).inserted
            else { return nil }
            return candidate.path
        }
    }
    private var userDisplayText: String {
        guard !imagePaths.isEmpty || !videoPaths.isEmpty ||
              message.parts.contains(where: { $0.kind == "image" || $0.kind == "video" }) else {
            return message.text
        }
        let automatic: Set<String> = ["请查看这张图片。", "请查看这些图片。",
                                      "请查看这个视频。", "请查看这些视频。", "请查看这些媒体。"]
        var lines = message.text.components(separatedBy: "\n")
        while let last = lines.last,
              automatic.contains(last.trimmingCharacters(in: .whitespacesAndNewlines)) {
            lines.removeLast()
        }
        return lines.joined(separator: "\n").trimmingCharacters(in: .whitespacesAndNewlines)
    }
    private var videoPaths: [String] {
        guard !workspacePath.isEmpty else { return [] }
        let root = URL(fileURLWithPath: workspacePath, isDirectory: true)
            .standardizedFileURL.resolvingSymlinksInPath()
        let prefix = root.path.hasSuffix("/") ? root.path : root.path + "/"
        var seen = Set<String>()
        return message.parts.compactMap { part in
            guard part.kind == "video", part.mimeType?.hasPrefix("video/") == true,
                  let relative = part.path, !(relative as NSString).isAbsolutePath else {
                return nil
            }
            let file = root.appendingPathComponent(relative)
                .standardizedFileURL.resolvingSymlinksInPath()
            guard file.path.hasPrefix(prefix), seen.insert(file.path).inserted else { return nil }
            return file.path
        }
    }
    private static func isImage(_ url: URL) -> Bool {
        guard let type = UTType(filenameExtension: url.pathExtension) else { return false }
        return type.conforms(to: .image)
    }
    private func toolStatus(_ state: String?) -> String {
        switch state {
        case "completed": return "已完成";
        case "error": return "失败";
        case "pending": return "等待授权";
        default: return "正在运行"
        }
    }
}

private struct AIAgentMediaArtifact: Identifiable {
    let id: String
    let filePath: String
    let type: String
    let caption: String
    let sourceTaskID: String?
    let sourceTool: String?
    let outputJSON: String
}

private struct AIPDFPreviewItem: Identifiable {
    let url: URL
    var id: String { url.path }
}

private struct AIVideoPreviewItem: Identifiable {
    let path: String
    var id: String { path }
}

private struct AIPDFPreviewScreen: View {
    let item: AIPDFPreviewItem
    let close: () -> Void

    var body: some View {
        NavigationStack {
            RemoteIMQuickLookPreview(filePath: item.url.path)
                .appNavigationTitle(item.url.lastPathComponent)
                .navigationBarTitleDisplayMode(.inline)
                .toolbar {
                    ToolbarItem(placement: .topBarLeading) {
                        ShareLink(item: item.url) { Image(systemName: "square.and.arrow.up") }
                    }
                    ToolbarItem(placement: .topBarTrailing) {
                        Button("关闭", action: close)
                    }
                }
        }
    }
}

private struct AIPDFArtifactButton: View {
    let path: String
    let open: () -> Void

    var body: some View {
        Button(action: open) {
            Label("预览 PDF · \((path as NSString).lastPathComponent)",
                  systemImage: "doc.richtext")
                .font(AppTypography.detail.weight(.medium))
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(12)
                .background(Color.blue.opacity(0.07), in: RoundedRectangle(cornerRadius: 10))
        }
        .buttonStyle(.plain)
        .accessibilityIdentifier("agentPdfPreviewButton")
    }
}

/// A stable image part owns one normal bubble. Pending, failure and expiry use
/// the same position; retry reads the same OSS object and never asks its owner
/// to upload again. SwiftUI cancellation reaches the shared native downloader.
private struct AIRemoteSessionImage: View {
    let owner: AIRemoteImageOwner?
    let messageID: String
    let part: AIPart
    let open: (String, UIImage, CGRect, UUID) -> Void

    private struct LoadIdentity: Equatable {
        let owner: AIRemoteImageOwner?
        let messageID: String
        let part: AIPart
        let retry: Int
    }
    @State private var loadedIdentity: LoadIdentity?
    @State private var checkedIdentity: LoadIdentity?
    @State private var file: URL?
    @State private var failure: String?
    @State private var expired = false
    @State private var retryable = false
    @State private var retry = 0

    var body: some View {
        let identity = LoadIdentity(owner: owner, messageID: messageID, part: part, retry: retry)
        Group {
            if loadedIdentity == identity, let file {
                AIWorkspaceImage(filePath: file.path) { image, frame, source in
                    open(file.path, image, frame, source)
                }
            } else {
                TimelineView(.periodic(from: .now, by: 30)) { _ in
                    VStack(spacing: 8) {
                        if expired || (checkedIdentity == identity && part.remoteImage?.isExpired == true) {
                            Label("图片已过期", systemImage: "photo")
                        } else if part.remoteImage?.state == "failed" {
                            Label("图片同步失败", systemImage: "exclamationmark.triangle")
                        } else if let failure {
                            Label(failure, systemImage: "photo")
                            if retryable {
                                Button("重新加载图片") { retry &+= 1 }
                            }
                        } else {
                            ProgressView()
                            Text(part.remoteImage?.state == "pending" ? "图片正在同步" : "图片正在加载")
                        }
                    }
                    .font(AppTypography.detail)
                    .foregroundStyle(.secondary)
                    .frame(width: 220, height: 146)
                    .background(Color(uiColor: .secondarySystemBackground), in: RoundedRectangle(cornerRadius: 8))
                    .accessibilityIdentifier("agent-remote-image-state-" + part.id)
                }
            }
        }
        .task(id: identity) {
            file = nil; failure = nil; expired = false; retryable = false
            checkedIdentity = nil
            defer { if !Task.isCancelled { checkedIdentity = identity } }
            guard let owner else { failure = "图片暂不可用"; return }
            do {
                let downloaded = try await AIAssistantModel.shared.loadRemoteImage(
                    owner: owner, messageID: messageID, part: part)
                try Task.checkCancellation()
                file = downloaded
                loadedIdentity = identity
            } catch is CancellationError {
                // A disappearing/replaced part must not publish an old result.
            } catch let error as AIRemoteImageError {
                guard !Task.isCancelled else { return }
                if case .expired = error { expired = true }
                else if case .pending = error { /* Await the same part's ready projection. */ }
                else { failure = error.localizedDescription }
            } catch {
                guard !Task.isCancelled else { return }
                failure = "图片加载失败"; retryable = true
            }
        }
    }
}

private struct AIWorkspaceImage: View {
    let filePath: String
    let openPreview: (UIImage, CGRect, UUID) -> Void
    @State private var decodedSize: CGSize?
    @State private var decodedImage: UIImage?
    @State private var decodedImagePath: String?
    @State private var sourceID = UUID()
    @Environment(\.aiImagePreviewSourceID) private var previewSourceID
    @Environment(\.aiConversationActionsAllowed) private var conversationActionsAllowed

    private var previewSize: CGSize {
        ImageBubbleSize.fitted(decodedSize ?? CGSize(width: 220, height: 180),
                               maximum: ImageBubbleSize.assistantMaximum)
    }

    var body: some View {
        GeometryReader { geometry in
            Button {
                // 侧滑从图片内开始时，已按下的 Button 仍可能在 touch-up 回调；再次读实时互锁。
                guard conversationActionsAllowed(), decodedImagePath == filePath,
                      let decodedImage else { return }
                let sourceFrame = geometry.frame(in: .global)
                guard sourceFrame.width > 0, sourceFrame.height > 0 else { return }
                openPreview(decodedImage, sourceFrame, sourceID)
            } label: {
                RemoteIMAsyncImage(filePath: filePath,
                                   maximumPointSize: previewSize) { image in
                    Image(uiImage: image).resizable().scaledToFit()
                        .task(id: ObjectIdentifier(image)) {
                            // 与当前可见像素绑定，不只按尺寸更新：相同尺寸的新图片也要替换来源。
                            decodedImage = image
                            decodedImagePath = filePath
                            if let pixels = image.cgImage {
                                decodedSize = CGSize(width: pixels.width, height: pixels.height)
                            }
                        }
                        .accessibilityLabel("创作中心图片附件")
                } placeholder: { failed in
                    if failed {
                        Label("图片无法显示", systemImage: "photo")
                            .foregroundStyle(Color.secondary)
                            .frame(width: previewSize.width, height: previewSize.height)
                    } else {
                        Color.clear.frame(width: previewSize.width, height: previewSize.height)
                    }
                }
                .frame(width: previewSize.width, height: previewSize.height)
                .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
            }
            .buttonStyle(.plain)
            .accessibilityLabel("放大图片")
            .accessibilityIdentifier("agent-image-bubble")
            // 保留原布局和位置测量，只隐藏源缩略图，避免动画中重影。
            .opacity(previewSourceID == sourceID ? 0 : 1)
            .preference(key: AIImageSourceFramesKey.self,
                         value: [sourceID: geometry.frame(in: .global)])
        }
        .frame(width: previewSize.width, height: previewSize.height)
    }
}

private struct AIWorkspaceVideoCard: View {
    let filePath: String
    let identifier: String
    let open: () -> Void
    @Environment(\.aiConversationActionsAllowed) private var conversationActionsAllowed
    @Environment(\.displayScale) private var displayScale
    @State private var cover: UIImage?
    @State private var videoSize: CGSize?
    @State private var durationSeconds = 0

    private var exists: Bool { FileManager.default.fileExists(atPath: filePath) }
    private var fileName: String { URL(fileURLWithPath: filePath).lastPathComponent }

    init(filePath: String, identifier: String = "agent-video-bubble",
         open: @escaping () -> Void) {
        self.filePath = filePath
        self.identifier = identifier
        self.open = open
    }

    var body: some View {
        Button {
            guard conversationActionsAllowed() else { return }
            open()
        } label: {
            ZStack {
                if let cover {
                    Image(uiImage: cover).resizable().scaledToFit()
                } else {
                    LinearGradient(
                        colors: [
                            Color(red: 0.10, green: 0.17, blue: 0.27),
                            Color(red: 0.18, green: 0.32, blue: 0.47),
                        ],
                        startPoint: .topLeading, endPoint: .bottomTrailing)
                }
                Image(systemName: exists ? "play.fill" : "exclamationmark.triangle.fill")
                    .font(AppTypography.symbol(size: 20, weight: .bold))
                    .foregroundStyle(.white)
                    .frame(width: 52, height: 52)
                    .background(.black.opacity(0.5), in: Circle())
                // 文件名压在缩略图角落，保留画面主体，也能辨认当前是哪一段视频。
                Text(fileName)
                    .font(AppTypography.metadata.weight(.semibold))
                    .foregroundStyle(.white)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .padding(.horizontal, 7)
                    .padding(.vertical, 4)
                    .background(.black.opacity(0.58), in: RoundedRectangle(cornerRadius: 6))
                    .frame(maxWidth: previewSize.width - 16, alignment: .leading)
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
                    .padding(8)
                Text(
                    String(
                        format: "%d:%02d", durationSeconds / 60,
                        durationSeconds % 60)
                )
                .font(AppTypography.detailMonospaced.weight(.semibold))
                .foregroundStyle(.white)
                .padding(.horizontal, 7)
                .frame(height: 22)
                .background(.black.opacity(0.56), in: Capsule())
                .frame(
                    maxWidth: .infinity, maxHeight: .infinity,
                    alignment: .bottomTrailing
                )
                .padding(8)
            }
            .frame(width: previewSize.width, height: previewSize.height)
            .clipped()
            .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
        }
        .buttonStyle(.plain)
        .disabled(!exists)
        .accessibilityLabel(exists ? "播放视频 \(fileName)" :
                            "视频文件已丢失")
        .accessibilityIdentifier(identifier)
        .task(id: filePath) {
            guard exists else { return }
            let asset = AVURLAsset(url: URL(fileURLWithPath: filePath))
            if let track = try? await asset.loadTracks(withMediaType: .video).first,
                let naturalSize = try? await track.load(.naturalSize),
                let transform = try? await track.load(.preferredTransform)
            {
                let displayed = CGRect(origin: .zero, size: naturalSize).applying(transform)
                videoSize = CGSize(width: abs(displayed.width), height: abs(displayed.height))
            }
            if let duration = try? await asset.load(.duration),
                duration.seconds.isFinite
            {
                durationSeconds = max(0, Int(duration.seconds))
            }
            let generator = AVAssetImageGenerator(asset: asset)
            generator.appliesPreferredTrackTransform = true
            generator.maximumSize = CGSize(
                width: VideoBubbleSize.assistantMaximum.width * min(displayScale, 3),
                height: VideoBubbleSize.assistantMaximum.height * min(displayScale, 3))
            let time = CMTime(
                seconds: durationSeconds > 1 ? 0.5 : 0,
                preferredTimescale: 600)
            if let image = try? await generator.image(at: time), !Task.isCancelled {
                videoSize = CGSize(width: image.image.width, height: image.image.height)
                cover = UIImage(cgImage: image.image)
            }
        }
    }

    private var previewSize: CGSize {
        VideoBubbleSize.fitted(videoSize ?? .zero,
                               maximum: VideoBubbleSize.assistantMaximum)
    }
}

private struct AIWorkspaceAudioCard: View {
    let filePath: String
    @State private var player: AVPlayer?
    @State private var isPlaying = false
    @State private var durationSeconds = 0

    var body: some View {
        Button {
            if isPlaying {
                player?.pause()
                isPlaying = false
            } else {
                if player == nil {
                    player = AVPlayer(url: URL(fileURLWithPath: filePath))
                }
                player?.play()
                isPlaying = true
            }
        } label: {
            HStack(spacing: 12) {
                Image(systemName: isPlaying ? "pause.fill" : "play.fill")
                    .frame(width: 38, height: 38)
                    .background(RemoteIMStyle.blueSoft, in: Circle())
                VStack(alignment: .leading, spacing: 3) {
                    Text(URL(fileURLWithPath: filePath).lastPathComponent)
                        .lineLimit(1)
                        .truncationMode(.middle)
                    Text(String(format: "%d:%02d", durationSeconds / 60,
                                durationSeconds % 60))
                        .font(AssistantMessageFont.metadata)
                        .foregroundStyle(RemoteIMStyle.textSecondary)
                }
                Spacer(minLength: 0)
            }
            .font(ChatTypography.body)
            .foregroundStyle(RemoteIMStyle.textPrimary)
            .padding(10)
            .frame(width: 240)
            .background(Color(uiColor: .secondarySystemBackground),
                        in: RoundedRectangle(cornerRadius: 12))
        }
        .buttonStyle(.plain)
        .accessibilityLabel(isPlaying ? "暂停音频" : "播放音频")
        .task(id: filePath) {
            let asset = AVURLAsset(url: URL(fileURLWithPath: filePath))
            if let duration = try? await asset.load(.duration), duration.seconds.isFinite {
                durationSeconds = max(0, Int(duration.seconds))
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: .AVPlayerItemDidPlayToEndTime)) {
            notification in
            if notification.object as? AVPlayerItem === player?.currentItem {
                isPlaying = false
                player?.seek(to: .zero)
            }
        }
        .onDisappear { player?.pause(); isPlaying = false }
    }
}


// 图片打开后在同一个原生容器中浏览所有相邻图片和视频；切换不再重建 Root 的播放器。
struct AIAssistantImagePreviewLayer: View {
    @ObservedObject private var model = AIAssistantModel.shared
    @ObservedObject var sources: AIImageSourceFrameStore

    var body: some View {
        ZStack {
            if let preview = model.previewImage {
                let selected = MaiMediaItem(kind: .image, path: preview.filePath)
                MaiLocalMediaPager(
                    sequence: preview.sequence ?? MaiMediaSequence(selected: selected, items: [selected]),
                    imageIdentifier: "agent-image-preview", showsImageActions: false,
                    sourceFrame: { view in
                        guard let frame = preview.sourceID.flatMap({ sources.frames[$0] }) ?? preview.sourceFrame
                        else { return nil }
                        return view.convert(frame, from: nil)
                    }, thumbnail: preview.thumbnail) {
                        model.previewImage = nil
                        maiLocalMediaTrace("image-model-closed")
                    }
                    .id(preview.id)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .ignoresSafeArea()
        .allowsHitTesting(model.previewImage != nil)
        .preference(key: AIImagePreviewVisibilityKey.self, value: model.previewImage != nil)
    }
}

private struct AIThinkingIndicator: View {
    var body: some View {
        HStack(spacing: 8) {
            ProgressView().controlSize(.small)
            Text("正在思考…").font(AssistantMessageFont.detail)
        }
        .foregroundStyle(.secondary)
        .frame(height: 24)
        .accessibilityLabel("正在思考")
    }
}

/// Details remain separate from AIPart: owner paths in recorded output must never
/// become local media actions. The same disclosure works online and with warm cache.
private struct AIRemoteToolDetailPane: View {
    @ObservedObject private var model = AIAssistantModel.shared
    let owner: AIRemoteImageOwner
    let messageID: String
    let part: AIPart

    private var state: AIRemoteToolDetailsState? {
        model.remoteToolDetailState(owner: owner, messageID: messageID, partID: part.id)
    }
    private var identity: String {
        [owner.accountID, owner.slot, owner.loginSessionID, owner.sessionID,
         String(owner.selectionGeneration), messageID, part.id, part.toolDetailIdentity].joined(separator: "|")
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            ForEach(AIRemoteToolDetailField.allCases, id: \.self) { field in
                fieldView(field, state?.fields[field] ?? AIRemoteToolDetailFieldState())
            }
            Button("从头读取") {
                Task { await model.loadRemoteToolDetails(owner: owner, messageID: messageID, part: part, refresh: true) }
            }
            .accessibilityIdentifier("tool-detail-restart-" + part.id)
            .disabled(state?.fields.values.contains(where: \.loading) == true)
        }
        .task(id: identity) { await model.loadRemoteToolDetails(owner: owner, messageID: messageID, part: part) }
        .onDisappear { model.cancelRemoteToolDetails(owner: owner, messageID: messageID, partID: part.id) }
    }

    private func fieldView(_ field: AIRemoteToolDetailField, _ value: AIRemoteToolDetailFieldState) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(field.title).font(AssistantMessageFont.detail.weight(.semibold))
            if value.retainedFromOffset > 0 {
                Text("较早的内容已收起，可从头读取。")
                    .font(AssistantMessageFont.detail).foregroundStyle(.secondary)
            }
            if value.loaded {
                Text(value.text.isEmpty ? "（空）" : value.text)
                    .font(AssistantMessageFont.detailMonospaced)
                    .textSelection(.enabled)
                    .accessibilityIdentifier("tool-detail-text-" + field.rawValue + "-" + part.id)
            }
            if value.containsInvalidUtf8 {
                Text("原记录含无效 UTF-8，预览使用替换字符；原页字节可在下方查看。")
                    .font(AssistantMessageFont.detail).foregroundStyle(.secondary)
                if !value.rawBase64Pages.isEmpty {
                    AIExpandableBlock(title: "原页 Base64") {
                        Text(value.rawBase64Pages.joined(separator: "\n"))
                            .font(AssistantMessageFont.detailMonospaced).textSelection(.enabled)
                    }
                }
            }
            if value.loading { ProgressView("读取中…").font(AssistantMessageFont.detail) }
            if let error = value.error {
                Text(error).font(AssistantMessageFont.detail).foregroundStyle(.red)
                    .accessibilityIdentifier("tool-detail-error-" + field.rawValue + "-" + part.id)
            }
            if value.hasMore {
                Button("继续加载 \(field.title)（\(value.sourceBytesRead)/\(value.totalBytes) 字节）") {
                    Task { await model.loadMoreRemoteToolDetail(owner: owner, messageID: messageID, part: part, field: field) }
                }
                .accessibilityIdentifier("tool-detail-more-" + field.rawValue + "-" + part.id)
                .disabled(value.loading)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

private struct AIExpandableBlock<Content: View>: View {
    let title: String
    let identifier: String
    let content: Content
    @State private var expanded = false

    init(title: String, identifier: String = "", @ViewBuilder content: () -> Content) {
        self.title = title
        self.identifier = identifier
        self.content = content()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Button { withAnimation(.easeOut(duration: 0.16)) { expanded.toggle() } } label: {
                HStack(spacing: 8) {
                    Text(title).font(AssistantMessageFont.detail).lineLimit(1)
                    Spacer()
                    Image(systemName: "chevron.right").rotationEffect(.degrees(expanded ? 90 : 0))
                }
                .foregroundStyle(.secondary)
                .frame(minHeight: 32)
            }.buttonStyle(.plain).accessibilityIdentifier(identifier)
            if expanded { content }
        }
    }
}

@MainActor
private final class AIComposerFocusController {
    weak var textView: UITextView?

    func focus() { textView?.becomeFirstResponder() }
    func dismiss() {
        #if DEBUG
        AISessionDrawerDiagnostics.dismissFocus()
        #endif
        textView?.resignFirstResponder()
    }
}

private struct AIPickedVideoTransfer: Transferable, Sendable {
    let fileURL: URL

    static var transferRepresentation: some TransferRepresentation {
        FileRepresentation(contentType: .movie) { video in
            SentTransferredFile(video.fileURL)
        } importing: { received in
            let sourceURL = received.file
            let targetURL = try await RemoteIMBackgroundWork.file {
                let directory = FileManager.default.temporaryDirectory
                    .appendingPathComponent("AIAssistantImports", isDirectory: true)
                try FileManager.default.createDirectory(at: directory,
                                                        withIntermediateDirectories: true)
                let fileExtension = sourceURL.pathExtension.isEmpty
                    ? "mov" : sourceURL.pathExtension
                let target = directory.appendingPathComponent(UUID().uuidString)
                    .appendingPathExtension(fileExtension)
                try FileManager.default.copyItem(at: sourceURL, to: target)
                return target
            }
            return AIPickedVideoTransfer(fileURL: targetURL)
        }
    }
}

private struct AIDraftAttachmentTile: View {
    @Environment(\.aiConversationActionsAllowed) private var conversationActionsAllowed
    let file: AIImportedFile
    let position: Int
    let workspacePath: String
    let open: () -> Void
    let remove: () -> Void
    let canRemove: Bool
    @State private var videoCover: UIImage?

    private var filePath: String {
        AIAssistantPathPolicy.resolve(file.relativePath, workspacePath: workspacePath)?.path ?? ""
    }

    private var fileName: String {
        let stored = URL(fileURLWithPath: file.relativePath).lastPathComponent
        if stored.count > 9,
           stored[stored.index(stored.startIndex, offsetBy: 8)] == "-" {
            return String(stored.dropFirst(9))
        }
        return stored
    }

    private var typeName: String {
        file.isImage ? "图片" : file.mimeType.hasPrefix("video/") ? "视频" :
            file.mimeType.hasPrefix("audio/") ? "音频" : "文件"
    }

    // 以缩略图或视频首帧识别待发媒体，移除按钮覆盖在卡片角落但保留独立命中区域。
    var body: some View {
        VStack(spacing: 3) {
            ZStack(alignment: .topTrailing) {
                Button {
                    guard conversationActionsAllowed() else { return }
                    open()
                } label: {
                    ZStack {
                        if file.isImage {
                            RemoteIMAsyncImage(
                                filePath: filePath,
                                maximumPointSize: CGSize(width: 96, height: 80)
                            ) { image in
                                Image(uiImage: image).resizable().scaledToFill()
                            } placeholder: { failed in
                                Image(systemName: failed ? "photo.badge.exclamationmark" : "photo")
                                    .foregroundStyle(RemoteIMStyle.textSecondary)
                            }
                        } else if file.mimeType.hasPrefix("video/"), let videoCover {
                            Image(uiImage: videoCover).resizable().scaledToFill()
                        } else if !file.mimeType.hasPrefix("video/") {
                            Image(systemName: file.mimeType.hasPrefix("audio/")
                                  ? "waveform" : "doc")
                                .font(AppTypography.symbol(size: 27))
                                .foregroundStyle(RemoteIMStyle.blue)
                                .frame(maxWidth: .infinity, maxHeight: .infinity)
                                .background(RemoteIMStyle.blueSoft)
                        } else {
                            LinearGradient(
                                colors: [Color(red: 0.10, green: 0.17, blue: 0.27),
                                         Color(red: 0.18, green: 0.32, blue: 0.47)],
                                startPoint: .topLeading, endPoint: .bottomTrailing
                            )
                        }
                        if file.mimeType.hasPrefix("video/") {
                            Image(systemName: "play.fill")
                                .font(AppTypography.symbol(size: 16, weight: .bold))
                                .foregroundStyle(.white)
                                .frame(width: 34, height: 34)
                                .background(.black.opacity(0.55), in: Circle())
                        }
                    }
                    .frame(width: 96, height: 80)
                    .clipped()
                    .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
                }
                .buttonStyle(.plain)
                .accessibilityLabel("预览\(typeName) \(fileName)")
                Button {
                    guard conversationActionsAllowed() else { return }
                    remove()
                } label: {
                    Image(systemName: "xmark.circle.fill")
                        .font(AppTypography.symbol(size: 20, weight: .semibold))
                        .foregroundStyle(.white)
                        .shadow(color: .black.opacity(0.7), radius: 3)
                        .frame(width: 44, height: 44)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .disabled(!canRemove)
                .accessibilityLabel("移除附件 \(fileName)")
                .accessibilityIdentifier("ai-remove-attachment-\(file.relativePath)")
            }
            Text("\(typeName) \(position)")
                .font(AppTypography.metadata.weight(.medium))
                .foregroundStyle(RemoteIMStyle.textSecondary)
                .lineLimit(1)
                .frame(width: 96)
            Text(fileName)
                .font(AppTypography.metadata)
                .foregroundStyle(RemoteIMStyle.textSecondary)
                .lineLimit(1)
                .truncationMode(.middle)
                .frame(width: 96)
        }
        .frame(width: 96)
        .task(id: filePath) {
            guard file.mimeType.hasPrefix("video/"), !filePath.isEmpty else { return }
            let generator = AVAssetImageGenerator(asset: AVURLAsset(url: URL(fileURLWithPath: filePath)))
            generator.appliesPreferredTrackTransform = true
            generator.maximumSize = CGSize(width: 240, height: 200)
            if let frame = try? await generator.image(at: .zero), !Task.isCancelled {
                videoCover = UIImage(cgImage: frame.image)
            }
        }
    }
}

private struct AIComposerSnapshot: Equatable {
    let ready: Bool
    let busy: Bool
    let isSubmitting: Bool
    let uploadStatus: String
    let isCreatingConversation: Bool
    let selected: String
    let quotedMessage: AIMessage?
    let attachmentPanelPresented: Bool
    let attachments: [AIImportedFile]
    let workspacePath: String
    let remoteRestoreText: String
    let remoteRestoreGeneration: Int
    let screenFrameImportRevision: Int
    let screenFrameAccountID: String
    let screenFrameOwnerID: String
}

/// 模式和所有者随同一次 presentation 提交，避免首帧用尚未刷新的可选 target 推断模式。
private struct AICameraPresentation: Identifiable {
    enum Mode {
        case manual(AIAttachmentImportTarget)
        case manualTool(UUID)
        case timed(UUID)
    }

    let id = UUID()
    let mode: Mode

    var timedRequestID: UUID? {
        if case .timed(let requestID) = mode { return requestID }
        return nil
    }

    var manualToolRequestID: UUID? {
        if case .manualTool(let requestID) = mode { return requestID }
        return nil
    }
}

private struct AIComposer: View, Equatable {
    // 流式消息每 100 毫秒可能更新一次；输入框只依赖这份轻量状态快照。
    // 保留 model 用于用户动作，但不订阅它的每一次消息变更。
    let model: AIAssistantModel
    let snapshot: AIComposerSnapshot
    let focusController: AIComposerFocusController
    let transcriptionPresentation: VoiceTranscriptionPresentation
    @EnvironmentObject private var appState: RemoteIMAppState
    @Binding var isAttachmentPanelPresented: Bool
    @StateObject private var speechRecognizer = RealtimeSpeechRecognizer()
    @State private var draft = ""
    @State private var importing = false
    @ObservedObject private var music = MaiLocalMusicPlayer.shared
    @ObservedObject private var screenCapture = AIMobileScreenCapture.shared
    @ObservedObject private var agentCamera = AIMobileCameraCapture.shared
    @ObservedObject private var localMediaRequests = AIMobileLocalMediaRequests.shared
    @Environment(\.scenePhase) private var scenePhase
    @State private var importingMusic = false
    @State private var showingMusicSource = false
    @State private var showingMusicLibrary = false
    @State private var musicLibrarySelectionReceived = false
    @State private var requestingMusicLibraryAuthorization = false
    @State private var showingMusic = false
    @State private var musicSelectionReceived = false
    @State private var musicPickerAgentRequestID: UUID?
    @State private var isPhotoPickerPresented = false
    @State private var cameraPresentation: AICameraPresentation?
    @State private var manualCameraOpening = false
    @State private var manualToolPresentationRequestID: UUID?
    @State private var selectedMediaItems: [PhotosPickerItem] = []
    @State private var libraryTarget: AIAttachmentImportTarget?
    @State private var fileTarget: AIAttachmentImportTarget?
    @State private var importingTargets: [AIAttachmentImportTarget: Int] = [:]
    @State private var draftVideoPreview: AIVideoPreviewItem?
    @State private var draftFilePreview: URL?
    @State private var screenFrameSelection: AIScreenFrameImportBinding?
    @State private var cameraFrameSelection: AIScreenFrameImportBinding?
    @State private var cameraBinding: AIScreenFrameImportBinding?
    @State private var cameraReviewAfterDismiss: AIScreenFrameImportBinding?
    @State private var cameraFinishing = false
    @State private var toolCameraRequestID: UUID?
    #if DEBUG && targetEnvironment(simulator)
    @State private var cameraFixtureInstalled = false
    #endif
    @State private var draftSession = ""
    @State private var draftImportTarget: AIAttachmentImportTarget?
    @State private var isVoiceMode = false
    @State private var isPressingVoice = false
    @State private var composerFocusRequestGeneration = 0
    @State private var composerEditingController = ComposerTextEditingController()
    @State private var composerEditMenuState: ComposerEditMenuState?
    @State private var voiceBaseDraft = ""
    @State private var voiceStartTask: Task<Bool, Never>?
    nonisolated static func == (lhs: Self, rhs: Self) -> Bool {
        lhs.model === rhs.model && lhs.snapshot == rhs.snapshot &&
            lhs.focusController === rhs.focusController &&
            lhs.transcriptionPresentation === rhs.transcriptionPresentation
    }
    // 输入栏上方只显示当前会话的待发媒体；横向托盘不挤压消息区和键盘上方输入区域。
    var body: some View {
        VStack(spacing: 0) {
            if !snapshot.uploadStatus.isEmpty {
                Label(snapshot.uploadStatus, systemImage: "icloud.and.arrow.up")
                    .font(AppTypography.detail)
                    .foregroundStyle(RemoteIMStyle.blue)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 16)
                    .padding(.vertical, 7)
                    .background(RemoteIMStyle.blueSoft)
            }
            if let quoted = snapshot.quotedMessage {
                HStack(spacing: 10) {
                    Image(systemName: "arrowshape.turn.up.left")
                        .foregroundStyle(RemoteIMStyle.blue)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(quoted.role == "user" ? "引用你的消息" : "引用 AI 消息")
                            .font(AppTypography.metadata.weight(.semibold))
                        Text(quoted.quotePreview)
                            .font(AppTypography.metadata)
                            .foregroundStyle(.secondary)
                            .lineLimit(2)
                    }
                    Spacer(minLength: 4)
                    Button { model.quotedMessage = nil } label: {
                        Image(systemName: "xmark.circle.fill")
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel("取消引用")
                }
                .padding(.horizontal, 16)
                .padding(.vertical, 9)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(Color(uiColor: .secondarySystemBackground))
            }
            if !snapshot.attachments.isEmpty || isImportingCurrentSession {
                VStack(alignment: .leading, spacing: 6) {
                    HStack {
                        Text("待发送附件")
                            .font(AppTypography.metadata.weight(.semibold))
                            .foregroundStyle(.secondary)
                        if !snapshot.attachments.isEmpty {
                            Text("\(snapshot.attachments.count)")
                                .font(AppTypography.metadata)
                                .foregroundStyle(.secondary)
                        }
                        Spacer()
                        if isImportingCurrentSession {
                            ProgressView("正在导入…").font(AppTypography.metadata)
                        }
                    }
                    if !snapshot.attachments.isEmpty {
                        ScrollView(.horizontal, showsIndicators: false) {
                            HStack(alignment: .top, spacing: 10) {
                                ForEach(Array(snapshot.attachments.enumerated()),
                                        id: \.element.attachmentIdentity) { entry in
                                    let file = entry.element
                                    AIDraftAttachmentTile(
                                        file: file,
                                        position: snapshot.attachments.prefix(entry.offset + 1)
                                            .filter { $0.isImage == file.isImage }.count,
                                        workspacePath: snapshot.workspacePath,
                                        open: { previewAttachment(file) },
                                        remove: {
                                            model.removeAttachment(file, from: snapshot.selected)
                                        },
                                        canRemove: !snapshot.isSubmitting
                                    )
                                }
                            }
                            .padding(.vertical, 2)
                        }
                    }
                }
                .padding(.horizontal, 16)
                .padding(.top, 10)
                .accessibilityIdentifier("ai-pending-attachments")
            }
            if music.isPlaying || music.isLoading {
                HStack(spacing: 10) {
                    Image(systemName: "music.note")
                        .foregroundStyle(RemoteIMStyle.blue)
                    Text(music.title.isEmpty ? "正在载入音乐…" : music.title)
                        .font(AppTypography.detail.weight(.medium))
                        .lineLimit(1)
                    Spacer(minLength: 4)
                    Button("播放器") { showingMusic = true }
                        .font(AppTypography.metadata.weight(.semibold))
                    Button { music.stop() } label: {
                        Image(systemName: "xmark")
                    }
                    .accessibilityLabel("停止音乐")
                }
                .buttonStyle(.plain)
                .padding(.horizontal, 14).frame(height: 40)
                .background(RemoteIMStyle.blueSoft,
                            in: RoundedRectangle(cornerRadius: 11))
                .padding(.horizontal, 16)
                .accessibilityIdentifier("ai-music-mini-player")
            }
            screenFramesStrip
            cameraFramesStrip
            #if DEBUG && targetEnvironment(simulator)
            if AICameraUITestSupport.isManualToolRequested {
                AICameraManualToolUITestControls()
            }
            if AICameraUITestSupport.isRequested {
                AICameraUITestControls {
                    do {
                        _ = try localMediaRequests.openCamera(in: model.selected,
                                                             workspacePath: model.workspacePath)
                    } catch { model.showTransientError(error.localizedDescription) }
                }
            }
            #endif
            HStack(alignment: .bottom, spacing: 8) {
                Button {
                    composerEditMenuState = nil
                    isAttachmentPanelPresented = false
                    focusController.dismiss()
                    withAnimation(.easeOut(duration: 0.16)) { isVoiceMode.toggle() }
                } label: {
                    Image(systemName: isVoiceMode ? "keyboard" : "speaker.wave.2.fill")
                        .font(AppTypography.symbol(size: 18, weight: .bold))
                        .frame(width: 44, height: 44)
                        .background(RemoteIMStyle.blueSoft, in: Circle())
                        .overlay(Circle().stroke(RemoteIMStyle.border, lineWidth: 1))
                }
                .buttonStyle(.plain)
                .foregroundStyle(RemoteIMStyle.blue)
                .accessibilityLabel(isVoiceMode ? "切换到键盘输入" : "切换到按住转文字")

                if isVoiceMode {
                    AITranscriptionButton(
                        isPressing: isPressingVoice,
                        isCancelling: transcriptionPresentation.target == .cancel,
                        isEnabled: canStartVoiceTranscription,
                        onChanged: { translation in
                            handleVoiceGestureChanged(translation: translation, location: nil)
                        },
                        onEnded: { translation in
                            Task {
                                await finishVoiceGesture(translation: translation, location: nil)
                            }
                        }
                    )
                } else {
                    ZStack(alignment: .topLeading) {
                        ComposerTextView(
                            text: $draft,
                            onSubmit: { submitDraft() },
                            focusRequestGeneration: composerFocusRequestGeneration,
                            editingController: composerEditingController,
                            onEditMenuRequested: { state in
                                let next = state.hasActions ? state : nil
                                guard composerEditMenuState != next else { return }
                                withAnimation(.easeOut(duration: 0.1)) {
                                    composerEditMenuState = next
                                }
                            },
                            onEditMenuDismissed: {
                                guard composerEditMenuState != nil else { return }
                                withAnimation(.easeOut(duration: 0.08)) {
                                    composerEditMenuState = nil
                                }
                            },
                            onTypingActivityChanged: { active in
                                model.setComposerTyping(active)
                            },
                            voiceTranscriptionEnabled: canStartVoiceTranscription,
                            onVoiceLongPressChanged: { translation, location in
                                handleVoiceGestureChanged(
                                    translation: translation,
                                    location: location
                                )
                            },
                            onVoiceLongPressEnded: { translation, location in
                                Task {
                                    await finishVoiceGesture(
                                        translation: translation,
                                        location: location
                                    )
                                }
                            },
                            onVoiceLongPressCancelled: {
                                cancelVoiceTranscription(restoresDraft: true)
                            },
                            allowsEmptySubmit: !snapshot.attachments.isEmpty,
                            registerTextView: { focusController.textView = $0 },
                            accessibilityIdentifier: "ai-composer",
                            accessibilityLabel: "创作中心输入框，可按住转文字"
                        )
                        if draft.isEmpty {
                            Text(composerPrompt)
                                .foregroundStyle(.secondary)
                                .font(ChatTypography.body.weight(isPressingVoice ? .semibold : .regular))
                                .padding(.horizontal, 13)
                                .padding(.vertical, 13)
                                .allowsHitTesting(false)
                                .accessibilityHidden(true)
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(Color(uiColor: .secondarySystemBackground),
                                in: RoundedRectangle(cornerRadius: 14, style: .continuous))
                    .overlay(
                        RoundedRectangle(cornerRadius: 14, style: .continuous)
                            .stroke(snapshot.ready ? RemoteIMStyle.blue : RemoteIMStyle.border,
                                    lineWidth: snapshot.ready ? 1.5 : 1)
                    )
                    .overlay(alignment: .topLeading) {
                        if let state = composerEditMenuState {
                            ComposerEditActionBar(
                                state: state,
                                pasteTarget: composerEditingController.textView,
                                perform: performComposerEditAction
                            )
                            .offset(x: 4, y: -50)
                            .transition(.opacity)
                            .zIndex(20)
                        }
                    }
                    .zIndex(composerEditMenuState == nil ? 0 : 20)
                }

                // 右侧只保留附件加号；发送统一走编辑框的键盘发送动作，不另画发送或停止按钮。
                // 前一条模型/上传/回执等待不阻止继续添加附件，新消息由同一队列接收。
                Button {
                    composerEditMenuState = nil
                    if isAttachmentPanelPresented {
                        isAttachmentPanelPresented = false
                    } else {
                        focusController.dismiss()
                        isAttachmentPanelPresented = true
                    }
                } label: {
                    Image(systemName: "plus")
                        .font(AppTypography.symbol(size: 20, weight: .semibold))
                        .foregroundStyle(Color.primary)
                        .frame(width: 44, height: 44)
                        .background(Color(uiColor: .secondarySystemBackground), in: Circle())
                        .overlay(Circle().stroke(RemoteIMStyle.border, lineWidth: 1))
                }
                .buttonStyle(.plain)
                .disabled(!snapshot.ready)
                .accessibilityLabel(isAttachmentPanelPresented ? "收起更多功能" : "展开更多功能")

            }
            .padding(.horizontal, 16)
            .padding(.top, 12)
            .padding(.bottom, 10)

            if isAttachmentPanelPresented {
                Divider().background(RemoteIMStyle.border)
                ComposerAttachmentPanel(
                    canSendImage: true,
                    canSendVideo: true,
                    canSendFile: true,
                    canSendVoice: false,
                    openLibrary: {
                        isAttachmentPanelPresented = false
                        Task { await openPhotoPicker() }
                    },
                    openCamera: {
                        isAttachmentPanelPresented = false
                        Task { await openCamera() }
                    },
                    openFile: {
                        isAttachmentPanelPresented = false
                        fileTarget = model.importTarget()
                        importing = true
                    },
                    openVoiceInput: {},
                    suggestReply: nil,
                    openMusic: {
                        isAttachmentPanelPresented = false
                        musicSelectionReceived = false
                        musicPickerAgentRequestID = nil
                        showingMusicSource = true
                    },
                    openScreenShare: screenShareAction,
                    screenShareTitle: screenCapture.isSharing ? "停止共享" : "共享屏幕",
                    showsVoiceInput: false
                )
                .transition(.move(edge: .bottom).combined(with: .opacity))
            }
        }
            .animation(.easeOut(duration: 0.18), value: isAttachmentPanelPresented)
            .photosPicker(
                isPresented: $isPhotoPickerPresented,
                selection: $selectedMediaItems,
                maxSelectionCount: max(1, 10 - snapshot.attachments.count),
                selectionBehavior: .ordered,
                matching: .any(of: [.images, .videos]),
                photoLibrary: .shared()
            )
            .fullScreenCover(item: $cameraPresentation, onDismiss: {
                if let id = manualToolPresentationRequestID {
                    manualToolPresentationRequestID = nil
                    localMediaRequests.failManualPhoto(.canceled, requestID: id)
                }
                if let binding = cameraReviewAfterDismiss {
                    cameraReviewAfterDismiss = nil
                    cameraFrameSelection = binding
                }
            }) { presentation in
                switch presentation.mode {
                case .timed(let requestID):
                    ZStack {
                        Color.black.ignoresSafeArea()
                        #if DEBUG && targetEnvironment(simulator)
                        if AICameraUITestSupport.isRequested {
                            Color.blue.opacity(0.7).ignoresSafeArea()
                            Text("模拟相机画面").foregroundStyle(.white)
                        } else if let session = agentCamera.previewSession {
                            AIMobileAgentCameraPreview(session: session) {
                                agentCamera.setOrientation($0)
                            }.ignoresSafeArea()
                        } else { ProgressView().tint(.white) }
                        #else
                        if let session = agentCamera.previewSession {
                            AIMobileAgentCameraPreview(session: session) {
                                agentCamera.setOrientation($0)
                            }.ignoresSafeArea()
                        } else { ProgressView().tint(.white) }
                        #endif
                        VStack {
                            HStack {
                                Text("每 2 秒拍摄一张 · 已拍摄 \(agentCamera.count) 张")
                                Spacer()
                                Button("停止拍摄") {
                                    Task { await finishToolCamera(reason: "stopped", requestID: requestID) }
                                }
                                    .accessibilityIdentifier("ai-camera-stop")
                            }
                            .font(AppTypography.detail.weight(.semibold)).foregroundStyle(.white)
                            .padding().background(.black.opacity(0.6))
                            Spacer()
                        }
                    }
                case .manual(let target):
                    AIMobileManualCameraPicker(
                    onCapture: { image in
                        // 回调只属于本次手动入口，后来到达的 Agent 请求不能接管这张照片。
                        guard cameraPresentation?.id == presentation.id else { return }
                        cameraPresentation = nil
                        Task { await importCapturedImage(image, to: target) }
                    },
                    onCancel: {
                        guard cameraPresentation?.id == presentation.id else { return }
                        cameraPresentation = nil
                    }
                )
                    .ignoresSafeArea()
                case .manualTool(let requestID):
                    AIMobileManualCameraPicker(onCapture: { image in
                        guard cameraPresentation?.id == presentation.id,
                              localMediaRequests.manualPhotoRequestID == requestID else { return }
                        cameraPresentation = nil
                        localMediaRequests.finishManualPhoto(image, requestID: requestID)
                    }, onCancel: {
                        guard cameraPresentation?.id == presentation.id else { return }
                        cameraPresentation = nil
                        localMediaRequests.failManualPhoto(.canceled, requestID: requestID)
                    }).ignoresSafeArea()
                }
            }
            .fullScreenCover(item: $draftVideoPreview) { item in
                MaiFfplayVideoScreen(path: item.path) { draftVideoPreview = nil }
            }
            .quickLookPreview($draftFilePreview)
            .sheet(item: $screenFrameSelection) { binding in
                AIMobileScreenFramesView(model: model, binding: binding)
            }
            .sheet(item: $cameraFrameSelection) { binding in
                AIMobileScreenFramesView(model: model, binding: binding, kind: .camera)
            }
            .task(id: "\(snapshot.screenFrameAccountID)|\(snapshot.workspacePath)|\(snapshot.screenFrameOwnerID)") {
                await model.restoreScreenFrameArchivesForCurrentDraft()
                await model.restoreCameraFrameArchivesForCurrentDraft()
                adoptScreenArchive()
            }
            .onChange(of: screenCapture.archiveContext?.batchID) { _ in adoptScreenArchive() }
            .onChange(of: screenCapture.isSharing) { sharing in
                if !sharing && !screenCapture.isStarting { presentFinishedScreenFrames() }
            }
            .onChange(of: screenCapture.archiveBatch?.status) { status in
                if status == "stopped" { presentFinishedScreenFrames() }
            }
            .onChange(of: snapshot.screenFrameAccountID) { _ in
                localMediaRequests.invalidateManualPhoto()
                screenFrameSelection = nil
                cameraFrameSelection = nil
                cameraReviewAfterDismiss = nil
            }
            .onChange(of: agentCamera.count) { count in
                if let requestID = toolCameraRequestID {
                    localMediaRequests.updateCameraCount(count, requestID: requestID)
                }
            }
            .onChange(of: agentCamera.error) { error in
                guard !error.isEmpty, let id = toolCameraRequestID else { return }
                model.showTransientError(error)
                Task { await finishToolCamera(reason: "camera_error", requestID: id) }
            }
            .onChange(of: scenePhase) { phase in
                if phase == .background { localMediaRequests.invalidateManualPhoto() }
                if phase == .background, let id = toolCameraRequestID {
                    Task { await finishToolCamera(reason: "interrupted", requestID: id) }
                }
            }
            .fileImporter(
                isPresented: $importing,
                allowedContentTypes: model.selectedRemote == nil
                    ? [.plainText, .sourceCode, .json, .image, .movie]
                    : [.item]
            ) { result in
                if case let .success(url) = result {
                    guard let target = fileTarget else { return }
                    Task { await importSelectedFile(url, to: target) }
                }
            }
            .fileImporter(isPresented: $importingMusic,
                          allowedContentTypes: [.audio]) { result in
                let agentRequestID = musicPickerAgentRequestID
                musicPickerAgentRequestID = nil
                if let agentRequestID,
                   localMediaRequests.musicRequestID != agentRequestID { return }
                switch result {
                case .success(let url):
                    musicSelectionReceived = true
                    Task {
                        do {
                            try await music.openPickedFile(url)
                            showingMusic = true
                            localMediaRequests.finishMusic(fileName: url.lastPathComponent)
                        } catch {
                            localMediaRequests.failMusic(.failed)
                            model.showTransientError(error.localizedDescription)
                        }
                    }
                case .failure(let error):
                    musicSelectionReceived = true
                    localMediaRequests.failMusic(.canceled)
                    model.showTransientError(error.localizedDescription)
                }
            }
            .confirmationDialog("选择音乐来源", isPresented: $showingMusicSource) {
                Button("系统音乐库") {
                    requestingMusicLibraryAuthorization = true
                    Task { await openMusicLibrary() }
                }
                Button("选择文件") { importingMusic = true }
                Button("取消", role: .cancel) { localMediaRequests.failMusic(.canceled) }
            }
            .sheet(isPresented: $showingMusicLibrary) {
                MaiSystemMusicPicker(onPick: { item in
                    musicLibrarySelectionReceived = true
                    showingMusicLibrary = false
                    let agentRequestID = musicPickerAgentRequestID
                    musicPickerAgentRequestID = nil
                    Task {
                        do {
                            if let agentRequestID,
                               localMediaRequests.musicRequestID != agentRequestID { return }
                            if item.hasProtectedAsset || item.isCloudItem || item.assetURL == nil {
                                showingMusic = true
                                try await music.openAppleMusicItem(item)
                            } else {
                                do {
                                    let exported = try await music.exportLibraryItem(item)
                                    if let agentRequestID,
                                       localMediaRequests.musicRequestID != agentRequestID {
                                        try? FileManager.default.removeItem(at: exported)
                                        return
                                    }
                                    try music.openLocalFile(exported,
                                                            title: item.title ?? "音乐库歌曲",
                                                            ownsCopy: true,
                                                            artist: item.artist ?? "",
                                                            sourceName: "系统音乐库 · FFplay")
                                } catch {
                                    showingMusic = true
                                    try await music.openAppleMusicItem(item)
                                }
                            }
                            if let agentRequestID,
                               localMediaRequests.musicRequestID != agentRequestID {
                                music.stop()
                                return
                            }
                            showingMusic = true
                            localMediaRequests.finishMusic(fileName: item.title ?? "音乐库歌曲")
                        } catch {
                            if showingMusic { music.reportError(error.localizedDescription) }
                            localMediaRequests.failMusic(.failed)
                            model.showTransientError(error.localizedDescription)
                        }
                    }
                }, onCancel: {
                    showingMusicLibrary = false
                    localMediaRequests.failMusic(.canceled)
                })
            }
            .sheet(isPresented: $showingMusic) {
                MaiLocalMusicPlayerView(music: music)
            }
            .onChange(of: localMediaRequests.cameraRequestID) { request in
                guard let request else {
                    if let id = toolCameraRequestID {
                        Task { await finishToolCamera(reason: "stopped", requestID: id) }
                    }
                    return
                }
                Task { await openToolCamera(requestID: request) }
            }
            .onChange(of: localMediaRequests.manualPhotoRequestID) { request in
                guard let request else {
                    if cameraPresentation?.manualToolRequestID != nil { cameraPresentation = nil }
                    return
                }
                Task { await openManualToolCamera(requestID: request) }
            }
            .onChange(of: localMediaRequests.musicRequestID) { request in
                guard request != nil else { return }
                musicSelectionReceived = false
                musicPickerAgentRequestID = request
                showingMusicSource = true
            }
            .onChange(of: showingMusicSource) { presented in
                guard !presented, localMediaRequests.musicRequestID != nil else { return }
                Task { @MainActor in
                    try? await Task.sleep(nanoseconds: 200_000_000)
                    if !showingMusicSource && !importingMusic && !showingMusicLibrary &&
                       !requestingMusicLibraryAuthorization {
                        localMediaRequests.failMusic(.canceled)
                    }
                }
            }
            .onChange(of: showingMusicLibrary) { presented in
                guard !presented, !musicLibrarySelectionReceived,
                      localMediaRequests.musicRequestID != nil else { return }
                localMediaRequests.failMusic(.canceled)
            }
            .onChange(of: localMediaRequests.musicPresentationID) { request in
                if request != nil { showingMusic = true }
            }
            .onChange(of: importingMusic) { presented in
                guard !presented, !musicSelectionReceived,
                      localMediaRequests.musicRequestID != nil else { return }
                Task { @MainActor in
                    try? await Task.sleep(nanoseconds: 200_000_000)
                    if !musicSelectionReceived { localMediaRequests.failMusic(.canceled) }
                }
            }
            .onChange(of: selectedMediaItems) { items in
                guard !items.isEmpty else { return }
                guard let target = libraryTarget else { return }
                Task { await importSelectedMedia(items, to: target) }
            }
            .onAppear {
                #if DEBUG && targetEnvironment(simulator)
                if (AICameraUITestSupport.isRequested || AICameraUITestSupport.isManualRequested),
                   !cameraFixtureInstalled {
                    cameraFixtureInstalled = true
                    if AICameraUITestSupport.isManualRequested {
                        AICameraUITestSupport.prepareManualFixture(model)
                    } else { model.installHistoryUITestFixture() }
                }
                #endif
                #if DEBUG && targetEnvironment(simulator)
                AICameraUITestSupport.prepareMusicMini(music)
                #endif
                localMediaRequests.presenterActive = true
                draftSession = snapshot.selected
                draftImportTarget = draftSession.isEmpty ? model.importTarget() : nil
                draft = model.drafts[draftSession] ?? ""
                transcriptionPresentation.onCancel = {
                    cancelVoiceTranscription(restoresDraft: true)
                }
                transcriptionPresentation.onEdit = {
                    Task {
                        await finishVoiceGesture(
                            translation: .zero,
                            location: nil,
                            explicitTarget: .edit
                        )
                    }
                }
                speechRecognizer.onLiveTextUpdate = { text, sessionID in
                    guard isPressingVoice else { return }
                    transcriptionPresentation.updateLiveText(text, sessionID: sessionID)
                }
                speechRecognizer.onFailure = { error, _ in
                    // 服务端可在松手前报错，立即恢复输入栏，不把中间态当作消息提交。
                    isPressingVoice = false
                    transcriptionPresentation.reset()
                    model.error = "语音转文字失败：\(error.localizedDescription)"
                }
            }
            .onChange(of: appState.remoteDiagnosticsIdentity) { _ in
                // 当前页面即使复用相同 Session，也不能沿用上一轮账号认证的识别会话。
                cancelVoiceTranscription(restoresDraft: true)
                localMediaRequests.invalidateManualPhoto()
                cameraFrameSelection = nil
                if let id = toolCameraRequestID {
                    Task { await finishToolCamera(reason: "interrupted", requestID: id) }
                }
            }
            .onDisappear {
                music.stop()
                if cameraPresentation == nil { localMediaRequests.invalidateManualPhoto() }
                if cameraPresentation == nil, let id = toolCameraRequestID {
                    Task { await finishToolCamera(reason: "interrupted", requestID: id) }
                }
                if cameraPresentation != nil || importingMusic {
                    localMediaRequests.presenterActive = false
                } else {
                    localMediaRequests.deactivate()
                }
                if let session = draftStorageSession { model.drafts[session] = draft }
                voiceStartTask?.cancel()
                voiceStartTask = nil
                speechRecognizer.onLiveTextUpdate = nil
                speechRecognizer.onFailure = nil
                transcriptionPresentation.onCancel = nil
                transcriptionPresentation.onEdit = nil
                transcriptionPresentation.reset()
                speechRecognizer.cancel()
                isAttachmentPanelPresented = false
                composerEditMenuState = nil
                selectedMediaItems = []
            }
            .onChange(of: snapshot.selected) { selected in
                cancelVoiceTranscription(restoresDraft: true)
                localMediaRequests.invalidateManualPhoto()
                if let id = toolCameraRequestID {
                    Task { await finishToolCamera(reason: "interrupted", requestID: id) }
                }
                let previousSession = draftSession
                let storageSession = draftStorageSession
                if let storageSession {
                    model.drafts[storageSession] = draft
                    if storageSession != previousSession {
                        model.drafts.removeValue(forKey: previousSession)
                    }
                }
                draftSession = selected
                draftImportTarget = selected.isEmpty ? model.importTarget() : nil
                if storageSession != selected { draft = model.drafts[selected] ?? "" }
                if !snapshot.attachments.isEmpty { isVoiceMode = false }
            }
            .onChange(of: snapshot.workspacePath) { _ in
                localMediaRequests.invalidateManualPhoto()
            }
            .onChange(of: snapshot.remoteRestoreGeneration) { _ in
                guard model.selectedRemote != nil, draftSession == model.selected,
                      draft.isEmpty, !model.remoteRestoreText.isEmpty else { return }
                draft = model.remoteRestoreText
            }
            .onChange(of: draft) { value in
                if let session = draftStorageSession { model.drafts[session] = value }
            }
            .onChange(of: snapshot.attachments) { files in
                if !files.isEmpty { isVoiceMode = false }
            }
    }

    @ViewBuilder private var screenFramesStrip: some View {
        let saved = model.screenFrameBindingsForCurrentDraft()
        let active = screenCapture.isSharing || screenCapture.isStarting
        if model.screenFrameArchiveUnavailableCountForCurrentDraft() > 0 {
            Text("部分旧屏幕截图批次不可读取，原文件仍保留。")
                .font(AppTypography.detail).foregroundStyle(.secondary)
                .frame(maxWidth: .infinity, alignment: .leading).padding(.horizontal, 16)
        }
        if active || !saved.isEmpty {
            HStack(spacing: 10) {
                Image(systemName: "rectangle.on.rectangle")
                    .foregroundStyle(RemoteIMStyle.blue)
                VStack(alignment: .leading, spacing: 2) {
                    if active {
                        Text(screenCapture.isStarting ? "正在开启屏幕共享…" :
                            "每 2 秒截取一张 · 已截取 \(screenCapture.archiveBatch?.frameCount ?? 0) 张")
                        if let context = screenCapture.archiveContext,
                           !saved.contains(where: { $0.id == context.batchID }) {
                            Text("图片保存在开始共享的原会话").font(AppTypography.metadata).foregroundStyle(.secondary)
                        }
                    } else {
                        Text("共享截图已保留 · 选择需要的图片")
                    }
                }.font(AppTypography.detail.weight(.medium))
                Spacer(minLength: 4)
                if active {
                    Button("停止共享") { Task { await stopScreenShareAndReview() } }
                        .accessibilityIdentifier("ai-screen-share-stop")
                } else if let latest = saved.last {
                    Button("选择图片") { screenFrameSelection = latest }
                        .accessibilityIdentifier("ai-screen-share-review")
                }
            }
            .font(AppTypography.metadata.weight(.semibold)).buttonStyle(.plain)
            .padding(.horizontal, 14).padding(.vertical, 10)
            .background(RemoteIMStyle.blueSoft, in: RoundedRectangle(cornerRadius: 11))
            .padding(.horizontal, 16)
            .accessibilityIdentifier("ai-screen-share-status")
        }
    }

    @ViewBuilder private var cameraFramesStrip: some View {
        if model.cameraFrameArchiveUnavailableCountForCurrentDraft() > 0 {
            Text("部分旧相机图片批次不可读取，原文件仍保留。")
                .font(AppTypography.detail).foregroundStyle(.secondary)
                .frame(maxWidth: .infinity, alignment: .leading).padding(.horizontal, 16)
        }
        if let latest = model.cameraFrameBindingsForCurrentDraft().last {
            HStack {
                Image(systemName: "camera").foregroundStyle(RemoteIMStyle.blue)
                Text("相机图片已保留 · 选择需要的图片")
                Spacer()
                Button("选择图片") { cameraFrameSelection = latest }
                    .accessibilityIdentifier("ai-camera-review")
            }
            .font(AppTypography.metadata.weight(.semibold)).buttonStyle(.plain)
            .padding(.horizontal, 14).padding(.vertical, 10)
            .background(RemoteIMStyle.blueSoft, in: RoundedRectangle(cornerRadius: 11))
            .padding(.horizontal, 16)
        }
    }

    private func adoptScreenArchive() {
        guard let context = screenCapture.archiveContext else { return }
        // Host starts already register the actual session. A UI start supplies its
        // click-time snapshot below; never rebind another session's batch here.
        if model.screenFrameBinding(id: context.batchID) == nil {
            _ = try? model.registerScreenFrameArchive(context: context)
        }
    }

    private func presentFinishedScreenFrames() {
        adoptScreenArchive()
        guard screenCapture.archiveBatch?.status == "stopped",
              let context = screenCapture.archiveContext,
              let binding = model.screenFrameBindingsForCurrentDraft().first(where: { $0.id == context.batchID })
        else { return }
        screenFrameSelection = binding
    }

    private func stopScreenShareAndReview() async {
        await screenCapture.stop()
        presentFinishedScreenFrames()
    }

    private var screenShareAction: (() -> Void)? {
        guard aiSupportsWholeDeviceScreenCapture() else { return nil }
        return {
            isAttachmentPanelPresented = false
            if screenCapture.isSharing || screenCapture.isStarting {
                Task { @MainActor in await stopScreenShareAndReview() }
                return
            }
            do {
                let original = try model.makeScreenFrameImportSnapshot()
                Task { @MainActor in
                    do {
                        try await screenCapture.start(workspace: original.workspace,
                                                      sessionID: original.ownerSessionID)
                        guard let context = screenCapture.archiveContext else { return }
                        _ = try model.registerScreenFrameArchive(context: context, snapshot: original)
                    } catch {
                        // A canceled/failed system picker may already have created
                        // an empty batch. Keep it on the original target as well.
                        if let context = screenCapture.archiveContext,
                           context.belongs(to: original.workspace, sessionID: original.ownerSessionID) {
                            do { _ = try model.registerScreenFrameArchive(context: context, snapshot: original) }
                            catch { await screenCapture.stop() }
                        }
                        model.showTransientError(error.localizedDescription)
                    }
                }
            } catch { model.showTransientError(error.localizedDescription) }
        }
    }

    private func openPhotoPicker() async {
        guard snapshot.attachments.count < 10 else {
            model.showTransientError("每条消息最多添加 10 项图片或视频")
            return
        }
        let target = model.importTarget()
        guard await requestPhotoLibraryPermission() else {
            model.showTransientError("没有相册权限，请在系统设置中允许访问照片")
            return
        }
        libraryTarget = target
        isPhotoPickerPresented = true
    }

    private func openCamera() async {
        guard cameraPresentation == nil, !manualCameraOpening, !cameraFinishing,
              toolCameraRequestID == nil, localMediaRequests.cameraRequestID == nil,
              localMediaRequests.manualPhotoRequestID == nil else {
            model.showTransientError("相机正在使用中，请先结束当前拍摄")
            return
        }
        guard snapshot.attachments.count < 10 else {
            model.showTransientError("每条消息最多添加 10 项图片或视频")
            return
        }
        let target = model.importTarget()
        manualCameraOpening = true
        defer { manualCameraOpening = false }
        #if DEBUG && targetEnvironment(simulator)
        let simulated = AICameraUITestSupport.isManualRequested
        #else
        let simulated = false
        #endif
        guard simulated || UIImagePickerController.isSourceTypeAvailable(.camera) else {
            model.showTransientError("当前设备不支持拍照")
            return
        }
        if !simulated {
            guard await requestCameraPermission() else {
                model.showTransientError("没有相机权限，请在系统设置中允许 MaiChat 使用相机")
                return
            }
        }
        guard model.importTarget() == target,
              UIApplication.shared.applicationState == .active else { return }
        cameraPresentation = AICameraPresentation(mode: .manual(target))
    }

    /// Manual tool capture shares the normal shutter presentation. Its request
    /// identity remains separate from periodic capture and never owns a draft.
    private func openManualToolCamera(requestID: UUID) async {
        guard localMediaRequests.beginManualPhotoOpening(requestID: requestID) else { return }
        defer { localMediaRequests.finishManualPhotoOpening(requestID: requestID) }
        guard cameraPresentation == nil, !manualCameraOpening, !cameraFinishing,
              toolCameraRequestID == nil, localMediaRequests.cameraRequestID == nil else {
            localMediaRequests.failManualPhoto(.unavailable, requestID: requestID)
            return
        }
        manualCameraOpening = true
        defer { manualCameraOpening = false }
        #if DEBUG && targetEnvironment(simulator)
        let simulated = AICameraUITestSupport.isManualRequested
        #else
        let simulated = false
        #endif
        guard simulated || UIImagePickerController.isSourceTypeAvailable(.camera) else {
            localMediaRequests.failManualPhoto(.unavailable, requestID: requestID)
            return
        }
        if !simulated {
            let granted = await requestCameraPermission()
            guard localMediaRequests.manualPhotoRequestID == requestID else { return }
            guard granted else {
                localMediaRequests.failManualPhoto(.permissionDenied, requestID: requestID)
                return
            }
        }
        guard localMediaRequests.manualPhotoRequestID == requestID,
              model.selected == localMediaRequests.manualPhotoSessionID,
              (try? model.makeCameraFrameImportSnapshot().workspace.path) == localMediaRequests.manualPhotoWorkspacePath,
              UIApplication.shared.applicationState == .active else {
            localMediaRequests.failManualPhoto(.canceled, requestID: requestID)
            return
        }
        manualToolPresentationRequestID = requestID
        cameraPresentation = AICameraPresentation(mode: .manualTool(requestID))
    }

    private func openToolCamera(requestID: UUID) async {
        #if DEBUG && targetEnvironment(simulator)
        let simulated = AICameraUITestSupport.isRequested
        #else
        let simulated = false
        #endif
        // 异步启动任务只属于派发它的请求，旧任务不能接管稍后创建的新请求。
        guard toolCameraRequestID != requestID else { return }
        guard localMediaRequests.beginCameraOpening(requestID: requestID) else { return }
        defer { localMediaRequests.finishCameraOpening(requestID: requestID) }
        // 手动快门包括授权过程都独占自己的 presentation；Agent 请求不能改写它的模式。
        guard cameraPresentation == nil, !manualCameraOpening, !cameraFinishing,
              toolCameraRequestID == nil, localMediaRequests.manualPhotoRequestID == nil else {
            localMediaRequests.failCamera(.unavailable, requestID: requestID)
            return
        }
        guard model.selected == localMediaRequests.cameraSessionID,
              simulated || AVCaptureDevice.default(for: .video) != nil else {
            localMediaRequests.failCamera(.unavailable, requestID: requestID)
            return
        }
        toolCameraRequestID = requestID
        if !simulated {
            let granted = await requestCameraPermission()
            guard localMediaRequests.cameraRequestID == requestID,
                  toolCameraRequestID == requestID else { return }
            guard granted else {
                localMediaRequests.failCamera(.permissionDenied, requestID: requestID)
                if toolCameraRequestID == requestID { toolCameraRequestID = nil }
                return
            }
        }
        guard localMediaRequests.cameraRequestID == requestID,
              toolCameraRequestID == requestID,
              model.selected == localMediaRequests.cameraSessionID,
              UIApplication.shared.applicationState == .active else {
            await finishToolCamera(reason: "interrupted", requestID: requestID)
            return
        }
        var pendingContext: AIMobileScreenArchiveContext?
        var ownedBinding: AIScreenFrameImportBinding?
        do {
            let snapshot = try model.makeCameraFrameImportSnapshot()
            let (context, _) = try await AIMobileScreenFrameArchive.shared.begin(
                workspace: snapshot.workspace, sessionID: snapshot.ownerSessionID)
            pendingContext = context
            // 授权或磁盘操作期间用户可能已关闭相机，空批次不应留下。
            guard localMediaRequests.cameraRequestID == requestID,
                  toolCameraRequestID == requestID,
                  model.selected == localMediaRequests.cameraSessionID,
                  UIApplication.shared.applicationState == .active else {
                _ = try? await AIMobileScreenFrameArchive.shared.finish(context: context, reason: "canceled")
                try? await AIMobileScreenFrameArchive.shared.discard(context: context)
                return
            }
            let binding = try model.registerCameraFrameArchive(context: context, snapshot: snapshot)
            ownedBinding = binding
            cameraBinding = binding
            localMediaRequests.recordCameraBatch(context.batchID, requestID: requestID)
            cameraPresentation = AICameraPresentation(mode: .timed(requestID))
            try await agentCamera.start(context: context)
            guard localMediaRequests.cameraRequestID == requestID,
                  toolCameraRequestID == requestID, cameraBinding?.id == binding.id else {
                await finishToolCamera(reason: "canceled", requestID: requestID)
                return
            }
            localMediaRequests.markCameraVisible(requestID, batchID: context.batchID)
        } catch is CancellationError {
            await finishToolCamera(reason: "canceled", requestID: requestID)
        } catch {
            // 一旦停止任务接管批次，它负责封存并保留已拍图片；旧任务不得删掉它。
            guard toolCameraRequestID == requestID,
                  localMediaRequests.cameraRequestID == requestID, !cameraFinishing else { return }
            if let context = pendingContext, agentCamera.archiveContext == context {
                _ = try? await agentCamera.stop(reason: "start_failed")
            }
            guard toolCameraRequestID == requestID,
                  localMediaRequests.cameraRequestID == requestID, !cameraFinishing else { return }
            if let context = pendingContext {
                _ = try? await AIMobileScreenFrameArchive.shared.finish(context: context, reason: "start_failed")
                guard toolCameraRequestID == requestID,
                      localMediaRequests.cameraRequestID == requestID, !cameraFinishing else { return }
                if let binding = ownedBinding {
                    try? await model.discardScreenFrameArchive(binding)
                } else { try? await AIMobileScreenFrameArchive.shared.discard(context: context) }
            }
            guard toolCameraRequestID == requestID,
                  localMediaRequests.cameraRequestID == requestID, !cameraFinishing else { return }
            if let batchID = pendingContext?.batchID {
                localMediaRequests.failFinishedCameraArchive(batchID: batchID)
            }
            model.showTransientError(error.localizedDescription)
            if cameraBinding?.id == ownedBinding?.id { cameraBinding = nil }
            localMediaRequests.failCamera(.failed, requestID: requestID)
            toolCameraRequestID = nil
            dismissToolCamera(requestID: requestID)
        }
    }

    /// 先停止并封存同一批次，再在原会话展示可选图片；迟到的重复关闭不重复执行。
    private func finishToolCamera(reason: String, requestID: UUID) async {
        guard !cameraFinishing, toolCameraRequestID == requestID else { return }
        cameraFinishing = true
        defer { cameraFinishing = false }
        if localMediaRequests.cameraRequestID == requestID {
            _ = localMediaRequests.closeCamera(in: localMediaRequests.cameraSessionID)
        }
        guard let binding = cameraBinding else {
            toolCameraRequestID = nil
            dismissToolCamera(requestID: requestID)
            return
        }
        do {
            let batch = try await agentCamera.stop(reason: reason)
            guard toolCameraRequestID == requestID, cameraBinding?.id == binding.id else { return }
            if let batch {
                localMediaRequests.finishCameraArchive(batchID: binding.id, count: batch.frameCount)
            }
            // 身份或目标会话改变时只保留批次，不向当前会话展示或导入旧图。
            if model.cameraFrameBindingsForCurrentDraft().contains(where: { $0.id == binding.id }) {
                if cameraPresentation?.timedRequestID == requestID { cameraReviewAfterDismiss = binding }
                else { cameraFrameSelection = binding }
            }
        } catch {
            guard toolCameraRequestID == requestID, cameraBinding?.id == binding.id else { return }
            localMediaRequests.failFinishedCameraArchive(batchID: binding.id)
            model.showTransientError("相机图片封存失败：\(error.localizedDescription)")
        }
        cameraBinding = nil
        toolCameraRequestID = nil
        dismissToolCamera(requestID: requestID)
    }

    private func dismissToolCamera(requestID: UUID) {
        // 迟到的定时停止只关闭自己的 presentation，不能关闭下一次手动快门。
        if cameraPresentation?.timedRequestID == requestID { cameraPresentation = nil }
    }

    private func openMusicLibrary() async {
        defer { requestingMusicLibraryAuthorization = false }
        var access = MPMediaLibrary.authorizationStatus()
        if access == .notDetermined {
            access = await withCheckedContinuation { continuation in
                MPMediaLibrary.requestAuthorization { result in
                    continuation.resume(returning: result)
                }
            }
        }
        guard access == .authorized || access == .restricted else {
            localMediaRequests.failMusic(.musicLibraryDenied)
            model.showTransientError("请在系统设置中允许访问媒体与 Apple Music")
            return
        }
        musicLibrarySelectionReceived = false
        showingMusicLibrary = true
    }

    private func requestCameraPermission() async -> Bool {
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized:
            return true
        case .notDetermined:
            return await withCheckedContinuation { continuation in
                AVCaptureDevice.requestAccess(for: .video) { granted in
                    continuation.resume(returning: granted)
                }
            }
        case .denied, .restricted:
            return false
        @unknown default:
            return false
        }
    }

    private func requestPhotoLibraryPermission() async -> Bool {
        switch PHPhotoLibrary.authorizationStatus(for: .readWrite) {
        case .authorized, .limited:
            return true
        case .notDetermined:
            let status = await withCheckedContinuation { continuation in
                PHPhotoLibrary.requestAuthorization(for: .readWrite) { status in
                    continuation.resume(returning: status)
                }
            }
            return status == .authorized || status == .limited
        case .denied, .restricted:
            return false
        @unknown default:
            return false
        }
    }

    /// 按用户选择顺序导入照片或视频，并把可引用附件交给当前会话。
    /// 异步读取期间若会话改变，文件保留在原目标会话，避免误发到新会话。
    private func importSelectedMedia(_ items: [PhotosPickerItem],
                                     to target: AIAttachmentImportTarget) async {
        beginImport(to: target)
        defer { finishImport(to: target) }
        defer { selectedMediaItems = [] }
        var imported: [AIImportedFile] = []
        var failedCount = 0
        for item in items {
            do {
                if item.supportedContentTypes.contains(where: { $0.conforms(to: .movie) }) {
                    guard let picked = try await item.loadTransferable(type: AIPickedVideoTransfer.self)
                    else { failedCount += 1; continue }
                    let temporaryURL = picked.fileURL
                    defer { Task { await Self.removeTemporaryFile(temporaryURL) } }
                    guard let file = model.selectedRemote == nil
                        ? await model.importVideoFile(temporaryURL)
                        : await model.importSessionFile(temporaryURL) else {
                        failedCount += 1
                        continue
                    }
                    imported.append(file)
                    continue
                }
                guard let data = try await item.loadTransferable(type: Data.self),
                      data.count <= 20 * 1024 * 1024
                else {
                    failedCount += 1
                    continue
                }
                let type = item.supportedContentTypes.first { $0.conforms(to: .image) }
                let temporaryURL = try await Self.writeTemporaryImage(
                    data,
                    pathExtension: type?.preferredFilenameExtension ?? "jpg"
                )
                defer { Task { await Self.removeTemporaryFile(temporaryURL) } }
                guard let file = await model.importFile(temporaryURL) else {
                    failedCount += 1
                    continue
                }
                imported.append(file)
            } catch {
                failedCount += 1
            }
        }
        let acceptedCount = model.addAttachments(imported, to: target)
        if failedCount > 0, acceptedCount == imported.count {
            model.showTransientError(
                acceptedCount > 0
                    ? "已导入\(acceptedCount)项，另有\(failedCount)项读取失败"
                    : "所选照片或视频读取失败"
            )
        }
    }

    private func importCapturedImage(_ image: UIImage,
                                     to target: AIAttachmentImportTarget) async {
        beginImport(to: target)
        defer { finishImport(to: target) }
        guard let data = image.jpegData(compressionQuality: 0.9) else {
            model.showTransientError("拍摄图片处理失败")
            return
        }
        do {
            let temporaryURL = try await Self.writeTemporaryImage(data, pathExtension: "jpg")
            defer { Task { await Self.removeTemporaryFile(temporaryURL) } }
            if let file = await model.importFile(temporaryURL) {
                model.addAttachments([file], to: target)
            }
        } catch {
            model.showTransientError("拍摄图片导入失败")
        }
    }

    private func importSelectedFile(_ url: URL, to target: AIAttachmentImportTarget) async {
        beginImport(to: target)
        defer { finishImport(to: target) }
        if target.remoteRoomKey != nil {
            if let file = await model.importSessionFile(url) {
                model.addAttachments([file], to: target)
            }
            return
        }
        let type = UTType(filenameExtension: url.pathExtension)
        if type?.conforms(to: .movie) == true {
            if let file = await model.importVideoFile(url) {
                model.addAttachments([file], to: target)
            }
        } else if let file = await model.importFile(url) {
            if file.isImage { model.addAttachments([file], to: target) }
            else { appendImportedDocument(file, to: target) }
        }
    }

    private func beginImport(to target: AIAttachmentImportTarget) {
        importingTargets[target, default: 0] += 1
    }

    private func finishImport(to target: AIAttachmentImportTarget) {
        let remaining = (importingTargets[target] ?? 1) - 1
        if remaining > 0 { importingTargets[target] = remaining }
        else { importingTargets.removeValue(forKey: target) }
    }

    private var isImportingCurrentSession: Bool {
        let remoteKey = model.selectedRemote.map {
            "remote:\($0.deviceSlot):\($0.loginSessionID):\($0.sessionID)"
        }
        return importingTargets.contains { target, count in
            count > 0 && target.remoteRoomKey == remoteKey &&
                model.attachmentSession(for: target) == snapshot.selected
        }
    }

    private func previewAttachment(_ file: AIImportedFile) {
        guard let url = AIAssistantPathPolicy.resolve(
            file.relativePath, workspacePath: snapshot.workspacePath
        ), FileManager.default.fileExists(atPath: url.path) else {
            model.showTransientError("附件文件已丢失，请移除后重新选择")
            return
        }
        if file.isImage { model.previewImage = AIImagePreview(filePath: url.path) }
        else if file.mimeType.hasPrefix("video/") {
            draftVideoPreview = AIVideoPreviewItem(path: url.path)
        } else { draftFilePreview = url }
    }

    private func appendImportedDocument(_ file: AIImportedFile,
                                        to target: AIAttachmentImportTarget) {
        guard let targetSession = model.attachmentSession(for: target) else {
            model.discardImportedFile(file)
            model.showTransientError("原会话已关闭，请重新选择文件")
            return
        }
        let reference = "请查看工作区文件：\(file.relativePath)"
        if targetSession == model.selected {
            draft += (draft.isEmpty ? "" : "\n") + reference
            focusController.focus()
        } else {
            let previous = model.drafts[targetSession, default: ""]
            model.drafts[targetSession] = previous
                + (previous.isEmpty ? "" : "\n")
                + reference
        }
    }

    nonisolated private static func writeTemporaryImage(
        _ data: Data,
        pathExtension: String
    ) async throws -> URL {
        try await RemoteIMBackgroundWork.file {
            let directory = FileManager.default.temporaryDirectory
                .appendingPathComponent("AIAssistantImports", isDirectory: true)
            try FileManager.default.createDirectory(
                at: directory,
                withIntermediateDirectories: true
            )
            let cleanExtension = pathExtension.trimmingCharacters(in: .whitespacesAndNewlines)
            let target = directory
                .appendingPathComponent(UUID().uuidString)
                .appendingPathExtension(cleanExtension.isEmpty ? "jpg" : cleanExtension)
            try data.write(to: target, options: .atomic)
            return target
        }
    }

    nonisolated private static func removeTemporaryFile(_ url: URL) async {
        _ = try? await RemoteIMBackgroundWork.file {
            try FileManager.default.removeItem(at: url)
        }
    }

    private var composerPrompt: String {
        snapshot.attachments.isEmpty ? "可按住转文字" : "添加说明（可选）"
    }

    private var draftStorageSession: String? {
        if draftSession.isEmpty {
            guard let draftImportTarget else { return "" }
            return model.attachmentSession(for: draftImportTarget)
        }
        return model.isDeletedDraftSession(draftSession) ? nil : draftSession
    }

    private var hasDraftContent: Bool {
        !draft.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ||
            !snapshot.attachments.isEmpty
    }

    private var canSubmitDraft: Bool {
        // 接收新点击只等待本地队列提交，不受上一条附件准备/native 回执等待阻塞。
        snapshot.ready && !isImportingCurrentSession && hasDraftContent
    }

    private var canStartVoiceTranscription: Bool {
        isPressingVoice || (
            snapshot.ready && draft.isEmpty
        )
    }

    private func submitDraft(_ submittedText: String? = nil) {
        guard canSubmitDraft else { return }
        let sent = submittedText ?? draft
        let sourceSession = draftSession
        let sourceTarget = draftImportTarget
        let restorationGeneration = model.remoteRestoreGeneration
        let submissionIdentity = model.composerSubmissionIdentity
        let receipt = model.enqueueMessage(sent, expectedSession: sourceSession)
        Task {
            guard await receipt.value else { return }
            guard model.composerSubmissionIdentity == submissionIdentity else { return }
            let destination: String? = sourceSession.isEmpty
                ? sourceTarget.flatMap { model.attachmentSession(for: $0) }
                : sourceSession
            if let destination, draftSession == destination, draft == sent { draft = "" }
            if model.drafts[sourceSession] == sent { model.drafts.removeValue(forKey: sourceSession) }
            if let destination, model.drafts[destination] == sent {
                model.drafts.removeValue(forKey: destination)
            }
            if model.remoteRestoreGeneration != restorationGeneration,
               model.remoteRestoreText == sent, draft.isEmpty {
                draft = sent
            }
        }
    }

    private func performComposerEditAction(_ action: ComposerEditAction) {
        composerEditingController.perform(action)
        switch action {
        case .select, .selectAll:
            let next = composerEditingController.menuState()
            composerEditMenuState = next.hasActions ? next : nil
        case .paste, .cut, .copy, .readAloud, .newLine:
            composerEditMenuState = nil
        }
    }

    private func handleVoiceGestureChanged(translation: CGSize, location: CGPoint?) {
        guard canStartVoiceTranscription else { return }
        if !isPressingVoice { beginVoiceTranscription(at: location) }
        guard isPressingVoice else { return }
        let target = transcriptionTarget(for: translation, location: location)
        if transcriptionPresentation.target != target {
            transcriptionPresentation.target = target
        }
    }

    private func beginVoiceTranscription(at gestureOrigin: CGPoint?) {
        guard !isPressingVoice else { return }
        guard draft.isEmpty else { return }
        focusController.dismiss()
        isPressingVoice = true
        voiceBaseDraft = draft
        transcriptionPresentation.prepareForNewSession(
            account: "ai-assistant",
            peer: model.selected,
            gestureOrigin: gestureOrigin
        )
        transcriptionPresentation.target = .send
        AppDiagnosticLog.shared.record(
            level: .info,
            category: "asr",
            event: "gesture-started",
            fields: ["mode": "ai-assistant-transcription"]
                .merging(transcriptionPresentation.diagnosticFields) { _, context in context }
        )
        voiceStartTask = Task { @MainActor in
            do {
                try await speechRecognizer.start(
                    diagnosticFields: transcriptionPresentation.diagnosticFields,
                    tencentCredentialProvider: { try await appState.asrCredential() },
                    aliyunCredentialProvider: { try await appState.aliyunASRCredential() }
                )
                return true
            } catch is CancellationError {
                return false
            } catch {
                // 已取消的领取任务可能稍后才收到网络错误，不覆盖下一轮长按的界面状态。
                guard !Task.isCancelled else { return false }
                isPressingVoice = false
                transcriptionPresentation.reset()
                model.error = "语音转文字失败：\(error.localizedDescription)"
                return false
            }
        }
    }

    /// 收起语音悬浮层并按“发送／编辑／取消”处理转写结果。
    /// 悬浮层先结束显示，文本成功后才交给模型；识别失败保留当前消息列表。
    private func finishVoiceGesture(
        translation: CGSize,
        location: CGPoint?,
        explicitTarget: VoiceTranscriptionTarget? = nil
    ) async {
        guard isPressingVoice else { return }
        isPressingVoice = false
        let target = explicitTarget ?? transcriptionTarget(
            for: translation,
            location: location
        )
        let diagnosticFields = transcriptionPresentation.diagnosticFields
        if target == .cancel {
            AppDiagnosticLog.shared.record(
                level: .info,
                category: "asr",
                event: "gesture-ended",
                fields: ["action": "cancel"]
                    .merging(diagnosticFields) { _, context in context }
            )
            cancelVoiceTranscription(restoresDraft: true)
            return
        }

        let shouldEdit = target == .edit
        transcriptionPresentation.target = shouldEdit ? .finishingEdit : .finishingSend
        AppDiagnosticLog.shared.record(
            level: .info,
            category: "asr",
            event: "gesture-ended",
            fields: ["action": shouldEdit ? "edit" : "send"]
                .merging(diagnosticFields) { _, context in context }
        )
        let startTask = voiceStartTask
        voiceStartTask = nil
        let didStart = await startTask?.value ?? speechRecognizer.isRecognizing
        guard didStart, speechRecognizer.isRecognizing else {
            transcriptionPresentation.reset()
            return
        }
        do {
            let text = try await speechRecognizer.stop()
                .trimmingCharacters(in: .whitespacesAndNewlines)
            if !text.isEmpty {
                transcriptionPresentation.showFinalTranscript(text)
                try? await Task.sleep(for: .milliseconds(240))
            }
            transcriptionPresentation.reset()
            guard !text.isEmpty else {
                model.showTransientError("没有识别到文字")
                return
            }
            if shouldEdit {
                draft = voiceText(base: voiceBaseDraft, transcript: text)
                focusController.focus()
            } else {
                _ = await model.sendMessage(text)
            }
        } catch is CancellationError {
            transcriptionPresentation.reset()
        } catch {
            transcriptionPresentation.reset()
            model.error = "语音转文字失败：\(error.localizedDescription)"
        }
    }

    private func cancelVoiceTranscription(restoresDraft: Bool) {
        let hadActiveTranscription = isPressingVoice || speechRecognizer.isRecognizing || voiceStartTask != nil
        voiceStartTask?.cancel()
        voiceStartTask = nil
        speechRecognizer.cancel()
        isPressingVoice = false
        transcriptionPresentation.reset()
        if restoresDraft, hadActiveTranscription { draft = voiceBaseDraft }
    }

    private func transcriptionTarget(
        for translation: CGSize,
        location: CGPoint?
    ) -> VoiceTranscriptionTarget {
        VoiceTranscriptionHitTest.target(
            translation: translation,
            location: location,
            cancelFrame: transcriptionPresentation.actionFrames[.cancel],
            editFrame: transcriptionPresentation.actionFrames[.edit],
            cancelDragDistance: 120
        )
    }

    private func voiceText(base: String, transcript: String) -> String {
        let cleaned = transcript.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !base.isEmpty, !cleaned.isEmpty else { return base.isEmpty ? cleaned : base }
        let separator = base.last?.isWhitespace == true ? "" : " "
        return base + separator + cleaned
    }
}

private struct AITranscriptionButton: View {
    let isPressing: Bool
    let isCancelling: Bool
    let isEnabled: Bool
    let onChanged: (CGSize) -> Void
    let onEnded: (CGSize) -> Void

    var body: some View {
        Text(title)
            .font(AppTypography.detail.weight(.semibold))
            .foregroundStyle(isEnabled ? Color.primary : Color.secondary)
            .frame(maxWidth: .infinity, minHeight: 44)
            .background(backgroundColor, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
            .overlay(
                RoundedRectangle(cornerRadius: 14, style: .continuous)
                    .stroke(borderColor, lineWidth: isPressing ? 1.5 : 1)
            )
            .contentShape(Rectangle())
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { onChanged($0.translation) }
                    .onEnded { onEnded($0.translation) }
            )
            .allowsHitTesting(isEnabled)
            .accessibilityLabel("按住转文字")
    }

    private var title: String {
        if !isEnabled { return "清空文字后可按住转文字" }
        if isCancelling { return "松开取消" }
        return isPressing ? "松开处理" : "按住 转文字"
    }

    private var backgroundColor: Color {
        if !isEnabled { return Color(uiColor: .secondarySystemBackground) }
        if isCancelling { return Color.red.opacity(0.08) }
        return isPressing ? Color.blue.opacity(0.16) : Color(uiColor: .secondarySystemBackground)
    }

    private var borderColor: Color {
        if isCancelling { return .red }
        return isPressing ? RemoteIMStyle.blue : RemoteIMStyle.border
    }
}

struct AIPermissionCard: View {
    // 付费生成、普通工具授权和真人 H5 验证使用同一闸门，但展示不同的后果。
    // 付费生成每次都确认；H5 链接也只允许本次生成，不提供整会话放行。
    let permission: AIPermission
    @ObservedObject var model: AIAssistantModel
    @State private var showsRawInput = false
    private var fields: [String: Any] {
        guard let data = permission.input.data(using: .utf8),
              let object = try? JSONSerialization.jsonObject(with: data),
              let fields = object as? [String: Any] else { return [:] }
        return fields
    }
    private var isSendText: Bool { permission.tool == "maichat_send_text" }
    private var isRealValidationLink: Bool {
        permission.tool == "ark_assets" && (fields["action"] as? String) == "begin_real_validation"
    }
    private var isPaidGeneration: Bool {
        ["wan_video", "wan_image",
         "seedance_video", "seedream_image",
         "glm_image", "kling_bailian_video", "kling_bailian_image",
         "minimax_video", "minimax_image"]
            .contains(permission.tool) && !isVideoTaskManagement
    }
    private var isVideoTaskManagement: Bool {
        let action = (fields["action"] as? String) ?? ""
        return action == "delete" &&
            ["wan_video", "seedance_video",
             "kling_bailian_video",
             "minimax_video"].contains(permission.tool) ||
            action == "cancel" &&
            ["wan_video", "seedance_video",
             "kling_bailian_video"].contains(permission.tool)
    }
    private var isVideoTaskDelete: Bool { (fields["action"] as? String) == "delete" }
    private var isVideoGeneration: Bool {
        ["wan_video", "seedance_video",
         "kling_bailian_video",
         "minimax_video"].contains(permission.tool)
    }
    private var isRevision: Bool {
        (fields["action"] as? String) == "revise" ||
        ["edit", "extend"].contains((fields["mode"] as? String) ?? "") ||
        (fields["task_mode"] as? String) == "edit"
    }
    private var paidTitle: String {
        if isVideoGeneration { return isRevision ? "确认编辑视频" : "确认生成视频" }
        return isRevision ? "确认编辑图片" : "确认生成图片"
    }
    private var paidActionTitle: String { isRevision ? "确认编辑" : "确认生成" }
    private var paidProvider: String {
        switch permission.tool {
        case "wan_video", "wan_image": return "万相"
        case "seedance_video": return "Seedance"
        case "glm_image": return "GLM"
        case "kling_bailian_video", "kling_bailian_image": return "可灵 · 百炼"
        case "minimax_video", "minimax_image": return "海螺 / MiniMax"
        default: return "Seedream"
        }
    }
    private var paidRequest: String {
        ((fields["message"] as? String) ?? (fields["prompt"] as? String) ?? "")
            .trimmingCharacters(in: .whitespacesAndNewlines)
    }
    private var paidDetails: String {
        let production = fields["production"] as? [String: Any] ?? [:]
        let content = fields["content"] as? [[String: Any]] ?? []
        var details: [String] = []
        if permission.tool == "seedance_video" {
            let selected = fields["model"] as? String
            switch selected {
            case "doubao-seedance-2-0-fast-260128": details.append("Seedance 2.0 Fast")
            case "doubao-seedance-2-0-mini-260615": details.append("Seedance 2.0 Mini")
            case "doubao-seedance-2-0-260128": details.append("Seedance 2.0")
            case "doubao-seedance-2-5-260628": details.append("Seedance 2.5")
            default:
                details.append((fields["action"] as? String) == "revise" && selected == nil
                               ? "沿用上一任务型号" : (selected ?? "Seedance 2.0 Mini"))
            }
        }
        if permission.tool == "minimax_video", let model = fields["model"] as? String {
            details.append(model == "MiniMax-H3-Max" ? "H3 Max" : "H3")
        }
        if permission.tool == "minimax_image" {
            details.append((fields["model"] as? String) == "image-01-live"
                           ? "Image-01 Live" : "Image-01")
        }
        if permission.tool == "kling_bailian_video" || permission.tool == "kling_bailian_image" {
            let selected = fields["model"] as? String ??
                (permission.tool == "kling_bailian_video"
                 ? "kling/kling-v3-turbo-video-generation"
                 : "kling/kling-v3-image-generation")
            details.append(selected.contains("omni") ? "可灵 V3 Omni" :
                           selected.contains("turbo") ? "可灵 V3 Turbo" : "可灵 V3")
        }
        if let duration = (fields["duration"] as? Int) ?? (production["duration"] as? Int) {
            details.append("\(duration) 秒")
        }
        if let sourceDuration = fields["source_duration_seconds"] as? Double {
            details.append(String(format: "源片 %.1f 秒", sourceDuration))
        }
        if let resolution = (fields["resolution"] as? String) ??
            (production["resolution"] as? String) { details.append(resolution) }
        if let ratio = (fields["ratio"] as? String) ??
            (production["ratio"] as? String) {
            details.append(ratio == "adaptive" ? "按素材比例" : ratio)
        }
        if (fields["virtual_avatar_asset_id"] as? String)?.isEmpty == false {
            details.append("平台虚拟人像")
        }
        if (fields["authorized_portrait_asset_id"] as? String)?.isEmpty == false {
            details.append("已授权真人形象")
        }
        let assetCount = (fields["reference_asset_ids"] as? [String])?.count ?? 0
        if assetCount > 0 { details.append("已登记参考素材 \(assetCount) 张") }
        let strictFrameAssetCount = ["first_frame_asset_id", "last_frame_asset_id"].filter {
            (fields[$0] as? String)?.isEmpty == false
        }.count
        if strictFrameAssetCount > 0 { details.append("已登记严格帧 \(strictFrameAssetCount) 张") }
        if (fields["video_asset_id"] as? String)?.isEmpty == false {
            details.append("已登记视频素材")
        }
        if let groupType = fields["asset_group_type"] as? String {
            details.append(groupType == "LivenessFace" ? "真人授权素材组" : "虚拟素材组")
        }
        if let size = fields["size"] as? String { details.append(size) }
        let imageCount = ((fields["local_image_paths"] as? [String])?.count ??
                          (fields["image_paths"] as? [String])?.count ?? 0)
            + (content.filter { ($0["type"] as? String) == "image_url" }.count)
            + ((((fields["image_path"] as? String)?.isEmpty == false) ||
                ((fields["image_url"] as? String)?.isEmpty == false) ||
                ((fields["first_frame"] as? String)?.isEmpty == false)) ? 1 : 0)
            + ((((fields["last_frame_path"] as? String)?.isEmpty == false) ||
                ((fields["last_frame"] as? String)?.isEmpty == false)) ? 1 : 0)
        if imageCount > 0 { details.append("参考图片 \(imageCount) 张") }
        let videoCount = content.filter { ($0["type"] as? String) == "video_url" }.count
            + ((((fields["reference_video_path"] as? String)?.isEmpty == false) ||
                ((fields["reference_video"] as? String)?.isEmpty == false) ||
                ((fields["video_url"] as? String)?.isEmpty == false) ||
                ((fields["video_path"] as? String)?.isEmpty == false) ||
                ((fields["video_asset_id"] as? String)?.isEmpty == false) ||
                ((fields["video_task_id"] as? String)?.isEmpty == false)) ? 1 : 0)
        if videoCount > 0 { details.append("参考视频 \(videoCount) 个") }
        let audioCount = content.filter { ($0["type"] as? String) == "audio_url" }.count
            + ((((fields["reference_audio_path"] as? String)?.isEmpty == false) ||
                ((fields["reference_audio"] as? String)?.isEmpty == false)) ? 1 : 0)
        if audioCount > 0 { details.append("参考音频 \(audioCount) 个") }
        return details.joined(separator: " · ")
    }
    private var formattedInput: String {
        guard !fields.isEmpty,
              let data = try? JSONSerialization.data(withJSONObject: fields,
                                                       options: [.prettyPrinted, .sortedKeys]),
              let result = String(data: data, encoding: .utf8) else { return permission.input }
        return result
    }
    private var approveTitle: String {
        guard permission.rememberOnApproval == true else { return "允许一次" }
        return (permission.fileCount ?? 0) > 1 ? "允许并记住这些文件" : "允许并记住此文件"
    }
    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Label(isPaidGeneration ? paidTitle
                  : isVideoTaskManagement ? (isVideoTaskDelete ? "删除视频任务记录" : "取消视频任务")
                  : isRealValidationLink ? "创建真人验证链接"
                  : (isSendText ? "确认发送消息" : "确认工具操作"),
                  systemImage: isPaidGeneration ? "sparkles"
                    : isRealValidationLink ? "person.crop.circle" : "hand.raised")
                .font(AppTypography.heading)
                .foregroundStyle(Color.primary)
            if isPaidGeneration {
                let usesRegisteredAsset =
                    (fields["reference_asset_ids"] as? [String])?.isEmpty == false ||
                    ["first_frame_asset_id", "last_frame_asset_id", "video_asset_id"].contains {
                        (fields[$0] as? String)?.isEmpty == false
                    }
                Text(usesRegisteredAsset
                     ? "将使用已登记的参考素材；平台仍会审核。确认后提交给\(paidProvider)，可能消耗模型额度。"
                     : (fields["virtual_avatar_asset_id"] as? String)?.isEmpty == false
                     ? "将使用平台虚拟人像，不保留真实人物长相。确认后提交给\(paidProvider)，可能消耗模型额度。"
                     : (fields["authorized_portrait_asset_id"] as? String)?.isEmpty == false
                         ? "将使用已授权真人形象。确认后提交给\(paidProvider)，可能消耗模型额度。"
                         : "确认后将提交给\(paidProvider)，可能消耗模型额度。")
                    .font(AppTypography.detail)
                    .foregroundStyle(.secondary)
                if !paidRequest.isEmpty {
                    ScrollView {
                        Text(paidRequest)
                            .font(AppTypography.body)
                            .textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    .frame(maxHeight: 150)
                    .padding(12)
                    .background(Color(uiColor: .systemBackground),
                                in: RoundedRectangle(cornerRadius: 10))
                }
                if !paidDetails.isEmpty {
                    Text(paidDetails)
                        .font(AppTypography.detail)
                        .foregroundStyle(.secondary)
                }
            } else if isVideoTaskManagement {
                Text(isVideoTaskDelete
                     ? (permission.tool == "seedance_video"
                        ? "将删除方舟中已结束的任务记录和本地任务索引；已保存的视频文件仍保留。"
                        : "只删除本地已交付的任务记录；云端任务和已保存的视频仍保留。")
                     : "仅排队中的任务可取消。若已开始生成，平台会拒绝，任务仍继续运行。")
                    .font(AppTypography.detail).foregroundStyle(.secondary)
                Text("任务：\((fields["conversation_id"] as? String) ?? "未指定")")
                    .font(AppTypography.metadata).textSelection(.enabled)
            } else if isRealValidationLink {
                Text("将创建一次性 H5 链接，请由照片中的本人打开并完成验证。认证凭证有效期为 30 分钟；完成后才能登记真人素材。")
                    .font(AppTypography.detail)
                    .foregroundStyle(.secondary)
            } else if isSendText, let peer = fields["peer_id"] as? String,
               let text = fields["text"] as? String {
                Label(peer, systemImage: "person.crop.circle")
                    .font(AppTypography.detail).foregroundStyle(.secondary)
                ScrollView {
                    Text(text)
                        .font(AppTypography.body)
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(maxHeight: 220)
                .padding(12)
                .background(Color(uiColor: .systemBackground),
                            in: RoundedRectangle(cornerRadius: 10))
            } else {
                Text(permission.tool).font(AppTypography.detail.bold()).foregroundStyle(.secondary)
                ScrollView {
                    Text(formattedInput)
                        .font(AppTypography.detailMonospaced)
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(maxHeight: 220)
                .padding(12)
                .background(Color(uiColor: .systemBackground),
                            in: RoundedRectangle(cornerRadius: 10))
            }
            if isSendText || isPaidGeneration || isVideoTaskManagement {
                DisclosureGroup(isPaidGeneration ? "查看完整请求" : "查看完整参数",
                                isExpanded: $showsRawInput) {
                    ScrollView {
                        Text(formattedInput)
                            .font(AppTypography.detailMonospaced)
                            .textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    .frame(maxHeight: 180)
                    .padding(.top, 6)
                }
                .font(AppTypography.metadata)
            }
            HStack {
                action(isVideoTaskManagement ? "返回"
                       : isPaidGeneration || isRealValidationLink ? "取消" : "拒绝", "denied")
                action(isPaidGeneration ? paidActionTitle
                       : isVideoTaskManagement ? (isVideoTaskDelete ? "确认删除" : "确认取消任务")
                       : isRealValidationLink ? "生成验证链接" : approveTitle, "approved")
            }
            if !isRealValidationLink && !isVideoTaskManagement &&
                permission.allowForSession != false &&
                permission.rememberOnApproval != true {
                action("本会话允许", "approved_for_session")
            }
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.orange.opacity(0.06), in: RoundedRectangle(cornerRadius: 14))
        .overlay(RoundedRectangle(cornerRadius: 14).stroke(Color.orange.opacity(0.16)))
    }
    private func action(_ title: String, _ decision: String) -> some View {
        Button(title) { Task { await model.action("permission", values: ["id": permission.id, "decision": decision]) } }
            .buttonStyle(.plain).font(AppTypography.metadata.weight(.semibold))
            .foregroundStyle(decision == "denied" ? Color.secondary : Color.orange)
            .padding(.horizontal, 10).frame(height: 32)
            .background(Color(uiColor: .systemBackground), in: RoundedRectangle(cornerRadius: 8))
            .overlay(RoundedRectangle(cornerRadius: 8).stroke(Color.black.opacity(0.08)))
    }
}


private struct MaiSystemMusicPicker: UIViewControllerRepresentable {
    let onPick: (MPMediaItem) -> Void
    let onCancel: () -> Void

    func makeCoordinator() -> Coordinator { Coordinator(onPick: onPick, onCancel: onCancel) }

    func makeUIViewController(context: Context) -> MPMediaPickerController {
        let picker = MPMediaPickerController(mediaTypes: .music)
        picker.delegate = context.coordinator
        picker.allowsPickingMultipleItems = false
        picker.showsCloudItems = true
        picker.showsItemsWithProtectedAssets = true
        picker.prompt = "从音乐资料库选择歌曲"
        return picker
    }

    func updateUIViewController(_ controller: MPMediaPickerController, context: Context) {}

    final class Coordinator: NSObject, MPMediaPickerControllerDelegate {
        let onPick: (MPMediaItem) -> Void
        let onCancel: () -> Void

        init(onPick: @escaping (MPMediaItem) -> Void, onCancel: @escaping () -> Void) {
            self.onPick = onPick
            self.onCancel = onCancel
        }

        func mediaPicker(_ mediaPicker: MPMediaPickerController,
                         didPickMediaItems mediaItemCollection: MPMediaItemCollection) {
            if let item = mediaItemCollection.items.first { onPick(item) }
            else { onCancel() }
        }

        func mediaPickerDidCancel(_ mediaPicker: MPMediaPickerController) { onCancel() }
    }
}

@MainActor
final class MaiLocalMusicPlayer: ObservableObject {
    static let shared = MaiLocalMusicPlayer()

    @Published private(set) var title = ""
    @Published private(set) var artist = ""
    @Published private(set) var sourceName = "本地文件"
    @Published private(set) var duration: Double = 0
    @Published private(set) var position: Double = 0
    @Published private(set) var isPlaying = false
    @Published private(set) var isLoading = false
    @Published private(set) var errorMessage = ""

    private var session: OpaquePointer?
    private var systemPlayer: MPMusicPlayerController?
    private var currentLibraryItem: MPMediaItem?
    private var timer: Timer?
    private var stagedFile: URL?
    private var currentFile: URL?
    private var playGeneration = 0
    private var loadingStartedAt = 0.0

    func openPickedFile(_ source: URL) async throws {
        let name = source.lastPathComponent
        let copied = try await RemoteIMBackgroundWork.file { () -> URL in
            let kind = source.pathExtension.lowercased()
            guard ["mp3", "m4a", "wav"].contains(kind) else {
                throw AIBackendError(message: "请选择 MP3、M4A 或 WAV 音乐文件")
            }
            let scoped = source.startAccessingSecurityScopedResource()
            defer { if scoped { source.stopAccessingSecurityScopedResource() } }
            let sourceBytes = (try? source.resourceValues(forKeys: [.fileSizeKey]))?.fileSize
            if let sourceBytes, sourceBytes > 200_000_000 {
                throw AIBackendError(message: "音乐文件超过 200 MB")
            }
            let directory = FileManager.default.temporaryDirectory
                .appendingPathComponent("MaiChatLocalMusic", isDirectory: true)
            try FileManager.default.createDirectory(at: directory,
                                                    withIntermediateDirectories: true)
            let target = directory.appendingPathComponent(UUID().uuidString)
                .appendingPathExtension(kind)
            try FileManager.default.copyItem(at: source, to: target)
            let bytes = (try target.resourceValues(forKeys: [.fileSizeKey])).fileSize ?? 0
            guard bytes > 0 && bytes <= 200_000_000 else {
                try? FileManager.default.removeItem(at: target)
                throw AIBackendError(message: "音乐文件为空或超过 200 MB")
            }
            return target
        }
        do {
            try Task.checkCancellation()
            try openLocalFile(copied, title: name, ownsCopy: true)
        }
        catch {
            try? FileManager.default.removeItem(at: copied)
            throw error
        }
    }

    func exportLibraryItem(_ item: MPMediaItem) async throws -> URL {
        guard !item.hasProtectedAsset, !item.isCloudItem,
              let source = item.assetURL else {
            throw AIBackendError(message: "这首歌受保护或只在云端，无法交给 FFplay 播放")
        }
        let asset = AVURLAsset(url: source)
        guard let exporter = AVAssetExportSession(asset: asset,
                                                  presetName: AVAssetExportPresetPassthrough) else {
            throw AIBackendError(message: "无法读取这首音乐库歌曲")
        }
        let fileType: AVFileType
        let suffix: String
        if exporter.supportedFileTypes.contains(.m4a) {
            fileType = .m4a
            suffix = "m4a"
        } else if exporter.supportedFileTypes.contains(.mov) {
            fileType = .mov
            suffix = "mov"
        } else {
            throw AIBackendError(message: "此歌曲无法导出供 FFplay 播放")
        }
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("MaiChatLocalMusic", isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let file = directory.appendingPathComponent(UUID().uuidString)
            .appendingPathExtension(suffix)
        exporter.outputURL = file
        exporter.outputFileType = fileType
        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
            exporter.exportAsynchronously { continuation.resume() }
        }
        guard exporter.status == .completed,
              let bytes = (try? file.resourceValues(forKeys: [.fileSizeKey]))?.fileSize,
              bytes > 0, bytes <= 200_000_000 else {
            try? FileManager.default.removeItem(at: file)
            throw AIBackendError(message: "音乐库歌曲导出失败或超过 200 MB")
        }
        return file
    }

    func openAppleMusicItem(_ item: MPMediaItem) async throws {
        var authorization = SKCloudServiceController.authorizationStatus()
        if authorization == .notDetermined {
            authorization = await withCheckedContinuation { continuation in
                SKCloudServiceController.requestAuthorization { result in
                    continuation.resume(returning: result)
                }
            }
        }
        guard authorization == .authorized else {
            throw AIBackendError(message: "请允许 MaiChat 访问 Apple Music")
        }
        if let video = MaiFfplayMobilePlayer.active {
            video.stop()
            video.close()
        }
        stop()
        if let stagedFile { try? FileManager.default.removeItem(at: stagedFile) }
        stagedFile = nil
        currentFile = nil
        currentLibraryItem = item
        title = item.title ?? "Apple Music 歌曲"
        artist = item.artist ?? ""
        sourceName = "Apple Music · 系统播放"
        duration = max(0, item.playbackDuration)
        position = 0
        isLoading = true
        errorMessage = ""
        loadingStartedAt = ProcessInfo.processInfo.systemUptime
        let requestGeneration = playGeneration
        let player = MPMusicPlayerController.applicationQueuePlayer
        systemPlayer = player
        player.setQueue(with: MPMediaItemCollection(items: [item]))
        try await withCheckedThrowingContinuation {
            (continuation: CheckedContinuation<Void, Error>) in
            player.prepareToPlay { error in
                if let error { continuation.resume(throwing: error) }
                else { continuation.resume() }
            }
        }
        guard playGeneration == requestGeneration, systemPlayer === player else {
            throw AIMobileLocalMediaRequestError.canceled
        }
        player.play()
        startTimer()
    }

    func openLocalFile(_ file: URL, title name: String? = nil,
                       ownsCopy: Bool = false, artist: String = "",
                       sourceName: String = "本地文件") throws {
        guard file.isFileURL, FileManager.default.fileExists(atPath: file.path) else {
            throw AIBackendError(message: "音乐文件已移动或删除")
        }
        if let video = MaiFfplayMobilePlayer.active {
            video.stop()
            video.close()
        }
        let audioSession = AVAudioSession.sharedInstance()
        try audioSession.setCategory(.playback, mode: .default)
        try audioSession.setActive(true)
        stop()
        currentLibraryItem = nil
        if let stagedFile, stagedFile != file { try? FileManager.default.removeItem(at: stagedFile) }
        let started = file.path.withCString { maiFfplayAudioStart($0) }
        guard let started else { throw AIBackendError(message: "FFplay 音频播放器无法启动") }
        session = started
        stagedFile = ownsCopy ? file : nil
        currentFile = file
        title = name ?? file.lastPathComponent
        self.artist = artist
        self.sourceName = sourceName
        duration = 0
        position = 0
        isLoading = true
        errorMessage = ""
        startTimer()
    }

    func toggle() {
        if let systemPlayer {
            if systemPlayer.playbackState == .playing { systemPlayer.pause(); isPlaying = false }
            else { systemPlayer.play(); isPlaying = true }
            return
        }
        if let currentLibraryItem {
            Task {
                do { try await openAppleMusicItem(currentLibraryItem) }
                catch { errorMessage = error.localizedDescription }
            }
            return
        }
        if let session {
            var code: Int32 = 0
            if maiFfplayAudioFinished(session, &code) == 0,
               maiFfplayAudioCommand(session, "pause") == 1 {
                isPlaying.toggle()
                return
            }
        }
        if let currentFile {
            do {
                try openLocalFile(currentFile, title: title, ownsCopy: stagedFile == currentFile,
                                  artist: artist, sourceName: sourceName)
            } catch { errorMessage = error.localizedDescription }
        }
    }

    func seek(to seconds: Double) {
        if let systemPlayer, seconds.isFinite, duration > 0 {
            systemPlayer.currentPlaybackTime = min(max(0, seconds), duration)
            position = seconds
            return
        }
        guard let session, seconds.isFinite, duration > 0 else { return }
        let target = Int64(min(max(0, seconds), duration) * 1_000_000)
        if maiFfplayAudioSeekAbsoluteUs(session, target) == 1 { position = seconds }
    }

    func stop() {
        playGeneration += 1
        timer?.invalidate()
        timer = nil
        if let session { maiFfplayAudioStop(session) }
        session = nil
        systemPlayer?.stop()
        systemPlayer = nil
        isPlaying = false
        isLoading = false
        position = 0
    }

    func reportError(_ message: String) {
        isLoading = false
        isPlaying = false
        errorMessage = message
    }

    private func startTimer() {
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refreshPosition() }
        }
    }

    private func refreshPosition() {
        if let systemPlayer {
            let playbackTime = systemPlayer.currentPlaybackTime
            if playbackTime.isFinite { position = min(duration, max(0, playbackTime)) }
            isPlaying = systemPlayer.playbackState == .playing
            if isPlaying { isLoading = false }
            else if isLoading && ProcessInfo.processInfo.systemUptime - loadingStartedAt > 15 {
                isLoading = false
                errorMessage = "Apple Music 暂时无法播放，请检查网络或订阅状态"
            }
            return
        }
        guard let session else { return }
        var state = MaiFfplayPlaybackStatus()
        if maiFfplayAudioStatus(session, &state) == 1, state.duration_us > 0 {
            duration = Double(state.duration_us) / 1_000_000
            position = min(duration, max(0, Double(state.position_us) / 1_000_000))
            isPlaying = state.paused == 0
            isLoading = false
            return
        }
        var code: Int32 = 0
        if maiFfplayAudioFinished(session, &code) == 1 {
            isPlaying = false
            isLoading = false
            if code != 0 { errorMessage = "此音乐无法用 FFplay 播放" }
            else { position = duration }
            timer?.invalidate()
            timer = nil
        }
    }
}

struct MaiLocalMusicPlayerView: View {
    @ObservedObject var music: MaiLocalMusicPlayer
    @State private var scrubPosition: Double = 0
    @State private var isScrubbing = false

    var body: some View {
        VStack(spacing: 16) {
            HStack {
                Text("正在播放").font(AppTypography.metadata.weight(.semibold))
                    .foregroundStyle(.secondary)
                Spacer()
                Text(music.sourceName).font(AppTypography.metadata).foregroundStyle(.secondary)
            }
            Image(systemName: "music.note")
                .font(AppTypography.symbol(size: 44, weight: .light))
                .foregroundStyle(.white)
                .frame(width: 108, height: 108)
                .background(LinearGradient(colors: [Color(red: 0.16, green: 0.57, blue: 0.94),
                                                    Color(red: 0.37, green: 0.31, blue: 0.76)],
                                           startPoint: .topLeading, endPoint: .bottomTrailing),
                            in: RoundedRectangle(cornerRadius: 27))
                .accessibilityHidden(true)
            Text(music.title)
                .font(AppTypography.title.weight(.semibold))
                .lineLimit(2).multilineTextAlignment(.center)
            if !music.artist.isEmpty {
                Text(music.artist).font(AppTypography.detail).foregroundStyle(.secondary)
            }
            if music.isLoading { ProgressView("正在载入…").font(AppTypography.metadata) }
            if !music.errorMessage.isEmpty {
                Text(music.errorMessage).font(AppTypography.metadata).foregroundStyle(.red)
            }
            Slider(value: Binding(
                get: { isScrubbing ? scrubPosition : music.position },
                set: { scrubPosition = $0 }
            ), in: 0...max(1, music.duration), onEditingChanged: { editing in
                isScrubbing = editing
                if !editing { music.seek(to: scrubPosition) }
            })
            HStack {
                Text(clock(music.position))
                Spacer()
                Text(clock(music.duration))
            }.font(AppTypography.metadata).foregroundStyle(.secondary)
            HStack(spacing: 24) {
                Button(music.isPlaying ? "暂停" : "播放") { music.toggle() }
                    .buttonStyle(.borderedProminent)
                Button("停止") { music.stop() }.buttonStyle(.bordered)
            }
            Spacer(minLength: 0)
        }
        .padding(24)
        .presentationDetents([.height(440)])
    }

    private func clock(_ seconds: Double) -> String {
        let total = Int(max(0, seconds))
        return "\(total / 60):" + String(format: "%02d", total % 60)
    }
}

enum AIMobileLocalMediaRequestError: LocalizedError, Sendable, Equatable {
    case unavailable
    case canceled
    case permissionDenied
    case musicLibraryDenied
    case failed

    var errorDescription: String? {
        switch self {
        case .unavailable: return "请先打开创作中心"
        case .canceled: return "已取消操作"
        case .permissionDenied: return "没有摄像头权限"
        case .musicLibraryDenied: return "没有系统音乐库权限"
        case .failed: return "媒体操作失败"
        }
    }
}

@MainActor
final class AIMobileLocalMediaRequests: ObservableObject {
    static let shared = AIMobileLocalMediaRequests()

    @Published private(set) var cameraRequestID: UUID?
    @Published private(set) var manualPhotoRequestID: UUID?
    @Published private(set) var musicRequestID: UUID?
    @Published private(set) var musicPresentationID: UUID?
    private(set) var cameraSessionID = ""
    private(set) var cameraWorkspacePath = ""
    private(set) var manualPhotoSessionID = ""
    private(set) var manualPhotoWorkspacePath = ""
    private(set) var manualPhotoLifecycleGeneration: UInt64 = 0
    private(set) var cameraState = "closed"
    var presenterActive = false
    private var lastCameraSessionID = ""
    private var lastCameraWorkspacePath = ""
    private var lastCameraPath = ""
    private var lastCameraBatchID = ""
    private var lastCameraRequestID: UUID?
    private var cameraStartInFlight = false
    private var cameraFrameCount = 0
    private var musicCompletion: CheckedContinuation<String, Error>?
    private var manualPhotoCompletion: CheckedContinuation<Data, Error>?
    private var manualPhotoOpeningID: UUID?



    func openCamera(in sessionID: String, workspacePath: String) throws -> String {
        guard presenterActive, UIApplication.shared.applicationState == .active,
              cameraRequestID == nil, cameraState != "stopping",
              manualPhotoRequestID == nil, manualPhotoOpeningID == nil,
              !sessionID.isEmpty, !workspacePath.isEmpty else {
            throw AIMobileLocalMediaRequestError.unavailable
        }
        cameraSessionID = sessionID
        cameraWorkspacePath = workspacePath
        lastCameraSessionID = sessionID
        lastCameraWorkspacePath = workspacePath
        lastCameraPath = ""
        lastCameraBatchID = ""
        cameraFrameCount = 0
        cameraState = "opening"
        let id = UUID()
        lastCameraRequestID = id
        cameraStartInFlight = false
        cameraRequestID = id
        Task { @MainActor in
            try? await Task.sleep(for: .seconds(600))
            if cameraRequestID == id { closeCamera(in: sessionID) }
        }
        return cameraState
    }

    /// A single completion belongs to one manual shutter. Cancellation and late
    /// UIKit callbacks use that identity so they cannot finish a later request.
    func requestManualPhoto(in sessionID: String, workspacePath: String) async throws -> Data {
        try Task.checkCancellation()
        guard presenterActive, UIApplication.shared.applicationState == .active,
              cameraRequestID == nil, cameraState != "stopping",
              manualPhotoRequestID == nil, manualPhotoOpeningID == nil,
              !sessionID.isEmpty, !workspacePath.isEmpty else {
            throw AIMobileLocalMediaRequestError.unavailable
        }
        let id = UUID()
        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { continuation in
                manualPhotoCompletion = continuation
                manualPhotoSessionID = sessionID
                manualPhotoWorkspacePath = workspacePath
                manualPhotoRequestID = id
                Task { @MainActor in
                    try? await Task.sleep(for: .seconds(120))
                    failManualPhoto(.canceled, requestID: id)
                }
            }
        } onCancel: {
            Task { @MainActor in self.failManualPhoto(.canceled, requestID: id) }
        }
    }

    func beginManualPhotoOpening(requestID: UUID) -> Bool {
        guard manualPhotoRequestID == requestID, manualPhotoOpeningID == nil else { return false }
        manualPhotoOpeningID = requestID
        return true
    }

    func finishManualPhotoOpening(requestID: UUID) {
        if manualPhotoOpeningID == requestID { manualPhotoOpeningID = nil }
    }

    func finishManualPhoto(_ image: UIImage, requestID: UUID) {
        guard manualPhotoRequestID == requestID, let completion = manualPhotoCompletion else { return }
        guard let data = image.jpegData(compressionQuality: 0.9), !data.isEmpty else {
            failManualPhoto(.failed, requestID: requestID)
            return
        }
        manualPhotoCompletion = nil
        manualPhotoRequestID = nil
        manualPhotoSessionID = ""
        manualPhotoWorkspacePath = ""
        completion.resume(returning: data)
    }

    func failManualPhoto(_ error: AIMobileLocalMediaRequestError, requestID: UUID? = nil) {
        if let requestID, manualPhotoRequestID != requestID { return }
        guard let completion = manualPhotoCompletion else { return }
        manualPhotoCompletion = nil
        manualPhotoRequestID = nil
        manualPhotoSessionID = ""
        manualPhotoWorkspacePath = ""
        completion.resume(throwing: error)
    }

    /// Also invalidates the short save interval after the shutter completed, so
    /// leaving and returning to the same owner cannot revive a canceled result.
    func invalidateManualPhoto() {
        manualPhotoLifecycleGeneration &+= 1
        failManualPhoto(.canceled)
    }

    func markCameraVisible(_ requestID: UUID, batchID: String) {
        guard cameraRequestID == requestID else { return }
        cameraState = "open"
        lastCameraBatchID = batchID
    }

    func recordCameraBatch(_ batchID: String, requestID: UUID) {
        guard cameraRequestID == requestID else { return }
        lastCameraBatchID = batchID
    }

    func beginCameraOpening(requestID: UUID) -> Bool {
        guard cameraRequestID == requestID else { return false }
        cameraStartInFlight = true
        return true
    }

    func updateCameraCount(_ count: Int, requestID: UUID) {
        guard cameraRequestID == requestID else { return }
        cameraFrameCount = count
    }

    @discardableResult
    func closeCamera(in sessionID: String) -> Bool {
        guard cameraRequestID != nil, cameraSessionID == sessionID else { return false }
        cameraRequestID = nil
        cameraSessionID = ""
        cameraWorkspacePath = ""
        // 已创建批次需先排空采样队列并封存，再向 Agent 提供最终数量。
        cameraState = cameraStartInFlight || !lastCameraBatchID.isEmpty ? "stopping" : "closed"
        return true
    }

    /// 取消发生在权限或建档等待时，要等旧启动任务退出才能开放下一轮。
    func finishCameraOpening(requestID: UUID) {
        guard lastCameraRequestID == requestID else { return }
        cameraStartInFlight = false
        guard cameraRequestID == nil,
              cameraState == "stopping", lastCameraBatchID.isEmpty else { return }
        cameraState = "closed"
    }

    func finishCameraArchive(batchID: String, count: Int) {
        guard cameraState == "stopping", lastCameraBatchID == batchID else { return }
        cameraFrameCount = count
        cameraState = "captured"
    }

    func failFinishedCameraArchive(batchID: String) {
        guard lastCameraBatchID == batchID else { return }
        cameraState = "unavailable"
    }

    func cameraStatus(in sessionID: String) -> [String: String] {
        if cameraRequestID != nil, cameraSessionID == sessionID {
            return ["state": cameraState, "batch_id": lastCameraBatchID,
                    "frame_count": String(cameraFrameCount)]
        }
        if lastCameraSessionID == sessionID,
           cameraState == "stopping" || (cameraState == "captured" && !lastCameraBatchID.isEmpty) {
            return ["state": cameraState, "batch_id": lastCameraBatchID,
                    "frame_count": String(cameraFrameCount)]
        }
        let lastPhoto = URL(fileURLWithPath: lastCameraWorkspacePath,
                            isDirectory: true).appendingPathComponent(lastCameraPath)
        if lastCameraSessionID == sessionID, !lastCameraPath.isEmpty,
           FileManager.default.fileExists(atPath: lastPhoto.path) {
            return ["state": "captured", "path": lastCameraPath,
                    "mime_type": "image/jpeg"]
        }
        if lastCameraSessionID == sessionID,
           cameraState == "unavailable" || cameraState == "permission_denied" {
            return ["state": cameraState]
        }
        return ["state": "closed"]
    }

    func finishCamera(path: String) {
        guard cameraRequestID != nil else { return }
        lastCameraSessionID = cameraSessionID
        lastCameraPath = path
        cameraRequestID = nil
        cameraSessionID = ""
        cameraWorkspacePath = ""
        cameraState = "captured"
    }

    func failCamera(_ error: AIMobileLocalMediaRequestError, requestID: UUID? = nil) {
        if let requestID, cameraRequestID != requestID { return }
        guard cameraRequestID != nil else { return }
        cameraRequestID = nil
        cameraSessionID = ""
        cameraWorkspacePath = ""
        cameraState = error == .permissionDenied ? "permission_denied" :
            (error == .unavailable ? "unavailable" : "closed")
    }

    func requestMusicSelection() async throws -> String {
        guard presenterActive, UIApplication.shared.applicationState == .active,
              musicCompletion == nil else { throw AIMobileLocalMediaRequestError.unavailable }
        return try await withCheckedThrowingContinuation { continuation in
            musicCompletion = continuation
            let id = UUID()
            musicRequestID = id
            Task { @MainActor in
                try? await Task.sleep(nanoseconds: 120_000_000_000)
                if musicRequestID == id { failMusic(.canceled) }
            }
        }
    }

    func finishMusic(fileName: String) {
        guard let completion = musicCompletion else { return }
        musicCompletion = nil
        musicRequestID = nil
        completion.resume(returning: fileName)
    }

    func failMusic(_ error: AIMobileLocalMediaRequestError) {
        guard let completion = musicCompletion else { return }
        musicCompletion = nil
        musicRequestID = nil
        completion.resume(throwing: error)
    }

    func showMusic() { musicPresentationID = UUID() }

    func deactivate() {
        presenterActive = false
        // 退出页面与工具关闭共享同一封存流程，不能把仍在异步启动的请求直接标成已关闭。
        if cameraRequestID != nil { _ = closeCamera(in: cameraSessionID) }
        invalidateManualPhoto()
        failMusic(.canceled)
    }
}
