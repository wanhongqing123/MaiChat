// 文件作用：连接 SwiftUI 创作中心与共享 C++ MaiAgent，维护会话、消息、任务及工作区状态。
// 会话切换先读取目标最近一页，成功后一次性替换可见状态；失败时保留原会话和消息。
// 轮询只合并已持久化的新消息，旧页按需从数据库取回，避免刷新时清空当前画面。
// 云端密钥仅驻留内存；消息持久化和工具调用由 MaiMobileAgent 适配器及核心库完成。
import Foundation
import Darwin
import CoreImage
import CryptoKit
import ImageIO
import MaiChatCore
import Photos
import SwiftUI
import UIKit
import UniformTypeIdentifiers
import Vision

func logAIHistoryEvent(_ message: @autoclosure () -> String) {
    #if DEBUG
    NSLog("[AIHistory] %@", message())
    #endif
}

private func aiMobileAgentSessionChanged(_ sessionID: UnsafePointer<CChar>?) {
    guard let sessionID else { return }
    let id = String(cString: sessionID)
    Task { @MainActor in AIAssistantModel.shared.localSessionChanged(id) }
}

@MainActor private func aiSyncDeviceDisplayName() -> String {
    var system = utsname()
    _ = uname(&system)
    let identifier = withUnsafeBytes(of: &system.machine) { bytes in
        String(decoding: bytes.prefix { $0 != 0 }, as: UTF8.self)
    }
    let model: String
    switch identifier {
    case "iPhone15,2": model = "iPhone 14 Pro"
    case "iPhone15,3": model = "iPhone 14 Pro Max"
    default:
        model = identifier.hasPrefix("iPhone") || identifier.hasPrefix("iPad") ?
            "\(UIDevice.current.model)（\(identifier)）" : UIDevice.current.model
    }
    let name = UIDevice.current.name.trimmingCharacters(in: .whitespacesAndNewlines)
    if name.isEmpty || name == UIDevice.current.model || name == model { return model }
    return String("\(model) · \(name)".prefix(40))
}

struct AIModelSettings: Codable, Sendable, Equatable {
    static let glmChatUrl = "https://open.bigmodel.cn/api/paas/v4"
    static let cloudModels = ["deepseek-flash", "deepseek-v4-pro", "glm-5.3", "glm-5.3-flash"]
    static let deepSeekUrl = "https://api.deepseek.com"

    var baseUrl = AIModelSettings.deepSeekUrl
    var model = "deepseek-flash"
    // 可选字段兼容已有账号设置；本地只持久化目录 ID，沙盒路径在配置时重新解析。
    var modelProvider: String? = nil
    var cloudModel: String? = nil
    var localModelID: String? = nil
    var localProjectorID: String? = nil
    var localContextSize: Int? = nil
    var localMaximumTokens: Int? = nil
    var localThreads: Int? = nil
    var isLocalModel: Bool { modelProvider == "llama" }
    // 旧字段只用于兼容解码；普通主模型自动管理预算，不沿用早期 QA 的短输出限制。
    // 完整移动工具目录首问已超过 23k；输出 0 由核心使用剩余上下文生成到模型结束。
    var effectiveLocalContextSize: Int { 32768 }
    var effectiveLocalMaximumTokens: Int { 0 }
    var effectiveLocalThreads: Int { 2 }
    var effectiveCloudModel: String {
        let saved = cloudModel ?? (isLocalModel ? "deepseek-flash" : model)
        return Self.cloudModels.contains(saved) ? saved : "deepseek-flash"
    }
    var displayName: String {
        if isLocalModel { return "本地模型" }
        switch model {
        case "glm-5.3": return "GLM-5.3"
        case "glm-5.3-flash": return "GLM-5.3-Flash"
        case "deepseek-v4-pro": return "DeepSeek V4 Pro"
        default: return "DeepSeek Flash"
        }
    }

    /// 普通选择只区分云端和随包本地模型；旧文件 ID 仅保留兼容解码，不再要求用户管理文件。
    mutating func selectModelSource(local: Bool) {
        if local {
            if !isLocalModel { cloudModel = effectiveCloudModel }
            modelProvider = "llama"
            localModelID = AILocalModelStore.bundledPrimaryID
            localProjectorID = nil
            model = "local:" + AILocalModelStore.bundledPrimaryID
        } else {
            selectModel(effectiveCloudModel)
        }
    }
    // Optional so settings written before this field existed still decode.
    var reasoningEffort: String? = nil
    var effectiveReasoningEffort: String {
        let value = reasoningEffort ?? "high"
        return ["low", "high", "max"].contains(value) ? value : "high"
    }
    var policy = "on-request"
    var effectiveWire: String {
        isLocalModel ? "local" : (["glm-5.3", "glm-5.3-flash"].contains(model) ? "chat_completions" : "responses")
    }

    mutating func selectModel(_ selected: String) {
        model = selected
        if ["deepseek-flash", "deepseek-v4-pro"].contains(selected) {
            cloudModel = selected; modelProvider = "remote"; baseUrl = Self.deepSeekUrl
        } else if ["glm-5.3", "glm-5.3-flash"].contains(selected) {
            cloudModel = selected; modelProvider = "remote"; baseUrl = Self.glmChatUrl
        }
    }

}

enum AIAssistantWorkspaceLocator {
    private static func baseDirectory() throws -> URL {
        let support = try FileManager.default.url(
            for: .applicationSupportDirectory, in: .userDomainMask,
            appropriateFor: nil, create: true
        )
        let base = support.appendingPathComponent("AIAssistant", isDirectory: true)
        try FileManager.default.createDirectory(at: base, withIntermediateDirectories: true)
        return base
    }

    static func claimLegacyWorkspace(for userID: String) throws {
        guard !userID.isEmpty else { return }
        let base = try baseDirectory()
        let marker = base.appendingPathComponent("legacy-owner-user-id.txt")
        guard !FileManager.default.fileExists(atPath: marker.path),
              FileManager.default.fileExists(
                atPath: base.appendingPathComponent("sessions.sqlite").path
              ) else { return }
        // 旧版本的 AI 数据库没有账号目录；切换前把它归属给当前账号，不移动付费任务文件。
        try Data(userID.utf8).write(to: marker, options: .atomic)
    }

    static func directory(for userID: String) throws -> URL {
        let base = try baseDirectory()
        if userID.isEmpty {
            let guest = base.appendingPathComponent("Guest", isDirectory: true)
            try FileManager.default.createDirectory(at: guest, withIntermediateDirectories: true)
            return guest
        }
        let marker = base.appendingPathComponent("legacy-owner-user-id.txt")
        if let owner = try? String(contentsOf: marker, encoding: .utf8), owner == userID {
            return base
        }
        let digest = SHA256.hash(data: Data(userID.utf8))
            .map { String(format: "%02x", $0) }.joined()
        let account = base.appendingPathComponent("Accounts", isDirectory: true)
            .appendingPathComponent(digest, isDirectory: true)
        try FileManager.default.createDirectory(at: account, withIntermediateDirectories: true)
        return account
    }

    static func removeWorkspace(for userID: String) throws {
        guard !userID.isEmpty else { return }
        let base = try baseDirectory()
        let marker = base.appendingPathComponent("legacy-owner-user-id.txt")
        if let owner = try? String(contentsOf: marker, encoding: .utf8), owner == userID {
            for item in try FileManager.default.contentsOfDirectory(
                at: base, includingPropertiesForKeys: nil
            ) where item.lastPathComponent != "Accounts" && item.lastPathComponent != "Guest" {
                try FileManager.default.removeItem(at: item)
            }
            return
        }
        let digest = SHA256.hash(data: Data(userID.utf8))
            .map { String(format: "%02x", $0) }.joined()
        let account = base.appendingPathComponent("Accounts", isDirectory: true)
            .appendingPathComponent(digest, isDirectory: true)
        if FileManager.default.fileExists(atPath: account.path) {
            try FileManager.default.removeItem(at: account)
        }
    }
}

final class AICloudCredentialStore: @unchecked Sendable {
    static let shared = AICloudCredentialStore()
    private let lock = NSLock()
    private var values: [String: String] = [:]
    private var arkExpiresAt = Date.distantPast
    private var bailianExpiresAt = Date.distantPast
    private var bailianWorkspaceId = ""

    func replaceAccountModelKeys(_ next: [String: String]) {
        lock.lock()
        for provider in ["glm", "deepseek", "minimax"] {
            if let key = next[provider], !key.isEmpty { values[provider] = key }
            else { values.removeValue(forKey: provider) }
        }
        lock.unlock()
    }

    func clearAll() {
        lock.lock()
        values.removeAll()
        arkExpiresAt = .distantPast
        bailianExpiresAt = .distantPast
        bailianWorkspaceId = ""
        lock.unlock()
    }

    func setTemporaryArk(_ key: String, expiresAt: Date) {
        lock.lock()
        values["ark"] = key
        arkExpiresAt = expiresAt
        lock.unlock()
    }

    func clearTemporaryArk() {
        lock.lock()
        values.removeValue(forKey: "ark")
        arkExpiresAt = .distantPast
        lock.unlock()
    }

    func setTemporaryBailian(_ key: String, workspaceId: String, expiresAt: Date) {
        lock.lock()
        values["wan"] = key
        bailianWorkspaceId = workspaceId
        bailianExpiresAt = expiresAt
        lock.unlock()
    }

    func clearTemporaryBailian() {
        lock.lock()
        values.removeValue(forKey: "wan")
        bailianWorkspaceId = ""
        bailianExpiresAt = .distantPast
        lock.unlock()
    }

    func bailianNeedsRefresh() -> Bool {
        lock.lock()
        defer { lock.unlock() }
        return Date().addingTimeInterval(90) >= bailianExpiresAt
    }

    func workspaceId() -> String {
        lock.lock()
        defer { lock.unlock() }
        return Date() < bailianExpiresAt ? bailianWorkspaceId : ""
    }

    func arkNeedsRefresh() -> Bool {
        lock.lock()
        defer { lock.unlock() }
        return Date().addingTimeInterval(90) >= arkExpiresAt
    }

    func apiKey(_ provider: String) -> String {
        lock.lock()
        defer { lock.unlock() }
        if provider == "ark", Date() >= arkExpiresAt { return "" }
        if provider == "wan", Date() >= bailianExpiresAt { return "" }
        return values[provider] ?? ""
    }
}

enum AICloudCredentialSync {
    @MainActor private static var arkRefreshTask: Task<Void, Error>?
    @MainActor private static var arkRetryAfter = Date.distantPast
    @MainActor private static var bailianRefreshTask: Task<Void, Error>?
    @MainActor private static var bailianRetryAfter = Date.distantPast

    @MainActor static func invalidateAccountModelCredentials() {
        arkRefreshTask?.cancel()
        arkRefreshTask = nil
        arkRetryAfter = .distantPast
        bailianRefreshTask?.cancel()
        bailianRefreshTask = nil
        bailianRetryAfter = .distantPast
        AICloudCredentialStore.shared.clearAll()
    }

    static func refreshAccountModelKeysIfAvailable() async throws -> Bool {
        guard let issued = try await MaiChatAuthClient.shared.accountModelKeysIfAvailable()
        else { return false }
        var valid: [String: String] = [:]
        for provider in ["glm", "deepseek", "minimax"] {
            guard let key = issued.apiKeys[provider], !key.isEmpty,
                  key.utf8.count <= 4_096,
                  key.unicodeScalars.allSatisfy({
                      !CharacterSet.controlCharacters.contains($0)
                  }) else { continue }
            valid[provider] = key
        }
        guard !valid.isEmpty else {
            throw AIBackendError(message: "当前账号没有可用的主聊天模型")
        }
        AICloudCredentialStore.shared.replaceAccountModelKeys(valid)
        return true
    }

    @MainActor static func refreshBailianIfNeeded(force: Bool = false) async throws {
        // 后台恢复或进程重启后，先凭手机号会话续领；短期 Key 只写进内存。
        guard AICloudCredentialStore.shared.bailianNeedsRefresh() else { return }
        if let bailianRefreshTask { try await bailianRefreshTask.value; return }
        guard force || Date() >= bailianRetryAfter else { return }
        bailianRetryAfter = Date().addingTimeInterval(30)
        let task = Task {
            guard let issued = try await MaiChatAuthClient.shared.bailianCredentialIfAvailable()
            else { return }
            let expiry = Date(timeIntervalSince1970: TimeInterval(issued.expiresAt))
            guard issued.apiKey.hasPrefix("st-"), issued.apiKey.count >= 11,
                  expiry > Date().addingTimeInterval(30),
                  expiry <= Date().addingTimeInterval(360),
                  issued.wanWorkspaceId.range(
                    of: "^(ws|llm)-[A-Za-z0-9_-]{1,120}$", options: .regularExpression
                  ) != nil else {
                throw AIBackendError(message: "百炼临时凭据无效")
            }
            guard !Task.isCancelled else { return }
            AICloudCredentialStore.shared.setTemporaryBailian(
                issued.apiKey, workspaceId: issued.wanWorkspaceId, expiresAt: expiry
            )
        }
        bailianRefreshTask = task
        defer { bailianRefreshTask = nil }
        try await task.value
    }

    @MainActor static func refreshSeedanceIfNeeded(force: Bool = false) async throws {
        guard AICloudCredentialStore.shared.arkNeedsRefresh() else { return }
        if let arkRefreshTask { try await arkRefreshTask.value; return }
        guard force || Date() >= arkRetryAfter else { return }
        arkRetryAfter = Date().addingTimeInterval(30)
        let task = Task {
            guard let issued = try await MaiChatAuthClient.shared.seedanceCredentialIfAvailable()
            else { return }
            let expiry = Date(timeIntervalSince1970: TimeInterval(issued.expiresAt))
            guard issued.apiKey.count >= 16,
                  expiry > Date().addingTimeInterval(30),
                  expiry <= Date().addingTimeInterval(360) else {
                throw AIBackendError(message: "Seedance 临时凭据无效")
            }
            guard !Task.isCancelled else { return }
            AICloudCredentialStore.shared.setTemporaryArk(issued.apiKey, expiresAt: expiry)
        }
        arkRefreshTask = task
        defer { arkRefreshTask = nil }
        try await task.value
    }

}
struct AISession: Codable, Identifiable, Sendable, Equatable {
    let id: String
    let title: String
    let busy: Bool
    var turnId: String? = nil
    var model: String? = nil
    var reasoningEffort: String? = nil
    var wire: String? = nil
    var displayTitle: String { title == "New session" ? "新对话" : title }
}

struct AIRemoteSession: Decodable, Identifiable, Sendable, Equatable {
    let sessionID: String
    let title: String
    let updatedAt: Int64?
    var busy: Bool
    let deviceSlot: String
    let deviceName: String
    let deviceID: String
    let loginSessionID: String
    let online: Bool
    var turnId: String? = nil
    var model: String? = nil
    var reasoningEffort: String? = nil
    var wire: String? = nil

    var id: String { "\(deviceSlot):\(deviceID):\(sessionID)" }
    var displayTitle: String { title == "New session" ? "新对话" : title }

    enum CodingKeys: String, CodingKey {
        case sessionID = "id"
        case title, busy, online, turnId
        // Discovery never supplies model settings. Owner history/command ACKs do.
        case updatedAt = "updated_at"
        case deviceSlot = "device_slot"
        case deviceName = "device_name"
        case deviceID = "device_id"
        case loginSessionID = "login_session_id"
    }
}

struct AISessionAttachmentReference: Decodable, Sendable {
    let attachmentID: String

    enum CodingKeys: String, CodingKey {
        case attachmentID = "attachment_id"
    }
}

struct AIRemoteDevice: Decodable, Identifiable, Sendable, Equatable {
    let deviceSlot: String
    let deviceName: String
    let deviceID: String
    let loginSessionID: String
    let online: Bool

    var id: String { "\(deviceSlot):\(deviceID)" }

    enum CodingKeys: String, CodingKey {
        case online
        case deviceSlot = "device_slot"
        case deviceName = "device_name"
        case deviceID = "device_id"
        case loginSessionID = "login_session_id"
    }
}

private final class AIRemoteSocketSink: @unchecked Sendable {
    private let lock = NSLock()
    private weak var transport: AIRemoteSessionTransport?

    init(_ transport: AIRemoteSessionTransport) { self.transport = transport }

    func deliver(event: Int32, data: Data) {
        lock.lock()
        let target = transport
        lock.unlock()
        if let target { Task { await target.receive(event: event, data: data) } }
    }

    func invalidate() {
        lock.lock()
        transport = nil
        lock.unlock()
    }
}

private func aiRemoteSocketEvent(_ context: UnsafeMutableRawPointer?, _ event: Int32,
                                 _ bytes: UnsafePointer<CChar>?, _ length: Int) {
    guard let context else { return }
    let sink = Unmanaged<AIRemoteSocketSink>.fromOpaque(context).takeUnretainedValue()
    let data = bytes.map { Data(bytes: $0, count: length) } ?? Data()
    sink.deliver(event: event, data: data)
}

private actor AIRemoteSessionTransport {
    private struct Catalog: Decodable {
        let sessions: [AIRemoteSession]
        let devices: [AIRemoteDevice]?
    }
    private struct Inbox: Decodable { let messages: [Envelope] }
    struct Envelope: Decodable, Sendable {
        let id: Int
        let kind: String
        let sessionID: String
        let clientMessageID: String
        let fromSlot: String
        let fromLoginSessionID: String
        let payload: Payload

        enum CodingKeys: String, CodingKey {
            case id, kind, payload
            case sessionID = "session_id"
            case clientMessageID = "client_message_id"
            case fromSlot = "from_slot"
            case fromLoginSessionID = "from_login_session_id"
        }
    }
    struct Payload: Decodable, Sendable {
        struct LifecycleResult: Decodable, Sendable {
            let ok: Bool
            let sessionID: String?
            let error: String?

            enum CodingKeys: String, CodingKey {
                case ok, error
                case sessionID = "session_id"
            }
        }

        let transferID: String?
        let chunkIndex: Int?
        let chunkCount: Int?
        let sha256: String?
        let dataBase64: String?
        let before: String?
        let limit: Int?
        let action: String?
        let model: String?
        let reasoningEffort: String?
        let text: String?
        let attachments: [AISessionAttachmentReference]?
        let requestID: String?
        let turnId: String?
        let steeringRequestId: String?
        let quoteMessageId: String?
        let decision: String?
        let title: String?
        let workspacePath: String?
        let targetSessionID: String?
        let type: String?
        let commandID: String?
        let result: LifecycleResult?

        enum CodingKeys: String, CodingKey {
            case transferID = "transfer_id"
            case chunkIndex = "chunk_index"
            case chunkCount = "chunk_count"
            case sha256
            case dataBase64 = "data_base64"
            case before, limit, action, model, reasoningEffort, text, attachments, decision, title
            case requestID = "id"
            case turnId, quoteMessageId
            case steeringRequestId = "requestId"
            case workspacePath = "workspace_path"
            case targetSessionID = "target_session_id"
            case type
            case commandID = "command_id"
            case result
        }
    }

    private var socket: OpaquePointer?
    private var socketSink: AIRemoteSocketSink?
    private var connected = false
    private var connectionGeneration = 0
    private var announcedGeneration: Int?
    private var lastPresenceBody: [String: Any]?
    private var authorizedBearer: String?
    private var reauthTask: Task<Void, Error>?
    private var connectionWaiters: [CheckedContinuation<Void, any Error>] = []
    private var responses: [String: CheckedContinuation<Data, any Error>] = [:]
    private var waitingInbox: CheckedContinuation<[Envelope], any Error>?
    private var inbox: [Envelope] = []
    private var cachedCatalog: [AIRemoteSession] = []
    private var localLoginSessionID: String?
    private var cachedDevices: [AIRemoteDevice] = []
    private var catalogHandler: (@Sendable ([AIRemoteSession], [AIRemoteDevice]) -> Void)?
    private var connectionEventSequence = 0
    private var connectionHandler: (@Sendable (Bool, Int) -> Void)?

    func setConnectionHandler(_ handler: @escaping @Sendable (Bool, Int) -> Void) {
        connectionHandler = handler
        if connected { handler(true, connectionEventSequence) }
    }

    func setCatalogHandler(_ handler: @escaping @Sendable ([AIRemoteSession], [AIRemoteDevice]) -> Void) {
        catalogHandler = handler
        if !cachedCatalog.isEmpty || !cachedDevices.isEmpty {
            handler(cachedCatalog, cachedDevices)
        }
    }

    func receive(event: Int32, data: Data) {
        guard socket != nil else { return }
        if event == 1 {
            connected = true
            connectionGeneration += 1
            connectionEventSequence += 1
            connectionHandler?(true, connectionEventSequence)
            announcedGeneration = nil
            localLoginSessionID = nil
            let waiters = connectionWaiters
            connectionWaiters.removeAll()
            waiters.forEach { $0.resume() }
            return
        }
        if event == 2 || event == 4 {
            connected = false
            connectionEventSequence += 1
            connectionHandler?(false, connectionEventSequence)
            announcedGeneration = nil
            localLoginSessionID = nil
            authorizedBearer = nil
            let failure = AIBackendError(message: "设备同步连接中断")
            let waiters = connectionWaiters
            connectionWaiters.removeAll()
            waiters.forEach { $0.resume(throwing: failure) }
            let pending = responses
            responses.removeAll()
            pending.values.forEach { $0.resume(throwing: failure) }
            waitingInbox?.resume(throwing: failure)
            waitingInbox = nil
            return
        }
        guard event == 3,
              let frame = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let type = frame["type"] as? String else { return }
        if type == "response", let id = frame["id"] as? String,
           let continuation = responses.removeValue(forKey: id) {
            let body = frame["body"] as? [String: Any] ?? [:]
            let status = frame["status"] as? Int ?? 0
            if status != 200 {
                continuation.resume(throwing: AIBackendError(
                    message: body["error"] as? String ?? "设备同步请求失败"))
            } else {
                do { continuation.resume(returning: try JSONSerialization.data(withJSONObject: body)) }
                catch { continuation.resume(throwing: error) }
            }
        } else if type == "catalog", let catalog = try? JSONDecoder().decode(Catalog.self, from: data) {
            cachedCatalog = catalog.sessions
            cachedDevices = catalog.devices ?? []
            catalogHandler?(catalog.sessions, cachedDevices)
        } else if type == "inbox", let batch = try? JSONDecoder().decode(Inbox.self, from: data) {
            if let continuation = waitingInbox {
                waitingInbox = nil
                continuation.resume(returning: batch.messages)
            } else { inbox.append(contentsOf: batch.messages) }
        }
    }

    private func connect() async throws {
        let token = try await MaiChatAuthClient.shared.sessionSyncAccessToken()
        if let socket {
            guard token.withCString({ maiSessionSyncUpdateBearer(socket, $0) }) == 1 else {
                throw AIBackendError(message: "设备同步凭据更新失败")
            }
        } else {
            let url = await MaiChatAuthClient.shared.sessionSyncSocketURL().absoluteString
            let certificate: String
            #if DEBUG && targetEnvironment(simulator)
            if ProcessInfo.processInfo.environment["MAICHAT_LOCAL_UI_PHONE"] != nil,
               let encoded = ProcessInfo.processInfo.environment["MAICHAT_LOCAL_UI_CA_BASE64"],
               let data = Data(base64Encoded: encoded),
               let localCertificate = String(data: data, encoding: .utf8) {
                certificate = localCertificate
            } else {
                guard let certificateURL = Bundle.main.url(
                    forResource: "cacert", withExtension: "pem", subdirectory: "MaiAgentCertificates"
                ), let bundled = try? String(contentsOf: certificateURL, encoding: .utf8) else {
                    throw AIBackendError(message: "设备同步证书不可用")
                }
                certificate = bundled
            }
            #else
            guard let certificateURL = Bundle.main.url(
                forResource: "cacert", withExtension: "pem", subdirectory: "MaiAgentCertificates"
            ), let bundled = try? String(contentsOf: certificateURL, encoding: .utf8) else {
                throw AIBackendError(message: "设备同步证书不可用")
            }
            certificate = bundled
            #endif
            let lookup = url.replacingOccurrences(of: "wss://", with: "https://")
                .replacingOccurrences(of: "ws://", with: "http://")
            var proxyBytes = [CChar](repeating: 0, count: 512)
            let capacity = proxyBytes.count
            let proxyFound: Bool
            #if DEBUG && targetEnvironment(simulator)
            if ProcessInfo.processInfo.environment["MAICHAT_LOCAL_UI_PHONE"] != nil {
                proxyFound = true
            } else {
                proxyFound = lookup.withCString {
                    maiChatHttpProxyForURL($0, &proxyBytes, capacity)
                } == 1
            }
            #else
            proxyFound = lookup.withCString {
                maiChatHttpProxyForURL($0, &proxyBytes, capacity)
            } == 1
            #endif
            guard proxyFound || URL(string: url)?.host == "ichat.life" else {
                throw AIBackendError(message: "当前网络代理不支持设备同步")
            }
            // 账号域名在系统代理无法使用时允许直连；共享 WSS 核心继续校验原域名证书。
            let proxy = proxyFound ? String(cString: proxyBytes) : ""
            let sink = AIRemoteSocketSink(self)
            guard let created = maiSessionSyncCreate(aiRemoteSocketEvent,
                Unmanaged.passUnretained(sink).toOpaque()) else {
                throw AIBackendError(message: "设备同步连接初始化失败")
            }
            let started = url.withCString { address in
                token.withCString { bearer in
                    certificate.withCString { pem in
                        proxy.withCString { route in
                            maiSessionSyncStart(created, address, bearer, pem, route)
                        }
                    }
                }
            }
            guard started == 1 else {
                maiSessionSyncDestroy(created)
                throw AIBackendError(message: "设备同步连接启动失败")
            }
            socket = created
            socketSink = sink
            authorizedBearer = token
        }
        if !connected {
            try await withCheckedThrowingContinuation { continuation in
                connectionWaiters.append(continuation)
                Task {
                    try? await Task.sleep(for: .seconds(10))
                    self.expireConnectionWaiters()
                }
            }
        }
        try await reauthorizeIfNeeded(token)
    }

    private func reauthorizeIfNeeded(_ token: String) async throws {
        guard authorizedBearer != token else { return }
        if let reauthTask {
            try await reauthTask.value
            return
        }
        let task = Task { [self] in
            _ = try await requestConnected("reauth", body: ["access_token": token])
        }
        reauthTask = task
        defer { reauthTask = nil }
        try await task.value
        authorizedBearer = token
    }

    private func expireConnectionWaiters() {
        guard !connected, !connectionWaiters.isEmpty else { return }
        let waiters = connectionWaiters
        connectionWaiters.removeAll()
        let failure = AIBackendError(message: "设备同步连接超时")
        waiters.forEach { $0.resume(throwing: failure) }
    }

    private func expireResponse(_ id: String) {
        responses.removeValue(forKey: id)?.resume(
            throwing: AIBackendError(message: "设备同步操作状态未知，请先查看会话记录"))
    }

    private func request(_ operation: String, body: [String: Any] = [:]) async throws -> Data {
        try await connect()
        if operation != "presence" { try await ensurePresence() }
        return try await requestConnected(operation, body: body)
    }

    private func announcePresence(_ body: [String: Any]) async throws {
        let generation = connectionGeneration
        let response = try await requestConnected("presence", body: body)
        guard connected, generation == connectionGeneration,
              let value = try JSONSerialization.jsonObject(with: response) as? [String: Any],
              let login = value["login_session_id"] as? String, !login.isEmpty else {
            throw AIBackendError(message: "设备同步连接已更新，请重试")
        }
        localLoginSessionID = login
        announcedGeneration = generation
    }

    private func ensurePresence() async throws {
        guard announcedGeneration != connectionGeneration else { return }
        guard let lastPresenceBody else {
            throw AIBackendError(message: "设备同步正在登记本机，请稍后重试")
        }
        try await announcePresence(lastPresenceBody)
    }

    private func requestConnected(_ operation: String,
                                  body: [String: Any] = [:]) async throws -> Data {
        let id = UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased()
        let frame: [String: Any] = ["id": id, "op": operation, "body": body]
        let data = try JSONSerialization.data(withJSONObject: frame, options: .sortedKeys)
        guard let socket else { throw AIBackendError(message: "设备同步连接已关闭") }
        return try await withCheckedThrowingContinuation { continuation in
            responses[id] = continuation
            let sent = data.withUnsafeBytes { bytes in
                maiSessionSyncSend(socket, bytes.baseAddress?.assumingMemoryBound(to: CChar.self), data.count)
            }
            if sent != 1 {
                responses.removeValue(forKey: id)?.resume(
                    throwing: AIBackendError(message: "设备同步连接中断"))
                return
            }
            Task {
                try? await Task.sleep(for: .seconds(15))
                self.expireResponse(id)
            }
        }
    }

    func catalog() async throws -> [AIRemoteSession] {
        try await connect()
        try await ensurePresence()
        return cachedCatalog
    }

    func history(ownerSlot: String, sessionID: String, before: String,
                 limit: Int) async throws -> Data {
        try await request("history", body: ["owner_slot": ownerSlot,
                                            "session_id": sessionID,
                                            "before": before, "limit": limit])
    }

    func publish(sessionID: String, data: Data) async throws {
        guard var body = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            throw AIBackendError(message: "会话消息格式错误")
        }
        body["session_id"] = sessionID
        _ = try await request("publish", body: body)
    }

    func deleteRoom(sessionID: String) async throws {
        _ = try await request("room-delete", body: ["session_id": sessionID])
    }

    func presence(deviceID: String, name: String, sessions: [AISession]) async throws {
        let updated = Int64(Date().timeIntervalSince1970 * 1_000)
        let entries: [[String: Any]] = sessions.prefix(200).enumerated().map { index, item in
            let safeTitle = String(String(item.title.prefix(120)).unicodeScalars.filter {
                !CharacterSet.controlCharacters.contains($0)
            })
            return ["id": item.id, "title": safeTitle, "turnId": item.turnId ?? "",
                    "busy": item.busy, "updated_at": updated - Int64(index)]
        }
        let body: [String: Any] = [
            "device_id": deviceID, "name": name, "sessions": entries
        ]
        lastPresenceBody = body
        try await connect()
        try await announcePresence(body)
    }

    func currentLoginSessionID() -> String? { localLoginSessionID }

    func poll() async throws -> [Envelope] {
        try await connect()
        try await ensurePresence()
        if !inbox.isEmpty {
            let messages = inbox
            inbox.removeAll()
            return messages
        }
        return try await withCheckedThrowingContinuation { continuation in
            waitingInbox = continuation
            Task {
                try? await Task.sleep(for: .seconds(10))
                self.finishIdlePoll()
            }
        }
    }

    private func finishIdlePoll() {
        waitingInbox?.resume(returning: [])
        waitingInbox = nil
    }

    func send(to session: AIRemoteSession, kind: String,
              payload: [String: Any], messageID: String? = nil) async throws -> String {
        try await send(toSlot: session.deviceSlot, toLogin: session.loginSessionID,
                       sessionID: session.sessionID, kind: kind, payload: payload,
                       messageID: messageID)
    }

    func sendCommand(to session: AIRemoteSession, encodedPayload: Data,
                     messageID: String) async throws {
        guard let payload = try JSONSerialization.jsonObject(with: encodedPayload) as? [String: Any] else {
            throw AIBackendError(message: "跨设备命令格式错误")
        }
        _ = try await send(to: session, kind: "command", payload: payload,
                           messageID: messageID)
    }

    func sendEncoded(toSlot: String, toLogin: String, sessionID: String,
                     kind: String, encodedPayload: Data,
                     messageID: String? = nil) async throws -> String {
        guard let payload = try JSONSerialization.jsonObject(with: encodedPayload) as? [String: Any] else {
            throw AIBackendError(message: "跨设备操作格式错误")
        }
        return try await send(toSlot: toSlot, toLogin: toLogin,
                              sessionID: sessionID, kind: kind,
                              payload: payload, messageID: messageID)
    }

    func send(toSlot: String, toLogin: String, sessionID: String,
              kind: String, payload: [String: Any], messageID: String? = nil) async throws -> String {
        let id = messageID ?? UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased()
        _ = try await request("send", body: [
            "to_slot": toSlot,
            "to_login_session_id": toLogin,
            "client_message_id": id,
            "session_id": sessionID,
            "kind": kind,
            "payload": payload
        ])
        return id
    }

    func sendPayload(to envelope: Envelope, kind: String,
                     value: [String: Any]) async throws {
        let data = try JSONSerialization.data(withJSONObject: value)
        try await sendPayloadData(to: envelope, kind: kind, data: data)
    }

    func sendPayloadData(to envelope: Envelope, kind: String,
                         data: Data) async throws {
        let chunkSize = 24 * 1024
        let count = max(1, (data.count + chunkSize - 1) / chunkSize)
        guard count <= 128 else { throw AIBackendError(message: "跨设备会话内容过大") }
        let transferID = UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased()
        let digest = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
        for index in 0..<count {
            let start = index * chunkSize
            let end = min(start + chunkSize, data.count)
            _ = try await send(toSlot: envelope.fromSlot,
                               toLogin: envelope.fromLoginSessionID,
                               sessionID: envelope.sessionID, kind: kind,
                               payload: ["transfer_id": transferID,
                                         "chunk_index": index,
                                         "chunk_count": count,
                                         "sha256": digest,
                                         "data_base64": data[start..<end].base64EncodedString()])
        }
    }

    func acknowledge(_ id: Int) async throws {
        _ = try await request("ack", body: ["ids": [id]])
    }

    func stop() {
        socketSink?.invalidate()
        if let socket { maiSessionSyncDestroy(socket) }
        socket = nil
        socketSink = nil
        connected = false
        authorizedBearer = nil
        let failure = AIBackendError(message: "设备同步连接已关闭")
        let waiters = connectionWaiters
        connectionWaiters.removeAll()
        waiters.forEach { $0.resume(throwing: failure) }
        let pending = responses
        responses.removeAll()
        pending.values.forEach { $0.resume(throwing: failure) }
        waitingInbox?.resume(throwing: failure)
        waitingInbox = nil
        inbox.removeAll()
        cachedCatalog.removeAll()
    }
}

