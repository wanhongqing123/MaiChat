// 文件作用：为共享 Session/IM 路由适配 iOS UI、原 SDK 边界和附件导入。
// 队列、因果归属、去重与交付授权只由 Native kernel 实现；本文件不另建 Agent 执行器。
import Foundation
import SwiftUI
import MaiChatCore

indirect enum IMSessionJSON: Codable, Equatable, Sendable {
    case object([String: IMSessionJSON]), array([IMSessionJSON]), string(String)
    case integer(Int64), decimal(Double), bool(Bool), null
    init(from decoder: Decoder) throws {
        let c = try decoder.singleValueContainer()
        if c.decodeNil() { self = .null }
        else if let v = try? c.decode(Bool.self) { self = .bool(v) }
        else if let v = try? c.decode(String.self) { self = .string(v) }
        else if let v = try? c.decode(Int64.self) { self = .integer(v) }
        else if let v = try? c.decode(Double.self) { self = .decimal(v) }
        else if let v = try? c.decode([String: IMSessionJSON].self) { self = .object(v) }
        else { self = .array(try c.decode([IMSessionJSON].self)) }
    }
    func encode(to encoder: Encoder) throws {
        var c = encoder.singleValueContainer()
        switch self {
        case .object(let v): try c.encode(v)
        case .array(let v): try c.encode(v)
        case .string(let v): try c.encode(v)
        case .integer(let v): try c.encode(v)
        case .decimal(let v): try c.encode(v)
        case .bool(let v): try c.encode(v)
        case .null: try c.encodeNil()
        }
    }
    var string: String? { if case .string(let v) = self { return v }; return nil }
    static func value<T: Encodable>(_ value: T) throws -> Self {
        try JSONDecoder().decode(Self.self, from: JSONEncoder().encode(value))
    }
}

struct IMSessionBindingScope: Codable, Equatable, Sendable {
    let sdkAppID: String
    let ownerID: String
    let authGeneration: String
    let controllerGeneration: String
    enum CodingKeys: String, CodingKey {
        case sdkAppID = "sdk_app_id", ownerID = "owner_id"
        case authGeneration = "auth_generation", controllerGeneration = "controller_generation"
    }
}
struct IMSessionBindingSession: Decodable, Identifiable, Equatable, Sendable {
    let id: String
    let title: String
    let busy: Bool
    let workspaceKey: String
    enum CodingKeys: String, CodingKey { case id, title, busy; case workspaceKey = "workspace_key" }
    var displayTitle: String { title.isEmpty || title == "New session" ? "新对话" : title }
}
struct IMSessionBindingItem: Codable, Equatable, Sendable {
    let peerID: String
    let sessionID: String
    let sessionTitle: String
    var displayTitle: String { sessionTitle.isEmpty || sessionTitle == "New session" ? "新对话" : sessionTitle }
    let epoch: String
    var historyState: String? = nil
    var importedCount: IMSessionJSON? = nil
    var isHistoryReady: Bool { historyState == "ready" }
    enum CodingKeys: String, CodingKey {
        case peerID = "peer_id", sessionID = "session_id", sessionTitle = "session_title", epoch
        case historyState = "history_state", importedCount = "imported_count"
    }
}

/// 整份 owned delivery 保持未知扩展字段，不从显示窗口/最后消息猜测回复归属。
struct IMSessionDelivery: Codable, Equatable, Sendable {
    let fields: [String: IMSessionJSON]
    init(from decoder: Decoder) throws { fields = try decoder.singleValueContainer().decode([String: IMSessionJSON].self) }
    func encode(to encoder: Encoder) throws { var c = encoder.singleValueContainer(); try c.encode(fields) }
    func string(_ key: String) -> String { fields[key]?.string ?? "" }
    var bridgeID: String { string("bridge_id") }
    var peerID: String { string("peer_id") }
    var sessionID: String { string("session_id") }
    var identity: String { bridgeID + ":" + string("token") + ":" + string("dispatch_index") }
    var scope: IMSessionBindingScope? {
        guard let value = fields["scope"], let data = try? JSONEncoder().encode(value) else { return nil }
        return try? JSONDecoder().decode(IMSessionBindingScope.self, from: data)
    }
}

/// Native 单次发出的原始 task 全程不改写，import_id 的新值只来自 progress。
struct IMSessionHistoryTask: Codable, Sendable {
    let fields: [String: IMSessionJSON]
    init(from decoder: Decoder) throws { fields = try decoder.singleValueContainer().decode([String: IMSessionJSON].self) }
    func encode(to encoder: Encoder) throws { var c = encoder.singleValueContainer(); try c.encode(fields) }
    func string(_ key: String) -> String { fields[key]?.string ?? "" }
    var identity: String { string("bridge_id") + ":" + string("task_id") }
    var scope: IMSessionBindingScope? {
        guard let value = fields["scope"], let data = try? JSONEncoder().encode(value) else { return nil }
        return try? JSONDecoder().decode(IMSessionBindingScope.self, from: data)
    }
}
struct IMSessionBindingLease: Equatable, Sendable {
    let bridgeID: String
    let scope: IMSessionBindingScope
}

enum IMSessionHistoryIO {
    static func request(_ fields: [String: IMSessionJSON]) async throws -> [String: IMSessionJSON] {
        let data = try JSONEncoder().encode(fields)
        let text = String(decoding: data, as: UTF8.self)
        // Caller 取消不能吞掉 open 刚分配的 snapshot_id。I/O 仍由原 prep worker 执行，
        // 先交付 owned 结果给 finally，再由 Native scope 取消/显式 close 释放读连接。
        return try await Task.detached(priority: .userInitiated) {
            try await RemoteIMBackgroundWork.metadata {
                guard let pointer = text.withCString({ maiMobileIMHistorySnapshot($0) }) else { throw LocalChatMessageQueryError.unavailable }
                defer { maiMobileAgentFree(pointer) }
                let result = try JSONDecoder().decode([String: IMSessionJSON].self, from: Data(String(cString: pointer).utf8))
                guard result["ok"] == .bool(true) else {
                    throw AIBackendError(message: result["error"]?.string ?? "读取本地聊天历史失败",
                                         code: result["errorCode"]?.string)
                }
                return result
            }
        }.value
    }
}

