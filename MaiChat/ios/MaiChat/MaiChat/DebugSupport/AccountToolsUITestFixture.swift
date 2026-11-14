// 隔离账号工具验收：正常宿主、图片规范化和 SDK 确认流程，不修改真实账号或访问线上服务。
#if DEBUG && targetEnvironment(simulator)
import Foundation
import MaiChatCore
import SwiftUI
import UIKit
import Darwin

// 仅显式 file-cancel 自有样本安装的门闩，不改变普通队列或发布构建。
final class FileSendCopyUITestGate: @unchecked Sendable {
    static let shared = FileSendCopyUITestGate()
    private let condition = NSCondition()
    private var armed = false
    private var released = false
    private var copied: URL?
    func arm() { condition.lock(); armed = true; released = false; copied = nil; condition.unlock() }
    func copiedURL() -> URL? { condition.lock(); defer { condition.unlock() }; return copied }
    func release() { condition.lock(); released = true; armed = false; condition.broadcast(); condition.unlock() }
    func pauseIfArmed(_ url: URL) {
        condition.lock()
        defer { condition.unlock() }
        guard armed else { return }
        copied = url
        let deadline = Date().addingTimeInterval(4)
        while !released { if !condition.wait(until: deadline) { break } }
    }
}

@MainActor
enum FileSendPreparedUITestGate {
    static var armed = false
    static var copiedURL: URL?
    private static var continuation: CheckedContinuation<Void, Never>?
    static func arm() { armed = true; copiedURL = nil }
    static func pauseIfArmed(_ url: URL) async {
        guard armed else { return }
        await withCheckedContinuation { value in copiedURL = url; continuation = value }
    }
    static func release() {
        armed = false
        let value = continuation
        continuation = nil
        value?.resume()
    }
}

private actor AccountTestAvatarUploader: MaiAccountAvatarUploading {
    var shouldDelay = false
    private var starts = 0
    func setDelayed(_ value: Bool) { shouldDelay = value }
    func startedCount() -> Int { starts }
    func upload(jpeg: Data, accountID: String, request: MaiAvatarMutationRequest) async throws -> String {
        guard !jpeg.isEmpty, jpeg.count <= 2_000_000 else { throw MaiChatAvatarError.invalidImage }
        starts += 1
        if shouldDelay { try await Task.sleep(for: .milliseconds(500)) }
        try request.checkActive()
        return "https://ichat.life/maichat/avatars/test_account/2026-11-03/123e4567-e89b-42d3-a456-426614174000.jpg"
    }
}

@MainActor
private final class AccountTestIMClient: RemoteIMClient {
    var onIncomingText: ((IncomingRemoteIMText) -> Void)?
    var onIncomingActivity: ((IncomingRemoteIMActivity) -> Void)?
    var onIncomingVoice: ((IncomingRemoteIMVoice) -> Void)?
    var onIncomingImage: ((IncomingRemoteIMImage) -> Void)?
    var onIncomingFile: ((IncomingRemoteIMFile) -> Void)?
    var onIncomingVideo: ((IncomingRemoteIMVideo) -> Void)?
    var onPresenceStatusChanged: (([String: RemoteIMPresenceStatus]) -> Void)?
    var onSelfProfileChanged: ((RemoteIMUserProfile) -> Void)?
    var rejectsAvatar = false
    var avatarCommits = 0
    var avatarDispatches = 0
    struct FileDispatch {
        let peerID: String
        let file: RemoteIMFile
        let bytes: Data
        let caption: String?
    }
    var allowsOwnedFileSend = false
    var rejectsOwnedFileSend = false
    var fileDispatches: [FileDispatch] = []
    func setSelfAvatar(url: String, expectedUserID: String, request: MaiAvatarMutationRequest) async throws {
        guard request.beginSDKMutation() else { throw CancellationError() }
        avatarDispatches += 1
        // 模拟 SDK 回调独立于调用 Task：派发后取消并不能撤回远端已接受的修改。
        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) { continuation.resume() }
        }
        if rejectsAvatar { throw MaiChatAvatarError.sdkRejected }
        request.confirmSDKMutation()
        avatarCommits += 1
    }
    func connect(sdkAppID: Int, userID: String, userSig: String) async throws {}
    func disconnect() async {}
    func sendText(to userID: String, text: String, origin: RemoteIMMessageOrigin,
                  quote: RemoteIMQuote?) async throws -> RemoteIMSendReceipt { throw CancellationError() }
    func sendActivity(to userID: String, signal: RemoteIMActivitySignal) async throws {}
    func sendApprovalDecision(to userID: String, token: String,
                               action: RemoteIMApprovalAction) async throws -> RemoteIMSendReceipt {
        throw CancellationError()
    }
    func sendVoice(to userID: String, recording: RemoteIMVoiceRecording,
                   origin: RemoteIMMessageOrigin) async throws -> RemoteIMSendReceipt { throw CancellationError() }
    func sendImage(to userID: String, image: RemoteIMImageFile, caption: String?, captionAbove: Bool,
                   origin: RemoteIMMessageOrigin) async throws -> RemoteIMSendReceipt { throw CancellationError() }
    func sendVideo(to userID: String, video: RemoteIMVideoFile, caption: String?, captionAbove: Bool,
                   origin: RemoteIMMessageOrigin) async throws -> RemoteIMSendReceipt { throw CancellationError() }
    func sendFile(to userID: String, file: RemoteIMFile, caption: String?, captionAbove: Bool,
                  origin: RemoteIMMessageOrigin) async throws -> RemoteIMSendReceipt {
        guard allowsOwnedFileSend else { throw CancellationError() }
        fileDispatches.append(FileDispatch(peerID: userID, file: file,
            bytes: try Data(contentsOf: file.fileURL), caption: caption))
        if rejectsOwnedFileSend { throw AIBackendError(message: "Owned SDK rejected the file") }
        return RemoteIMSendReceipt(remoteID: "owned-file-receipt-\(fileDispatches.count)",
                                   createdAt: Date(timeIntervalSince1970: 1700000000))
    }
    func deleteContact(userID: String) async throws {}
    func clearHistory(userID: String) async throws {}
    func refreshUserProfiles(userIDs: [String]) async throws -> [RemoteIMUserProfile] { [] }
    func refreshPresenceStatuses(userIDs: [String]) async throws -> [String: RemoteIMPresenceStatus] { [:] }
    func subscribePresenceStatuses(userIDs: [String]) async throws {}
    func updateApplicationBadgeCount(_ count: Int) {}
}

