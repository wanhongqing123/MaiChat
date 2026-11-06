// 文件作用：以隔离临时目录验证创作中心 outbox 的持久 FIFO、确认、恢复、账号边界和写盘失败。
import Foundation
import XCTest

@testable import MaiChatCore

@MainActor
final class AIOutgoingMessageQueueTests: XCTestCase {
    private struct Fixture {
        let root: URL
        var file: URL { root.appendingPathComponent("account/outbox.json") }
        let owner = AIOutgoingMessageQueueOwner(
            accountID: "account-a", workspaceID: "workspace-a",
            conversationID: "local:session-a")

        init() throws {
            root = FileManager.default.temporaryDirectory.appendingPathComponent(
                "ai-outbox-test-" + UUID().uuidString)
            try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        }
        func queue(epoch: String = "epoch-a", maximumEntries: Int = 256) throws
            -> AIOutgoingMessageQueue
        {
            try AIOutgoingMessageQueue(
                fileURL: file, accountID: owner.accountID,
                workspaceID: owner.workspaceID, hostEpoch: epoch,
                maximumEntries: maximumEntries)
        }
        func cleanup() { try? FileManager.default.removeItem(at: root) }
    }

    func testAnswerRetryKeepsItsOriginalQuestionAcrossReload() async throws {
        let fixture = try Fixture()
        defer { fixture.cleanup() }
        let queue = try fixture.queue()
        let entry = try await queue.enqueue(owner: fixture.owner, payloadData: Data("answer".utf8))
        let binding = AIOutgoingMessageQueueBinding(mode: .answer, questionID: "question-a", hostEpoch: "epoch-a")
        let sending = try await queue.transition(id: entry.id, expectedRevision: entry.revision,
                                                to: .dispatch(binding))
        _ = try await queue.transition(id: sending.id, expectedRevision: sending.revision, to: .unknown)
        let reopened = try fixture.queue()
        let loaded = try await reopened.load()
        let unknown = try XCTUnwrap(loaded.first)
        let retry = try await reopened.transition(id: unknown.id, expectedRevision: unknown.revision, to: .retry)
        XCTAssertEqual(retry.binding, binding)
        XCTAssertEqual(retry.binding?.questionID, "question-a")
        XCTAssertEqual(retry.state, .dispatching)
        let restarted = try fixture.queue(epoch: "epoch-b")
        let recovered = try await restarted.load()
        let old = try XCTUnwrap(recovered.first)
        do {
            _ = try await restarted.transition(id: old.id, expectedRevision: old.revision, to: .retry)
            XCTFail("An answer cannot be replayed into a replacement Agent runtime")
        } catch { XCTAssertEqual(error as? AIOutgoingMessageQueueError, .invalidTransition) }
    }

    func testForwardedRemoteInputsDoNotWaitForOwnerReceiptsAndCanConfirmOutOfOrder() async throws {
        let fixture = try Fixture(); defer { fixture.cleanup() }
        let queue = try fixture.queue()
        let first = try await queue.enqueue(owner: fixture.owner, payloadData: Data("first".utf8))
        let second = try await queue.enqueue(owner: fixture.owner, payloadData: Data("second".utf8))
        let binding = AIOutgoingMessageQueueBinding(mode: .enqueue, hostEpoch: "epoch-a")
        let dispatched = try await queue.transition(id: first.id, expectedRevision: first.revision, to: .dispatch(binding))
        let forwarded = try await queue.transition(id: first.id, expectedRevision: dispatched.revision, to: .forwarded)
        let next = try await queue.head(for: fixture.owner)
        XCTAssertEqual(next?.id, second.id)
        let secondDispatch = try await queue.transition(id: second.id, expectedRevision: second.revision, to: .dispatch(binding))
        let secondForward = try await queue.transition(id: second.id, expectedRevision: secondDispatch.revision, to: .forwarded)
        _ = try await queue.transition(id: second.id, expectedRevision: secondForward.revision, to: .accepted(messageID: "msg_second"))
        _ = try await queue.transition(id: first.id, expectedRevision: forwarded.revision, to: .accepted(messageID: "msg_first"))
        let all = try await queue.allEntries()
        XCTAssertEqual(all.map(\.acceptedMessageID), ["msg_first", "msg_second"])
    }