struct IMSessionBindingProjection: Decodable, Sendable {
    let ok: Bool
    let bridgeID: String?
    let scope: IMSessionBindingScope?
    let sessions: [IMSessionBindingSession]
    let bindings: [IMSessionBindingItem]
    let hasBindings: Bool
    let historyCleanupPending: Bool
    let pendingCount: Int
    let deliveries: [IMSessionDelivery]
    let error: String?
    let errorCode: String?
    let warnings: [[String: IMSessionJSON]]
    let accepted: Bool?
    let historyTasks: [IMSessionHistoryTask]
    let historyProgress: IMSessionJSON?
    let pageAccepted: Bool?
    let reason: String?
    // 仅 Swift worker→Main 的本地发布序号，不增加 Native JSON 字段。
    var hostPublication: UInt64 = 0
    enum CodingKeys: String, CodingKey {
        case ok, scope, sessions, bindings, deliveries, error, errorCode, warnings, accepted, reason
        case historyTasks = "history_tasks", historyProgress = "history_progress", pageAccepted = "page_accepted"
        case bridgeID = "bridge_id", hasBindings = "has_bindings", pendingCount = "pending_count"
        case historyCleanupPending = "history_cleanup_pending"
    }
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        ok = try c.decode(Bool.self, forKey: .ok)
        bridgeID = try c.decodeIfPresent(String.self, forKey: .bridgeID)
        scope = try c.decodeIfPresent(IMSessionBindingScope.self, forKey: .scope)
        sessions = try c.decodeIfPresent([IMSessionBindingSession].self, forKey: .sessions) ?? []
        bindings = try c.decodeIfPresent([IMSessionBindingItem].self, forKey: .bindings) ?? []
        hasBindings = try c.decodeIfPresent(Bool.self, forKey: .hasBindings) ?? false
        historyCleanupPending = try c.decodeIfPresent(Bool.self, forKey: .historyCleanupPending) ?? false
        pendingCount = try c.decodeIfPresent(Int.self, forKey: .pendingCount) ?? 0
        deliveries = try c.decodeIfPresent([IMSessionDelivery].self, forKey: .deliveries) ?? []
        error = try c.decodeIfPresent(String.self, forKey: .error)
        errorCode = try c.decodeIfPresent(String.self, forKey: .errorCode)
        warnings = try c.decodeIfPresent([[String: IMSessionJSON]].self, forKey: .warnings) ?? []
        accepted = try c.decodeIfPresent(Bool.self, forKey: .accepted)
        historyTasks = try c.decodeIfPresent([IMSessionHistoryTask].self, forKey: .historyTasks) ?? []
        historyProgress = try c.decodeIfPresent(IMSessionJSON.self, forKey: .historyProgress)
        pageAccepted = try c.decodeIfPresent(Bool.self, forKey: .pageAccepted)
        reason = try c.decodeIfPresent(String.self, forKey: .reason)
    }
}
struct IMSessionBindingRequest: Sendable {
    var fields: [String: IMSessionJSON]
    init(_ action: String, bridgeID: String? = nil, scope: IMSessionBindingScope? = nil) {
        fields = ["action": .string(action)]
        if let bridgeID { fields["bridge_id"] = .string(bridgeID) }
        if let scope { fields["scope"] = try? IMSessionJSON.value(scope) }
    }
    mutating func set(_ key: String, _ value: String) { fields[key] = .string(value) }
    mutating func set(_ key: String, _ value: Bool) { fields[key] = .bool(value) }
    mutating func setStrings(_ key: String, _ values: [String]) { fields[key] = .array(values.map(IMSessionJSON.string)) }
}

struct IMSessionBindingHostIdentity: Equatable, Sendable {
    let sdkAppID: String
    let ownerID: String
    let generation: String
    let epoch: UInt64
}
struct IMSessionIngressCapture: Sendable {
    let bridgeID: String
    let scope: IMSessionBindingScope
    let peerID: String
    let messageID: String
    let expectedSessionID: String
    let expectedBindingEpoch: String
}
struct IMSessionIngressTicket: Equatable, Sendable {
    let bridgeID: String
    let scope: IMSessionBindingScope
    let peerID: String
    let messageID: String
    let expectedSessionID: String
    let expectedBindingEpoch: String
    let inputID: String
}

