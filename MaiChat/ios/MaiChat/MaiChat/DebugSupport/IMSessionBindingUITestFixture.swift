// 隔离模拟器：普通 Native router + AppState 持久化 + 自有 SDK 边界/本机脚本模型。
// 不登录、不读取真实聊天库、不发送网络 IM；只验证宿主适配和明确的回执边界。
#if DEBUG && targetEnvironment(simulator)
import Foundation
import MaiChatCore
import SwiftUI
#if canImport(ImSDK_Plus)
import ImSDK_Plus

private final class OwnedMetadataSDKProbe: @unchecked Sendable {
    private let lock = NSLock()
    private var committed = false
    private var downloads = 0
    private var early = false
    func commit() { lock.lock(); committed = true; lock.unlock() }
    func downloading() { lock.lock(); downloads += 1; early = early || !committed; lock.unlock() }
    func result() -> (Int, Bool) { lock.lock(); defer { lock.unlock() }; return (downloads, early) }
}
private final class OwnedMetadataFileElement: V2TIMFileElem, @unchecked Sendable {
    let probe: OwnedMetadataSDKProbe
    init(probe: OwnedMetadataSDKProbe) { self.probe = probe; super.init() }
    override var uuid: String? { "owned-media-resource" }
    override var filename: String? { "owned-unavailable.txt" }
    override var fileSize: Int32 { 42 }
    override func getUrl(_ urlCallBack: @escaping (String?) -> Void) {
        probe.downloading()
        urlCallBack(nil) // 正常失败下载路径；不访问公网或真实 SDK login。
    }
}
private final class OwnedMetadataSDKMessage: V2TIMMessage, @unchecked Sendable {
    let element: OwnedMetadataFileElement
    init(probe: OwnedMetadataSDKProbe) { element = OwnedMetadataFileElement(probe: probe); super.init() }
    override var msgID: String? { "owned-sdk-raw-metadata" }
    override var sender: String? { "sim-friend-a" }
    override var userID: String? { "sim-friend-a" }
    override var timestamp: Date? { Date(timeIntervalSince1970: 1234.567) }
    override var isSelf: Bool { false }
    override var elemType: V2TIMElemType { V2TIMElemType(rawValue: 6)! }
    override var fileElem: V2TIMFileElem? { element }
}
#endif