struct AgentFileArtifactUITestRoot: View {
    @StateObject private var state: RemoteIMAppState
    @State private var started = false

    init() {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("owned-file-card-" + UUID().uuidString)
        let defaults = UserDefaults(suiteName: "owned-file-card-" + UUID().uuidString)!
        _state = StateObject(wrappedValue: RemoteIMAppState(settingsStore: LocalSettingsStore(defaults: defaults),
            historyStore: LocalChatHistoryStore(baseDirectoryURL: directory), client: AccountTestIMClient(),
            avatarUploader: AccountTestAvatarUploader(), loadedSettings: .empty))
    }

    var body: some View {
        AIAssistantView().environmentObject(state).onAppear {
            guard !started else { return }
            started = true
            let directory = FileManager.default.temporaryDirectory.appendingPathComponent("owned-file-artifacts-" + UUID().uuidString)
            do {
                try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
                let pdf = directory.appendingPathComponent("本轮报告.pdf")
                let bytes = UIGraphicsPDFRenderer(bounds: CGRect(x: 0, y: 0, width: 320, height: 440)).pdfData { context in
                    context.beginPage()
                    ("Owned file preview" as NSString).draw(at: CGPoint(x: 30, y: 50),
                        withAttributes: [.font: UIFont.systemFont(ofSize: 20)])
                }
                try bytes.write(to: pdf)
                let archive = directory.appendingPathComponent("资料归档.zip")
                try Data("PK-owned-ui-fixture".utf8).write(to: archive)
                AIAssistantModel.shared.installFileArtifactUITestFixture(workspace: directory, files: [pdf, archive])
            } catch {
                AIAssistantModel.shared.showTransientError("自有文件卡片样本准备失败")
            }
        }
    }
}

struct AccountToolsUITestRoot: View {
    @StateObject private var state: RemoteIMAppState
    private let client: AccountTestIMClient
    private let uploader: AccountTestAvatarUploader
    private let queryStore: LocalChatHistoryStore
    private let queryDirectory: URL
    @State private var started = false
    @State private var status = "准备账号工具测试"

    init() {
        let client = AccountTestIMClient(), uploader = AccountTestAvatarUploader()
        let queryDirectory = FileManager.default.temporaryDirectory
            .appendingPathComponent("im-query-contract-" + UUID().uuidString)
        let queryStore = LocalChatHistoryStore(baseDirectoryURL: queryDirectory)
        let defaults = UserDefaults(suiteName: "account-tools-" + UUID().uuidString)!
        _state = StateObject(wrappedValue: RemoteIMAppState(settingsStore: LocalSettingsStore(defaults: defaults),
            historyStore: queryStore, client: client, avatarUploader: uploader, loadedSettings: .empty))
        self.client = client
        self.uploader = uploader
        self.queryStore = queryStore
        self.queryDirectory = queryDirectory
        AIAssistantModel.shared.installSessionSwitchUITestFixture()
    }

