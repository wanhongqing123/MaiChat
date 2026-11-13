// 文件作用：用本地 SQLite 保存 IM 消息、会话索引和同步游标，支持离线读取。
// 阅读边界：持久化顺序和去重在此实现；网络消息由 IM 客户端获取。
// 使用 SQLite 保存消息、附件元数据、会话摘要和搜索索引。
import Foundation
import MaiChatCore
import SQLite3
import CryptoKit

struct LocalChatMessageQueryError: LocalizedError, Sendable {
    let code: String
    let message: String
    var errorDescription: String? { message }
    static func invalid(_ message: String) -> Self { Self(code: "invalid_input", message: message) }
    static let unavailable = Self(code: "not_configured", message: "Local persisted IM database is unavailable.")
    static let changed = Self(code: "canceled", message: "The active IM account or database changed.")
}

struct LocalChatMessageFilters: Codable, Equatable, Sendable {
    var query: String? = nil
    var peerID = ""
    var kind = ""
    var direction = ""
    var sinceMS: Int64? = nil
    var untilMS: Int64? = nil

    func normalized() throws -> Self {
        var value = self
        value.query = query?.trimmingCharacters(in: .whitespacesAndNewlines)
        value.peerID = peerID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard value.query == nil || value.query?.isEmpty == false,
              (value.query?.utf8.count ?? 0) <= 8192, value.peerID.utf8.count <= 128,
              ["", "text", "image", "video", "voice", "file"].contains(kind),
              ["", "incoming", "outgoing"].contains(direction),
              sinceMS == nil || untilMS == nil || sinceMS! <= untilMS! else {
            throw LocalChatMessageQueryError.invalid("Invalid local IM query filters.")
        }
        guard (try JSONEncoder().encode(value)).count <= 16000 else {
            throw LocalChatMessageQueryError.invalid("Local IM query filters exceed the continuation budget.")
        }
        return value
    }
}

struct LocalChatMessageQuery: Sendable {
    var filters = LocalChatMessageFilters()
    var limit = 50
    var cursor: String? = nil
    var deadlineUptimeNanoseconds: UInt64? = nil
}

struct LocalChatMessageQueryHit: Sendable {
    let id: String
    let peerUserID: String
    let senderID: String
    let text: String
    let kind: String
    let direction: String
    let createdAtMilliseconds: Int64
    var storedID: String { id }
}

struct LocalChatMessageQueryPage: Sendable {
    let hits: [LocalChatMessageQueryHit]
    let limit: Int
    let hasMore: Bool
    let nextCursor: String?
    let isFirstPage: Bool
}

struct LocalChatMessageQueryIdentity: Equatable, Sendable {
    let sdk: Int?
    let owner: String
    let generation: UInt64
    let authenticated: Bool
}

// Main 发布账号身份，native 在排队前取得 owned 租约；后台回调不读取 MainActor 状态。
// 每次身份/代际变更都废弃旧租约，即使 A→B→A，也不能重新接受旧 A 的调用。
final class LocalChatMessageQueryAdmission: @unchecked Sendable {
    struct Lease: Sendable {
        fileprivate let source: LocalChatMessageQueryAdmission
        fileprivate let revision: UInt64
        let identity: LocalChatMessageQueryIdentity
        var isCurrent: Bool { source.accepts(self) }
        func belongs(to admission: LocalChatMessageQueryAdmission) -> Bool { source === admission }
    }

    private let lock = NSLock()
    private var identity: LocalChatMessageQueryIdentity?
    private var revision: UInt64 = 0

    func update(_ identity: LocalChatMessageQueryIdentity) {
        lock.lock()
        if self.identity != identity { revision &+= 1; self.identity = identity }
        lock.unlock()
    }

    func invalidate() {
        lock.lock(); revision &+= 1; identity = nil; lock.unlock()
    }

    // 共享 Backend 被替换/释放时保留当前账号，但废弃该 bridge 尚未交付的租约。
    func advanceRevision() {
        lock.lock(); revision &+= 1; lock.unlock()
    }

    func capture() -> Lease? {
        lock.lock(); defer { lock.unlock() }
        guard let identity, identity.authenticated, !identity.owner.isEmpty else { return nil }
        return Lease(source: self, revision: revision, identity: identity)
    }

    private func accepts(_ lease: Lease) -> Bool {
        lock.lock(); defer { lock.unlock() }
        return revision == lease.revision && identity == lease.identity
    }
}

private struct LocalChatMessagePageCursor: Codable {
    let version: Int
    let database: String
    let sdk: String
    let owner: String
    let generation: UInt64
    let filters: LocalChatMessageFilters
    let milliseconds: Int64
    let id: String
}

// 索引和查询共用毫秒/对端表达式，保持 wire 时间、同毫秒 ID 排序与旧 REAL 秒存储一致。
enum LocalChatMessageSQL {
    static let milliseconds = "CAST(ROUND(created_at * 1000.0) AS INTEGER)"
    static let peer = "CASE WHEN peer_user_id IS NULL OR peer_user_id='' THEN CASE WHEN from_user=owner_user_id THEN to_user ELSE from_user END ELSE peer_user_id END"
    static let kind = "CASE WHEN image_attachment IS NOT NULL AND image_attachment<>'' AND image_attachment<>'null' THEN 'image' WHEN video_attachment IS NOT NULL AND video_attachment<>'' AND video_attachment<>'null' THEN 'video' WHEN voice_attachment IS NOT NULL AND voice_attachment<>'' AND voice_attachment<>'null' THEN 'voice' WHEN file_attachment IS NOT NULL AND file_attachment<>'' AND file_attachment<>'null' THEN 'file' ELSE 'text' END"
}

fileprivate final class LocalChatMessageQueryControl: @unchecked Sendable {
    let deadline: UInt64
    private let lock = NSLock()
    private var canceled = false
    init(deadline: UInt64? = nil) {
        self.deadline = min(deadline ?? UInt64.max, DispatchTime.now().uptimeNanoseconds + 5_000_000_000)
    }
    func cancel() { lock.lock(); canceled = true; lock.unlock() }
    func check() throws {
        lock.lock(); let value = canceled; lock.unlock()
        if value { throw CancellationError() }
        if DispatchTime.now().uptimeNanoseconds >= deadline {
            throw LocalChatMessageQueryError(code: "busy", message: "Local IM query exceeded its five-second deadline.")
        }
    }
    func interrupted() -> Bool { (try? check()) == nil }
}

// 排队、取消、SQL 运行共用一个截止时间；完成只交付一次，过期任务不阻塞调用方。
private final class LocalChatMessageQueryCompletion: @unchecked Sendable {
    private let lock = NSLock()
    private var continuation: CheckedContinuation<LocalChatMessageQueryPage, Error>?
    private var pending: Result<LocalChatMessageQueryPage, Error>?
    private var finished = false
    func attach(_ value: CheckedContinuation<LocalChatMessageQueryPage, Error>) {
        lock.lock()
        if let pending { self.pending = nil; lock.unlock(); value.resume(with: pending) }
        else { continuation = value; lock.unlock() }
    }
    func finish(_ result: Result<LocalChatMessageQueryPage, Error>) {
        lock.lock()
        guard !finished else { lock.unlock(); return }
        finished = true
        let callback = continuation
        continuation = nil
        if callback == nil { pending = result }
        lock.unlock()
        callback?.resume(with: result)
    }
}

// 独立的两个只读 worker，不占用写入 actor；每次只读 handle/decoder 均由本任务独占。
private final class LocalChatMessageQueryReader: @unchecked Sendable {
    private let directory: URL
    private let instance = UUID().uuidString
    private let queue = OperationQueue()
    private let lock = NSLock()
    private var admitted = 0
    init(directory: URL) {
        self.directory = directory
        queue.name = "MaiChat.IMDatabaseQuery"
        queue.maxConcurrentOperationCount = 2
        queue.qualityOfService = .userInitiated
    }
    func page(sdkAppID: Int?, owner: String, generation: UInt64,
              request: LocalChatMessageQuery) async throws -> LocalChatMessageQueryPage {
        let control = LocalChatMessageQueryControl(deadline: request.deadlineUptimeNanoseconds)
        let completion = LocalChatMessageQueryCompletion()
        return try await withTaskCancellationHandler {
            try Task.checkCancellation()
            return try await withCheckedThrowingContinuation { continuation in
                completion.attach(continuation)
                lock.lock()
                guard admitted < 32 else {
                    lock.unlock()
                    completion.finish(.failure(LocalChatMessageQueryError(code: "busy", message: "Local IM query workers are busy.")))
                    return
                }
                admitted += 1
                lock.unlock()
                DispatchQueue.global(qos: .userInitiated).asyncAfter(deadline: DispatchTime(uptimeNanoseconds: control.deadline)) {
                    do { try control.check() } catch { completion.finish(.failure(error)) }
                }
                queue.addOperation { [self] in
                    defer { lock.lock(); admitted -= 1; lock.unlock() }
                    completion.finish(Result {
                        try control.check()
                        let store = LocalChatHistoryStore(baseDirectoryURL: directory)
                        return try store.readMessageQuery(sdkAppID: sdkAppID, ownerUserID: owner,
                            generation: generation, instance: instance, request: request, control: control)
                    })
                }
            }
        } onCancel: {
            control.cancel()
            completion.finish(.failure(CancellationError()))
        }
    }
}

/// 单条后台音频线程还维护运行循环，以接收 AVFoundation 和 SDK 回调。
final class RemoteIMAudioWorkQueue: @unchecked Sendable {
    private final class State: @unchecked Sendable {
        let lock = NSLock()
        var loop: CFRunLoop?
        var pending: [@Sendable () -> Void] = []
        func enqueue(_ work: @escaping @Sendable () -> Void) {
            lock.lock()
            guard let loop else { pending.append(work); lock.unlock(); return }
            CFRunLoopPerformBlock(loop, CFRunLoopMode.commonModes.rawValue, work)
            lock.unlock()
            CFRunLoopWakeUp(loop)
        }
        func run() {
            Thread.current.threadDictionary["MaiChat.AudioWorker"] = true
            let port = Port()
            RunLoop.current.add(port, forMode: .default)
            let loop = CFRunLoopGetCurrent()!
            lock.lock()
            self.loop = loop
            for work in pending { CFRunLoopPerformBlock(loop, CFRunLoopMode.commonModes.rawValue, work) }
            pending.removeAll()
            lock.unlock()
            CFRunLoopRun()
        }
    }
    private let state = State()
    private let thread: Thread
    init() {
        let state = state
        thread = Thread { state.run() }
        thread.name = "MaiChat.AudioWorker"
        thread.qualityOfService = .userInitiated
        thread.start()
    }
    func async(_ work: @escaping @Sendable () -> Void) { state.enqueue(work) }
    func assertCurrent() { precondition(Thread.current.threadDictionary["MaiChat.AudioWorker"] as? Bool == true) }
}