private struct AIRemoteTransfer {
    let sessionID: String
    let fromSlot: String
    let fromLoginSessionID: String
    let kind: String
    let count: Int
    let sha256: String
    var chunks: [Int: Data] = [:]
    var started = Date()
}
/// Temporary object metadata is a display receipt, never a local path or a
/// request to re-upload. Malformed optional fields disable this image without
/// making the rest of a remote history page unreadable.
struct AIRemoteImageMetadata: Codable, Sendable, Equatable {
    let state: String?
    let retention: String?
    let attachmentID: String?
    let ownerLoginSessionID: String?
    let roomKey: String?
    let name: String?
    let mediaType: String?
    let contentType: String?
    let sizeBytes: UInt64?
    let expiresAt: Int64?
    let errorCode: String?

    enum CodingKeys: String, CodingKey {
        case state, retention, name
        case attachmentID = "attachment_id", ownerLoginSessionID = "owner_login_session_id"
        case roomKey = "room_key", mediaType = "media_type", contentType = "content_type"
        case sizeBytes = "size_bytes", expiresAt = "expires_at", errorCode = "error_code"
    }

    init(from decoder: Decoder) throws {
        let values = try? decoder.container(keyedBy: CodingKeys.self)
        state = try? values?.decode(String.self, forKey: .state)
        retention = try? values?.decode(String.self, forKey: .retention)
        attachmentID = try? values?.decode(String.self, forKey: .attachmentID)
        ownerLoginSessionID = try? values?.decode(String.self, forKey: .ownerLoginSessionID)
        roomKey = try? values?.decode(String.self, forKey: .roomKey)
        name = try? values?.decode(String.self, forKey: .name)
        mediaType = try? values?.decode(String.self, forKey: .mediaType)
        contentType = try? values?.decode(String.self, forKey: .contentType)
        sizeBytes = try? values?.decode(UInt64.self, forKey: .sizeBytes)
        expiresAt = try? values?.decode(Int64.self, forKey: .expiresAt)
        errorCode = try? values?.decode(String.self, forKey: .errorCode)
    }

    var isExpired: Bool {
        expiresAt.map { $0 > 0 && $0 <= Int64(Date().timeIntervalSince1970 * 1_000) } ?? false
    }
}

struct AIRemoteImageOwner: Hashable, Sendable {
    let accountID: String
    let slot: String
    let loginSessionID: String
    let sessionID: String
    let selectionGeneration: Int
}

/// The immutable original-owner tuple is also the cache namespace. The current
/// catalog login is kept separately by AIRemoteImageOwner for stale UI checks.
struct AIRemoteImageRequest: Codable, Hashable, Sendable {
    let accountID: String
    let ownerSlot: String
    let ownerLoginSessionID: String
    let sessionID: String
    let messageID: String
    let partID: String
    let attachmentID: String
    let name: String
    let contentType: String
    let sizeBytes: UInt64
    let expiresAt: Int64

    private static func digest(_ bytes: Data) -> String {
        SHA256.hash(data: bytes).map { String(format: "%02x", $0) }.joined()
    }
    var accountKey: String { Self.digest(Data(accountID.utf8)) }
    var cacheKey: String {
        Self.digest(try! JSONEncoder().encode([accountID, ownerSlot, ownerLoginSessionID,
            sessionID, messageID, partID, attachmentID]))
    }
    var signature: String {
        let encoder = JSONEncoder(); encoder.outputFormatting = .sortedKeys
        return Self.digest(try! encoder.encode(self))
    }
    var isExpired: Bool { expiresAt <= Int64(Date().timeIntervalSince1970 * 1_000) }

    init?(owner: AIRemoteImageOwner, messageID: String, part: AIPart) {
        guard let image = part.remoteImage, image.retention == "temporary",
              image.state == "ready" || image.state == "expired",
              !owner.accountID.isEmpty, ["mac", "windows", "ios_phone", "android_phone", "tablet"].contains(owner.slot),
              !owner.sessionID.isEmpty, !messageID.isEmpty, !part.id.isEmpty,
              part.kind == "image", part.remotePartID == nil || part.remotePartID == part.id,
              let id = image.attachmentID, UUID(uuidString: id) != nil,
              let login = image.ownerLoginSessionID, !login.isEmpty, login.utf8.count <= 128,
              image.roomKey == owner.slot + ":" + owner.sessionID,
              let name = image.name, let mime = image.contentType, image.mediaType == "image",
              let size = image.sizeBytes, size > 0,
              let expiry = image.expiresAt, expiry > 0 else { return nil }
        var info = MaiCloudDriveMediaInfo()
        guard name.withCString({ mai_session_attachment_media_info($0, &info) != 0 }),
              String(cString: info.media_type_utf8) == "image",
              String(cString: info.content_type_utf8) == mime, size <= info.max_size_bytes else { return nil }
        accountID = owner.accountID; ownerSlot = owner.slot; ownerLoginSessionID = login
        sessionID = owner.sessionID; self.messageID = messageID; partID = part.id
        attachmentID = id; self.name = name; contentType = mime; sizeBytes = size; expiresAt = expiry
    }
}

enum AIRemoteImageError: LocalizedError {
    case unavailable, pending, uploadFailed, expired, changed
    var errorDescription: String? {
        switch self {
        case .unavailable: "图片暂不可用"
        case .pending: "图片正在同步"
        case .uploadFailed: "图片同步失败"
        case .expired: "图片已过期"
        case .changed: "会话已切换"
        }
    }
}

/// Only completed, exact-size files with a matching receipt are reusable. This
/// actor does filesystem work off MainActor; HTTP/OSS stays in the shared C++ core.
private actor AIRemoteImageCache {
    private struct Receipt: Codable {
        let signature: String
        let sha256: String
    }
    private let root = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("MaiSessionTemporaryImages", isDirectory: true)
        .resolvingSymlinksInPath().standardizedFileURL
    private var invalidated = false

    private func directory(_ request: AIRemoteImageRequest) -> URL {
        root.appendingPathComponent(request.accountKey, isDirectory: true)
            .appendingPathComponent(request.cacheKey, isDirectory: true)
    }
    private func image(_ directory: URL, _ request: AIRemoteImageRequest) -> URL {
        directory.appendingPathComponent(request.name)
    }
    private func completeBytes(_ url: URL, _ request: AIRemoteImageRequest) -> Data? {
        guard url.resolvingSymlinksInPath().standardizedFileURL == url.standardizedFileURL,
              let values = try? url.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey]),
              values.isRegularFile == true, let size = values.fileSize, UInt64(size) == request.sizeBytes,
              let bytes = try? Data(contentsOf: url), UInt64(bytes.count) == request.sizeBytes else { return nil }
        return bytes
    }
    func cached(_ request: AIRemoteImageRequest) -> URL? {
        guard !invalidated else { return nil }
        let directory = directory(request)
        let receiptURL = directory.appendingPathComponent("complete.json")
        guard receiptURL.resolvingSymlinksInPath().standardizedFileURL == receiptURL.standardizedFileURL,
              let length = try? receiptURL.resourceValues(forKeys: [.fileSizeKey]).fileSize,
              length > 0, length <= 4_096,
              let raw = try? Data(contentsOf: receiptURL),
              let receipt = try? JSONDecoder().decode(Receipt.self, from: raw),
              receipt.signature == request.signature,
              let bytes = completeBytes(image(directory, request), request),
              SHA256.hash(data: bytes).map({ String(format: "%02x", $0) }).joined() == receipt.sha256
        else { return nil }
        return image(directory, request)
    }
    func staging(_ request: AIRemoteImageRequest) throws -> URL {
        guard !invalidated else { throw CancellationError() }
        let folder = root.appendingPathComponent(request.accountKey, isDirectory: true)
            .appendingPathComponent("staging-" + UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        return image(folder, request)
    }
    func commit(_ source: URL, request: AIRemoteImageRequest) throws -> URL {
        guard !invalidated, let bytes = completeBytes(source, request) else {
            throw AIRemoteImageError.unavailable
        }
        if let existing = cached(request) { discard(source); return existing }
        let receipt = Receipt(signature: request.signature,
            sha256: SHA256.hash(data: bytes).map { String(format: "%02x", $0) }.joined())
        let stage = source.deletingLastPathComponent()
        try JSONEncoder().encode(receipt).write(to: stage.appendingPathComponent("complete.json"), options: .atomic)
        let destination = directory(request)
        if FileManager.default.fileExists(atPath: destination.path) {
            try FileManager.default.removeItem(at: destination)
        }
        try FileManager.default.moveItem(at: stage, to: destination)
        return image(destination, request)
    }
    func discard(_ source: URL) {
        let folder = source.deletingLastPathComponent()
        guard folder.lastPathComponent.hasPrefix("staging-"),
              folder.path.hasPrefix(root.path + "/") else { return }
        try? FileManager.default.removeItem(at: folder)
    }
    func invalidate(accountID: String) {
        invalidated = true
        let account = SHA256.hash(data: Data(accountID.utf8)).map { String(format: "%02x", $0) }.joined()
        try? FileManager.default.removeItem(at: root.appendingPathComponent(account, isDirectory: true))
    }
}

struct AIPart: Codable, Identifiable, Sendable, Equatable {
    let id: String
    let kind: String
    var text: String?
    var tool: String?
    var input: String?
    var output: String?
    var error: String?
    var state: String?
    var path: String?
    var mimeType: String?
    var messageId: String? = nil
    var preview: String? = nil
    var attachmentID: String? = nil
    var name: String? = nil
    var created: Int64? = nil
    var filename: String? = nil
    var remotePartID: String? = nil
    var remoteImage: AIRemoteImageMetadata? = nil

    enum CodingKeys: String, CodingKey {
        case id, kind, text, tool, input, output, error, state, path
        case mimeType, messageId, preview, name, created, filename, remoteImage
        case remotePartID = "remotePartId"
        case attachmentID = "attachment_id"
    }

    // 问题沿用已持久化的工具片段位置；不再创建临时选项卡和第二个回答输入框。
    // 即使等待已经结束，历史仍能还原问题与真实成功的回答；取消/失败不伪装成用户回答。
    var questionText: String? {
        guard kind == "tool", tool == "question", let data = input?.data(using: .utf8),
              let value = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
              let question = value["question"] as? String, !question.isEmpty else { return nil }
        let options = value["options"] as? [String] ?? []
        guard !options.isEmpty else { return question }
        return question + "\n\n" + options.enumerated().map { "\($0.offset + 1). \($0.element)" }
            .joined(separator: "\n")
    }

    var questionAnswer: String? {
        guard questionText != nil, state == "completed", let output, !output.isEmpty else { return nil }
        return output
    }
}
struct AIMessage: Codable, Identifiable, Sendable, Equatable {
    let id: String
    let role: String
    let created: Int64
    let completed: Int64
    let active: Bool
    let parts: [AIPart]
    // 子任务的问题只投影到父会话阅读；原消息仍属于真实子会话，不能作为父会话引用提交。
    var sessionId: String? = nil
    var questionProjection: Bool? = nil
    // 只影响显示身份与媒体去重；原 id 仍用于 Core 查询/引用，不进入同步协议。
    var displayId: String? = nil
    var isLastDisplayFragment = true
    var sourceParts: [AIPart]? = nil
    var displayIdentity: String { displayId ?? id }
    enum CodingKeys: String, CodingKey {
        case id, role, created, completed, active, parts, sessionId, questionProjection
    }
    // 思考正文仍由核心持久化；界面快照只保留片段身份。流式思考每个增量因此
    // 不会改变可见消息，也不会触发整行 Markdown 重排或打断键盘输入。
    func withoutReasoningText() -> AIMessage {
        guard parts.contains(where: { $0.kind == "reasoning" }) else { return self }
        return AIMessage(id: id, role: role, created: created, completed: completed,
                         active: active, parts: parts.map { original in
            var part = original
            if part.kind == "reasoning" { part.text = nil }
            return part
        }, sessionId: sessionId, questionProjection: questionProjection)
    }
    var text: String {
        parts.compactMap { part in
            if part.kind == "text" { return part.text }
            guard let question = part.questionText else { return nil }
            return question + (part.questionAnswer.map { "\n\n" + $0 } ?? "")
        }.joined(separator: "\n")
    }
    var copyText: String {
        (sourceParts ?? parts).compactMap { part in
            if part.kind == "text" { return part.text }
            guard let question = part.questionText else { return nil }
            return question + (part.questionAnswer.map { "\n\n" + $0 } ?? "")
        }.joined(separator: "\n")
    }
    var quotePreview: String {
        var media: [String] = []
        for part in parts {
            if part.kind == "image" { media.append("图片") }
            if part.kind == "video" { media.append("视频") }
            if part.kind == "tool", part.tool == "agent_send_media",
               let output = part.output?.data(using: .utf8),
               let payload = (try? JSONSerialization.jsonObject(with: output)) as? [String: Any],
               let type = payload["type"] as? String {
                media.append(type == "video" ? "视频" : type == "image" ? "图片" : "音频")
            }
        }
        let summary = media.isEmpty ? "" : "[\(media.joined(separator: "、"))] "
        let excerpt = text.trimmingCharacters(in: .whitespacesAndNewlines)
        return summary + (excerpt.isEmpty ? "消息" : String(excerpt.prefix(80)))
    }
}
struct AIPermission: Codable, Identifiable, Sendable, Equatable {
    let id: String
    let tool: String
    let input: String
    var allowForSession: Bool?
    var rememberOnApproval: Bool?
    var fileCount: Int?
}
struct AIQuestion: Codable, Identifiable, Sendable, Equatable {
    let id: String
    let question: String
    let options: [String]
    var sessionId: String? = nil
    var messageId: String? = nil
    var partId: String? = nil
    var asked: Int64? = nil
}
struct AIImportedFile: Sendable, Equatable {
    let relativePath: String
    let mimeType: String
    let isImage: Bool
    let archiveReference: AIMobileScreenArchiveReference?
    var attachmentIdentity: String { archiveReference?.referenceID ?? relativePath }

    init(relativePath: String, mimeType: String, isImage: Bool,
         archiveReference: AIMobileScreenArchiveReference? = nil) {
        self.relativePath = relativePath
        self.mimeType = mimeType
        self.isImage = isImage
        self.archiveReference = archiveReference
    }
}
struct AIAttachmentImportTarget: Sendable, Hashable {
    let sessionID: String
    let emptyDraftToken: UUID?
    let remoteRoomKey: String?
}

struct AIScreenFrameImportSnapshot: Sendable, Hashable {
    let accountID: String
    let workspace: URL
    let ownerSessionID: String
    let importTarget: AIAttachmentImportTarget
    let isCamera: Bool

    init(accountID: String, workspace: URL, ownerSessionID: String,
         importTarget: AIAttachmentImportTarget, isCamera: Bool = false) {
        self.accountID = accountID
        self.workspace = workspace
        self.ownerSessionID = ownerSessionID
        self.importTarget = importTarget
        self.isCamera = isCamera
    }
}

struct AIScreenFrameImportBinding: Identifiable, Sendable, Equatable {
    let context: AIMobileScreenArchiveContext
    let snapshot: AIScreenFrameImportSnapshot
    var id: String { context.batchID }
}
struct AIImagePreview: Identifiable, Equatable {
    let id: UUID
    let filePath: String
    let imageSize: CGSize
    let sourceFrame: CGRect?
    // 每个缩略图视图的稳定标识；同一文件可能出现在多条消息中，不能仅按路径返回。
    let sourceID: UUID?
    let sequence: MaiMediaSequence?
    // 持有用户已看到的缩略图，让原生预览在冷解码原图期间也能连续放大显示。
    // 生命周期仅限当前预览；不能依赖全图缓存一定命中，也不在点击时同步重读文件。
    let thumbnail: UIImage?

    init(filePath: String, imageSize: CGSize = .zero, sourceFrame: CGRect? = nil,
         sourceID: UUID? = nil,
         sequence: MaiMediaSequence? = nil, thumbnail: UIImage? = nil, id: UUID = UUID()) {
        self.id = id
        self.filePath = filePath
        self.imageSize = imageSize
        self.sourceFrame = sourceFrame
        self.sourceID = sourceID
        self.sequence = sequence
        self.thumbnail = thumbnail
    }
}
struct AIAssistantOpenResult: Sendable {
    let settings: AIModelSettings
    let workspacePath: String
    var configurationError: String? = nil
}
struct AIReplySuggestions: Codable, Sendable, Equatable {
    let natural: String
    let casual: String
    let professional: String
}
struct AIResponse: Codable, Sendable {
    let ok: Bool
    var id: String?
    var userMessageID: String?
    var questionID: String?
    var path: String?
    var error: String?
    var errorCode: String?
    var changed: Bool?
    var sessions: [AISession]?
    var messages: [AIMessage]?
    var messageUpdates: [AIMessage]?
    var hasMessageUpdates: Bool?
    var questionMessages: [AIMessage]?
    var rootSessionId: String?
    var permissions: [AIPermission]?
    var questions: [AIQuestion]?
    var busy: Bool?
    var turnId: String?
    var configured: Bool?
    var model: String?
    var reasoningEffort: String?
    var defaultReasoningEffort: String?
    var session: String?
    var selectedModel: String?
    var selectedReasoningEffort: String?
    var selectedWire: String?
    var wire: String?
    var localGpuSelected: Bool?
    var localGpuModelBytes: UInt64?
    var localGpuBackend: String?
    var natural: String?
    var casual: String?
    var professional: String?

    enum CodingKeys: String, CodingKey {
        case ok, id, path, error, errorCode, changed, sessions, messages, messageUpdates, hasMessageUpdates, questionMessages, rootSessionId, permissions, questions
        case busy, turnId, configured, model, reasoningEffort, defaultReasoningEffort, session, selectedModel, selectedReasoningEffort, selectedWire, wire, localGpuSelected, localGpuModelBytes, localGpuBackend
        case natural, casual, professional
        case userMessageID = "user_message_id"
        case questionID = "question_id"
    }

}
struct AIBackendError: LocalizedError {
    let message: String
    // Unknown delivery may be retried with the original steering request ID.
    var definitive = true
    var code: String? = nil
    var errorDescription: String? { message }
}

/// One supplemental message stays bound to the original task across retries.
/// No model key or account credentials are included in this value.
struct AISteeringRequest: Sendable {
    let session: String
    let turnId: String
    let requestId: String
    let text: String
    let images: [AIImportedFile]
    let videos: [AIImportedFile]
    let quoteMessageId: String
}

struct AISteeringImportCacheIdentity {
    let key: String
    let session: String
    let turnId: String
    let order: UInt64
    var acceptedOrder: UInt64? = nil
}

struct AISteeringImportRuntimeState {
    let busy: Bool
    let turnId: String
}

enum AISteeringImportCachePolicy {
    static let activeCapacity = 128
    static let receiptCapacity = 128
    static let capacity = activeCapacity + receiptCapacity

    static func protectedReceiptKeys(_ entries: [AISteeringImportCacheIdentity]) -> Set<String> {
        Set(entries.filter { $0.acceptedOrder != nil }
            .sorted { $0.acceptedOrder! > $1.acceptedOrder! }
            .prefix(receiptCapacity).map(\.key))
    }

    static func activeCount(_ entries: [AISteeringImportCacheIdentity],
                            states: [String: AISteeringImportRuntimeState]) -> Int {
        entries.filter { entry in
            guard let state = states[entry.session] else { return true }
            return state.busy && (state.turnId.isEmpty || state.turnId == entry.turnId)
        }.count
    }

    /// An unknown owner state protects the entry. Only an ended/replaced turn
    /// can lose its host import receipt; an active turn keeps stable media paths.
    static func evictableKey(_ entries: [AISteeringImportCacheIdentity],
                             states: [String: AISteeringImportRuntimeState]) -> String? {
        let recent = protectedReceiptKeys(entries)
        return entries.filter { entry in
            guard entry.acceptedOrder != nil else { return false }
            guard !recent.contains(entry.key) else { return false }
            guard let state = states[entry.session] else { return false }
            return !state.busy || (!state.turnId.isEmpty && state.turnId != entry.turnId)
        }.min { $0.order < $1.order }?.key
    }
}