/// Main 发布只读 projection，SDK 原 live 回调在排队、下载之前同步捕获。
/// 捕获缺失不补发；旧 ticket 即使在重绑后完成导入，也只能由 Native 拒绝，不能改投新会话。
final class IMSessionBindingCaptureRegistry: @unchecked Sendable {
    static let shared = IMSessionBindingCaptureRegistry()
    private let lock = NSLock()
    private var host: IMSessionBindingHostIdentity?
    private var controllerHighWater: UInt64 = 0
    private var publication: UInt64 = 0
    private var restoredScope: IMSessionBindingScope?
    private var bridgeID: String?
    private var scope: IMSessionBindingScope?
    private var bindings: [String: IMSessionBindingItem] = [:]
    private var authoritativeBindings: [String: IMSessionBindingItem] = [:]
    private var acceptedFriends = Set<String>()
    private var suppressedPeers = Set<String>()
    func authorizeHost(_ identity: IMSessionBindingHostIdentity?) {
        lock.lock(); let changed = host != identity; lock.unlock()
        if changed { revoke("logout") }
        lock.lock(); host = identity
        if changed { suppressedPeers = []; acceptedFriends = [] }
        lock.unlock()
    }
    func isHostCurrent(_ identity: IMSessionBindingHostIdentity) -> Bool {
        lock.lock(); defer { lock.unlock() }; return host == identity
    }
    @discardableResult
    func publish(bridgeID: String, scope: IMSessionBindingScope, bindings: [IMSessionBindingItem],
                 hostIdentity: IMSessionBindingHostIdentity) -> UInt64? {
        lock.lock()
        guard let host, host == hostIdentity, host.sdkAppID == scope.sdkAppID, host.ownerID == scope.ownerID,
              host.generation == scope.authGeneration,
              let generation = UInt64(scope.controllerGeneration), generation > 0,
              generation >= controllerHighWater,
              generation != controllerHighWater || self.bridgeID == nil || self.bridgeID == bridgeID
        else { lock.unlock(); return nil }
        controllerHighWater = generation
        if self.scope != scope || self.bridgeID != bridgeID { restoredScope = nil }
        self.bridgeID = bridgeID; self.scope = scope
        self.authoritativeBindings = Dictionary(bindings.map { ($0.peerID, $0) }, uniquingKeysWith: { _, last in last })
        self.bindings = Dictionary(bindings.filter {
            acceptedFriends.contains($0.peerID) && !suppressedPeers.contains($0.peerID)
        }.map { ($0.peerID, $0) }, uniquingKeysWith: { _, last in last })
        publication &+= 1
        let revision = publication
        lock.unlock()
        return revision
    }
    func markRestored(scope: IMSessionBindingScope, hostIdentity: IMSessionBindingHostIdentity) {
        lock.lock(); defer { lock.unlock() }
        if host == hostIdentity && self.scope == scope { restoredScope = scope }
    }
    func canPersist(_ value: IMSessionBindingProjection, hostIdentity: IMSessionBindingHostIdentity) -> Bool {
        lock.lock(); defer { lock.unlock() }
        return host == hostIdentity && restoredScope == value.scope && publication == value.hostPublication &&
            self.bridgeID == value.bridgeID && value.hostPublication > 0
    }
    func isPublished(_ revision: UInt64, bridgeID: String) -> Bool {
        lock.lock(); defer { lock.unlock() }
        return revision > 0 && publication == revision && self.bridgeID == bridgeID
    }
    func clear(bridgeID: String, scope: IMSessionBindingScope) {
        lock.lock()
        if self.bridgeID == bridgeID && self.scope == scope {
            self.bridgeID = nil; self.scope = nil; bindings = [:]; authoritativeBindings = [:]
        }
        lock.unlock()
        // 清理的是所属旧 bridge，不能顺便清掉新 configure 发布的 authority。
        IMSessionBindingControl.revoke("logout", bridgeID: bridgeID, scope: scope)
    }
    func captureIngress(accountTag: String?, peerID: String, messageID: String, recipientID: String) -> IMSessionIngressCapture? {
        lock.lock(); defer { lock.unlock() }
        guard let bridgeID, let scope, let sdk = Int(scope.sdkAppID),
              accountTag == RemoteDiagnosticsProtocol.accountTag(sdkAppID: sdk, ownerUserID: scope.ownerID),
              recipientID == scope.ownerID, !messageID.isEmpty, let item = bindings[peerID], !item.epoch.isEmpty else { return nil }
        return IMSessionIngressCapture(bridgeID: bridgeID, scope: scope, peerID: peerID,
            messageID: messageID, expectedSessionID: item.sessionID, expectedBindingEpoch: item.epoch)
    }
    func admit(_ captured: IMSessionIngressCapture?, origin: RemoteIMMessageOrigin?, group: Bool,
               controlFrame: Bool, hasContent: Bool) -> IMSessionIngressTicket? {
        guard let captured else { return nil }
        var fields = IMSessionBindingRequest("admit", bridgeID: captured.bridgeID, scope: captured.scope).fields
        fields["peer_id"] = .string(captured.peerID); fields["message_id"] = .string(captured.messageID)
        fields["sender_id"] = .string(captured.peerID); fields["recipient_id"] = .string(captured.scope.ownerID)
        fields["origin"] = .string(origin?.rawValue ?? "unknown")
        fields["live"] = .bool(true); fields["group"] = .bool(group)
        fields["control_frame"] = .bool(controlFrame); fields["has_content"] = .bool(hasContent)
        fields["expected_binding_epoch"] = .string(captured.expectedBindingEpoch)
        fields["expected_session_id"] = .string(captured.expectedSessionID)
        guard let result = IMSessionBindingControl.response(fields), result["ok"] == .bool(true),
              result["accepted"] == .bool(true), let input = result["input_id"]?.string, !input.isEmpty else { return nil }
        return IMSessionIngressTicket(bridgeID: captured.bridgeID, scope: captured.scope, peerID: captured.peerID,
            messageID: captured.messageID, expectedSessionID: captured.expectedSessionID,
            expectedBindingEpoch: captured.expectedBindingEpoch, inputID: input)
    }
    func admit(accountTag: String?, peerID: String, messageID: String, origin: RemoteIMMessageOrigin?,
               recipientID: String, group: Bool, controlFrame: Bool, hasContent: Bool) -> IMSessionIngressTicket? {
        admit(captureIngress(accountTag: accountTag, peerID: peerID, messageID: messageID, recipientID: recipientID),
              origin: origin, group: group, controlFrame: controlFrame, hasContent: hasContent)
    }
    func isCurrent(_ task: IMSessionHistoryTask) -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard bridgeID == task.string("bridge_id"), scope == task.scope,
              let binding = bindings[task.string("peer_id")] else { return false }
        return binding.sessionID == task.string("session_id") && binding.epoch == task.string("binding_epoch")
    }
    func isCurrent(_ ticket: IMSessionIngressTicket) -> Bool {
        lock.lock(); defer { lock.unlock() }
        return bridgeID == ticket.bridgeID && scope == ticket.scope &&
            bindings[ticket.peerID]?.sessionID == ticket.expectedSessionID &&
            bindings[ticket.peerID]?.epoch == ticket.expectedBindingEpoch
    }
    func revoke(_ action: String, sessionID: String? = nil) {
        lock.lock()
        let currentBridge = bridgeID, currentScope = scope
        if action == "logout" { bridgeID = nil; scope = nil; bindings = [:]; authoritativeBindings = [:] }
        lock.unlock()
        if let currentBridge, let currentScope {
            IMSessionBindingControl.revoke(action, bridgeID: currentBridge, scope: currentScope, sessionID: sessionID)
        }
    }
    func setAcceptedFriends(_ friends: Set<String>) {
        lock.lock(); acceptedFriends = friends
        let removed = bindings.values.filter { !friends.contains($0.peerID) }
        lock.unlock()
        for binding in removed { withdraw(peerID: binding.peerID) }
    }
    func allowPeer(_ peer: String) {
        lock.lock(); suppressedPeers.remove(peer)
        if acceptedFriends.contains(peer) { bindings[peer] = authoritativeBindings[peer] }
        lock.unlock()
    }
    func withdraw(peerID: String) {
        lock.lock()
        let item = bindings.removeValue(forKey: peerID), bridge = bridgeID, currentScope = scope
        suppressedPeers.insert(peerID)
        lock.unlock()
        if let item, let bridge, let currentScope {
            IMSessionBindingControl.revoke("takeover", bridgeID: bridge, scope: currentScope, sessionID: item.sessionID)
        }
    }
    func allows(_ delivery: IMSessionDelivery) -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard let scope, self.bridgeID == delivery.bridgeID, scope == delivery.scope,
              let binding = bindings[delivery.peerID] else { return false }
        return binding.sessionID == delivery.sessionID
    }
}