@MainActor
private final class SessionBindingTestIMClient: RemoteIMClient {
    var onIncomingMetadata: ((IncomingRemoteIMMetadata) async -> RemoteIMMetadataReceipt?)?
    let persistenceFence = IMIngressPersistenceFence()
    let metadataGeneration = UUID()
    var incomingPersistenceFence: IMIngressPersistenceFence? { persistenceFence }
    init() {
        persistenceFence.activate(accountTag: RemoteDiagnosticsProtocol.accountTag(sdkAppID: 1,
            ownerUserID: GroupChatUITestData.ownerID), generation: metadataGeneration)
    }
    var onSessionIMInput: ((IMSessionIncomingPayload) -> Void)?
    var onIncomingText: ((IncomingRemoteIMText) -> Void)?
    var onIncomingActivity: ((IncomingRemoteIMActivity) -> Void)?
    var onIncomingVoice: ((IncomingRemoteIMVoice) -> Void)?
    var onIncomingImage: ((IncomingRemoteIMImage) -> Void)?
    var onIncomingFile: ((IncomingRemoteIMFile) -> Void)?
    var onIncomingVideo: ((IncomingRemoteIMVideo) -> Void)?
    var onPresenceStatusChanged: (([String: RemoteIMPresenceStatus]) -> Void)?
    var dispatched: [String] = []
    var holdNextReceipt = false
    var beforeClaim: (() -> Void)?
    var heldReceipt: CheckedContinuation<RemoteIMSendReceipt, Error>?
    var rejectedClaims = 0
    func sendSessionReply(_ payload: IMSessionOutgoingPayload, to userID: String,
                          permit: IMSessionDispatchPermit) async throws -> RemoteIMSendReceipt {
        beforeClaim?(); beforeClaim = nil
        guard permit.claim(accountTag: RemoteDiagnosticsProtocol.accountTag(
            sdkAppID: 1, ownerUserID: GroupChatUITestData.ownerID)) else {
            rejectedClaims += 1; permit.acknowledge(); throw CancellationError()
        }
        switch payload {
        case .text(let text): dispatched.append(text)
        case .image: dispatched.append("image")
        case .video: dispatched.append("video")
        case .file: dispatched.append("file")
        }
        // 模拟实际 SDK 调用返回即 ack；可单独延迟网络回执，不能让其挡住下一次派发。
        permit.acknowledge()
        if holdNextReceipt {
            holdNextReceipt = false
            return try await withCheckedThrowingContinuation { heldReceipt = $0 }
        }
        return RemoteIMSendReceipt(remoteID: "owned-sdk-" + UUID().uuidString, createdAt: Date())
    }
    func connect(sdkAppID: Int, userID: String, userSig: String) async throws {}
    func disconnect() async {}
    func sendText(to userID: String, text: String, origin: RemoteIMMessageOrigin,
                  quote: RemoteIMQuote?) async throws -> RemoteIMSendReceipt { throw CancellationError() }
    func sendActivity(to userID: String, signal: RemoteIMActivitySignal) async throws {}
    func sendApprovalDecision(to userID: String, token: String, action: RemoteIMApprovalAction) async throws
        -> RemoteIMSendReceipt { throw CancellationError() }
    func sendVoice(to userID: String, recording: RemoteIMVoiceRecording, origin: RemoteIMMessageOrigin) async throws
        -> RemoteIMSendReceipt { throw CancellationError() }
    func sendImage(to userID: String, image: RemoteIMImageFile, caption: String?, captionAbove: Bool,
                   origin: RemoteIMMessageOrigin) async throws -> RemoteIMSendReceipt { throw CancellationError() }
    func sendVideo(to userID: String, video: RemoteIMVideoFile, caption: String?, captionAbove: Bool,
                   origin: RemoteIMMessageOrigin) async throws -> RemoteIMSendReceipt { throw CancellationError() }
    func sendFile(to userID: String, file: RemoteIMFile, caption: String?, captionAbove: Bool,
                  origin: RemoteIMMessageOrigin) async throws -> RemoteIMSendReceipt { throw CancellationError() }
    func deleteContact(userID: String) async throws {}
    func clearHistory(userID: String) async throws {}
    func refreshUserProfiles(userIDs: [String]) async throws -> [RemoteIMUserProfile] { [] }
    func refreshPresenceStatuses(userIDs: [String]) async throws -> [String: RemoteIMPresenceStatus] { [:] }
    func subscribePresenceStatuses(userIDs: [String]) async throws {}
    func updateApplicationBadgeCount(_ count: Int) {}
}