// 普通 actor 使用后台执行器。数据库、原生核心、Keychain 和文件都不在 MainActor。
actor AIAssistantBackend {
    private let address: UInt
    private var settings = AIModelSettings()
    private var agentEpoch = UUID().uuidString
    func runtimeEpoch() -> String { agentEpoch }
    private var root: URL?
    private var initialized = false
    private var configurationInProgress = false
    private var permanentlyInvalidated = false

    // 账号删除只影响此实例；普通换号不调用它，旧账号已接受的发送收据仍可正常落盘。
    func invalidatePermanently() {
        permanentlyInvalidated = true
    }

    private func checkLifecycle() throws {
        guard !permanentlyInvalidated else {
            throw AIBackendError(message: "原账号已删除，创作中心已关闭", code: "account_deleted")
        }
        try Task.checkCancellation()
    }
    #if DEBUG && targetEnvironment(simulator)
    /// 合成手动快门仍走 importDocument；只给文件导入准备隔离目录，不初始化 Agent 或凭证。
    func prepareManualCameraImportForUITesting(_ workspace: URL) throws {
        let arguments = ProcessInfo.processInfo.arguments
        let resolved = workspace.standardizedFileURL.resolvingSymlinksInPath()
        let temporary = FileManager.default.temporaryDirectory.standardizedFileURL.resolvingSymlinksInPath()
        guard arguments.contains("--ai-ui-test"),
              arguments.contains("--ai-manual-camera-ui-test") || arguments.contains("--ai-manual-camera-tool-ui-test"),
              !initialized, workspace.isFileURL, resolved.lastPathComponent == "Workspace",
              resolved.path.hasPrefix(temporary.path + "/"),
              FileManager.default.fileExists(atPath: resolved.path) else { throw CancellationError() }
        root = resolved.deletingLastPathComponent()
    }
    #endif
    private func storedKey(for model: String) -> String {
        AICloudCredentialStore.shared.apiKey(["deepseek-flash", "deepseek-v4-pro"].contains(model) ? "deepseek" : "glm")
    }

    init() {
        let pointer = maiMobileAgentCreate()
        address = UInt(bitPattern: pointer)
        guard let pointer else { return }
        if let apiBase = OrtGetApiBase() {
            _ = maiMobileAgentSetOrtApiBase(pointer, UnsafeRawPointer(apiBase))
            // 共用照片处理器延迟加载模型；这里仅登记宿主运行库和固定资源目录。
            if let models = Bundle.main.resourceURL?.appendingPathComponent("MaiPortraitModels") {
                models.path.withCString { maiPortraitConfigure($0, nil, UnsafeRawPointer(apiBase)) }
            }
        }
        _ = maiMobileAgentSetChangeHandler(pointer, aiMobileAgentSessionChanged)
        let context = Unmanaged.passRetained(AIMaiChatHostToolCallbackContext()).toOpaque()
        let registered = maiMobileAgentSetHostToolHandler(
            pointer,
            context,
            aiMaiChatHostToolHandler,
            aiMaiChatHostToolResponseFree,
            aiMaiChatHostToolContextRelease
        )
        if registered == 0 {
            Unmanaged<AIMaiChatHostToolCallbackContext>.fromOpaque(context).release()
        }
    }
    deinit {
        let address = address
        DispatchQueue.global(qos: .utility).async {
            maiMobileAgentDestroy(UnsafeMutableRawPointer(bitPattern: address))
        }
    }

    private func requestAgentOperation(_ operation: String, values: [String: Any] = [:], force: Bool = false) throws
        -> AIResponse
    {
        try checkLifecycle()
        var request: [String: Any] = values
        request["op"] = operation
        request["force"] = force
        let data = try JSONSerialization.data(withJSONObject: request)
        guard let text = String(data: data, encoding: .utf8),
            let response = text.withCString({ maiMobileAgentRequest(UnsafeMutableRawPointer(bitPattern: address), $0) })
        else { throw AIBackendError(
            message: ["steer", "send", "answer"].contains(operation) ? "内容送达状态未知，已保留原操作编号" :
                "创作中心初始化失败", definitive: !["steer", "send", "answer"].contains(operation))
        }
        defer { maiMobileAgentFree(response) }
        let decoded = try JSONDecoder().decode(AIResponse.self, from: Data(String(cString: response).utf8))
        guard decoded.ok else { throw AIBackendError(message: decoded.error ?? "操作失败", code: decoded.errorCode) }
        return decoded
    }

    /// 准备当前账号的工作区、模型凭证和 C++ Agent，返回界面可使用的设置与路径。
    /// 已初始化时复用同一后端，避免页面反复进入时重建数据库连接和任务状态。
    /// 任一必需目录或配置失败时抛错，由页面保留原画面并显示可恢复的错误。
    func open(accountUserID: String) async throws -> AIAssistantOpenResult {
        try checkLifecycle()
        if initialized {
            return AIAssistantOpenResult(
                settings: settings,
                workspacePath: root!.appendingPathComponent("Workspace", isDirectory: true).path
            )
        }
        guard !configurationInProgress else { throw AIBackendError(message: "模型配置正在准备，请稍后重试") }
        configurationInProgress = true
        defer { configurationInProgress = false }
        var directory = try AIAssistantWorkspaceLocator.directory(for: accountUserID)
        #if DEBUG && targetEnvironment(simulator)
        let uiTest = ProcessInfo.processInfo.arguments.contains("--ai-ui-test") ||
            ProcessInfo.processInfo.environment["MAICHAT_LOCAL_UI_MODEL"] == "1"
            if uiTest {
                directory = FileManager.default.temporaryDirectory.appendingPathComponent(
                    "AIAssistantUITest-" + ProcessInfo.processInfo.environment["MAICHAT_AI_TEST_ID", default: "manual"])
            }
        #endif
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        root = directory
        let file = directory.appendingPathComponent("settings.json")
        if FileManager.default.fileExists(atPath: file.path) {
            settings = try JSONDecoder().decode(AIModelSettings.self, from: Data(contentsOf: file))
        }
        // 旧全局 model 不再覆盖任何 session；新/未设置会话统一使用固定默认。
        // 本地会话是否需要准备由稍后的 owner snapshot 决定，不为云端首次打开复制 GGUF。
        settings.selectModel("deepseek-flash")
        var key = ""
        #if DEBUG && targetEnvironment(simulator)
        let primaryModelUITest = AIPrimaryModelUITestSupport.isRequested || AIPrimaryModelUITestSupport.isSessionConfigurationRequested
        #else
        let primaryModelUITest = false
        #endif
        if !settings.isLocalModel && !primaryModelUITest {
            _ = try? await AICloudCredentialSync.refreshAccountModelKeysIfAvailable()
            try checkLifecycle()
            try? await AICloudCredentialSync.refreshSeedanceIfNeeded(force: true)
            try checkLifecycle()
            try? await AICloudCredentialSync.refreshBailianIfNeeded(force: true)
            try checkLifecycle()
            key = storedKey(for: settings.model)
        }
        #if DEBUG && targetEnvironment(simulator)
        if uiTest && !settings.isLocalModel && !AIPrimaryModelUITestSupport.isSessionConfigurationRequested {
            // 独立验收账号使用配套的本机模型端口，避免把测试调用发送到其他正在运行的 fixture。
            let modelPort = Int(ProcessInfo.processInfo.environment["MAICHAT_LOCAL_UI_MODEL_PORT"] ?? "18189")
                .flatMap { (1...65535).contains($0) ? $0 : nil } ?? 18189
            settings.baseUrl = "http://127.0.0.1:\(modelPort)"
            settings.model = "test-model"
            settings.policy = "on-request"
            key = "test-key"
        }
        #endif
        do {
            let local = try await resolveLocalConfiguration(settings)
            try checkLifecycle()
            try configure(settings, key: key, local: local)
        } catch {
            try checkLifecycle()
            guard settings.isLocalModel else { throw error }
            // 包内模型异常保留当前来源；不静默改用云端，也不要求用户修复文件目录。
            return AIAssistantOpenResult(settings: settings,
                workspacePath: directory.appendingPathComponent("Workspace", isDirectory: true).path,
                configurationError: "内置本地模型暂时不可用，请重试；若仍失败，请更新 App。")
        }
        #if DEBUG && targetEnvironment(simulator)
        if uiTest && !settings.isLocalModel {
            let state = try requestAgentOperation("snapshot", values: ["session": "", "messageLimit": 0], force: true)
            if state.sessions?.isEmpty != false { _ = try requestAgentOperation("create") }
        }
        #endif
        initialized = true
        return AIAssistantOpenResult(
            settings: settings,
            workspacePath: directory.appendingPathComponent("Workspace", isDirectory: true).path
        )
    }

    private func resolveLocalConfiguration(_ config: AIModelSettings) async throws -> AILocalModelSelection? {
        guard config.isLocalModel else { return nil }
        return try await AILocalModelStore.shared.bundledPrimarySelection()
    }

    private func configure(_ config: AIModelSettings, key: String, local: AILocalModelSelection? = nil) throws {
        try checkLifecycle()
        // Swift 只负责应用目录、云端密钥和线协议选择；工具调用规则在共享 C++ 层。
        guard let root else { throw AIBackendError(message: "创作中心尚未准备好") }
        let caBundle = Bundle.main.url(forResource: "cacert", withExtension: "pem",
                                      subdirectory: "MaiAgentCertificates")
        guard config.isLocalModel || caBundle != nil else {
            throw AIBackendError(message: "HTTPS 根证书文件缺失")
        }
        let workspace = root.appendingPathComponent("Workspace", isDirectory: true)
        try FileManager.default.createDirectory(at: workspace, withIntermediateDirectories: true)
        let appRootPath = AIAssistantPathPolicy.appRoot(workspacePath: workspace.path)
        var routeURLs: [String: Any] = ["glmBaseUrl": AIModelSettings.glmChatUrl,
                                        "deepseekBaseUrl": AIModelSettings.deepSeekUrl]
        #if DEBUG && targetEnvironment(simulator)
        if AIPrimaryModelUITestSupport.isSessionConfigurationRequested {
            let environment = ProcessInfo.processInfo.environment
            for (provider, field) in [("GLM", "glmBaseUrl"), ("DEEPSEEK", "deepseekBaseUrl")] {
                guard let port = Int(environment["MAICHAT_SESSION_MODELS_" + provider + "_PORT"] ?? ""),
                      (1...65535).contains(port) else { throw AIBackendError(message: "缺少隔离测试端口") }
                routeURLs[field] = "http://127.0.0.1:\(port)"
            }
        }
        #endif
        var nativeConfiguration: [String: Any] = [

                "database": root.appendingPathComponent("sessions.sqlite").path,
                "workspace": workspace.path, "baseUrl": config.baseUrl, "apiKey": key,
            "scheduleDeviceName": UIDevice.current.userInterfaceIdiom == .pad ? "iPad" : "iPhone",
            "wire": config.effectiveWire,
            "model": config.model, "policy": config.policy, "appRoot": appRootPath,
            "modelProvider": config.isLocalModel ? "llama" : "remote",
            "localModelPath": local?.modelURL.path ?? "",
            "localProjectorPath": local?.projectorURL?.path ?? "",
            "localContextSize": config.effectiveLocalContextSize,
            "localMaximumTokens": config.effectiveLocalMaximumTokens,
            "localThreads": config.effectiveLocalThreads,
            "localTestModelPath": local?.modelURL.path ?? "",
            "supportsScreenCapture": aiSupportsWholeDeviceScreenCapture(),
            "reasoningEffort": config.effectiveReasoningEffort,
            "caBundle": caBundle?.path ?? "",
            "temporaryDirectory": FileManager.default.temporaryDirectory.path,
            "cacheDirectory": try FileManager.default.url(for: .cachesDirectory,
                in: .userDomainMask, appropriateFor: nil, create: true).path,
            "rvmModelPath": Bundle.main.url(
                forResource: "rvm_mobilenetv3_fp32", withExtension: "onnx",
                subdirectory: "MaiAgentModels")?.path ?? "",
            "lamaModelPath": Bundle.main.url(
                forResource: "lama_fp32", withExtension: "onnx",
                subdirectory: "MaiAgentModels")?.path ?? ""]
        nativeConfiguration["localModelName"] = "local:" + AILocalModelStore.bundledPrimaryID
        nativeConfiguration.merge(routeURLs) { _, supplied in supplied }
        _ = try requestAgentOperation("configure", values: nativeConfiguration)
        agentEpoch = UUID().uuidString
    }

    private var preparedLocalEpoch = ""
    func prepareLocalPrimaryModel() async throws {
        try checkLifecycle()
        let epoch = agentEpoch
        if preparedLocalEpoch == epoch { return }
        let local = try await AILocalModelStore.shared.bundledPrimarySelection()
        try checkLifecycle()
        guard epoch == agentEpoch else { throw CancellationError() }
        _ = try requestAgentOperation("prepare_local_model", values: [
            "model": "local:" + AILocalModelStore.bundledPrimaryID,
            "localModelPath": local.modelURL.path,
            "localProjectorPath": local.projectorURL?.path ?? "",
            "localContextSize": settings.effectiveLocalContextSize,
            "localMaximumTokens": settings.effectiveLocalMaximumTokens,
            "localThreads": settings.effectiveLocalThreads])
        preparedLocalEpoch = epoch
    }

    func save(_ config: AIModelSettings) async throws -> AIModelSettings {
        try checkLifecycle()
        guard !configurationInProgress else { throw AIBackendError(message: "模型配置正在准备，请稍后重试") }
        guard ["on-request", "unless-trusted", "never"].contains(config.policy) else {
            throw AIBackendError(message: "请选择受支持的权限模式")
        }
        configurationInProgress = true
        defer { configurationInProgress = false }
        var normalized = settings
        normalized.policy = config.policy
        let previous = settings
        #if DEBUG && targetEnvironment(simulator)
        let testKey = ProcessInfo.processInfo.arguments.contains("--ai-ui-test") && normalized.model == "test-model"
        #else
        let testKey = false
        #endif
        let key = testKey ? "test-key" : storedKey(for: normalized.model)
        try configure(normalized, key: key)
        do {
            try checkLifecycle()
            try JSONEncoder().encode(normalized).write(to: root!.appendingPathComponent("settings.json"), options: .atomic)
            settings = normalized
            return normalized
        } catch {
            try? configure(previous, key: testKey ? "test-key" : storedKey(for: previous.model))
            throw error
        }
    }

    func request(_ operation: String, values: [String: String] = [:], force: Bool = false) throws -> AIResponse {
        try requestAgentOperation(operation, values: values.mapValues { $0 as Any }, force: force)
    }

    func snapshot(session: String, limit: Int, force: Bool) throws -> AIResponse {
        try requestAgentOperation("snapshot", values: ["session": session, "messageLimit": limit,
            "includeMessageUpdates": true], force: force)
    }

    func messagesPage(session: String, beforeId: String, limit: Int) throws -> [AIMessage] {
        try requestAgentOperation("messages_page", values: ["session": session, "before": beforeId,
                                            "limit": limit]).messages ?? []
    }

    /// Use the persisted receipt ID, rather than a moving latest-message window.
    func userMessage(session: String, id: String) throws -> AIMessage? {
        guard !id.isEmpty else { return nil }
        return try requestAgentOperation("message", values: ["session": session, "id": id])
            .messages?.first { $0.id == id && $0.role == "user" }
    }

    func remoteSnapshot(session: String, beforeId: String = "", limit: Int,
                        includeMessageUpdates: Bool = false) throws -> AIResponse {
        try requestAgentOperation("remote_snapshot", values: ["session": session,
            "before": beforeId, "limit": limit, "includeMessageUpdates": includeMessageUpdates])
    }

    private func prepareTargetSessionModel(_ session: String) async throws {
        let choice = try requestAgentOperation("get_session", values: ["session": session])
        if choice.wire == "local" { try await prepareLocalPrimaryModel() }
        try checkLifecycle()
    }

    func send(session: String, text: String, images: [AIImportedFile],
              videos: [AIImportedFile], quoteMessageId: String) async throws -> AIResponse {
        try await prepareTargetSessionModel(session)
        return try requestAgentOperation("send", values: [
            "session": session,
            "text": text,
            "quoteMessageId": quoteMessageId,
            "images": images.map { ["path": $0.relativePath, "mimeType": $0.mimeType] },
            "videos": videos.map { ["path": $0.relativePath, "mimeType": $0.mimeType] }
        ])
    }

    func steer(_ request: AISteeringRequest) async throws -> AIResponse {
        try await prepareTargetSessionModel(request.session)
        return try requestAgentOperation("steer", values: [
            "session": request.session, "turnId": request.turnId,
            "requestId": request.requestId, "text": request.text,
            "quoteMessageId": request.quoteMessageId,
            "images": request.images.map { ["path": $0.relativePath, "mimeType": $0.mimeType] },
            "videos": request.videos.map { ["path": $0.relativePath, "mimeType": $0.mimeType] }
        ])
    }

    func enqueue(session: String, requestID: String, text: String, images: [AIImportedFile],
                 videos: [AIImportedFile], quoteMessageId: String) async throws -> AIResponse {
        try await prepareTargetSessionModel(session)
        return try requestAgentOperation("enqueue", values: [
            "session": session, "requestId": requestID, "text": text, "quoteMessageId": quoteMessageId,
            "images": images.map { ["path": $0.relativePath, "mimeType": $0.mimeType] },
            "videos": videos.map { ["path": $0.relativePath, "mimeType": $0.mimeType] }])
    }

    func pinInputFile(_ file: AIImportedFile, requestID: String, attachmentID: String) throws -> AIImportedFile {
        let result = try requestAgentOperation("pin_input_file", values: [
            "path": file.relativePath, "requestId": requestID, "attachmentId": attachmentID])
        guard let path = result.path else { throw AIBackendError(message: "无法保存会话附件") }
        return AIImportedFile(relativePath: path, mimeType: file.mimeType, isImage: file.isImage)
    }

    func suggestReplies(messages: [[String: String]], accountUserID: String) async throws
        -> AIReplySuggestions {
        _ = try await open(accountUserID: accountUserID)
        let response = try requestAgentOperation("suggest_replies", values: ["messages": messages])
        guard let natural = response.natural?.trimmingCharacters(in: .whitespacesAndNewlines),
              let casual = response.casual?.trimmingCharacters(in: .whitespacesAndNewlines),
              let professional = response.professional?.trimmingCharacters(in: .whitespacesAndNewlines),
              !natural.isEmpty, !casual.isEmpty, !professional.isEmpty else {
            throw AIBackendError(message: "模型返回的三种回复不完整，请重试。")
        }
        return AIReplySuggestions(
            natural: natural,
            casual: casual,
            professional: professional
        )
    }

    func importDocument(_ source: URL) throws -> AIImportedFile {
        guard let root else { throw AIBackendError(message: "创作中心尚未准备好") }
        let scoped = source.startAccessingSecurityScopedResource()
        defer { if scoped { source.stopAccessingSecurityScopedResource() } }
        let values = try source.resourceValues(forKeys: [.fileSizeKey, .contentTypeKey])
        let contentType = values.contentType
            ?? UTType(filenameExtension: source.pathExtension)
        let isImage = contentType?.conforms(to: .image) == true
        let size = values.fileSize ?? 0
        let maximumSize = isImage ? 20 * 1024 * 1024 : 5 * 1024 * 1024
        guard size <= maximumSize else {
            throw AIBackendError(
                message: isImage ? "请选择不超过 20 MB 的图片" : "请选择不超过 5 MB 的文本文件"
            )
        }
        var data = try Data(contentsOf: source)
        guard isImage || String(data: data, encoding: .utf8) != nil else {
            throw AIBackendError(message: "目前支持 UTF-8 文本、代码文件和图片")
        }
        var storedName = source.lastPathComponent
        var mimeType = contentType?.preferredMIMEType ?? (isImage ? "image/jpeg" : "text/plain")
        let supportedImageTypes = ["image/jpeg", "image/png", "image/webp", "image/gif"]
        if isImage, !supportedImageTypes.contains(mimeType) {
            guard let image = UIImage(data: data),
                  let jpeg = image.jpegData(compressionQuality: 0.92)
            else { throw AIBackendError(message: "图片格式无法转换") }
            data = jpeg
            storedName = source.deletingPathExtension().lastPathComponent + ".jpg"
            mimeType = "image/jpeg"
        }
        guard data.count <= maximumSize else {
            throw AIBackendError(message: "转换后的图片超过 20 MB")
        }
        let name = UUID().uuidString.prefix(8) + "-" + storedName
        let target = root.appendingPathComponent("Workspace").appendingPathComponent(String(name))
        try data.write(to: target, options: .atomic)
        return AIImportedFile(
            relativePath: String(name),
            mimeType: mimeType,
            isImage: isImage
        )
    }

    func importVideo(_ source: URL) throws -> AIImportedFile {
        guard let root else { throw AIBackendError(message: "创作中心尚未准备好") }
        let scoped = source.startAccessingSecurityScopedResource()
        defer { if scoped { source.stopAccessingSecurityScopedResource() } }
        let values = try source.resourceValues(forKeys: [.fileSizeKey, .contentTypeKey])
        let type = values.contentType ?? UTType(filenameExtension: source.pathExtension)
        guard type?.conforms(to: .movie) == true else {
            throw AIBackendError(message: "所选文件不是可用的视频")
        }
        let size = values.fileSize ?? 0
        guard size > 0, size <= 1024 * 1024 * 1024 else {
            throw AIBackendError(message: "请选择不超过 1 GB 的视频")
        }
        let name = UUID().uuidString.prefix(8) + "-" + source.lastPathComponent
        let target = root.appendingPathComponent("Workspace").appendingPathComponent(String(name))
        do {
            try FileManager.default.copyItem(at: source, to: target)
        } catch {
            try? FileManager.default.removeItem(at: target)
            throw error
        }
        return AIImportedFile(
            relativePath: String(name),
            mimeType: type?.preferredMIMEType ?? "video/quicktime",
            isImage: false
        )
    }

    func importSessionFile(_ source: URL, displayName: String? = nil)
        async throws -> AIImportedFile {
        guard let root else { throw AIBackendError(message: "创作中心尚未准备好") }
        let scoped = source.startAccessingSecurityScopedResource()
        defer { if scoped { source.stopAccessingSecurityScopedResource() } }
        let name = displayName ?? source.lastPathComponent
        var info = MaiCloudDriveMediaInfo()
        guard name.withCString({ mai_session_attachment_media_info($0, &info) != 0 }),
              let size = try source.resourceValues(forKeys: [.fileSizeKey]).fileSize,
              size > 0, UInt64(size) <= info.max_size_bytes else {
            throw AIBackendError(message: "会话附件格式或大小无效")
        }
        let relative = String(UUID().uuidString.prefix(8)) + "-" + name
        let target = root.appendingPathComponent("Workspace", isDirectory: true)
            .appendingPathComponent(relative)
        try await RemoteIMBackgroundWork.file {
            do { try FileManager.default.copyItem(at: source, to: target) }
            catch {
                try? FileManager.default.removeItem(at: target)
                throw error
            }
        }
        let mime = String(cString: info.content_type_utf8)
        return AIImportedFile(relativePath: relative, mimeType: mime,
                              isImage: mime.hasPrefix("image/"))
    }
}

@MainActor
final class AIAssistantModel: ObservableObject {
    static var shared = AIAssistantModel()
    private static let recentMessageCount = 8
    private static let olderPageCount = 20
    private static let lifecycleSessionID = "session_lifecycle"

    static func resetForAccountSwitch() {
        shared.submissionGeneration &+= 1
        shared.outgoingPump?.stop()
        let pending = Array(shared.pendingAttachments.values).flatMap { $0 } +
            shared.remoteFilesByCommandID.values.flatMap { $0.files }
        shared.discardImportedFiles(pending)
        Task { @MainActor in await AIMobileScreenCapture.shared.stop() }
        AIMobileLocalMediaRequests.shared.deactivate()
        shared.disappear()
        shared.stopRemoteSync()
        shared.initialPreparation?.cancel()
        shared.appearingTask?.cancel()
        shared.hostStartup?.cancel()
        shared.transientErrorTask?.cancel()
        shared.clearQuestionConversationState()
        shared = AIAssistantModel()
    }

    /// 删除账号专用屏障；普通换号只 stop，不撤销旧账号已经接受的持久收据。
    /// 必须 await 此方法后才移除工作区，防迟到 ACK/附件准备重建已删除账号目录。
    static func invalidateOutgoingForDeletedAccount() async {
        let model = shared
        model.outgoingPermanentlyInvalidated = true
        model.remoteImageFiles.removeAll()
        await model.remoteImageCache.invalidate(accountID: model.screenFrameCurrentAccountID)
        model.clearQuestionConversationState()
        model.outgoingPump?.stop()
        // actor 屏障覆盖正在等待模型文件/凭据的 open/save；删除目录后不能迟到重建。
        await model.backend.invalidatePermanently()
        for pending in model.remoteSteeringConfirmations.values {
            pending.continuation.resume(throwing: AIBackendError(message: "账号已删除", definitive: false))
        }
        model.remoteSteeringConfirmations.removeAll()
        await model.outgoingQueue?.invalidatePermanently()
        await model.outgoingPump?.invalidateAndWait()
        await model.outgoingIntakeTask?.value
        for cleanup in Array(model.outgoingCleanupTasks.values) { await cleanup.value }
    }
    @Published var sessions: [AISession] = []
    @Published private(set) var remoteSessions: [AIRemoteSession] = []
    @Published private(set) var remoteDevices: [AIRemoteDevice] = []
    @Published private(set) var remoteSessionMutationInProgress = false
    @Published private(set) var selectedRemote: AIRemoteSession?
    @Published var messages: [AIMessage] = []
    @Published private(set) var questionMessages: [AIMessage] = []
    // Native 点取窗口外仍在更新的旧 assistant；它们不改变根页的 oldest/before。
    @Published private var updatedRootMessages: [AIMessage] = []
    @Published var permissions: [AIPermission] = []
    @Published var questions: [AIQuestion] = []
    @Published var selected = ""
    @Published var settings = AIModelSettings()
    private var sessionDefaultReasoningEffort = "high"
    @Published private(set) var changingModelSource = false
    @Published private(set) var actualModelID = ""
    @Published private(set) var actualWire = ""
    @Published private(set) var actualGPUSelected = false
    @Published private(set) var actualGPUModelBytes: UInt64 = 0
    @Published private(set) var actualGPUBackend = ""
    @Published private(set) var actualBackendHasError = false
    @Published var configured = false
    @Published var error = ""
    @Published private(set) var remoteRestoreText = ""
    @Published private(set) var remoteRestoreGeneration = 0
    @Published var ready = false
    @Published var historyLoaded = false
    @Published private(set) var hasOlderMessages = false
    @Published private(set) var visibleMessageLimit = AIAssistantModel.recentMessageCount
    @Published var isSubmitting = false
    @Published private(set) var remoteUploadStatus = ""
    @Published private(set) var isCreatingConversation = false
    @Published var scrollRequest = 0
    @Published private(set) var workspacePath = ""
    @Published var previewImage: AIImagePreview?
    @Published var quotedMessage: AIMessage?
    var drafts: [String: String] = [:]
    @Published private(set) var pendingAttachments: [String: [AIImportedFile]] = [:]
    @Published private(set) var outgoingEntries: [AIOutgoingMessageQueueEntry] = []
    private var outgoingQueue: AIOutgoingMessageQueue?
    private(set) var outgoingPump: AIOutgoingMessagePump?
    private var outgoingPermanentlyInvalidated = false
    private var outgoingCleanupTasks: [UUID: Task<Void, Never>] = [:]
    private var outgoingCurrentEpoch = ""
    private var outgoingQueueIdentity = ""
    private var outgoingDeliveryCount = 0
    private var outgoingProtectedPaths: Set<String> = []
    private var outgoingProtectedReferenceIDs: Set<String> = []
    private var outgoingPreparedPins: [UUID: [AIMobileScreenArchiveReference]] = [:]
    private var outgoingIntakeProtection: [String: Int] = [:]
    private struct OutgoingIntake {
        let id: UUID
        let account: String
        let workspace: String
        let session: String
        let remote: AIRemoteSession?
        let emptyToken: UUID
        let text: String
        let quote: String
        let files: [AIImportedFile]
        let questionID: String?
        let question: AIQuestion?
        let result: AsyncStream<Bool>.Continuation
    }
    private var outgoingIntakes: [OutgoingIntake] = []
    // MainActor 上同步保留首次回答的归属，连续点击不会把两条草稿抢投同一个问题。
    private var questionAnswerReservations: [String: UUID] = [:]
    private var questionAnswerReservationOrder: [String] = []
    private struct AcceptedQuestionAnswer {
        let owner: AIOutgoingMessageQueueOwner
        let question: AIQuestion
        let text: String
    }
    // Owner ACK 可能早于工具结果的下一次快照；只保留显示回声，不写入 Core 消息历史。
    @Published private var acceptedQuestionAnswers: [UUID: AcceptedQuestionAnswer] = [:]
    private var acceptedQuestionAnswerOrder: [UUID] = []
    private var outgoingIntakeTask: Task<Void, Never>?
    // Once submitted to the native sender, the token can also protect a
    // persisted user message. A draft cleanup must not release that history.
    private var screenFrameHistoryPins: [String: String] = [:]
    private var screenFrameSubmissionPins: Set<String> = []
    @Published private(set) var screenFrameImportRevision = 0
    private var screenFrameImports: [AIScreenFrameImportBinding] = []
    // A warning must survive even when no readable batch can create a binding.
    // The full snapshot keeps account, workspace, source and original draft separate.
    private var unavailableFrameBatches: [AIScreenFrameImportSnapshot: Int] = [:]
    private(set) var firstMessageSessionID = ""
    private var emptyDraftToken = UUID()
    private var createdEmptyDraftSessions: [UUID: String] = [:]
    private var deletedDraftSessions: Set<String> = []
    private let backend = AIAssistantBackend()
    private let remoteTransport = AIRemoteSessionTransport()
    private let remoteImageCache = AIRemoteImageCache()
    @Published private var remoteImageFiles: [String: URL] = [:]
    private var poll: Task<Void, Never>?
    private var remotePoll: Task<Void, Never>?
    private var initiallyPublishedSessions: Set<String> = []
    private var publishedRemoteMessageDigests: [String: String] = [:]
    private var publishedRemoteStateDigests: [String: String] = [:]
    private var pendingRemotePushSessions: Set<String> = []
    private var remotePublishRoots: [String: String] = [:]
    private var remotePushTask: Task<Void, Never>?
    private var remoteBackfillTask: Task<Void, Never>?
    private var pendingBackfillSessions: [String] = []
    private var remoteCommandsInProgress: Set<String> = []
    @Published private(set) var pendingRemoteSelection: AIRemoteSession?
    private var pendingRemoteSelectionStarted = Date.distantPast
    private var remoteTransfers: [String: AIRemoteTransfer] = [:]
    private var pendingRemoteCommands: [String: Date] = [:]
    private var ownerCommandQueues: [String: [AIRemoteSessionTransport.Envelope]] = [:]
    private var ownerCommandIDs: Set<String> = []
    private var ownerCommandTasks: [String: Task<Void, Never>] = [:]
    private var attachmentRemoteCommands: Set<String> = []
    private var remoteRetryIDByFingerprint: [String: String] = [:]
    private var remoteFingerprintByCommandID: [String: String] = [:]
    private var remoteDraftByCommandID: [String: String] = [:]
    private var remoteOptimisticByCommandID: [String: String] = [:]
    private var remoteResultByCommandID: [String: Bool] = [:]
    private var latestModelCommandByRemote: [String: String] = [:]
    private var remoteConfigurationConnectionSequence = 0
    private struct PendingModelChoice {
        let owner: AIRemoteSession
        let action: String
        let model: String?
        let reasoningEffort: String?
        let generation: Int
        let account: String
    }
    private var pendingModelOwners: [String: PendingModelChoice] = [:]
    private var remoteFilesByCommandID: [String: (key: String, files: [AIImportedFile])] = [:]
    private var remoteDeliveryPendingIDs: Set<String> = []
    private struct PendingLifecycle {
        let deviceSlot: String
        let loginSessionID: String
        let continuation: CheckedContinuation<String, Error>
    }
    private var pendingLifecycle: [String: PendingLifecycle] = [:]
    private var lifecycleRetryIDs: [String: String] = [:]
    private var lastRemotePresence = Date.distantPast
    private var transientErrorTask: Task<Void, Never>?
    private var lastBackendError = ""
    private var initialPreparation: Task<Void, Never>?
    private var hostStartup: Task<Void, Never>?
    private var appearingTask: Task<Void, Never>?
    private var pendingInitialPage: (session: String, newestID: String)?
    private var loadingOlderMessages = false
    private var selectionGeneration = 0
    private var submissionGeneration = 0
    private struct RemoteSteeringConfirmation {
        let remote: AIRemoteSession
        let questionID: String?
        let continuation: CheckedContinuation<String, Error>
    }
    private var remoteSteeringConfirmations: [String: RemoteSteeringConfirmation] = [:]
    // Re-delivery must resolve to the same local paths, not newly imported copies.
    private struct RemoteSteeringImport {
        var identity: AISteeringImportCacheIdentity
        let references: [String]
        let files: [AIImportedFile]
        let names: [String]
        let kinds: [String]
        let mimeTypes: [String]
    }
    private var remoteSteeringImports: [String: RemoteSteeringImport] = [:]
    private var remoteSteeringImportRevision: UInt64 = 0
    private var remoteSteeringImportOrder: UInt64 = 0
    private var remoteSteeringAcceptedOrder: UInt64 = 0
    var composerSubmissionIdentity: String {
        [screenFrameCurrentAccountID, String(submissionGeneration), String(selectionGeneration),
         workspacePath].joined(separator: "\u{1f}")
    }
    private var visible = false
    private var pageActive = true
    private var composerTyping = false
    var busy: Bool {
        if let selectedRemote {
            return remoteSessions.first {
                $0.id == selectedRemote.id &&
                $0.loginSessionID == selectedRemote.loginSessionID
            }?.busy == true
        }
        return sessions.first { $0.id == selected }?.busy == true
    }
    var hasEmptyDraft: Bool {
        !attachments(for: "").isEmpty ||
            !(drafts[""] ?? "").trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }
    var visibleMessages: [AIMessage] {
        // 投影只进入显示层；分页、oldest/before 与历史数量继续只使用根会话 messages。
        var root = Array(messages.suffix(visibleMessageLimit))
        let rootIDs = Set(root.map(\.id))
        let cutoff = root.first?.created ?? 0
        for updated in updatedRootMessages where !rootIDs.contains(updated.id) {
            let recent = updated.parts.filter {
                ($0.created.flatMap { $0 > 0 ? $0 : nil } ?? Self.coreIdTime($0.id) ?? updated.created) >= cutoff ||
                    $0.questionText != nil
            }
            guard !recent.isEmpty else { continue }
            root.append(AIMessage(id: updated.id, role: updated.role, created: updated.created,
                completed: updated.completed, active: updated.active, parts: recent,
                sessionId: updated.sessionId, questionProjection: updated.questionProjection, sourceParts: updated.parts))
        }
        let echoQuestions = currentAcceptedQuestionAnswers.map(\.question)
        let displayQuestions = questions + echoQuestions.filter { echo in !questions.contains { $0.id == echo.id } }
        let pendingPartIDs = Set(displayQuestions.compactMap(\.partId))
        let rootParts = Set(root.flatMap { $0.parts.map(\.id) })
        let oldest = root.first?.created
        var result = root
        for projection in questionMessages {
            guard !result.contains(where: { $0.id == projection.id }) else { continue }
            let parts = projection.parts.filter { part in
                guard part.questionText != nil && !rootParts.contains(part.id) else { return false }
                let created = part.created.flatMap { $0 > 0 ? $0 : nil } ?? Self.coreIdTime(part.id) ?? projection.created
                return (oldest.map { created >= $0 } ?? true) || pendingPartIDs.contains(part.id)
            }
            guard !parts.isEmpty else { continue }
            result.append(AIMessage(id: projection.id, role: "assistant", created: projection.created,
                completed: projection.completed, active: projection.active, parts: parts,
                sessionId: projection.sessionId, questionProjection: true))
        }
        let shownParts = Set(result.flatMap { $0.parts.map(\.id) })
        for question in displayQuestions {
            // 旧远端或窗口之外的等待问题仍显示为普通文本；完整投影到达后由原 part ID 去重。
            if let part = question.partId, shownParts.contains(part) { continue }
            guard let input = try? JSONSerialization.data(withJSONObject: [
                "question": question.question, "options": question.options]),
                  let text = String(data: input, encoding: .utf8) else { continue }
            let part = AIPart(id: question.partId ?? "question-part-" + question.id, kind: "tool",
                tool: "question", input: text, state: "running")
            if question.partId == nil, result.contains(where: { $0.parts.contains { $0.questionText == part.questionText } }) {
                continue
            }
            result.append(AIMessage(id: "question-pending-" + question.id, role: "assistant",
                created: question.asked ?? result.last?.created ?? 0, completed: 0, active: true,
                parts: [part], sessionId: question.sessionId, questionProjection: true))
        }
        return Self.questionDisplayTimeline(result)
    }