    func testQueuedRemoteRetrySurvivesEpochChangeWithoutChangingItsRequestIdentity() async throws {
        let fixture = try Fixture(); defer { fixture.cleanup() }
        let queue = try fixture.queue()
        let first = try await queue.enqueue(owner: fixture.owner, payloadData: Data("input".utf8))
        let sent = try await queue.transition(id: first.id, expectedRevision: first.revision,
            to: .dispatch(AIOutgoingMessageQueueBinding(mode: .enqueue, hostEpoch: "epoch-a")))
        _ = try await queue.transition(id: first.id, expectedRevision: sent.revision, to: .forwarded)
        let reopened = try fixture.queue(epoch: "epoch-b")
        let restored = try await reopened.load()
        XCTAssertEqual(restored.first?.state, .unknown)
        let retried = try await reopened.transition(id: first.id, expectedRevision: restored[0].revision, to: .retry)
        XCTAssertEqual(retried.requestID, first.requestID)
        XCTAssertEqual(retried.payloadData, first.payloadData)
    }

    func testOldUnknownSendDoesNotBlockNewRemoteQueueInputs() async throws {
        let fixture = try Fixture(); defer { fixture.cleanup() }
        let queue = try fixture.queue()
        let old = try await queue.enqueue(owner: fixture.owner, payloadData: Data("old".utf8))
        let sent = try await queue.transition(id: old.id, expectedRevision: old.revision,
            to: .dispatch(AIOutgoingMessageQueueBinding(mode: .send, hostEpoch: "epoch-a")))
        _ = try await queue.transition(id: old.id, expectedRevision: sent.revision, to: .unknown)
        let fresh = try await queue.enqueue(owner: fixture.owner, payloadData: Data("new".utf8))
        let serial = try await queue.head(for: fixture.owner)
        let forwarding = try await queue.head(for: fixture.owner, forwarding: true)
        XCTAssertEqual(serial?.id, old.id)
        XCTAssertEqual(forwarding?.id, fresh.id)
        _ = try await queue.transition(id: fresh.id, expectedRevision: fresh.revision,
            to: .dispatch(AIOutgoingMessageQueueBinding(mode: .enqueue, hostEpoch: "epoch-a")))
    }

    func testIdenticalClicksRemainDistinctAndSameClickEnqueueIsIdempotentAcrossReload() async throws
    {
        let fixture = try Fixture()
        defer { fixture.cleanup() }
        let queue = try fixture.queue()
        let payload = Data("same user message".utf8)
        let first = try await queue.enqueue(
            owner: fixture.owner, payloadData: payload,
            createdAt: Date(timeIntervalSince1970: 200))
        let second = try await queue.enqueue(
            owner: fixture.owner, payloadData: payload,
            createdAt: Date(timeIntervalSince1970: 100))
        let retry = try await queue.enqueue(
            owner: fixture.owner, payloadData: payload, id: first.id)
        XCTAssertEqual(retry, first)
        XCTAssertNotEqual(first.id, second.id)
        XCTAssertEqual(first.requestID, first.id.uuidString.lowercased())
        let reopened = try fixture.queue(epoch: "epoch-b")
        let loaded = try await reopened.load()
        XCTAssertEqual(loaded.map(\.id), [first.id, second.id])
        XCTAssertEqual(loaded.map(\.sequence), [1, 2])
        do {
            _ = try await reopened.enqueue(
                owner: fixture.owner, payloadData: Data("changed".utf8), id: first.id)
            XCTFail("A reused click ID cannot change its content")
        } catch { XCTAssertEqual(error as? AIOutgoingMessageQueueError, .duplicateID) }
        let third = try await reopened.enqueue(owner: fixture.owner, payloadData: payload)
        XCTAssertEqual(third.sequence, 3)
    }