enum IMSessionBindingControl {
    /// 无 raw handle/worker 递归；仅在 SDK 派发边界和本地接管入口操作共享权威。
    static func response(_ request: [String: IMSessionJSON]) -> [String: IMSessionJSON]? {
        guard let data = try? JSONEncoder().encode(request), let text = String(data: data, encoding: .utf8),
              let response = text.withCString({ maiMobileSessionIMControl($0) }) else { return nil }
        defer { maiMobileAgentFree(response) }
        return try? JSONDecoder().decode([String: IMSessionJSON].self,
            from: Data(String(cString: response).utf8))
    }
    static func accepts(action: String?, response: [String: IMSessionJSON]) -> Bool {
        guard response["ok"] == .bool(true) else { return false }
        return action == "claim" ? response["allowed"] == .bool(true) : true
    }
    static func call(_ request: [String: IMSessionJSON]) -> Bool {
        guard let value = response(request) else { return false }
        return accepts(action: request["action"]?.string, response: value)
    }
    static func persistedInput(_ ticket: IMSessionIngressTicket, receipt: RemoteIMMetadataReceipt, remoteID: String?) -> Bool {
        var request = IMSessionBindingRequest("persisted_input", bridgeID: ticket.bridgeID, scope: ticket.scope)
        request.set("input_id", ticket.inputID); request.set("inserted", receipt.wasInserted)
        request.set("source_row_id", receipt.localID.uuidString)
        if let remoteID, !remoteID.isEmpty { request.set("remote_id", remoteID) }
        return call(request.fields)
    }
    static func abandon(_ ticket: IMSessionIngressTicket?, reason: String = "canceled") {
        guard let ticket else { return }
        var request = IMSessionBindingRequest("abandon_input", bridgeID: ticket.bridgeID, scope: ticket.scope)
        request.set("input_id", ticket.inputID)
        request.set("reason", reason)
        _ = call(request.fields)
    }
    static func delivery(_ action: String, _ delivery: IMSessionDelivery) -> Bool {
        call(["action": .string(action), "delivery": .object(delivery.fields)])
    }
    static func revoke(_ action: String, bridgeID: String, scope: IMSessionBindingScope, sessionID: String? = nil) {
        var value: [String: IMSessionJSON] = ["action": .string(action), "bridge_id": .string(bridgeID)]
        value["scope"] = try? IMSessionJSON.value(scope)
        if let sessionID { value["session_id"] = .string(sessionID) }
        _ = call(value)
    }
}

final class IMSessionDispatchPermit: @unchecked Sendable {
    let delivery: IMSessionDelivery
    private let lock = NSLock()
    private var acknowledged = false
    init(delivery: IMSessionDelivery) { self.delivery = delivery }
    func claim(accountTag: String?) -> Bool {
        guard let scope = delivery.scope, let sdk = Int(scope.sdkAppID),
              accountTag == RemoteDiagnosticsProtocol.accountTag(sdkAppID: sdk, ownerUserID: scope.ownerID),
              IMSessionBindingCaptureRegistry.shared.allows(delivery) else { return false }
        return IMSessionBindingControl.delivery("claim", delivery)
    }
    /// ack 仅确认 SDK 调用已发起或已放弃，不代表网络送达。多层清理仍只确认一次。
    func acknowledge() {
        lock.lock()
        guard !acknowledged else { lock.unlock(); return }
        acknowledged = true; lock.unlock()
        _ = IMSessionBindingControl.delivery("ack", delivery)
    }
}

/// 仅搬运原 SDK owned payload；此处没有重排、重试或历史补发队列。
struct IMSessionIncomingPayload: Sendable {
    let ticket: IMSessionIngressTicket
    let text: String
    let kind: String
    let fileURL: URL?
    let quoteText: String?
}

enum IMSessionOutgoingPayload: Sendable {
    case text(String)
    case image(RemoteIMImageFile, String?)
    case video(RemoteIMVideoFile, String?)
    case file(RemoteIMFile, String?)
}

enum IMSessionBindingPreferences {
    static func key(_ identity: IMSessionBindingHostIdentity) -> String {
        "maichat.im-session-bindings." + identity.sdkAppID + "." + identity.ownerID
    }
    static func persist(_ value: IMSessionBindingProjection, host: IMSessionBindingHostIdentity) {
        guard IMSessionBindingCaptureRegistry.shared.canPersist(value, hostIdentity: host) else { return }
        UserDefaults.standard.set(Dictionary(value.bindings.filter(\.isHistoryReady).map { ($0.peerID, $0.sessionID) },
            uniquingKeysWith: { _, last in last }), forKey: key(host))
    }
}

@MainActor
enum IMSessionBindingChanges {
    // 普通 Native change callback 唤醒现有宿主；不依赖创作页 ready/listener。
    static var onChange: (() -> Void)?
}

@MainActor
final class IMSessionBindingController {
    private weak var appState: RemoteIMAppState?
    private var identity: IMSessionBindingHostIdentity?
    private var projection: IMSessionBindingProjection?
    private var poll: Task<Void, Never>?
    private var openingIdentity: IMSessionBindingHostIdentity?
    private var changeTask: Task<Void, Never>?
    private var changePending = false
    private var showingNativeWarning = false
    private var loggedMaintenanceRetry = false
    private var operationFailure = false
    private var locallyCanceledPeers = Set<String>()
    private var inFlight = Set<String>()
    private var historyPreparations: [String: Task<Void, Never>] = [:]
    private var restoredBridge = ""
    init(appState: RemoteIMAppState) {
        self.appState = appState
        IMSessionBindingChanges.onChange = { [weak self] in
            guard let self else { return }
            self.changePending = true
            guard self.poll == nil, self.changeTask == nil else { return }
            self.changeTask = Task { @MainActor [weak self] in
                self?.changePending = false
                await self?.refresh()
                self?.changeTask = nil
            }
        }
    }
    deinit { poll?.cancel(); changeTask?.cancel(); for task in historyPreparations.values { task.cancel() } }