    var body: some View {
        RootView().environmentObject(state)
            .overlay(alignment: .bottom) {
                Text(status).font(.caption).padding(12).background(.regularMaterial)
                    .accessibilityIdentifier("account-tools-test-status").padding(.bottom, 110)
            }
            .onAppear {
                guard !started else { return }; started = true
                Task {
                    if ProcessInfo.processInfo.arguments.contains("--im-query-contract-ui-test") {
                        await runIMQueryContract()
                    } else if ProcessInfo.processInfo.environment["MAICHAT_ACCOUNT_TOOLS_CASE"] == "file-send" {
                        if let point = ProcessInfo.processInfo.environment["MAICHAT_FILE_CANCEL_POINT"] {
                            await runOwnedFileCancellation(point: point)
                        } else { await runOwnedFileSendContract() }
                    } else { await run() }
                }
            }
    }

    @MainActor private func check(_ value: Bool, _ message: String) throws {
        if !value { throw AIBackendError(message: message) }
    }
    @MainActor private func call(_ tool: String, _ arguments: [String: Any] = [:]) async -> AIMaiChatHostToolExecution {
        let data = try! JSONSerialization.data(withJSONObject: arguments)
        return await AIMobileHostToolProvider.shared.execute(name: tool,
            argumentsJSON: String(decoding: data, as: UTF8.self))
    }
    @MainActor private func object(_ result: AIMaiChatHostToolExecution) throws -> [String: Any] {
        if result.errorCode != nil { throw AIBackendError(message: result.errorMessage ?? "host failed") }
        let data = Data((result.output ?? "").utf8)
        return try JSONSerialization.jsonObject(with: data) as! [String: Any]
    }

    // 只走正常宿主/普通文件发送与自有 SDK 替身；不连接服务、不创建真实好友或读真实库。
    @MainActor private func runOwnedFileSendContract() async {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("owned-agent-file-send-" + UUID().uuidString)
        defer {
            for dispatch in client.fileDispatches { try? FileManager.default.removeItem(at: dispatch.file.fileURL) }
            try? FileManager.default.removeItem(at: directory)
        }
        do {
            state.configureGroupUITestFixture()
            AIMobileHostToolProvider.shared.appState = state
            client.allowsOwnedFileSend = true
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            AIAssistantModel.shared.installVideoToolTestWorkspace(directory.path)
            guard let peer = state.chatState.contacts.first?.userID else {
                throw AIBackendError(message: "Owned fixture needs a contact")
            }
            let samples: [(String, Data, String)] = [
                ("自有报告.pdf", Data("%PDF-1.4\nowned fixture bytes\n%%EOF".utf8), "application/pdf"),
                ("owned.payload", Data([0, 1, 255, 128, 0, 42]), "application/octet-stream"),
            ]
            for (name, bytes, mime) in samples {
                let source = directory.appendingPathComponent(name)
                try bytes.write(to: source)
                let result = try object(await call("maichat_send_media",
                    ["peer_id": peer, "type": "file", "file_path": source.path, "caption": "Owned file caption"]))
                try check(result["sent"] as? Bool == true && result["message_kind"] as? String == "file",
                          "普通文件没有作为文件消息返回成功")
                guard let dispatch = client.fileDispatches.last else {
                    throw AIBackendError(message: "Normal file SDK was not called")
                }
                try check(dispatch.peerID == peer && dispatch.bytes == bytes && dispatch.file.fileName == name &&
                          dispatch.file.mimeType == mime && dispatch.caption == "Owned file caption",
                          "文件字节、名称、类型、好友或说明丢失")
                try check(dispatch.file.fileURL != source && (try Data(contentsOf: source)) == bytes,
                          "发送没有保留独立持久副本或修改了原文件")
            }
            let count = client.fileDispatches.count
            let empty = directory.appendingPathComponent("empty.zip")
            try Data().write(to: empty)
            for path in [directory.path, empty.path, directory.appendingPathComponent("missing.pdf").path] {
                let rejected = await call("maichat_send_media", ["peer_id": peer, "type": "file", "file_path": path])
                try check(rejected.errorCode != nil && client.fileDispatches.count == count,
                          "目录、空文件或缺失文件进入了 SDK")
            }
            let source = directory.appendingPathComponent(samples[0].0)
            let unknown = await call("maichat_send_media",
                ["peer_id": "owned-non-contact", "type": "file", "file_path": source.path])
            try check(unknown.errorCode != nil && client.fileDispatches.count == count,
                      "非联系人被接受为收件人")
            let canceled = Task { @MainActor in
                await call("maichat_send_media", ["peer_id": peer, "type": "file", "file_path": source.path])
            }
            canceled.cancel()
            let canceledResult = await canceled.value
            try check(canceledResult.errorCode != nil && client.fileDispatches.count == count,
                      "已取消的发送进入了 SDK")
            client.rejectsOwnedFileSend = true
            let failed = await call("maichat_send_media",
                ["peer_id": peer, "type": "file", "file_path": source.path])
            try check(failed.errorCode != nil && client.fileDispatches.count == count + 1,
                      "SDK 明确失败被误报为成功")
            try check(client.fileDispatches.last.map { FileManager.default.fileExists(atPath: $0.file.fileURL.path) } == true,
                      "SDK 已派发失败行的附件被误清理")
            status = "PASS FILE SEND: bytes/name/MIME/caption, validation, cancellation and SDK failure"
            print(status)
        } catch {
            status = "FAIL FILE SEND: \(error.localizedDescription)"
            print(status)
        }
    }