/// 有界工作队列隔离 CPU 解析和文件 I/O，不占用主执行器。
enum RemoteIMBackgroundWork {
    static let audioQueue = RemoteIMAudioWorkQueue()
    private static let fileQueue = DispatchQueue(label: "MaiChat.FileWork", qos: .userInitiated)
    private static let metadataQueue = DispatchQueue(label: "MaiChat.FileMetadata", qos: .userInitiated)
    private static let parsingQueue = DispatchQueue(label: "MaiChat.ParsingWork", qos: .userInitiated)

    final class Cancellation: @unchecked Sendable {
        private let lock = NSLock()
        private var cancelled = false
        func cancel() { lock.lock(); cancelled = true; lock.unlock() }
        func check() throws {
            lock.lock(); let value = cancelled; lock.unlock()
            if value { throw CancellationError() }
        }
    }

    static func audio<T: Sendable>(_ operation: @escaping @Sendable () throws -> T) async throws -> T {
        try Task.checkCancellation()
        let cancellation = Cancellation()
        let result: T = try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { continuation in
                audioQueue.async {
                    continuation.resume(with: Result {
                        try cancellation.check()
                        return try autoreleasepool(invoking: operation)
                    })
                }
            }
        } onCancel: { cancellation.cancel() }
        try Task.checkCancellation()
        return result
    }
    static func file<T: Sendable>(_ operation: @escaping @Sendable () throws -> T) async throws -> T {
        try await submit(to: fileQueue) { _ in try operation() }
    }
    static func metadata<T: Sendable>(_ operation: @escaping @Sendable () throws -> T) async throws -> T {
        try await submit(to: metadataQueue) { _ in try operation() }
    }
    static func fileCancellable<T: Sendable>(_ operation: @escaping @Sendable (Cancellation) throws -> T) async throws
        -> T
    { try await submit(to: fileQueue, operation)
    }
    static func parse<T: Sendable>(_ operation: @escaping @Sendable () throws -> T) async throws -> T {
        try await submit(to: parsingQueue) { _ in try operation() }
    }
    static func parseCancellable<T: Sendable>(_ operation: @escaping @Sendable (Cancellation) throws -> T) async throws
        -> T
    { try await submit(to: parsingQueue, operation)
    }

    private static func submit<T: Sendable>(to queue: DispatchQueue,
        _ operation: @escaping @Sendable (Cancellation) throws -> T) async throws -> T {
        try Task.checkCancellation()
        let cancellation = Cancellation()
        let result: T = try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { continuation in
                queue.async {
                    dispatchPrecondition(condition: .notOnQueue(.main))
                    continuation.resume(with: Result {
                        try cancellation.check()
                        return try autoreleasepool { try operation(cancellation) }
                    })
                }
            }
        } onCancel: { cancellation.cancel() }
        try Task.checkCancellation()
        return result
    }
}

enum RemoteIMMediaStorage {
    enum Category: String, CaseIterable {
        case incomingImages = "Incoming/Images"
        case outgoingImages = "Outgoing/Images"
        case incomingVideos = "Incoming/Videos"
        case outgoingVideos = "Outgoing/Videos"
        case incomingVideoCovers = "Incoming/VideoCovers"
        case outgoingVideoCovers = "Outgoing/VideoCovers"
        case incomingVoices = "Incoming/Voices"
        case outgoingVoices = "Outgoing/Voices"
        case incomingFiles = "Incoming/Files"
        case outgoingFiles = "Outgoing/Files"
    }

    static func fileURL(
        category: Category,
        stem: String,
        pathExtension: String
    ) -> URL {
        let cleanStem = sanitizedFileName(stem).trimmingCharacters(in: CharacterSet(charactersIn: "."))
        let cleanExtension = pathExtension
            .trimmingCharacters(in: .whitespacesAndNewlines)
            .trimmingCharacters(in: CharacterSet(charactersIn: "."))
        let fileName = cleanExtension.isEmpty
            ? (cleanStem.isEmpty ? UUID().uuidString : cleanStem)
            : "\(cleanStem.isEmpty ? UUID().uuidString : cleanStem).\(cleanExtension)"
        return fileURL(category: category, fileName: fileName)
    }

    static func fileURL(category: Category, fileName: String) -> URL {
        let directory = directoryURL(category: category)
        return directory.appendingPathComponent(sanitizedFileName(fileName))
    }

    static func persistentReference(for runtimePath: String, category: Category) -> String {
        let cleanPath = runtimePath.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanPath.isEmpty else { return "" }

        let sourceURL = URL(fileURLWithPath: cleanPath)
        let mediaRootPath = mediaRootURL().standardizedFileURL.path
        let standardizedPath = sourceURL.standardizedFileURL.path
        if standardizedPath.hasPrefix(mediaRootPath + "/") {
            let relativePath = String(standardizedPath.dropFirst(mediaRootPath.count + 1))
            return relativePath.hasPrefix(category.rawValue + "/") ? relativePath : ""
        }
        return ""
    }

    static func resolvedPath(from storedPath: String, category: Category) -> String {
        let cleanPath = storedPath.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanPath.isEmpty else { return "" }
        guard !cleanPath.hasPrefix("/"), cleanPath.hasPrefix(category.rawValue + "/") else {
            return ""
        }
        let resolvedURL = mediaRootURL().appendingPathComponent(cleanPath).standardizedFileURL
        let categoryRoot = directoryURL(category: category).standardizedFileURL.path
        guard resolvedURL.path.hasPrefix(categoryRoot + "/") else { return "" }
        return resolvedURL.path
    }

    private static func mediaRootURL() -> URL {
        let appSupport = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        let root = appSupport
            .appendingPathComponent("MaiChat", isDirectory: true)
            .appendingPathComponent("RemoteIMMedia", isDirectory: true)
        return root
    }

    private static func directoryURL(category: Category) -> URL {
        let directory = category.rawValue
            .split(separator: "/")
            .reduce(mediaRootURL()) { partialURL, component in
                partialURL.appendingPathComponent(String(component), isDirectory: true)
            }
        return directory
    }

    static func prepareDirectories() throws {
        let root = mediaRootURL()
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        var excludedRoot = root
        var values = URLResourceValues()
        values.isExcludedFromBackup = true
        try excludedRoot.setResourceValues(values)
        for category in Category.allCases {
            try FileManager.default.createDirectory(
                at: directoryURL(category: category), withIntermediateDirectories: true)
        }
    }

    private static func sanitizedFileName(_ value: String) -> String {
        let clean = value.trimmingCharacters(in: .whitespacesAndNewlines)
        let fallback = clean.isEmpty ? UUID().uuidString : clean
        return fallback
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: ":", with: "_")
    }
}

struct LocalChatHistoryAccount: Hashable, Sendable {
    let sdkAppID: Int?
    let ownerUserID: String
}

struct LocalChatHistorySaveResult: Sendable, Equatable {
    let messageCount: Int
    let upsertedCount: Int
    let removedCount: Int
    let durationMilliseconds: Int
}

struct LocalChatHistoryCursor: Sendable, Equatable {
    let createdAt: Date
    let messageID: UUID
}

struct LocalChatHistoryPage: Sendable, Equatable {
    let messages: [RemoteIMMessage]
    let hasEarlierMessages: Bool
}

struct LocalChatHistoryGroup: Sendable, Equatable {
    let groupID: String
    let name: String
    let memberCount: Int
    let pendingActivation: Bool

    init(groupID: String, name: String, memberCount: Int, pendingActivation: Bool = false) {
        self.groupID = groupID
        self.name = name
        self.memberCount = memberCount
        self.pendingActivation = pendingActivation
    }
}

struct LocalChatHistorySearchHit: Identifiable, Sendable, Equatable {
    var id: UUID { message.id }
    let peerUserID: String
    let message: RemoteIMMessage
}

enum LocalChatHistoryMutationOperation: Sendable {
    case upsert([RemoteIMMessage])
    case removeConversation(peerUserID: String)
    case removeMessage(id: UUID)
}

struct LocalChatHistoryMutation: Sendable {
    let account: LocalChatHistoryAccount
    let operation: LocalChatHistoryMutationOperation

    static func upsert(
        _ messages: [RemoteIMMessage],
        sdkAppID: Int?,
        ownerUserID: String
    ) -> Self {
        Self(
            account: LocalChatHistoryAccount(
                sdkAppID: sdkAppID,
                ownerUserID: ownerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
            ),
            operation: .upsert(messages)
        )
    }

    static func removeConversation(
        peerUserID: String,
        sdkAppID: Int?,
        ownerUserID: String
    ) -> Self {
        Self(
            account: LocalChatHistoryAccount(
                sdkAppID: sdkAppID,
                ownerUserID: ownerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
            ),
            operation: .removeConversation(
                peerUserID: peerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
            )
        )
    }

    static func removeMessage(id: UUID, sdkAppID: Int?, ownerUserID: String) -> Self {
        Self(
            account: LocalChatHistoryAccount(
                sdkAppID: sdkAppID,
                ownerUserID: ownerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
            ),
            operation: .removeMessage(id: id)
        )
    }
}

struct LocalChatIncomingMetadataResult: Sendable {
    let message: RemoteIMMessage
    let inserted: Bool
}

final class LocalChatHistoryStore {
    private struct StoredChatHistory: Codable {
        let schemaVersion: Int
        let sdkAppID: Int?
        let ownerUserID: String
        let messages: [RemoteIMMessage]
    }

    private enum StoreError: LocalizedError {
        case database(String)

        var errorDescription: String? {
            switch self {
            case .database(let message):
                return "保存本地消息失败：\(message)"
            }
        }
    }

    private let baseDirectoryURL: URL
    private let fileManager: FileManager
    private let encoder: JSONEncoder
    private let decoder: JSONDecoder

    init(
        baseDirectoryURL: URL? = nil,
        fileManager: FileManager = .default,
        encoder: JSONEncoder = JSONEncoder(),
        decoder: JSONDecoder = JSONDecoder()
    ) {
        self.fileManager = fileManager
        self.encoder = encoder
        self.decoder = decoder
        self.baseDirectoryURL = baseDirectoryURL ?? Self.defaultBaseDirectoryURL(fileManager: fileManager)
    }

    func prepareDatabase() throws { try withDatabase { _ in () } }

    // 可信来源路径只由普通数据库 owner 提供；导入 reader 不接收模型传来的任意文件路径。
    var persistentDatabasePath: String { databaseURL.path }