    /// Assistant 消息会跨多个工具周期继续追加片段；按真实片段时刻合流，不能用它最初的消息时刻
    /// 把后续进展排到子任务提问之前。问题与其 answer 仍由同一个原片段渲染，始终相邻。
    private static func coreIdTime(_ id: String) -> Int64? {
        guard id.hasPrefix("prt_") || id.hasPrefix("msg_"), id.count >= 14 else { return nil }
        let alphabet = Array("0123456789ABCDEFGHJKMNPQRSTVWXYZ")
        var time: Int64 = 0
        for character in id.dropFirst(4).prefix(10) {
            guard let index = alphabet.firstIndex(of: character) else { return nil }
            time = time * 32 + Int64(index)
        }
        return time
    }

    private static func questionDisplayTimeline(_ source: [AIMessage]) -> [AIMessage] {
        struct Unit {
            let message: AIMessage
            let parts: [AIPart]
            let time: Int64
            let order: String
        }
        var units: [Unit] = []
        for message in source {
            let parts = message.parts.filter { $0.kind != "reasoning" }
            if message.role == "user" || parts.isEmpty {
                units.append(Unit(message: message, parts: parts, time: message.created,
                    order: coreIdTime(message.id) == nil ? message.id : String(message.id.dropFirst(4))))
                continue
            }
            var previous = message.created
            for (index, part) in parts.enumerated() {
                let actual = part.created.flatMap { $0 > 0 ? $0 : nil } ?? coreIdTime(part.id) ?? message.created
                previous = max(previous, actual)
                units.append(Unit(message: message, parts: [part], time: previous,
                    order: coreIdTime(part.id) == nil ? message.id + String(format: ":%06d", index)
                                                  : String(part.id.dropFirst(4))))
            }
        }
        units.sort { $0.time == $1.time ? $0.order < $1.order : $0.time < $1.time }
        var displayed: [AIMessage] = []
        var counts: [String: Int] = [:]
        for unit in units {
            if let last = displayed.last, last.id == unit.message.id, last.role != "user" {
                let combined = AIMessage(id: last.id, role: last.role, created: last.created,
                    completed: last.completed, active: last.active, parts: last.parts + unit.parts,
                    sessionId: last.sessionId, questionProjection: last.questionProjection,
                    sourceParts: last.sourceParts ?? unit.message.sourceParts)
                displayed[displayed.count - 1] = combined
            } else {
                displayed.append(AIMessage(id: unit.message.id, role: unit.message.role, created: unit.message.created,
                    completed: unit.message.completed, active: unit.message.active, parts: unit.parts,
                    sessionId: unit.message.sessionId, questionProjection: unit.message.questionProjection,
                    sourceParts: unit.message.sourceParts))
                counts[unit.message.id, default: 0] += 1
            }
        }
        var remaining = counts
        var seen = Set<String>()
        let originals = Dictionary(source.map { ($0.id, $0) }, uniquingKeysWith: { first, _ in first })
        for index in displayed.indices {
            let id = displayed[index].id
            remaining[id, default: 1] -= 1
            displayed[index].isLastDisplayFragment = remaining[id] == 0
            if !seen.insert(id).inserted {
                displayed[index].displayId = id + ":segment:" + (displayed[index].parts.first?.id ?? "empty")
            }
            if (counts[id] ?? 0) > 1, let original = originals[id] {
                // Earlier fragments depend only on media ownership, not on text still streaming later.
                let fullSource = original.sourceParts ?? original.parts
                displayed[index].sourceParts = displayed[index].isLastDisplayFragment ? fullSource
                    : fullSource.filter { $0.kind == "image" || $0.kind == "video" || $0.kind == "tool" }
            }
        }
        return displayed
    }

    private var currentAcceptedQuestionAnswers: [AcceptedQuestionAnswer] {
        let conversation = outgoingConversationID(session: selected, remote: selectedRemote)
        return acceptedQuestionAnswers.values.filter {
            $0.owner.accountID == screenFrameCurrentAccountID && $0.owner.conversationID == conversation
        }
    }

    func questionAnswerEchoes(for message: AIMessage) -> [String: String] {
        let parts = Set(message.parts.map(\.id))
        return Dictionary(currentAcceptedQuestionAnswers.compactMap { echo in
            let part = echo.question.partId ?? "question-part-" + echo.question.id
            return parts.contains(part) ? (part, echo.text) : nil
        }, uniquingKeysWith: { _, newest in newest })
    }

    private func reconcileQuestionAnswerEchoes() {
        guard !acceptedQuestionAnswers.isEmpty else { return }
        let finished = Set((messages + questionMessages + updatedRootMessages).flatMap { $0.parts }
            .filter { $0.questionAnswer != nil }.map(\.id))
        let conversation = outgoingConversationID(session: selected, remote: selectedRemote)
        let retained = acceptedQuestionAnswers.filter { _, echo in
            echo.owner.accountID != screenFrameCurrentAccountID || echo.owner.conversationID != conversation ||
                echo.question.partId.map { !finished.contains($0) } != false
        }
        if retained.count != acceptedQuestionAnswers.count { acceptedQuestionAnswers = retained }
    }

    private func clearQuestionConversationState() {
        questionAnswerReservations.removeAll()
        questionAnswerReservationOrder.removeAll()
        acceptedQuestionAnswers.removeAll()
        acceptedQuestionAnswerOrder.removeAll()
        updatedRootMessages.removeAll()
    }
    var hasOlderVisibleMessages: Bool {
        messages.count > visibleMessageLimit || hasOlderMessages
    }

    #if DEBUG
    // DebugSupport 通过这一小型边界设置隔离状态，正式状态的 setter 继续保持私有。
    let uiTestSupport = AIAssistantModelUITestSupport()
    #if targetEnvironment(simulator)
    func publishSessionModelUITestState(_ data: Data) async throws {
        guard ProcessInfo.processInfo.arguments.contains("--ai-remote-session-model-ui-test") else { return }
        let state = try JSONDecoder().decode(AIResponse.self, from: data)
        try await publishLocalPage(state, sessionID: state.session ?? "", backfill: false)
    }

    func reconnectSessionModelUITestPeer() async {
        guard ProcessInfo.processInfo.arguments.contains("--ai-remote-session-model-ui-test") else { return }
        await remoteConfigurationConnectionChanged(connected: false)
        await remoteConfigurationConnectionChanged(connected: true)
    }

    /// Feeds an owned fake peer's receipt through the exact production tuple/ACK decoder.
    func deliverSessionModelUITestReceipt(owner: AIRemoteSession, id: String, result: [String: Any]) async {
        guard ProcessInfo.processInfo.arguments.contains("--ai-remote-session-model-ui-test") else { return }
        let payload: [String: Any] = ["type": "command_result", "command_id": id, "result": result]
        let value: [String: Any] = ["id": 1, "kind": "event", "session_id": owner.sessionID,
            "client_message_id": id, "from_slot": owner.deviceSlot,
            "from_login_session_id": owner.loginSessionID, "payload": payload]
        guard let data = try? JSONSerialization.data(withJSONObject: value),
              let envelope = try? JSONDecoder().decode(AIRemoteSessionTransport.Envelope.self, from: data),
              let content = try? JSONSerialization.data(withJSONObject: payload) else { return }
        await applyRemotePayload(content, envelope: envelope)
    }

    func prepareManualCameraImportForUITesting(_ workspace: URL) async throws {
        try await backend.prepareManualCameraImportForUITesting(workspace)
    }
    #endif
    func applyUITestMutation(_ mutation: AIAssistantUITestMutation) {
        switch mutation {
        case .workspace(let path): workspacePath = path
        case .remoteSessions(let values): remoteSessions = values
        case .clearRemoteSelection: selectedRemote = nil
        case .remoteImagePage(let data, let remote):
            pendingRemoteSelection = remote
            applyRemoteHistory(data, source: remote, older: false)
        case .attachments(let files, let firstSession):
            pendingAttachments.merge(files) { _, replacement in replacement }
            firstMessageSessionID = firstSession
        case .selectedHistory(let id, let page):
            selectionGeneration &+= 1
            selected = id
            messages = page
            visibleMessageLimit = Self.recentMessageCount
            historyLoaded = true
            scrollRequest += 1
        }
    }
    #endif

    func setComposerTyping(_ active: Bool) { composerTyping = active }


    func suggestReplies(messages: [[String: String]]) async throws -> AIReplySuggestions {
        let owner = AIMobileHostToolProvider.shared.appState?.masterUserID ?? ""
        return try await backend.suggestReplies(messages: messages, accountUserID: owner)
    }

    func showTransientError(_ message: String, duration: Duration = .seconds(3)) {
        transientErrorTask?.cancel()
        error = message
        transientErrorTask = Task { @MainActor [weak self] in
            try? await Task.sleep(for: duration)
            guard !Task.isCancelled, let self, self.error == message else { return }
            self.error = ""
            self.transientErrorTask = nil
        }
    }

    func prepareFirstPage() async {
        if ready && pendingInitialPage != nil { return }
        if historyLoaded {
            logAIHistoryEvent("prepare skip selected=\(selected) count=\(messages.count)")
            return
        }
        if let initialPreparation {
            await initialPreparation.value
            return
        }
        let task = Task { @MainActor in
            do {
                let owner = AIMobileHostToolProvider.shared.appState?.masterUserID ?? ""
                let opened = try await backend.open(accountUserID: owner)
                settings = opened.settings
                workspacePath = opened.workspacePath
                ready = true
                if let configurationError = opened.configurationError {
                    configured = false
                    historyLoaded = true
                    error = configurationError
                    return
                }
                if selected.isEmpty {
                    let summary = try await backend.snapshot(session: "", limit: 0,
                                                             force: true)
                    sessions = summary.sessions ?? []
                    configured = summary.configured ?? false
                    actualModelID = summary.model ?? ""
                    actualWire = summary.wire ?? ""
                    actualGPUSelected = summary.localGpuSelected ?? false
                    actualGPUModelBytes = summary.localGpuModelBytes ?? 0
                    actualGPUBackend = summary.localGpuBackend ?? ""
                    actualBackendHasError = !(summary.error ?? "").isEmpty
                    selected = sessions.first?.id ?? ""
                }
                await restoreOutgoingMessages()
                logAIHistoryEvent("prepare selected=\(selected) sessions=\(sessions.count)")
                guard !selected.isEmpty else { historyLoaded = true; return }
                let session = selected
                if let newestID = try await showNewestMessage(for: session) {
                    pendingInitialPage = (session, newestID)
                }
            } catch {
                logAIHistoryEvent("prepare error=\(error.localizedDescription)")
                self.error = error.localizedDescription
            }
        }
        initialPreparation = task
        await task.value
        initialPreparation = nil
    }

    func appear() {
        visible = true
        guard appearingTask == nil else { return }
        appearingTask = Task { @MainActor in
            await prepareFirstPage()
            if let pending = pendingInitialPage {
                pendingInitialPage = nil
                await completeInitialPage(for: pending.session, newestID: pending.newestID)
            } else if historyLoaded {
                await refresh(force: true)
            }
            startPolling()
            startRemotePolling()
            appearingTask = nil
        }
    }

    func startSessionSyncForAccount() {
        startRemotePolling()
        guard hostStartup == nil else { return }
        hostStartup = Task { @MainActor [weak self] in
            guard let self else { return }
            await self.prepareFirstPage()
            if self.ready { self.lastRemotePresence = .distantPast }
            self.hostStartup = nil
        }
    }
    func disappear() {
        visible = false
        composerTyping = false
        poll?.cancel()
        poll = nil
    }

    private func stopRemoteSync() {
        latestModelCommandByRemote.removeAll()
        pendingModelOwners.removeAll()
        remoteImageFiles.removeAll()
        remotePoll?.cancel()
        remotePoll = nil
        remotePushTask?.cancel()
        remotePushTask = nil
        remoteBackfillTask?.cancel()
        remoteBackfillTask = nil
        pendingBackfillSessions.removeAll()
        pendingRemotePushSessions.removeAll()
        remotePublishRoots.removeAll()
        initiallyPublishedSessions.removeAll()
        publishedRemoteMessageDigests.removeAll()
        publishedRemoteStateDigests.removeAll()
        remoteCommandsInProgress.removeAll()
        remoteOptimisticByCommandID.removeAll()
        for request in pendingLifecycle.values {
            request.continuation.resume(throwing:
                AIBackendError(message: "设备同步连接已关闭"))
        }
        pendingLifecycle.removeAll()
        for request in remoteSteeringConfirmations.values {
            request.continuation.resume(throwing: AIBackendError(
                message: "设备同步连接已关闭，补充内容仍保留在草稿中", definitive: false))
        }
        remoteSteeringConfirmations.removeAll()
        remoteSteeringImports.removeAll()
        remoteSteeringImportRevision &+= 1
        lifecycleRetryIDs.removeAll()
        remoteSessionMutationInProgress = false
        remoteDevices = []
        Task { await remoteTransport.stop() }
    }

    fileprivate func localSessionChanged(_ sessionID: String) {
        guard ready else { return }
        pendingRemotePushSessions.insert(sessionID)
        guard remotePushTask == nil else { return }
        remotePushTask = Task { [weak self] in
            guard let self else { return }
            while !Task.isCancelled {
                try? await Task.sleep(for: .milliseconds(150))
                guard !Task.isCancelled else { break }
                let changed = self.pendingRemotePushSessions
                self.pendingRemotePushSessions.removeAll()
                if changed.isEmpty { break }
                await self.pushLocalSessions(changed)
                if self.pendingRemotePushSessions.isEmpty { break }
            }
            self.remotePushTask = nil
        }
    }

    private func pushLocalSessions(_ changed: Set<String>) async {
        for sessionID in changed {
            if remoteCommandsInProgress.contains(sessionID) {
                pendingRemotePushSessions.insert(sessionID)
                continue
            }
            var publishSessionID = sessionID
            do {
                var rootOwner = remotePublishRoots[sessionID]
                if rootOwner == nil {
                    // 归属查询不消费 producer 的窗口外更新队列。
                    let owner = try await backend.remoteSnapshot(session: sessionID, limit: 1)
                    rootOwner = owner.rootSessionId ?? sessionID
                    if remotePublishRoots.count >= 256 { remotePublishRoots.removeAll() }
                    remotePublishRoots[sessionID] = rootOwner
                }
                let root = rootOwner ?? sessionID
                if root.isEmpty { continue }
                publishSessionID = root
                if root != sessionID {
                    if remoteCommandsInProgress.contains(root) {
                        pendingRemotePushSessions.insert(root)
                        continue
                    }
                }
                // 仅真正的根房间 publisher 注册并消费独立的 remote 更新队列。
                let state = try await backend.remoteSnapshot(session: root, limit: 20, includeMessageUpdates: true)
                try await publishLocalPage(state, sessionID: root, backfill: false)
            } catch {
                initiallyPublishedSessions.remove(publishSessionID)
                logAIHistoryEvent("remote-push snapshot failed: \(error.localizedDescription)")
            }
        }
    }

    private func publishSessionConfigurationFrame(sessionID: String, data: Data) async throws {
        #if DEBUG && targetEnvironment(simulator)
        if ProcessInfo.processInfo.arguments.contains("--ai-remote-session-model-ui-test"),
           let handler = uiTestSupport.sessionModelPublishHandler { try await handler(sessionID, data); return }
        #endif
        try await remoteTransport.publish(sessionID: sessionID, data: data)
    }

    private func publishLocalPage(_ state: AIResponse, sessionID: String,
                                  backfill: Bool) async throws {
        let value = try remoteHistoryPayload(state)
        let messages = value["messages"] as? [[String: Any]] ?? []
        for message in messages {
            guard let messageID = message["id"] as? String else { continue }
            let messageBytes = try JSONSerialization.data(
                withJSONObject: message, options: .sortedKeys)
            let messageDigest = SHA256.hash(data: messageBytes)
                .map { String(format: "%02x", $0) }.joined()
            let key = sessionID + ":" + messageID
            guard publishedRemoteMessageDigests[key] != messageDigest else { continue }
            var frame = value
            frame["session_id"] = sessionID
            frame["messages"] = [message]
            let published = try boundedQuestionHistoryPayload(frame)
            let encoded = try JSONSerialization.data(withJSONObject: published, options: .sortedKeys)
            try await publishSessionConfigurationFrame(sessionID: sessionID, data: encoded)
            publishedRemoteMessageDigests[key] = messageDigest
            publishedRemoteStateDigests[sessionID] = try remotePublishedStateDigest(published)
            if backfill { try await Task.sleep(for: .milliseconds(250)) }
        }
        // 大根消息可能暂时裁掉投影；紧接着用无正文的小状态帧恢复，历史和当前问题不被截断。
        var frame = value
        frame["session_id"] = sessionID
        frame["messages"] = []
        let published = try boundedQuestionHistoryPayload(frame)
        let stateDigest = try remotePublishedStateDigest(published)
        if publishedRemoteStateDigests[sessionID] != stateDigest {
            let encoded = try JSONSerialization.data(withJSONObject: published, options: .sortedKeys)
            try await publishSessionConfigurationFrame(sessionID: sessionID, data: encoded)
            publishedRemoteStateDigests[sessionID] = stateDigest
        }
        if state.hasMessageUpdates == true { localSessionChanged(sessionID) }
    }

    private func boundedQuestionHistoryPayload(_ source: [String: Any]) throws -> [String: Any] {
        var payload = source
        var projections = payload["questionMessages"] as? [[String: Any]] ?? []
        // 只裁显示副本，完整根正文与 pending question metadata 保留。留出 framing/session ID 的余量。
        while !projections.isEmpty,
              try JSONSerialization.data(withJSONObject: payload).count > 95 * 1024 {
            projections.removeFirst()
            payload["questionMessages"] = projections
        }
        return payload
    }

    private func remotePublishedStateDigest(_ payload: [String: Any]) throws -> String {
        let state: [String: Any] = ["permissions": payload["permissions"] ?? [], "questions": payload["questions"] ?? [],
            "questionMessages": payload["questionMessages"] ?? [], "busy": payload["busy"] ?? false,
            "turnId": payload["turnId"] ?? "", "session": payload["session"] ?? "",
            "model": payload["model"] ?? "", "reasoningEffort": payload["reasoningEffort"] ?? "",
            "wire": payload["wire"] ?? "", "defaultReasoningEffort": payload["defaultReasoningEffort"] ?? ""]
        let bytes = try JSONSerialization.data(withJSONObject: state, options: .sortedKeys)
        return SHA256.hash(data: bytes).map { String(format: "%02x", $0) }.joined()
    }

    private func scheduleBackfill(_ sessionID: String) {
        guard !pendingBackfillSessions.contains(sessionID) else { return }
        pendingBackfillSessions.append(sessionID)
        guard remoteBackfillTask == nil else { return }
        remoteBackfillTask = Task { [weak self] in
            guard let self else { return }
            try? await Task.sleep(for: .seconds(2))
            while !Task.isCancelled && !self.pendingBackfillSessions.isEmpty {
                let sessionID = self.pendingBackfillSessions.removeFirst()
                do {
                    let latest = try await self.backend.remoteSnapshot(
                        session: sessionID, limit: 20)
                    guard let current = latest.messages, current.count == 20,
                          let initialCursor = current.last?.id else { continue }
                    var before = initialCursor
                    repeat {
                        let page = try await self.backend.remoteSnapshot(
                            session: sessionID, beforeId: before, limit: 20)
                        guard let messages = page.messages, !messages.isEmpty else { break }
                        try await self.publishLocalPage(page, sessionID: sessionID,
                                                        backfill: true)
                        before = messages.last?.id ?? ""
                        if messages.count < 20 { break }
                    } while !Task.isCancelled && !before.isEmpty
                } catch {
                    self.initiallyPublishedSessions.remove(sessionID)
                    logAIHistoryEvent("room backfill failed: \(error.localizedDescription)")
                }
            }
            self.remoteBackfillTask = nil
        }
    }
    func setPageActive(_ active: Bool) {
        pageActive = active
        if active && poll == nil && ready { appear() }
    }
    private func startPolling() {
        poll?.cancel()
        guard visible else { return }
        poll = Task { [weak self] in
            while !Task.isCancelled {
                let busy = self?.sessions.contains(where: \.busy) == true
                // 用户正在输入时把流式界面刷新降到每 250 毫秒一次，优先留出键盘事件时间。
                // 模型和工具继续后台工作；仅合并显示增量，不丢失任何输出。
                let typing = self?.composerTyping == true
                let interval: Duration = self?.pageActive == true
                    ? .milliseconds(busy ? (typing ? 250 : 100) : 500)
                    : .milliseconds(busy ? 500 : 2_000)
                try? await Task.sleep(for: interval)
                guard !Task.isCancelled, let self else { return }
                await self.refresh(force: false)
            }
        }
    }

    private var syncDeviceSlot: String {
        UIDevice.current.userInterfaceIdiom == .pad ? "tablet" : "ios_phone"
    }

    private var syncDeviceID: String {
        let key = "maichat-session-sync-device-id"
        if let saved = UserDefaults.standard.string(forKey: key),
           saved.count == 32, saved.allSatisfy({ $0.isHexDigit }) { return saved }
        let created = UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased()
        UserDefaults.standard.set(created, forKey: key)
        return created
    }

    private func applyRemoteCatalog(_ catalog: [AIRemoteSession],
                                    devices: [AIRemoteDevice]) {
        let previous = remoteSessions
        remoteSessions = catalog.filter { $0.deviceSlot != syncDeviceSlot }.map { discovered in
            var current = discovered
            // catalog 只更新发现/在线状态，不能冲掉同一 owner login 已回读的会话配置。
            if let known = previous.first(where: { $0.id == discovered.id && $0.loginSessionID == discovered.loginSessionID }) {
                current.model = known.model
                current.reasoningEffort = known.reasoningEffort
                current.wire = known.wire
            }
            return current
        }
        remoteDevices = devices.filter { $0.deviceSlot != syncDeviceSlot }
        if let selected = selectedRemote,
           let updated = remoteSessions.first(where: {
               $0.id == selected.id && $0.loginSessionID == selected.loginSessionID
           }) {
            selectedRemote = updated
            // The socket may reconnect before the fresh catalog marks this owner online.
            // Retry only the idempotent config read; never replay a model mutation.
            if updated.online && updated.model == nil {
                Task { @MainActor [weak self] in await self?.requestRemoteConfiguration(updated) }
            }
        } else if let selected = selectedRemote,
                  remoteDevices.contains(where: {
                      $0.deviceSlot == selected.deviceSlot && $0.online
                  }) {
            selectedRemote = nil
            pendingRemoteSelection = nil
            self.selected = ""
            messages = []
            permissions = []
            questions = []
            questionMessages = []
            updatedRootMessages = []
            historyLoaded = true
            scrollRequest += 1
        }
    }

    private func remoteConfigurationConnectionChanged(connected: Bool, sequence: Int? = nil) async {
        guard self === Self.shared, !outgoingPermanentlyInvalidated else { return }
        if let sequence {
            guard sequence > remoteConfigurationConnectionSequence else { return }
            remoteConfigurationConnectionSequence = sequence
        }
        // A previous transport's delayed receipt cannot commit settings after reconnect.
        // Writes are never replayed automatically; get_session resolves the owner's actual state.
        for (id, _) in pendingModelOwners {
            pendingRemoteCommands.removeValue(forKey: id)
            if let fingerprint = remoteFingerprintByCommandID.removeValue(forKey: id) {
                remoteRetryIDByFingerprint.removeValue(forKey: fingerprint)
            }
        }
        pendingModelOwners.removeAll()
        latestModelCommandByRemote.removeAll()
        for index in remoteSessions.indices {
            remoteSessions[index].model = nil
            remoteSessions[index].reasoningEffort = nil
            remoteSessions[index].wire = nil
        }
        if var remote = selectedRemote {
            remote.model = nil; remote.reasoningEffort = nil; remote.wire = nil
            selectedRemote = remote
            if connected { await requestRemoteConfiguration(remote) }
        }
    }

    private func requestRemoteConfiguration(_ remote: AIRemoteSession) async {
        guard remoteIsOnline(remote), !pendingModelOwners.values.contains(where: {
            $0.owner.id == remote.id && $0.owner.loginSessionID == remote.loginSessionID
        }) else { return }
        let generation = submissionGeneration
        let account = screenFrameCurrentAccountID
        do { _ = try await enqueueRemoteCommand(to: remote, payload: ["action": "get_session"]) }
        catch {
            guard generation == submissionGeneration, account == screenFrameCurrentAccountID,
                  selectedRemote?.id == remote.id, selectedRemote?.loginSessionID == remote.loginSessionID else { return }
            showTransientError(error.localizedDescription, duration: .seconds(8))
        }
    }

    private func startRemotePolling() {
        guard remotePoll == nil else { return }
        remotePoll = Task { [weak self] in
            guard let self else { return }
            await self.remoteTransport.setConnectionHandler { [weak self] connected, sequence in
                Task { @MainActor [weak self] in
                    await self?.remoteConfigurationConnectionChanged(connected: connected, sequence: sequence)
                }
            }
            await self.remoteTransport.setCatalogHandler { [weak self] catalog, devices in
                Task { @MainActor [weak self] in
                    self?.applyRemoteCatalog(catalog, devices: devices)
                }
            }
            while !Task.isCancelled {
                guard AIMobileHostToolProvider.shared.appState?.hasUnifiedAccount == true else {
                    try? await Task.sleep(for: .seconds(2))
                    continue
                }
                do {
                    if Date().timeIntervalSince(self.lastRemotePresence) >= 15 {
                        if self.ready, (!self.visible || self.selectedRemote != nil),
                           let summary = try? await self.backend.snapshot(
                               session: "", limit: 0, force: true),
                           let localSessions = summary.sessions {
                            self.sessions = localSessions
                        }
                        let name = String(String(aiSyncDeviceDisplayName().prefix(40))
                            .unicodeScalars.filter {
                                !CharacterSet.controlCharacters.contains($0)
                            })
                        try await self.remoteTransport.presence(
                            deviceID: self.syncDeviceID,
                            name: name.isEmpty ? "iPhone" : name,
                            sessions: self.ready ? self.sessions : []
                        )
                        self.lastRemotePresence = Date()
                        let currentIDs = Set(self.sessions.map(\.id))
                        let removedSessions = self.ready
                            ? self.initiallyPublishedSessions.subtracting(currentIDs) : Set<String>()
                        for removed in removedSessions {
                            try await self.remoteTransport.deleteRoom(sessionID: removed)
                            do { try await self.cleanupLocalSessionAttachments(removed) }
                            catch {
                                logAIHistoryEvent("session attachment cleanup retry: \(error.localizedDescription)")
                                continue
                            }
                            self.initiallyPublishedSessions.remove(removed)
                            self.pendingBackfillSessions.removeAll { $0 == removed }
                            self.publishedRemoteStateDigests.removeValue(forKey: removed)
                            self.publishedRemoteMessageDigests =
                                self.publishedRemoteMessageDigests.filter {
                                    !$0.key.hasPrefix(removed + ":")
                                }
                        }
                        for session in self.sessions where
                            !self.initiallyPublishedSessions.contains(session.id) {
                            self.initiallyPublishedSessions.insert(session.id)
                            self.localSessionChanged(session.id)
                            self.scheduleBackfill(session.id)
                        }
                    }
                    let envelopes = try await self.remoteTransport.poll()
                    for envelope in envelopes {
                        await self.handleRemoteEnvelope(envelope)
                    }
                    self.expireRemoteRequests()
                } catch {
                    logAIHistoryEvent("session-sync failed: \(error.localizedDescription)")
                    self.initiallyPublishedSessions.removeAll()
                    self.publishedRemoteMessageDigests.removeAll()
                    self.publishedRemoteStateDigests.removeAll()
                    self.expireRemoteRequests()
                    try? await Task.sleep(for: .seconds(2))
                }
            }
        }
    }

    private func expireRemoteRequests() {
        let timedOut = pendingRemoteCommands.filter {
            Date().timeIntervalSince($0.value) >
                (attachmentRemoteCommands.contains($0.key) ? 2 * 60 * 60 : 30)
        }
        if !timedOut.isEmpty {
            var notifyCurrentRoom = false
            for id in timedOut.keys {
                pendingRemoteCommands.removeValue(forKey: id)
                if let pending = pendingModelOwners.removeValue(forKey: id) {
                    if latestModelCommandByRemote[pending.owner.id] == id {
                        latestModelCommandByRemote.removeValue(forKey: pending.owner.id)
                    }
                    notifyCurrentRoom = notifyCurrentRoom ||
                        (selectedRemote?.id == pending.owner.id && selectedRemote?.loginSessionID == pending.owner.loginSessionID)
                } else { notifyCurrentRoom = true }
                attachmentRemoteCommands.remove(id)
                if let pending = remoteFilesByCommandID.removeValue(forKey: id) {
                    pendingAttachments[pending.key, default: []].append(contentsOf: pending.files)
                }
                if let temporaryID = remoteOptimisticByCommandID.removeValue(forKey: id) {
                    messages.removeAll { $0.id == temporaryID }
                }
                if let draft = remoteDraftByCommandID.removeValue(forKey: id), !draft.isEmpty {
                    remoteRestoreText = draft
                    remoteRestoreGeneration += 1
                }
            }
            if notifyCurrentRoom {
                showTransientError(
                    "所属设备是否执行了操作尚未确定，请先查看会话记录，不要直接重复发送",
                    duration: .seconds(12))
            }
        }
        if pendingRemoteSelection != nil,
           Date().timeIntervalSince(pendingRemoteSelectionStarted) > 20 {
            pendingRemoteSelection = nil
            showTransientError("读取所属设备会话超时，请重新选择")
        }
    }

    private func remoteIsOnline(_ remote: AIRemoteSession) -> Bool {
        remoteSessions.contains {
            $0.id == remote.id && $0.loginSessionID == remote.loginSessionID && $0.online
        }
    }