    @MainActor private func runOwnedFileCancellation(point: String) async {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("owned-file-cancel-" + UUID().uuidString)
        var copied: URL?
        defer {
            FileSendCopyUITestGate.shared.release()
            FileSendPreparedUITestGate.release()
            if let copied { try? FileManager.default.removeItem(at: copied) }
            try? FileManager.default.removeItem(at: directory)
        }
        do {
            state.configureGroupUITestFixture()
            AIMobileHostToolProvider.shared.appState = state
            client.allowsOwnedFileSend = true
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            AIAssistantModel.shared.installVideoToolTestWorkspace(directory.path)
            let source = directory.appendingPathComponent("owned.bin")
            let bytes = Data([0, 1, 2, 255])
            try bytes.write(to: source)
            let peer = state.chatState.contacts.first!.userID
            if point == "copy" { FileSendCopyUITestGate.shared.arm() }
            else { FileSendPreparedUITestGate.arm() }
            let send = Task { @MainActor in
                await call("maichat_send_media", ["peer_id": peer, "type": "file", "file_path": source.path])
            }
            for _ in 0..<200 {
                copied = point == "copy" ? FileSendCopyUITestGate.shared.copiedURL() : FileSendPreparedUITestGate.copiedURL
                if copied != nil { break }
                try await Task.sleep(for: .milliseconds(10))
            }
            guard let target = copied else { send.cancel(); throw AIBackendError(message: "Owned cancellation gate was not reached") }
            send.cancel()
            FileSendCopyUITestGate.shared.release()
            FileSendPreparedUITestGate.release()
            let result = await send.value
            try check(result.errorCode != nil && client.fileDispatches.isEmpty, "取消后仍进入 SDK")
            try check(!FileManager.default.fileExists(atPath: target.path), "取消后遗留未派发的文件副本")
            try check(try Data(contentsOf: source) == bytes, "用户原文件被改写")
            status = "PASS FILE CANCEL: " + point
            print(status)
        } catch {
            status = "FAIL FILE CANCEL: " + point + " " + error.localizedDescription
            print(status)
        }
    }

    @MainActor private func blockOwnedMainForBudgetProbe() { Thread.sleep(forTimeInterval: 5.3) }
    @MainActor private func waitOwnedBudgetProbe(_ signal: DispatchSemaphore) -> Bool {
        signal.wait(timeout: .now() + 2) == .success
    }