    func testConcurrentLocalIntakeKeepsEveryMessageWhileTheHeadAwaitsConfirmation() async throws {
        let fixture = try Fixture()
        defer { fixture.cleanup() }
        let queue = try fixture.queue()
        let owner = fixture.owner
        let first = try await queue.enqueue(owner: owner, payloadData: Data("waiting".utf8))
        let sending = try await queue.transition(
            id: first.id, expectedRevision: first.revision,
            to: .dispatch(AIOutgoingMessageQueueBinding(mode: .send, hostEpoch: "epoch-a")))
        _ = try await queue.transition(
            id: sending.id, expectedRevision: sending.revision, to: .unknown)
        let received = try await withThrowingTaskGroup(of: AIOutgoingMessageQueueEntry.self) {
            group in
            for _ in 0..<16 {
                group.addTask {
                    try await queue.enqueue(
                        owner: owner, payloadData: Data("same repeated text".utf8))
                }
            }
            var entries: [AIOutgoingMessageQueueEntry] = []
            for try await entry in group { entries.append(entry) }
            return entries
        }
        XCTAssertEqual(Set(received.map(\.id)).count, 16)
        let loaded = try await fixture.queue(epoch: "next-process").load()
        XCTAssertEqual(loaded.count, 17)
        XCTAssertEqual(loaded.map(\.sequence), Array(1...17).map(UInt64.init))
        XCTAssertEqual(loaded.first?.id, first.id)
        XCTAssertEqual(loaded.first?.state, .unknown)
        XCTAssertTrue(loaded.dropFirst().allSatisfy { $0.state == .queued })
    }

    func testOwnerIsolationAndWrongAccountDoNotOverwriteTheStore() async throws {
        let fixture = try Fixture()
        defer { fixture.cleanup() }
        let queue = try fixture.queue()
        let otherRoom = AIOutgoingMessageQueueOwner(
            accountID: fixture.owner.accountID,
            workspaceID: fixture.owner.workspaceID, conversationID: "remote:phone:login-b:session-a"
        )
        let first = try await queue.enqueue(owner: fixture.owner, payloadData: Data("one".utf8))
        let second = try await queue.enqueue(owner: otherRoom, payloadData: Data("two".utf8))
        let binding = AIOutgoingMessageQueueBinding(mode: .send, hostEpoch: "epoch-a")
        let sending = try await queue.transition(
            id: first.id, expectedRevision: first.revision, to: .dispatch(binding))
        _ = try await queue.transition(
            id: sending.id, expectedRevision: sending.revision, to: .unknown)
        let otherHead = try await queue.head(for: otherRoom)
        XCTAssertEqual(otherHead?.id, second.id)
        let unrelated = try await queue.transition(
            id: second.id, expectedRevision: second.revision, to: .dispatch(binding))
        XCTAssertEqual(unrelated.state, .dispatching)
        let originalFile = try Data(contentsOf: fixture.file)
        let wrong = try AIOutgoingMessageQueue(
            fileURL: fixture.file, accountID: "account-b",
            workspaceID: fixture.owner.workspaceID, hostEpoch: "epoch-b")
        do {
            _ = try await wrong.load()
            XCTFail("Another account cannot adopt a queue file")
        } catch { XCTAssertEqual(error as? AIOutgoingMessageQueueError, .ownerMismatch) }
        XCTAssertEqual(try Data(contentsOf: fixture.file), originalFile)
    }

