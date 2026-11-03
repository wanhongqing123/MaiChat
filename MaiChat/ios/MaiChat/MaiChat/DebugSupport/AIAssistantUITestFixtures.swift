// 测试宿主与素材仅供 Debug 使用，正式页面不在本文件实现。
#if DEBUG
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

#if targetEnvironment(simulator)
struct AIVideoCircleUITestRoot: View {
    @StateObject private var appState = RemoteIMAppState(loadedSettings: .empty)
    @ObservedObject private var circle = MaiVideoCircleModel.shared
    @State private var loginError = ""
    @State private var sampleURL: URL?

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 20) {
                    if !loginError.isEmpty { Text(loginError).foregroundStyle(.red) }
                    if let sample = sampleURL {
                        AIMessageRow(message: sampleMessage(sample, sourced: true),
                                     workspacePath: sample.deletingLastPathComponent().path,
                                     quote: {},
                                     canPublishVideo: circle.availability == .available &&
                                        circle.canPublishFromThisBuild)
                        AIMessageRow(message: sampleMessage(sample, sourced: false),
                                     workspacePath: sample.deletingLastPathComponent().path,
                                     quote: {},
                                     canPublishVideo: circle.availability == .available &&
                                        circle.canPublishFromThisBuild)
                    }
                }
                .padding(20)
            }
            .navigationTitle("创作中心")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    if circle.availability == .available {
                        Button("视频圈") { circle.presentation = .circle }
                            .accessibilityIdentifier("ai-video-circle-entry")
                    }
                }
            }
            .sheet(item: $circle.presentation) { presentation in
                switch presentation {
                case .circle:
                    MaiVideoCircleView { circle.presentation = nil }
                case let .publish(source):
                    MaiVideoCirclePublishView(source: source,
                                              close: { circle.presentation = nil },
                                              published: { circle.presentation = .circle })
                }
            }
            .task {
                let authPort = ProcessInfo.processInfo.environment[
                    "MAICHAT_LOCAL_UI_AUTH_PORT"] ?? "18190"
                guard let sourceEndpoint = URL(string: "http://127.0.0.1:\(authPort)/test/video-circle/source")
                else { loginError = "本机测试地址无效"; return }
                let localSample = FileManager.default.temporaryDirectory
                    .appendingPathComponent("video-circle-ui-sample.mp4")
                do {
                    let (data, response) = try await URLSession.shared.data(from: sourceEndpoint)
                    guard (response as? HTTPURLResponse)?.statusCode == 200,
                          !data.isEmpty else { throw URLError(.badServerResponse) }
                    try data.write(to: localSample, options: .atomic)
                    sampleURL = localSample
                }
                catch { loginError = "本机测试视频无法打开"; return }
                guard let account = ProcessInfo.processInfo.environment["MAICHAT_LOCAL_UI_PHONE"],
                      let password = ProcessInfo.processInfo.environment["MAICHAT_LOCAL_UI_PASSWORD"]
                else { loginError = "本机测试账号未提供"; return }
                do {
                    let session = try await MaiChatAuthClient.shared.loginWithPassword(
                        account: account, password: password, ageConfirmed: true)
                    appState.maiChatAccountID = session.user.id
                    appState.maiChatNickname = session.user.nickname ?? ""
                    await circle.probe()
                } catch { loginError = error.localizedDescription }
            }
        }
        .environmentObject(appState)
    }

    private func sampleMessage(_ sample: URL, sourced: Bool) -> AIMessage {
        var output: [String: Any] = [
            "delivery": "current_ai_session", "path": sample.lastPathComponent,
            "type": "video", "mime_type": "video/mp4", "caption": "本机测试视频"
        ]
        if sourced {
            output["source_task_id"] = "fixture-video-task"
            output["source_tool"] = "wan_video"
        }
        let json = (try? JSONSerialization.data(withJSONObject: output))
            .flatMap { String(data: $0, encoding: .utf8) } ?? "{}"
        let part = AIPart(id: sourced ? "circle-sourced" : "circle-legacy",
                          kind: "tool", text: nil, tool: "agent_send_media",
                          input: nil, output: json, error: nil, state: "completed",
                          path: nil, mimeType: nil)
        return AIMessage(id: part.id, role: "assistant", created: 1,
                         completed: 2, active: false, parts: [part])
    }
}