    /// 同一写事务完成 remote/local ID 的实际成员检查与首次插入；UI 工作集不能代替数据库回执。
    func insertIncomingMetadata(_ message: RemoteIMMessage, sdkAppID: Int?, ownerUserID: String)
        throws -> LocalChatIncomingMetadataResult {
        guard message.direction == .incoming, message.groupID == nil,
              message.toUserID == ownerUserID, !ownerUserID.isEmpty,
              !message.fromUserID.isEmpty, message.fromUserID != ownerUserID else {
            throw LocalChatMessageQueryError.invalid("Incoming metadata does not match this account.")
        }
        return try withDatabase { database in
            try execute(database, sql: "BEGIN IMMEDIATE TRANSACTION")
            do {
                let hasRemote = message.remoteID?.isEmpty == false
                let statement = try prepare(database, sql: """
                    SELECT id,remote_id,from_user,to_user,text,direction,status,created_at,
                           voice_attachment,image_attachment,file_attachment,video_attachment,
                           approval_request,approval_decision,caption_above,quote,media_caption,group_id
                    FROM messages WHERE sdk_app_id=? AND owner_user_id=? AND group_id IS NULL
                      AND \(hasRemote ? "remote_id=?" : "id=?") LIMIT 1
                    """)
                defer { sqlite3_finalize(statement) }
                try bindText(accountKey(for: sdkAppID), to: statement, at: 1, database: database)
                try bindText(ownerUserID, to: statement, at: 2, database: database)
                try bindText(hasRemote ? message.remoteID! : message.id.uuidString, to: statement, at: 3, database: database)
                let step = sqlite3_step(statement)
                if step == SQLITE_ROW {
                    guard let stored = decodeMessage(from: statement), stored.fromUserID == message.fromUserID,
                          stored.toUserID == ownerUserID else { throw databaseError(database) }
                    try execute(database, sql: "COMMIT")
                    return LocalChatIncomingMetadataResult(message: stored, inserted: false)
                }
                guard step == SQLITE_DONE else { throw databaseError(database) }
                try insertMessages([message], into: database, sdkAppID: sdkAppID, ownerUserID: ownerUserID, updateExisting: false)
                guard sqlite3_changes(database) == 1 else {
                    throw LocalChatMessageQueryError.invalid("Incoming local message ID conflicts with a stored row.")
                }
                try execute(database, sql: "COMMIT")
                return LocalChatIncomingMetadataResult(message: message, inserted: true)
            } catch { try? execute(database, sql: "ROLLBACK"); throw error }
        }
    }

    func makePersistence() -> LocalChatHistoryPersistence {
        LocalChatHistoryPersistence(baseDirectoryURL: baseDirectoryURL)
    }

    // 不走会创建/迁移数据库的 withDatabase；仅读取已经由普通持久化路径初始化的库。
    // WAL 允许查询与消息写入并行；毫秒表达式索引保证分页无需全表排序。
    fileprivate func readMessageQuery(sdkAppID: Int?, ownerUserID: String, generation: UInt64,
        instance: String, request: LocalChatMessageQuery, control: LocalChatMessageQueryControl)
        throws -> LocalChatMessageQueryPage {
        try control.check()
        let filters = try request.filters.normalized()
        let limit = min(max(request.limit, 1), 200)
        let owner = ownerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !owner.isEmpty else { throw LocalChatMessageQueryError.unavailable }
        func identity() throws -> String {
            guard let attributes = try? fileManager.attributesOfItem(atPath: databaseURL.path),
                  attributes[.type] as? FileAttributeType == .typeRegular,
                  let inode = attributes[.systemFileNumber] as? NSNumber,
                  let device = attributes[.systemNumber] as? NSNumber else {
                throw LocalChatMessageQueryError.unavailable
            }
            let raw = "\(instance)|\(databaseURL.standardizedFileURL.path)|\(device)|\(inode)"
            return SHA256.hash(data: Data(raw.utf8)).map { String(format: "%02x", $0) }.joined()
        }
        let databaseIdentity = try identity()
        let sdk = accountKey(for: sdkAppID)
        var cursor: LocalChatMessagePageCursor?
        if let raw = request.cursor, !raw.isEmpty {
            guard raw.utf8.count <= 32768, let bytes = Data(base64Encoded: raw),
                  let decoded = try? JSONDecoder().decode(LocalChatMessagePageCursor.self, from: bytes),
                  decoded.version == 1, decoded.database == databaseIdentity, decoded.sdk == sdk,
                  decoded.owner == owner, decoded.generation == generation, decoded.filters == filters,
                  !decoded.id.isEmpty && decoded.id.utf8.count <= 128 else {
                throw LocalChatMessageQueryError.invalid("The IM cursor does not match this account, database or query.")
            }
            cursor = decoded
        }
        var handle: OpaquePointer?
        guard sqlite3_open_v2(databaseURL.path, &handle,
            SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nil) == SQLITE_OK, let database = handle else {
            if let handle { sqlite3_close(handle) }
            throw LocalChatMessageQueryError.unavailable
        }
        defer { sqlite3_close(database) }
        sqlite3_busy_timeout(database, 150)
        sqlite3_progress_handler(database, 1000, { context in
            guard let context else { return 1 }
            return Unmanaged<LocalChatMessageQueryControl>.fromOpaque(context).takeUnretainedValue().interrupted() ? 1 : 0
        }, Unmanaged.passUnretained(control).toOpaque())
        defer { sqlite3_progress_handler(database, 0, nil, nil) }
        do {
            let journal = try prepare(database, sql: "PRAGMA journal_mode")
            let journalReady = sqlite3_step(journal) == SQLITE_ROW && textColumn(journal, at: 0) == "wal"
            sqlite3_finalize(journal)
            guard journalReady else { throw LocalChatMessageQueryError.unavailable }
            let index = try prepare(database, sql: "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name IN ('idx_im_query_ms','idx_im_query_peer_ms')")
            let indexed = sqlite3_step(index) == SQLITE_ROW && sqlite3_column_int(index, 0) == 2
            sqlite3_finalize(index)
            guard indexed else { throw LocalChatMessageQueryError.unavailable }

            let ms = LocalChatMessageSQL.milliseconds
            let peer = LocalChatMessageSQL.peer
            let kind = LocalChatMessageSQL.kind
            var whereSQL = ["sdk_app_id=?", "owner_user_id=?", "group_id IS NULL"]
            enum Binding { case text(String), integer(Int64) }
            var bindings: [Binding] = [.text(sdk), .text(owner)]
            if let query = filters.query {
                whereSQL.append("instr(lower(text),lower(?))>0"); bindings.append(.text(query))
            }
            if !filters.peerID.isEmpty { whereSQL.append("(\(peer))=?"); bindings.append(.text(filters.peerID)) }
            if !filters.kind.isEmpty { whereSQL.append("(\(kind))=?"); bindings.append(.text(filters.kind)) }
            if !filters.direction.isEmpty { whereSQL.append("direction=?"); bindings.append(.text(filters.direction)) }
            if let since = filters.sinceMS { whereSQL.append("\(ms)>=?"); bindings.append(.integer(since)) }
            if let until = filters.untilMS { whereSQL.append("\(ms)<=?"); bindings.append(.integer(until)) }
            if let cursor {
                whereSQL.append("(\(ms),id)<(?,?)")
                bindings += [.integer(cursor.milliseconds), .text(cursor.id)]
            }
            bindings.append(.integer(Int64(limit + 1)))
            let sql = """
                SELECT id,\(peer),from_user,text,\(kind),direction,\(ms)
                FROM messages WHERE \(whereSQL.joined(separator: " AND "))
                ORDER BY \(ms) DESC,id DESC LIMIT ?
                """
            let statement = try prepare(database, sql: sql)
            defer { sqlite3_finalize(statement) }
            for (offset, binding) in bindings.enumerated() {
                let position = Int32(offset + 1)
                switch binding {
                case .text(let value):
                    guard value.utf8.count <= Int(Int32.max) else { throw LocalChatMessageQueryError.unavailable }
                    let status = value.withCString { pointer in
                        sqlite3_bind_text(statement, position, pointer, Int32(value.utf8.count),
                                          unsafeBitCast(-1, to: sqlite3_destructor_type.self))
                    }
                    guard status == SQLITE_OK else { throw LocalChatMessageQueryError.unavailable }
                case .integer(let value):
                    guard sqlite3_bind_int64(statement, position, value) == SQLITE_OK else {
                        throw LocalChatMessageQueryError.unavailable
                    }
                }
            }
            var hits: [LocalChatMessageQueryHit] = []
            func field(_ column: Int32) throws -> String {
                let count = Int(sqlite3_column_bytes(statement, column))
                if count == 0 { return "" }
                guard let pointer = sqlite3_column_text(statement, column),
                      let text = String(bytes: UnsafeBufferPointer(start: pointer, count: count), encoding: .utf8) else {
                    throw LocalChatMessageQueryError(code: "internal", message: "Invalid UTF-8 in persisted IM data.")
                }
                return text
            }
            while true {
                try control.check()
                let result = sqlite3_step(statement)
                if result == SQLITE_DONE { break }
                guard result == SQLITE_ROW else {
                    try control.check()
                    throw LocalChatMessageQueryError(code: result == SQLITE_BUSY || result == SQLITE_LOCKED ? "busy" : "internal",
                                                    message: "The local IM database could not finish this query.")
                }
                let id = try field(0)
                let peerID = try field(1)
                let direction = try field(5)
                guard !id.isEmpty, id.utf8.count <= 128, !peerID.isEmpty,
                      ["incoming", "outgoing"].contains(direction) else {
                    throw LocalChatMessageQueryError(code: "internal", message: "The local IM database contains an invalid message row.")
                }
                hits.append(LocalChatMessageQueryHit(id: id, peerUserID: peerID,
                    senderID: try field(2), text: try field(3),
                    kind: try field(4), direction: direction,
                    createdAtMilliseconds: sqlite3_column_int64(statement, 6)))
            }
            try control.check()
            guard try identity() == databaseIdentity else { throw LocalChatMessageQueryError.changed }
            let hasMore = hits.count > limit
            if hasMore { hits.removeLast() }
            var next: String?
            if hasMore, let last = hits.last {
                next = try JSONEncoder().encode(LocalChatMessagePageCursor(version: 1,
                    database: databaseIdentity, sdk: sdk, owner: owner, generation: generation,
                    filters: filters, milliseconds: last.createdAtMilliseconds, id: last.storedID)).base64EncodedString()
            }
            return LocalChatMessageQueryPage(hits: hits, limit: limit, hasMore: hasMore,
                nextCursor: next, isFirstPage: cursor == nil)
        } catch let error as LocalChatMessageQueryError { throw error }
        catch {
            try control.check()
            if [SQLITE_BUSY, SQLITE_LOCKED].contains(sqlite3_errcode(database)) {
                throw LocalChatMessageQueryError(code: "busy", message: "The local IM database is busy.")
            }
            throw LocalChatMessageQueryError.unavailable
        }
    }

