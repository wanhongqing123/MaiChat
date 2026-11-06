// 文件作用：持久保存创作中心尚未确认送达的消息；只管理本地 FIFO 与确认状态，不发模型请求。
import Foundation

/// conversationID 是宿主提供的稳定本地会话、空草稿或远端 owner/会话标识，不作为文件路径。
/// workspaceID 使用逻辑标识，不能依赖恢复后可能改变的沙盒绝对路径。内容不包含账号凭证。
public struct AIOutgoingMessageQueueOwner: Codable, Hashable, Sendable {
    public let accountID: String
    public let workspaceID: String
    public let conversationID: String

    public init(accountID: String, workspaceID: String, conversationID: String) {
        self.accountID = accountID
        self.workspaceID = workspaceID
        self.conversationID = conversationID
    }
}

public struct AIOutgoingMessageQueueBinding: Codable, Equatable, Sendable {
    public enum Mode: String, Codable, Sendable { case send, steer, enqueue, answer }
    public let mode: Mode
    public let turnID: String
    /// 回答只能投递到点击时捕获的问题；取消或超时后不得退化成新的普通消息。
    public let questionID: String?
    /// 每次创建 native Agent 都换一个 epoch；不能把进程重启后的 Agent 当成旧收据拥有者。
    public let hostEpoch: String

    public init(mode: Mode, turnID: String = "", questionID: String? = nil, hostEpoch: String) {
        self.mode = mode
        self.turnID = turnID
        self.questionID = questionID
        self.hostEpoch = hostEpoch
    }
}

public enum AIOutgoingMessageQueueState: String, Codable, Sendable {
    case queued, dispatching, forwarded, unknown, accepted, failed
}

public struct AIOutgoingMessageQueueEntry: Codable, Equatable, Identifiable, Sendable {
    public let id: UUID
    public fileprivate(set) var owner: AIOutgoingMessageQueueOwner
    public let sequence: UInt64
    public let createdAt: Date
    /// 宿主的 Codable DTO；队列不解释文本、媒体路径或引用，也不把它写入诊断日志。
    public fileprivate(set) var payloadData: Data
    public fileprivate(set) var state: AIOutgoingMessageQueueState
    public fileprivate(set) var revision: UInt64
    public fileprivate(set) var binding: AIOutgoingMessageQueueBinding?
    public fileprivate(set) var acceptedMessageID: String?
    /// 曾经无法确定是否接受的条目永不自动清除原绑定，即使重试随后返回 NotFound。
    public fileprivate(set) var everUnknown: Bool
    public var requestID: String { id.uuidString.lowercased() }
}

public enum AIOutgoingMessageQueueChange: Sendable {
    /// 附件上传产生的稳定引用在首次派发前写入；未知或已派发的 payload 永不改变。
    case updatePayload(Data)
    case dispatch(AIOutgoingMessageQueueBinding)
    /// 转发服务已收到，所属端的持久接收回执仍可稍后到达；不阻挡下一条远端输入。
    case forwarded
    case accepted(messageID: String)
    case unknown
    case failed
    /// 只供明确的未接受响应使用，例如 Busy/NotFound；网络失败或缺失收据不能走这里。
    case requeueRejected
    /// enqueue 可在新 epoch 下复用持久请求编号重试；旧 unknown 仅能按同 epoch 的 steer 原绑定重试。
    case retry
}

public enum AIOutgoingMessageQueueError: Error, Equatable, Sendable {
    case invalidInput, ownerMismatch, duplicateID, capacityExceeded
    case missingEntry, staleRevision, invalidTransition, incompatibleStore, corruptStore
    case permanentlyInvalidated
}