struct IMSessionBindingUITestRoot: View {
    @StateObject private var state: RemoteIMAppState
    private let client: SessionBindingTestIMClient
    private let historyStore: LocalChatHistoryStore
    @State private var started = false
    @State private var status = "准备 Session IM 宿主测试"
    init() {
        let client = SessionBindingTestIMClient()
        let defaults = UserDefaults(suiteName: "owned-session-binding-" + UUID().uuidString)!
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("owned-session-im-db-" + UUID().uuidString)
        let historyStore = LocalChatHistoryStore(baseDirectoryURL: directory)
        _state = StateObject(wrappedValue: RemoteIMAppState(settingsStore: LocalSettingsStore(defaults: defaults),
            historyStore: historyStore, client: client, loadedSettings: .empty))
        self.client = client
        self.historyStore = historyStore
    }
    var body: some View {
        RootView().environmentObject(state)
            .overlay(alignment: .bottom) {
                Text(status).font(.caption).padding(10).background(.regularMaterial)
                    .accessibilityIdentifier("im-session-host-status").padding(.bottom, 100)
            }
            .task {
                guard !started else { return }; started = true
                if ProcessInfo.processInfo.arguments.contains("--im-session-maintenance-warning-ui-test") {
                    runMaintenanceWarningContract()
                } else if ProcessInfo.processInfo.arguments.contains("--im-raw-metadata-ui-test") {
                    await runRawSDKMetadata()
                } else if ProcessInfo.processInfo.arguments.contains("--im-session-binding-open-ui-test") {
                    await runPreparationRecovery()
                } else if ProcessInfo.processInfo.arguments.contains("--im-session-history-ui-test") {
                    await runHistoryBootstrap()
                } else { await run() }
            }
    }
    @MainActor private func check(_ value: Bool, _ message: String) throws {
        if !value { throw AIBackendError(message: message) }
    }
    @MainActor private func wait(_ condition: () -> Bool, _ message: String) async throws {
        let deadline = Date().addingTimeInterval(15)
        while !condition() && Date() < deadline { try await Task.sleep(for: .milliseconds(25)) }
        try check(condition(), message + " / " + (state.imSessionBindingError ?? ""))
    }
    private struct OwnedRaw {
        let metadata: IncomingRemoteIMMetadata
        let live: IMSessionIngressTicket?
    }
    @MainActor private func beginRaw(_ id: String, text: String, origin: RemoteIMMessageOrigin = .human,
                                     group: Bool = false, control: Bool = false) -> OwnedRaw? {
        guard !group && !control else { return nil }
        let account = RemoteDiagnosticsProtocol.accountTag(sdkAppID: 1, ownerUserID: GroupChatUITestData.ownerID)
        let live = IMSessionBindingCaptureRegistry.shared.admit(accountTag: account, peerID: "sim-friend-a", messageID: id,
            origin: origin, recipientID: GroupChatUITestData.ownerID, group: group, controlFrame: control, hasContent: true)
        guard let fence = client.persistenceFence.begin(accountTag: account,
            generation: client.metadataGeneration, peerID: "sim-friend-a", sourceID: "sdk:" + id) else { return nil }
        let message = RemoteIMMessage(remoteID: id, fromUserID: "sim-friend-a", toUserID: GroupChatUITestData.ownerID,
            text: text, direction: .incoming, status: .received, createdAt: Date())
        return OwnedRaw(metadata: IncomingRemoteIMMetadata(accountTag: account, fenceTicket: fence, message: message), live: live)
    }
    @MainActor private func persistRaw(_ raw: OwnedRaw) async -> IMSessionIngressTicket? {
        let receipt = await client.onIncomingMetadata?(raw.metadata)
        var valid = receipt?.persisted == true && receipt?.wasInserted == true
        if let live = raw.live, let receipt, receipt.persisted {
            let confirmed = IMSessionBindingControl.persistedInput(live, receipt: receipt, remoteID: raw.metadata.message.remoteID)
            valid = valid && confirmed
        } else if raw.live != nil { IMSessionBindingControl.abandon(raw.live, reason: "persistence_failed") }
        client.persistenceFence.finish(raw.metadata.fenceTicket, persisted: receipt?.persisted == true)
        return valid ? raw.live : nil
    }
    @MainActor private func ticket(_ id: String, origin: RemoteIMMessageOrigin = .human,
                                   group: Bool = false, control: Bool = false) async -> IMSessionIngressTicket? {
        guard let raw = beginRaw(id, text: "metadata:" + id, origin: origin, group: group, control: control) else { return nil }
        return await persistRaw(raw)
    }
    @MainActor private func ready(_ ticket: IMSessionIngressTicket, _ text: String, file: URL? = nil) {
        client.onSessionIMInput?(IMSessionIncomingPayload(ticket: ticket, text: text,
            kind: file == nil ? "text" : "file", fileURL: file, quoteText: nil))
    }
    /// 同一个普通 backend 的真实并发 open；不替换模型/ABI，不读取现场数据。
    @MainActor private func runPreparationRecovery() async {
        var failures: [String] = []
        let backend = AIAssistantBackend()
        let errors = await withTaskGroup(of: String?.self, returning: [String].self) { group in
            for _ in 0..<20 {
                group.addTask {
                    do { _ = try await backend.open(accountUserID: GroupChatUITestData.ownerID); return nil }
                    catch { return error.localizedDescription }
                }
            }
            var errors: [String] = []
            for await error in group { if let error { errors.append(error) } }
            return errors
        }
        if !errors.isEmpty { failures.append("concurrentOpenFailures=\(errors.count):" + errors[0]) }
        state.configureGroupUITestFixture()
        await state.refreshIMSessionBindings()
        state.imSessionBindingError = "模型配置正在准备，请稍后重试"
        await state.refreshIMSessionBindings()
        if !state.imSessionBindingSessions.isEmpty && state.imSessionBindingError != nil {
            failures.append("loaded list retained stale preparation error")
        }
        let previousCount = state.imSessionBindingSessions.count
        await AIAssistantModel.shared.action("create")
        await state.refreshIMSessionBindings()
        do { try await wait({ state.imSessionBindingSessions.count > previousCount }, "会话列表未反映新建会话") }
        catch { failures.append(error.localizedDescription) }
        status = failures.isEmpty ? "PASS Session IM preparation: shared open and recovered notice" :
            "FAIL Session IM preparation: " + failures.joined(separator: " / ")
    }
    @MainActor private func runMaintenanceWarningContract() {
        do {
            let controller = IMSessionBindingController(appState: state)
            let maintenance: [String: IMSessionJSON] = ["code": .string("history_cleanup_failed"),
                                                       "outcome": .string("retry_local_cleanup")]
            let live: [String: IMSessionJSON] = ["code": .string("history_input_failed"),
                                                "outcome": .string("not_submitted")]
            state.imSessionBindingError = nil
            controller.acceptWarningsForUITesting([maintenance], cleanupPending: true)
            try check(state.imSessionBindingError == nil, "后台维护被误报成消息未提交")
            controller.acceptWarningsForUITesting([], cleanupPending: false)
            try check(state.imSessionBindingError == nil, "维护完成后残留红字")
            controller.acceptWarningsForUITesting([maintenance, live], cleanupPending: true)
            let realError = state.imSessionBindingError
            try check(realError?.contains("有消息尚未保存或准备好") == true, "同一投影的真实live错误被维护提示覆盖")
            controller.acceptWarningsForUITesting([], cleanupPending: false)
            try check(state.imSessionBindingError == realError, "维护恢复错误清除了真实live提示")
            status = "PASS Session IM maintenance warning: separate from live errors"
        } catch { status = "FAIL Session IM maintenance warning: " + error.localizedDescription }
    }
    @MainActor private func runRawSDKMetadata() async {
        #if canImport(ImSDK_Plus)
        do {
            let sdk = TencentIMClient()
            try sdk.configureRawIngressIdentityForUITest(sdkAppID: 1, owner: GroupChatUITestData.ownerID)
            let directory = FileManager.default.temporaryDirectory.appendingPathComponent("owned-raw-sdk-" + UUID().uuidString)
            let store = LocalChatHistoryStore(baseDirectoryURL: directory)
            let defaults = UserDefaults(suiteName: "owned-raw-sdk-" + UUID().uuidString)!
            let owner = RemoteIMAppState(settingsStore: LocalSettingsStore(defaults: defaults), historyStore: store,
                client: sdk, loadedSettings: .empty)
            owner.configureGroupUITestFixture()
            let sink = sdk.onIncomingMetadata
            let probe = OwnedMetadataSDKProbe()
            var inserts: [Bool] = []
            sdk.onIncomingMetadata = { metadata in
                let receipt = await sink?(metadata)
                if let receipt { inserts.append(receipt.wasInserted) }
                if receipt?.persisted == true { probe.commit() }
                return receipt
            }
            let message = OwnedMetadataSDKMessage(probe: probe)
            sdk.onRecvNewMessage(msg: message)
            try await wait({ probe.result().0 == 1 }, "真实SDK raw→写库→下载边界未执行")
            let page = try await store.makePersistence().queryMessages(sdkAppID: 1, ownerUserID: GroupChatUITestData.ownerID,
                generation: 1, request: LocalChatMessageQuery(filters: LocalChatMessageFilters(peerID: "sim-friend-a")))
            try check(page.hits.count == 1 && page.hits[0].kind == "file", "失败媒体没有保留普通数据库行")
            try check(!probe.result().1 && inserts == [true], "下载早于真正元数据commit")
            sdk.onRecvNewMessage(msg: message)
            try await wait({ inserts.count == 2 }, "原SDK重放没有走数据库去重")
            try check(inserts == [true, false] && probe.result().0 == 1, "旧行被当作新消息或重新下载")
            status = "PASS raw SDK metadata: actual DB first, unavailable media retained, replay classified"
        } catch { status = "FAIL raw SDK metadata: " + error.localizedDescription }
        #else
        status = "FAIL raw SDK metadata: normal SDK unavailable"
        #endif
    }