struct AIImageFullscreenUITestRoot: View {
    @StateObject private var appState = RemoteIMAppState(loadedSettings: .empty)
    var body: some View {
        Group {
            if ProcessInfo.processInfo.environment["MAICHAT_IMAGE_ZOOM_ANCHOR_TEST"] == "1" {
                RootView()
            } else {
                AIAssistantView()
            }
        }
            .environmentObject(appState)
            .onAppear {
                let anchorTest = ProcessInfo.processInfo.environment["MAICHAT_IMAGE_ZOOM_ANCHOR_TEST"] == "1"
                let imageSize = anchorTest ? CGSize(width: 600, height: 240) : CGSize(width: 300, height: 500)
                let image = UIGraphicsImageRenderer(size: imageSize)
                    .image { context in
                        UIColor.systemBlue.setFill()
                        context.fill(CGRect(origin: .zero, size: imageSize))
                    }
                let file = FileManager.default.temporaryDirectory
                    .appendingPathComponent("image-fullscreen-ui-test.png")
                guard let data = image.pngData(),
                      (try? data.write(to: file)) != nil else { return }
                if anchorTest {
                    // 使用正常 RootView 的五个底部入口，覆盖真实父层级和状态栏优先级。
                    let owner = "image-zoom-ui-test"
                    appState.masterUserID = owner
                    appState.chatState = MasterChatState(ownerUserID: owner)
                    appState.hasCompletedInitialLogin = true
                    AIAssistantModel.shared.installImageZoomUITestFixture(file.path)
                } else {
                    AIAssistantModel.shared.previewImage = AIImagePreview(
                        filePath: file.path, imageSize: image.size)
                }
            }
    }
}

struct AIImageMixedUITestRoot: View {
    @StateObject private var appState = RemoteIMAppState(loadedSettings: .empty)

    var body: some View {
        // 使用正式 Root 的分屏播放器和视频回图片路径，避免测试宿主漏掉切换后的标题。
        RootView()
        .environmentObject(appState)
        .task {
            let owner = "image-mixed-ui-test"
            appState.masterUserID = owner
            appState.chatState = MasterChatState(ownerUserID: owner)
            appState.hasCompletedInitialLogin = true
            AIMobileHostToolProvider.shared.appState = appState
            // 测试素材通过环境输入写入当前 App 容器，不依赖每次安装会变化的容器路径。
            var customVideo: URL?
            if let bytes = ProcessInfo.processInfo.environment["MAICHAT_MEDIA_TEST_VIDEO_B64"],
               let data = Data(base64Encoded: bytes),
               let name = ProcessInfo.processInfo.environment["MAICHAT_MEDIA_TEST_VIDEO_NAME"] {
                let file = FileManager.default.temporaryDirectory.appendingPathComponent(
                    URL(fileURLWithPath: name).lastPathComponent)
                guard (try? data.write(to: file)) != nil else { return }
                customVideo = file
            }
            guard let video = customVideo ?? Bundle.main.url(forResource: "ffplay-seek-sample",
                                                               withExtension: "mp4") else { return }
            let image = UIGraphicsImageRenderer(size: CGSize(width: 300, height: 500))
                .image { context in
                    UIColor.systemBlue.setFill()
                    context.fill(CGRect(x: 0, y: 0, width: 300, height: 500))
                }
            let file = FileManager.default.temporaryDirectory
                .appendingPathComponent("ai-image-mixed-ui-test.png")
            guard let data = image.pngData(),
                  (try? data.write(to: file)) != nil else { return }
            let first = MaiMediaItem(kind: .image, path: file.path)
            let sequence = MaiMediaSequence(selected: first, items: [
                first, MaiMediaItem(kind: .video, path: video.path)
            ])
            if ProcessInfo.processInfo.environment["MAICHAT_MEDIA_VIDEO_FIRST"] == "1",
               let videoSequence = sequence.adjacent(1) {
                appState.agentVideoPresentation = MaiFfplayPresentation(sequence: videoSequence)
            } else {
                AIAssistantModel.shared.previewImage = AIImagePreview(
                    filePath: file.path, imageSize: image.size, sequence: sequence)
            }
        }
    }
}

struct AITextMediaUITestRoot: View {
    var body: some View {
        if let video = Bundle.main.url(forResource: "ffplay-sample", withExtension: "mp4") {
            let part = AIPart(id: "mention", kind: "text",
                              text: "视频文件：`ffplay-sample.mp4`", tool: nil,
                              input: nil, output: nil, error: nil, state: nil,
                              path: nil, mimeType: nil)
            let message = AIMessage(id: "text-media", role: "assistant", created: 0,
                                    completed: 1, active: false, parts: [part])
            AIMessageRow(message: message,
                         workspacePath: video.deletingLastPathComponent().path, quote: {})
                .padding(20)
        }
    }
}

struct AIVideoBubbleUITestRoot: View {
    var body: some View {
        if let video = Bundle.main.url(forResource: "ffplay-sample", withExtension: "mp4") {
            let part = AIPart(id: "video", kind: "video", text: nil, tool: nil,
                              input: nil, output: nil, error: nil, state: nil,
                              path: video.lastPathComponent, mimeType: "video/mp4")
            let message = AIMessage(id: "video-message", role: "user", created: 0,
                                    completed: 1, active: false, parts: [part])
            AIMessageRow(message: message,
                         workspacePath: video.deletingLastPathComponent().path, quote: {})
                .padding(20)
        }
    }
}