    func selectRemoteConversation(_ remote: AIRemoteSession) async {
        logAIHistoryEvent("remote-select-request slot=\(remote.deviceSlot) online=\(remoteIsOnline(remote))")
        guard remoteSessions.contains(where: { $0.id == remote.id }) else {
            showTransientError("这段会话已不在设备目录中")
            return
        }
        if selectedRemote?.id == remote.id &&
            selectedRemote?.loginSessionID == remote.loginSessionID {
            scrollRequest += 1
            await requestRemoteConfiguration(remote)
            return
        }
        let generation = submissionGeneration
        let account = screenFrameCurrentAccountID
        selectionGeneration &+= 1
        let selection = selectionGeneration
        pendingRemoteSelection = remote
        pendingRemoteSelectionStarted = Date()
        do {
            let data: Data
            #if DEBUG && targetEnvironment(simulator)
            if ProcessInfo.processInfo.arguments.contains("--ai-remote-session-model-ui-test"),
               let handler = uiTestSupport.sessionModelHistoryHandler { data = try await handler(remote) }
            else { data = try await remoteTransport.history(ownerSlot: remote.deviceSlot,
                sessionID: remote.sessionID, before: "", limit: Self.recentMessageCount) }
            #else
            data = try await remoteTransport.history(ownerSlot: remote.deviceSlot,
                sessionID: remote.sessionID, before: "", limit: Self.recentMessageCount)
            #endif
            guard generation == submissionGeneration, account == screenFrameCurrentAccountID,
                  selection == selectionGeneration, pendingRemoteSelection?.id == remote.id,
                  pendingRemoteSelection?.loginSessionID == remote.loginSessionID else { return }
            applyRemoteHistory(data, source: remote, older: false)
            if selectedRemote?.id == remote.id, selectedRemote?.loginSessionID == remote.loginSessionID {
                await requestRemoteConfiguration(remote)
            }
        } catch {
            guard generation == submissionGeneration, account == screenFrameCurrentAccountID,
                  selection == selectionGeneration, pendingRemoteSelection?.id == remote.id,
                  pendingRemoteSelection?.loginSessionID == remote.loginSessionID else { return }
            pendingRemoteSelection = nil
            logAIHistoryEvent("remote-select-error \(error.localizedDescription)")
            showTransientError(error.localizedDescription, duration: .seconds(8))
        }
    }

    private func handleRemoteEnvelope(_ envelope: AIRemoteSessionTransport.Envelope) async {
        logAIHistoryEvent("remote-envelope kind=\(envelope.kind) slot=\(envelope.fromSlot)")
        defer {
            if envelope.kind != "command" { Task { try? await remoteTransport.acknowledge(envelope.id) } }
        }
        if envelope.sessionID == Self.lifecycleSessionID {
            if envelope.kind == "command" {
                await applyRemoteCommand(envelope)
            } else if envelope.kind == "event",
                      envelope.payload.type == "command_result",
                      let commandID = envelope.payload.commandID,
                      let result = envelope.payload.result,
                      let pending = pendingLifecycle[commandID],
                      pending.deviceSlot == envelope.fromSlot,
                      pending.loginSessionID == envelope.fromLoginSessionID {
                finishLifecycle(commandID, sessionID: result.ok ? result.sessionID : nil,
                    error: result.ok ? nil : AIBackendError(
                        message: result.error ?? "所属设备未完成操作"),
                    definitive: true)
            }
            return
        }
        if envelope.kind == "command" {
            await applyRemoteCommand(envelope)
            return
        }
        guard envelope.kind == "event",
              let transferID = envelope.payload.transferID,
              let index = envelope.payload.chunkIndex,
              let count = envelope.payload.chunkCount,
              let digest = envelope.payload.sha256,
              let encoded = envelope.payload.dataBase64,
              (1...128).contains(count), (0..<count).contains(index),
              let chunk = Data(base64Encoded: encoded), chunk.count <= 24 * 1024 else { return }
        remoteTransfers = remoteTransfers.filter {
            Date().timeIntervalSince($0.value.started) < 120
        }
        var transfer = remoteTransfers[transferID] ?? AIRemoteTransfer(
            sessionID: envelope.sessionID, fromSlot: envelope.fromSlot,
            fromLoginSessionID: envelope.fromLoginSessionID,
            kind: envelope.kind, count: count, sha256: digest)
        guard transfer.sessionID == envelope.sessionID,
              transfer.fromSlot == envelope.fromSlot,
              transfer.fromLoginSessionID == envelope.fromLoginSessionID,
              transfer.kind == envelope.kind, transfer.count == count,
              transfer.sha256 == digest else { return }
        transfer.chunks[index] = chunk
        if transfer.chunks.count < count {
            remoteTransfers[transferID] = transfer
            return
        }
        remoteTransfers.removeValue(forKey: transferID)
        let data = (0..<count).reduce(into: Data()) { result, part in
            if let bytes = transfer.chunks[part] { result.append(bytes) }
        }
        guard data.count <= 3 * 1024 * 1024,
              SHA256.hash(data: data).map({ String(format: "%02x", $0) }).joined() == digest
        else { return }
        await applyRemotePayload(data, envelope: envelope)
    }

    private func applyRemotePayload(_ data: Data,
                                    envelope: AIRemoteSessionTransport.Envelope) async {
        // A receipt belongs to the captured room even when the user has switched
        // conversations. Only the caller decides whether its draft may be cleared.
        if let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
           object["type"] as? String == "command_result",
           let id = object["command_id"] as? String,
           let pending = remoteSteeringConfirmations[id],
           pending.remote.sessionID == envelope.sessionID,
           pending.remote.deviceSlot == envelope.fromSlot,
           pending.remote.loginSessionID == envelope.fromLoginSessionID {
            let result = object["result"] as? [String: Any]
            let unknownDelivery = result?["unknown"] as? Bool == true ||
                result?["delivery_unknown"] as? Bool == true
            if !unknownDelivery, result?["ok"] as? Bool == true,
               let questionID = pending.questionID,
               result?["question_id"] as? String == questionID {
                // 问题回答确认的是原问题，不伪造普通用户消息 ID。
                finishRemoteSteering(id, result: .success(questionID))
            } else if pending.questionID == nil, !unknownDelivery, result?["ok"] as? Bool == true,
               let messageID = result?["user_message_id"] as? String, !messageID.isEmpty {
                finishRemoteSteering(id, result: .success(messageID))
            } else {
                finishRemoteSteering(id, result: .failure(AIBackendError(
                    message: result?["error"] as? String ??
                        "所属设备未确认补充内容；请更新所属设备后重试",
                    definitive: result?["ok"] as? Bool == false && !unknownDelivery)))
            }
            return
        }
        if let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
           object["type"] as? String == "command_result",
           let id = object["command_id"] as? String, let pending = pendingModelOwners[id] {
            let owner = pending.owner
            guard pending.generation == submissionGeneration, pending.account == screenFrameCurrentAccountID,
                  owner.sessionID == envelope.sessionID, owner.deviceSlot == envelope.fromSlot,
                  owner.loginSessionID == envelope.fromLoginSessionID else { return }
            guard pendingRemoteCommands[id] != nil,
                  let result = object["result"] as? [String: Any], let success = result["ok"] as? Bool else { return }
            // 先验证完整回执，再消费请求。错误 owner、错误型号/等级或旧回执不能吞掉正确确认。
            if success {
                guard result["session"] as? String == owner.sessionID,
                      let model = result["model"] as? String,
                      let effort = result["reasoningEffort"] as? String,
                      let wire = result["wire"] as? String,
                      validRemoteModelConfiguration(model: model, effort: effort, wire: wire),
                      pending.model == nil || pending.model == model,
                      pending.reasoningEffort == nil || effort == pending.reasoningEffort else { return }
            }
            pendingRemoteCommands.removeValue(forKey: id)
            pendingModelOwners.removeValue(forKey: id)
            if remoteDeliveryPendingIDs.contains(id) { remoteResultByCommandID[id] = success }
            if let fingerprint = remoteFingerprintByCommandID.removeValue(forKey: id) {
                remoteRetryIDByFingerprint.removeValue(forKey: fingerprint)
            }
            guard latestModelCommandByRemote[owner.id] == id else { return }
            latestModelCommandByRemote.removeValue(forKey: owner.id)
            guard success else {
                if selectedRemote?.id == owner.id && selectedRemote?.loginSessionID == owner.loginSessionID {
                    showTransientError(result["error"] as? String ?? "所属设备未保存模型配置", duration: .seconds(8))
                }
                return
            }
            let model = result["model"] as? String ?? ""
            let effort = result["reasoningEffort"] as? String ?? ""
            let wire = result["wire"] as? String ?? ""
            var current = owner
            current.model = model; current.reasoningEffort = effort; current.wire = wire
            if let index = remoteSessions.firstIndex(where: { $0.id == owner.id && $0.loginSessionID == owner.loginSessionID }) {
                remoteSessions[index] = current
            }
            if selectedRemote?.id == owner.id && selectedRemote?.loginSessionID == owner.loginSessionID {
                selectedRemote = current
                if let fallback = result["defaultReasoningEffort"] as? String, ["low", "high", "max"].contains(fallback) {
                    sessionDefaultReasoningEffort = fallback
                }
                projectSessionConfiguration(model: model, effort: effort, wire: wire)
            }
            return
        }
        let source = pendingRemoteSelection ?? selectedRemote
        guard let source, source.sessionID == envelope.sessionID,
              source.deviceSlot == envelope.fromSlot,
              source.loginSessionID == envelope.fromLoginSessionID else { return }
        if let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
           object["type"] as? String == "command_result" {
            if let id = object["command_id"] as? String,
               pendingRemoteCommands.removeValue(forKey: id) != nil {
                attachmentRemoteCommands.remove(id)
                let pendingDraft = remoteDraftByCommandID.removeValue(forKey: id)
                let result = object["result"] as? [String: Any]
                if let pending = remoteFilesByCommandID.removeValue(forKey: id) {
                    if result?["ok"] as? Bool == true {
                        discardImportedFiles(pending.files)
                    } else {
                        pendingAttachments[pending.key, default: []]
                            .append(contentsOf: pending.files)
                    }
                }
                if let temporaryID = remoteOptimisticByCommandID.removeValue(forKey: id),
                   let position = messages.firstIndex(where: { $0.id == temporaryID }) {
                    if result?["ok"] as? Bool == true,
                       let canonicalID = result?["user_message_id"] as? String,
                       !canonicalID.isEmpty,
                       !messages.contains(where: { $0.id == canonicalID }) {
                        let previous = messages[position]
                        messages[position] = AIMessage(id: canonicalID, role: previous.role,
                            created: previous.created, completed: previous.completed,
                            active: false, parts: previous.parts)
                        messages.sort { $0.id < $1.id }
                    } else {
                        messages.remove(at: position)
                    }
                }
                if remoteDeliveryPendingIDs.contains(id) {
                    remoteResultByCommandID[id] = result?["ok"] as? Bool == true
                }
                if let fingerprint = remoteFingerprintByCommandID.removeValue(forKey: id) {
                    remoteRetryIDByFingerprint.removeValue(forKey: fingerprint)
                }
                if let result, result["ok"] as? Bool != true {
                    if let pendingDraft, !pendingDraft.isEmpty {
                        remoteRestoreText = pendingDraft
                        remoteRestoreGeneration += 1
                    }
                    showTransientError(result["error"] as? String ?? "所属设备未执行操作",
                                       duration: .seconds(8))
                }
                if let warning = result?["warning"] as? String, !warning.isEmpty {
                    showTransientError(warning, duration: .seconds(8))
                }
            }
            return
        }
        guard let page = try? JSONDecoder().decode(AIResponse.self, from: data),
              page.ok else { return }
        applyRemotePage(page, source: source, older: false)
    }

    private func validRemoteModelConfiguration(model: String, effort: String, wire: String) -> Bool {
        guard ["", "low", "high", "max"].contains(effort) else { return false }
        if model.hasPrefix("local:"), model.count > "local:".count { return wire == "local" }
        guard AIModelSettings.cloudModels.contains(model) else { return false }
        return wire == (["glm-5.3", "glm-5.3-flash"].contains(model) ? "chat_completions" : "responses")
    }

    private func applyRemoteHistory(_ data: Data, source: AIRemoteSession,
                                    older: Bool) {
        guard let page = try? JSONDecoder().decode(AIResponse.self, from: data),
              page.ok else { return }
        applyRemotePage(page, source: source, older: older)
    }

    private func applyRemotePage(_ page: AIResponse, source: AIRemoteSession,
                                 older: Bool) {
        var source = source
        // 旧页请求捕获的 discovery 对象可能早于一次模型 ACK；分页不能回退配置。
        if let current = remoteSessions.first(where: { $0.id == source.id && $0.loginSessionID == source.loginSessionID }) {
            source.model = current.model
            source.reasoningEffort = current.reasoningEffort
            source.wire = current.wire
        }
        if let busy = page.busy { source.busy = busy }
        if let turnId = page.turnId { source.turnId = turnId }
        if !older, page.session == source.sessionID,
           let model = page.model, let effort = page.reasoningEffort, let wire = page.wire,
           validRemoteModelConfiguration(model: model, effort: effort, wire: wire),
           !pendingModelOwners.values.contains(where: {
               $0.owner.id == source.id && $0.owner.loginSessionID == source.loginSessionID
           }) {
            source.model = model; source.reasoningEffort = effort; source.wire = wire
            if selectedRemote?.id == source.id || pendingRemoteSelection?.id == source.id,
               let fallback = page.defaultReasoningEffort, ["low", "high", "max"].contains(fallback) {
                sessionDefaultReasoningEffort = fallback
            }
        }
        if let index = remoteSessions.firstIndex(where: {
            $0.id == source.id && $0.loginSessionID == source.loginSessionID
        }) { remoteSessions[index] = source }
        if selectedRemote?.id == source.id || pendingRemoteSelection?.id == source.id {
            if selectedRemote?.id == source.id { selectedRemote = source }
            if let model = source.model {
                projectSessionConfiguration(model: model, effort: source.reasoningEffort, wire: source.wire)
            }
        }
        guard let received = page.messages else { return }
        let displayMessages = received.map(remoteDisplayMessage)
        if pendingRemoteSelection?.id == source.id {
            var transaction = Transaction(animation: nil)
            transaction.disablesAnimations = true
            withTransaction(transaction) {
                selectedRemote = source
                pendingRemoteSelection = nil
                selected = source.sessionID
                messages = Array(displayMessages.reversed())
                updatedRootMessages = []
                questionMessages = (page.questionMessages ?? []).map(remoteDisplayMessage)
                permissions = page.permissions ?? []
                questions = page.questions ?? []
                quotedMessage = nil
                visibleMessageLimit = Self.recentMessageCount
                historyLoaded = true
                hasOlderMessages = received.count == Self.recentMessageCount
                scrollRequest += 1
            }
        } else if selectedRemote?.id == source.id {
            mergeMessages(displayMessages)
            if let permissions = page.permissions { self.permissions = permissions }
            if let questions = page.questions { self.questions = questions }
            if let projections = page.questionMessages { questionMessages = projections.map(remoteDisplayMessage) }
            if older {
                visibleMessageLimit += received.count
                hasOlderMessages = received.count == Self.olderPageCount
            }
        }
        reconcileQuestionAnswerEchoes()
        Task { @MainActor [weak self] in await self?.outgoingPump?.publish() }
    }

    private func remoteDisplayMessage(_ message: AIMessage) -> AIMessage {
        var parts: [AIPart] = []
        for original in message.parts {
            var part = original
            if part.kind == "image" || part.kind == "video" {
                // 所属设备的相对路径不能在本机工作区解析，否则可能显示同名的其他文件。
                part.path = nil
                parts.append(part)
                if part.attachmentID == nil && part.remoteImage == nil { parts.append(AIPart(
                    id: part.id + "-remote-placeholder", kind: "text",
                    text: part.kind == "image" ? "[图片在所属设备，暂不可跨设备预览]" :
                        "[视频在所属设备，暂不可跨设备预览]",
                    tool: nil, input: nil, output: nil, error: nil, state: nil,
                    path: nil, mimeType: nil)) }
            } else {
                parts.append(part)
            }
        }
        return AIMessage(id: message.id, role: message.role,
                         created: message.created, completed: message.completed,
                         active: message.active, parts: parts, sessionId: message.sessionId,
                         questionProjection: message.questionProjection).withoutReasoningText()
    }

    private func remoteHistoryMessage(_ message: AIMessage) -> AIMessage {
        let parts = message.parts.compactMap { original -> AIPart? in
            var part = original
            switch part.kind {
            case "text":
                break
            case "tool":
                // 问题和回答是会话正文；其他工具仍不跨设备发布原始参数和输出。
                if part.tool != "question" { part.input = nil; part.output = nil }
                part.error = nil
            case "image", "video":
                part.path = nil
            default:
                return nil
            }
            return part
        }
        return AIMessage(id: message.id, role: message.role,
                         created: message.created, completed: message.completed,
                         active: message.active, parts: parts, sessionId: message.sessionId,
                         questionProjection: message.questionProjection)
    }

    private func remoteHistoryPayload(_ state: AIResponse) throws -> [String: Any] {
        var page = state.messages ?? []
        // 旁路更新覆盖相同 actual ID，跨设备仍发送原 messages 协议；原 state.messages 页游标不变。
        for updated in state.messageUpdates ?? [] {
            if let index = page.firstIndex(where: { $0.id == updated.id }) { page[index] = updated }
            else { page.append(updated) }
        }
        let safePage = page.map(remoteHistoryMessage)
        let encoded = try JSONEncoder().encode(safePage)
        return ["ok": true, "session": state.session ?? "", "model": state.model ?? "",
                "reasoningEffort": state.reasoningEffort ?? "", "wire": state.wire ?? "",
                "defaultReasoningEffort": state.defaultReasoningEffort ?? "high",
                "busy": state.busy ?? false, "turnId": state.turnId ?? "",
                "messages": try JSONSerialization.jsonObject(with: encoded),
                "questionMessages": try JSONSerialization.jsonObject(
                    with: JSONEncoder().encode((state.questionMessages ?? []).map(remoteHistoryMessage))),
                "permissions": try JSONSerialization.jsonObject(
                    with: JSONEncoder().encode(state.permissions ?? [])),
                "questions": try JSONSerialization.jsonObject(
                    with: JSONEncoder().encode(state.questions ?? []))]
    }

    private func forgetRemoteSteeringImports(session: String) {
        let previous = remoteSteeringImports.count
        remoteSteeringImports = remoteSteeringImports.filter { $0.value.identity.session != session }
        if previous != remoteSteeringImports.count { remoteSteeringImportRevision &+= 1 }
    }

    /// Admission is bounded and conservative. Snapshot reads are native owner
    /// state, not the periodically refreshed UI session list. No file is deleted.
    private func makeRoomForRemoteSteeringImport(ownerGeneration: Int,
                                                 ownerAccount: String,
                                                 ownerLogin: String) async throws {
        for _ in 0..<3 {
            guard submissionGeneration == ownerGeneration, screenFrameCurrentAccountID == ownerAccount else {
                throw AIBackendError(message: "账号已变化，未导入补充附件")
            }
            // Only one not-yet-confirmed import may occupy space. Its existing
            // request can retry; a different request cannot displace old receipts.
            guard !remoteSteeringImports.values.contains(where: { $0.identity.acceptedOrder == nil }) else {
                throw AIBackendError(message: "上一次附件补充尚未确认，请先重试原补充内容")
            }
            guard remoteSteeringImports.count >= AISteeringImportCachePolicy.activeCapacity else { return }
            let revision = remoteSteeringImportRevision
            let entries = remoteSteeringImports.values.map(\.identity)
            var states: [String: AISteeringImportRuntimeState] = [:]
            for session in Set(entries.map(\.session)).sorted() {
                // Failed/missing snapshots cannot establish that a turn ended.
                if let snapshot = try? await backend.remoteSnapshot(session: session, limit: 1),
                   let busy = snapshot.busy, let turnId = snapshot.turnId {
                    states[session] = AISteeringImportRuntimeState(busy: busy, turnId: turnId)
                }
                guard submissionGeneration == ownerGeneration, screenFrameCurrentAccountID == ownerAccount else {
                    throw AIBackendError(message: "账号已变化，未导入补充附件")
                }
                if revision != remoteSteeringImportRevision { break }
            }
            guard revision == remoteSteeringImportRevision else { continue }
            let currentLogin = await remoteTransport.currentLoginSessionID()
            guard submissionGeneration == ownerGeneration, screenFrameCurrentAccountID == ownerAccount,
                  currentLogin == ownerLogin else {
                throw AIBackendError(message: "账号或所属设备登录状态已变化，未导入补充附件")
            }
            guard revision == remoteSteeringImportRevision else { continue }
            guard AISteeringImportCachePolicy.activeCount(entries, states: states) <
                    AISteeringImportCachePolicy.activeCapacity else {
                throw AIBackendError(message: "当前仍有较多未结束的附件任务，请等待部分任务完成后重试")
            }
            if let key = AISteeringImportCachePolicy.evictableKey(entries, states: states) {
                remoteSteeringImports.removeValue(forKey: key)
                remoteSteeringImportRevision &+= 1
            }
            guard remoteSteeringImports.count < AISteeringImportCachePolicy.capacity else {
                throw AIBackendError(message: "附件补充的确认状态尚未释放，请稍后重试")
            }
            return
        }
        throw AIBackendError(message: "所属设备任务状态正在变化，补充附件未发送，请稍后重试")
    }

    private func applyRemoteCommand(_ envelope: AIRemoteSessionTransport.Envelope) async {
        let generation = submissionGeneration
        let account = screenFrameCurrentAccountID
        let key = "\(account):\(generation):\(envelope.sessionID)"
        let receipt = key + ":\(envelope.id)"
        guard ownerCommandIDs.insert(receipt).inserted else { return }
        ownerCommandQueues[key, default: []].append(envelope)
        guard ownerCommandTasks[key] == nil else { return }
        ownerCommandTasks[key] = Task { @MainActor [weak self] in
            guard let self else { return }
            defer {
                self.ownerCommandTasks.removeValue(forKey: key)
                let abandoned = self.ownerCommandQueues.removeValue(forKey: key) ?? []
                for item in abandoned { self.ownerCommandIDs.remove(key + ":\(item.id)") }
            }
            while !Task.isCancelled, self.submissionGeneration == generation,
                  self.screenFrameCurrentAccountID == account,
                  let next = self.ownerCommandQueues[key]?.first {
                await self.performRemoteCommand(next)
                self.ownerCommandQueues[key]?.removeFirst()
                self.ownerCommandIDs.remove(key + ":\(next.id)")
            }
        }
    }

    private func performRemoteCommand(_ envelope: AIRemoteSessionTransport.Envelope) async {
        // 入站消息已经排入所属端队列，收到前一条的模型输出不再阻挡 socket 继续接收。
        // 只有持久接收/明确拒绝后才 ACK 信箱，进程退出时未处理的控制消息仍可重送。
        defer { Task { try? await remoteTransport.acknowledge(envelope.id) } }
        if envelope.sessionID == Self.lifecycleSessionID {
            await applyRemoteLifecycleCommand(envelope)
            return
        }
        guard sessions.contains(where: { $0.id == envelope.sessionID }) else { return }
        let isPrompt = ["prompt", "steer"].contains(envelope.payload.action ?? "")
        let ownerGeneration = submissionGeneration
        let ownerAccount = screenFrameCurrentAccountID
        if isPrompt { remoteCommandsInProgress.insert(envelope.sessionID) }
        defer {
            if isPrompt {
                remoteCommandsInProgress.remove(envelope.sessionID)
                localSessionChanged(envelope.sessionID)
            }
        }
        var result: [String: Any] = ["ok": true]
        var steeringImportKey: String?
        var steeringDispatched = false
        var questionAnswerDispatched = false
        do {
            switch envelope.payload.action {
            case "prompt", "steer":
                let isSteering = envelope.payload.action == "steer"
                guard let text = envelope.payload.text,
                      !text.isEmpty || !(envelope.payload.attachments ?? []).isEmpty,
                      text.utf8.count <= 16 * 1024 else {
                    throw AIBackendError(message: "消息为空或过长")
                }
                let references = envelope.payload.attachments ?? []
                guard references.count <= 10 else {
                    throw AIBackendError(message: "附件数量过多")
                }
                guard let ownLogin = await remoteTransport.currentLoginSessionID() else {
                    throw AIBackendError(message: "会话同步登录状态不可用")
                }
                let turnID = isSteering ? envelope.payload.turnId ?? "" : "input"
                let requestID = envelope.payload.steeringRequestId ?? envelope.clientMessageID
                if isSteering && (turnID.isEmpty || requestID.isEmpty) {
                    throw AIBackendError(message: "补充内容缺少原任务编号，请更新发起设备")
                }
                let room = MaiCloudDriveClient.SessionRoom(
                    ownerSlot: syncDeviceSlot,
                    ownerLoginSessionID: ownLogin,
                    sessionID: envelope.sessionID)
                var images: [AIImportedFile] = []
                var videos: [AIImportedFile] = []
                var documents: [AIImportedFile] = []
                var synced: [(id: String, name: String, kind: String,
                              mimeType: String, path: String)] = []
                let importKey = [ownLogin, envelope.sessionID, turnID, requestID].joined(separator: ":")
                let referenceIDs = references.map(\.attachmentID)
                let cached = remoteSteeringImports[importKey]
                if let cached, cached.references != referenceIDs {
                    throw AIBackendError(message: "补充操作编号不能用于不同附件")
                }
                if !references.isEmpty { steeringImportKey = importKey }
                if cached == nil && !references.isEmpty {
                    try await makeRoomForRemoteSteeringImport(ownerGeneration: ownerGeneration,
                        ownerAccount: ownerAccount, ownerLogin: ownLogin)
                }
                for (index, reference) in references.enumerated() {
                    if let cached {
                        let imported = cached.files[index]
                        synced.append((reference.attachmentID, cached.names[index],
                            cached.kinds[index], cached.mimeTypes[index], imported.relativePath))
                        if imported.isImage { images.append(imported) }
                        else if imported.mimeType.hasPrefix("video/") { videos.append(imported) }
                        else { documents.append(imported) }
                        continue
                    }
                    let downloaded = try await MaiCloudDriveClient.shared
                        .downloadSessionAttachment(id: reference.attachmentID, room: room)
                    defer { try? FileManager.default.removeItem(at: downloaded.fileURL) }
                    var imported = downloaded.mediaType == "image"
                        ? try await backend.importDocument(downloaded.fileURL)
                        : try await backend.importSessionFile(
                            downloaded.fileURL, displayName: downloaded.name)
                    if !isSteering {
                        imported = try await backend.pinInputFile(imported, requestID: requestID,
                                                                 attachmentID: reference.attachmentID)
                    }
                    synced.append((reference.attachmentID, downloaded.name,
                                   downloaded.mediaType, downloaded.contentType,
                                   imported.relativePath))
                    if imported.isImage { images.append(imported) }
                    else if imported.mimeType.hasPrefix("video/") { videos.append(imported) }
                    else { documents.append(imported) }
                }
                if cached == nil && !references.isEmpty {
                    // An import awaited network/file work; recheck capacity and
                    // owner identity before the single synchronous insertion.
                    try await makeRoomForRemoteSteeringImport(ownerGeneration: ownerGeneration,
                        ownerAccount: ownerAccount, ownerLogin: ownLogin)
                    let allFiles = images + videos + documents
                    remoteSteeringImportOrder &+= 1
                    let identity = AISteeringImportCacheIdentity(key: importKey,
                        session: envelope.sessionID, turnId: turnID, order: remoteSteeringImportOrder)
                    remoteSteeringImports[importKey] = RemoteSteeringImport(identity: identity,
                        references: referenceIDs,
                        files: synced.compactMap { item in allFiles.first { $0.relativePath == item.path } },
                        names: synced.map(\.name), kinds: synced.map(\.kind),
                        mimeTypes: synced.map(\.mimeType))
                    remoteSteeringImportRevision &+= 1
                }
                let currentOwnerLogin = await remoteTransport.currentLoginSessionID()
                guard submissionGeneration == ownerGeneration,
                      screenFrameCurrentAccountID == ownerAccount,
                      currentOwnerLogin == ownLogin else {
                    throw AIBackendError(message: "账号或所属设备登录状态已变化，未发送内容")
                }
                var prompt = text
                if !documents.isEmpty {
                    prompt += "\n\n会话附件已保存到本机工作目录：\n" +
                        documents.map { "- \($0.relativePath)" }.joined(separator: "\n")
                }
                let sent: AIResponse
                if isSteering {
                    steeringDispatched = true
                    sent = try await backend.steer(AISteeringRequest(
                        session: envelope.sessionID, turnId: turnID, requestId: requestID,
                        text: prompt, images: images, videos: videos,
                        quoteMessageId: envelope.payload.quoteMessageId ?? ""))
                    if let key = steeringImportKey,
                       remoteSteeringImports[key]?.identity.acceptedOrder == nil {
                        remoteSteeringAcceptedOrder &+= 1
                        remoteSteeringImports[key]?.identity.acceptedOrder = remoteSteeringAcceptedOrder
                        remoteSteeringImportRevision &+= 1
                    }
                } else {
                    steeringDispatched = true
                    sent = try await backend.enqueue(session: envelope.sessionID, requestID: requestID, text: prompt,
                        images: images, videos: videos, quoteMessageId: envelope.payload.quoteMessageId ?? "")
                    guard let id = sent.id, !id.isEmpty else { throw AIBackendError(message: "所属设备未确认接收") }
                    result["user_message_id"] = id
                }
                // Queue receipts identify the persisted user row, independently of
                // whichever model turn is active when the snapshot is read.
                let acceptedID = isSteering ? sent.userMessageID : sent.id
                guard let acceptedID, !acceptedID.isEmpty else {
                    throw AIBackendError(message: "所属设备未返回消息接收编号")
                }
                result["user_message_id"] = acceptedID
                if isSteering { result["turnId"] = turnID }
                let latest = try? await backend.remoteSnapshot(session: envelope.sessionID, limit: 1)
                let receiptUser = try? await backend.userMessage(session: envelope.sessionID, id: acceptedID)
                if let userMessage = receiptUser {
                    if !synced.isEmpty, let latest {
                        var parts = remoteHistoryMessage(userMessage).parts
                        for item in synced {
                            if let original = userMessage.parts.first(where: {
                                $0.path == item.path && $0.kind == item.kind
                            }), let index = parts.firstIndex(where: { $0.id == original.id }) {
                                parts[index].attachmentID = item.id
                                parts[index].name = item.name
                            } else {
                                parts.append(AIPart(id: "session_attachment_" + item.id,
                                    kind: item.kind, text: item.name, tool: nil,
                                    input: nil, output: nil, error: nil, state: nil,
                                    path: nil, mimeType: item.mimeType,
                                    attachmentID: item.id, name: item.name))
                            }
                        }
                        let decorated = AIMessage(id: userMessage.id, role: userMessage.role,
                            created: userMessage.created, completed: userMessage.completed,
                            active: userMessage.active, parts: parts)
                        var payload = try remoteHistoryPayload(latest)
                        payload["messages"] = try JSONSerialization.jsonObject(
                            with: JSONEncoder().encode([decorated]))
                        payload["session_id"] = envelope.sessionID
                        let encoded = try JSONSerialization.data(withJSONObject: boundedQuestionHistoryPayload(payload))
                        do {
                            try await remoteTransport.publish(sessionID: envelope.sessionID,
                                data: encoded)
                        } catch {
                            result["warning"] = "附件已送达，历史记录正在重试同步"
                            Task {
                                for _ in 0..<5 {
                                    try? await Task.sleep(for: .seconds(2))
                                    if (try? await remoteTransport.publish(
                                        sessionID: envelope.sessionID, data: encoded)) != nil {
                                        return
                                    }
                                }
                            }
                        }
                    }
                }
            case "get_session", "set_model":
                var values = ["session": envelope.sessionID]
                if envelope.payload.action == "set_model" {
                    guard let model = envelope.payload.model, !model.isEmpty else {
                        throw AIBackendError(message: "缺少主模型编号", code: "invalid_input")
                    }
                    values["model"] = model
                    if let effort = envelope.payload.reasoningEffort { values["reasoningEffort"] = effort }
                }
                let choice = try await backend.request(envelope.payload.action ?? "", values: values)
                result = ["ok": true, "session": choice.session ?? envelope.sessionID,
                          "model": choice.model ?? "", "reasoningEffort": choice.reasoningEffort ?? "",
                          "defaultReasoningEffort": choice.defaultReasoningEffort ?? "high",
                          "wire": choice.wire ?? ""]
                localSessionChanged(envelope.sessionID)
            case "stop":
                let closeCamera = AIMobileLocalMediaRequests.shared.cameraSessionID == envelope.sessionID
                do {
                    // 原任务编号交给共享桥接校验；网络迟到的 Stop 不取消后续任务。
                    var values = ["session": envelope.sessionID]
                    if let turn = envelope.payload.turnId, !turn.isEmpty { values["turnId"] = turn }
                    _ = try await backend.request("stop", values: values)
                } catch {
                    if closeCamera {
                        AIMobileLocalMediaRequests.shared.closeCamera(in: envelope.sessionID)
                    }
                    throw error
                }
                if closeCamera {
                    AIMobileLocalMediaRequests.shared.closeCamera(in: envelope.sessionID)
                }
            case "answer":
                guard let questionID = envelope.payload.requestID, !questionID.isEmpty else {
                    throw AIBackendError(message: "回答缺少原问题编号")
                }
                guard envelope.payload.attachments?.isEmpty != false else {
                    throw AIBackendError(message: "问题回答仅发送文字，附件已保留")
                }
                questionAnswerDispatched = true
                let receipt = try await backend.request("answer", values: ["session": envelope.sessionID,
                    "id": questionID, "text": envelope.payload.text ?? "",
                    "requestId": envelope.payload.steeringRequestId ?? ""])
                guard receipt.questionID == questionID else {
                    throw AIBackendError(message: "所属设备未确认原问题的回答", definitive: false)
                }
                result["question_id"] = questionID
            case "permission", "rename":
                var values = ["session": envelope.sessionID]
                if let id = envelope.payload.requestID { values["id"] = id }
                if let decision = envelope.payload.decision { values["decision"] = decision }
                if let text = envelope.payload.text { values["text"] = text }
                if let title = envelope.payload.title { values["title"] = title }
                _ = try await backend.request(envelope.payload.action ?? "", values: values)
            default:
                throw AIBackendError(message: "不支持的远端操作")
            }
        } catch {
            let unknown = (steeringDispatched || questionAnswerDispatched) &&
                (error as? AIBackendError)?.definitive != true
            if let key = steeringImportKey,
               remoteSteeringImports[key]?.identity.acceptedOrder == nil, !unknown {
                remoteSteeringImports.removeValue(forKey: key)
                remoteSteeringImportRevision &+= 1
            }
            result = ["ok": false, "error": error.localizedDescription]
            if let code = (error as? AIBackendError)?.code { result["errorCode"] = code }
            if unknown { result["delivery_unknown"] = true }
        }
        try? await remoteTransport.sendPayload(
            to: envelope, kind: "event",
            value: ["type": "command_result", "command_id": envelope.clientMessageID,
                    "result": result])
    }

