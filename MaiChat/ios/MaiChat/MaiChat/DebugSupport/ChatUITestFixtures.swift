// 测试宿主与素材仅供 Debug 使用，正式页面不在本文件实现。
#if DEBUG
import AVFoundation
import AVKit
import CoreTransferable
import CryptoKit
import ImageIO
import MaiChatCore
import Photos
import PhotosUI
import QuickLook
import QuartzCore
import SwiftUI
import UIKit
import UniformTypeIdentifiers
import WebKit

#if targetEnvironment(simulator)
@MainActor
struct GroupChatUITestRoot: View {
    @StateObject private var appState: RemoteIMAppState

    init() {
        let state = RemoteIMAppState(loadedSettings: StoredRemoteIMSettings(
            sdkAppID: nil, masterUserID: "sim-group-owner",
            contacts: [], reconnectOnLaunch: false
        ))
        state.configureGroupUITestFixture()
        _appState = StateObject(wrappedValue: state)
    }

    var body: some View {
        RootView()
            .environmentObject(appState)
            .overlay(alignment: .topLeading) {
                VStack(spacing: 5) {
                    Button("退群") {
                        guard let groupID = appState.selectedGroupID ?? appState.groups.first?.groupID
                        else { return }
                        appState.dropGroupForUITest(groupID: groupID)
                    }
                    .accessibilityIdentifier("fixture-drop-group")
                    Button("切号") { appState.switchOwnerForUITest() }
                        .accessibilityIdentifier("fixture-switch-owner")
                }
                .font(.system(size: 11, weight: .semibold))
                .padding(6)
                .background(Color.yellow.opacity(0.9),
                            in: RoundedRectangle(cornerRadius: 8))
                .padding(.leading, 4)
                .padding(.top, 100)
            }
    }
}

struct ChatAddFriendMenuUITestRoot: View {
    @State private var activeContact: RemoteIMContact?
    @State private var activeGroup: RemoteIMGroup?
    @State private var isShowingAddContact = false
    @State private var isShowingQRScanner = false
    @State private var pendingScannedFriendID: String?
    @StateObject private var appState = RemoteIMAppState(
        loadedSettings: StoredRemoteIMSettings(
            sdkAppID: nil, masterUserID: "im-add-friend-menu-test",
            contacts: [], reconnectOnLaunch: false
        )
    )

    var body: some View {
        ZStack {
            ChatView(
                activeContact: $activeContact,
                activeGroup: $activeGroup,
                showRemoteDesktop: {},
                showCreateGroup: {},
                showAddContact: {
                    appState.newContactUserID = ""
                    isShowingAddContact = true
                },
                showScanQRCode: {
                    isShowingQRScanner = true
                },
                bottomBar: AnyView(Text("消息页签").frame(height: 48))
            )

            if isShowingAddContact {
                AddContactDialog(isPresented: $isShowingAddContact)
                    .zIndex(10)
            }
        }
        .environmentObject(appState)
        .background(RemoteIMStyle.panelBackground, ignoresSafeAreaEdges: [])
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
    }
}

struct ChatLandscapeUITestRoot: View {
    @State private var activeContact: RemoteIMContact?
    @State private var activeGroup: RemoteIMGroup?
    @State private var installedLargeHistory = false
    @State private var expandedHistoryReady = false
    @State private var databaseHistoryReady = false
    @StateObject private var appState = RemoteIMAppState(
        loadedSettings: StoredRemoteIMSettings(
            sdkAppID: nil, masterUserID: "im-layout-test",
            contacts: [RemoteIMContact(userID: "im-test-peer", displayName: "横屏测试联系人")],
            reconnectOnLaunch: false
        )
    )