struct AITranscriptUITestRoot: View {
    private func makeTestPart(_ id: String, kind: String, text: String? = nil,
                      tool: String? = nil) -> AIPart {
        AIPart(id: id, kind: kind, text: text, tool: tool,
               input: tool == nil ? nil : "{}", output: tool == nil ? nil : "done",
               error: nil, state: tool == nil ? nil : "completed",
               path: nil, mimeType: nil)
    }

    var body: some View {
        let parts = [makeTestPart("reasoning-1", kind: "reasoning", text: "检查视频"),
                     makeTestPart("reasoning-2", kind: "reasoning", text: "检查视频")]
            + (0..<4).map { makeTestPart("ffmpeg-\($0)", kind: "tool", tool: "ffmpeg") }
            + (0..<4).map { makeTestPart("view-image-\($0)", kind: "tool", tool: "view_image") }
            + [makeTestPart("answer", kind: "text", text: "处理完成，结果已经准备好。")]
        let message = AIMessage(id: "transcript", role: "assistant", created: 0,
                                completed: 1, active: false, parts: parts)
        AIMessageRow(message: message, workspacePath: "", quote: {})
            .padding(20)
    }
}

struct AIHistoryUITestRoot: View {
    @StateObject private var appState = RemoteIMAppState(loadedSettings: .empty)
    var body: some View {
        AIAssistantView()
            .environmentObject(appState)
            .onAppear { AIAssistantModel.shared.installHistoryUITestFixture() }
    }
}

struct AIWelcomeUITestRoot: View {
    @StateObject private var appState = RemoteIMAppState(loadedSettings: .empty)

    var body: some View {
        AIAssistantView()
            .environmentObject(appState)
            .onAppear { AIAssistantModel.shared.installWelcomeUITestFixture() }
    }
}

struct AISessionSwitchUITestRoot: View {
    @StateObject private var appState = RemoteIMAppState(loadedSettings: .empty)
    var body: some View {
        AIAssistantView()
            .environmentObject(appState)
            .onAppear { AIAssistantModel.shared.installSessionSwitchUITestFixture() }
    }
}

/// 正常 Root 的会话侧滑验收使用内存历史与独立 App 数据，不登录或请求付费模型。
struct AISessionRevealUITestRoot: View {
    @StateObject private var appState = RemoteIMAppState(loadedSettings: .empty)

    var body: some View {
        RootView()
            .environmentObject(appState)
            .onAppear {
                let owner = "session-reveal-ui-test"
                appState.masterUserID = owner
                appState.chatState = MasterChatState(ownerUserID: owner)
                appState.hasCompletedInitialLogin = true
                AIAssistantModel.shared.installSessionSwitchUITestFixture()
            }
    }
}

#if DEBUG && targetEnvironment(simulator)
struct AIBrowserUITestRoot: View {
    @StateObject private var appState = RemoteIMAppState(loadedSettings: .empty)
    @ObservedObject private var browser = AIMobileBrowserTools.shared
    @State private var prepared = false
    @State private var verification = ""

    var body: some View {
        RootView()
            .environmentObject(appState)
            .onAppear {
                guard !prepared else { return }
                prepared = true
                appState.masterUserID = "browser-ui-test"
                appState.chatState = MasterChatState(ownerUserID: "browser-ui-test")
                appState.hasCompletedInitialLogin = true
                AIAssistantModel.shared.installSessionSwitchUITestFixture()
                Task {
                    if ProcessInfo.processInfo.environment["MAICHAT_BROWSER_ORDER_TEST"] == "1" {
                        await AIMobileBrowserTools.shared.prepareOrderUITestPage()
                    } else if ProcessInfo.processInfo.environment["MAICHAT_BROWSER_PRESENTATION_TEST"] == "1" {
                        await AIMobileBrowserTools.shared.preparePresentationUITestPage()
                    } else {
                        if ProcessInfo.processInfo.environment["MAICHAT_BROWSER_FORM_TEST"] == "1" {
                            await AIMobileBrowserTools.shared.runFormUITestFixture()
                        } else {
                            await AIMobileBrowserTools.shared.runUITestFixture()
                        }
                        let file = FileManager.default.temporaryDirectory
                            .appendingPathComponent("maichat-browser-ui-test/result.txt")
                        verification = (try? String(contentsOf: file, encoding: .utf8)) ?? "missing result"
                    }
                }
            }
            .overlay(alignment: .top) {
                if !verification.isEmpty {
                    Text(verification).font(.caption2)
                        .accessibilityIdentifier("browser-fixture-verification-result")
                        .padding(4).background(.regularMaterial)
                }
            }
            .overlay(alignment: .bottomTrailing) {
                if ProcessInfo.processInfo.environment["MAICHAT_BROWSER_ORDER_TEST"] == "1" {
                    VStack(alignment: .trailing) {
                        Button("追加测试消息") {
                            AIAssistantModel.shared.appendBrowserOrderUITestReply()
                            Task { _ = await browser.observePage(sessionID: "switch-A") }
                        }
                        Button("推进测试分页") {
                            for _ in 0..<9 { AIAssistantModel.shared.appendBrowserOrderUITestReply() }
                        }
                        Button("加载测试历史") {
                            Task { await AIAssistantModel.shared.loadOlderMessages() }
                        }
                    }
                    .font(.caption).padding(8).background(.regularMaterial)
                    .padding(.trailing, 12).padding(.bottom, 110)
                }
            }
            .onChange(of: browser.isPresented) { presented in
                if presented && ProcessInfo.processInfo.environment["MAICHAT_BROWSER_TEST_APPROVAL"] == "1" {
                    AIAssistantModel.shared.permissions = [AIPermission(
                        id: "browser-approval", tool: "mobile_browser", input: "{\"action\":\"click\"}")]
                }
            }
    }
}
#endif