    func invalidate() {
        if let bridge = projection?.bridgeID, let scope = projection?.scope {
            IMSessionBindingCaptureRegistry.shared.clear(bridgeID: bridge, scope: scope)
        }
        identity = nil; projection = nil; restoredBridge = ""
        showingNativeWarning = false; operationFailure = false; locallyCanceledPeers = []
        loggedMaintenanceRetry = false
        for task in historyPreparations.values { task.cancel() }
        historyPreparations = [:]
        poll?.cancel(); poll = nil
        appState?.publishIMSessionBinding(nil)
    }
    private func accept(_ value: IMSessionBindingProjection, expected: IMSessionBindingHostIdentity) throws {
        guard appState?.imSessionHostIdentity == expected, identity == expected,
              value.ok, let bridge = value.bridgeID, let scope = value.scope,
              scope.ownerID == expected.ownerID, scope.sdkAppID == expected.sdkAppID,
              scope.authGeneration == expected.generation else { throw CancellationError() }
        // one-shot I/O task 即使比更新的 Main 显示快照迟到，也须独立按原 scope/epoch处理。
        scheduleHistory(value.historyTasks, expected: expected)
        // 只有 serial backend 发布 authority；Main 的旧 await completion 不可再写回快照。
        guard IMSessionBindingCaptureRegistry.shared.isPublished(value.hostPublication, bridgeID: bridge) else { return }
        projection = value
        appState?.publishIMSessionBinding(value)
        if restoredBridge == bridge { persist(value, identity: expected) }
        acceptWarnings(value.warnings, cleanupPending: value.historyCleanupPending)
    }
    // Native 投影的本地提示在同一入口分类；维护重试与消息派发结果的语义不同。
    private func acceptWarnings(_ warnings: [[String: IMSessionJSON]], cleanupPending: Bool) {
        if !cleanupPending { loggedMaintenanceRetry = false }
        let liveWarnings = warnings.filter { warning in
            guard warning["code"]?.string == "history_cleanup_failed" else { return true }
            // 后台回收会自动重试，无用户动作；不把它变成派发失败，也不清真正的 live 提示。
            if cleanupPending && !loggedMaintenanceRetry {
                NSLog("IMSessionBinding: deferred background history cleanup; automatic retry pending")
                loggedMaintenanceRetry = true
            }
            return false
        }
        if !liveWarnings.isEmpty {
            showingNativeWarning = true
            appState?.imSessionBindingError = liveWarnings.map {
                let code = $0["code"]?.string ?? "binding_failed"
                let outcome = $0["outcome"]?.string ?? "not_submitted"
                if outcome == "unknown" { return "回复送达状态未知，未自动重发。" }
                switch code {
                case "history_live_overflow": return "导入期间收到的新消息过多，绑定未完成，请重试。"
                case "history_input_failed", "history_input_timeout": return "有消息尚未保存或准备好，绑定未完成，请重试。"
                case "history_timeout": return "聊天记录导入超时，未完成绑定，请重试。"
                case "history_source_failed": return "本地聊天记录读取失败，请重试。"
                case "delivery_timeout": return "回复发送超时，未自动重发。"
                default: return "消息未提交到会话，请检查设置后重试。"
                }
            }.joined(separator: "\n")
            if let message = appState?.imSessionBindingError { appState?.showIMSessionBindingWarning(message) }
        }
    }
    #if DEBUG
    // 自有宿主例走正式 warning 入口，不创建 Native 请求、SDK 登录或模型调用。
    func acceptWarningsForUITesting(_ warnings: [[String: IMSessionJSON]], cleanupPending: Bool) {
        acceptWarnings(warnings, cleanupPending: cleanupPending)
    }
    #endif
    private func request(_ action: String) throws -> IMSessionBindingRequest {
        guard let value = projection, let bridge = value.bridgeID, let scope = value.scope,
              appState?.imSessionHostIdentity == identity else { throw CancellationError() }
        return IMSessionBindingRequest(action, bridgeID: bridge, scope: scope)
    }
    private func persist(_ value: IMSessionBindingProjection, identity: IMSessionBindingHostIdentity) {
        IMSessionBindingPreferences.persist(value, host: identity)
    }
    func refresh() async {
        guard let appState, appState.imSessionAcceptedFriendsLoaded,
              let expected = appState.imSessionHostIdentity else { return }
        if openingIdentity == expected {
            // 同 scope 的普通调用等待同一次刷新，不能把“另一路还在读”冒充空列表。
            while openingIdentity == expected && appState.imSessionHostIdentity == expected {
                if Task.isCancelled { return }
                try? await Task.sleep(for: .milliseconds(10))
            }
            return
        }
        openingIdentity = expected
        appState.imSessionBindingIsPreparing = true
        defer {
            // 旧账号的等待不能堵住新账号，也不能在迟到完成时清掉新账号的加载状态。
            if openingIdentity == expected {
                openingIdentity = nil
                appState.imSessionBindingIsPreparing = false
            }
        }
        do {
            if identity != expected { invalidate(); identity = expected }
            IMSessionBindingCaptureRegistry.shared.authorizeHost(expected)
            IMSessionBindingCaptureRegistry.shared.setAcceptedFriends(appState.acceptedFriendIMIDs)
            var activate = IMSessionBindingRequest("activate")
            activate.set("sdk_app_id", expected.sdkAppID); activate.set("owner_id", expected.ownerID)
            activate.set("auth_generation", expected.generation)
            activate.setStrings("friends", appState.acceptedFriendIMIDs.sorted())
            activate.fields["history_source"] = .object(appState.imSessionHistorySource)
            let value = try await AIAssistantModel.shared.imSessionBindingRequest(activate, host: expected)
            // 成功读取后清除旧的准备/读取错误；本轮或此前尚未处理的 Native warning 单独保留。
            if !showingNativeWarning && !operationFailure { appState.imSessionBindingError = nil }
            try accept(value, expected: expected)
            // 恢复的是用户明确选定的关系；绝不回放缓存聊天消息。
            if restoredBridge != value.bridgeID {
                restoredBridge = value.bridgeID ?? ""
                let saved = UserDefaults.standard.dictionary(forKey: IMSessionBindingPreferences.key(expected)) as? [String: String] ?? [:]
                for (peer, session) in saved.sorted(by: { $0.key < $1.key }) {
                    guard appState.acceptedFriendIMIDs.contains(peer),
                          value.sessions.contains(where: { $0.id == session }),
                          !value.bindings.contains(where: { $0.peerID == peer }) else { continue }
                    var bind = try request("bind"); bind.set("peer_id", peer); bind.set("session_id", session)
                    guard let workspace = value.sessions.first(where: { $0.id == session })?.workspaceKey else { continue }
                    bind.set("expected_workspace_key", workspace)
                    let restored = try await AIAssistantModel.shared.imSessionBindingRequest(bind, host: expected)
                    try accept(restored, expected: expected)
                }
            }
            if let projection, let scope = projection.scope {
                IMSessionBindingCaptureRegistry.shared.markRestored(scope: scope, hostIdentity: expected)
                persist(projection, identity: expected)
            }
            startPolling()
        } catch {
            if appState.imSessionHostIdentity == expected {
                appState.imSessionBindingError = error.localizedDescription
            }
        }
    }
    func bind(peer: String, session: String?, displayedLease: IMSessionBindingLease? = nil, expectedWorkspaceKey: String? = nil) async -> Bool {
        guard let appState, let expected = appState.imSessionHostIdentity, expected == identity else { return false }
        do {
            operationFailure = false
            if session == nil { locallyCanceledPeers.insert(peer) } else { locallyCanceledPeers.remove(peer) }
            if let displayedLease {
                guard projection?.bridgeID == displayedLease.bridgeID, projection?.scope == displayedLease.scope else {
                    throw AIBackendError(message: "会话列表已更新，请重新选择")
                }
            }
            // 只有明确解绑才提前撤销。改绑由 Native 原子校验；失败保留原关系与 pending。
            if session == nil { IMSessionBindingCaptureRegistry.shared.withdraw(peerID: peer) }
            var value = try request(session == nil ? "unbind" : "bind")
            value.set("peer_id", peer)
            if let session {
                guard let workspace = expectedWorkspaceKey ?? projection?.sessions.first(where: { $0.id == session })?.workspaceKey,
                      !workspace.isEmpty else { throw AIBackendError(message: "会话列表已更新，请重新选择") }
                value.set("session_id", session); value.set("expected_workspace_key", workspace)
            }
            let result = try await AIAssistantModel.shared.imSessionBindingRequest(value, host: expected)
            IMSessionBindingCaptureRegistry.shared.allowPeer(peer)
            try accept(result, expected: expected)
            showingNativeWarning = false
            appState.imSessionBindingError = nil
            startPolling()
            if let session {
                guard let submittedEpoch = result.bindings.first(where: { $0.peerID == peer })?.epoch else {
                    throw AIBackendError(message: "历史导入未完成，请重试")
                }
                // 只等待 Native 的 ready 投影，不回放历史或在 Swift 驱动模型执行。
                while appState.imSessionHostIdentity == expected {
                    try Task.checkCancellation()
                    if locallyCanceledPeers.contains(peer) { throw CancellationError() }
                    guard let binding = projection?.bindings.first(where: { $0.peerID == peer }) else {
                        throw AIBackendError(message: appState.imSessionBindingError ?? "历史导入未完成，请重试")
                    }
                    guard binding.sessionID == session && binding.epoch == submittedEpoch else { throw CancellationError() }
                    if binding.isHistoryReady { return true }
                    try await Task.sleep(for: .milliseconds(30))
                }
                throw CancellationError()
            }
            return true
        } catch {
            guard appState.imSessionHostIdentity == expected else { return false }
            if error is CancellationError { return false }
            operationFailure = true
            appState.imSessionBindingError = error.localizedDescription
            IMSessionBindingCaptureRegistry.shared.allowPeer(peer)
            await refresh(); return false
        }
    }
    func receive(_ payload: IMSessionIncomingPayload) async {
        let ticket = payload.ticket
        var received = false
        defer { if !received { IMSessionBindingControl.abandon(ticket) } }
        guard let appState, let expected = appState.imSessionHostIdentity, expected == identity,
              IMSessionBindingCaptureRegistry.shared.isCurrent(ticket) else { return }
        do {
            var value = try request("receive")
            value.set("input_id", ticket.inputID)
            value.set("expected_binding_epoch", ticket.expectedBindingEpoch)
            value.set("expected_session_id", ticket.expectedSessionID)
            value.set("peer_id", ticket.peerID); value.set("message_id", ticket.messageID)
            value.set("sender_id", ticket.peerID); value.set("recipient_id", ticket.scope.ownerID)
            value.set("origin", "human"); value.set("live", true); value.set("group", false)
            value.set("control_frame", false)
            var text = payload.text
            if let quote = payload.quoteText, !quote.isEmpty { text = "引用：\(quote)\n\n" + text }
            if let url = payload.fileURL {
                let file = try await AIAssistantModel.shared.importIMSessionInput(url, kind: payload.kind)
                guard appState.imSessionHostIdentity == expected,
                      IMSessionBindingCaptureRegistry.shared.isCurrent(ticket) else { return }
                if payload.kind == "image" || payload.kind == "video" {
                    value.fields[payload.kind == "image" ? "images" : "videos"] = .array([
                        .object(["path": .string(file.relativePath), "mimeType": .string(file.mimeType)])])
                } else { text += "\n附件：" + file.relativePath }
            }
            value.set("text", text)
            let result = try await AIAssistantModel.shared.imSessionBindingRequest(value, host: expected)
            try accept(result, expected: expected)
            guard result.accepted == true else { return }
            received = true; startPolling()
        } catch {
            if appState.imSessionHostIdentity == expected { appState.imSessionBindingError = error.localizedDescription }
        }
    }
    private func scheduleHistory(_ tasks: [IMSessionHistoryTask], expected: IMSessionBindingHostIdentity) {
        for task in tasks where historyPreparations[task.identity] == nil {
            historyPreparations[task.identity] = Task { @MainActor [weak self] in
                await self?.prepareHistory(task, expected: expected)
                self?.historyPreparations.removeValue(forKey: task.identity)
            }
        }
    }
    private func prepareHistory(_ task: IMSessionHistoryTask, expected: IMSessionBindingHostIdentity) async {
        var snapshotID: String?
        do {
            guard let appState, appState.imSessionHostIdentity == expected,
                  IMSessionBindingCaptureRegistry.shared.isCurrent(task) else { throw CancellationError() }
            try await appState.awaitIMHistoryPersistenceFence(task)
            let openDeadline = DispatchTime.now().uptimeNanoseconds + 300_000_000_000
            var opened: [String: IMSessionJSON]
            while true {
                try Task.checkCancellation()
                guard appState.imSessionHostIdentity == expected, IMSessionBindingCaptureRegistry.shared.isCurrent(task) else { throw CancellationError() }
                do {
                    opened = try await IMSessionHistoryIO.request(["action": .string("open"), "task": .object(task.fields)])
                    break
                } catch let error as AIBackendError where error.code == "busy" {
                    guard DispatchTime.now().uptimeNanoseconds < openDeadline else { throw error }
                    try await Task.sleep(for: .milliseconds(100))
                }
            }
            guard let id = opened["snapshot_id"]?.string, !id.isEmpty else { throw LocalChatMessageQueryError.unavailable }
            snapshotID = id
            try Task.checkCancellation()
            guard appState.imSessionHostIdentity == expected, IMSessionBindingCaptureRegistry.shared.isCurrent(task) else { throw CancellationError() }
            var request = try self.request("history_opened")
            request.fields["task"] = .object(task.fields); request.set("snapshot_id", id)
            let status = try await AIAssistantModel.shared.imSessionBindingRequest(request, host: expected)
            try accept(status, expected: expected)
            while true {
                try Task.checkCancellation()
                guard appState.imSessionHostIdentity == expected, IMSessionBindingCaptureRegistry.shared.isCurrent(task) else { throw CancellationError() }
                let page = try await IMSessionHistoryIO.request(["action": .string("read"), "snapshot_id": .string(id)])
                guard page["snapshot_id"]?.string == id, case .bool(let more)? = page["has_more"] else {
                    throw LocalChatMessageQueryError.unavailable
                }
                while true {
                    try Task.checkCancellation()
                    guard appState.imSessionHostIdentity == expected, IMSessionBindingCaptureRegistry.shared.isCurrent(task) else { throw CancellationError() }
                    var chunk = try self.request("history_chunk")
                    chunk.fields["task"] = .object(task.fields); chunk.fields["page"] = .object(page)
                    let receipt = try await AIAssistantModel.shared.imSessionBindingRequest(chunk, host: expected)
                    try accept(receipt, expected: expected)
                    if receipt.pageAccepted == false && receipt.reason == "awaiting_ingress_metadata" {
                        try await Task.sleep(for: .milliseconds(20)); continue
                    }
                    guard receipt.pageAccepted == true else { throw LocalChatMessageQueryError.unavailable }
                    break
                }
                if !more { break }
            }
        } catch {
            if let appState, appState.imSessionHostIdentity == expected,
               IMSessionBindingCaptureRegistry.shared.isCurrent(task) {
                var failure = IMSessionBindingRequest("history_failed", bridgeID: task.string("bridge_id"), scope: task.scope)
                failure.fields["task"] = .object(task.fields)
                failure.set("reason", String(error.localizedDescription.prefix(256)))
                if let result = try? await AIAssistantModel.shared.imSessionBindingRequest(failure, host: expected) {
                    try? accept(result, expected: expected)
                }
                operationFailure = true
                appState.imSessionBindingError = "本地聊天历史导入失败，请重试"
            }
        }
        if let snapshotID {
            // Cancellation must still close its owned reader; cleanup uses the existing prep queue and no borrowed context.
            _ = try? await IMSessionHistoryIO.request(["action": .string("close"), "snapshot_id": .string(snapshotID)])
        }
    }

