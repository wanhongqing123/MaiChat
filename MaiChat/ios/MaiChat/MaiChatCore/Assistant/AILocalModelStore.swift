// 文件作用：持久化本机 GGUF 目录，供普通推理界面和 Agent 共用真实的沙盒文件。
// 导入只创建新 UUID 文件；没有替换/删除 API，避免改写共享 runtime 正在 mmap 的模型。
import Foundation
import CryptoKit

public enum AILocalModelRole: String, Codable, Sendable, CaseIterable {
    case model, projector
}

public struct AILocalModelEntry: Identifiable, Codable, Equatable, Sendable {
    public let id: String
    public let name: String
    public let filename: String
    public let bytes: UInt64
    public let sha256: String
    public let role: AILocalModelRole
    public let isBuiltin: Bool
}

public struct AILocalModelCatalog: Sendable {
    public let entries: [AILocalModelEntry]
    public let selectedModelID: String?
    public let selectedProjectorID: String?
    public let builtinAvailable: Bool
}

public struct AILocalModelSelection: Sendable {
    public let modelURL: URL
    public let projectorURL: URL?
    public let isBuiltin: Bool
}

public enum AILocalModelStoreError: Error, LocalizedError {
    case invalidFile, invalidGGUF, tooLarge, catalogFull, invalidCatalog, unavailable, busy

    public var errorDescription: String? {
        switch self {
        case .invalidFile: "请选择可读取的普通 GGUF 文件，不支持文件夹或符号链接"
        case .invalidGGUF: "文件不是完整的受支持 GGUF（v2/v3），或元数据超过安全上限"
        case .tooLarge: "模型文件超过 16 GB 导入上限"
        case .catalogFull: "本地模型数量已达 128 个上限"
        case .invalidCatalog: "本地模型目录不可读取，现有文件已保留"
        case .unavailable: "模型文件不可用，请重新导入；已有文件不会被覆盖"
        case .busy: "正在导入模型，请稍后再试"
        }
    }
}