struct AIMediaDraftUITestRoot: View {
    @StateObject private var appState = RemoteIMAppState(loadedSettings: .empty)
    var body: some View {
        AIAssistantView()
            .environmentObject(appState)
            .onAppear { AIAssistantModel.shared.installMediaDraftUITestFixture() }
    }
}

struct AIStreamTypingUITestRoot: View {
    @StateObject private var appState = RemoteIMAppState(loadedSettings: .empty)
    var body: some View {
        AIAssistantView()
            .environmentObject(appState)
            .onAppear { AIAssistantModel.shared.installStreamingTypingUITestFixture() }
    }
}
#endif

#if targetEnvironment(simulator)
struct AIPermissionUITestRoot: View {
    private let permission = AIPermission(
        id: "preview", tool: "maichat_send_text",
        input: "{\"peer_id\":\"demo-contact\",\"text\":\"" +
            String(repeating: "A paragraph for approval.\\n", count: 60) + "\"}",
        allowForSession: false, rememberOnApproval: false, fileCount: nil
    )

    var body: some View {
        ScrollView {
            AIPermissionCard(permission: permission, model: .shared)
                .padding(16)
        }
        .background(Color(uiColor: .systemBackground))
    }
}
#endif

#if targetEnvironment(simulator)
struct AILocalMusicUITestRoot: View {
    @ObservedObject private var music = MaiLocalMusicPlayer.shared
    @State private var showing = false

    var body: some View {
        Button("测试本地音乐") {
            do {
                let file = try makeSilentWav()
                try music.openLocalFile(file, title: "测试静音音乐")
                showing = true
            } catch { assertionFailure(error.localizedDescription) }
        }
        .sheet(isPresented: $showing) { MaiLocalMusicPlayerView(music: music) }
    }

    private func makeSilentWav() throws -> URL {
        let file = FileManager.default.temporaryDirectory
            .appendingPathComponent("maichat-local-music-ui-test.wav")
        let sampleBytes = 160_000
        var data = Data("RIFF".utf8)
        func append<T: FixedWidthInteger>(_ value: T) {
            var littleEndian = value.littleEndian
            withUnsafeBytes(of: &littleEndian) { data.append(contentsOf: $0) }
        }
        append(UInt32(36 + sampleBytes))
        data.append(contentsOf: "WAVEfmt ".utf8)
        append(UInt32(16)); append(UInt16(1)); append(UInt16(1))
        append(UInt32(8_000)); append(UInt32(16_000))
        append(UInt16(2)); append(UInt16(16))
        data.append(contentsOf: "data".utf8)
        append(UInt32(sampleBytes))
        data.append(Data(repeating: 0, count: sampleBytes))
        try data.write(to: file, options: .atomic)
        return file
    }
}
#endif

#if targetEnvironment(simulator)
enum AIAssistantUITestRouting {
    static var usesStandaloneFixture: Bool {
        let arguments = ProcessInfo.processInfo.arguments
        return arguments.contains("--app-bars-layout-ui-test") ||
            arguments.contains("--ai-history-ui-test") ||
            arguments.contains("--ai-welcome-ui-test") ||
            arguments.contains("--ai-session-reveal-ui-test") ||
            arguments.contains("--ai-session-switch-ui-test") ||
            arguments.contains("--ai-browser-ui-test") ||
            arguments.contains("--ai-portrait-ui-test") ||
            arguments.contains("--ai-media-draft-ui-test") ||
            arguments.contains("--ai-stream-typing-ui-test") ||
            arguments.contains("--voice-conversation-preview") ||
            arguments.contains("--ai-image-fullscreen-ui-test") ||
            arguments.contains("--ai-image-mixed-ui-test")
    }
}
#endif
#endif