    func testOnlyHeadDispatchesAndPayloadAndCallbacksUseRevisionCompareAndSwap() async throws {
        let fixture = try Fixture()
        defer { fixture.cleanup() }
        let queue = try fixture.queue()
        let first = try await queue.enqueue(
            owner: fixture.owner, payloadData: Data("upload not prepared".utf8))
        let second = try await queue.enqueue(owner: fixture.owner, payloadData: Data("next".utf8))
        let binding = AIOutgoingMessageQueueBinding(
            mode: .steer, turnID: "turn-a", hostEpoch: "epoch-a")
        do {
            _ = try await queue.transition(
                id: second.id, expectedRevision: second.revision, to: .dispatch(binding))
            XCTFail("Later messages cannot overtake the head")
        } catch { XCTAssertEqual(error as? AIOutgoingMessageQueueError, .invalidTransition) }
        let uploaded = try await queue.transition(
            id: first.id, expectedRevision: first.revision,
            to: .updatePayload(Data("stable attachment id".utf8)))
        do {
            _ = try await queue.transition(
                id: first.id, expectedRevision: first.revision, to: .dispatch(binding))
            XCTFail("The old upload snapshot is stale")
        } catch { XCTAssertEqual(error as? AIOutgoingMessageQueueError, .staleRevision) }
        let sending = try await queue.transition(
            id: uploaded.id, expectedRevision: uploaded.revision, to: .dispatch(binding))
        do {
            _ = try await queue.transition(
                id: sending.id, expectedRevision: sending.revision,
                to: .updatePayload(Data("different attachment".utf8)))
            XCTFail("Dispatched content is immutable")
        } catch { XCTAssertEqual(error as? AIOutgoingMessageQueueError, .invalidTransition) }
        let accepted = try await queue.transition(
            id: sending.id, expectedRevision: sending.revision,
            to: .accepted(messageID: "user-message-a"))
        let head = try await queue.head(for: fixture.owner)
        XCTAssertEqual(head?.id, second.id)
        let visible = try await queue.entries(for: fixture.owner)
        XCTAssertEqual(visible.first?.acceptedMessageID, "user-message-a")
        let removed = try await queue.removeAccepted(
            id: accepted.id, expectedRevision: accepted.revision)
        XCTAssertTrue(removed)
        let remaining = try await queue.entries(for: fixture.owner)
        XCTAssertEqual(remaining.map(\.id), [second.id])
    }

    func testUnknownSendBlocksItsHeadButStillAcceptsMoreLocalMessages() async throws {
        let fixture = try Fixture()
        defer { fixture.cleanup() }
        let queue = try fixture.queue()
        let first = try await queue.enqueue(owner: fixture.owner, payloadData: Data("one".utf8))
        let sending = try await queue.transition(
            id: first.id, expectedRevision: first.revision,
            to: .dispatch(AIOutgoingMessageQueueBinding(mode: .send, hostEpoch: "epoch-a")))
        let unknown = try await queue.transition(
            id: sending.id, expectedRevision: sending.revision, to: .unknown)
        let later = try await queue.enqueue(owner: fixture.owner, payloadData: Data("two".utf8))
        let head = try await queue.head(for: fixture.owner)
        XCTAssertEqual(head?.id, unknown.id)
        XCTAssertEqual(later.state, .queued)
        for change in [AIOutgoingMessageQueueChange.retry, .requeueRejected, .failed] {
            do {
                _ = try await queue.transition(
                    id: unknown.id, expectedRevision: unknown.revision, to: change)
                XCTFail("Unknown ordinary send cannot be replayed or declared rejected")
            } catch { XCTAssertEqual(error as? AIOutgoingMessageQueueError, .invalidTransition) }
        }
        let confirmed = try await queue.transition(
            id: unknown.id, expectedRevision: unknown.revision,
            to: .accepted(messageID: "known-receipt"))
        XCTAssertTrue(confirmed.everUnknown)
        let next = try await queue.head(for: fixture.owner)
        XCTAssertEqual(next?.id, later.id)
    }

    func testUnknownSteerRetriesSameBindingAndCannotRebindAfterASecondRejection() async throws {
        let fixture = try Fixture()
        defer { fixture.cleanup() }
        let queue = try fixture.queue()
        let first = try await queue.enqueue(
            owner: fixture.owner, payloadData: Data("supplement".utf8))
        let binding = AIOutgoingMessageQueueBinding(
            mode: .steer, turnID: "turn-a", hostEpoch: "epoch-a")
        let sending = try await queue.transition(
            id: first.id, expectedRevision: first.revision, to: .dispatch(binding))
        let unknown = try await queue.transition(
            id: sending.id, expectedRevision: sending.revision, to: .unknown)
        let retry = try await queue.transition(
            id: unknown.id, expectedRevision: unknown.revision, to: .retry)
        XCTAssertEqual(retry.binding, binding)
        XCTAssertEqual(retry.requestID, first.requestID)
        for change in [AIOutgoingMessageQueueChange.requeueRejected, .failed] {
            do {
                _ = try await queue.transition(
                    id: retry.id, expectedRevision: retry.revision, to: change)
                XCTFail("A later NotFound cannot prove the earlier unknown attempt was rejected")
            } catch { XCTAssertEqual(error as? AIOutgoingMessageQueueError, .invalidTransition) }
        }
        let uncertain = try await queue.transition(
            id: retry.id, expectedRevision: retry.revision, to: .unknown)
        try await queue.updateHostEpoch("epoch-after-configure")
        do {
            _ = try await queue.transition(
                id: uncertain.id, expectedRevision: uncertain.revision, to: .retry)
            XCTFail("A reconfigured Agent does not own the previous deduplication receipts")
        } catch { XCTAssertEqual(error as? AIOutgoingMessageQueueError, .invalidTransition) }
        let unchanged = try await queue.head(for: fixture.owner)
        XCTAssertEqual(unchanged?.binding, binding)
        XCTAssertEqual(unchanged?.state, .unknown)
    }

