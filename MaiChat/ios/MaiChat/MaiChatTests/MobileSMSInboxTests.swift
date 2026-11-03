import Foundation
import XCTest

/// 合成短信和独立临时目录；不读取正式账号、系统短信或真实验证码。
final class MobileSMSInboxTests: XCTestCase {
    private func directory() throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("sms-test-" + UUID().uuidString)
        addTeardownBlock { try? FileManager.default.removeItem(at: url) }
        return url
    }

    func testDisabledAndOtherAccountCannotImportOrRead() async throws {
        let inbox = AIMobileSMSInbox(directory: try directory())
        do {
            try await inbox.importMessage(body: "Synthetic message", sender: "fixture", receivedAt: nil, ownerID: "owner-A")
            XCTFail("disabled import succeeded")
        } catch AIMobileSMSInbox.InboxError.notEnabled { }
        try await inbox.setEnabled(true, ownerID: "owner-A")
        try await inbox.importMessage(body: "Synthetic message", sender: "fixture", receivedAt: nil, ownerID: "owner-A")
        do {
            _ = try await inbox.readRecent(ownerID: "owner-B")
            XCTFail("another account read imported SMS")
        } catch AIMobileSMSInbox.InboxError.notEnabled { }
        do {
            try await inbox.importMessage(body: "Wrong account", sender: "fixture", receivedAt: nil, ownerID: "owner-B")
            XCTFail("another account imported SMS")
        } catch AIMobileSMSInbox.InboxError.notEnabled { }
        let status = try await inbox.status(ownerID: "owner-B")
        XCTAssertFalse(status.enabled)
        XCTAssertEqual(status.count, 0)
    }

    func testBoundedReadFilterAndPersistence() async throws {
        let root = try directory()
        let now = Date(timeIntervalSince1970: 1_700_000_000)
        let inbox = AIMobileSMSInbox(directory: root, clock: { now })
        try await inbox.setEnabled(true, ownerID: "owner-A")
        for i in 0..<25 {
            try await inbox.importMessage(body: "Synthetic message \(i)", sender: "fixture",
                                          receivedAt: now.addingTimeInterval(Double(i - 25)), ownerID: "owner-A")
        }
        let reloaded = AIMobileSMSInbox(directory: root, clock: { now })
        let status = try await reloaded.status(ownerID: "owner-A")
        XCTAssertEqual(status.count, 20)
        let limited = try await reloaded.readRecent(ownerID: "owner-A", limit: 2)
        XCTAssertEqual(limited.map(\.body), ["Synthetic message 24", "Synthetic message 23"])
        let filtered = try await reloaded.readRecent(ownerID: "owner-A", limit: 20,
                                                     afterMilliseconds: Int64(now.timeIntervalSince1970 * 1000) - 2000)
        XCTAssertEqual(filtered.count, 2)
        XCTAssertEqual(filtered.first?.timestampSource, "shortcut")
        let excluded = try root.resourceValues(forKeys: [.isExcludedFromBackupKey])
        XCTAssertEqual(excluded.isExcludedFromBackup, true)
    }

    func testExpirationDisableAndAccountChangeClearMessages() async throws {
        let root = try directory()
        let now = Date(timeIntervalSince1970: 1_700_000_000)
        let inbox = AIMobileSMSInbox(directory: root, clock: { now })
        try await inbox.setEnabled(true, ownerID: "owner-A")
        try await inbox.importMessage(body: "Expiring synthetic message", sender: "", receivedAt: nil, ownerID: "owner-A")
        let later = AIMobileSMSInbox(directory: root, clock: { now.addingTimeInterval(86401) })
        let expired = try await later.readRecent(ownerID: "owner-A")
        XCTAssertTrue(expired.isEmpty)
        try await inbox.importMessage(body: "Clear synthetic message", sender: "", receivedAt: nil, ownerID: "owner-A")
        try await inbox.setEnabled(false, ownerID: "owner-A")
        try await inbox.setEnabled(true, ownerID: "owner-A")
        let disabled = try await inbox.readRecent(ownerID: "owner-A")
        XCTAssertTrue(disabled.isEmpty)
        try await inbox.importMessage(body: "Account synthetic message", sender: "", receivedAt: nil, ownerID: "owner-A")
        try await inbox.setEnabled(true, ownerID: "owner-B")
        let changed = try await inbox.readRecent(ownerID: "owner-B")
        XCTAssertTrue(changed.isEmpty)
        try await inbox.disableAndClear()
        XCTAssertFalse(FileManager.default.fileExists(atPath: root.appendingPathComponent("inbox.json").path))
    }

    func testInvalidValuesAndImportedTimestampAreExplicit() async throws {
        let inbox = AIMobileSMSInbox(directory: try directory())
        try await inbox.setEnabled(true, ownerID: "owner-A")
        do {
            try await inbox.importMessage(body: " ", sender: "", receivedAt: nil, ownerID: "owner-A")
            XCTFail("empty body accepted")
        } catch AIMobileSMSInbox.InboxError.invalidInput { }
        do {
            _ = try await inbox.readRecent(ownerID: "owner-A", limit: 21)
            XCTFail("unbounded read accepted")
        } catch AIMobileSMSInbox.InboxError.invalidInput { }
        try await inbox.importMessage(body: "Synthetic handoff", sender: "", receivedAt: nil, ownerID: "owner-A")
        let messages = try await inbox.readRecent(ownerID: "owner-A")
        XCTAssertEqual(messages.first?.timestampSource, "imported")
        XCTAssertEqual(messages.first?.sender, "", "missing sender must not be invented")
    }
}