    var body: some View {
        ChatView(activeContact: $activeContact, activeGroup: $activeGroup,
                 showRemoteDesktop: {}, showCreateGroup: {},
                 showAddContact: {},
                 showScanQRCode: {},
                 bottomBar: AnyView(Text("消息页签").frame(height: 48)))
            .environmentObject(appState)
            .overlay(alignment: .topLeading) {
                if expandedHistoryReady {
                    Text("IM 历史已就绪")
                        .font(.caption2)
                        .accessibilityIdentifier("im-history-expanded-ready")
                }
                if databaseHistoryReady {
                    Text("IM 已加载 \(appState.visibleMessages(with: "im-test-peer").count) 条")
                        .font(.caption2)
                        .accessibilityIdentifier("im-history-page-count")
                }
            }
            .onAppear {
                let searchFocus = ProcessInfo.processInfo.arguments.contains("--im-search-focus-ui-test")
                guard ProcessInfo.processInfo.arguments.contains("--im-large-history-ui-test") ||
                        searchFocus,
                      !installedLargeHistory else { return }
                installedLargeHistory = true
                // Each UI test owns fresh synthetic history; a previous SQLite
                // run must not merge duplicate text rows into this fixture.
                let store = LocalChatHistoryStore()
                try? store.purgeAccount(ownerUserID: "im-layout-test")
                let contact = RemoteIMContact(userID: "im-test-peer", displayName: "横屏测试联系人")
                let messages = (0..<2544).map { index in
                    RemoteIMMessage(
                        fromUserID: contact.userID,
                        toUserID: "im-layout-test",
                        text: "IM 历史消息 \(index)\n第二行内容",
                        direction: .incoming,
                        status: .sent,
                        createdAt: Date(timeIntervalSince1970: TimeInterval(index))
                    )
                }
                appState.chatState = MasterChatState(
                    ownerUserID: "im-layout-test", contacts: [contact],
                    messages: ProcessInfo.processInfo.environment["MAICHAT_IM_HISTORY_DATABASE"] == "1"
                        ? [] : messages)
                if ProcessInfo.processInfo.environment["MAICHAT_IM_HISTORY_DATABASE"] == "1" {
                    try? store.makePersistence().persist(mutations: [
                        .upsert(messages, sdkAppID: nil, ownerUserID: "im-layout-test")
                    ])
                    Task {
                        await appState.loadInitialMessages(with: contact.userID)
                        databaseHistoryReady = true
                    }
                    return
                }
                if searchFocus {
                    let hit = LocalChatHistorySearchHit(peerUserID: contact.userID,
                                                        message: messages[100])
                    _ = appState.openMessageSearchHit(hit)
                    activeContact = contact
                }
            }
            .onChange(of: activeContact?.userID) { userID in
                guard ProcessInfo.processInfo.environment["MAICHAT_IM_HISTORY_EXPANDED_PAGES"] == "1",
                      let userID else { return }
                expandedHistoryReady = false
                Task {
                    for _ in 0..<8 {
                        await appState.loadEarlierMessages(with: userID)
                    }
                    expandedHistoryReady = true
                }
            }
    }
}

#endif

#if targetEnvironment(simulator)
// 设计审查专用：用真实播放器和创作中心消息列表呈现语音悬浮层，不调用收费服务。
struct VoiceTranscriptionConversationPreviewRoot: View {
    @StateObject private var presentation = VoiceTranscriptionPresentation()

    var body: some View {
        GeometryReader { viewport in
            let dockHeight = min(max(viewport.size.height * 0.62, 320), 560)
            ZStack { AIAssistantView() }
                .padding(.top, dockHeight)
                .overlay {
                    GeometryReader { geometry in
                        let previewVideo = ProcessInfo.processInfo.environment["MAICHAT_VOICE_DESIGN_VIDEO"]
                            .flatMap { FileManager.default.fileExists(atPath: $0) ? $0 : nil }
                            ?? Bundle.main.url(forResource: "ffplay-sample", withExtension: "mp4")?.path
                        if let previewVideo {
                            MaiFfplayVideoScreen(path: previewVideo) {}
                                .frame(width: geometry.size.width, height: dockHeight)
                                .position(x: geometry.size.width / 2, y: dockHeight / 2)
                        }
                    }
                }
        }
        .task {
            AIAssistantModel.shared.installHistoryUITestFixture()
            presentation.prepareForNewSession(account: "preview", peer: "preview")
            presentation.onCancel = { presentation.reset() }
            presentation.onEdit = { presentation.reset() }
            presentation.target = .send
            presentation.updateLiveText("我想从第十秒开始播放这个视频，再看一下人物动作。",
                                        sessionID: 1)
        }
        .onDisappear { presentation.reset() }
    }
}

struct RemoteIMImageFullscreenUITestRoot: View {
    @StateObject private var appState = RemoteIMAppState(loadedSettings: .empty)
    @State private var preview: PresentedRemoteIMImage?
    @State private var imagePath = ""
    @State private var secondImagePath = ""
    @State private var sourceFrameState = "尚未打开"