    func purgeAccount(ownerUserID: String, sdkAppID: Int? = nil) throws {
        let owner = ownerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !owner.isEmpty else { return }
        var ownedPaths = Set<String>()
        var otherPaths = Set<String>()
        var legacySDKIDs = Set<String>()
        legacySDKIDs.insert(accountKey(for: sdkAppID))
        try withDatabase { database in
            let listing = try prepare(database, sql: """
                SELECT sdk_app_id,owner_user_id,voice_attachment,image_attachment,
                       file_attachment,video_attachment FROM messages
                """)
            defer { sqlite3_finalize(listing) }
            while true {
                let result = sqlite3_step(listing)
                if result == SQLITE_DONE { break }
                guard result == SQLITE_ROW else { throw databaseError(database) }
                let isOwner = textColumn(listing, at: 1) == owner
                if isOwner { legacySDKIDs.insert(textColumn(listing, at: 0)) }
                var paths = Set<String>()
                for column in 2 ... 5 {
                    guard let raw = optionalTextColumn(listing, at: Int32(column)),
                          let data = raw.data(using: .utf8),
                          let value = try? JSONSerialization.jsonObject(with: data)
                              as? [String: Any] else { continue }
                    for key in ["localFilePath", "localPath", "coverPath"] {
                        if let path = value[key] as? String, !path.isEmpty {
                            paths.insert(path)
                        }
                    }
                }
                if isOwner { ownedPaths.formUnion(paths) }
                else { otherPaths.formUnion(paths) }
            }
            try execute(database, sql: "BEGIN IMMEDIATE TRANSACTION")
            do {
                let deletion = try prepare(database,
                    sql: "DELETE FROM messages WHERE owner_user_id=?")
                defer { sqlite3_finalize(deletion) }
                try bindText(owner, to: deletion, at: 1, database: database)
                guard sqlite3_step(deletion) == SQLITE_DONE else {
                    throw databaseError(database)
                }
                let groupDeletion = try prepare(database,
                    sql: "DELETE FROM groups WHERE owner_user_id=?")
                defer { sqlite3_finalize(groupDeletion) }
                try bindText(owner, to: groupDeletion, at: 1, database: database)
                guard sqlite3_step(groupDeletion) == SQLITE_DONE else {
                    throw databaseError(database)
                }
                try execute(database, sql: "COMMIT")
            } catch {
                try? execute(database, sql: "ROLLBACK")
                throw error
            }
        }
        for path in ownedPaths.subtracting(otherPaths) {
            for category in RemoteIMMediaStorage.Category.allCases {
                let resolved = RemoteIMMediaStorage.resolvedPath(from: path, category: category)
                if !resolved.isEmpty { try? fileManager.removeItem(atPath: resolved) }
            }
        }
        for rawSDKID in legacySDKIDs {
            try? fileManager.removeItem(at: legacyFileURL(
                sdkAppID: Int(rawSDKID), ownerUserID: owner
            ))
        }
    }

    /// 按稳定游标读取单个会话的有界历史页。
    /// 先迁移旧数据，再以时间和消息标识共同排序；缺少账号、联系人或有效页长时返回空页。
    func loadConversationPage(
        sdkAppID: Int?,
        ownerUserID: String,
        peerUserID: String,
        before cursor: LocalChatHistoryCursor?,
        limit: Int
    ) throws -> LocalChatHistoryPage {
        let cleanOwnerUserID = ownerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        let cleanPeerUserID = peerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanOwnerUserID.isEmpty, !cleanPeerUserID.isEmpty, limit > 0 else {
            return LocalChatHistoryPage(messages: [], hasEarlierMessages: false)
        }
        try migrateLegacyHistoryIfNeeded(
            sdkAppID: sdkAppID,
            ownerUserID: cleanOwnerUserID
        )

        return try withDatabase { database in
            let cursorClause = cursor == nil
                ? ""
                : "AND (created_at < ? OR (created_at = ? AND id < ?))"
            let statement = try prepare(
                database,
                sql: """
                SELECT id, remote_id, from_user, to_user, text, direction, status, created_at,
                       voice_attachment, image_attachment, file_attachment, video_attachment,
                       approval_request, approval_decision, caption_above, quote, media_caption, group_id
                FROM messages
                WHERE sdk_app_id = ? AND owner_user_id = ?
                  AND peer_user_id = ? AND group_id IS NULL
                  \(cursorClause)
                ORDER BY created_at DESC, id DESC
                LIMIT ?
                """
            )
            defer { sqlite3_finalize(statement) }
            try bindText(accountKey(for: sdkAppID), to: statement, at: 1, database: database)
            try bindText(cleanOwnerUserID, to: statement, at: 2, database: database)
            try bindText(cleanPeerUserID, to: statement, at: 3, database: database)

            var limitIndex: Int32 = 4
            if let cursor {
                sqlite3_bind_double(statement, 4, cursor.createdAt.timeIntervalSince1970)
                sqlite3_bind_double(statement, 5, cursor.createdAt.timeIntervalSince1970)
                try bindText(cursor.messageID.uuidString, to: statement, at: 6, database: database)
                limitIndex = 7
            }
            sqlite3_bind_int64(statement, limitIndex, Int64(limit + 1))

            var descendingMessages: [RemoteIMMessage] = []
            descendingMessages.reserveCapacity(limit + 1)
            while true {
                switch sqlite3_step(statement) {
                case SQLITE_ROW:
                    if let message = decodeMessage(from: statement) {
                        descendingMessages.append(message)
                    }
                case SQLITE_DONE:
                    let hasEarlierMessages = descendingMessages.count > limit
                    let messages = Array(descendingMessages.prefix(limit).reversed())
                    return LocalChatHistoryPage(
                        messages: messages,
                        hasEarlierMessages: hasEarlierMessages
                    )
                default:
                    throw databaseError(database)
                }
            }
        }
    }

    /// 为会话列表读取每位联系人最新的一条持久化消息。
    /// 先规范化账号和联系人标识，再按数据库结果去重；不把完整历史带入列表页面。
    func loadConversationSummaries(
        sdkAppID: Int?,
        ownerUserID: String,
        peerUserIDs: [String]
    ) throws -> [RemoteIMMessage] {
        let cleanOwnerUserID = ownerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        let cleanPeerUserIDs = Array(Set(peerUserIDs.map {
            $0.trimmingCharacters(in: .whitespacesAndNewlines)
        })).filter { !$0.isEmpty }
        guard !cleanOwnerUserID.isEmpty else { return [] }
        try migrateLegacyHistoryIfNeeded(
            sdkAppID: sdkAppID,
            ownerUserID: cleanOwnerUserID
        )
        guard !cleanPeerUserIDs.isEmpty else { return [] }

        return try withDatabase { database in
            let statement = try prepare(
                database,
                sql: """
                SELECT id, remote_id, from_user, to_user, text, direction, status, created_at,
                       voice_attachment, image_attachment, file_attachment, video_attachment,
                       approval_request, approval_decision, caption_above, quote, media_caption, group_id
                FROM messages
                WHERE sdk_app_id = ? AND owner_user_id = ? AND peer_user_id = ? AND group_id IS NULL
                ORDER BY created_at DESC, id DESC
                LIMIT 1
                """
            )
            defer { sqlite3_finalize(statement) }
            var messages: [RemoteIMMessage] = []
            messages.reserveCapacity(cleanPeerUserIDs.count)
            for peerUserID in cleanPeerUserIDs {
                sqlite3_reset(statement)
                sqlite3_clear_bindings(statement)
                try bindText(accountKey(for: sdkAppID), to: statement, at: 1, database: database)
                try bindText(cleanOwnerUserID, to: statement, at: 2, database: database)
                try bindText(peerUserID, to: statement, at: 3, database: database)
                switch sqlite3_step(statement) {
                case SQLITE_ROW:
                    if let message = decodeMessage(from: statement) {
                        messages.append(message)
                    }
                case SQLITE_DONE:
                    continue
                default:
                    throw databaseError(database)
                }
            }
            return messages.sorted {
                if $0.createdAt != $1.createdAt { return $0.createdAt > $1.createdAt }
                return $0.id.uuidString > $1.id.uuidString
            }
        }
    }

    func loadGroupConversationPage(
        sdkAppID: Int?,
        ownerUserID: String,
        groupID: String,
        before cursor: LocalChatHistoryCursor?,
        limit: Int
    ) throws -> LocalChatHistoryPage {
        let owner = ownerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        let group = groupID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !owner.isEmpty, !group.isEmpty, limit > 0 else {
            return LocalChatHistoryPage(messages: [], hasEarlierMessages: false)
        }
        return try withDatabase { database in
            let cursorClause = cursor == nil
                ? ""
                : "AND (created_at < ? OR (created_at = ? AND id < ?))"
            let statement = try prepare(database, sql: """
                SELECT id, remote_id, from_user, to_user, text, direction, status, created_at,
                       voice_attachment, image_attachment, file_attachment, video_attachment,
                       approval_request, approval_decision, caption_above, quote, media_caption, group_id
                FROM messages
                WHERE sdk_app_id = ? AND owner_user_id = ? AND group_id = ?
                  \(cursorClause)
                ORDER BY created_at DESC, id DESC
                LIMIT ?
                """)
            defer { sqlite3_finalize(statement) }
            try bindText(accountKey(for: sdkAppID), to: statement, at: 1, database: database)
            try bindText(owner, to: statement, at: 2, database: database)
            try bindText(group, to: statement, at: 3, database: database)
            var limitIndex: Int32 = 4
            if let cursor {
                sqlite3_bind_double(statement, 4, cursor.createdAt.timeIntervalSince1970)
                sqlite3_bind_double(statement, 5, cursor.createdAt.timeIntervalSince1970)
                try bindText(cursor.messageID.uuidString, to: statement, at: 6, database: database)
                limitIndex = 7
            }
            sqlite3_bind_int64(statement, limitIndex, Int64(limit + 1))
            var descending: [RemoteIMMessage] = []
            while true {
                switch sqlite3_step(statement) {
                case SQLITE_ROW:
                    if let message = decodeMessage(from: statement) { descending.append(message) }
                case SQLITE_DONE:
                    return LocalChatHistoryPage(
                        messages: Array(descending.prefix(limit).reversed()),
                        hasEarlierMessages: descending.count > limit
                    )
                default:
                    throw databaseError(database)
                }
            }
        }
    }