/// 每个账号工作区 URL 只创建一个实例，由同一个宿主泵负责调用现有 send/steer 和所属端 enqueue。
/// 初始化不做 I/O；异步 actor 方法隔离文件操作，不在 MainActor 同步读写。
/// enqueue/transition 先原子写盘再发布内存结果，失败时调用者必须保留草稿和附件持有引用。
/// queued 尚未选定投递方式；dispatch 必须先落盘，然后宿主才能调用 native。
/// enqueue 的 forwarded/unknown/failed 不挡住后续消息；旧 send/steer 仍保留原绑定保护。
/// load 首次恢复时把 dispatching 保存为 unknown，绝不自动重放普通 send。
/// accepted 仅表示 native 已返回落库消息 ID；宿主合并历史后才能 removeAccepted 并释放媒体。
/// 本类不承诺跨 native Agent 生命周期的 exactly-once；普通 send 本身没有幂等请求 ID。
public actor AIOutgoingMessageQueue {
    private struct Document: Codable {
        var version = 1
        let accountID: String
        let workspaceID: String
        var nextSequence: UInt64 = 1
        var entries: [AIOutgoingMessageQueueEntry] = []
    }

    private let fileURL: URL
    private let accountID: String
    private let workspaceID: String
    private var hostEpoch: String
    private let maximumEntries: Int
    private let maximumPayloadBytes: Int
    private let maximumStoreBytes: Int
    private var document: Document
    private var loaded = false
    private var permanentlyInvalidated = false

    public init(
        fileURL: URL, accountID: String, workspaceID: String, hostEpoch: String,
        maximumEntries: Int = 256, maximumPayloadBytes: Int = 1024 * 1024,
        maximumStoreBytes: Int = 16 * 1024 * 1024
    ) throws {
        guard fileURL.isFileURL, Self.validIdentifier(accountID),
            Self.validIdentifier(workspaceID), Self.validIdentifier(hostEpoch),
            maximumEntries > 0, maximumPayloadBytes > 0, maximumStoreBytes > 0
        else {
            throw AIOutgoingMessageQueueError.invalidInput
        }
        self.fileURL = fileURL.standardizedFileURL
        self.accountID = accountID
        self.workspaceID = workspaceID
        self.hostEpoch = hostEpoch
        self.maximumEntries = maximumEntries
        self.maximumPayloadBytes = maximumPayloadBytes
        self.maximumStoreBytes = maximumStoreBytes
        document = Document(accountID: accountID, workspaceID: workspaceID)
    }

    /// 首次载入和崩溃恢复；后续调用只取当前快照，不覆盖仍在进行的宿主调用。
    @discardableResult
    public func load() throws -> [AIOutgoingMessageQueueEntry] {
        try ensureLoaded()
        return ordered(document.entries)
    }

    /// 仅在永久删除账号时调用；宿主 await 本方法后再删除目录，晚到收据不得重建账号数据。
    /// actor 屏障等待此前文件操作完成，本方法不删除文件；普通退出登录或换号不得调用。
    /// 同一个实例无法重新启用，之后的读取、写入与 epoch 配置均报告 permanentlyInvalidated。
    public func invalidatePermanently() {
        permanentlyInvalidated = true
    }

    /// 每次用户点击生成新的 UUID；相同文字不合并。同 ID 同 owner/payload 的重复入队幂等。
    /// 显式 removeAccepted 后不再保留该 ID 的幂等记录，宿主不能重新提交已消费的旧点击。
    public func enqueue(
        owner: AIOutgoingMessageQueueOwner, payloadData: Data,
        id: UUID = UUID(), createdAt: Date = Date()
    ) throws
        -> AIOutgoingMessageQueueEntry
    {
        try ensureLoaded()
        try validateOwner(owner)
        guard !payloadData.isEmpty, payloadData.count <= maximumPayloadBytes,
            createdAt.timeIntervalSinceReferenceDate.isFinite
        else {
            throw AIOutgoingMessageQueueError.invalidInput
        }
        if let existing = document.entries.first(where: { $0.id == id }) {
            guard existing.owner == owner, existing.payloadData == payloadData else {
                throw AIOutgoingMessageQueueError.duplicateID
            }
            return existing
        }
        guard document.entries.count < maximumEntries, document.nextSequence < UInt64.max else {
            throw AIOutgoingMessageQueueError.capacityExceeded
        }
        var candidate = document
        let entry = AIOutgoingMessageQueueEntry(
            id: id, owner: owner, sequence: candidate.nextSequence, createdAt: createdAt,
            payloadData: payloadData, state: .queued, revision: 1, binding: nil,
            acceptedMessageID: nil, everUnknown: false)
        candidate.nextSequence += 1
        candidate.entries.append(entry)
        try commit(candidate)
        return entry
    }

    public func allEntries() throws -> [AIOutgoingMessageQueueEntry] { try load() }

    /// configure 也可能替换 native Agent；只更新之后的派发/重试门禁，不改绑旧条目。
    public func updateHostEpoch(_ value: String) throws {
        try ensureActive()
        guard Self.validIdentifier(value) else { throw AIOutgoingMessageQueueError.invalidInput }
        hostEpoch = value
    }

    public func entries(for owner: AIOutgoingMessageQueueOwner) throws
        -> [AIOutgoingMessageQueueEntry]
    {
        try ensureLoaded()
        try validateOwner(owner)
        return ordered(document.entries.filter { $0.owner == owner })
    }

    public func head(for owner: AIOutgoingMessageQueueOwner, forwarding: Bool = false) throws
        -> AIOutgoingMessageQueueEntry?
    {
        try entries(for: owner).first {
            if $0.state == .accepted || $0.state == .forwarded { return false }
            return !((forwarding || $0.binding?.mode == .enqueue) && [.unknown, .failed].contains($0.state))
        }
    }

    /// revision 是一次宿主派发的 CAS 凭据；旧回调不能覆盖随后重试、迁移或恢复的状态。
    /// requeueRejected 的“明确未接受”由调用者判断；everUnknown 一旦为 true 就禁止改绑。
    @discardableResult
    public func transition(
        id: UUID, expectedRevision: UInt64,
        to change: AIOutgoingMessageQueueChange
    ) throws
        -> AIOutgoingMessageQueueEntry
    {
        try ensureLoaded()
        guard let index = document.entries.firstIndex(where: { $0.id == id }) else {
            throw AIOutgoingMessageQueueError.missingEntry
        }
        var entry = document.entries[index]
        guard entry.revision == expectedRevision else {
            throw AIOutgoingMessageQueueError.staleRevision
        }
        switch change {
        case .updatePayload(let data):
            guard entry.state == .queued, entry.binding == nil, !entry.everUnknown,
                !data.isEmpty, data.count <= maximumPayloadBytes
            else {
                throw AIOutgoingMessageQueueError.invalidTransition
            }
            entry.payloadData = data
        case .dispatch(let binding):
            guard entry.state == .queued,
                try head(for: entry.owner, forwarding: binding.mode == .enqueue)?.id == entry.id,
                binding.hostEpoch == hostEpoch, Self.validBinding(binding)
            else {
                throw AIOutgoingMessageQueueError.invalidTransition
            }
            entry.binding = binding
            entry.state = .dispatching
        case .forwarded:
            guard entry.state == .dispatching, entry.binding != nil else {
                throw AIOutgoingMessageQueueError.invalidTransition
            }
            entry.state = .forwarded
        case .accepted(let messageID):
            guard [.dispatching, .forwarded, .unknown].contains(entry.state),
                Self.validIdentifier(messageID)
            else {
                throw AIOutgoingMessageQueueError.invalidTransition
            }
            entry.acceptedMessageID = messageID
            entry.state = .accepted
        case .unknown:
            guard [.dispatching, .forwarded].contains(entry.state) else {
                throw AIOutgoingMessageQueueError.invalidTransition
            }
            entry.state = .unknown
            entry.everUnknown = true
        case .failed:
            guard [.queued, .dispatching, .forwarded].contains(entry.state), !entry.everUnknown,
                entry.binding == nil || entry.binding?.mode == .enqueue || entry.binding?.hostEpoch == hostEpoch
            else {
                throw AIOutgoingMessageQueueError.invalidTransition
            }
            entry.state = .failed
        case .requeueRejected:
            guard [.dispatching, .forwarded].contains(entry.state), !entry.everUnknown,
                entry.binding?.hostEpoch == hostEpoch
            else {
                throw AIOutgoingMessageQueueError.invalidTransition
            }
            entry.state = .queued
            entry.binding = nil
        case .retry:
            guard [.unknown, .failed].contains(entry.state) else {
                throw AIOutgoingMessageQueueError.invalidTransition
            }
            if let binding = entry.binding {
                let isHead = try head(for: entry.owner)?.id == entry.id
                guard binding.mode == .enqueue || binding.hostEpoch == hostEpoch,
                    entry.state != .unknown || binding.mode != .send,
                    binding.mode == .enqueue || isHead
                else {
                    throw AIOutgoingMessageQueueError.invalidTransition
                }
                entry.state = .dispatching
            } else {
                guard entry.state == .failed, !entry.everUnknown else {
                    throw AIOutgoingMessageQueueError.invalidTransition
                }
                entry.state = .queued
            }
        }
        guard entry.revision < UInt64.max else {
            throw AIOutgoingMessageQueueError.invalidTransition
        }
        entry.revision += 1
        var candidate = document
        candidate.entries[index] = entry
        try commit(candidate)
        return entry
    }

    @discardableResult
    public func removeAccepted(id: UUID, expectedRevision: UInt64) throws -> Bool {
        try ensureLoaded()
        guard let entry = document.entries.first(where: { $0.id == id }) else { return false }
        guard entry.revision == expectedRevision else {
            throw AIOutgoingMessageQueueError.staleRevision
        }
        guard entry.state == .accepted else { throw AIOutgoingMessageQueueError.invalidTransition }
        var candidate = document
        candidate.entries.removeAll { $0.id == id }
        try commit(candidate)
        return true
    }

    /// 空草稿创建出真实会话后，在任何派发之前迁移所有条目；不修改 payload 或用户点击 ID。
    @discardableResult
    public func reassignQueued(
        from source: AIOutgoingMessageQueueOwner,
        to target: AIOutgoingMessageQueueOwner
    ) throws
        -> [AIOutgoingMessageQueueEntry]
    {
        try ensureLoaded()
        try validateOwner(source)
        try validateOwner(target)
        let selected = document.entries.filter { $0.owner == source }
        guard selected.allSatisfy({ $0.state == .queued && $0.revision < UInt64.max }) else {
            throw AIOutgoingMessageQueueError.invalidTransition
        }
        var candidate = document
        let selectedIDs = Set(selected.map(\.id))
        for index in candidate.entries.indices where candidate.entries[index].owner == source {
            candidate.entries[index].owner = target
            candidate.entries[index].revision += 1
        }
        try commit(candidate)
        return ordered(candidate.entries.filter { selectedIDs.contains($0.id) })
    }

    /// 仅供显式删除会话；宿主先停止该会话的泵，写盘成功后才释放返回条目的附件引用。
    @discardableResult
    public func removeAll(for owner: AIOutgoingMessageQueueOwner) throws
        -> [AIOutgoingMessageQueueEntry]
    {
        try ensureLoaded()
        try validateOwner(owner)
        let removed = document.entries.filter { $0.owner == owner }
        var candidate = document
        candidate.entries.removeAll { $0.owner == owner }
        try commit(candidate)
        return ordered(removed)
    }

    private func ensureLoaded() throws {
        try ensureActive()
        guard !loaded else { return }
        if FileManager.default.fileExists(atPath: fileURL.path) {
            let values = try fileURL.resourceValues(forKeys: [.fileSizeKey, .isRegularFileKey])
            guard values.isRegularFile == true, let bytes = values.fileSize,
                bytes <= maximumStoreBytes
            else { throw AIOutgoingMessageQueueError.corruptStore }
            let data = try Data(contentsOf: fileURL)
            guard data.count <= maximumStoreBytes else {
                throw AIOutgoingMessageQueueError.corruptStore
            }
            let decoded: Document
            do { decoded = try JSONDecoder().decode(Document.self, from: data) } catch {
                throw AIOutgoingMessageQueueError.corruptStore
            }
            try validateDocument(decoded)
            var recovered = decoded
            var changed = false
            for index in recovered.entries.indices
            where [.dispatching, .forwarded].contains(recovered.entries[index].state) {
                guard recovered.entries[index].revision < UInt64.max else {
                    throw AIOutgoingMessageQueueError.corruptStore
                }
                recovered.entries[index].state = .unknown
                recovered.entries[index].everUnknown = true
                recovered.entries[index].revision += 1
                changed = true
            }
            if changed { try write(recovered) }
            document = recovered
        }
        loaded = true
    }

    private func commit(_ candidate: Document) throws {
        try ensureActive()
        try write(candidate)
        document = candidate
    }

    private func write(_ candidate: Document) throws {
        // 恢复也会直接写盘；所有路径在建目录之前统一拒绝已删除账号的旧 actor。
        try ensureActive()
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        let data = try encoder.encode(candidate)
        guard data.count <= maximumStoreBytes else {
            throw AIOutgoingMessageQueueError.capacityExceeded
        }
        try FileManager.default.createDirectory(
            at: fileURL.deletingLastPathComponent(),
            withIntermediateDirectories: true,
            attributes: [.posixPermissions: 0o700])
        #if os(iOS)
            try data.write(
                to: fileURL,
                options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])
        #else
            try data.write(to: fileURL, options: .atomic)
        #endif
        // 原子提交之后的辅助文件属性失败不把已提交的消息误报为“未入队”。
        try? FileManager.default.setAttributes(
            [.posixPermissions: 0o600], ofItemAtPath: fileURL.path)
        var backupURL = fileURL
        var values = URLResourceValues()
        values.isExcludedFromBackup = true
        try? backupURL.setResourceValues(values)
    }

    private func ensureActive() throws {
        guard !permanentlyInvalidated else {
            throw AIOutgoingMessageQueueError.permanentlyInvalidated
        }
    }

    private func validateOwner(_ owner: AIOutgoingMessageQueueOwner) throws {
        guard owner.accountID == accountID, owner.workspaceID == workspaceID else {
            throw AIOutgoingMessageQueueError.ownerMismatch
        }
        guard Self.validIdentifier(owner.conversationID) else {
            throw AIOutgoingMessageQueueError.invalidInput
        }
    }

    private func validateDocument(_ value: Document) throws {
        guard value.version == 1 else { throw AIOutgoingMessageQueueError.incompatibleStore }
        guard value.accountID == accountID, value.workspaceID == workspaceID else {
            throw AIOutgoingMessageQueueError.ownerMismatch
        }
        guard value.entries.count <= maximumEntries,
            Set(value.entries.map(\.id)).count == value.entries.count,
            Set(value.entries.map(\.sequence)).count == value.entries.count,
            value.nextSequence > (value.entries.map(\.sequence).max() ?? 0)
        else {
            throw AIOutgoingMessageQueueError.corruptStore
        }
        for entry in value.entries {
            try validateOwner(entry.owner)
            guard entry.sequence > 0, entry.revision > 0,
                !entry.payloadData.isEmpty, entry.payloadData.count <= maximumPayloadBytes,
                entry.createdAt.timeIntervalSinceReferenceDate.isFinite,
                entry.binding.map(Self.validBinding) ?? true,
                !entry.everUnknown || (entry.binding != nil && entry.state != .queued)
            else {
                throw AIOutgoingMessageQueueError.corruptStore
            }
            switch entry.state {
            case .queued:
                guard entry.binding == nil, entry.acceptedMessageID == nil else {
                    throw AIOutgoingMessageQueueError.corruptStore
                }
            case .dispatching, .forwarded, .unknown:
                guard entry.binding != nil, entry.acceptedMessageID == nil,
                    entry.state != .unknown || entry.everUnknown
                else {
                    throw AIOutgoingMessageQueueError.corruptStore
                }
            case .accepted:
                guard entry.binding != nil,
                    entry.acceptedMessageID.map(Self.validIdentifier) == true
                else {
                    throw AIOutgoingMessageQueueError.corruptStore
                }
            case .failed:
                guard entry.acceptedMessageID == nil else {
                    throw AIOutgoingMessageQueueError.corruptStore
                }
            }
        }
    }

    private static func validIdentifier(_ value: String) -> Bool {
        !value.isEmpty && value.utf8.count <= 4096 && !value.contains("\0")
    }

    private static func validBinding(_ value: AIOutgoingMessageQueueBinding) -> Bool {
        guard validIdentifier(value.hostEpoch) else { return false }
        switch value.mode {
        case .send, .enqueue: return value.turnID.isEmpty && value.questionID == nil
        case .steer: return validIdentifier(value.turnID) && value.questionID == nil
        case .answer: return value.turnID.isEmpty && value.questionID.map(validIdentifier) == true
        }
    }

    private func ordered(_ entries: [AIOutgoingMessageQueueEntry]) -> [AIOutgoingMessageQueueEntry]
    {
        entries.sorted { $0.sequence < $1.sequence }
    }
}