    private func startPolling() {
        guard poll == nil, projection?.hasBindings == true || projection?.historyCleanupPending == true else { return }
        poll = Task { @MainActor [weak self] in
            while !Task.isCancelled {
                guard let self, let appState = self.appState, let expected = self.identity,
                      appState.imSessionHostIdentity == expected else { break }
                do {
                    let value = try self.request("pump")
                    let result = try await AIAssistantModel.shared.imSessionBindingRequest(value, host: expected)
                    try self.accept(result, expected: expected)
                    for delivery in result.deliveries where self.inFlight.insert(delivery.identity).inserted {
                        // 只追踪 SDK callback 生命周期；内核仍是唯一 FIFO 与 dispatch authority。
                        Task { @MainActor [weak self, weak appState] in
                            let permit = IMSessionDispatchPermit(delivery: delivery)
                            defer { permit.acknowledge(); self?.inFlight.remove(delivery.identity) }
                            await appState?.sendIMSessionDelivery(delivery, permit: permit)
                        }
                    }
                    if !result.hasBindings && result.pendingCount == 0 && !result.historyCleanupPending { break }
                } catch {
                    guard appState.imSessionHostIdentity == expected else { break }
                    appState.imSessionBindingError = error.localizedDescription
                    await self.refresh()
                }
                try? await Task.sleep(for: .milliseconds(400))
            }
            self?.poll = nil
            if self?.changePending == true {
                self?.changePending = false
                await self?.refresh()
            }
        }
    }
}