    func loadGroupConversationSummaries(
        sdkAppID: Int?,
        ownerUserID: String,
        groupIDs: [String]
    ) throws -> [RemoteIMMessage] {
        let owner = ownerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        let groups = Array(Set(groupIDs.map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }))
            .filter { !$0.isEmpty }
        guard !owner.isEmpty, !groups.isEmpty else { return [] }
        return try withDatabase { database in
            let statement = try prepare(database, sql: """
                SELECT id, remote_id, from_user, to_user, text, direction, status, created_at,
                       voice_attachment, image_attachment, file_attachment, video_attachment,
                       approval_request, approval_decision, caption_above, quote, media_caption, group_id
                FROM messages
                WHERE sdk_app_id = ? AND owner_user_id = ? AND group_id = ?
                ORDER BY created_at DESC, id DESC
                LIMIT 1
                """)
            defer { sqlite3_finalize(statement) }
            var messages: [RemoteIMMessage] = []
            for group in groups {
                sqlite3_reset(statement)
                sqlite3_clear_bindings(statement)
                try bindText(accountKey(for: sdkAppID), to: statement, at: 1, database: database)
                try bindText(owner, to: statement, at: 2, database: database)
                try bindText(group, to: statement, at: 3, database: database)
                switch sqlite3_step(statement) {
                case SQLITE_ROW:
                    if let message = decodeMessage(from: statement) { messages.append(message) }
                case SQLITE_DONE:
                    continue
                default:
                    throw databaseError(database)
                }
            }
            return messages.sorted { $0.createdAt > $1.createdAt }
        }
    }

    func loadGroups(sdkAppID: Int?, ownerUserID: String) throws -> [LocalChatHistoryGroup] {
        let owner = ownerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !owner.isEmpty else { return [] }
        return try withDatabase { database in
            let statement = try prepare(database, sql: """
                SELECT group_id, name, member_count, pending_activation
                FROM groups WHERE sdk_app_id = ? AND owner_user_id = ?
                ORDER BY updated_at DESC, group_id ASC
                """)
            defer { sqlite3_finalize(statement) }
            try bindText(accountKey(for: sdkAppID), to: statement, at: 1, database: database)
            try bindText(owner, to: statement, at: 2, database: database)
            var result: [LocalChatHistoryGroup] = []
            while true {
                switch sqlite3_step(statement) {
                case SQLITE_ROW:
                    result.append(LocalChatHistoryGroup(
                        groupID: textColumn(statement, at: 0),
                        name: textColumn(statement, at: 1),
                        memberCount: Int(sqlite3_column_int(statement, 2)),
                        pendingActivation: sqlite3_column_int(statement, 3) != 0
                    ))
                case SQLITE_DONE:
                    return result
                default:
                    throw databaseError(database)
                }
            }
        }
    }

    func replaceGroups(_ groups: [LocalChatHistoryGroup], sdkAppID: Int?, ownerUserID: String) throws {
        let owner = ownerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !owner.isEmpty else { return }
        try withDatabase { database in
            try execute(database, sql: "BEGIN IMMEDIATE TRANSACTION")
            do {
                let deletion = try prepare(database,
                    sql: "DELETE FROM groups WHERE sdk_app_id = ? AND owner_user_id = ?")
                defer { sqlite3_finalize(deletion) }
                try bindText(accountKey(for: sdkAppID), to: deletion, at: 1, database: database)
                try bindText(owner, to: deletion, at: 2, database: database)
                guard sqlite3_step(deletion) == SQLITE_DONE else { throw databaseError(database) }
                let insertion = try prepare(database, sql: """
                    INSERT INTO groups(sdk_app_id, owner_user_id, group_id, name,
                                       member_count, updated_at, pending_activation)
                    VALUES (?, ?, ?, ?, ?, ?, ?)
                    """)
                defer { sqlite3_finalize(insertion) }
                for group in groups where !group.groupID.isEmpty {
                    sqlite3_reset(insertion)
                    sqlite3_clear_bindings(insertion)
                    try bindText(accountKey(for: sdkAppID), to: insertion, at: 1, database: database)
                    try bindText(owner, to: insertion, at: 2, database: database)
                    try bindText(group.groupID, to: insertion, at: 3, database: database)
                    try bindText(group.name, to: insertion, at: 4, database: database)
                    sqlite3_bind_int(insertion, 5, Int32(max(0, group.memberCount)))
                    sqlite3_bind_double(insertion, 6, Date().timeIntervalSince1970)
                    sqlite3_bind_int(insertion, 7, group.pendingActivation ? 1 : 0)
                    guard sqlite3_step(insertion) == SQLITE_DONE else { throw databaseError(database) }
                }
                try execute(database, sql: "COMMIT")
            } catch {
                try? execute(database, sql: "ROLLBACK")
                throw error
            }
        }
    }

    /// 搜索该账号的完整持久化历史，而非只搜索当前已经打开的消息页。
    /// 使用 SQLite instr 将百分号和下划线视为普通文本，避免 LIKE 通配符误匹配；
    /// 返回数量受上限约束，搜索结果通过稳定消息标识供界面定位。
    func searchMessages(
        sdkAppID: Int?,
        ownerUserID: String,
        query: String,
        limit: Int
    ) throws -> [LocalChatHistorySearchHit] {
        let cleanOwnerUserID = ownerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        let cleanQuery = query.trimmingCharacters(in: .whitespacesAndNewlines)
        let safeLimit = min(max(limit, 1), 200)
        guard !cleanOwnerUserID.isEmpty, !cleanQuery.isEmpty else { return [] }
        try migrateLegacyHistoryIfNeeded(
            sdkAppID: sdkAppID,
            ownerUserID: cleanOwnerUserID
        )

        return try withDatabase { database in
            let statement = try prepare(
                database,
                sql: """
                SELECT id, remote_id, from_user, to_user, text, direction, status, created_at,
                       voice_attachment, image_attachment, file_attachment, video_attachment,
                       approval_request, approval_decision, caption_above, quote, media_caption, group_id,
                       CASE
                           WHEN peer_user_id IS NULL OR peer_user_id = '' THEN
                               CASE WHEN from_user = owner_user_id THEN to_user ELSE from_user END
                           ELSE peer_user_id
                       END
                FROM messages
                WHERE sdk_app_id = ? AND owner_user_id = ?
                  AND group_id IS NULL
                  AND text <> ''
                  AND instr(lower(text), lower(?)) > 0
                ORDER BY created_at DESC, id DESC
                LIMIT ?
                """
            )
            defer { sqlite3_finalize(statement) }
            try bindText(accountKey(for: sdkAppID), to: statement, at: 1, database: database)
            try bindText(cleanOwnerUserID, to: statement, at: 2, database: database)
            try bindText(cleanQuery, to: statement, at: 3, database: database)
            sqlite3_bind_int64(statement, 4, Int64(safeLimit))

            var hits: [LocalChatHistorySearchHit] = []
            hits.reserveCapacity(safeLimit)
            while true {
                switch sqlite3_step(statement) {
                case SQLITE_ROW:
                    let peerUserID = textColumn(statement, at: 18)
                        .trimmingCharacters(in: .whitespacesAndNewlines)
                    if !peerUserID.isEmpty, let message = decodeMessage(from: statement) {
                        hits.append(LocalChatHistorySearchHit(
                            peerUserID: peerUserID,
                            message: message
                        ))
                    }
                case SQLITE_DONE:
                    return hits
                default:
                    throw databaseError(database)
                }
            }
        }
    }

    func containsMessage(
        remoteID: String,
        sdkAppID: Int?,
        ownerUserID: String
    ) throws -> Bool {
        let cleanRemoteID = remoteID.trimmingCharacters(in: .whitespacesAndNewlines)
        let cleanOwnerUserID = ownerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanRemoteID.isEmpty, !cleanOwnerUserID.isEmpty else { return false }
        try migrateLegacyHistoryIfNeeded(
            sdkAppID: sdkAppID,
            ownerUserID: cleanOwnerUserID
        )
        return try withDatabase { database in
            let statement = try prepare(
                database,
                sql: """
                SELECT 1 FROM messages
                WHERE sdk_app_id = ? AND owner_user_id = ? AND remote_id = ?
                LIMIT 1
                """
            )
            defer { sqlite3_finalize(statement) }
            try bindText(accountKey(for: sdkAppID), to: statement, at: 1, database: database)
            try bindText(cleanOwnerUserID, to: statement, at: 2, database: database)
            try bindText(cleanRemoteID, to: statement, at: 3, database: database)
            switch sqlite3_step(statement) {
            case SQLITE_ROW:
                return true
            case SQLITE_DONE:
                return false
            default:
                throw databaseError(database)
            }
        }
    }

    func message(
        remoteID: String,
        sdkAppID: Int?,
        ownerUserID: String,
        peerUserID: String
    ) throws -> RemoteIMMessage? {
        let cleanRemoteID = remoteID.trimmingCharacters(in: .whitespacesAndNewlines)
        let cleanOwnerUserID = ownerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        let cleanPeerUserID = peerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanRemoteID.isEmpty, !cleanOwnerUserID.isEmpty, !cleanPeerUserID.isEmpty else {
            return nil
        }
        try migrateLegacyHistoryIfNeeded(sdkAppID: sdkAppID, ownerUserID: cleanOwnerUserID)
        return try withDatabase { database in
            let statement = try prepare(
                database,
                sql: """
                SELECT id, remote_id, from_user, to_user, text, direction, status, created_at,
                       voice_attachment, image_attachment, file_attachment, video_attachment,
                       approval_request, approval_decision, caption_above, quote, media_caption, group_id
                FROM messages
                WHERE sdk_app_id = ? AND owner_user_id = ?
                  AND peer_user_id = ? AND remote_id = ? AND group_id IS NULL
                ORDER BY created_at DESC, id DESC
                LIMIT 1
                """
            )
            defer { sqlite3_finalize(statement) }
            try bindText(accountKey(for: sdkAppID), to: statement, at: 1, database: database)
            try bindText(cleanOwnerUserID, to: statement, at: 2, database: database)
            try bindText(cleanPeerUserID, to: statement, at: 3, database: database)
            try bindText(cleanRemoteID, to: statement, at: 4, database: database)
            switch sqlite3_step(statement) {
            case SQLITE_ROW:
                return decodeMessage(from: statement)
            case SQLITE_DONE:
                return nil
            default:
                throw databaseError(database)
            }
        }
    }

    /// 在同一事务中应用会话、消息和游标变更。
    /// 只提交本次变化，不能因当前页未加载就删除数据库里更早的消息。
    func applyMutations(
        _ mutations: [LocalChatHistoryMutation]
    ) throws -> LocalChatHistorySaveResult {
        let startedAt = ProcessInfo.processInfo.systemUptime
        let validMutations = mutations.filter { !$0.account.ownerUserID.isEmpty }
        guard !validMutations.isEmpty else {
            return LocalChatHistorySaveResult(
                messageCount: 0,
                upsertedCount: 0,
                removedCount: 0,
                durationMilliseconds: 0
            )
        }

        var upsertedCount = 0
        var removedCount = 0
        let accounts = Set(validMutations.map(\.account))
        for account in accounts {
            try migrateLegacyHistoryIfNeeded(
                sdkAppID: account.sdkAppID,
                ownerUserID: account.ownerUserID
            )
        }
        try withDatabase { database in
            try execute(database, sql: "BEGIN IMMEDIATE TRANSACTION")
            do {
                for mutation in validMutations {
                    let ownerUserID = mutation.account.ownerUserID
                    switch mutation.operation {
                    case .upsert(let messages):
                        let validMessages = Self.deduplicatedMessages(messages).filter {
                            ($0.groupID != nil && $0.toUserID == $0.groupID) ||
                                $0.fromUserID == ownerUserID || $0.toUserID == ownerUserID
                        }
                        try insertMessages(
                            validMessages,
                            into: database,
                            sdkAppID: mutation.account.sdkAppID,
                            ownerUserID: ownerUserID
                        )
                        upsertedCount += validMessages.count
                    case .removeConversation(let peerUserID):
                        removedCount += try removeConversationMessages(
                            peerUserID: peerUserID,
                            from: database,
                            sdkAppID: mutation.account.sdkAppID,
                            ownerUserID: ownerUserID
                        )
                    case .removeMessage(let id):
                        removedCount += try removeMessage(
                            id: id,
                            from: database,
                            sdkAppID: mutation.account.sdkAppID,
                            ownerUserID: ownerUserID
                        )
                    }
                }
                try execute(database, sql: "COMMIT")
            } catch {
                try? execute(database, sql: "ROLLBACK")
                throw error
            }
        }

        for account in accounts {
            try? fileManager.removeItem(
                at: legacyFileURL(
                    sdkAppID: account.sdkAppID,
                    ownerUserID: account.ownerUserID
                )
            )
        }
        return LocalChatHistorySaveResult(
            messageCount: upsertedCount,
            upsertedCount: upsertedCount,
            removedCount: removedCount,
            durationMilliseconds: max(
                0,
                Int(((ProcessInfo.processInfo.systemUptime - startedAt) * 1_000).rounded())
            )
        )
    }

    private func removeConversationMessages(
        peerUserID: String,
        from database: OpaquePointer,
        sdkAppID: Int?,
        ownerUserID: String
    ) throws -> Int {
        let cleanPeerUserID = peerUserID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanPeerUserID.isEmpty else { return 0 }
        let statement = try prepare(
            database,
            sql: """
            DELETE FROM messages
            WHERE sdk_app_id = ? AND owner_user_id = ?
              AND peer_user_id = ? AND group_id IS NULL
            """
        )
        defer { sqlite3_finalize(statement) }
        try bindText(accountKey(for: sdkAppID), to: statement, at: 1, database: database)
        try bindText(ownerUserID, to: statement, at: 2, database: database)
        try bindText(cleanPeerUserID, to: statement, at: 3, database: database)
        guard sqlite3_step(statement) == SQLITE_DONE else {
            throw databaseError(database)
        }
        return Int(sqlite3_changes(database))
    }

    private func removeMessage(
        id: UUID,
        from database: OpaquePointer,
        sdkAppID: Int?,
        ownerUserID: String
    ) throws -> Int {
        let statement = try prepare(database, sql: """
            DELETE FROM messages
            WHERE sdk_app_id = ? AND owner_user_id = ? AND id = ?
            """)
        defer { sqlite3_finalize(statement) }
        try bindText(accountKey(for: sdkAppID), to: statement, at: 1, database: database)
        try bindText(ownerUserID, to: statement, at: 2, database: database)
        try bindText(id.uuidString, to: statement, at: 3, database: database)
        guard sqlite3_step(statement) == SQLITE_DONE else { throw databaseError(database) }
        return Int(sqlite3_changes(database))
    }

    /// 把消息正文和附件元数据写入本地 SQLite。
    /// 稳定消息标识用于覆盖更新；失败时整个批次回滚，避免半条消息。
    private func insertMessages(
        _ messages: [RemoteIMMessage],
        into database: OpaquePointer,
        sdkAppID: Int?,
        ownerUserID: String,
        updateExisting: Bool = true
    ) throws {
        let conflictClause = updateExisting
            ? """
              DO UPDATE SET
                  remote_id = excluded.remote_id,
                  from_user = excluded.from_user,
                  to_user = excluded.to_user,
                  text = excluded.text,
                  direction = excluded.direction,
                  status = excluded.status,
                  created_at = excluded.created_at,
                  voice_attachment = excluded.voice_attachment,
                  image_attachment = excluded.image_attachment,
                  file_attachment = excluded.file_attachment,
                  video_attachment = excluded.video_attachment,
                  approval_request = excluded.approval_request,
                  approval_decision = excluded.approval_decision,
                  caption_above = excluded.caption_above,
                  quote = excluded.quote,
                  media_caption = excluded.media_caption,
                  peer_user_id = excluded.peer_user_id,
                  group_id = excluded.group_id
              """
            : "DO NOTHING"
        let statement = try prepare(
            database,
            sql: """
            INSERT INTO messages(
                sdk_app_id, owner_user_id, id, remote_id, from_user, to_user, text,
                direction, status, created_at,
                voice_attachment, image_attachment, file_attachment, video_attachment,
                approval_request, approval_decision, caption_above, quote,
                media_caption, peer_user_id, group_id
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(sdk_app_id, owner_user_id, id) \(conflictClause)
            """
        )
        defer { sqlite3_finalize(statement) }

        for message in messages {
            sqlite3_reset(statement)
            sqlite3_clear_bindings(statement)
            let incoming = message.direction == .incoming
            let voiceCategory: RemoteIMMediaStorage.Category = incoming ? .incomingVoices : .outgoingVoices
            let imageCategory: RemoteIMMediaStorage.Category = incoming ? .incomingImages : .outgoingImages
            let fileCategory: RemoteIMMediaStorage.Category = incoming ? .incomingFiles : .outgoingFiles
            let videoCategory: RemoteIMMediaStorage.Category = incoming ? .incomingVideos : .outgoingVideos
            let coverCategory: RemoteIMMediaStorage.Category = incoming ? .incomingVideoCovers : .outgoingVideoCovers
            let persistedVoiceAttachment = message.voiceAttachment.map {
                RemoteIMVoiceAttachment(
                    localFilePath: RemoteIMMediaStorage.persistentReference(
                        for: $0.localFilePath,
                        category: voiceCategory
                    ),
                    durationSeconds: $0.durationSeconds,
                    remoteID: $0.remoteID
                )
            }
            let persistedImageAttachment = message.imageAttachment.map {
                RemoteIMImageAttachment(
                    localFilePath: RemoteIMMediaStorage.persistentReference(
                        for: $0.localFilePath,
                        category: imageCategory
                    ),
                    remoteID: $0.remoteID,
                    width: $0.width,
                    height: $0.height,
                    sizeBytes: $0.sizeBytes
                )
            }
            let persistedFileAttachment = message.fileAttachment.map {
                RemoteIMFileAttachment(
                    localFilePath: RemoteIMMediaStorage.persistentReference(
                        for: $0.localFilePath,
                        category: fileCategory
                    ),
                    fileName: $0.fileName,
                    mimeType: $0.mimeType,
                    remoteID: $0.remoteID,
                    sizeBytes: $0.sizeBytes
                )
            }
            let persistedVideoAttachment = message.videoAttachment.map {
                RemoteIMVideoAttachment(
                    localPath: RemoteIMMediaStorage.persistentReference(
                        for: $0.localPath,
                        category: videoCategory
                    ),
                    coverPath: $0.coverPath.map {
                        RemoteIMMediaStorage.persistentReference(for: $0, category: coverCategory)
                    },
                    durationSeconds: $0.durationSeconds,
                    width: $0.width,
                    height: $0.height,
                    sizeBytes: $0.sizeBytes
                )
            }
            try bindText(accountKey(for: sdkAppID), to: statement, at: 1, database: database)
            try bindText(ownerUserID, to: statement, at: 2, database: database)
            try bindText(message.id.uuidString, to: statement, at: 3, database: database)
            try bindOptionalText(message.remoteID, to: statement, at: 4, database: database)
            try bindText(message.fromUserID, to: statement, at: 5, database: database)
            try bindText(message.toUserID, to: statement, at: 6, database: database)
            try bindText(message.text, to: statement, at: 7, database: database)
            try bindText(message.direction.rawValue, to: statement, at: 8, database: database)
            try bindText(message.status.rawValue, to: statement, at: 9, database: database)
            sqlite3_bind_double(statement, 10, message.createdAt.timeIntervalSince1970)
            try bindOptionalJSON(persistedVoiceAttachment, to: statement, at: 11, database: database)
            try bindOptionalJSON(persistedImageAttachment, to: statement, at: 12, database: database)
            try bindOptionalJSON(persistedFileAttachment, to: statement, at: 13, database: database)
            try bindOptionalJSON(persistedVideoAttachment, to: statement, at: 14, database: database)
            try bindOptionalJSON(message.approvalRequest, to: statement, at: 15, database: database)
            try bindOptionalJSON(message.approvalDecision, to: statement, at: 16, database: database)
            sqlite3_bind_int(statement, 17, message.captionAbove ? 1 : 0)
            try bindOptionalJSON(message.quote, to: statement, at: 18, database: database)
            try bindOptionalText(message.mediaCaption, to: statement, at: 19, database: database)
            if let groupID = message.groupID, !groupID.isEmpty {
                sqlite3_bind_null(statement, 20)
                try bindText(groupID, to: statement, at: 21, database: database)
            } else {
                let peerUserID = message.fromUserID == ownerUserID
                    ? message.toUserID
                    : message.fromUserID
                try bindText(peerUserID, to: statement, at: 20, database: database)
                sqlite3_bind_null(statement, 21)
            }
            guard sqlite3_step(statement) == SQLITE_DONE else {
                throw databaseError(database)
            }
        }
    }

    /// 从数据库行还原完整 IM 消息及附件引用。
    /// 按字段版本处理旧数据；无法恢复的附件不应使整页历史消失。
    private func decodeMessage(from statement: OpaquePointer) -> RemoteIMMessage? {
        guard let id = UUID(uuidString: textColumn(statement, at: 0)),
              let direction = RemoteIMMessageDirection(rawValue: textColumn(statement, at: 5)),
              let status = RemoteIMMessageStatus(rawValue: textColumn(statement, at: 6))
        else {
            return nil
        }

        let incoming = direction == .incoming
        let voiceCategory: RemoteIMMediaStorage.Category = incoming ? .incomingVoices : .outgoingVoices
        let imageCategory: RemoteIMMediaStorage.Category = incoming ? .incomingImages : .outgoingImages
        let fileCategory: RemoteIMMediaStorage.Category = incoming ? .incomingFiles : .outgoingFiles
        let videoCategory: RemoteIMMediaStorage.Category = incoming ? .incomingVideos : .outgoingVideos
        let coverCategory: RemoteIMMediaStorage.Category = incoming ? .incomingVideoCovers : .outgoingVideoCovers

        let voiceAttachment = decodeOptionalJSON(
            RemoteIMVoiceAttachment.self,
            from: optionalTextColumn(statement, at: 8)
        ).map {
            RemoteIMVoiceAttachment(
                localFilePath: RemoteIMMediaStorage.resolvedPath(
                    from: $0.localFilePath,
                    category: voiceCategory
                ),
                durationSeconds: $0.durationSeconds,
                remoteID: $0.remoteID
            )
        }
        let imageAttachment = decodeOptionalJSON(
            RemoteIMImageAttachment.self,
            from: optionalTextColumn(statement, at: 9)
        ).map {
            RemoteIMImageAttachment(
                localFilePath: RemoteIMMediaStorage.resolvedPath(
                    from: $0.localFilePath,
                    category: imageCategory
                ),
                remoteID: $0.remoteID,
                width: $0.width,
                height: $0.height,
                sizeBytes: $0.sizeBytes
            )
        }
        let fileAttachment = decodeOptionalJSON(
            RemoteIMFileAttachment.self,
            from: optionalTextColumn(statement, at: 10)
        ).map {
            RemoteIMFileAttachment(
                localFilePath: RemoteIMMediaStorage.resolvedPath(
                    from: $0.localFilePath,
                    category: fileCategory
                ),
                fileName: $0.fileName,
                mimeType: $0.mimeType,
                remoteID: $0.remoteID,
                sizeBytes: $0.sizeBytes
            )
        }
        let videoAttachment = decodeOptionalJSON(
            RemoteIMVideoAttachment.self,
            from: optionalTextColumn(statement, at: 11)
        ).map {
            RemoteIMVideoAttachment(
                localPath: RemoteIMMediaStorage.resolvedPath(from: $0.localPath, category: videoCategory),
                coverPath: $0.coverPath.map {
                    RemoteIMMediaStorage.resolvedPath(from: $0, category: coverCategory)
                },
                durationSeconds: $0.durationSeconds,
                width: $0.width,
                height: $0.height,
                sizeBytes: $0.sizeBytes
            )
        }
        let approvalRequest = decodeOptionalJSON(
            RemoteIMApprovalRequest.self,
            from: optionalTextColumn(statement, at: 12)
        )
        let approvalDecision = decodeOptionalJSON(
            RemoteIMApprovalDecision.self,
            from: optionalTextColumn(statement, at: 13)
        )

        return RemoteIMMessage(
            id: id,
            remoteID: optionalTextColumn(statement, at: 1),
            fromUserID: textColumn(statement, at: 2),
            toUserID: textColumn(statement, at: 3),
            groupID: optionalTextColumn(statement, at: 17),
            text: textColumn(statement, at: 4),
            mediaCaption: optionalTextColumn(statement, at: 16),
            captionAbove: sqlite3_column_int(statement, 14) != 0,
            quote: decodeOptionalJSON(
                RemoteIMQuote.self,
                from: optionalTextColumn(statement, at: 15)
            ),
            voiceAttachment: voiceAttachment,
            imageAttachment: imageAttachment,
            fileAttachment: fileAttachment,
            videoAttachment: videoAttachment,
            approvalRequest: approvalRequest,
            approvalDecision: approvalDecision,
            direction: direction,
            status: status,
            createdAt: Date(timeIntervalSince1970: sqlite3_column_double(statement, 7))
        )
    }

    private func withDatabase<T>(_ operation: (OpaquePointer) throws -> T) throws -> T {
        try fileManager.createDirectory(
            at: baseDirectoryURL,
            withIntermediateDirectories: true
        )

        var database: OpaquePointer?
        let openResult = sqlite3_open_v2(
            databaseURL.path,
            &database,
            SQLITE_OPEN_CREATE | SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
            nil
        )
        guard openResult == SQLITE_OK, let database else {
            let message = database.map(databaseErrorMessage) ?? "无法打开数据库"
            if let database {
                sqlite3_close(database)
            }
            throw StoreError.database(message)
        }
        defer { sqlite3_close(database) }

        try migrate(database)
        return try operation(database)
    }

    /// 把旧版数据库结构升级到当前会话游标和附件格式。
    /// 迁移必须保留原消息顺序与标识，并在失败时保留可重试的旧数据。
    private func migrate(_ database: OpaquePointer) throws {
        let previousSchemaVersion = try databaseUserVersion(database)
        try execute(database, sql: "PRAGMA journal_mode = WAL")
        try execute(database, sql: "PRAGMA synchronous = NORMAL")
        try execute(
            database,
            sql: """
            CREATE TABLE IF NOT EXISTS messages (
                sdk_app_id TEXT NOT NULL,
                owner_user_id TEXT NOT NULL,
                id TEXT NOT NULL,
                remote_id TEXT,
                from_user TEXT NOT NULL,
                to_user TEXT NOT NULL,
                text TEXT NOT NULL DEFAULT '',
                direction TEXT NOT NULL,
                status TEXT NOT NULL,
                created_at REAL NOT NULL,
                voice_attachment TEXT,
                image_attachment TEXT,
                file_attachment TEXT,
                video_attachment TEXT,
                approval_request TEXT,
                approval_decision TEXT,
                caption_above INTEGER NOT NULL DEFAULT 0,
                quote TEXT,
                media_caption TEXT,
                peer_user_id TEXT,
                group_id TEXT,
                PRIMARY KEY (sdk_app_id, owner_user_id, id)
            )
            """
        )
        try execute(database, sql: """
            CREATE TABLE IF NOT EXISTS groups (
                sdk_app_id TEXT NOT NULL,
                owner_user_id TEXT NOT NULL,
                group_id TEXT NOT NULL,
                name TEXT NOT NULL,
                member_count INTEGER NOT NULL DEFAULT 0,
                updated_at REAL NOT NULL,
                pending_activation INTEGER NOT NULL DEFAULT 0,
                PRIMARY KEY (sdk_app_id, owner_user_id, group_id)
            )
            """)
        if try !columnExists("pending_activation", in: "groups", database: database) {
            try execute(database, sql: "ALTER TABLE groups ADD COLUMN pending_activation INTEGER NOT NULL DEFAULT 0")
        }
        if try !columnExists("remote_id", in: "messages", database: database) {
            try execute(database, sql: "ALTER TABLE messages ADD COLUMN remote_id TEXT")
        }
        if try !columnExists("peer_user_id", in: "messages", database: database) {
            try execute(database, sql: "ALTER TABLE messages ADD COLUMN peer_user_id TEXT")
        }
        if try !columnExists("group_id", in: "messages", database: database) {
            try execute(database, sql: "ALTER TABLE messages ADD COLUMN group_id TEXT")
        }
        if try !columnExists("video_attachment", in: "messages", database: database) {
            try execute(database, sql: "ALTER TABLE messages ADD COLUMN video_attachment TEXT")
        }
        if try !columnExists("approval_request", in: "messages", database: database) {
            try execute(database, sql: "ALTER TABLE messages ADD COLUMN approval_request TEXT")
        }
        if try !columnExists("approval_decision", in: "messages", database: database) {
            try execute(database, sql: "ALTER TABLE messages ADD COLUMN approval_decision TEXT")
        }
        if try !columnExists("caption_above", in: "messages", database: database) {
            try execute(
                database,
                sql: "ALTER TABLE messages ADD COLUMN caption_above INTEGER NOT NULL DEFAULT 0"
            )
        }
        if try !columnExists("quote", in: "messages", database: database) {
            try execute(database, sql: "ALTER TABLE messages ADD COLUMN quote TEXT")
        }
        if try !columnExists("media_caption", in: "messages", database: database) {
            try execute(database, sql: "ALTER TABLE messages ADD COLUMN media_caption TEXT")
        }
        if previousSchemaVersion < 3 {
            try execute(
                database,
                sql: """
                UPDATE messages
                SET peer_user_id = CASE
                    WHEN from_user = owner_user_id THEN to_user
                    ELSE from_user
                END
                WHERE peer_user_id IS NULL OR peer_user_id = ''
                """
            )
        }
        try execute(
            database,
            sql: """
            CREATE INDEX IF NOT EXISTS idx_messages_account_time
            ON messages(sdk_app_id, owner_user_id, created_at, id)
            """
        )
        try execute(
            database,
            sql: """
            CREATE INDEX IF NOT EXISTS idx_messages_account_remote_id
            ON messages(sdk_app_id, owner_user_id, remote_id)
            """
        )
        try execute(
            database,
            sql: """
            CREATE INDEX IF NOT EXISTS idx_messages_account_peer_time
            ON messages(sdk_app_id, owner_user_id, peer_user_id, created_at DESC, id DESC)
            """
        )
        try execute(
            database,
            sql: """
            CREATE INDEX IF NOT EXISTS idx_messages_account_group_time
            ON messages(sdk_app_id, owner_user_id, group_id, created_at DESC, id DESC)
            """
        )
        if previousSchemaVersion < 10 {
            try execute(database, sql: "PRAGMA user_version = 10")
        }
        // 普通启动/写入时一次性建立只读 IM 查询索引；query worker 不改 schema 或 journal。
        try execute(database, sql: """
            CREATE INDEX IF NOT EXISTS idx_im_query_ms
            ON messages(sdk_app_id,owner_user_id,\(LocalChatMessageSQL.milliseconds) DESC,id DESC)
            WHERE group_id IS NULL
            """)
        try execute(database, sql: """
            CREATE INDEX IF NOT EXISTS idx_im_query_peer_ms
            ON messages(sdk_app_id,owner_user_id,(\(LocalChatMessageSQL.peer)),\(LocalChatMessageSQL.milliseconds) DESC,id DESC)
            WHERE group_id IS NULL
            """)
    }

    private func databaseUserVersion(_ database: OpaquePointer) throws -> Int {
        let statement = try prepare(database, sql: "PRAGMA user_version")
        defer { sqlite3_finalize(statement) }
        guard sqlite3_step(statement) == SQLITE_ROW else {
            throw databaseError(database)
        }
        return Int(sqlite3_column_int(statement, 0))
    }

    private func execute(_ database: OpaquePointer, sql: String) throws {
        var errorPointer: UnsafeMutablePointer<CChar>?
        let result = sqlite3_exec(database, sql, nil, nil, &errorPointer)
        guard result == SQLITE_OK else {
            let message = errorPointer.map { String(cString: $0) } ?? databaseErrorMessage(database)
            sqlite3_free(errorPointer)
            throw StoreError.database(message)
        }
    }

    private func prepare(_ database: OpaquePointer, sql: String) throws -> OpaquePointer {
        var statement: OpaquePointer?
        guard sqlite3_prepare_v2(database, sql, -1, &statement, nil) == SQLITE_OK,
              let statement
        else {
            throw databaseError(database)
        }
        return statement
    }

    private func bindText(
        _ value: String,
        to statement: OpaquePointer,
        at index: Int32,
        database: OpaquePointer
    ) throws {
        let result = value.withCString { pointer in
            sqlite3_bind_text(
                statement,
                index,
                pointer,
                -1,
                unsafeBitCast(-1, to: sqlite3_destructor_type.self)
            )
        }
        guard result == SQLITE_OK else {
            throw databaseError(database)
        }
    }

    private func bindOptionalText(
        _ value: String?,
        to statement: OpaquePointer,
        at index: Int32,
        database: OpaquePointer
    ) throws {
        guard let value, !value.isEmpty else {
            guard sqlite3_bind_null(statement, index) == SQLITE_OK else {
                throw databaseError(database)
            }
            return
        }
        try bindText(value, to: statement, at: index, database: database)
    }

    private func columnExists(
        _ columnName: String,
        in tableName: String,
        database: OpaquePointer
    ) throws -> Bool {
        let statement = try prepare(database, sql: "PRAGMA table_info(\(tableName))")
        defer { sqlite3_finalize(statement) }
        while sqlite3_step(statement) == SQLITE_ROW {
            if textColumn(statement, at: 1) == columnName {
                return true
            }
        }
        return false
    }

    private func bindOptionalJSON<Value: Encodable>(
        _ value: Value?,
        to statement: OpaquePointer,
        at index: Int32,
        database: OpaquePointer
    ) throws {
        guard let value else {
            guard sqlite3_bind_null(statement, index) == SQLITE_OK else {
                throw databaseError(database)
            }
            return
        }
        let data = try encoder.encode(value)
        guard let json = String(data: data, encoding: .utf8) else {
            throw StoreError.database("附件数据编码失败")
        }
        try bindText(json, to: statement, at: index, database: database)
    }

    private func decodeOptionalJSON<Value: Decodable>(
        _ type: Value.Type,
        from json: String?
    ) -> Value? {
        guard let json, let data = json.data(using: .utf8) else { return nil }
        return try? decoder.decode(type, from: data)
    }

    private func textColumn(_ statement: OpaquePointer, at index: Int32) -> String {
        guard let value = sqlite3_column_text(statement, index) else { return "" }
        return String(cString: value)
    }

    private func optionalTextColumn(_ statement: OpaquePointer, at index: Int32) -> String? {
        guard sqlite3_column_type(statement, index) != SQLITE_NULL else { return nil }
        return textColumn(statement, at: index)
    }

    private func databaseError(_ database: OpaquePointer) -> StoreError {
        StoreError.database(databaseErrorMessage(database))
    }

    private func databaseErrorMessage(_ database: OpaquePointer) -> String {
        guard let message = sqlite3_errmsg(database) else { return "未知数据库错误" }
        return String(cString: message)
    }

    private func loadLegacyHistory(
        sdkAppID: Int?,
        ownerUserID: String
    ) -> StoredChatHistory? {
        let historyURL = legacyFileURL(sdkAppID: sdkAppID, ownerUserID: ownerUserID)
        guard let data = try? Data(contentsOf: historyURL),
              let history = try? decoder.decode(StoredChatHistory.self, from: data),
              history.ownerUserID == ownerUserID,
              history.sdkAppID == sdkAppID
        else {
            return nil
        }
        return history
    }

    private func migrateLegacyHistoryIfNeeded(
        sdkAppID: Int?,
        ownerUserID: String
    ) throws {
        guard let legacyHistory = loadLegacyHistory(
            sdkAppID: sdkAppID,
            ownerUserID: ownerUserID
        ) else { return }
        try withDatabase { database in
            try execute(database, sql: "BEGIN IMMEDIATE TRANSACTION")
            do {
                try insertMessages(
                    Self.deduplicatedMessages(legacyHistory.messages),
                    into: database,
                    sdkAppID: sdkAppID,
                    ownerUserID: ownerUserID,
                    updateExisting: false
                )
                try execute(database, sql: "COMMIT")
            } catch {
                try? execute(database, sql: "ROLLBACK")
                throw error
            }
        }
        try fileManager.removeItem(
            at: legacyFileURL(sdkAppID: sdkAppID, ownerUserID: ownerUserID)
        )
    }

    private func legacyFileURL(sdkAppID: Int?, ownerUserID: String) -> URL {
        let rawFileName = "\(sdkAppID.map(String.init) ?? "default")__\(ownerUserID)"
        let allowedCharacters = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "-_."))
        let fileName = rawFileName
            .addingPercentEncoding(withAllowedCharacters: allowedCharacters) ?? "history"
        return baseDirectoryURL.appendingPathComponent("\(fileName).json", isDirectory: false)
    }

    private var databaseURL: URL {
        baseDirectoryURL.appendingPathComponent("messages.sqlite3", isDirectory: false)
    }

    private func accountKey(for sdkAppID: Int?) -> String {
        sdkAppID.map(String.init) ?? "default"
    }

    private static func deduplicatedMessages(_ messages: [RemoteIMMessage]) -> [RemoteIMMessage] {
        var messagesByID: [UUID: RemoteIMMessage] = [:]
        for message in messages {
            messagesByID[message.id] = message
        }
        return messagesByID.values.sorted {
            if $0.createdAt != $1.createdAt {
                return $0.createdAt < $1.createdAt
            }
            return $0.id.uuidString < $1.id.uuidString
        }
    }

    private static func defaultBaseDirectoryURL(fileManager: FileManager) -> URL {
        if let applicationSupportURL = try? fileManager.url(
            for: .applicationSupportDirectory,
            in: .userDomainMask,
            appropriateFor: nil,
            create: true
        ) {
            return applicationSupportURL
                .appendingPathComponent("MaiChat", isDirectory: true)
                .appendingPathComponent("ChatHistory", isDirectory: true)
        }
        return fileManager.temporaryDirectory
            .appendingPathComponent("MaiChat", isDirectory: true)
            .appendingPathComponent("ChatHistory", isDirectory: true)
    }
}