    private func applyRemoteLifecycleCommand(
        _ envelope: AIRemoteSessionTransport.Envelope) async {
        var result: [String: Any]
        do {
            switch envelope.payload.action {
            case "session_create":
                guard envelope.payload.workspacePath == nil else {
                    throw AIBackendError(message: "iPhone 只能使用本机默认工作目录")
                }
                let created = try await backend.request("create")
                guard let sessionID = created.id, !sessionID.isEmpty else {
                    throw AIBackendError(message: "新对话没有返回编号")
                }
                result = ["ok": true, "session_id": sessionID]
            case "session_delete":
                guard let target = envelope.payload.targetSessionID,
                      sessions.contains(where: { $0.id == target }) else {
                    throw AIBackendError(message: "对话已不存在")
                }
                let archiveWorkspace = URL(fileURLWithPath: workspacePath, isDirectory: true)
                _ = try await backend.request("delete", values: ["session": target])
                // Native deletion invalidates the session's core receipts too.
                forgetRemoteSteeringImports(session: target)
                await releaseScreenFrameHistory(sessionID: target, workspace: archiveWorkspace)
                deletedDraftSessions.insert(target)
                discardImportedFiles(pendingAttachments.removeValue(forKey: target) ?? [])
                drafts.removeValue(forKey: target)
                if selectedRemote == nil, selected == target {
                    selected = ""
                    messages = []
                    permissions = []
                    questions = []
                    questionMessages = []
                    updatedRootMessages = []
                    quotedMessage = nil
                    historyLoaded = true
                    scrollRequest += 1
                }
                try? await remoteTransport.deleteRoom(sessionID: target)
                if (try? await cleanupLocalSessionAttachments(target)) != nil {
                    initiallyPublishedSessions.remove(target)
                }
                result = ["ok": true, "session_id": target]
            case "session_set_workspace":
                throw AIBackendError(message: "iPhone 的对话只能使用本机应用工作目录")
            default:
                throw AIBackendError(message: "不支持的对话操作")
            }
        } catch {
            result = ["ok": false,
                      "error": String(error.localizedDescription.prefix(120))]
        }
        if let encoded = try? JSONSerialization.data(withJSONObject: [
            "type": "command_result", "command_id": envelope.clientMessageID,
            "result": result
        ]) {
            _ = try? await remoteTransport.sendEncoded(
                toSlot: envelope.fromSlot, toLogin: envelope.fromLoginSessionID,
                sessionID: Self.lifecycleSessionID, kind: "event",
                encodedPayload: encoded)
        }
        if result["ok"] as? Bool == true {
            if let summary = try? await backend.snapshot(session: "", limit: 0, force: true),
               let localSessions = summary.sessions {
                sessions = localSessions
            }
            let name = String(String(aiSyncDeviceDisplayName().prefix(40))
                .unicodeScalars.filter { !CharacterSet.controlCharacters.contains($0) })
            do {
                try await remoteTransport.presence(
                    deviceID: syncDeviceID, name: name.isEmpty ? "iPhone" : name,
                    sessions: sessions)
                lastRemotePresence = Date()
            } catch {
                lastRemotePresence = .distantPast
            }
        }
    }
    private func preparePrimaryModelForDisplay() async throws {
        #if DEBUG && targetEnvironment(simulator)
        if ProcessInfo.processInfo.arguments.contains("--ai-remote-session-model-ui-test"),
           let handler = uiTestSupport.sessionModelPreparationHandler { try await handler(); return }
        #endif
        try await backend.prepareLocalPrimaryModel()
    }

    private func displaySessionSnapshot(_ session: String, force: Bool) async throws -> AIResponse {
        #if DEBUG && targetEnvironment(simulator)
        if ProcessInfo.processInfo.arguments.contains("--ai-remote-session-model-ui-test"),
           let data = try uiTestSupport.sessionModelSnapshotHandler?(session) {
            return try JSONDecoder().decode(AIResponse.self, from: data)
        }
        #endif
        return try await backend.snapshot(session: session, limit: Self.recentMessageCount, force: force)
    }

    private func createModelChoiceSession(_ values: [String: String]) async throws -> AIResponse {
        #if DEBUG && targetEnvironment(simulator)
        if ProcessInfo.processInfo.arguments.contains("--ai-remote-session-model-ui-test") {
            uiTestSupport.sessionModelCreateObserver?(values)
        }
        #endif
        return try await backend.request("create", values: values)
    }

    private func refresh(force: Bool) async {
        if selectedRemote != nil { return }
        let target = selected
        let selection = selectionGeneration
        let generation = submissionGeneration
        let account = screenFrameCurrentAccountID
        func isCurrentPage() -> Bool {
            self === Self.shared && !outgoingPermanentlyInvalidated && !Task.isCancelled &&
                selectedRemote == nil && selected == target && selectionGeneration == selection &&
                submissionGeneration == generation && screenFrameCurrentAccountID == account
        }
        #if DEBUG && targetEnvironment(simulator)
        let primaryModelUITest = AIPrimaryModelUITestSupport.isRequested || AIPrimaryModelUITestSupport.isSessionConfigurationRequested
        #else
        let primaryModelUITest = false
        #endif
        if !settings.isLocalModel && !primaryModelUITest {
            try? await AICloudCredentialSync.refreshSeedanceIfNeeded()
            try? await AICloudCredentialSync.refreshBailianIfNeeded()
        }
        guard isCurrentPage() else { return }
        do {
            // 轮询只同步最近几条；旧消息由用户上滑时单独分页读取，避免又把首屏扩成 30 条。
            var result = try await displaySessionSnapshot(target, force: force)
            guard isCurrentPage() else { return }
            if let effort = result.defaultReasoningEffort { sessionDefaultReasoningEffort = effort }
            if result.selectedWire == "local" {
                projectSessionConfiguration(model: result.selectedModel ?? "", effort: result.selectedReasoningEffort,
                                            wire: result.selectedWire)
                configured = false
                try await preparePrimaryModelForDisplay()
                guard isCurrentPage() else { return }
                result = try await displaySessionSnapshot(target, force: true)
                guard isCurrentPage() else { return }
            }
            if let model = result.selectedModel ?? result.model { actualModelID = model }
            if let wire = result.selectedWire ?? result.wire { actualWire = wire }
            if let model = result.selectedModel {
                projectSessionConfiguration(model: model, effort: result.selectedReasoningEffort,
                                            wire: result.selectedWire)
            }
            if let selected = result.localGpuSelected { actualGPUSelected = selected }
            if let bytes = result.localGpuModelBytes { actualGPUModelBytes = bytes }
            if let backend = result.localGpuBackend { actualGPUBackend = backend }
            if let error = result.error { actualBackendHasError = !error.isEmpty }
            else if result.changed == true { actualBackendHasError = false }
            guard result.changed == true else { return }
            if let value = result.sessions, value != sessions { sessions = value }
            if let value = result.messages {
                let knownIDs = Set(messages.map(\.id))
                let newFailure = value.contains { message in
                    !knownIDs.contains(message.id) && message.parts.contains { part in
                        part.kind == "text" &&
                        ((part.text?.hasPrefix("Video task failed. ") == true) ||
                         (part.text?.hasPrefix("Image task failed. ") == true) ||
                         (part.text?.hasPrefix("Video task status unavailable. ") == true) ||
                         (part.text?.hasPrefix("Image task status unavailable. ") == true) ||
                         (part.text?.hasPrefix("Video output unavailable. ") == true) ||
                         (part.text?.hasPrefix("Image output unavailable. ") == true))
                    }
                }
                mergeMessages(value)
                if newFailure {
                    showTransientError("生成任务出现异常，详情见对话。", duration: .seconds(8))
                }
            }
            if let value = result.permissions, value != permissions { permissions = value }
            if let value = result.questions, value != questions { questions = value }
            if let updates = result.messageUpdates, !updates.isEmpty {
                let known = Set(messages.map(\.id))
                // 未加载过的旧 ID 仅进显示旁路，不能让下一次历史查询跳过未读取的中间页。
                mergeMessages(updates.filter { known.contains($0.id) })
                var latest = updatedRootMessages
                for message in updates {
                    latest.removeAll { $0.id == message.id }
                    latest.append(message.withoutReasoningText())
                }
                latest = Array(latest.suffix(20))
                if latest != updatedRootMessages { updatedRootMessages = latest }
            }
            if let value = result.questionMessages, value != questionMessages { questionMessages = value }
            reconcileQuestionAnswerEchoes()
            configured = result.configured ?? false
            if let detail = result.error, !detail.isEmpty {
                if detail != lastBackendError {
                    lastBackendError = detail
                    let display = detail.hasPrefix("model stream inactive")
                        ? "模型暂时没有响应，本次等待已结束。稍后可继续对话。"
                        : detail
                    showTransientError(display, duration: .seconds(8))
                }
            } else {
                lastBackendError = ""
            }
        } catch is CancellationError { }
        catch {
            guard isCurrentPage() else { return }
            showTransientError(error.localizedDescription, duration: .seconds(8))
        }
    }

    private func showNewestMessage(for session: String) async throws -> String? {
        let latest = try await backend.messagesPage(session: session, beforeId: "", limit: 1)
        guard selected == session else { return nil }
        messages = Array(latest.reversed()).map { $0.withoutReasoningText() }
        historyLoaded = true
        logAIHistoryEvent(
            """
            first-page session=\(session) count=\(latest.count) newest=\(latest.first?.id ?? "none") \
            role=\(latest.first?.role ?? "none") parts=\(latest.first?.parts.count ?? 0)
            """
        )
        return latest.first?.id
    }

    private func completeInitialPage(for session: String, newestID: String) async {
        do {
            let earlier = try await backend.messagesPage(session: session,
                                                         beforeId: newestID,
                                                         limit: Self.recentMessageCount - 1)
            guard selected == session else { return }
            mergeMessages(earlier)
            hasOlderMessages = earlier.count == Self.recentMessageCount - 1
            logAIHistoryEvent(
                """
                older-page session=\(session) added=\(earlier.count) first=\(messages.first?.id ?? "none") \
                last=\(messages.last?.id ?? "none")
                """
            )
            await refresh(force: true)
        } catch {
            logAIHistoryEvent("older-page error=\(error.localizedDescription)")
            self.error = error.localizedDescription
        }
    }

    private func mergeMessages(_ page: [AIMessage]) {
        // 快照只更新最近的有界消息页；用户已经展开的旧页必须保留。
        // 新旧页有相同稳定 ID 时替换旧行，避免重复显示。
        guard !page.isEmpty else { return }
        var merged = messages
        let previousNewestID = messages.last?.id
        var newlyAppendedCount = 0
        var changed = false
        for rawMessage in page {
            let message = rawMessage.withoutReasoningText()
            var lower = 0
            var upper = merged.count
            while lower < upper {
                let middle = (lower + upper) / 2
                if merged[middle].id < message.id { lower = middle + 1 }
                else { upper = middle }
            }
            if lower < merged.count, merged[lower].id == message.id {
                if merged[lower] != message {
                    merged[lower] = message
                    changed = true
                }
            } else {
                merged.insert(message, at: lower)
                changed = true
                if let previousNewestID, message.id > previousNewestID {
                    newlyAppendedCount += 1
                }
            }
        }
        if changed {
            // 已展开旧页时保持其起点；新消息进入末尾不能把正在阅读的旧行挤出可见窗。
            if visibleMessageLimit > Self.recentMessageCount {
                visibleMessageLimit += newlyAppendedCount
            }
            messages = merged
        }
    }

    func loadOlderMessages() async {
        guard !loadingOlderMessages, hasOlderVisibleMessages,
              let oldest = messages.first?.id, !selected.isEmpty else { return }
        if messages.count > visibleMessageLimit {
            visibleMessageLimit = min(messages.count,
                                      visibleMessageLimit + Self.olderPageCount)
            return
        }
        loadingOlderMessages = true
        defer { loadingOlderMessages = false }
        let session = selected
        if let remote = selectedRemote {
            do {
                let data = try await remoteTransport.history(ownerSlot: remote.deviceSlot,
                    sessionID: remote.sessionID, before: oldest, limit: Self.olderPageCount)
                guard selectedRemote?.id == remote.id else { return }
                applyRemoteHistory(data, source: remote, older: true)
            } catch {
                showTransientError(error.localizedDescription, duration: .seconds(8))
            }
            return
        }
        do {
            let page = try await backend.messagesPage(session: session,
                                                      beforeId: oldest,
                                                      limit: Self.olderPageCount)
            guard selected == session else { return }
            mergeMessages(page)
            visibleMessageLimit += page.count
            hasOlderMessages = page.count == Self.olderPageCount
        } catch { self.error = error.localizedDescription }
    }

    func messagesForMediaPlaylist(in session: String) async -> [AIMessage] {
        guard !session.isEmpty, selected == session else { return [] }
        // A remote ID must never query an unrelated local Session with the same
        // ID. Its preview only includes already received/cached remote images.
        if selectedRemote != nil { return messages }
        var collected: [AIMessage] = []
        var before = ""
        do {
            while !Task.isCancelled {
                // 只读本地会话存储，不把旧消息装进界面；预览仍可遍历全部历史媒体。
                let page = try await backend.messagesPage(session: session,
                                                          beforeId: before, limit: 100)
                guard selected == session else { return [] }
                collected.append(contentsOf: page)
                guard page.count == 100, let oldest = page.last?.id, oldest != before else { break }
                before = oldest
            }
        } catch {
            return messages
        }
        return collected.sorted { $0.id < $1.id }
    }

    /// 打开指定会话：先读取目标最近一页，再在无动画事务中替换可见消息。
    /// 读取期间保留原会话；并发切换通过代次丢弃过时结果，失败时不清空旧画面。
    func selectConversation(_ id: String) async {
        pendingRemoteSelection = nil
        if id.isEmpty {
            selectionGeneration &+= 1
            selectedRemote = nil
            pendingRemoteSelection = nil
            selected = ""
            projectSessionConfiguration(model: "deepseek-flash", effort: nil, wire: "responses")
            messages = []
            permissions = []
            questions = []
            questionMessages = []
            updatedRootMessages = []
            quotedMessage = nil
            historyLoaded = true
            hasOlderMessages = false
            scrollRequest += 1
            return
        }
        #if DEBUG && targetEnvironment(simulator)
        if selectUITestConversationIfNeeded(id) { return }
        #endif
        if id == selected && selectedRemote == nil {
            scrollRequest += 1
            return
        }
        selectionGeneration &+= 1
        let generation = selectionGeneration
        do {
            // 先在后台读取新会话最近一页；读取完成前保留当前消息，避免切换时闪白。
            // 一次性替换消息列表，避免“先一条、再整页”造成两次滚动和空白帧。
            let page = try await backend.messagesPage(session: id, beforeId: "",
                                                     limit: Self.recentMessageCount)
            guard generation == selectionGeneration else { return }
            var transaction = Transaction(animation: nil)
            transaction.disablesAnimations = true
            withTransaction(transaction) {
                selectedRemote = nil
                pendingRemoteSelection = nil
                selected = id
                messages = Array(page.reversed()).map { $0.withoutReasoningText() }
                visibleMessageLimit = Self.recentMessageCount
                permissions = []
                questions = []
                questionMessages = []
                updatedRootMessages = []
                error = ""
                quotedMessage = nil
                historyLoaded = true
                hasOlderMessages = page.count == Self.recentMessageCount
                pendingInitialPage = nil
                scrollRequest += 1
            }
            await refresh(force: true)
        } catch {
            guard generation == selectionGeneration else { return }
            // 查询失败时继续显示原会话，用户可重试；不能把可见历史清空。
            showTransientError(error.localizedDescription, duration: .seconds(8))
        }
    }
    func create() async {
        guard !isSubmitting, !isCreatingConversation else { return }
        isCreatingConversation = true
        defer { isCreatingConversation = false }
        do {
            let result = try await backend.request("create")
            await selectConversation(result.id ?? "")
        } catch { self.error = error.localizedDescription }
    }

    private func migrateEmptyDraft(to session: String) {
        firstMessageSessionID = session
        createdEmptyDraftSessions[emptyDraftToken] = session
        emptyDraftToken = UUID()
        let files = pendingAttachments.removeValue(forKey: "") ?? []
        pendingAttachments[session, default: []].append(contentsOf: files)
        if let textDraft = drafts.removeValue(forKey: "") {
            drafts[session] = textDraft
        }
    }

    private func remoteDeviceIsOnline(_ device: AIRemoteDevice) -> Bool {
        remoteDevices.contains {
            $0.id == device.id && $0.loginSessionID == device.loginSessionID && $0.online
        }
    }

    private func finishLifecycle(_ id: String, sessionID: String?, error: Error?,
                                 definitive: Bool) {
        guard let pending = pendingLifecycle.removeValue(forKey: id) else { return }
        if definitive {
            lifecycleRetryIDs = lifecycleRetryIDs.filter { $0.value != id }
        }
        if let error {
            pending.continuation.resume(throwing: error)
        } else if let sessionID {
            pending.continuation.resume(returning: sessionID)
        } else {
            pending.continuation.resume(throwing:
                AIBackendError(message: "所属设备没有返回对话编号"))
        }
    }

    private func sendLifecycle(to slot: String, login: String,
                               payload: [String: Any], fingerprint: String) async throws -> String {
        let encodedPayload = try JSONSerialization.data(withJSONObject: payload)
        let id = lifecycleRetryIDs[fingerprint] ??
            UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased()
        lifecycleRetryIDs[fingerprint] = id
        return try await withCheckedThrowingContinuation { continuation in
            pendingLifecycle[id] = PendingLifecycle(
                deviceSlot: slot, loginSessionID: login, continuation: continuation)
            Task { [weak self] in
                guard let self else { return }
                do {
                    _ = try await self.remoteTransport.sendEncoded(
                        toSlot: slot, toLogin: login,
                        sessionID: Self.lifecycleSessionID, kind: "command",
                        encodedPayload: encodedPayload, messageID: id)
                } catch {
                    self.finishLifecycle(id, sessionID: nil, error: error, definitive: false)
                }
                try? await Task.sleep(for: .seconds(30))
                self.finishLifecycle(id, sessionID: nil,
                    error: AIBackendError(message: "所属设备是否已执行操作尚未确定，请查看对话目录"),
                    definitive: false)
            }
        }
    }

    func createRemoteSession(on device: AIRemoteDevice,
                             workspacePath: String? = nil,
                             selectOnSuccess: Bool = true) async -> String? {
        guard !remoteSessionMutationInProgress, remoteDeviceIsOnline(device) else {
            showTransientError("所属设备离线，无法新建对话")
            return nil
        }
        remoteSessionMutationInProgress = true
        defer { remoteSessionMutationInProgress = false }
        var payload: [String: Any] = ["action": "session_create"]
        if let workspacePath, !workspacePath.isEmpty {
            payload["workspace_path"] = workspacePath
        }
        let fingerprint = "create:\(device.deviceSlot):\(device.loginSessionID):\(workspacePath ?? "")"
        do {
            let createdID = try await sendLifecycle(to: device.deviceSlot,
                login: device.loginSessionID, payload: payload, fingerprint: fingerprint)
            guard selectOnSuccess else { return createdID }
            for _ in 0..<20 {
                if let session = remoteSessions.first(where: {
                    $0.deviceSlot == device.deviceSlot &&
                    $0.loginSessionID == device.loginSessionID &&
                    $0.sessionID == createdID
                }) {
                    await selectRemoteConversation(session)
                    return createdID
                }
                try? await Task.sleep(for: .milliseconds(200))
            }
            showTransientError("对话已建立，正在同步设备目录")
            return createdID
        } catch {
            showTransientError(error.localizedDescription, duration: .seconds(8))
            return nil
        }
    }

    func deleteRemoteSession(_ session: AIRemoteSession) async -> Bool {
        guard !remoteSessionMutationInProgress, remoteIsOnline(session), !session.busy else {
            showTransientError("所属设备离线或对话仍在执行，无法删除")
            return false
        }
        remoteSessionMutationInProgress = true
        defer { remoteSessionMutationInProgress = false }
        let payload: [String: Any] = ["action": "session_delete",
                                       "target_session_id": session.sessionID]
        let fingerprint = "delete:\(session.deviceSlot):\(session.loginSessionID):\(session.sessionID)"
        do {
            _ = try await sendLifecycle(to: session.deviceSlot,
                login: session.loginSessionID, payload: payload, fingerprint: fingerprint)
            if selectedRemote?.id == session.id {
                selectedRemote = nil
                pendingRemoteSelection = nil
                selected = ""
                messages = []
                permissions = []
                questions = []
                questionMessages = []
                updatedRootMessages = []
                historyLoaded = true
                scrollRequest += 1
            }
            remoteSessions.removeAll { $0.id == session.id }
            return true
        } catch {
            showTransientError(error.localizedDescription, duration: .seconds(8))
            return false
        }
    }

    func setRemoteSessionWorkspace(_ session: AIRemoteSession,
                                   workspacePath: String) async -> Bool {
        guard !workspacePath.isEmpty, !remoteSessionMutationInProgress,
              remoteIsOnline(session), !session.busy else {
            showTransientError("所属设备离线、对话正在执行，或工作目录为空")
            return false
        }
        remoteSessionMutationInProgress = true
        defer { remoteSessionMutationInProgress = false }
        let payload: [String: Any] = ["action": "session_set_workspace",
                                       "target_session_id": session.sessionID,
                                       "workspace_path": workspacePath]
        let fingerprint = "workspace:\(session.deviceSlot):\(session.loginSessionID):\(session.sessionID):\(workspacePath)"
        do {
            _ = try await sendLifecycle(to: session.deviceSlot,
                login: session.loginSessionID, payload: payload, fingerprint: fingerprint)
            return true
        } catch {
            showTransientError(error.localizedDescription, duration: .seconds(8))
            return false
        }
    }

    private func enqueueRemoteCommand(to remote: AIRemoteSession,
                                      payload: [String: Any],
                                      optimisticID: String? = nil) async throws -> String {
        let encoded = try JSONSerialization.data(withJSONObject: payload, options: .sortedKeys)
        let digest = SHA256.hash(data: encoded).map { String(format: "%02x", $0) }.joined()
        let fingerprint = [remote.deviceSlot, remote.loginSessionID,
                           remote.sessionID, digest].joined(separator: ":")
        // A read must get a fresh receipt ID after timeout/reconnect. Mutating
        // commands retain their explicit retry ID for owner-side deduplication.
        let id = (payload["action"] as? String == "get_session" ? nil : remoteRetryIDByFingerprint[fingerprint]) ??
            UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased()
        remoteRetryIDByFingerprint[fingerprint] = id
        pendingRemoteCommands[id] = Date()
        if let action = payload["action"] as? String, ["set_model", "get_session"].contains(action) {
            latestModelCommandByRemote[remote.id] = id
            pendingModelOwners[id] = PendingModelChoice(owner: remote, action: action, model: payload["model"] as? String,
                reasoningEffort: payload["reasoningEffort"] as? String,
                generation: submissionGeneration, account: screenFrameCurrentAccountID)
        }
        if let attachments = payload["attachments"] as? [[String: String]],
           !attachments.isEmpty { attachmentRemoteCommands.insert(id) }
        remoteFingerprintByCommandID[id] = fingerprint
        remoteDeliveryPendingIDs.insert(id)
        defer { remoteDeliveryPendingIDs.remove(id) }
        if payload["action"] as? String == "prompt",
           let text = payload["text"] as? String {
            remoteDraftByCommandID[id] = text
            if let optimisticID { remoteOptimisticByCommandID[id] = optimisticID }
        }
        do {
            #if DEBUG && targetEnvironment(simulator)
            if ProcessInfo.processInfo.arguments.contains("--ai-remote-session-model-ui-test"),
               let handler = uiTestSupport.sessionModelCommandHandler { try await handler(remote, id, payload) }
            else { try await remoteTransport.sendCommand(to: remote, encodedPayload: encoded, messageID: id) }
            #else
            try await remoteTransport.sendCommand(to: remote, encodedPayload: encoded, messageID: id)
            #endif
        } catch {
            if remoteResultByCommandID[id] == nil { throw error }
        }
        guard pendingRemoteCommands[id] != nil || remoteResultByCommandID[id] != nil else {
            throw AIBackendError(message: "操作状态未知，请先查看所属设备的会话记录")
        }
        return id
    }
    private func finishRemoteSteering(_ id: String, result: Result<String, Error>) {
        remoteSteeringConfirmations.removeValue(forKey: id)?.continuation.resume(with: result)
    }

    /// Transport acknowledgement is not owner acceptance. Each delivery attempt
    /// uses a new envelope ID, while requestId stays stable for core deduplication.
    private func sendRemoteSteering(to remote: AIRemoteSession,
                                    payload: [String: Any], questionID: String? = nil,
                                    onForwarded: (@MainActor () async -> Void)? = nil) async throws -> String {
        let encoded = try JSONSerialization.data(withJSONObject: payload, options: .sortedKeys)
        let id = UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased()
        return try await withCheckedThrowingContinuation { continuation in
            remoteSteeringConfirmations[id] = RemoteSteeringConfirmation(
                remote: remote, questionID: questionID, continuation: continuation)
            Task { @MainActor [weak self] in
                guard let self else { return }
                do {
                    try await remoteTransport.sendCommand(to: remote, encodedPayload: encoded,
                                                          messageID: id)
                    await onForwarded?()
                } catch {
                    finishRemoteSteering(id, result: .failure(AIBackendError(
                        message: error.localizedDescription, definitive: false)))
                }
            }
            Task { @MainActor [weak self] in
                try? await Task.sleep(for: .seconds(30))
                self?.finishRemoteSteering(id, result: .failure(AIBackendError(
                    message: "所属设备尚未确认接收，内容已保留；重试会复用原请求编号",
                    definitive: false)))
            }
        }
    }

    private func outgoingConversationID(session: String, remote: AIRemoteSession?) -> String {
        if let remote {
            let data = (try? JSONEncoder().encode([remote.deviceSlot, remote.loginSessionID, remote.sessionID])) ?? Data()
            return "remote:" + SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
        }
        return "local:" + session
    }

    var visibleOutgoingRows: [AIOutgoingMessageRow] {
        let conversation = outgoingConversationID(session: selected, remote: selectedRemote)
        return outgoingEntries.compactMap { entry in
            guard entry.owner.accountID == screenFrameCurrentAccountID,
                  entry.owner.conversationID == conversation,
                  !(entry.state == .accepted && messages.contains { $0.id == entry.acceptedMessageID }),
                  let payload = try? JSONDecoder().decode(AIOutgoingMessagePayload.self, from: entry.payloadData)
            else { return nil }
            if payload.questionID != nil, entry.state == .accepted, acceptedQuestionAnswers[entry.id] != nil { return nil }
            let sameEpoch = entry.binding == nil || entry.binding?.mode == .enqueue || entry.binding?.hostEpoch == outgoingCurrentEpoch
            let retry = sameEpoch && (entry.state == .failed ||
                (entry.state == .unknown && entry.binding?.mode != .send))
            return AIOutgoingMessageRow(entry: entry, payload: payload, allowsRetry: retry)
        }
    }