struct IMSessionBindingPicker: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @Environment(\.dismiss) private var dismiss
    let peerID: String
    @State private var selected = ""
    @State private var selectedWorkspace = ""
    @State private var loading = true
    @State private var saving = false
    @State private var displayedLease: IMSessionBindingLease?
    private let accent = Color(red: 0.51, green: 0.36, blue: 0.88)
    private var preparing: Bool { loading || appState.imSessionBindingIsPreparing }
    private var selectionAvailable: Bool {
        displayedLease != nil && displayedLease == appState.imSessionBindingLease &&
            !selected.isEmpty && appState.imSessionBindingSessions.contains { $0.id == selected && $0.workspaceKey == selectedWorkspace } &&
            !appState.imSessionBindings.contains { $0.sessionID == selected && $0.peerID != peerID }
    }
    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                ScrollView {
                    VStack(alignment: .leading, spacing: 22) {
                        HStack(alignment: .top, spacing: 12) {
                            Image(systemName: "sparkles").foregroundStyle(accent)
                            Text("先导入本机与这位好友已有的聊天记录，再处理新消息并回复。请保持 MaiChat 运行并联网。")
                                .font(AppTypography.detail)
                                .foregroundStyle(RemoteIMStyle.textSecondary)
                        }
                        if preparing {
                            ProgressView("正在读取会话…").tint(RemoteIMStyle.blue)
                                .font(AppTypography.detail)
                        }
                        VStack(alignment: .leading, spacing: 11) {
                            Text("选择会话").font(AppTypography.detail.weight(.semibold))
                                .foregroundStyle(RemoteIMStyle.textPrimary)
                            if appState.imSessionBindingSessions.isEmpty && !preparing {
                                Text("暂无本机会话，请先在创作中心新建对话。")
                                    .font(AppTypography.detail).foregroundStyle(RemoteIMStyle.textSecondary)
                                    .frame(maxWidth: .infinity, alignment: .leading).padding(18)
                                    .background(RemoteIMStyle.panelBackground, in: RoundedRectangle(cornerRadius: 12))
                            } else {
                                VStack(spacing: 0) {
                                    ForEach(appState.imSessionBindingSessions) { session in
                                        let occupied = appState.imSessionBindings.contains {
                                            $0.sessionID == session.id && $0.peerID != peerID
                                        }
                                        Button {
                                            // 重复点选只取消待确认选择，同时清掉旧 workspace pin。
                                            let deselecting = selected == session.id
                                            selected = deselecting ? "" : session.id
                                            selectedWorkspace = deselecting ? "" : session.workspaceKey
                                        } label: {
                                            HStack(spacing: 12) {
                                                VStack(alignment: .leading, spacing: 5) {
                                                    Text(session.displayTitle).font(AppTypography.detail.weight(.medium))
                                                        .foregroundStyle(RemoteIMStyle.textPrimary)
                                                        .lineLimit(2).multilineTextAlignment(.leading)
                                                    if occupied {
                                                        Text("已绑定其他好友").font(AppTypography.metadata)
                                                            .foregroundStyle(RemoteIMStyle.textSecondary)
                                                    } else if session.busy {
                                                        Text("会话正在运行").font(AppTypography.metadata)
                                                            .foregroundStyle(RemoteIMStyle.textSecondary)
                                                    }
                                                }
                                                Spacer(minLength: 8)
                                                ZStack {
                                                    Circle().fill(selected == session.id ? RemoteIMStyle.blue : Color.clear)
                                                    Circle().strokeBorder(selected == session.id ? RemoteIMStyle.blue : RemoteIMStyle.textSecondary, lineWidth: 1.5)
                                                    if selected == session.id {
                                                        Image(systemName: "checkmark").font(AppTypography.symbol(size: 13, weight: .bold))
                                                            .foregroundStyle(Color.white)
                                                    }
                                                }.frame(width: 22, height: 22).id(selected == session.id).accessibilityHidden(true)
                                            }
                                            .frame(maxWidth: .infinity, minHeight: 58, alignment: .leading)
                                            .padding(.horizontal, 14).padding(.vertical, 4)
                                            .background(selected == session.id ? RemoteIMStyle.blueSoft : Color.clear)
                                            .contentShape(Rectangle())
                                        }
                                        .buttonStyle(.plain).disabled(saving || preparing || occupied)
                                        .accessibilityIdentifier("im-session-choice-" + session.id)
                                        .accessibilityAddTraits(selected == session.id ? .isSelected : [])
                                        if session.id != appState.imSessionBindingSessions.last?.id {
                                            Divider().padding(.leading, 14)
                                        }
                                    }
                                }
                                .background(RemoteIMStyle.panelBackground, in: RoundedRectangle(cornerRadius: 12))
                            }
                        }
                    }.padding(20)
                }
                if let error = appState.imSessionBindingError {
                    Text(error).font(AppTypography.detail).foregroundStyle(.red)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(.horizontal, 20).padding(.top, 8)
                        .accessibilityIdentifier("im-session-binding-error")
                }
                Button {
                    saving = true
                    Task {
                        let success = await appState.bindIMSession(peer: peerID, session: selected, displayedLease: displayedLease, expectedWorkspaceKey: selectedWorkspace)
                        saving = false
                        if success { dismiss() }
                    }
                } label: {
                    HStack(spacing: 8) {
                        if saving { ProgressView().tint(.white) }
                        Text(saving ? "正在导入聊天记录…" : "导入历史并绑定")
                    }
                    .font(AppTypography.body.weight(.semibold)).foregroundStyle(.white)
                    .frame(maxWidth: .infinity).frame(height: 48)
                    .background(!preparing && !saving && selectionAvailable ? RemoteIMStyle.blue : RemoteIMStyle.textSecondary,
                                in: RoundedRectangle(cornerRadius: 12))
                }
                .buttonStyle(.plain).disabled(preparing || saving || !selectionAvailable)
                .accessibilityIdentifier("im-session-binding-confirm").padding(20)
            }
            .background(RemoteIMStyle.pageBackground.ignoresSafeArea())
            .appNavigationTitle("绑定会话").navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .topBarLeading) {
                Button("取消") {
                    if saving {
                        Task { await appState.unbindIMSession(peer: peerID); dismiss() }
                    } else { dismiss() }
                }
            } }
            #if DEBUG && targetEnvironment(simulator)
            .toolbar {
                if ProcessInfo.processInfo.arguments.contains("--im-session-picker-refresh-ui-test") {
                    ToolbarItem(placement: .topBarTrailing) {
                        Button("刷新测试标题") {
                            Task {
                                guard let bound = appState.imSessionBinding(for: peerID) else { return }
                                // 走普通会话选择/改名/查询链路，不直接注入显示数组或更改绑定。
                                await AIAssistantModel.shared.selectConversation(bound.sessionID)
                                await AIAssistantModel.shared.action("rename", values: ["title": "已刷新绑定标题"])
                                await appState.refreshIMSessionBindings()
                            }
                        }.accessibilityIdentifier("im-session-picker-refresh-fixture")
                    }
                }
            }
            #endif
            .interactiveDismissDisabled(saving)
            .tint(RemoteIMStyle.blue)
            .task {
                await appState.refreshIMSessionBindings()
                displayedLease = appState.imSessionBindingLease
                selected = appState.imSessionBinding(for: peerID)?.sessionID ?? ""
                selectedWorkspace = appState.imSessionBindingSessions.first(where: { $0.id == selected })?.workspaceKey ?? ""
                loading = false
            }
            .onChange(of: appState.imSessionBindingLease) { lease in
                if displayedLease != lease { selected = ""; selectedWorkspace = ""; displayedLease = lease }
            }
            .onChange(of: appState.imSessionBindings) { _ in
                if !selectionAvailable {
                    // 已有绑定只在首次打开时默认选中；后续投影不能撤销用户明确的反选。
                    selected = ""; selectedWorkspace = ""
                }
            }
            .onChange(of: appState.imSessionBindingSessions) { _ in
                if !selectionAvailable { selected = ""; selectedWorkspace = "" }
            }
        }
    }
}