    func testDefinitiveRejectionCanWaitForANewSnapshotBeforeChoosingTheRoute() async throws {
        let fixture = try Fixture()
        defer { fixture.cleanup() }
        let queue = try fixture.queue()
        let first = try await queue.enqueue(
            owner: fixture.owner, payloadData: Data("not yet accepted".utf8))
        let sending = try await queue.transition(
            id: first.id, expectedRevision: first.revision,
            to: .dispatch(
                AIOutgoingMessageQueueBinding(
                    mode: .steer, turnID: "ending-turn", hostEpoch: "epoch-a")))
        let rejected = try await queue.transition(
            id: sending.id, expectedRevision: sending.revision, to: .requeueRejected)
        XCTAssertEqual(rejected.state, .queued)
        XCTAssertNil(rejected.binding)
        XCTAssertFalse(rejected.everUnknown)
        let nextTurn = try await queue.transition(
            id: rejected.id, expectedRevision: rejected.revision,
            to: .dispatch(AIOutgoingMessageQueueBinding(mode: .send, hostEpoch: "epoch-a")))
        XCTAssertEqual(nextTurn.id, first.id)
        XCTAssertEqual(nextTurn.binding?.mode, .send)
        try await queue.updateHostEpoch("reconfigured-agent")
        do {
            _ = try await queue.transition(
                id: nextTurn.id, expectedRevision: nextTurn.revision, to: .requeueRejected)
            XCTFail("A replacement Agent cannot prove the old Agent did not accept the message")
        } catch { XCTAssertEqual(error as? AIOutgoingMessageQueueError, .invalidTransition) }
    }

    func testRecoveryPersistsUnknownAndNeverAutomaticallyReplaysAnInterruptedDispatch() async throws
    {
        let fixture = try Fixture()
        defer { fixture.cleanup() }
        let queue = try fixture.queue()
        let first = try await queue.enqueue(
            owner: fixture.owner, payloadData: Data("possibly accepted".utf8))
        let sending = try await queue.transition(
            id: first.id, expectedRevision: first.revision,
            to: .dispatch(
                AIOutgoingMessageQueueBinding(mode: .steer, turnID: "turn-a", hostEpoch: "epoch-a"))
        )
        let reopened = try fixture.queue(epoch: "new-native-agent")
        let recovered = try await reopened.load()
        XCTAssertEqual(recovered.count, 1)
        XCTAssertEqual(recovered.first?.state, .unknown)
        XCTAssertEqual(recovered.first?.binding, sending.binding)
        XCTAssertEqual(recovered.first?.revision, sending.revision + 1)
        XCTAssertEqual(recovered.first?.everUnknown, true)
        let again = try fixture.queue(epoch: "third-agent")
        let persisted = try await again.load()
        XCTAssertEqual(persisted, recovered)
        XCTAssertTrue(
            String(decoding: try Data(contentsOf: fixture.file), as: UTF8.self).contains(
                "\"state\":\"unknown\""))
    }

