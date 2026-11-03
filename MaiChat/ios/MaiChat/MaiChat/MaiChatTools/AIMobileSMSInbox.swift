import Foundation

/// iOS 只保存用户通过快捷指令交入的消息，不访问“信息”收件箱。
/// 同一 actor 串行读写，账号不匹配时不返回内容；关闭接入同时清除消息。
actor AIMobileSMSInbox {
    static let shared = AIMobileSMSInbox(directory: FileManager.default.urls(
        for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("AgentSMS"))

    struct Message: Codable, Sendable {
        let id: UUID
        let sender: String
        let body: String
        let timestampMilliseconds: Int64
        let importedMilliseconds: Int64
        let timestampSource: String
    }

    struct Status: Sendable {
        let enabled: Bool
        let count: Int
    }

    enum InboxError: LocalizedError {
        case notEnabled, invalidInput, accountRequired
        var errorDescription: String? {
            switch self {
            case .notEnabled: "请在 MaiChat 的设置中开启短信接入，再配置快捷指令的消息自动化。"
            case .invalidInput: "短信参数无效，正文最多 4000 字，发送方最多 128 字。"
            case .accountRequired: "请先登录 MaiChat。"
            }
        }
    }

    private struct Contents: Codable {
        var ownerID: String
        var enabled: Bool
        var messages: [Message]
    }
    private let directory: URL
    private let clock: @Sendable () -> Date
    private var file: URL { directory.appendingPathComponent("inbox.json") }
    private let lifetimeMilliseconds: Int64 = 24 * 60 * 60 * 1000

    // 可注入独立目录与时钟，测试不接触正式短信或账号。
    init(directory: URL, clock: @escaping @Sendable () -> Date = { Date() }) {
        self.directory = directory
        self.clock = clock
    }

    func status(ownerID: String) throws -> Status {
        var contents = try load()
        guard !ownerID.isEmpty, contents.ownerID == ownerID else { return Status(enabled: false, count: 0) }
        let messages = recent(contents.messages)
        if messages.count != contents.messages.count {
            contents.messages = messages
            try save(contents)
        }
        return Status(enabled: contents.enabled, count: messages.count)
    }

    func setEnabled(_ enabled: Bool, ownerID: String) throws {
        guard !ownerID.isEmpty else { throw InboxError.accountRequired }
        let previous = try load()
        try save(Contents(ownerID: ownerID, enabled: enabled,
                          messages: enabled && previous.ownerID == ownerID ? recent(previous.messages) : []))
    }

    func clear(ownerID: String) throws {
        var contents = try load()
        guard contents.ownerID == ownerID else { throw InboxError.accountRequired }
        contents.messages = []
        try save(contents)
    }

    func disableAndClear() throws {
        if FileManager.default.fileExists(atPath: file.path) { try FileManager.default.removeItem(at: file) }
    }

    /// 普通 App 与 AppIntent 共用此入口；返回值只表示本机接入，不表示 Agent 已读或已处理。
    func importMessage(body: String, sender: String, receivedAt: Date?, ownerID: String) throws {
        guard !ownerID.isEmpty else { throw InboxError.accountRequired }
        guard !body.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty, body.count <= 4000,
              sender.count <= 128 else { throw InboxError.invalidInput }
        if let receivedAt {
            guard receivedAt.timeIntervalSince1970.isFinite, receivedAt.timeIntervalSince1970 >= 0,
                  receivedAt <= clock().addingTimeInterval(60) else { throw InboxError.invalidInput }
        }
        var contents = try load()
        guard contents.enabled, contents.ownerID == ownerID else { throw InboxError.notEnabled }
        let now = clock()
        let timestamp = receivedAt ?? now
        contents.messages = recent(contents.messages)
        contents.messages.insert(Message(id: UUID(), sender: sender, body: body,
                                         timestampMilliseconds: Int64(timestamp.timeIntervalSince1970 * 1000),
                                         importedMilliseconds: Int64(now.timeIntervalSince1970 * 1000),
                                         timestampSource: receivedAt == nil ? "imported" : "shortcut"), at: 0)
        contents.messages = Array(contents.messages.prefix(20))
        try save(contents)
    }

    func readRecent(ownerID: String, limit: Int = 10, afterMilliseconds: Int64 = 0) throws -> [Message] {
        guard (1...20).contains(limit), afterMilliseconds >= 0 else { throw InboxError.invalidInput }
        var contents = try load()
        guard contents.enabled, !ownerID.isEmpty, contents.ownerID == ownerID else { throw InboxError.notEnabled }
        contents.messages = recent(contents.messages)
        // 下次访问时从磁盘清理过期短信，过期正文不再进入 Agent 上下文。
        try save(contents)
        return Array(contents.messages.filter { $0.timestampMilliseconds >= afterMilliseconds }
            .sorted { $0.timestampMilliseconds > $1.timestampMilliseconds }.prefix(limit))
    }

    private func recent(_ messages: [Message]) -> [Message] {
        let earliest = Int64(clock().timeIntervalSince1970 * 1000) - lifetimeMilliseconds
        return messages.filter { $0.importedMilliseconds > earliest }
    }

    private func load() throws -> Contents {
        guard FileManager.default.fileExists(atPath: file.path) else {
            return Contents(ownerID: "", enabled: false, messages: [])
        }
        return try JSONDecoder().decode(Contents.self, from: Data(contentsOf: file))
    }

    private func save(_ contents: Contents) throws {
        // 不进云盘、备份或诊断日志；首次解锁后的后台快捷指令可以交入新短信。
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true,
                                               attributes: [.protectionKey: FileProtectionType.completeUntilFirstUserAuthentication])
        var localDirectory = directory
        var values = URLResourceValues()
        values.isExcludedFromBackup = true
        try localDirectory.setResourceValues(values)
        let data = try JSONEncoder().encode(contents)
        try data.write(to: file, options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])
    }
}
