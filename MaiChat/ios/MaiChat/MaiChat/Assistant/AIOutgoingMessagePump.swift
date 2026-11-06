// 文件作用：在持久队列上按会话顺序派发；主线程只调度，I/O 与 native 调用由注入的普通接口执行。
import Foundation
import MaiChatCore

@MainActor
final class AIOutgoingMessagePump {
    enum FailureDecision { case unknown, failed, requeueRejected, wait }
    let queue: AIOutgoingMessageQueue
    private let prepare: (AIOutgoingMessageQueueEntry) async throws -> Data
    private let bind: (AIOutgoingMessageQueueEntry) async throws -> AIOutgoingMessageQueueBinding
    private let deliver: (AIOutgoingMessageQueueEntry) async throws -> String
    private let failure: (AIOutgoingMessageQueueEntry, Error) async -> FailureDecision
    private let changed: ([AIOutgoingMessageQueueEntry]) -> Void
    private let accepted: (AIOutgoingMessageQueueEntry) async -> Void
    private let forwardsWithoutWaiting: (AIOutgoingMessageQueueOwner) -> Bool
    private var tasks: [AIOutgoingMessageQueueOwner: Task<Void, Never>] = [:]
    private var laneIDs: [AIOutgoingMessageQueueOwner: UUID] = [:]
    private var entryLanes: [UUID: UUID] = [:]
    private var confirmations: [UUID: Task<Void, Never>] = [:]
    private var ownerVersions: [AIOutgoingMessageQueueOwner: UInt64] = [:]
    private var paused: Set<AIOutgoingMessageQueueOwner> = []
    private var deferredWakes: Set<AIOutgoingMessageQueueOwner> = []
    private var stopped = false

    init(queue: AIOutgoingMessageQueue,
         prepare: @escaping (AIOutgoingMessageQueueEntry) async throws -> Data,
         bind: @escaping (AIOutgoingMessageQueueEntry) async throws -> AIOutgoingMessageQueueBinding,
         deliver: @escaping (AIOutgoingMessageQueueEntry) async throws -> String,
         failure: @escaping (AIOutgoingMessageQueueEntry, Error) async -> FailureDecision,
         changed: @escaping ([AIOutgoingMessageQueueEntry]) -> Void,
         accepted: @escaping (AIOutgoingMessageQueueEntry) async -> Void,
         forwardsWithoutWaiting: @escaping (AIOutgoingMessageQueueOwner) -> Bool = { _ in false }) {
        self.queue = queue
        self.prepare = prepare
        self.bind = bind
        self.deliver = deliver
        self.failure = failure
        self.changed = changed
        self.accepted = accepted
        self.forwardsWithoutWaiting = forwardsWithoutWaiting
    }

    func publish() async {
        if let entries = try? await queue.allEntries() { changed(entries) }
    }

    func wake(_ owner: AIOutgoingMessageQueueOwner) {
        guard !stopped else { return }
        paused.remove(owner)
        guard tasks[owner] == nil else { deferredWakes.insert(owner); return }
        let version = ownerVersions[owner, default: 0]
        let lane = UUID()
        laneIDs[owner] = lane
        tasks[owner] = Task { @MainActor [weak self] in await self?.run(owner, version: version, lane: lane) }
    }

    func pause(_ owner: AIOutgoingMessageQueueOwner) {
        ownerVersions[owner, default: 0] &+= 1
        paused.insert(owner)
        deferredWakes.remove(owner)
    }

    func stop() {
        stopped = true
        // native 请求已经派发后不因切页/换号强杀；旧回执只更新旧队列，不清新账号草稿。
        // 任务在每次 await 后检查 stopped，已派发任务仍完成收据持久化后退出。
    }

    func invalidateAndWait() async {
        stopped = true
        let active = Array(tasks.values) + Array(confirmations.values)
        active.forEach { $0.cancel() }
        for task in active { await task.value }
    }

    func retry(_ id: UUID) async {
        guard !stopped, let entry = try? await queue.allEntries().first(where: { $0.id == id }) else { return }
        do {
            _ = try await queue.transition(id: id, expectedRevision: entry.revision, to: .retry)
            await publish()
            wake(entry.owner)
        } catch { /* 不兼容的 unknown 保持待确认，不盲目重播普通 send。 */ }
    }