    /// 在点击的 MainActor 调用栈中捕获目标和顺序；收据只确认本地已持久入队，不冒充模型接收。
    func enqueueMessage(_ text: String, expectedSession: String? = nil) -> Task<Bool, Never> {
        let session = selected
        let draftFiles = pendingAttachments[selectedRemote.map(remoteDraftKey) ?? session] ?? []
        guard !outgoingPermanentlyInvalidated, ready, expectedSession == nil || expectedSession == session,
              !screenFrameCurrentAccountID.isEmpty, !workspacePath.isEmpty,
              !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || !draftFiles.isEmpty else {
            return Task { false }
        }
        let intakeID = UUID()
        let conversation = outgoingConversationID(session: session, remote: selectedRemote)
        let queuedQuestionIDs = Set(outgoingEntries.filter {
            $0.owner.accountID == screenFrameCurrentAccountID && $0.owner.conversationID == conversation
        }.compactMap { (try? JSONDecoder().decode(AIOutgoingMessagePayload.self, from: $0.payloadData))?.questionID })
        let question = questions.sorted {
            ($0.asked ?? 0) == ($1.asked ?? 0) ? $0.id < $1.id : ($0.asked ?? 0) < ($1.asked ?? 0)
        }.first { !queuedQuestionIDs.contains($0.id) && questionAnswerReservations[questionReservationKey(session: session,
            remote: selectedRemote, questionID: $0.id)] == nil }
        if question != nil, text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            showTransientError("请输入回答文字，附件仍保留在输入栏")
            return Task { false }
        }
        if let question {
            let key = questionReservationKey(session: session, remote: selectedRemote, questionID: question.id)
            questionAnswerReservations[key] = intakeID
            questionAnswerReservationOrder.removeAll { $0 == key }
            questionAnswerReservationOrder.append(key)
            while questionAnswerReservationOrder.count > 128 {
                questionAnswerReservations.removeValue(forKey: questionAnswerReservationOrder.removeFirst())
            }
        }
        // question 工具只接受文字；引用和媒体留在原草稿，不能随回答被悄悄消费或删除。
        let files = question == nil ? draftFiles : []
        var answer: AsyncStream<Bool>.Continuation!
        let stream = AsyncStream<Bool>(bufferingPolicy: .bufferingNewest(1)) { answer = $0 }
        for file in files { outgoingIntakeProtection[file.relativePath, default: 0] += 1 }
        outgoingIntakes.append(OutgoingIntake(id: intakeID, account: screenFrameCurrentAccountID,
            workspace: workspacePath, session: session, remote: selectedRemote, emptyToken: emptyDraftToken,
            text: text, quote: question == nil ? quotedMessage?.id ?? "" : "", files: files,
            questionID: question?.id, question: question, result: answer))
        if outgoingIntakeTask == nil {
            outgoingIntakeTask = Task { @MainActor [weak self] in await self?.receiveOutgoingIntakes() }
        }
        return Task { for await value in stream { return value }; return false }
    }

    private func questionReservationKey(session: String, remote: AIRemoteSession?, questionID: String) -> String {
        screenFrameCurrentAccountID + ":" + outgoingConversationID(session: session, remote: remote) + ":" + questionID
    }

    /// 普通调用复用同一个队列；UI 直接用同步 enqueueMessage 保持点击顺序。
    func sendMessage(_ text: String, expectedSession: String? = nil) async -> Bool {
        await enqueueMessage(text, expectedSession: expectedSession).value
    }

    private func receiveOutgoingIntakes() async {
        defer { outgoingIntakeTask = nil }
        while !outgoingIntakes.isEmpty {
            let intake = outgoingIntakes.removeFirst()
            var accepted = false
            do {
                guard !outgoingPermanentlyInvalidated, self === Self.shared, intake.account == screenFrameCurrentAccountID,
                      intake.workspace == workspacePath else { throw AIBackendError(message: "原账号已切换，内容未发送") }
                var session = intake.session
                if intake.remote == nil && session.isEmpty {
                    if let created = createdEmptyDraftSessions[intake.emptyToken] { session = created }
                    else {
                        let created = try await backend.request("create")
                        guard let id = created.id, !id.isEmpty else { throw AIBackendError(message: "会话尚未建立") }
                        session = id
                        createdEmptyDraftSessions[intake.emptyToken] = id
                        if selected.isEmpty && emptyDraftToken == intake.emptyToken {
                            migrateEmptyDraft(to: id)
                            selected = id
                            historyLoaded = true
                        }
                    }
                }
                guard !deletedDraftSessions.contains(session), self === Self.shared,
                      intake.account == screenFrameCurrentAccountID else { throw AIBackendError(message: "原会话已关闭") }
                let pump = try await ensureOutgoingPump()
                let owner = AIOutgoingMessageQueueOwner(accountID: intake.account, workspaceID: "assistant",
                    conversationID: outgoingConversationID(session: session, remote: intake.remote))
                let remote = intake.remote.map { AIOutgoingMessagePayload.RemoteOwner(
                    slot: $0.deviceSlot, login: $0.loginSessionID, session: $0.sessionID) }
                var payload = AIOutgoingMessagePayload(text: intake.text, quotedMessageID: intake.quote,
                    files: intake.files.map(AIOutgoingMessagePayload.File.init), localSession: session,
                    emptyDraftToken: nil, remote: remote)
                payload.questionID = intake.questionID
                payload.question = intake.question
                _ = try await pump.queue.enqueue(owner: owner, payloadData: JSONEncoder().encode(payload), id: intake.id)
                // 拥有原文件/截图引用的队列先落盘，才从该次草稿分离附件；不删除实际像素。
                await pump.publish()
                let draftKey = intake.remote.map(remoteDraftKey) ?? session
                let identities = Set(intake.files.map(\.attachmentIdentity))
                pendingAttachments[draftKey]?.removeAll { identities.contains($0.attachmentIdentity) }
                if pendingAttachments[draftKey]?.isEmpty == true { pendingAttachments.removeValue(forKey: draftKey) }
                if quotedMessage?.id == intake.quote && selected == session { quotedMessage = nil }
                accepted = true
                scrollRequest += 1
                pump.wake(owner)
            } catch {
                if let questionID = intake.questionID {
                    let key = questionReservationKey(session: intake.session, remote: intake.remote, questionID: questionID)
                    if questionAnswerReservations[key] == intake.id { questionAnswerReservations.removeValue(forKey: key) }
                }
                if self === Self.shared, intake.account == screenFrameCurrentAccountID {
                    showTransientError("内容尚未保存到发送队列，请保留草稿后重试", duration: .seconds(8))
                }
            }
            for file in intake.files {
                let remaining = (outgoingIntakeProtection[file.relativePath] ?? 1) - 1
                if remaining > 0 { outgoingIntakeProtection[file.relativePath] = remaining }
                else { outgoingIntakeProtection.removeValue(forKey: file.relativePath) }
            }
            intake.result.yield(accepted)
            intake.result.finish()
        }
    }

    private func ensureOutgoingPump() async throws -> AIOutgoingMessagePump {
        let account = screenFrameCurrentAccountID
        let workspace = workspacePath
        guard !outgoingPermanentlyInvalidated, self === Self.shared, !account.isEmpty, !workspace.isEmpty else {
            throw AIBackendError(message: "原账号或工作区不可用")
        }
        let epoch = await backend.runtimeEpoch()
        let identity = account + "\u{1f}" + workspace
        guard !outgoingPermanentlyInvalidated, self === Self.shared, account == screenFrameCurrentAccountID, workspace == workspacePath else {
            throw AIBackendError(message: "原账号已切换")
        }
        if let pump = outgoingPump, outgoingQueueIdentity == identity {
            outgoingCurrentEpoch = epoch
            try await pump.queue.updateHostEpoch(epoch)
            return pump
        }
        let url = URL(fileURLWithPath: workspace, isDirectory: true)
            .deletingLastPathComponent().appendingPathComponent("outgoing-messages.json")
        let queue = try AIOutgoingMessageQueue(fileURL: url, accountID: account, workspaceID: "assistant", hostEpoch: epoch)
        let pump = AIOutgoingMessagePump(queue: queue,
            prepare: { [weak self] entry in
                guard let self else { throw CancellationError() }
                return try await self.prepareOutgoing(entry)
            }, bind: { [weak self] entry in
                guard let self else { throw CancellationError() }
                return try await self.bindOutgoing(entry)
            }, deliver: { [weak self] entry in
                guard let self else { throw CancellationError() }
                return try await self.deliverOutgoing(entry)
            }, failure: { [weak self] entry, error in
                guard let self else { return .unknown }
                return await self.outgoingFailure(entry, error: error)
            }, changed: { [weak self] entries in
                guard let self, !self.outgoingPermanentlyInvalidated, self === Self.shared, self.screenFrameCurrentAccountID == account,
                      self.workspacePath == workspace else { return }
                self.publishOutgoingEntries(entries)
            }, accepted: { [weak self] entry in
                guard let self, !self.outgoingPermanentlyInvalidated, self === Self.shared, self.screenFrameCurrentAccountID == account,
                      self.workspacePath == workspace else { return }
                if let payload = try? JSONDecoder().decode(AIOutgoingMessagePayload.self, from: entry.payloadData),
                   let questionID = payload.questionID {
                    if let question = payload.question {
                        self.acceptedQuestionAnswers[entry.id] = AcceptedQuestionAnswer(owner: entry.owner,
                            question: question, text: payload.text)
                        self.acceptedQuestionAnswerOrder.removeAll { $0 == entry.id }
                        self.acceptedQuestionAnswerOrder.append(entry.id)
                        while self.acceptedQuestionAnswerOrder.count > 64 {
                            self.acceptedQuestionAnswers.removeValue(forKey: self.acceptedQuestionAnswerOrder.removeFirst())
                        }
                    }
                    if entry.owner.conversationID == self.outgoingConversationID(session: self.selected, remote: self.selectedRemote) {
                        self.questions.removeAll { $0.id == questionID }
                    }
                }
                if self === Self.shared, self.screenFrameCurrentAccountID == account {
                    #if DEBUG && targetEnvironment(simulator)
                    if self.uiTestSupport.steeringHandler == nil { await self.refresh(force: true) }
                    #else
                    await self.refresh(force: true)
                    #endif
                    self.scrollRequest += 1
                }
                if self.messages.contains(where: { $0.id == entry.acceptedMessageID }) {
                    _ = try? await queue.removeAccepted(id: entry.id, expectedRevision: entry.revision)
                }
                if let current = self.outgoingPump { await current.publish() }
            }, forwardsWithoutWaiting: { $0.conversationID.hasPrefix("remote:") })
        outgoingQueueIdentity = identity
        outgoingCurrentEpoch = epoch
        outgoingQueue = queue
        outgoingPump = pump
        _ = try await queue.load()
        await pump.publish()
        return pump
    }

    private func checkedOutgoingPayload(_ entry: AIOutgoingMessageQueueEntry) throws -> AIOutgoingMessagePayload {
        guard !outgoingPermanentlyInvalidated, self === Self.shared, entry.owner.accountID == screenFrameCurrentAccountID,
              entry.owner.workspaceID == "assistant", !workspacePath.isEmpty else { throw CancellationError() }
        let payload = try JSONDecoder().decode(AIOutgoingMessagePayload.self, from: entry.payloadData)
        if let remote = payload.remote {
            guard let target = remoteSessions.first(where: { $0.deviceSlot == remote.slot && $0.loginSessionID == remote.login &&
                $0.sessionID == remote.session }),
                  entry.owner.conversationID == outgoingConversationID(session: target.sessionID, remote: target)
            else { throw AIBackendError(message: "原设备或登录状态已变化") }
        } else if deletedDraftSessions.contains(payload.localSession) {
            throw AIBackendError(message: "原会话已关闭", code: "not_found")
        } else if entry.owner.conversationID != outgoingConversationID(session: payload.localSession, remote: nil) {
            throw AIBackendError(message: "发送队列与原会话不匹配")
        }
        return payload
    }

    private func publishOutgoingEntries(_ entries: [AIOutgoingMessageQueueEntry]) {
        let old = outgoingEntries.flatMap {
            (try? JSONDecoder().decode(AIOutgoingMessagePayload.self, from: $0.payloadData)
                .importedFiles(workspace: workspacePath)) ?? []
        }
        let files = entries.flatMap {
            (try? JSONDecoder().decode(AIOutgoingMessagePayload.self, from: $0.payloadData)
                .importedFiles(workspace: workspacePath)) ?? []
        }
        outgoingEntries = entries
        let conversation = outgoingConversationID(session: selected, remote: selectedRemote)
        let canonical = Set(messages.map(\.id))
        let confirmed = entries.filter { $0.owner.conversationID == conversation && $0.state == .accepted &&
            $0.acceptedMessageID.map(canonical.contains) == true }
        if !confirmed.isEmpty, let pump = outgoingPump {
            Task { @MainActor in
                for entry in confirmed { _ = try? await pump.queue.removeAccepted(id: entry.id, expectedRevision: entry.revision) }
                await pump.publish()
            }
        }
        outgoingProtectedPaths = Set(files.map(\.relativePath))
        outgoingProtectedReferenceIDs = Set(files.compactMap { $0.archiveReference?.referenceID })
        for (id, pins) in outgoingPreparedPins where
            Set(pins.map(\.referenceID)).isSubset(of: outgoingProtectedReferenceIDs) {
            outgoingPreparedPins.removeValue(forKey: id)
        }
        let stillDraft = Set(pendingAttachments.values.flatMap { $0 }.compactMap { $0.archiveReference?.referenceID })
        let released = old.compactMap(\.archiveReference).filter {
            !outgoingProtectedReferenceIDs.contains($0.referenceID) && !stillDraft.contains($0.referenceID) &&
                screenFrameHistoryPins[$0.referenceID] == nil
        }
        if !released.isEmpty {
            let id = UUID()
            outgoingCleanupTasks[id] = Task { @MainActor [weak self] in
                try? await AIMobileScreenFrameArchive.shared.release(references: released)
                self?.outgoingCleanupTasks.removeValue(forKey: id)
            }
        }
    }

    private func persistPreparedOutgoing(_ entry: AIOutgoingMessageQueueEntry,
                                          payload: AIOutgoingMessagePayload) async throws {
        guard let pump = outgoingPump,
              let latest = try await pump.queue.allEntries().first(where: { $0.id == entry.id }) else {
            throw AIBackendError(message: "原发送队列不可用")
        }
        _ = try await pump.queue.transition(id: entry.id, expectedRevision: latest.revision,
                                           to: .updatePayload(JSONEncoder().encode(payload)))
        await pump.publish()
    }

    private func prepareOutgoing(_ entry: AIOutgoingMessageQueueEntry) async throws -> Data {
        var payload = try checkedOutgoingPayload(entry)
        if payload.questionID != nil {
            guard payload.files.isEmpty, payload.quotedMessageID.isEmpty else {
                throw AIBackendError(message: "问题回答仅发送文字，附件和引用请保留在输入栏")
            }
            return try JSONEncoder().encode(payload)
        }
        var files = try payload.importedFiles(workspace: workspacePath)
        if !payload.archivePinsPrepared {
            var owned: [AIMobileScreenArchiveReference] = []
            do {
                for (index, file) in files.enumerated() {
                    guard let reference = file.archiveReference else { continue }
                    guard let retained = try await AIMobileScreenFrameArchive.shared.retainSelected(
                        frameIDs: [reference.frame.id], context: reference.context,
                        usageSessionID: "outbox:" + entry.requestID).first else {
                        throw AIBackendError(message: "截图引用不可读取")
                    }
                    owned.append(retained)
                    files[index] = AIImportedFile(relativePath: file.relativePath, mimeType: file.mimeType,
                                                  isImage: file.isImage, archiveReference: retained)
                }
            } catch {
                try? await AIMobileScreenFrameArchive.shared.release(references: owned)
                throw error
            }
            payload.files = files.map(AIOutgoingMessagePayload.File.init)
            payload.archivePinsPrepared = true
            outgoingPreparedPins[entry.id] = owned
            try await persistPreparedOutgoing(entry, payload: payload)
        }
        if let remote = payload.remote {
            guard let owner = remoteSessions.first(where: { $0.deviceSlot == remote.slot &&
                $0.loginSessionID == remote.login && $0.sessionID == remote.session }), remoteIsOnline(owner) else {
                throw AIBackendError(message: "原设备离线，内容已在本地排队", code: "owner_offline")
            }
            let room = MaiCloudDriveClient.SessionRoom(ownerSlot: remote.slot,
                ownerLoginSessionID: remote.login, sessionID: remote.session)
            while payload.remoteAttachmentIDs.count < files.count {
                let file = files[payload.remoteAttachmentIDs.count]
                guard let url = AIAssistantPathPolicy.resolve(file.relativePath, workspacePath: workspacePath),
                      FileManager.default.fileExists(atPath: url.path) else {
                    throw AIBackendError(message: "排队附件已丢失")
                }
                let uploaded = try await MaiCloudDriveClient.shared.uploadSessionAttachment(
                    fileURL: url, displayName: url.lastPathComponent, room: room)
                payload.remoteAttachmentIDs.append(uploaded.attachmentId)
                try await persistPreparedOutgoing(entry, payload: payload)
            }
        }
        return try JSONEncoder().encode(payload)
    }

    private func bindOutgoing(_ entry: AIOutgoingMessageQueueEntry) async throws -> AIOutgoingMessageQueueBinding {
        let payload = try checkedOutgoingPayload(entry)
        let epoch = await backend.runtimeEpoch()
        if let questionID = payload.questionID {
            return AIOutgoingMessageQueueBinding(mode: .answer, questionID: questionID, hostEpoch: epoch)
        }
        var running: Bool
        var turn: String
        if let remote = payload.remote {
            guard let owner = remoteSessions.first(where: { $0.deviceSlot == remote.slot &&
                $0.loginSessionID == remote.login && $0.sessionID == remote.session }), remoteIsOnline(owner) else {
                throw AIBackendError(message: "原设备离线", code: "owner_offline")
            }
            _ = owner
            return AIOutgoingMessageQueueBinding(mode: .enqueue, hostEpoch: epoch)
        } else {
            #if DEBUG && targetEnvironment(simulator)
            if uiTestSupport.steeringHandler != nil {
                guard let state = sessions.first(where: { $0.id == payload.localSession }) else {
                    throw AIBackendError(message: "原会话已关闭", code: "not_found")
                }
                running = state.busy; turn = state.turnId ?? ""
            } else {
                let snapshot = try await backend.snapshot(session: payload.localSession, limit: 0, force: true)
                guard let state = snapshot.sessions?.first(where: { $0.id == payload.localSession }) else {
                    throw AIBackendError(message: "原会话已关闭", code: "not_found")
                }
                running = state.busy; turn = state.turnId ?? ""
            }
            #else
            let snapshot = try await backend.snapshot(session: payload.localSession, limit: 0, force: true)
            guard let state = snapshot.sessions?.first(where: { $0.id == payload.localSession }) else {
                throw AIBackendError(message: "原会话已关闭", code: "not_found")
            }
            running = state.busy; turn = state.turnId ?? ""
            #endif
        }
        if running && turn.isEmpty { throw AIBackendError(message: "所属设备需更新后才能接收补充") }
        _ = try checkedOutgoingPayload(entry)
        return AIOutgoingMessageQueueBinding(mode: running ? .steer : .send, turnID: running ? turn : "", hostEpoch: epoch)
    }

    private func deliverOutgoing(_ entry: AIOutgoingMessageQueueEntry) async throws -> String {
        let payload = try checkedOutgoingPayload(entry)
        guard let binding = entry.binding else { throw AIBackendError(message: "排队请求尚未绑定任务") }
        let files = try payload.importedFiles(workspace: workspacePath)
        outgoingDeliveryCount += 1; isSubmitting = true
        var counting = true
        defer { if counting { counting = false; outgoingDeliveryCount -= 1; isSubmitting = outgoingDeliveryCount > 0 } }
        if binding.mode == .answer {
            guard let questionID = binding.questionID, questionID == payload.questionID,
                  files.isEmpty, payload.quotedMessageID.isEmpty else {
                throw AIBackendError(message: "回答与原问题不匹配，内容已保留")
            }
            if let remote = payload.remote {
                guard let owner = remoteSessions.first(where: { $0.deviceSlot == remote.slot &&
                    $0.loginSessionID == remote.login && $0.sessionID == remote.session }), remoteIsOnline(owner) else {
                    throw AIBackendError(message: "原设备未确认回答", definitive: false)
                }
                return try await sendRemoteSteering(to: owner, payload: ["action": "answer", "id": questionID,
                    "text": payload.text, "requestId": entry.requestID], questionID: questionID)
            }
            #if DEBUG && targetEnvironment(simulator)
            // Isolated UI tests retain the production queue/binding validation;
            // only the final native receipt is supplied without contacting a model.
            if let handler = uiTestSupport.questionHandler {
                let receivedID = try await handler(AIQuestionAnswerUITestRequest(
                    sessionID: payload.localSession, questionID: questionID,
                    requestID: entry.requestID, text: payload.text))
                guard receivedID == questionID else {
                    throw AIBackendError(message: "回答送达状态待确认，内容已保留", definitive: false)
                }
                return questionID
            }
            #endif
            let receipt = try await backend.request("answer", values: ["session": payload.localSession,
                "id": questionID, "text": payload.text, "requestId": entry.requestID])
            guard receipt.questionID == questionID else {
                throw AIBackendError(message: "回答送达状态待确认，内容已保留", definitive: false)
            }
            return questionID
        }
        if let remote = payload.remote {
            guard let owner = remoteSessions.first(where: { $0.deviceSlot == remote.slot &&
                $0.loginSessionID == remote.login && $0.sessionID == remote.session }), remoteIsOnline(owner) else {
                throw AIBackendError(message: "原设备未确认接收", definitive: false)
            }
            var command: [String: Any] = ["action": binding.mode == .steer ? "steer" : "prompt",
                "text": payload.text, "requestId": entry.requestID, "quoteMessageId": payload.quotedMessageID]
            if binding.mode == .steer { command["turnId"] = binding.turnID }
            if !payload.remoteAttachmentIDs.isEmpty {
                command["attachments"] = payload.remoteAttachmentIDs.map { ["attachment_id": $0] }
            }
            return try await sendRemoteSteering(to: owner, payload: command, onForwarded: { [weak self] in
                guard let self else { return }
                if counting { counting = false; self.outgoingDeliveryCount -= 1; self.isSubmitting = self.outgoingDeliveryCount > 0 }
                await self.outgoingPump?.forwarded(entry)
            })
        }
        try await prepareScreenFrameHistory(files.compactMap(\.archiveReference).filter {
            screenFrameHistoryPins[$0.referenceID] != payload.localSession
        }, sessionID: payload.localSession)
        _ = try checkedOutgoingPayload(entry)
        let response: AIResponse
        if binding.mode == .steer {
            let request = AISteeringRequest(session: payload.localSession, turnId: binding.turnID,
                requestId: entry.requestID, text: payload.text, images: files.filter(\.isImage),
                videos: files.filter { $0.mimeType.hasPrefix("video/") }, quoteMessageId: payload.quotedMessageID)
            #if DEBUG && targetEnvironment(simulator)
            if let handler = uiTestSupport.steeringHandler { return try await handler(request) }
            #endif
            response = try await backend.steer(request)
        } else {
            response = try await backend.send(session: payload.localSession, text: payload.text,
                images: files.filter(\.isImage), videos: files.filter { $0.mimeType.hasPrefix("video/") },
                quoteMessageId: payload.quotedMessageID)
        }
        guard let id = response.id, !id.isEmpty else { throw AIBackendError(message: "接收状态待确认", definitive: false) }
        return id
    }

    private func outgoingFailure(_ entry: AIOutgoingMessageQueueEntry, error: Error) async -> AIOutgoingMessagePump.FailureDecision {
        if let unused = outgoingPreparedPins.removeValue(forKey: entry.id) {
            try? await AIMobileScreenFrameArchive.shared.release(references: unused)
        }
        if entry.state == .queued {
            return self !== Self.shared || (error as? AIBackendError)?.code == "owner_offline" ? .wait : .failed
        }
        if entry.everUnknown || (error as? AIBackendError)?.definitive != true { return .unknown }
        // 失效回答只能留在原 outbox 恢复，不能重新绑定为下一轮 send/steer。
        if entry.binding?.mode == .answer { return .failed }
        if let payload = try? checkedOutgoingPayload(entry), payload.remote == nil,
           let files = try? payload.importedFiles(workspace: workspacePath) {
            let pins = files.compactMap(\.archiveReference).filter {
                screenFrameHistoryPins[$0.referenceID] == payload.localSession
            }
            do {
                try await AIMobileScreenFrameArchive.shared.assignSession(references: pins,
                    usageSessionID: "outbox:" + entry.requestID)
                for pin in pins { screenFrameHistoryPins.removeValue(forKey: pin.referenceID) }
            } catch { /* 无法确认回退则保守持有，不删除可能已在历史中的像素。 */ }
        }
        if (error as? AIBackendError)?.code == "busy" { return .requeueRejected }
        if let code = (error as? AIBackendError)?.code, code == "not_found",
           let fresh = try? await bindOutgoing(entry), fresh != entry.binding {
            return .requeueRejected
        }
        return .failed
    }

    func retryOutgoingMessage(_ id: UUID) { Task { await outgoingPump?.retry(id) } }

    /// 普通会话清理入口复用队列 actor；先让该 owner 的迟到准备结果失效，再移除本地记录。
    func discardOutgoingMessages(for session: String) async throws {
        guard let pump = outgoingPump else { return }
        let owner = AIOutgoingMessageQueueOwner(accountID: screenFrameCurrentAccountID,
            workspaceID: "assistant", conversationID: outgoingConversationID(session: session, remote: nil))
        pump.pause(owner)
        _ = try await pump.queue.removeAll(for: owner)
        await pump.publish()
    }

    private func restoreOutgoingMessages() async {
        guard let pump = try? await ensureOutgoingPump() else { return }
        if let entries = try? await pump.queue.allEntries() {
            for accepted in entries where accepted.state == .accepted {
                _ = try? await pump.queue.removeAccepted(id: accepted.id, expectedRevision: accepted.revision)
            }
            await pump.publish()
            for owner in Set(entries.filter { $0.state == .queued }.map(\.owner)) { pump.wake(owner) }
        }
    }

    func action(_ op: String, values: [String: String] = [:]) async {
        if let remote = selectedRemote {
            guard remoteIsOnline(remote) else {
                showTransientError("所属设备离线，无法执行操作")
                return
            }
            guard ["stop", "permission", "answer", "rename"].contains(op) else {
                showTransientError("此操作暂不支持跨设备执行")
                return
            }
            var payload: [String: Any] = values
            payload["action"] = op
            do {
                let id = try await enqueueRemoteCommand(to: remote, payload: payload)
                _ = remoteResultByCommandID.removeValue(forKey: id)
            } catch {
                showTransientError("送达状态未知；重试会复用操作编号。\n" +
                    error.localizedDescription, duration: .seconds(8))
            }
            return
        }
        if (op == "delete" || op == "clear") && (isSubmitting || isCreatingConversation) {
            return
        }
        let target = selected
        let closeCamera = op == "stop" && AIMobileLocalMediaRequests.shared.cameraSessionID == target
        let archiveWorkspace = URL(fileURLWithPath: workspacePath, isDirectory: true)
        let clearingOwner = AIOutgoingMessageQueueOwner(accountID: screenFrameCurrentAccountID,
            workspaceID: "assistant", conversationID: outgoingConversationID(session: target, remote: nil))
        if op == "delete" || op == "clear" { outgoingPump?.pause(clearingOwner) }
        var nativeCleared = false
        do {
            var values = values; values["session"] = target
            _ = try await backend.request(op, values: values)
            nativeCleared = op == "delete" || op == "clear"
            if op == "delete" { forgetRemoteSteeringImports(session: target) }
            if op == "delete" || op == "clear" {
                try await discardOutgoingMessages(for: target)
                await releaseScreenFrameHistory(sessionID: target, workspace: archiveWorkspace)
            }
            if closeCamera { AIMobileLocalMediaRequests.shared.closeCamera(in: target) }
            if op == "delete" {
                deletedDraftSessions.insert(target)
                discardImportedFiles(pendingAttachments.removeValue(forKey: target) ?? [])
                drafts.removeValue(forKey: target)
                createdEmptyDraftSessions = createdEmptyDraftSessions.filter { $0.value != target }
                try? await cleanupLocalSessionAttachments(target)
            }
            if op == "delete", selected == target {
                selected = ""; messages = []; historyLoaded = true
                quotedMessage = nil
                firstMessageSessionID = ""
                emptyDraftToken = UUID()
            }
            if op == "clear", selected == target {
                messages = []; historyLoaded = true
                quotedMessage = nil
            }
            await refresh(force: true)
        } catch {
            if (op == "delete" || op == "clear") && !nativeCleared { outgoingPump?.wake(clearingOwner) }
            if closeCamera { AIMobileLocalMediaRequests.shared.closeCamera(in: target) }
            self.error = error.localizedDescription
        }
    }

    private func cleanupLocalSessionAttachments(_ session: String) async throws {
        guard let login = await remoteTransport.currentLoginSessionID() else {
            throw AIBackendError(message: "会话同步登录状态不可用")
        }
        let room = MaiCloudDriveClient.SessionRoom(ownerSlot: syncDeviceSlot,
            ownerLoginSessionID: login, sessionID: session)
        try await MaiCloudDriveClient.shared.cleanupSessionRoom(room)
    }
    /// 显示层投影 owner 的会话配置；不写设置文件，不配置/重建 Native Agent。
    private func projectSessionConfiguration(model: String, effort: String?, wire: String?) {
        if model.hasPrefix("local:") {
            settings.modelProvider = "llama"
            settings.model = model
        } else if AIModelSettings.cloudModels.contains(model) {
            settings.selectModel(model)
        } else {
            #if DEBUG && targetEnvironment(simulator)
            if model == "test-model" { settings.modelProvider = "remote"; settings.model = model }
            #endif
        }
        settings.reasoningEffort = effort?.isEmpty == false ? effort : sessionDefaultReasoningEffort
        actualModelID = model
        if let wire { actualWire = wire }
    }

    var canChangeSessionModel: Bool {
        !busy && !isSubmitting && !isCreatingConversation && !changingModelSource &&
            (selectedRemote.map { $0.online && $0.model != nil } ?? ready)
    }

    var canChangeSessionReasoningEffort: Bool {
        canChangeSessionModel && (selectedRemote.map {
            AIModelSettings.cloudModels.contains($0.model ?? "")
        } ?? true)
    }

    func selectPrimaryModel(_ model: String, reasoningEffort: String? = nil) async {
        guard canChangeSessionModel else { return }
        guard AIModelSettings.cloudModels.contains(model) || model == "local:" + AILocalModelStore.bundledPrimaryID else { return }
        if let reasoningEffort, !["", "low", "high", "max"].contains(reasoningEffort) { return }
        let target = selected
        let remote = selectedRemote
        let generation = submissionGeneration
        let draftToken = emptyDraftToken
        let selection = selectionGeneration
        let requestedEffort = reasoningEffort ?? settings.effectiveReasoningEffort
        let account = screenFrameCurrentAccountID
        changingModelSource = true
        defer { changingModelSource = false }
        do {
            if let remote {
                guard AIModelSettings.cloudModels.contains(model) else { return }
                var payload: [String: Any] = ["action": "set_model", "model": model]
                if let reasoningEffort { payload["reasoningEffort"] = reasoningEffort }
                _ = try await enqueueRemoteCommand(to: remote, payload: payload)
                return // 只有 owner ACK/history 可以提交显示选择，传输成功不冒充保存成功。
            }
            if model.hasPrefix("local:") { try await preparePrimaryModelForDisplay() }
            guard generation == submissionGeneration else { throw CancellationError() }
            if target.isEmpty {
                // Leaving a blank page while its local model prepares cancels that
                // choice. Check before creating anything, including leave-and-return.
                guard account == screenFrameCurrentAccountID, selectedRemote == nil, selected.isEmpty,
                      emptyDraftToken == draftToken, selectionGeneration == selection else { return }
                let created = try await createModelChoiceSession(["model": model,
                    "reasoningEffort": requestedEffort])
                guard let id = created.id, !id.isEmpty else { throw AIBackendError(message: "没有返回会话编号") }
                guard generation == submissionGeneration, account == screenFrameCurrentAccountID,
                      selectedRemote == nil, selected.isEmpty, emptyDraftToken == draftToken,
                      selectionGeneration == selection else { return }
                migrateEmptyDraft(to: id)
                await selectConversation(id)
            } else {
                var values = ["session": target, "model": model]
                if let reasoningEffort { values["reasoningEffort"] = reasoningEffort }
                _ = try await backend.request("set_model", values: values)
                guard generation == submissionGeneration else { return }
                await refresh(force: true)
            }
        } catch is CancellationError { }
        catch {
            guard generation == submissionGeneration, account == screenFrameCurrentAccountID else { return }
            if let remote {
                guard selectedRemote?.id == remote.id, selectedRemote?.loginSessionID == remote.loginSessionID else { return }
            } else {
                guard selectedRemote == nil, selected == target,
                      !target.isEmpty || (emptyDraftToken == draftToken && selectionGeneration == selection) else { return }
            }
            showTransientError(error.localizedDescription, duration: .seconds(8))
        }
    }

    func selectReasoningEffort(_ effort: String) async {
        guard canChangeSessionReasoningEffort else { return }
        await selectPrimaryModel(settings.model, reasoningEffort: effort)
    }

    func selectPrimaryModelSource(local: Bool) async {
        await selectPrimaryModel(local ? "local:" + AILocalModelStore.bundledPrimaryID : "deepseek-flash")
    }

    func save(_ config: AIModelSettings) async -> Bool {
        guard self === Self.shared, !outgoingPermanentlyInvalidated else { return false }
        do {
            var normalized = config
            normalized.selectModel(config.model)
            let savedDefaults = try await backend.save(normalized)
            settings.policy = savedDefaults.policy
            guard self === Self.shared, !outgoingPermanentlyInvalidated else { return false }
            ready = true
            error = ""
            // configure 换了 native epoch；更新重试门禁，unknown 保留原绑定且不自动重放。
            await restoreOutgoingMessages()
            await refresh(force: true)
            return true
        } catch { self.error = error.localizedDescription; return false }
    }
    func attachments(for session: String) -> [AIImportedFile] {
        pendingAttachments[attachmentKey(for: session)] ?? []
    }

    var remoteImageOwner: AIRemoteImageOwner? {
        guard let remote = selectedRemote, !screenFrameCurrentAccountID.isEmpty else { return nil }
        return AIRemoteImageOwner(accountID: screenFrameCurrentAccountID, slot: remote.deviceSlot,
            loginSessionID: remote.loginSessionID, sessionID: remote.sessionID,
            selectionGeneration: selectionGeneration)
    }

    func cachedRemoteImagePath(owner: AIRemoteImageOwner, messageID: String, part: AIPart) -> String? {
        guard remoteImageOwner == owner,
              let request = AIRemoteImageRequest(owner: owner, messageID: messageID, part: part),
              let file = remoteImageFiles[request.signature], FileManager.default.fileExists(atPath: file.path)
        else { return nil }
        return file.path
    }

    private func isCurrentRemoteImage(owner: AIRemoteImageOwner, messageID: String, part: AIPart) -> Bool {
        self === Self.shared && !outgoingPermanentlyInvalidated && remoteImageOwner == owner &&
            messages.contains { $0.id == messageID && $0.parts.contains {
                $0.id == part.id && $0.remoteImage == part.remoteImage
            } }
    }

    func loadRemoteImage(owner: AIRemoteImageOwner, messageID: String, part: AIPart) async throws -> URL {
        try Task.checkCancellation()
        guard isCurrentRemoteImage(owner: owner, messageID: messageID, part: part),
              let metadata = part.remoteImage, metadata.retention == "temporary" else {
            throw AIRemoteImageError.unavailable
        }
        if metadata.state == "pending" { throw AIRemoteImageError.pending }
        if metadata.state == "failed" {
            throw metadata.isExpired ? AIRemoteImageError.expired : AIRemoteImageError.uploadFailed
        }
        guard let request = AIRemoteImageRequest(owner: owner, messageID: messageID, part: part) else {
            throw AIRemoteImageError.unavailable
        }
        // The completed local copy outlives the temporary OSS object. Expiry is
        // checked only after this exact identity/integrity lookup, before HTTP.
        if let file = await remoteImageCache.cached(request) {
            try Task.checkCancellation()
            guard isCurrentRemoteImage(owner: owner, messageID: messageID, part: part) else {
                throw AIRemoteImageError.changed
            }
            remoteImageFiles[request.signature] = file
            return file
        }
        remoteImageFiles.removeValue(forKey: request.signature)
        try Task.checkCancellation()
        guard isCurrentRemoteImage(owner: owner, messageID: messageID, part: part) else {
            throw AIRemoteImageError.changed
        }
        guard !request.isExpired else { throw AIRemoteImageError.expired }
        let staged = try await remoteImageCache.staging(request)
        do {
            try Task.checkCancellation()
            guard isCurrentRemoteImage(owner: owner, messageID: messageID, part: part) else {
                throw AIRemoteImageError.changed
            }
            #if DEBUG && targetEnvironment(simulator)
            if ProcessInfo.processInfo.arguments.contains("--ai-remote-image-ui-test") {
                guard let handler = uiTestSupport.remoteImageDownloadHandler else {
                    throw AIBackendError(message: "Isolated image download fixture is missing")
                }
                uiTestSupport.remoteImageDownloadCount += 1
                uiTestSupport.remoteImageRequestedIDs.append(request.attachmentID)
                try await handler(request, staged)
            } else {
                _ = try await MaiCloudDriveClient.shared.downloadTemporarySessionImage(request, to: staged) { [weak self] in
                    self?.isCurrentRemoteImage(owner: owner, messageID: messageID, part: part) == true
                }
            }
            #else
            _ = try await MaiCloudDriveClient.shared.downloadTemporarySessionImage(request, to: staged) { [weak self] in
                self?.isCurrentRemoteImage(owner: owner, messageID: messageID, part: part) == true
            }
            #endif
            try Task.checkCancellation()
            guard isCurrentRemoteImage(owner: owner, messageID: messageID, part: part) else {
                throw AIRemoteImageError.changed
            }
            guard !request.isExpired else { throw AIRemoteImageError.expired }
            let file = try await remoteImageCache.commit(staged, request: request)
            try Task.checkCancellation()
            guard isCurrentRemoteImage(owner: owner, messageID: messageID, part: part) else {
                throw AIRemoteImageError.changed
            }
            remoteImageFiles[request.signature] = file
            return file
        } catch {
            await remoteImageCache.discard(staged)
            if let failure = error as? MaiCloudDriveError,
               failure.httpStatus == 410, failure.errorCode == "image_expired" {
                throw AIRemoteImageError.expired
            }
            throw error
        }
    }

    func downloadRemoteAttachment(_ part: AIPart)
        async throws -> MaiCloudDriveClient.DownloadedSessionAttachment {
        guard let remote = selectedRemote, let id = part.attachmentID else {
            throw AIBackendError(message: "会话附件已不可用")
        }
        let room = MaiCloudDriveClient.SessionRoom(ownerSlot: remote.deviceSlot,
            ownerLoginSessionID: remote.loginSessionID,
            sessionID: remote.sessionID)
        let downloaded = try await MaiCloudDriveClient.shared
            .downloadSessionAttachment(id: id, room: room)
        guard selectedRemote?.id == remote.id else {
            try? FileManager.default.removeItem(at: downloaded.fileURL)
            throw AIBackendError(message: "会话已切换")
        }
        return downloaded
    }

    func openLocalSessionAttachmentForAgent(id: String, sessionID: String)
        async throws -> AIImportedFile {
        guard sessions.contains(where: { $0.id == sessionID }),
              let login = await remoteTransport.currentLoginSessionID() else {
            throw AIBackendError(message: "本机会话或同步登录状态不可用")
        }
        let room = MaiCloudDriveClient.SessionRoom(ownerSlot: syncDeviceSlot,
            ownerLoginSessionID: login, sessionID: sessionID)
        let downloaded = try await MaiCloudDriveClient.shared
            .downloadSessionAttachment(id: id, room: room)
        defer { try? FileManager.default.removeItem(at: downloaded.fileURL) }
        return downloaded.mediaType == "image"
            ? try await backend.importDocument(downloaded.fileURL)
            : try await backend.importSessionFile(
                downloaded.fileURL, displayName: downloaded.name)
    }

    private func remoteDraftKey(_ remote: AIRemoteSession) -> String {
        "remote:\(remote.deviceSlot):\(remote.loginSessionID):\(remote.sessionID)"
    }

    private func attachmentKey(for session: String) -> String {
        if let remote = selectedRemote, remote.sessionID == session {
            return remoteDraftKey(remote)
        }
        return session
    }

    func importTarget() -> AIAttachmentImportTarget {
        AIAttachmentImportTarget(
            sessionID: selected,
            emptyDraftToken: selected.isEmpty ? emptyDraftToken : nil,
            remoteRoomKey: selectedRemote.map(remoteDraftKey)
        )
    }

    func isDeletedDraftSession(_ session: String) -> Bool {
        deletedDraftSessions.contains(session)
    }

    func attachmentSession(for target: AIAttachmentImportTarget) -> String? {
        if !target.sessionID.isEmpty {
            return deletedDraftSessions.contains(target.sessionID) ? nil : target.sessionID
        }
        guard let token = target.emptyDraftToken else { return nil }
        if let created = createdEmptyDraftSessions[token] { return created }
        return token == emptyDraftToken ? "" : nil
    }

    var screenFrameCurrentAccountID: String {
        #if DEBUG && targetEnvironment(simulator)
        if let accountID = uiTestSupport.screenFrameAccountID { return accountID }
        #endif
        guard let appState = AIMobileHostToolProvider.shared.appState else { return "" }
        return appState.maiChatAccountID.isEmpty ? appState.masterUserID : appState.maiChatAccountID
    }

    func makeScreenFrameImportSnapshot() throws -> AIScreenFrameImportSnapshot {
        let account = screenFrameCurrentAccountID
        guard !account.isEmpty, !workspacePath.isEmpty, self === Self.shared else {
            throw AIBackendError(message: "账号或创作中心尚未准备好")
        }
        let target = importTarget()
        let owner = target.remoteRoomKey ?? (!target.sessionID.isEmpty ? target.sessionID :
            "draft:" + (target.emptyDraftToken ?? UUID()).uuidString.lowercased())
        return AIScreenFrameImportSnapshot(accountID: account,
            workspace: URL(fileURLWithPath: workspacePath, isDirectory: true)
                .standardizedFileURL.resolvingSymlinksInPath(),
            ownerSessionID: owner, importTarget: target)
    }

    // A camera owner is separate from the screen owner, even when both were
    // started in the same Session. Length framing avoids ambiguous identities.
    func makeCameraFrameImportSnapshot() throws -> AIScreenFrameImportSnapshot {
        let screen = try makeScreenFrameImportSnapshot()
        let identity = "\(screen.accountID.utf8.count):\(screen.accountID)\(screen.ownerSessionID.utf8.count):\(screen.ownerSessionID)"
        let digest = SHA256.hash(data: Data(identity.utf8)).map { String(format: "%02x", $0) }.joined()
        return AIScreenFrameImportSnapshot(accountID: screen.accountID, workspace: screen.workspace,
            ownerSessionID: "camera:" + digest, importTarget: screen.importTarget, isCamera: true)
    }

    @discardableResult
    func registerCameraFrameArchive(context: AIMobileScreenArchiveContext,
                                    snapshot: AIScreenFrameImportSnapshot) throws -> AIScreenFrameImportBinding {
        guard snapshot.isCamera, snapshot.ownerSessionID.hasPrefix("camera:") else {
            throw AIBackendError(message: "相机图片的会话身份无效")
        }
        return try registerScreenFrameArchive(context: context, snapshot: snapshot)
    }

    @discardableResult
    func registerScreenFrameArchive(context: AIMobileScreenArchiveContext,
                                    snapshot supplied: AIScreenFrameImportSnapshot? = nil) throws
        -> AIScreenFrameImportBinding {
        if let existing = screenFrameImports.first(where: { $0.id == context.batchID }) {
            try validateScreenFrameImport(existing)
            return existing
        }
        let snapshot: AIScreenFrameImportSnapshot
        if let supplied { snapshot = supplied }
        else {
            let current = try makeScreenFrameImportSnapshot()
            if context.ownerSessionID == current.ownerSessionID { snapshot = current }
            else {
                guard sessions.contains(where: { $0.id == context.ownerSessionID }),
                      !context.ownerSessionID.isEmpty else {
                    throw AIBackendError(message: "请返回开始共享的原会话查看截图")
                }
                snapshot = AIScreenFrameImportSnapshot(accountID: current.accountID,
                    workspace: current.workspace, ownerSessionID: context.ownerSessionID,
                    importTarget: AIAttachmentImportTarget(sessionID: context.ownerSessionID,
                        emptyDraftToken: nil, remoteRoomKey: nil))
            }
        }
        let binding = AIScreenFrameImportBinding(context: context, snapshot: snapshot)
        try validateScreenFrameImport(binding)
        screenFrameImports.append(binding)
        screenFrameImportRevision &+= 1
        return binding
    }

    private func validateScreenFrameSnapshot(_ snapshot: AIScreenFrameImportSnapshot) throws {
        guard self === Self.shared, !snapshot.accountID.isEmpty,
              snapshot.accountID == screenFrameCurrentAccountID,
              snapshot.workspace.standardizedFileURL.resolvingSymlinksInPath().path ==
                URL(fileURLWithPath: workspacePath, isDirectory: true)
                    .standardizedFileURL.resolvingSymlinksInPath().path,
              attachmentSession(for: snapshot.importTarget) != nil else {
            throw AIBackendError(message: "账号、工作区或原会话已变化，截图已保留但未加入草稿")
        }
    }

    private func validateScreenFrameImport(_ binding: AIScreenFrameImportBinding) throws {
        try validateScreenFrameSnapshot(binding.snapshot)
        guard
              binding.context.workspace.standardizedFileURL.resolvingSymlinksInPath().path ==
                binding.snapshot.workspace.path,
              binding.context.ownerSessionID == binding.snapshot.ownerSessionID else {
            throw AIBackendError(message: "账号、工作区或原会话已变化，截图已保留但未加入草稿")
        }
    }

    private func recordUnavailableFrameBatches(_ listing: AIMobileScreenArchiveListing,
                                                snapshot: AIScreenFrameImportSnapshot) throws {
        try validateScreenFrameSnapshot(snapshot)
        let count = listing.unavailableBatches?.count ?? 0
        guard unavailableFrameBatches[snapshot, default: 0] != count else { return }
        if count == 0 { unavailableFrameBatches.removeValue(forKey: snapshot) }
        else { unavailableFrameBatches[snapshot] = count }
        screenFrameImportRevision &+= 1
    }

    func screenFrameArchiveUnavailableCountForCurrentDraft() -> Int {
        guard let snapshot = try? makeScreenFrameImportSnapshot() else { return 0 }
        return unavailableFrameBatches[snapshot, default: 0]
    }

    func cameraFrameArchiveUnavailableCountForCurrentDraft() -> Int {
        guard let snapshot = try? makeCameraFrameImportSnapshot() else { return 0 }
        return unavailableFrameBatches[snapshot, default: 0]
    }

    func screenFrameBindingsForCurrentDraft() -> [AIScreenFrameImportBinding] {
        let current = importTarget()
        return screenFrameImports.filter { binding in
            guard !binding.snapshot.isCamera,
                  (try? validateScreenFrameImport(binding)) != nil,
                  let session = attachmentSession(for: binding.snapshot.importTarget) else { return false }
            return binding.snapshot.importTarget.remoteRoomKey == current.remoteRoomKey &&
                session == attachmentSession(for: current)
        }
    }

    func cameraFrameBindingsForCurrentDraft() -> [AIScreenFrameImportBinding] {
        let current = importTarget()
        return screenFrameImports.filter { binding in
            guard binding.snapshot.isCamera,
                  (try? validateScreenFrameImport(binding)) != nil,
                  let session = attachmentSession(for: binding.snapshot.importTarget) else { return false }
            return binding.snapshot.importTarget.remoteRoomKey == current.remoteRoomKey &&
                session == attachmentSession(for: current)
        }
    }

    func restoreCameraFrameArchivesForCurrentDraft() async {
        do {
            let snapshot = try makeCameraFrameImportSnapshot()
            let listing = try await AIMobileScreenFrameArchive.shared.list(
                workspace: snapshot.workspace, sessionID: snapshot.ownerSessionID)
            guard !Task.isCancelled else { return }
            try recordUnavailableFrameBatches(listing, snapshot: snapshot)
            for batch in listing.batches.sorted(by: { $0.startedAtMS < $1.startedAtMS }) {
                let context = AIMobileScreenArchiveContext(workspace: snapshot.workspace,
                    ownerSessionID: snapshot.ownerSessionID, batchID: batch.id)
                let binding = AIScreenFrameImportBinding(context: context, snapshot: snapshot)
                try validateScreenFrameImport(binding)
                // A live or starting capture owns its batch; only recover orphaned recordings.
                if batch.status == "recording", AIMobileCameraCapture.shared.archiveContext != context,
                   AIMobileLocalMediaRequests.shared.cameraState != "opening" {
                    _ = try await AIMobileScreenFrameArchive.shared.finish(context: context, reason: "interrupted")
                    try validateScreenFrameImport(binding)
                }
                _ = try registerCameraFrameArchive(context: context, snapshot: snapshot)
            }
        } catch { /* No archive exists before the first camera capture. */ }
    }

    func screenFrameBinding(id: String) -> AIScreenFrameImportBinding? {
        guard let binding = screenFrameImports.first(where: { $0.id == id }),
              (try? validateScreenFrameImport(binding)) != nil else { return nil }
        return binding
    }

    func restoreScreenFrameArchivesForCurrentDraft() async {
        do {
            let snapshot = try makeScreenFrameImportSnapshot()
            let listing = try await AIMobileScreenFrameArchive.shared.list(
                workspace: snapshot.workspace, sessionID: snapshot.ownerSessionID)
            guard !Task.isCancelled else { return }
            try recordUnavailableFrameBatches(listing, snapshot: snapshot)
            for batch in listing.batches.sorted(by: { $0.startedAtMS < $1.startedAtMS }) {
                let context = AIMobileScreenArchiveContext(workspace: snapshot.workspace,
                    ownerSessionID: snapshot.ownerSessionID, batchID: batch.id)
                let binding = AIScreenFrameImportBinding(context: context, snapshot: snapshot)
                try validateScreenFrameImport(binding)
                let capture = AIMobileScreenCapture.shared
                let isLive = (capture.isSharing || capture.isStarting) &&
                    capture.archiveContext == context
                if batch.status == "recording", !isLive, !capture.isStarting {
                    _ = try await AIMobileScreenFrameArchive.shared.finish(
                        context: context, reason: "interrupted")
                    try validateScreenFrameImport(binding)
                }
                _ = try registerScreenFrameArchive(context: context, snapshot: snapshot)
            }
        } catch { /* A missing archive is normal before the first capture. */ }
    }

    func screenFrameAttachmentCapacity(_ binding: AIScreenFrameImportBinding) -> Int {
        guard (try? validateScreenFrameImport(binding)) != nil,
              let session = attachmentSession(for: binding.snapshot.importTarget) else { return 0 }
        let key = binding.snapshot.importTarget.remoteRoomKey ?? session
        return max(0, 10 - pendingAttachments[key, default: []].count)
    }

    @discardableResult
    func addScreenFrames(_ frameIDs: [String], binding: AIScreenFrameImportBinding) async throws -> Int {
        try validateScreenFrameImport(binding)
        let capacity = screenFrameAttachmentCapacity(binding)
        guard !frameIDs.isEmpty, frameIDs.count <= capacity else {
            throw AIBackendError(message: capacity == 0 ? "原会话草稿已有 10 项附件，请先移除部分附件" :
                "原会话草稿还可添加 \(capacity) 张图片")
        }
        let references = try await AIMobileScreenFrameArchive.shared.retainSelected(
            frameIDs: frameIDs, context: binding.context,
            usageSessionID: "draft:" + UUID().uuidString.lowercased())
        do {
            try Task.checkCancellation()
            try validateScreenFrameImport(binding)
            guard references.count <= screenFrameAttachmentCapacity(binding) else {
                throw AIBackendError(message: "原会话草稿的附件数量已变化，请减少选择后重试")
            }
        }
        catch {
            try? await AIMobileScreenFrameArchive.shared.release(references: references)
            throw error
        }
        let imported = references.map { AIImportedFile(relativePath: $0.relativePath,
            mimeType: "image/png", isImage: true, archiveReference: $0) }
        return addAttachments(imported, to: binding.snapshot.importTarget)
    }

    func discardScreenFrameArchive(_ binding: AIScreenFrameImportBinding) async throws {
        try validateScreenFrameImport(binding)
        try await AIMobileScreenFrameArchive.shared.discard(context: binding.context)
        try validateScreenFrameImport(binding)
        AIMobileScreenCapture.shared.forgetFinishedArchive(batchID: binding.id)
        screenFrameImports.removeAll { $0.id == binding.id }
        screenFrameImportRevision &+= 1
    }

    @discardableResult
    func addAttachments(_ files: [AIImportedFile], to target: AIAttachmentImportTarget) -> Int {
        guard !files.isEmpty else { return 0 }
        guard let session = attachmentSession(for: target) else {
            discardImportedFiles(files)
            showTransientError("原会话已关闭，请重新选择媒体")
            return 0
        }
        let key = target.remoteRoomKey ?? session
        let remaining = max(0, 10 - pendingAttachments[key, default: []].count)
        var seen = Set(pendingAttachments[key, default: []].map(\.relativePath))
        let existingTokens = Set(pendingAttachments[key, default: []]
            .compactMap { $0.archiveReference?.referenceID })
        var accepted: [AIImportedFile] = []
        var extra: [AIImportedFile] = []
        for file in files {
            let duplicateFrame = file.archiveReference != nil && seen.contains(file.relativePath)
            if accepted.count < remaining && !duplicateFrame {
                accepted.append(file)
                seen.insert(file.relativePath)
            } else { extra.append(file) }
        }
        pendingAttachments[key, default: []].append(contentsOf: accepted)
        if !extra.isEmpty {
            discardImportedFiles(extra.filter {
                guard let reference = $0.archiveReference else { return true }
                return !existingTokens.contains(reference.referenceID)
            })
            if accepted.count == remaining {
                showTransientError("每条消息最多添加 10 项附件，超出的文件未加入草稿")
            }
        }
        return accepted.count
    }

    func discardImportedFile(_ file: AIImportedFile) {
        discardImportedFiles([file])
    }

    private func discardImportedFiles(_ files: [AIImportedFile]) {
        let references = files.compactMap(\.archiveReference).filter {
            screenFrameHistoryPins[$0.referenceID] == nil &&
                !screenFrameSubmissionPins.contains($0.referenceID) &&
                !outgoingProtectedReferenceIDs.contains($0.referenceID) &&
                outgoingIntakeProtection[$0.relativePath] == nil
        }
        let archiveRoot = URL(fileURLWithPath: workspacePath, isDirectory: true)
            .appendingPathComponent(".maichat/screen-frame-archive", isDirectory: true)
            .standardizedFileURL.resolvingSymlinksInPath().path + "/"
        let urls = files.filter { $0.archiveReference == nil &&
            !outgoingProtectedPaths.contains($0.relativePath) && outgoingIntakeProtection[$0.relativePath] == nil }.compactMap {
            AIAssistantPathPolicy.resolve($0.relativePath, workspacePath: workspacePath)
        }.filter { !$0.path.hasPrefix(archiveRoot) }
        Task.detached(priority: .utility) {
            try? await AIMobileScreenFrameArchive.shared.release(references: references)
            for url in urls { try? FileManager.default.removeItem(at: url) }
        }
    }

    private func prepareScreenFrameHistory(_ references: [AIMobileScreenArchiveReference],
                                           sessionID: String) async throws {
        guard !references.isEmpty else { return }
        let expectedAccount = screenFrameCurrentAccountID
        let expectedWorkspace = workspacePath
        let pending = references.filter { screenFrameHistoryPins[$0.referenceID] == nil }
        screenFrameSubmissionPins.formUnion(pending.map(\.referenceID))
        do {
            try await AIMobileScreenFrameArchive.shared.assignSession(
                references: references, usageSessionID: sessionID)
            guard self === Self.shared, screenFrameCurrentAccountID == expectedAccount,
                  workspacePath == expectedWorkspace, !deletedDraftSessions.contains(sessionID) else {
                throw AIBackendError(message: "账号或工作区已变化，未发送截图")
            }
            for reference in references { screenFrameHistoryPins[reference.referenceID] = sessionID }
            screenFrameSubmissionPins.subtract(pending.map(\.referenceID))
        } catch {
            screenFrameSubmissionPins.subtract(pending.map(\.referenceID))
            let stillPending = Set(pendingAttachments.values.flatMap { $0 }
                .compactMap { $0.archiveReference?.referenceID })
            let abandoned = pending.filter { !outgoingProtectedReferenceIDs.contains($0.referenceID) &&
                (self !== Self.shared || !stillPending.contains($0.referenceID)) }
            try? await AIMobileScreenFrameArchive.shared.release(references: abandoned)
            throw error
        }
    }

    /// Only a confirmed rejection may move newly prepared pins back to the draft.
    /// An unknown native receipt could already be in history and must remain held.
    private func restoreScreenFrameDraftAfterRejectedSteering(
        _ references: [AIMobileScreenArchiveReference], sessionID: String
    ) async {
        let prepared = references.filter { screenFrameHistoryPins[$0.referenceID] == sessionID }
        guard !prepared.isEmpty else { return }
        do {
            try await AIMobileScreenFrameArchive.shared.assignSession(references: prepared,
                usageSessionID: "draft:" + UUID().uuidString.lowercased())
            for reference in prepared { screenFrameHistoryPins.removeValue(forKey: reference.referenceID) }
            let pending = Set(pendingAttachments.values.flatMap { $0 }
                .compactMap { $0.archiveReference?.referenceID })
            let abandoned = prepared.filter { self !== Self.shared || !pending.contains($0.referenceID) }
            try? await AIMobileScreenFrameArchive.shared.release(references: abandoned)
        } catch {
            // Keep the history pin if rollback cannot be confirmed; never delete
            // pixels which may still belong to an accepted or retained message.
        }
    }

    private func releaseScreenFrameHistory(sessionID: String, workspace: URL) async {
        do {
            let unavailable = try await AIMobileScreenFrameArchive.shared.releaseSession(
                workspace: workspace, sessionID: sessionID)
            let released = Set(screenFrameHistoryPins.filter { $0.value == sessionID }.keys)
            screenFrameHistoryPins = screenFrameHistoryPins.filter { $0.value != sessionID }
            // An uncertain native send can leave its already-pinned item in
            // the draft. Explicit history cleanup also removes that stale
            // draft item; newer selections have different tokens and remain.
            for key in Array(pendingAttachments.keys) {
                pendingAttachments[key]?.removeAll {
                    $0.archiveReference.map { released.contains($0.referenceID) } ?? false
                }
                if pendingAttachments[key]?.isEmpty == true { pendingAttachments.removeValue(forKey: key) }
            }
            if unavailable > 0 {
                showTransientError("对话已清理，部分旧图片批次不可读取，原文件仍保留。")
            }
        } catch {
            showTransientError("对话已清理，但截图引用清理失败：" + error.localizedDescription)
        }
    }

    func removeAttachment(_ file: AIImportedFile, from session: String) {
        let key = attachmentKey(for: session)
        let removed = pendingAttachments[key]?.filter {
            $0.attachmentIdentity == file.attachmentIdentity
        } ?? []
        pendingAttachments[key]?.removeAll { $0.attachmentIdentity == file.attachmentIdentity }
        if pendingAttachments[key]?.isEmpty == true {
            pendingAttachments.removeValue(forKey: key)
        }
        discardImportedFiles(removed)
    }
    func importFile(_ url: URL) async -> AIImportedFile? {
        do { return try await backend.importDocument(url) }
        catch { self.error = error.localizedDescription; return nil }
    }

    func importVideoFile(_ url: URL) async -> AIImportedFile? {
        do { return try await backend.importVideo(url) }
        catch { self.error = error.localizedDescription; return nil }
    }

    func importSessionFile(_ url: URL) async -> AIImportedFile? {
        do { return try await backend.importSessionFile(url) }
        catch { self.error = error.localizedDescription; return nil }
    }
}