    @MainActor private func modelStatus() async throws -> (Int, String) {
        let port = ProcessInfo.processInfo.environment["MAICHAT_LOCAL_UI_MODEL_PORT"] ?? "18667"
        let (data, response) = try await URLSession.shared.data(from: URL(string: "http://127.0.0.1:\(port)/test/status")!)
        guard (response as? HTTPURLResponse)?.statusCode == 200,
              let object = try JSONSerialization.jsonObject(with: data) as? [String: Any],
              let count = object["count"] as? Int else { throw AIBackendError(message: "本机脚本模型未就绪") }
        return (count, String(decoding: data, as: UTF8.self))
    }
    @MainActor private func seedHistory() async throws {
        let rows = (0..<205).map { index in
            let incoming = index % 2 == 0
            return RemoteIMMessage(id: UUID(uuidString: String(format: "50000000-0000-0000-0000-%012d", index))!,
                remoteID: "owned-history-sdk-" + String(index),
                fromUserID: incoming ? "sim-friend-a" : GroupChatUITestData.ownerID,
                toUserID: incoming ? GroupChatUITestData.ownerID : "sim-friend-a",
                text: String(format: "owned-history-%03d", index), direction: incoming ? .incoming : .outgoing,
                status: incoming ? .received : .sent,
                createdAt: Date(timeIntervalSince1970: 1000 + Double(index / 5) / 1000))
        }
        _ = try await historyStore.makePersistence().persist(mutations: [.upsert(rows, sdkAppID: 1,
            ownerUserID: GroupChatUITestData.ownerID)])
    }
    @MainActor private func runHistoryBootstrap() async {
        do {
            state.configureGroupUITestFixture()
            await state.refreshIMSessionBindings()
            guard let session = state.imSessionBindingSessions.first?.id else { throw AIBackendError(message: "没有本机Session") }
            try await seedHistory()
            let before = try await modelStatus().0
            // Raw 已到但还没有写入：绑定必须等这个 metadata fence，不等其网络下载。
            guard let prebind = beginRaw("raw-before-bind", text: "owned-prebind-metadata") else {
                throw AIBackendError(message: "原 SDK metadata fence 未创建")
            }
            try check(prebind.live == nil, "未绑定的旧事件错误取得 live ticket")
            let binding = Task { @MainActor in await state.bindIMSession(peer: "sim-friend-a", session: session) }
            try await wait({ state.imSessionBinding(for: "sim-friend-a")?.historyState == "pending" }, "未发布provisional epoch")
            // 已存在于实际数据库的 SDK 重放只去 live 槽，不能让整次历史导入失败或漏掉该历史行。
            guard let replay = beginRaw("owned-history-sdk-0", text: "must-not-overwrite-history") else {
                throw AIBackendError(message: "重放资格样本未创建")
            }
            try check(await persistRaw(replay) == nil, "数据库旧消息重放取得new-live资格")
            // 本条同时进入 pinned DB 和原 live 槽；历史只排除已确认 new 的这一条。
            guard let live = beginRaw("owned-live-during-import", text: "owned-live-question"), let liveTicket = live.live else {
                throw AIBackendError(message: "导入期间未按原 epoch admit")
            }
            let confirmed = await persistRaw(live)
            try check(confirmed != nil, "实际数据库首次插入回执未被接受")
            _ = await persistRaw(prebind)
            try check(await binding.value, "历史导入未完成 / " + (state.imSessionBindingError ?? ""))
            try check(state.imSessionBinding(for: "sim-friend-a")?.importedCount?.string == "206", "历史页计数/重叠去重错误")
            let afterImport = try await modelStatus().0
            try check(afterImport == before && client.dispatched.isEmpty, "历史导入执行了模型或回放回复")
            ready(liveTicket, "owned-live-question")
            try await wait({ client.dispatched.count == 1 }, "历史ready后新live未接续")
            let afterLive = try await modelStatus()
            try check(afterLive.0 == before + 1, "一条后续live产生额外模型请求")
            try check(afterLive.1.contains("owned-history-000") && afterLive.1.contains("owned-history-204") && afterLive.1.contains("owned-history-001") &&
                      afterLive.1.contains("owned-prebind-metadata"), "首条live没有读取已完成的历史上下文")
            try check(client.dispatched[0].contains("owned-live-question"), "exact assistant回发归属错误")
            await state.unbindIMSession(peer: "sim-friend-a")
            guard let held = beginRaw("owned-overflow-fence", text: "held raw metadata") else { throw CancellationError() }
            let overflowBinding = Task { @MainActor in await state.bindIMSession(peer: "sim-friend-a", session: session) }
            try await wait({ state.imSessionBinding(for: "sim-friend-a")?.historyState == "pending" }, "溢出样本未建立屏障")
            var admitted = 0
            for index in 0..<33 {
                guard let raw = beginRaw("owned-overflow-" + String(index), text: "owned overflow metadata") else {
                    throw AIBackendError(message: "raw元数据丢失")
                }
                if raw.live != nil { admitted += 1 }
                _ = await persistRaw(raw)
            }
            _ = await persistRaw(held)
            try check(admitted == 32, "live pending上限未保持32")
            try check(!(await overflowBinding.value), "导入期间丢弃第33条却报告ready")
            try check(state.imSessionBinding(for: "sim-friend-a") == nil, "失败导入保留了ready绑定")
            try check(try await modelStatus().0 == afterLive.0, "溢出/回滚执行了模型")
            guard let canceledRaw = beginRaw("owned-cancel-fence", text: "owned canceled metadata") else { throw CancellationError() }
            let cancelBinding = Task { @MainActor in await state.bindIMSession(peer: "sim-friend-a", session: session) }
            try await wait({ state.imSessionBinding(for: "sim-friend-a")?.historyState == "pending" }, "取消样本未建立屏障")
            await state.unbindIMSession(peer: "sim-friend-a")
            _ = await persistRaw(canceledRaw)
            try check(!(await cancelBinding.value) && state.imSessionBinding(for: "sim-friend-a") == nil,
                      "取消的导入被旧completion标为ready")
            try check(try await modelStatus().0 == afterLive.0, "取消/回滚调用了模型")
            status = "PASS history bootstrap: 206 records, replay/fence, overlap, no replay, live, overflow32/cancel"
        } catch { status = "FAIL history bootstrap: " + error.localizedDescription }
    }

