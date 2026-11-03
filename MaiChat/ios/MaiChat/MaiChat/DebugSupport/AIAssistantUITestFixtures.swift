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
enum AIImagePreviewUITestSupport {
    static var isRequested: Bool {
        ProcessInfo.processInfo.arguments.contains("--ai-ui-test") &&
            ProcessInfo.processInfo.environment["MAICHAT_IMAGE_PREVIEW_DELAY_TEST"] == "1"
    }

    // 只延迟显式要求的模拟器回归，放大冷全图解码窗口，不影响缩略图管线和普通 App。
    static func waitForFullImageIfRequested() async {
        guard isRequested else { return }
        try? await Task.sleep(for: .seconds(3))
    }
}

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
                let delayedDecode = anchorTest && AIImagePreviewUITestSupport.isRequested
                let imageSize = delayedDecode ? CGSize(width: 4032, height: 3024) :
                    anchorTest ? CGSize(width: 600, height: 240) : CGSize(width: 300, height: 500)
                let renderer: UIGraphicsImageRenderer
                if delayedDecode {
                    let format = UIGraphicsImageRendererFormat()
                    format.scale = 1
                    renderer = UIGraphicsImageRenderer(size: imageSize, format: format)
                } else {
                    renderer = UIGraphicsImageRenderer(size: imageSize)
                }
                let image = renderer
                    .image { context in
                        if delayedDecode {
                            UIColor(red: 0.04, green: 0.30, blue: 0.94, alpha: 1).setFill()
                        } else { UIColor.systemBlue.setFill() }
                        context.fill(CGRect(origin: .zero, size: imageSize))
                        if delayedDecode {
                            UIColor(red: 1, green: 0.84, blue: 0.02, alpha: 1).setFill()
                            context.fill(CGRect(x: imageSize.width * 0.43, y: 0,
                                                width: imageSize.width * 0.14, height: imageSize.height))
                        }
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

/// Normal Root/composer, isolated owner and deterministic backend receipt. No
/// paid provider is contacted; native tool-batch ordering has separate C ABI tests.
@MainActor
private final class AISteeringUITestProbe: ObservableObject {
    @Published var status = "尚未补充"
    @Published var archiveStatus = "归档未检查"
    @Published private(set) var acceptedByRequestID: [String: String] = [:]
    @Published private(set) var acknowledgementGateWaiting = false
    @Published private(set) var acknowledgementGateReleased = false
    private var acknowledgementGate: CheckedContinuation<Void, Never>?
    private var requests: [AISteeringRequest] = []
    private var archiveUnknownRequests: Set<String> = []

    var holdsFirstAcknowledgement: Bool {
        ProcessInfo.processInfo.environment["MAICHAT_STEERING_HOLD_FIRST_ACK"] == "1"
    }

    func releaseAcknowledgementGate() {
        acknowledgementGateReleased = true
        acknowledgementGateWaiting = false
        let waiting = acknowledgementGate
        acknowledgementGate = nil
        waiting?.resume()
    }

    var ledger: String {
        let fields: [String: Any] = ["attempts": requests.count, "accepted": acceptedByRequestID.count,
            "texts": requests.map(\.text), "request_ids": requests.map(\.requestId),
            "turn_ids": requests.map(\.turnId),
            "pending_entries": AIAssistantModel.shared.outgoingEntries.count,
            "ack_gate_waiting": acknowledgementGateWaiting,
            "ack_gate_released": acknowledgementGateReleased]
        return (try? JSONSerialization.data(withJSONObject: fields, options: .sortedKeys))
            .map { String(decoding: $0, as: UTF8.self) } ?? "invalid-fixture-ledger"
    }

    func submit(_ request: AISteeringRequest) async throws -> String {
        let capturedAccount = AIAssistantModel.shared.screenFrameCurrentAccountID
        requests.append(request)
        let previous = requests.dropLast().last
        status = "已提交 \(requests.count) · \(request.turnId)"
        if holdsFirstAcknowledgement && requests.count == 1 && !acknowledgementGateReleased {
            // 只控制合成宿主回执，不改生产队列时序；XCTest 明确放行前不会产生首条 ACK。
            acknowledgementGateWaiting = true
            await withTaskCancellationHandler {
                await withCheckedContinuation { continuation in
                    if acknowledgementGateReleased { continuation.resume() }
                    else { acknowledgementGate = continuation }
                }
            } onCancel: {
                Task { @MainActor in self.releaseAcknowledgementGate() }
            }
            try Task.checkCancellation()
        } else {
            let requested = Int(ProcessInfo.processInfo.environment["MAICHAT_STEERING_ACK_DELAY_MS"] ?? "") ?? 900
            try await Task.sleep(for: .milliseconds(max(200, min(3000, requested))))
        }
        if request.text.contains("收尾拒绝") {
            status = "收尾拒绝，未启动新任务"
            throw AIBackendError(message: "当前任务正在收尾，补充内容未发送")
        }
        let messageID = acceptedByRequestID[request.requestId] ?? "msg_fixture_user_\(acceptedByRequestID.count + 1)"
        acceptedByRequestID[request.requestId] = messageID
        if request.text.contains("归档未知"), archiveUnknownRequests.insert(request.requestId).inserted {
            status = "归档回执未知"
            throw AIBackendError(message: "模拟归档回执丢失", definitive: false)
        }
        if request.text.contains("重试") && previous == nil {
            status = "送达未知，队列保留"
            throw AIBackendError(message: "模拟回执丢失", definitive: false)
        }
        if request.text.contains("重试") {
            guard previous?.requestId == request.requestId,
                  previous?.turnId == request.turnId else {
                status = "重试编号错误"
                throw AIBackendError(message: "重试编号错误")
            }
        }
        status = "已落库 \(requests.count) · \(request.turnId)"
        let model = AIAssistantModel.shared
        if model.screenFrameCurrentAccountID == capturedAccount && model.selected == request.session &&
            !model.messages.contains(where: { $0.id == messageID }) {
            let now = Int64(Date().timeIntervalSince1970 * 1000)
            model.messages.append(AIMessage(id: messageID, role: "user", created: now, completed: now,
                active: false, parts: [AIPart(id: "part_" + messageID, kind: "text", text: request.text)]))
        }
        return messageID
    }

    func checkArchiveLifecycle() async {
        archiveStatus = "归档检查中"
        let model = AIAssistantModel.shared
        let archive = AIMobileScreenFrameArchive.shared
        let session = model.selected
        let workspace = FileManager.default.temporaryDirectory
            .appendingPathComponent("steering-archive-" + UUID().uuidString, isDirectory: true)
        do {
            try checkImportCachePolicy()
            let context = try await archive.prepareScreenArchiveUITest(workspace: workspace, sessionID: session)
            let listing = try await archive.list(workspace: workspace, sessionID: session, batchID: context.batchID)
            guard let frame = listing.frames.first else { throw AIBackendError(message: "真实采样帧缺失") }
            let references = try await archive.retainSelected(frameIDs: [frame.id], context: context,
                usageSessionID: "draft:" + UUID().uuidString)
            guard let reference = references.first else { throw AIBackendError(message: "原图引用缺失") }
            model.applyUITestMutation(.workspace(workspace.path))
            model.applyUITestMutation(.attachments([session: [AIImportedFile(
                relativePath: reference.relativePath, mimeType: "image/png", isImage: true,
                archiveReference: reference)]], firstMessageSessionID: session))
            guard await model.sendMessage("归档收尾拒绝", expectedSession: session) else {
                throw AIBackendError(message: "拒绝测试未保存到本地队列")
            }
            _ = try await waitForOutgoingState(.failed, text: "归档收尾拒绝")
            try await archive.discard(context: context)
            _ = try await archive.releaseSession(workspace: workspace, sessionID: session)
            guard FileManager.default.fileExists(atPath: reference.fileURL.path) else {
                throw AIBackendError(message: "拒绝后仍在本地队列的原图被清理")
            }
            let nextReferences = try await archive.retainSelected(frameIDs: [frame.id], context: context,
                usageSessionID: "draft:" + UUID().uuidString)
            guard let next = nextReferences.first else { throw AIBackendError(message: "下一条截图引用缺失") }
            model.applyUITestMutation(.attachments([session: [AIImportedFile(
                relativePath: next.relativePath, mimeType: "image/png", isImage: true,
                archiveReference: next)]], firstMessageSessionID: session))
            try await model.discardOutgoingMessages(for: session)
            guard await model.sendMessage("归档未知", expectedSession: session) else {
                throw AIBackendError(message: "未知回执测试未保存到本地队列")
            }
            let unknown = try await waitForOutgoingState(.unknown, text: "归档未知")
            let manifest = reference.fileURL.deletingLastPathComponent().appendingPathComponent("manifest.json")
            let retained = try Data(contentsOf: manifest)
            model.retryOutgoingMessage(unknown.id)
            let deadline = Date().addingTimeInterval(5)
            while Date() < deadline && (model.outgoingEntries.contains(where: { $0.id == unknown.id }) ||
                !status.hasPrefix("已落库")) {
                try await Task.sleep(for: .milliseconds(50))
            }
            guard !model.outgoingEntries.contains(where: { $0.id == unknown.id }) else {
                throw AIBackendError(message: "原队列操作未确认接受")
            }
            guard try Data(contentsOf: manifest) == retained,
                  requests.suffix(2).allSatisfy({ $0.images.first?.relativePath == reference.relativePath }),
                  requests.suffix(2).first?.requestId == requests.last?.requestId else {
                throw AIBackendError(message: "幂等重试改了引用、导入路径或操作编号")
            }
            _ = try await archive.releaseSession(workspace: workspace, sessionID: session)
            guard !FileManager.default.fileExists(atPath: reference.fileURL.path) else {
                throw AIBackendError(message: "接受后原图未归属正式会话，清理漏释放")
            }
            archiveStatus = "归档引用与幂等通过"
        } catch { archiveStatus = "归档失败：" + error.localizedDescription }
    }

    private func waitForOutgoingState(_ state: AIOutgoingMessageQueueState,
                                      text: String) async throws -> AIOutgoingMessageQueueEntry {
        let deadline = Date().addingTimeInterval(6)
        while Date() < deadline {
            if let entry = AIAssistantModel.shared.outgoingEntries.first(where: {
                $0.state == state && (try? JSONDecoder().decode(AIOutgoingMessagePayload.self,
                    from: $0.payloadData).text) == text
            }) { return entry }
            try await Task.sleep(for: .milliseconds(50))
        }
        throw AIBackendError(message: "实际队列未进入预期状态")
    }

    /// Exercise the production policy at capacity, including unavailable owner
    /// state. The native snapshot/identity checks stay in the ordinary model path.
    private func checkImportCachePolicy() throws {
        let entries = (0..<AISteeringImportCachePolicy.capacity).map {
            AISteeringImportCacheIdentity(key: "fixture_\($0)", session: "session_\($0 % 4)",
                turnId: "turn_\($0 % 4)", order: UInt64($0), acceptedOrder: UInt64($0))
        }
        var states = Dictionary(uniqueKeysWithValues: (0..<4).map {
            ("session_\($0)", AISteeringImportRuntimeState(busy: true, turnId: "turn_\($0)"))
        })
        guard AISteeringImportCachePolicy.evictableKey(entries, states: states) == nil,
              AISteeringImportCachePolicy.evictableKey(entries, states: [:]) == nil else {
            throw AIBackendError(message: "活动或未知轮次的导入收据被淘汰")
        }
        states["session_1"] = AISteeringImportRuntimeState(busy: false, turnId: "")
        guard AISteeringImportCachePolicy.evictableKey(entries, states: states) == "fixture_1" else {
            throw AIBackendError(message: "未选择最旧的已结束附件轮次")
        }
        states["session_0"] = AISteeringImportRuntimeState(busy: true, turnId: "replacement_turn")
        guard AISteeringImportCachePolicy.evictableKey(entries, states: states) == "fixture_0" else {
            throw AIBackendError(message: "未区分同会话的新旧轮次")
        }
        states["session_0"] = AISteeringImportRuntimeState(busy: true, turnId: "")
        guard AISteeringImportCachePolicy.evictableKey(entries, states: states) == "fixture_1" else {
            throw AIBackendError(message: "未确认轮次的忙会话丢失导入收据")
        }
        let recent = (0..<128).map {
            AISteeringImportCacheIdentity(key: "receipt_\($0)", session: "ended", turnId: "ended_turn",
                order: UInt64($0), acceptedOrder: UInt64($0))
        }
        let ended = ["ended": AISteeringImportRuntimeState(busy: false, turnId: "")]
        guard AISteeringImportCachePolicy.evictableKey(recent, states: ended) == nil else {
            throw AIBackendError(message: "新请求尚未接受就淘汰了最近128条收据")
        }
        let unknown = AISteeringImportCacheIdentity(key: "unknown", session: "ended", turnId: "ended_turn",
            order: 128)
        guard AISteeringImportCachePolicy.evictableKey(recent + [unknown], states: ended) == nil else {
            throw AIBackendError(message: "未确认的新请求挤掉已接受收据")
        }
        let accepted = AISteeringImportCacheIdentity(key: "accepted", session: "ended", turnId: "ended_turn",
            order: 128, acceptedOrder: 128)
        guard AISteeringImportCachePolicy.evictableKey(recent + [accepted], states: ended) == "receipt_0" else {
            throw AIBackendError(message: "新接受请求后未保留最近128条窗口")
        }
        let activeOld = AISteeringImportCacheIdentity(key: "active_old", session: "running", turnId: "running_turn",
            order: 0, acceptedOrder: 0)
        let later = (1...128).map {
            AISteeringImportCacheIdentity(key: "later_\($0)", session: "ended", turnId: "ended_turn",
                order: UInt64($0), acceptedOrder: UInt64($0))
        }
        var combined = ended
        combined["running"] = AISteeringImportRuntimeState(busy: true, turnId: "running_turn")
        guard AISteeringImportCachePolicy.evictableKey([activeOld] + later, states: combined) == nil else {
            throw AIBackendError(message: "旧活动轮或最近128收据丢失")
        }
    }
}

struct AISteeringUITestRoot: View {
    @StateObject private var appState = RemoteIMAppState(loadedSettings: .empty)
    @StateObject private var probe = AISteeringUITestProbe()
    @ObservedObject private var model = AIAssistantModel.shared

    var body: some View {
        RootView()
            .environmentObject(appState)
            .safeAreaInset(edge: .top, spacing: 0) {
                VStack(spacing: 2) {
                    Text(probe.status).font(.caption)
                        .accessibilityIdentifier("steering-receipt")
                    Text("任务状态").font(.caption)
                        .accessibilityIdentifier("steering-task-state")
                        .accessibilityValue("busy:\(model.busy);session:\(model.selected);turn:" +
                            (model.sessions.first { $0.id == model.selected }?.turnId ?? ""))
                    Text("发送记录").font(.caption)
                        .accessibilityIdentifier("steering-ledger")
                        .accessibilityValue(probe.ledger)
                    if probe.holdsFirstAcknowledgement {
                        Button("放行首条回执") { probe.releaseAcknowledgementGate() }
                            .accessibilityIdentifier("steering-release-first-ack")
                            .disabled(!probe.acknowledgementGateWaiting)
                    }
                    HStack {
                        Button("原会话") {
                            Task { await AIAssistantModel.shared.selectConversation("switch-A") }
                        }.accessibilityIdentifier("steering-current")
                        Button("切会话") {
                            Task { await AIAssistantModel.shared.selectConversation("switch-B") }
                        }.accessibilityIdentifier("steering-other")
                        Button("切测试账号") {
                            AIAssistantModel.shared.uiTestSupport.screenFrameAccountID = "fixture-other-owner"
                        }.accessibilityIdentifier("steering-account")
                    }.font(.caption)
                    if ProcessInfo.processInfo.arguments.contains("--ai-steering-archive-ui-test") {
                        Button("验证归档补充") { Task { await probe.checkArchiveLifecycle() } }
                            .accessibilityIdentifier("steering-archive-check")
                        Text(probe.archiveStatus).font(.caption)
                            .accessibilityIdentifier("steering-archive-result")
                    }
                }.padding(4).frame(maxWidth: .infinity).background(.regularMaterial)
            }
            .onAppear {
                appState.masterUserID = "steering-fixture-owner"
                appState.chatState = MasterChatState(ownerUserID: "steering-fixture-owner")
                appState.hasCompletedInitialLogin = true
                AIAssistantModel.shared.installSteeringUITestFixture { request in
                    try await probe.submit(request)
                }
            }
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
            .safeAreaInset(edge: .top, spacing: 0) {
                if AIPrimaryModelUITestSupport.isRequested { AIPrimaryModelUITestStatus() }
            }
            .onAppear {
                let owner = "session-reveal-ui-test"
                appState.masterUserID = owner
                appState.chatState = MasterChatState(ownerUserID: owner)
                appState.hasCompletedInitialLogin = true
                if AIPrimaryModelUITestSupport.isRequested {
                    // session-reveal 路由会跳过普通页面 appear；这里显式进入真实准备与轮询链。
                    // 不安装内存会话或生成替身，open/configure/send 仍由正式后端完成。
                    AIAssistantModel.shared.uiTestSupport.screenFrameAccountID = owner
                    AIAssistantModel.shared.appear()
                } else { AIAssistantModel.shared.installSessionSwitchUITestFixture() }
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
            arguments.contains("--ai-steering-ui-test") ||
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