    func testDraftMigrationOnlyMovesUnsubmittedEntriesAndKeepsClickOrderAndPayload() async throws {
        let fixture = try Fixture()
        defer { fixture.cleanup() }
        let queue = try fixture.queue()
        let draft = AIOutgoingMessageQueueOwner(
            accountID: fixture.owner.accountID,
            workspaceID: fixture.owner.workspaceID, conversationID: "draft:stable-uuid")
        let one = try await queue.enqueue(owner: draft, payloadData: Data("one".utf8))
        let two = try await queue.enqueue(owner: draft, payloadData: Data("two".utf8))
        let moved = try await queue.reassignQueued(from: draft, to: fixture.owner)
        XCTAssertEqual(moved.map(\.id), [one.id, two.id])
        XCTAssertEqual(moved.map(\.payloadData), [one.payloadData, two.payloadData])
        XCTAssertTrue(moved.allSatisfy { $0.owner == fixture.owner })
        let old = try await queue.entries(for: draft)
        XCTAssertTrue(old.isEmpty)
        let head = try XCTUnwrap(moved.first)
        _ = try await queue.transition(
            id: head.id, expectedRevision: head.revision,
            to: .dispatch(AIOutgoingMessageQueueBinding(mode: .send, hostEpoch: "epoch-a")))
        do {
            _ = try await queue.reassignQueued(from: fixture.owner, to: draft)
            XCTFail("An in-flight or unknown message cannot move to a different conversation")
        } catch { XCTAssertEqual(error as? AIOutgoingMessageQueueError, .invalidTransition) }
    }

    func testDiskFailureDoesNotConsumeRevisionOrLoseTheLastCommittedFile() async throws {
        let fixture = try Fixture()
        defer { fixture.cleanup() }
        let queue = try fixture.queue()
        let first = try await queue.enqueue(owner: fixture.owner, payloadData: Data("durable".utf8))
        let bytes = try Data(contentsOf: fixture.file)
        let parent = fixture.file.deletingLastPathComponent()
        let savedParent = fixture.root.appendingPathComponent("saved-account")
        try FileManager.default.moveItem(at: parent, to: savedParent)
        try Data("not a directory".utf8).write(to: parent)
        do {
            _ = try await queue.transition(
                id: first.id, expectedRevision: first.revision,
                to: .dispatch(AIOutgoingMessageQueueBinding(mode: .send, hostEpoch: "epoch-a")))
            XCTFail("The filesystem failure must be reported before acceptance")
        } catch { XCTAssertFalse(error is AIOutgoingMessageQueueError) }
        let unchanged = try await queue.head(for: fixture.owner)
        XCTAssertEqual(unchanged, first)
        do {
            _ = try await queue.enqueue(
                owner: fixture.owner, payloadData: Data("not yet durable".utf8))
            XCTFail("A failed durable enqueue must leave the new draft with its caller")
        } catch { XCTAssertFalse(error is AIOutgoingMessageQueueError) }
        let retained = try await queue.allEntries()
        XCTAssertEqual(retained, [first])
        XCTAssertEqual(
            try Data(contentsOf: savedParent.appendingPathComponent("outbox.json")), bytes)
        try FileManager.default.removeItem(at: parent)
        try FileManager.default.moveItem(at: savedParent, to: parent)
        let retry = try await queue.transition(
            id: first.id, expectedRevision: first.revision,
            to: .dispatch(AIOutgoingMessageQueueBinding(mode: .send, hostEpoch: "epoch-a")))
        XCTAssertEqual(retry.revision, first.revision + 1)
    }

    func testCapacityAndCorruptFilePreservePendingMessages() async throws {
        let fixture = try Fixture()
        defer { fixture.cleanup() }
        let queue = try fixture.queue(maximumEntries: 1)
        let first = try await queue.enqueue(owner: fixture.owner, payloadData: Data("keep".utf8))
        let bytes = try Data(contentsOf: fixture.file)
        do {
            _ = try await queue.enqueue(owner: fixture.owner, payloadData: Data("full".utf8))
            XCTFail("A full outbox must reject the new item rather than discard an old message")
        } catch { XCTAssertEqual(error as? AIOutgoingMessageQueueError, .capacityExceeded) }
        XCTAssertEqual(try Data(contentsOf: fixture.file), bytes)
        let head = try await queue.head(for: fixture.owner)
        XCTAssertEqual(head?.id, first.id)
        let malformed = Data("not json".utf8)
        try malformed.write(to: fixture.file, options: .atomic)
        let reopened = try fixture.queue()
        do {
            _ = try await reopened.load()
            XCTFail("A corrupt store cannot be replaced with an empty queue")
        } catch { XCTAssertEqual(error as? AIOutgoingMessageQueueError, .corruptStore) }
        XCTAssertEqual(try Data(contentsOf: fixture.file), malformed)
    }