actor LocalChatHistoryPersistence {
    private let store: LocalChatHistoryStore
    private nonisolated let messageQueryReader: LocalChatMessageQueryReader

    init(baseDirectoryURL: URL) {
        self.store = LocalChatHistoryStore(baseDirectoryURL: baseDirectoryURL)
        self.messageQueryReader = LocalChatMessageQueryReader(directory: baseDirectoryURL)
    }

    func prepareDatabase() throws { try store.prepareDatabase() }

    func insertIncomingMetadata(_ message: RemoteIMMessage, sdkAppID: Int?, ownerUserID: String)
        throws -> LocalChatIncomingMetadataResult {
        try store.insertIncomingMetadata(message, sdkAppID: sdkAppID, ownerUserID: ownerUserID)
    }

    // 不在历史写入 actor 执行 SQL；普通调用方与 Agent 共用同一独立只读 worker。
    nonisolated func queryMessages(sdkAppID: Int?, ownerUserID: String, generation: UInt64,
                                  request: LocalChatMessageQuery) async throws -> LocalChatMessageQueryPage {
        try await messageQueryReader.page(sdkAppID: sdkAppID, owner: ownerUserID,
                                         generation: generation, request: request)
    }

    func purgeAccount(ownerUserID: String, sdkAppID: Int?) throws {
        try store.purgeAccount(ownerUserID: ownerUserID, sdkAppID: sdkAppID)
    }

    func loadGroups(sdkAppID: Int?, ownerUserID: String) throws -> [LocalChatHistoryGroup] {
        try store.loadGroups(sdkAppID: sdkAppID, ownerUserID: ownerUserID)
    }

    func replaceGroups(_ groups: [LocalChatHistoryGroup], sdkAppID: Int?, ownerUserID: String) throws {
        try store.replaceGroups(groups, sdkAppID: sdkAppID, ownerUserID: ownerUserID)
    }

    func loadConversationPage(
        sdkAppID: Int?,
        ownerUserID: String,
        peerUserID: String,
        before cursor: LocalChatHistoryCursor?,
        limit: Int
    ) throws -> LocalChatHistoryPage {
        try store.loadConversationPage(
            sdkAppID: sdkAppID,
            ownerUserID: ownerUserID,
            peerUserID: peerUserID,
            before: cursor,
            limit: limit
        )
    }

    func loadGroupConversationPage(
        sdkAppID: Int?,
        ownerUserID: String,
        groupID: String,
        before cursor: LocalChatHistoryCursor?,
        limit: Int
    ) throws -> LocalChatHistoryPage {
        try store.loadGroupConversationPage(
            sdkAppID: sdkAppID,
            ownerUserID: ownerUserID,
            groupID: groupID,
            before: cursor,
            limit: limit
        )
    }

    func loadGroupConversationSummaries(
        sdkAppID: Int?,
        ownerUserID: String,
        groupIDs: [String]
    ) throws -> [RemoteIMMessage] {
        try store.loadGroupConversationSummaries(
            sdkAppID: sdkAppID,
            ownerUserID: ownerUserID,
            groupIDs: groupIDs
        )
    }

    func loadConversationSummaries(
        sdkAppID: Int?,
        ownerUserID: String,
        peerUserIDs: [String]
    ) throws -> [RemoteIMMessage] {
        try store.loadConversationSummaries(
            sdkAppID: sdkAppID,
            ownerUserID: ownerUserID,
            peerUserIDs: peerUserIDs
        )
    }

    func searchMessages(
        sdkAppID: Int?,
        ownerUserID: String,
        query: String,
        limit: Int
    ) throws -> [LocalChatHistorySearchHit] {
        try store.searchMessages(
            sdkAppID: sdkAppID,
            ownerUserID: ownerUserID,
            query: query,
            limit: limit
        )
    }

    func containsMessage(
        remoteID: String,
        sdkAppID: Int?,
        ownerUserID: String
    ) throws -> Bool {
        try store.containsMessage(
            remoteID: remoteID,
            sdkAppID: sdkAppID,
            ownerUserID: ownerUserID
        )
    }

    func message(
        remoteID: String,
        sdkAppID: Int?,
        ownerUserID: String,
        peerUserID: String
    ) throws -> RemoteIMMessage? {
        try store.message(
            remoteID: remoteID,
            sdkAppID: sdkAppID,
            ownerUserID: ownerUserID,
            peerUserID: peerUserID
        )
    }

    func persist(
        mutations: [LocalChatHistoryMutation]
    ) throws -> LocalChatHistorySaveResult {
        try store.applyMutations(mutations)
    }
}