public actor AILocalModelStore {
    public static let shared = AILocalModelStore()
    public static let builtinID = "builtin-stories260K"
    public static let builtinBytes: UInt64 = 1_185_376
    public static let builtinSHA256 = "270cba1bd5109f42d03350f60406024560464db173c0e387d91f0426d3bd256d"
    public static let maximumFileBytes: UInt64 = 16 * 1024 * 1024 * 1024

    private struct StoredCatalog: Codable {
        var version = 1
        var entries: [AILocalModelEntry] = []
        var selectedModelID: String? = AILocalModelStore.builtinID
        var selectedProjectorID: String?
    }

    private let directory: URL
    private let builtinSource: URL?
    private var catalog = StoredCatalog()
    private var loaded = false
    private var importing = false
    private var builtinURL: URL?

    /// 自定义目录只用于隔离的普通 Foundation 测试；正式文件位于 App Documents 下，
    /// 不在只读 Bundle 中，因此 Agent 的 NSHomeDirectory 文件边界也能访问同一份模型。
    public init(directory: URL? = nil, builtinSource: URL? = nil) {
        self.directory = directory ?? Self.defaultDirectory
        self.builtinSource = builtinSource ?? Bundle.main.url(
            forResource: "stories260K", withExtension: "gguf", subdirectory: "MaiLocalModels")
    }

    private static var defaultDirectory: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("MaiLocalModels", isDirectory: true)
    }

    /// 后端配置可在自己的文件工作队列调用。仅复制固定 1.2 MB 测试资源并核 SHA；
    /// 目标已存在但不匹配时返回 nil，绝不覆盖，不能把已被加载的 mmap 文件替换掉。
    public static func ensureBundledTestModel(bundle: Bundle = .main) -> URL? {
        guard let source = bundle.url(forResource: "stories260K", withExtension: "gguf",
                                      subdirectory: "MaiLocalModels") else { return nil }
        return try? installBundledTestModel(source: source, directory: defaultDirectory)
    }

    public static func installBundledTestModel(source: URL, directory: URL) throws -> URL {
        try prepareDirectory(directory)
        let destination = directory.appendingPathComponent("stories260K.gguf")
        if FileManager.default.fileExists(atPath: destination.path) {
            let existing = try fingerprint(destination)
            guard existing.bytes == builtinBytes, existing.hash == builtinSHA256 else {
                throw AILocalModelStoreError.unavailable
            }
            return destination
        }
        let copied: (bytes: UInt64, hash: String)
        do {
            copied = try copyValidatedFile(source: source, directory: directory,
                                           filename: "stories260K.gguf")
        } catch {
            // 后端配置与普通页面可能同时 ensure；另一个调用已原子发布正确文件时复用，
            // 不能为了消除竞争去 replace/remove 已驻留文件。
            if let existing = try? fingerprint(destination), existing.bytes == builtinBytes,
               existing.hash == builtinSHA256 { return destination }
            throw error
        }
        guard copied.bytes == builtinBytes, copied.hash == builtinSHA256 else {
            // 失败文件尚未发布。copyValidatedFile 的期望值守卫确保这里不会删除已存在文件。
            throw AILocalModelStoreError.unavailable
        }
        return destination
    }

    public func snapshot() throws -> AILocalModelCatalog {
        try loadIfNeeded()
        if builtinURL == nil, let builtinSource {
            builtinURL = try? Self.installBundledTestModel(source: builtinSource, directory: directory)
        }
        return AILocalModelCatalog(entries: (builtinEntry.map { [$0] } ?? []) + catalog.entries,
            selectedModelID: catalog.selectedModelID,
            selectedProjectorID: catalog.selectedProjectorID, builtinAvailable: builtinURL != nil)
    }

    public func selectModel(id: String) throws {
        try loadIfNeeded()
        guard (id == Self.builtinID && builtinURL != nil) ||
                catalog.entries.contains(where: { $0.id == id && $0.role == .model }) else {
            throw AILocalModelStoreError.unavailable
        }
        var next = catalog
        next.selectedModelID = id
        if id == Self.builtinID { next.selectedProjectorID = nil }
        try save(next)
    }

    public func selectProjector(id: String?) throws {
        try loadIfNeeded()
        guard id == nil || catalog.entries.contains(where: { $0.id == id && $0.role == .projector }) else {
            throw AILocalModelStoreError.unavailable
        }
        var next = catalog
        next.selectedProjectorID = id
        try save(next)
    }

    public func selection() throws -> AILocalModelSelection {
        try loadIfNeeded()
        let model: URL
        let builtin = catalog.selectedModelID == Self.builtinID
        if builtin, let builtinURL { model = builtinURL }
        else { model = try url(for: catalog.selectedModelID, role: .model) }
        let projector = try catalog.selectedProjectorID.map { try url(for: $0, role: .projector) }
        return AILocalModelSelection(modelURL: model, projectorURL: projector, isBuiltin: builtin)
    }

    /// 主模型按账号设置保存的稳定 ID 解析；独立自测的全局选择不能更改已应用的主模型。
    /// 每次配置复查文件存在性，返回沙盒内文件，不保存旧进程的绝对路径。
    public func resolve(modelID: String, projectorID: String?) throws -> AILocalModelSelection {
        _ = try snapshot()
        let builtin = modelID == Self.builtinID
        let model: URL
        if builtin, let builtinURL { model = builtinURL }
        else { model = try url(for: modelID, role: .model) }
        let projector = try projectorID.map { try url(for: $0, role: .projector) }
        return AILocalModelSelection(modelURL: model, projectorURL: projector, isBuiltin: builtin)
    }

    /// 64 KiB 流式复制和哈希不占 MainActor；取消必须等 worker 删除自己的临时文件后返回。
    /// security scope 覆盖完整复制阶段，目录只保存相对 UUID 文件名与导入时 SHA，不保存外部 URL。
    @discardableResult
    public func importModel(from source: URL, role: AILocalModelRole) async throws -> AILocalModelEntry {
        try loadIfNeeded()
        guard !importing else { throw AILocalModelStoreError.busy }
        guard catalog.entries.count < 128 else { throw AILocalModelStoreError.catalogFull }
        importing = true
        defer { importing = false }
        let id = UUID().uuidString
        let destinationDirectory = directory
        let worker = Task.detached(priority: .userInitiated) {
            let scoped = source.startAccessingSecurityScopedResource()
            defer { if scoped { source.stopAccessingSecurityScopedResource() } }
            let result = try Self.copyValidatedFile(source: source, directory: destinationDirectory,
                                                     filename: id + ".gguf")
            return AILocalModelEntry(id: id,
                name: String(source.lastPathComponent.prefix(120)), filename: id + ".gguf",
                bytes: result.bytes, sha256: result.hash, role: role, isBuiltin: false)
        }
        let entry = try await withTaskCancellationHandler {
            try await worker.value
        } onCancel: { worker.cancel() }
        var next = catalog
        next.entries.append(entry)
        if role == .model { next.selectedModelID = entry.id }
        else { next.selectedProjectorID = entry.id }
        do { try save(next) }
        catch {
            // 新文件尚未向调用方交付，不能驻留于 runtime；只清理本次新建的 UUID。
            try? FileManager.default.removeItem(at: directory.appendingPathComponent(entry.filename))
            throw error
        }
        return entry
    }

    private var builtinEntry: AILocalModelEntry? {
        guard builtinURL != nil else { return nil }
        return AILocalModelEntry(id: Self.builtinID, name: "内置英文测试模型 · 1.2 MB",
            filename: "stories260K.gguf", bytes: Self.builtinBytes, sha256: Self.builtinSHA256,
            role: .model, isBuiltin: true)
    }

    private func url(for id: String?, role: AILocalModelRole) throws -> URL {
        guard let entry = catalog.entries.first(where: { $0.id == id && $0.role == role }) else {
            throw AILocalModelStoreError.unavailable
        }
        let result = directory.appendingPathComponent(entry.filename)
        let values = try Self.uncachedValues(result, keys: [.isRegularFileKey, .isSymbolicLinkKey, .fileSizeKey])
        guard values.isRegularFile == true, values.isSymbolicLink != true,
              values.fileSize.map({ UInt64($0) }) == entry.bytes else {
            throw AILocalModelStoreError.unavailable
        }
        return result
    }

    private func loadIfNeeded() throws {
        guard !loaded else { return }
        try Self.prepareDirectory(directory)
        let file = directory.appendingPathComponent("catalog.json")
        if FileManager.default.fileExists(atPath: file.path) {
            let values = try Self.uncachedValues(file, keys: [.isRegularFileKey, .isSymbolicLinkKey, .fileSizeKey])
            guard values.isRegularFile == true, values.isSymbolicLink != true,
                  let bytes = values.fileSize, bytes <= 256 * 1024 else {
                throw AILocalModelStoreError.invalidCatalog
            }
            let reader = try FileHandle(forReadingFrom: file)
            defer { try? reader.close() }
            let data = try reader.read(upToCount: 256 * 1024 + 1) ?? Data()
            guard data.count <= 256 * 1024 else { throw AILocalModelStoreError.invalidCatalog }
            let decoded = try JSONDecoder().decode(StoredCatalog.self, from: data)
            guard decoded.version == 1, decoded.entries.count <= 128,
                  Set(decoded.entries.map(\.id)).count == decoded.entries.count,
                  decoded.entries.allSatisfy({ entry in
                      UUID(uuidString: entry.id) != nil && entry.filename == entry.id + ".gguf" &&
                      !entry.isBuiltin && entry.name.utf8.count <= 512 && entry.bytes > 0 &&
                      entry.bytes <= Self.maximumFileBytes && entry.sha256.count == 64 &&
                      entry.sha256.allSatisfy({ $0.isHexDigit && $0.isASCII })
                  }) else { throw AILocalModelStoreError.invalidCatalog }
            catalog = decoded
        }
        if let builtinSource {
            builtinURL = try? Self.installBundledTestModel(source: builtinSource, directory: directory)
        }
        loaded = true
    }

    private func save(_ next: StoredCatalog) throws {
        let data = try JSONEncoder().encode(next)
        guard data.count <= 256 * 1024 else { throw AILocalModelStoreError.invalidCatalog }
        try data.write(to: directory.appendingPathComponent("catalog.json"), options: .atomic)
        catalog = next
    }

    private static func prepareDirectory(_ directory: URL) throws {
        guard directory.isFileURL else { throw AILocalModelStoreError.invalidCatalog }
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let values = try uncachedValues(directory, keys: [.isDirectoryKey, .isSymbolicLinkKey])
        guard values.isDirectory == true, values.isSymbolicLink != true else {
            throw AILocalModelStoreError.invalidCatalog
        }
    }

    private static func fingerprint(_ source: URL) throws -> (bytes: UInt64, hash: String) {
        guard source.isFileURL else { throw AILocalModelStoreError.invalidFile }
        let values = try uncachedValues(source, keys: [.isRegularFileKey, .isSymbolicLinkKey, .fileSizeKey])
        guard source.isFileURL, values.isRegularFile == true, values.isSymbolicLink != true,
              let bytes = values.fileSize, bytes >= 32 else { throw AILocalModelStoreError.invalidFile }
        guard UInt64(bytes) <= maximumFileBytes else { throw AILocalModelStoreError.tooLarge }
        let handle = try FileHandle(forReadingFrom: source)
        defer { try? handle.close() }
        var hash = SHA256()
        var count: UInt64 = 0
        while let chunk = try handle.read(upToCount: 64 * 1024), !chunk.isEmpty {
            try Task.checkCancellation()
            count += UInt64(chunk.count)
            guard count <= UInt64(bytes) else { throw AILocalModelStoreError.invalidFile }
            hash.update(data: chunk)
        }
        guard count == UInt64(bytes) else { throw AILocalModelStoreError.invalidFile }
        return (count, hash.finalize().map { String(format: "%02x", $0) }.joined())
    }

    private static func copyValidatedFile(source: URL, directory: URL,
                                          filename: String) throws -> (bytes: UInt64, hash: String) {
        guard source.isFileURL else { throw AILocalModelStoreError.invalidFile }
        let values = try uncachedValues(source, keys: [.isRegularFileKey, .isSymbolicLinkKey,
                                                       .fileSizeKey, .contentModificationDateKey])
        guard source.isFileURL, values.isRegularFile == true, values.isSymbolicLink != true,
              let size = values.fileSize, size >= 32 else { throw AILocalModelStoreError.invalidFile }
        guard UInt64(size) <= maximumFileBytes else { throw AILocalModelStoreError.tooLarge }
        try validateGGUF(source, size: UInt64(size))
        try prepareDirectory(directory)
        let staging = directory.appendingPathComponent(".import-" + UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: staging, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: staging) }
        let temporary = staging.appendingPathComponent("model.gguf")
        guard FileManager.default.createFile(atPath: temporary.path, contents: nil) else {
            throw AILocalModelStoreError.unavailable
        }
        let input = try FileHandle(forReadingFrom: source)
        defer { try? input.close() }
        let output = try FileHandle(forWritingTo: temporary)
        defer { try? output.close() }
        var hash = SHA256()
        var bytes: UInt64 = 0
        while let chunk = try input.read(upToCount: 64 * 1024), !chunk.isEmpty {
            try Task.checkCancellation()
            bytes += UInt64(chunk.count)
            guard bytes <= UInt64(size) else { throw AILocalModelStoreError.invalidFile }
            try output.write(contentsOf: chunk)
            hash.update(data: chunk)
        }
        try output.synchronize()
        try output.close()
        let end = try uncachedValues(source, keys: [.fileSizeKey, .contentModificationDateKey])
        guard bytes == UInt64(size), end.fileSize == values.fileSize,
              end.contentModificationDate == values.contentModificationDate else {
            throw AILocalModelStoreError.invalidFile
        }
        try validateGGUF(temporary, size: bytes)
        let digest = hash.finalize().map { String(format: "%02x", $0) }.joined()
        if filename == "stories260K.gguf" {
            guard bytes == builtinBytes, digest == builtinSHA256 else {
                throw AILocalModelStoreError.unavailable
            }
        }
        try Task.checkCancellation()
        // moveItem 拒绝已存在的目标；没有 remove/replace，也不写外部源文件。
        try FileManager.default.moveItem(at: temporary, to: directory.appendingPathComponent(filename))
        return (bytes, digest)
    }

    private static func uncachedValues(_ url: URL, keys: Set<URLResourceKey>) throws -> URLResourceValues {
        // Files provider 或同路径二次导入可改变文件大小；URL 的缓存不能作为复制前后校验依据。
        var fresh = url
        fresh.removeAllCachedResourceValues()
        return try fresh.resourceValues(forKeys: keys)
    }

    // 只做有界容器检查，不加载张量、不把“GGUF 可读”说成架构/内存兼容。最多扫描 64 MB
    // 元数据、64K 键、100K 张量描述和 1M 数组元素，损坏长度不会引发巨量分配。
    private static func validateGGUF(_ file: URL, size: UInt64) throws {
        let reader = try GGUFReader(file: file, size: size)
        guard try reader.read(4) == Data("GGUF".utf8) else { throw AILocalModelStoreError.invalidGGUF }
        let version = try reader.number(4)
        let tensors = try reader.number(8)
        let metadata = try reader.number(8)
        guard (version == 2 || version == 3), tensors > 0, tensors <= 100_000,
              metadata <= 65_536 else { throw AILocalModelStoreError.invalidGGUF }
        for _ in 0..<metadata {
            try reader.string(maximum: 4_096)
            try reader.value(type: reader.number(4))
        }
        for _ in 0..<tensors {
            try reader.string(maximum: 4_096)
            let dimensions = try reader.number(4)
            guard (1...4).contains(dimensions) else { throw AILocalModelStoreError.invalidGGUF }
            for _ in 0..<dimensions {
                guard try reader.number(8) > 0 else { throw AILocalModelStoreError.invalidGGUF }
            }
            _ = try reader.number(4)
            guard try reader.number(8) < size else { throw AILocalModelStoreError.invalidGGUF }
        }
        guard reader.offset < size else { throw AILocalModelStoreError.invalidGGUF }
    }

    private final class GGUFReader {
        let handle: FileHandle
        let size: UInt64
        var offset: UInt64 = 0
        init(file: URL, size: UInt64) throws { handle = try FileHandle(forReadingFrom: file); self.size = size }
        deinit { try? handle.close() }
        func read(_ count: Int) throws -> Data {
            guard count >= 0, offset + UInt64(count) <= min(size, 64 * 1024 * 1024),
                  let data = try handle.read(upToCount: count), data.count == count else {
                throw AILocalModelStoreError.invalidGGUF
            }
            offset += UInt64(count)
            return data
        }
        func number(_ count: Int) throws -> UInt64 {
            let data = try read(count)
            return data.enumerated().reduce(0) { $0 | UInt64($1.element) << ($1.offset * 8) }
        }
        func skip(_ count: UInt64) throws {
            guard count <= 64 * 1024 * 1024, offset <= min(size, 64 * 1024 * 1024),
                  count <= min(size, 64 * 1024 * 1024) - offset else {
                throw AILocalModelStoreError.invalidGGUF
            }
            offset += count
            try handle.seek(toOffset: offset)
        }
        func string(maximum: UInt64) throws {
            let length = try number(8)
            guard length <= maximum else { throw AILocalModelStoreError.invalidGGUF }
            try skip(length)
        }
        func value(type: UInt64) throws {
            let widths: [UInt64: UInt64] = [0: 1, 1: 1, 2: 2, 3: 2, 4: 4, 5: 4,
                                           6: 4, 7: 1, 10: 8, 11: 8, 12: 8]
            if let width = widths[type] { try skip(width) }
            else if type == 8 { try string(maximum: 16 * 1024 * 1024) }
            else if type == 9 {
                let element = try number(4), count = try number(8)
                guard count <= 1_000_000 else { throw AILocalModelStoreError.invalidGGUF }
                if let width = widths[element] { try skip(count * width) }
                else if element == 8 {
                    for _ in 0..<count { try string(maximum: 1024 * 1024) }
                } else { throw AILocalModelStoreError.invalidGGUF }
            } else { throw AILocalModelStoreError.invalidGGUF }
        }
    }
}