    /// 只释放转发顺序，不伪造所属端的接收成功；各条的确认仍独立落盘。
    func forwarded(_ entry: AIOutgoingMessageQueueEntry) async {
        guard forwardsWithoutWaiting(entry.owner) else { return }
        do {
            let current = try await queue.allEntries().first(where: { $0.id == entry.id })
            if let current, current.state == .dispatching {
                _ = try await queue.transition(id: current.id, expectedRevision: current.revision, to: .forwarded)
                await publish()
            } else if let current, ![.accepted, .forwarded].contains(current.state) { return }
            // 业务回执可先于转发 RPC 返回，历史刷新也不能占住下一条的发送通道。
            guard let lane = entryLanes[entry.id], laneIDs[entry.owner] == lane,
                  let task = tasks.removeValue(forKey: entry.owner) else { return }
            confirmations[entry.id] = task
            laneIDs.removeValue(forKey: entry.owner)
            if !stopped, !paused.contains(entry.owner) { wake(entry.owner) }
        } catch { /* 写盘失败时保留 dispatching，不放行后续派发。 */ }
    }

    private func run(_ owner: AIOutgoingMessageQueueOwner, version: UInt64, lane: UUID) async {
        var currentID: UUID?
        defer {
            if let currentID { entryLanes.removeValue(forKey: currentID); confirmations.removeValue(forKey: currentID) }
            if laneIDs[owner] == lane {
                tasks.removeValue(forKey: owner)
                laneIDs.removeValue(forKey: owner)
                if deferredWakes.remove(owner) != nil, !paused.contains(owner), !stopped { wake(owner) }
            }
        }
        while !stopped && !paused.contains(owner) && ownerVersions[owner, default: 0] == version {
            guard var entry = try? await queue.head(for: owner, forwarding: forwardsWithoutWaiting(owner)) else { return }
            if entry.state == .unknown || entry.state == .failed { return }
            do {
                if entry.state == .queued {
                    let data = try await prepare(entry)
                    guard !stopped, !paused.contains(owner), ownerVersions[owner, default: 0] == version else { return }
                    if let latest = try await queue.allEntries().first(where: { $0.id == entry.id }) {
                        entry = latest
                    }
                    if data != entry.payloadData {
                        entry = try await queue.transition(id: entry.id, expectedRevision: entry.revision,
                                                           to: .updatePayload(data))
                    }
                    let binding = try await bind(entry)
                    guard !stopped, !paused.contains(owner), ownerVersions[owner, default: 0] == version else { return }
                    entry = try await queue.transition(id: entry.id, expectedRevision: entry.revision,
                                                       to: .dispatch(binding))
                    await publish()
                }
                guard entry.state == .dispatching else { return }
                currentID = entry.id
                entryLanes[entry.id] = lane
                let message = try await deliver(entry)
                if let latest = try await queue.allEntries().first(where: { $0.id == entry.id }) { entry = latest }
                let confirmed = try await queue.transition(id: entry.id, expectedRevision: entry.revision,
                                                           to: .accepted(messageID: message))
                await publish()
                await accepted(confirmed)
                entryLanes.removeValue(forKey: entry.id)
                if laneIDs[owner] != lane { return }
            } catch {
                if let latest = try? await queue.allEntries().first(where: { $0.id == entry.id }) { entry = latest }
                let decision = await failure(entry, error)
                do {
                    switch decision {
                    case .wait:
                        scheduleWake(owner, version: version)
                        return
                    case .unknown:
                        guard [.dispatching, .forwarded].contains(entry.state) else { return }
                        _ = try await queue.transition(id: entry.id, expectedRevision: entry.revision, to: .unknown)
                    case .failed:
                        _ = try await queue.transition(id: entry.id, expectedRevision: entry.revision,
                            to: entry.everUnknown ? .unknown : .failed)
                    case .requeueRejected:
                        guard !entry.everUnknown else {
                            _ = try await queue.transition(id: entry.id, expectedRevision: entry.revision,
                                to: entry.everUnknown ? .unknown : .failed)
                            await publish()
                            return
                        }
                        _ = try await queue.transition(id: entry.id, expectedRevision: entry.revision,
                                                       to: .requeueRejected)
                        await publish()
                        scheduleWake(owner, version: version)
                        return
                    }
                } catch { /* 旧 revision/写盘失败保留原状态，不用错误回调覆盖已接受消息。 */ }
                await publish()
                if entry.binding?.mode == .enqueue, laneIDs[owner] == lane {
                    deferredWakes.insert(owner)
                }
                return
            }
        }
    }

    private func scheduleWake(_ owner: AIOutgoingMessageQueueOwner, version: UInt64) {
        Task { @MainActor [weak self] in
            try? await Task.sleep(for: .milliseconds(500))
            guard let self, !self.stopped, !self.paused.contains(owner),
                  self.ownerVersions[owner, default: 0] == version else { return }
            self.wake(owner)
        }
    }
}
