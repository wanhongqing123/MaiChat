// 文件作用：在 SDK 原到达处登记序列，以实际 DB 写入结果确认历史快照的持久化屏障。
// 不保存正文、不等待下载，不是路由/执行队列。失败债务只由同消息真实成功或新来源代际解除。
import Foundation

final class IMIngressPersistenceFence: @unchecked Sendable {
    struct Ticket: Sendable, Equatable {
        let generation: UUID
        let accountTag: String
        let peerID: String
        let sourceID: String
        let sequence: UInt64
        let tracked: Bool
    }
    struct Capture: Sendable {
        fileprivate let generation: UUID
        fileprivate let accountTag: String
        fileprivate let peerID: String
        fileprivate let sequence: UInt64
    }
    private struct Pending {
        let peerID: String
        let sourceID: String
    }
    private struct Source: Hashable {
        let peerID: String
        let sourceID: String
    }
    private let lock = NSLock()
    private var generation = UUID()
    private var accountTag: String?
    private var sequence: UInt64 = 0
    private var pending: [UInt64: Pending] = [:]
    private var overflow: [String: Int] = [:]
    private var debt: [Source: UInt64] = [:]
    private var debtOverflow = Set<String>()
    // Only keep success ordering while another callback for that source is pending.
    private var confirmed: [Source: UInt64] = [:]

    func activate(accountTag: String, generation: UUID) {
        lock.lock(); defer { lock.unlock() }
        guard self.accountTag != accountTag || self.generation != generation else { return }
        self.accountTag = accountTag; self.generation = generation
        sequence = 0; pending = [:]; overflow = [:]; debt = [:]; debtOverflow = []; confirmed = [:]
    }
    func invalidate() {
        lock.lock(); defer { lock.unlock() }
        accountTag = nil; generation = UUID()
        pending = [:]; overflow = [:]; debt = [:]; debtOverflow = []; confirmed = [:]
    }
    func begin(accountTag: String?, generation: UUID, peerID: String, sourceID: String? = nil) -> Ticket? {
        lock.lock(); defer { lock.unlock() }
        guard let accountTag, self.accountTag == accountTag, self.generation == generation,
              !peerID.isEmpty, sequence < UInt64.max else { return nil }
        sequence += 1
        let identity = sourceID ?? "ingress:\(sequence)"
        let tracked = pending.count < 4096
        if tracked { pending[sequence] = Pending(peerID: peerID, sourceID: identity) }
        else { overflow[peerID, default: 0] += 1 }
        return Ticket(generation: generation, accountTag: accountTag, peerID: peerID,
                      sourceID: identity, sequence: sequence, tracked: tracked)
    }
    func finish(_ ticket: Ticket, persisted: Bool) {
        lock.lock(); defer { lock.unlock() }
        guard generation == ticket.generation, accountTag == ticket.accountTag else { return }
        let source = Source(peerID: ticket.peerID, sourceID: ticket.sourceID)
        let completed: Bool
        if ticket.tracked { completed = pending.removeValue(forKey: ticket.sequence) != nil }
        else {
            completed = overflow[ticket.peerID, default: 0] > 0
            if completed { overflow[ticket.peerID, default: 0] -= 1 }
        }
        // A retry can report the original owned receipt only after that exact row really commits.
        guard completed || (persisted && debt[source] != nil) else { return }
        if persisted {
            debt.removeValue(forKey: source)
            confirmed[source] = max(confirmed[source] ?? 0, ticket.sequence)
        } else if ticket.sequence > (confirmed[source] ?? 0) {
            if debt[source] != nil || debt.count < 4096 {
                debt[source] = min(debt[source] ?? ticket.sequence, ticket.sequence)
            } else {
                // Do not forget an unrepresented failure when bounded bookkeeping is exhausted.
                debtOverflow.insert(ticket.peerID)
            }
        }
        if !pending.contains(where: { $0.value.peerID == ticket.peerID && $0.value.sourceID == ticket.sourceID }) {
            confirmed.removeValue(forKey: source)
        }
    }
    private func checkDebt(peerID: String, through sequence: UInt64) throws {
        guard !debtOverflow.contains(peerID),
              !debt.contains(where: { $0.key.peerID == peerID && $0.value <= sequence }) else {
            throw LocalChatMessageQueryError(code: "history_persistence_failed", message: "有消息尚未保存，请重试")
        }
    }
    func capture(accountTag: String, peerID: String) throws -> Capture {
        lock.lock(); defer { lock.unlock() }
        guard self.accountTag == accountTag else { throw LocalChatMessageQueryError.changed }
        try checkDebt(peerID: peerID, through: sequence)
        guard overflow[peerID, default: 0] == 0 else {
            throw LocalChatMessageQueryError(code: "busy", message: "IM metadata persistence is busy.")
        }
        return Capture(generation: generation, accountTag: accountTag, peerID: peerID, sequence: sequence)
    }
    private func isComplete(_ capture: Capture) throws -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard generation == capture.generation, accountTag == capture.accountTag else {
            throw LocalChatMessageQueryError.changed
        }
        try checkDebt(peerID: capture.peerID, through: capture.sequence)
        guard overflow[capture.peerID, default: 0] == 0 else {
            throw LocalChatMessageQueryError(code: "busy", message: "IM metadata persistence is busy.")
        }
        return !pending.contains { $0.key <= capture.sequence && $0.value.peerID == capture.peerID }
    }
    func wait(_ capture: Capture, deadline: UInt64) async throws {
        while true {
            try Task.checkCancellation()
            if try isComplete(capture) { return }
            guard DispatchTime.now().uptimeNanoseconds < deadline else {
                throw LocalChatMessageQueryError(code: "busy", message: "IM metadata persistence deadline elapsed.")
            }
            try await Task.sleep(for: .milliseconds(10))
        }
    }
}