    func testPermanentAccountInvalidationRejectsLateReceiptsWithoutRecreatingDeletedDirectory()
        async throws
    {
        let fixture = try Fixture()
        defer { fixture.cleanup() }
        let queue = try fixture.queue()
        let first = try await queue.enqueue(owner: fixture.owner, payloadData: Data("pending".utf8))
        let sending = try await queue.transition(
            id: first.id, expectedRevision: first.revision,
            to: .dispatch(AIOutgoingMessageQueueBinding(mode: .send, hostEpoch: "epoch-a")))
        let bytes = try Data(contentsOf: fixture.file)
        await queue.invalidatePermanently()
        await queue.invalidatePermanently()
        XCTAssertEqual(try Data(contentsOf: fixture.file), bytes)

        // 账号目录由宿主在 actor 屏障之后删除；此前已开始的 native 调用仍可能回传 accepted。
        let parent = fixture.file.deletingLastPathComponent()
        try FileManager.default.removeItem(at: parent)
        do {
            _ = try await queue.transition(
                id: sending.id, expectedRevision: sending.revision,
                to: .accepted(messageID: "late-native-receipt"))
            XCTFail("A late receipt cannot revive a permanently deleted account")
        } catch { XCTAssertEqual(error as? AIOutgoingMessageQueueError, .permanentlyInvalidated) }
        XCTAssertFalse(FileManager.default.fileExists(atPath: parent.path))
        do {
            _ = try await queue.enqueue(owner: fixture.owner, payloadData: Data("late intake".utf8))
            XCTFail("An old intake task cannot recreate the deleted account directory")
        } catch { XCTAssertEqual(error as? AIOutgoingMessageQueueError, .permanentlyInvalidated) }
        do {
            try await queue.updateHostEpoch("epoch-after-deletion")
            XCTFail("Reconfiguration cannot reactivate a permanently invalidated queue")
        } catch { XCTAssertEqual(error as? AIOutgoingMessageQueueError, .permanentlyInvalidated) }
        do {
            _ = try await queue.load()
            XCTFail("A previously loaded queue cannot expose its stale account snapshot")
        } catch { XCTAssertEqual(error as? AIOutgoingMessageQueueError, .permanentlyInvalidated) }
        XCTAssertFalse(FileManager.default.fileExists(atPath: parent.path))

        let neverLoaded = try fixture.queue()
        await neverLoaded.invalidatePermanently()
        do {
            _ = try await neverLoaded.load()
            XCTFail("An invalidated actor cannot load even when its file has already been deleted")
        } catch { XCTAssertEqual(error as? AIOutgoingMessageQueueError, .permanentlyInvalidated) }
        XCTAssertFalse(FileManager.default.fileExists(atPath: parent.path))
    }

    func testOrdinaryAccountSwitchPreservesQueueForLaterReopen() async throws {
        let fixture = try Fixture()
        defer { fixture.cleanup() }
        let queue = try fixture.queue()
        let first = try await queue.enqueue(
            owner: fixture.owner, payloadData: Data("retained".utf8))
        let sending = try await queue.transition(
            id: first.id, expectedRevision: first.revision,
            to: .dispatch(AIOutgoingMessageQueueBinding(mode: .send, hostEpoch: "epoch-a")))

        // 普通换号只停泵并切走宿主；原 actor 的真实收据仍可提交，重新进入时保留确认状态。
        try await queue.updateHostEpoch("epoch-after-switch")
        let accepted = try await queue.transition(
            id: sending.id, expectedRevision: sending.revision,
            to: .accepted(messageID: "receipt-after-switch"))
        let reopened = try fixture.queue(epoch: "epoch-reopened")
        let restored = try await reopened.load()
        XCTAssertEqual(restored, [accepted])
        XCTAssertTrue(FileManager.default.fileExists(atPath: fixture.file.path))
    }
}