    @MainActor private func checkProjectionFences() throws {
        try check(!IMSessionBindingControl.accepts(action: "claim", response: ["ok": .bool(true)]), "缺失 allowed 被放行")
        try check(!IMSessionBindingControl.accepts(action: "claim", response: ["ok": .bool(true), "allowed": .bool(false)]), "拒绝 claim 被放行")
        try check(IMSessionBindingControl.accepts(action: "claim", response: ["ok": .bool(true), "allowed": .bool(true)]), "明确允许 claim 被拒")
        let registry = IMSessionBindingCaptureRegistry()
        let host = IMSessionBindingHostIdentity(sdkAppID: "1", ownerID: "owned-projection", generation: "1", epoch: 1)
        let old = IMSessionBindingScope(sdkAppID: "1", ownerID: "owned-projection", authGeneration: "1", controllerGeneration: "4")
        let new = IMSessionBindingScope(sdkAppID: "1", ownerID: "owned-projection", authGeneration: "1", controllerGeneration: "5")
        registry.authorizeHost(host); registry.setAcceptedFriends(["peer"])
        let binding = IMSessionBindingItem(peerID: "peer", sessionID: "session", sessionTitle: "owned", epoch: "1")
        guard let revision = registry.publish(bridgeID: "owned-new", scope: new, bindings: [binding], hostIdentity: host)
        else { throw AIBackendError(message: "新 projection 被拒") }
        try check(registry.publish(bridgeID: "owned-old", scope: old, bindings: [binding], hostIdentity: host) == nil, "倒退 controller 代际被接受")
        registry.clear(bridgeID: "owned-old", scope: old)
        try check(registry.isPublished(revision, bridgeID: "owned-new"), "旧 clear 抹掉新 bridge")
        let newerHost = IMSessionBindingHostIdentity(sdkAppID: "1", ownerID: "owned-projection", generation: "1", epoch: 2)
        registry.authorizeHost(newerHost)
        try check(registry.publish(bridgeID: "owned-new", scope: new, bindings: [binding], hostIdentity: host) == nil,
                  "旧登录 host lease 覆盖了新投影")
    }
    @MainActor private func run() async {
        do {
            try checkProjectionFences()
            state.configureGroupUITestFixture()
            await state.refreshIMSessionBindings()
            guard let session = state.imSessionBindingSessions.first?.id else {
                throw AIBackendError(message: "普通 Native root Session 缺失 / " + (state.imSessionBindingError ?? ""))
            }
            try check(await state.bindIMSession(peer: "sim-friend-a", session: session), "普通绑定失败")
            // 冷启动：可信好友列表尚未返回时，旧偏好不能被空列表覆盖或提前宣告恢复完成。
            state.configureGroupUITestFixture()
            state.setSessionBindingFriendLoadForUITest(loaded: false, friends: [])
            await state.refreshIMSessionBindings()
            try check(state.imSessionBindings.isEmpty, "好友未加载即恢复绑定")
            state.setSessionBindingFriendLoadForUITest(loaded: true, friends: ["sim-friend-a", "sim-friend-b"])
            await state.refreshIMSessionBindings()
            try await wait({ state.imSessionBinding(for: "sim-friend-a")?.sessionID == session }, "异步好友返回后未恢复")
            status = "宿主范围/冷启动恢复通过"
            try check(await ticket("machine", origin: .machine) == nil, "Machine 回环被准入")
            try check(await ticket("group", group: true) == nil, "群消息被准入")
            try check(await ticket("control", control: true) == nil, "控制消息被准入")
            guard let first = await ticket("live-media-A"), let second = await ticket("live-text-B") else {
                throw AIBackendError(message: "原 live admit 未返回 owned input_id")
            }
            try check(await ticket("live-media-A") == nil, "原 SDK ID 未去重")
            let source = FileManager.default.temporaryDirectory.appendingPathComponent("owned-bound-input.txt")
            try Data("owned media reference".utf8).write(to: source)
            ready(second, "second-text-B")
            try await Task.sleep(for: .milliseconds(600))
            try check(client.dispatched.isEmpty, "晚文字越过未 ready 的媒体")
            client.holdNextReceipt = true
            ready(first, "first-media-A", file: source)
            try await wait({ client.dispatched.count >= 2 }, "原 FIFO/网络回执分离未前进")
            try check(client.dispatched[0].contains("first-media-A") && client.dispatched[1].contains("second-text-B"),
                      "回复顺序或 exact receipt 归属错误")
            try check(client.heldReceipt != nil, "第一条回执没有被独立保留")
            client.heldReceipt?.resume(returning: RemoteIMSendReceipt(remoteID: nil, createdAt: nil))
            client.heldReceipt = nil
            try await Task.sleep(for: .milliseconds(100))
            try check(state.locallyQueuedMessageID == nil, "Machine 写入触发了 Human 接管标记")
            let outgoing = state.chatState.messages.filter { $0.direction == .outgoing }
            try check(outgoing.count == 2 && outgoing[0].status == .failed && outgoing[1].status == .sent,
                      "未知网络回执被误记为精确送达")
            status = "原 live FIFO/派发 ack/真实回执边界通过"
            guard let late = await ticket("old-binding-late") else { throw AIBackendError(message: "迟到输入准入失败") }
            await state.unbindIMSession(peer: "sim-friend-a")
            try check(await state.bindIMSession(peer: "sim-friend-a", session: session), "重新绑定失败")
            ready(late, "must-not-retarget")
            try await Task.sleep(for: .milliseconds(600))
            try check(client.dispatched.count == 2, "旧 ticket 改投了新 epoch")
            client.beforeClaim = { IMSessionBindingCaptureRegistry.shared.revoke("takeover", sessionID: session) }
            guard let canceled = await ticket("local-takeover") else { throw AIBackendError(message: "接管样本准入失败") }
            ready(canceled, "must-not-dispatch")
            try await wait({ client.rejectedClaims == 1 }, "SDK 最后边界未拒绝接管后的回复")
            try check(client.dispatched.count == 2, "被撤销的回复仍被发送")
            try check(state.errorMessage?.contains("CancellationError") != true, "预期撤销泄露技术错误提示")
            // takeover 与 configure 间没有等待 Main observer；worker 屏障必须先保存空权威关系。
            try check(await state.bindIMSession(peer: "sim-friend-a", session: session), "重配样本绑定失败")
            IMSessionBindingCaptureRegistry.shared.revoke("takeover", sessionID: session)
            try await AIAssistantModel.shared.reconfigureIMSessionBackendForUITest()
            await state.refreshIMSessionBindings()
            try check(state.imSessionBinding(for: "sim-friend-a") == nil, "立即 takeover→重配复活关系")
            try check(await state.bindIMSession(peer: "sim-friend-a", session: session), "Native 解绑样本绑定失败")
            try await AIAssistantModel.shared.reconfigureIMSessionBackendForUITest(unbindPeer: "sim-friend-a")
            await state.refreshIMSessionBindings()
            try check(state.imSessionBinding(for: "sim-friend-a") == nil, "无 observer 等待的 Native 解绑→重配复活关系")
            status = "PASS Session IM host: FIFO/ack/epoch/takeover/friends/preferences"
        } catch {
            status = "FAIL Session IM host: " + error.localizedDescription
        }
    }
}
#endif