    var body: some View {
        ZStack {
        ChatNavigationHost(
            root: AnyView(Color.white),
            detail: AnyView(
                VStack(spacing: 0) {
                    ScrollView {
                        VStack {
                            Color.clear.frame(height: 900)
                            if !imagePath.isEmpty {
                                ImageBubbleContent(
                                    attachment: RemoteIMImageAttachment(localFilePath: imagePath,
                                                                         width: 300, height: 500),
                                    previewImage: { imageSize, sourceSnapshot, thumbnailImage in
                                        sourceFrameState = sourceSnapshot?.frame.isEmpty == false
                                            ? "有效" : "无效"
                                        preview = PresentedRemoteIMImage(
                                            item: RemoteIMImagePreviewItem(id: UUID(),
                                                                           localFilePath: imagePath),
                                            imageSize: imageSize,
                                            sourceSnapshot: sourceSnapshot,
                                            thumbnailImage: thumbnailImage,
                                            sequence: testSequence()
                                        )
                                    }
                                )
                            }
                            Color.clear.frame(height: 1000)
                        }
                        .frame(maxWidth: .infinity)
                    }
                    Text(sourceFrameState)
                        .accessibilityIdentifier("im-source-frame-state")
                }
                .environmentObject(appState)
                .background(Color.white)
                .background {
                    RemoteIMImagePresentationHost(presentation: $preview,
                                                  close: closePreview)
                }
            ),
            selectionID: "im-image-fullscreen-test",
            onPop: {}
        )
        }
        .task { prepareImage() }
    }

    private func testSequence() -> MaiMediaSequence? {
        guard !imagePath.isEmpty, !secondImagePath.isEmpty,
              let videoPath = Bundle.main.url(forResource: "ffplay-seek-sample",
                                                  withExtension: "mp4")?.path else { return nil }
        let first = MaiMediaItem(kind: .image, path: imagePath)
        return MaiMediaSequence(selected: first, items: [
            first,
            MaiMediaItem(kind: .image, path: secondImagePath),
            MaiMediaItem(kind: .video, path: videoPath)
        ])
    }

    private func prepareImage() {
        guard imagePath.isEmpty else { return }
        let image = UIGraphicsImageRenderer(size: CGSize(width: 300, height: 500))
            .image { context in
                UIColor.systemBlue.setFill()
                context.fill(CGRect(x: 0, y: 0, width: 300, height: 500))
            }
        let file = FileManager.default.temporaryDirectory
            .appendingPathComponent("im-image-fullscreen-ui-test.png")
        guard let data = image.pngData(),
              (try? data.write(to: file)) != nil else { return }
        imagePath = file.path
        let second = UIGraphicsImageRenderer(size: CGSize(width: 300, height: 500))
            .image { context in
                UIColor.systemOrange.setFill()
                context.fill(CGRect(x: 0, y: 0, width: 300, height: 500))
            }
        let secondFile = FileManager.default.temporaryDirectory
            .appendingPathComponent("im-image-fullscreen-ui-test-second.png")
        guard let secondData = second.pngData(),
              (try? secondData.write(to: secondFile)) != nil else { return }
        secondImagePath = secondFile.path
    }

    private func closePreview() {
        preview = nil
    }
}

struct VoiceTranscriptionDockUITestRoot: View {
    @StateObject private var presentation = VoiceTranscriptionPresentation()
    @State private var inputTapped = false
    @State private var actionTapped = ""

    var body: some View {
        GeometryReader { geometry in
            VStack(spacing: 0) {
                Color.black
                    .frame(height: geometry.size.height * 0.56)
                    .accessibilityIdentifier("voice-test-video")
                VStack {
                    Spacer()
                    Text(actionTapped).accessibilityIdentifier("voice-test-action")
                    Button(inputTapped ? "输入已触达" : "底部输入") {
                        inputTapped = true
                    }
                    .accessibilityIdentifier("voice-test-input")
                    .padding(.bottom, 24)
                }
                .frame(maxWidth: .infinity)
                .frame(height: geometry.size.height * 0.44)
                .background(Color.white)
            }
        }
        .task {
            presentation.onCancel = {
                actionTapped = "已取消"
                presentation.reset()
            }
            presentation.onEdit = {
                actionTapped = "已编辑"
                presentation.reset()
            }
            presentation.target = .send
            presentation.updateLiveText(
                String(repeating: "正在识别你说的话，文字应始终显示在视频下方。", count: 8),
                sessionID: 1)
        }
    }
}
#endif
#endif