    // 自有 SQLite 和原宿主 execute 入口复现漏搜/截断，不触真实库、SDK 或模型。
    @MainActor private func runIMQueryContract() async {
        defer { try? FileManager.default.removeItem(at: queryDirectory) }
        let mode = ProcessInfo.processInfo.environment["MAICHAT_IM_QUERY_CASE"] ?? "peer"
        do {
            state.configureGroupUITestFixture()
            let owner = state.chatState.ownerUserID
            let peers = state.chatState.contacts.map(\.userID)
            try check(peers.count >= 2, "need two owned fixture contacts")
            let target = peers[0], noise = peers[1]
            let rows = (0..<205).map { index in
                RemoteIMMessage(fromUserID: noise, toUserID: owner, text: "OWNED_QUERY noise",
                    direction: .incoming, status: .received,
                    createdAt: Date(timeIntervalSince1970: Double(100 + index)))
            } + (0..<3).map { index in
                RemoteIMMessage(id: UUID(uuidString: String(format: "10000000-0000-0000-0000-%012d", index + 1))!,
                    fromUserID: target, toUserID: owner, text: "OWNED_QUERY target",
                    direction: .incoming, status: .received, createdAt: Date(timeIntervalSince1970: 10))
            }
            _ = try queryStore.applyMutations([.upsert(rows, sdkAppID: nil, ownerUserID: owner)])
            let host = AIMobileHostToolProvider()
            host.appState = state
            func invoke(_ name: String, _ args: [String: Any]) async -> AIMaiChatHostToolExecution {
                let data = try! JSONSerialization.data(withJSONObject: args)
                return await host.execute(name: name, argumentsJSON: String(decoding: data, as: UTF8.self))
            }
            if mode == "old-context" {
                AIMobileHostToolProvider.shared.appState = state
                let previousContext = AIMaiChatHostToolCallbackContext()
                state.switchOwnerForUITest()
                // 与普通换号重建 Backend 一样注册新的 callback context；不创建原生 Agent。
                let currentContext = AIMaiChatHostToolCallbackContext()
                defer { withExtendedLifetime(currentContext) {} }
                _ = try queryStore.applyMutations([.upsert([
                    RemoteIMMessage(fromUserID: target, toUserID: state.masterUserID,
                        text: "OWNED_QUERY B private", direction: .incoming, status: .received,
                        createdAt: Date(timeIntervalSince1970: 20))
                ], sdkAppID: nil, ownerUserID: state.masterUserID)])
                let readsBefore = state.persistedIMReadCountForUITesting
                let text = await Task.detached {
                    let context = Unmanaged.passRetained(previousContext)
                    defer { context.release() }
                    return "maichat_search_messages".withCString { name in
                        #"{"query":"OWNED_QUERY"}"#.withCString { arguments in
                            guard let response = aiMaiChatHostToolHandler(context.toOpaque(), name, arguments) else { return "{}" }
                            defer { aiMaiChatHostToolResponseFree(nil, response) }
                            return String(cString: response)
                        }
                    }
                }.value
                let reply = try JSONSerialization.jsonObject(with: Data(text.utf8)) as? [String: Any]
                try check(reply?["ok"] as? Bool == false && reply?["errorCode"] as? String == "canceled",
                          "Retired A context read B: \(text)")
                try check(state.persistedIMReadCountForUITesting == readsBefore,
                          "Retired A context queried B's database")
                let currentReply = await Task.detached {
                    let context = Unmanaged.passRetained(currentContext)
                    defer { context.release() }
                    return "maichat_search_messages".withCString { name in
                        #"{"query":"OWNED_QUERY"}"#.withCString { arguments in
                            guard let response = aiMaiChatHostToolHandler(context.toOpaque(), name, arguments) else { return "{}" }
                            defer { aiMaiChatHostToolResponseFree(nil, response) }
                            return String(cString: response)
                        }
                    }
                }.value
                let currentEnvelope = try JSONSerialization.jsonObject(with: Data(currentReply.utf8)) as? [String: Any]
                try check(currentEnvelope?["ok"] as? Bool == true && currentReply.contains("OWNED_QUERY B private"),
                          "Current B callback context could not query its database")
            } else if mode == "switch-admission" {
                AIMobileHostToolProvider.shared.appState = state
                let admitted = DispatchSemaphore(value: 0)
                let callbackContext = AIMaiChatHostToolCallbackContext()
                callbackContext.queryAdmissionForUITesting = { admitted.signal() }
                let operation = Task.detached { () -> String in
                    let context = Unmanaged.passRetained(callbackContext)
                    defer { context.release() }
                    return "maichat_search_messages".withCString { name in
                        #"{"query":"OWNED_QUERY"}"#.withCString { arguments in
                            guard let response = aiMaiChatHostToolHandler(context.toOpaque(), name, arguments) else { return "{}" }
                            defer { aiMaiChatHostToolResponseFree(nil, response) }
                            return String(cString: response)
                        }
                    }
                }
                // 直到 native 捕获入队身份后才切 B；整个 Main 排队不到五秒。
                try check(waitOwnedBudgetProbe(admitted), "owned callback did not reach admission")
                state.switchOwnerForUITest()
                _ = try queryStore.applyMutations([.upsert([
                    RemoteIMMessage(fromUserID: target, toUserID: state.masterUserID,
                        text: "OWNED_QUERY B private", direction: .incoming, status: .received,
                        createdAt: Date(timeIntervalSince1970: 20))
                ], sdkAppID: nil, ownerUserID: state.masterUserID)])
                let readsBefore = state.persistedIMReadCountForUITesting
                let text = await operation.value
                let reply = try JSONSerialization.jsonObject(with: Data(text.utf8)) as? [String: Any]
                try check(reply?["ok"] as? Bool == false && reply?["errorCode"] as? String == "canceled",
                          "A native call was admitted under B: \(text)")
                try check(state.persistedIMReadCountForUITesting == readsBefore,
                          "A native call queried B's persisted database")
            } else if mode == "main-budget" {
                AIMobileHostToolProvider.shared.appState = state
                let started = DispatchSemaphore(value: 0)
                let operation = Task.detached { () -> (String, Double) in
                    let context = Unmanaged.passRetained(AIMaiChatHostToolCallbackContext())
                    defer { context.release() }
                    guard let name = strdup("maichat_search_messages"),
                          let args = strdup(#"{"query":"OWNED_QUERY","limit":2}"#) else { return ("{}", 100) }
                    defer {
                        memset(name, 0xA5, strlen(name)); free(name)
                        memset(args, 0xA5, strlen(args)); free(args)
                    }
                    let start = DispatchTime.now().uptimeNanoseconds
                    started.signal()
                    let response = aiMaiChatHostToolHandler(context.toOpaque(), name, args)
                    guard let response else { return ("{}", 100) }
                    let text = String(cString: response)
                    aiMaiChatHostToolResponseFree(nil, response)
                    return (text, Double(DispatchTime.now().uptimeNanoseconds - start) / 1_000_000_000)
                }
                try check(waitOwnedBudgetProbe(started), "owned callback did not start")
                // 仅阻塞本次自有模拟器 App，证明 native 截止时间不依赖 MainActor 调度。
                blockOwnedMainForBudgetProbe()
                let (text, elapsed) = await operation.value
                let reply = try JSONSerialization.jsonObject(with: Data(text.utf8)) as? [String: Any]
                try check(reply?["ok"] as? Bool == false && reply?["errorCode"] as? String == "busy",
                          "native callback waited for MainActor and returned stale success after \(elapsed)s")
                try check(elapsed < 5.2, "native queue deadline exceeded: \(elapsed)s")
            } else if mode == "peer" {
                let value = try object(await invoke("maichat_search_messages",
                    ["query": "OWNED_QUERY", "peer_id": target, "limit": 10]))
                try check(value["count"] as? Int == 3, "peer-before-limit expected3 got\(value["count"] ?? "nil")")
                let hits = value["matches"] as? [[String: Any]] ?? []
                try check(hits.allSatisfy { Set($0.keys) == ["id", "peer_id", "sender_id", "text", "kind", "direction", "created_at_ms"] && $0["created_at_ms"] as? String == "10000" }, "query materialized extra fields or non-string timestamp")
                try check(state.chatState.messages.isEmpty, "database query mutated UI memory")
            } else if mode == "page" {
                var args: [String: Any] = ["query": "OWNED_QUERY target", "peer_id": target, "limit": 2]
                let first = try object(await invoke("maichat_search_messages", args))
                try check(first["has_more"] as? Bool == true, "missing has_more on persisted first page")
                guard let cursor = first["next_cursor"] as? String, !cursor.isEmpty else {
                    throw AIBackendError(message: "missing next_cursor")
                }
                args["cursor"] = cursor
                let second = try object(await invoke("maichat_search_messages", args))
                try check(second["count"] as? Int == 1 && second["has_more"] as? Bool == false,
                          "same-time continuation did not finish3 rows")
                args["peer_id"] = noise
                let wrong = await invoke("maichat_search_messages", args)
                try check(wrong.errorCode != nil, "cursor accepted different peer")
            } else if mode == "closed" {
                let path = queryDirectory.appendingPathComponent("messages.sqlite3")
                try FileManager.default.moveItem(at: path, to: queryDirectory.appendingPathComponent("saved.sqlite3"))
                try FileManager.default.createDirectory(at: path, withIntermediateDirectories: false)
                let result = await invoke("maichat_search_messages", ["query": "OWNED_QUERY", "peer_id": target])
                try check(result.errorCode != nil, "unavailable DB returned successful empty matches")
            } else if mode == "get" {
                let value = try object(await invoke("maichat_get_messages", ["peer_id": target, "limit": 2]))
                try check(value["has_more"] as? Bool == true, "get discarded persisted hasEarlier")
                try check(value["coverage"] as? String == "local_persisted", "missing authoritative local scope")
                try check(state.chatState.messages.isEmpty, "get mutated UI memory")
            } else if mode == "owner" {
                let first = try object(await invoke("maichat_search_messages",
                    ["query": "OWNED_QUERY", "peer_id": target, "limit": 2]))
                let cursor = try ownedQueryCursor(first["next_cursor"])
                state.switchOwnerForUITest()
                let changed = await invoke("maichat_search_messages",
                    ["query": "OWNED_QUERY", "peer_id": target, "cursor": cursor])
                try check(changed.errorCode != nil, "old owner cursor was accepted")
            } else if mode == "validation" {
                let value = try object(await invoke("maichat_search_messages", [
                    "query": "OWNED_QUERY", "peer_id": target, "since_ms": "10000", "until_ms": 10000,
                    "kind": "text", "direction": "incoming"
                ]))
                try check(value["count"] as? Int == 3, "inclusive milliseconds/type filters changed")
                let invalid = await invoke("maichat_search_messages", ["query": "OWNED_QUERY", "since_ms": true])
                try check(invalid.errorCode == "invalid_input", "boolean timestamp was accepted")
                let wrongGet = await invoke("maichat_get_messages", ["peer_id": target, "query": "forbidden"])
                try check(wrongGet.errorCode == "invalid_input", "get accepted query parameter")
            } else { throw AIBackendError(message: "unknown owned query case") }
            status = "PASS IM QUERY " + mode
        } catch { status = "FAIL IM QUERY " + mode + ": " + error.localizedDescription }
    }

    private func ownedQueryCursor(_ value: Any?) throws -> String {
        guard let value = value as? String, !value.isEmpty else { throw AIBackendError(message: "missing owned query cursor") }
        return value
    }

    @MainActor private func run() async {
        do {
            let emptyProvider = AIMobileHostToolProvider()
            let guest = try object(await emptyProvider.execute(name: "maichat_get_current_account", argumentsJSON: "{}"))
            try check(guest.count == 1 && guest["logged_in"] as? Bool == false, "退出态暴露了资料")
            state.configureGroupUITestFixture()
            state.maiChatPhoneMasked = "+8613****9876"
            AIMobileHostToolProvider.shared.appState = state
            let directory = FileManager.default.temporaryDirectory
                .appendingPathComponent("account-tools-" + UUID().uuidString).appendingPathComponent("Workspace")
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            defer { try? FileManager.default.removeItem(at: directory.deletingLastPathComponent()) }
            AIAssistantModel.shared.installVideoToolTestWorkspace(directory.path)
            let image = UIGraphicsImageRenderer(size: CGSize(width: 1200, height: 700)).image { context in
                UIColor.systemBlue.setFill(); context.fill(CGRect(x: 0, y: 0, width: 1200, height: 700))
                UIColor.systemYellow.setFill(); context.fill(CGRect(x: 400, y: 0, width: 400, height: 700))
            }
            let original = image.pngData()!, file = directory.appendingPathComponent("avatar.png")
            try original.write(to: file)
            let normalized = try MaiAvatarImagePreparation.jpeg(fileURL: file)
            let decoded = UIImage(data: normalized)!
            try check(decoded.size == CGSize(width: 512, height: 512) && normalized.count <= 2_000_000,
                      "头像未规范化为512JPEG")
            let unchanged = try Data(contentsOf: file)
            try check(unchanged == original, "原图被修改")
            let decoder = JSONDecoder(); decoder.keyDecodingStrategy = .convertFromSnakeCase
            let authUser = try decoder.decode(MaiChatAuthUser.self,
                from: Data(#"{"id":"test_account","im_user_id":"im_owner","nickname":null,"phone_masked":"+8613****9876"}"#.utf8))
            try check(authUser.phoneMasked == "+8613****9876", "auth/me未解析脱敏号码")
            let profile = try object(await call("maichat_get_current_account", ["workspace_path": directory.path]))
            try check(Set(profile.keys) == ["logged_in", "account_id", "im_user_id", "nickname", "phone_masked", "avatar_url"],
                      "账号结果字段不在白名单内")
            try check(profile["phone_masked"] as? String == "+8613****9876" && profile["avatar_url"] is NSNull,
                      "脱敏号码或空头像错误")
            client.rejectsAvatar = true
            let rejected = await call("maichat_set_avatar", ["path": file.path, "workspace_path": directory.path])
            try check(rejected.output == nil && client.avatarCommits == 0 && state.currentAccountProfile().avatarURL == nil,
                      "SDK失败被误报为头像修改完成")
            client.rejectsAvatar = false
            let accepted = try object(await call("maichat_set_avatar", ["path": file.path, "workspace_path": directory.path]))
            try check(Set(accepted.keys) == ["updated", "avatar_url"] && accepted["updated"] as? Bool == true && client.avatarCommits == 1,
                      "头像未等待SDK确认")
            try check(state.currentAccountProfile().avatarURL == accepted["avatar_url"] as? String, "头像缓存没有提交")
            let priorDispatches = client.avatarDispatches
            let dispatched = Task { await call("maichat_set_avatar", ["path": file.path, "workspace_path": directory.path]) }
            for _ in 0..<100 {
                if client.avatarDispatches > priorDispatches { break }
                try await Task.sleep(for: .milliseconds(10))
            }
            try check(client.avatarDispatches > priorDispatches, "SDK尚未派发，无法测试确认后的真实结果")
            let uploadsBeforeConcurrent = await uploader.startedCount()
            let concurrent = await call("maichat_set_avatar", ["path": file.path, "workspace_path": directory.path])
            let uploadsAfterConcurrent = await uploader.startedCount()
            try check(concurrent.errorCode == "busy" && concurrent.output == nil &&
                      client.avatarDispatches == priorDispatches + 1 && uploadsBeforeConcurrent == uploadsAfterConcurrent,
                      "已有SDK修改未完成时，第二个头像请求仍开始上传或派发")
            dispatched.cancel()
            let canceledButPending = await call("maichat_set_avatar", ["path": file.path, "workspace_path": directory.path])
            try check(canceledButPending.errorCode == "busy" && client.avatarDispatches == priorDispatches + 1,
                      "SDK已派发的修改仅因取消就提前释放了头像闸门")
            let confirmedAfterStop = try object(await dispatched.value)
            try check(confirmedAfterStop["updated"] as? Bool == true && client.avatarCommits == 2,
                      "SDK已派发后真实成功被误报为取消")
            await uploader.setDelayed(true)
            var starts = await uploader.startedCount()
            let canceled = Task { await call("maichat_set_avatar", ["path": file.path, "workspace_path": directory.path]) }
            try await waitForUpload(after: starts)
            canceled.cancel()
            let canceledResult = await canceled.value
            try check(canceledResult.errorCode == "canceled" && client.avatarCommits == 2, "进行中的Task取消未阻止SDK写入")
            starts = await uploader.startedCount()
            let internalStop = Task { await call("maichat_set_avatar", ["path": file.path,
                "workspace_path": directory.path, "request_id": "active-request"]) }
            try await waitForUpload(after: starts)
            _ = await call("maichat_cancel_avatar", ["request_id": "active-request"])
            let stopped = await internalStop.value
            try check(stopped.errorCode == "canceled" && client.avatarCommits == 2, "Agent停止通知没有取消正在上传的头像")
            _ = await call("maichat_cancel_avatar", ["request_id": "early-request"])
            let early = await call("maichat_set_avatar", ["path": file.path,
                "workspace_path": directory.path, "request_id": "early-request"])
            try check(early.errorCode == "canceled", "先到取消通知没有阻止随后上传")
            starts = await uploader.startedCount()
            let switched = Task { await call("maichat_set_avatar", ["path": file.path, "workspace_path": directory.path]) }
            try await waitForUpload(after: starts)
            state.masterUserID = "another-account"
            let switchedResult = await switched.value
            try check(switchedResult.errorCode == "canceled" && client.avatarCommits == 2, "切换账号后旧头像仍被提交")
            AIAssistantModel.shared.installVideoToolTestWorkspace(directory.appendingPathComponent("new-owner").path)
            let stale = await call("maichat_get_current_account", ["workspace_path": directory.path])
            try check(stale.errorCode == "canceled" && stale.output == nil, "旧账号工作区读到了新账号资料")
            state.configureGroupUITestFixture()
            let remoteURL = "https://ichat.life/maichat/avatars/test_account/2026-11-03/123e4567-e89b-42d3-a456-426614174001.jpg"
            // 跨端通知沿用刚验证的真实规范 JPEG；正常头像控件命中本机缓存，不请求假线上 URL。
            RemoteIMAvatarImageStore.shared.store(normalized, for: remoteURL)
            client.onSelfProfileChanged?(RemoteIMUserProfile(userID: state.masterUserID,
                displayName: "remote display", avatarURL: remoteURL))
            try check(state.currentAccountProfile().avatarURL == remoteURL, "跨设备本人资料没有更新")
            client.onSelfProfileChanged?(RemoteIMUserProfile(userID: "foreign-account",
                displayName: "other", avatarURL: "https://example.com/other.jpg"))
            try check(state.currentAccountProfile().avatarURL == remoteURL, "他人头像覆盖了本人资料")
            status = "PASS：资料白名单、512头像、SDK确认、并行busy、进行中取消、账号隔离和跨设备资料更新"
        } catch { status = "FAIL：" + error.localizedDescription }
    }

    @MainActor private func waitForUpload(after count: Int) async throws {
        for _ in 0..<100 {
            if await uploader.startedCount() > count { return }
            try await Task.sleep(for: .milliseconds(10))
        }
        throw AIBackendError(message: "上传尚未开始，不能把预先取消当成进行中取消验收")
    }
}
#endif
