// 云盘原图和缩略图使用同一份私有快照；只编码小 JPEG，不改变用户的原始文件。
import Foundation
import CryptoKit
import CoreGraphics
import ImageIO
import UniformTypeIdentifiers

enum MaiCloudDriveImageThumbnail {
    private struct Failure: LocalizedError {
        let detail: String
        var errorDescription: String? { detail }
    }
    struct Prepared {
        let source: URL
        let jpeg: URL
    }

    private struct Receipt: Codable {
        let version: Int
        let sourceID: String
        let size: UInt64
        let sourceSHA256: String
        let jpegSHA256: String
    }

    private struct Files {
        let source: URL
        let jpeg: URL
        let receipt: URL
        init(_ checkpoint: URL) {
            source = URL(fileURLWithPath: checkpoint.path + ".cloud-original")
            jpeg = URL(fileURLWithPath: checkpoint.path + ".cloud-thumb.jpg")
            receipt = URL(fileURLWithPath: checkpoint.path + ".cloud-thumb.json")
        }
    }

    // ImageIO 可能需要解码原图。只让一个准备任务进入，传输本身不占用此锁。
    private static let preparationLock = NSLock()
    private static let maximumSourceBytes: UInt64 = 100_000_000
    private static let maximumSourcePixels: Int64 = 64_000_000
    private static let maximumJPEGBytes = 2_000_000

    static func prepare(checkpoint: URL, sourceID: String, size: UInt64,
                        sourceURL: () throws -> URL,
                        isCanceled: () -> Bool) throws -> Prepared? {
        preparationLock.lock()
        defer { preparationLock.unlock() }
        try checkCancellation(isCanceled)
        guard checkpoint.isFileURL, checkpoint.path.hasPrefix("/"),
              !sourceID.isEmpty, size > 0, size <= maximumSourceBytes else {
            throw failure("图片文件信息无效")
        }
        let files = Files(checkpoint)
        let manager = FileManager.default
        if manager.fileExists(atPath: files.receipt.path) {
            let metadata = try freshValues(files.receipt,
                forKeys: [.fileSizeKey, .isRegularFileKey, .isSymbolicLinkKey])
            guard metadata.isRegularFile == true, metadata.isSymbolicLink != true,
                  let bytes = metadata.fileSize, bytes > 0, bytes <= 16_384,
                  let value = try? JSONDecoder().decode(
                    Receipt.self, from: Data(contentsOf: files.receipt)) else {
                throw failure("上传缓存已损坏，请取消该上传后重新选择图片")
            }
            guard value.version == 1, value.sourceID == sourceID, value.size == size,
                  try digest(files.source, expectedSize: size, isCanceled: isCanceled) == value.sourceSHA256 else {
                throw failure("上传缓存已变化，请取消该上传后重新选择图片")
            }
            if !manager.fileExists(atPath: checkpoint.path) {
                guard try jpegDigest(files.jpeg) == value.jpegSHA256 else {
                    throw failure("上传缓存已变化，请取消该上传后重新选择图片")
                }
            }
            // 已有核心 checkpoint 时，是否仍需本地 JPEG 由核心的远端 ready 状态决定。
            // pending 仍由核心严格核对原指纹；宿主不再读私有字段或阻断已 ready 的恢复。
            // 暂停后继续使用首次选择的原图快照和同一 JPEG；不重新压缩或换内容。
            return Prepared(source: files.source, jpeg: files.jpeg)
        }
        if manager.fileExists(atPath: checkpoint.path) {
            // 已有小图的旧 checkpoint 保留原上传方式，避免把旧断点切成新 source 模式。
            guard size <= 20_000_000 else {
                throw failure("上传缩略图已缺失，请取消该上传后重新选择图片")
            }
            return nil
        }

        let original = try sourceURL()
        guard original.isFileURL else { throw failure("图片文件无法访问") }
        let scoped = original.startAccessingSecurityScopedResource()
        defer { if scoped { original.stopAccessingSecurityScopedResource() } }
        try manager.createDirectory(at: checkpoint.deletingLastPathComponent(),
                                    withIntermediateDirectories: true)
        if let available = try? checkpoint.deletingLastPathComponent().resourceValues(
            forKeys: [.volumeAvailableCapacityForImportantUsageKey])
            .volumeAvailableCapacityForImportantUsage,
           available >= 0, UInt64(available) < size + UInt64(maximumJPEGBytes) {
            throw failure("设备空间不足，无法准备图片上传")
        }
        let temporary = checkpoint.deletingLastPathComponent()
            .appendingPathComponent(".cloud-image-" + UUID().uuidString)
        defer { try? manager.removeItem(at: temporary) }
        let input = try FileHandle(forReadingFrom: original)
        defer { try? input.close() }
        guard manager.createFile(atPath: temporary.path, contents: nil,
                                 attributes: [.posixPermissions: 0o600]) else {
            throw failure("无法创建图片上传缓存")
        }
        let output = try FileHandle(forWritingTo: temporary)
        defer { try? output.close() }
        var hash = SHA256()
        var count: UInt64 = 0
        while let bytes = try input.read(upToCount: 1_048_576), !bytes.isEmpty {
            try checkCancellation(isCanceled)
            count += UInt64(bytes.count)
            guard count <= size else { throw failure("源图片已变化，请重新选择") }
            hash.update(data: bytes)
            try output.write(contentsOf: bytes)
        }
        try output.synchronize()
        guard count == size else { throw failure("源图片已变化，请重新选择") }
        let sourceDigest = hex(hash.finalize())
        guard try digest(original, expectedSize: size, isCanceled: isCanceled) == sourceDigest else {
            throw failure("源图片已变化，请重新选择")
        }
        try checkCancellation(isCanceled)
        let jpeg = try encode(temporary)
        try checkCancellation(isCanceled)

        // 发布标记最后写入。崩溃前未完成的临时文件不会被当作可续传原件。
        // 旧网络操作尚未开始，只有本次 checkpoint 派生的固定文件名可以覆盖。
        for path in [files.source, files.jpeg] where manager.fileExists(atPath: path.path) {
            try manager.removeItem(at: path)
        }
        try manager.moveItem(at: temporary, to: files.source)
        try jpeg.write(to: files.jpeg, options: .atomic)
        let value = Receipt(version: 1, sourceID: sourceID, size: size,
                            sourceSHA256: sourceDigest, jpegSHA256: hex(SHA256.hash(data: jpeg)))
        try JSONEncoder().encode(value).write(to: files.receipt, options: .atomic)
        return Prepared(source: files.source, jpeg: files.jpeg)
    }

    // 调用方仅在持久 completed 回执或确认 abort 后使用；暂停和普通错误不得调用。
    static func removeAfterCompletionOrAbort(checkpoint: URL) throws {
        // 同一上传的准备/传输此时已经结束。不占用全局编码准备锁，
        // 避免 MainActor 的完成清理被其它文件提供者的慢速读取阻塞。
        let files = Files(checkpoint)
        for url in [files.jpeg, files.source, files.receipt]
            where FileManager.default.fileExists(atPath: url.path) {
            let values = try freshValues(url, forKeys: [.isRegularFileKey, .isSymbolicLinkKey])
            guard values.isRegularFile == true, values.isSymbolicLink != true else {
                throw failure("上传缓存类型异常，已保留文件")
            }
            try FileManager.default.removeItem(at: url)
        }
    }

    private static func encode(_ source: URL) throws -> Data {
        guard let imageSource = CGImageSourceCreateWithURL(
            source as CFURL, [kCGImageSourceShouldCache: false] as CFDictionary),
              CGImageSourceGetCount(imageSource) > 0,
              CGImageSourceGetStatus(imageSource) == .statusComplete,
              CGImageSourceGetStatusAtIndex(imageSource, 0) == .statusComplete,
              let properties = CGImageSourceCopyPropertiesAtIndex(
                imageSource, 0, nil) as? [String: Any],
              let width = properties[kCGImagePropertyPixelWidth as String] as? Int,
              let height = properties[kCGImagePropertyPixelHeight as String] as? Int,
              width > 0, height > 0, width <= 20_000, height <= 20_000,
              Int64(width) * Int64(height) <= maximumSourcePixels,
              let decoded = CGImageSourceCreateThumbnailAtIndex(imageSource, 0, [
                kCGImageSourceCreateThumbnailFromImageAlways: true,
                kCGImageSourceCreateThumbnailWithTransform: true,
                kCGImageSourceThumbnailMaxPixelSize: 320,
                kCGImageSourceShouldCacheImmediately: true,
              ] as CFDictionary),
              decoded.width > 0, decoded.height > 0,
              decoded.width <= 320, decoded.height <= 320,
              let bitmap = CGContext(data: nil, width: decoded.width, height: decoded.height,
                bitsPerComponent: 8, bytesPerRow: decoded.width * 4,
                space: CGColorSpaceCreateDeviceRGB(),
                bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue) else {
            throw failure("图片无法生成缩略图，请使用可正常预览的图片")
        }
        // JPEG 没有透明通道；只为派生小图合成白色底，用户原图仍逐字节保留。
        bitmap.setFillColor(CGColor(gray: 1, alpha: 1))
        bitmap.fill(CGRect(x: 0, y: 0, width: decoded.width, height: decoded.height))
        bitmap.draw(decoded, in: CGRect(x: 0, y: 0, width: decoded.width, height: decoded.height))
        let bytes = NSMutableData()
        guard let image = bitmap.makeImage(),
              let destination = CGImageDestinationCreateWithData(
                bytes, UTType.jpeg.identifier as CFString, 1, nil) else {
            throw failure("图片缩略图编码失败")
        }
        CGImageDestinationAddImage(destination, image, [
            kCGImageDestinationLossyCompressionQuality: 0.8,
            kCGImagePropertyOrientation: 1,
        ] as CFDictionary)
        guard CGImageDestinationFinalize(destination), bytes.length > 0,
              bytes.length <= maximumJPEGBytes else { throw failure("图片缩略图编码失败") }
        return bytes as Data
    }

    private static func jpegDigest(_ url: URL) throws -> String {
        let values = try freshValues(url, forKeys: [.fileSizeKey, .isRegularFileKey, .isSymbolicLinkKey])
        guard values.isRegularFile == true, values.isSymbolicLink != true,
              let size = values.fileSize, size > 0, size <= maximumJPEGBytes else {
            throw failure("上传缩略图已缺失或损坏")
        }
        let bytes = try Data(contentsOf: url)
        guard let source = CGImageSourceCreateWithData(bytes as CFData, nil),
              CGImageSourceGetType(source) as String? == UTType.jpeg.identifier,
              let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [String: Any],
              let width = properties[kCGImagePropertyPixelWidth as String] as? Int,
              let height = properties[kCGImagePropertyPixelHeight as String] as? Int,
              width > 0, height > 0, width <= 320, height <= 320 else {
            throw failure("上传缩略图已缺失或损坏")
        }
        return hex(SHA256.hash(data: bytes))
    }

    private static func digest(_ url: URL, expectedSize: UInt64,
                               isCanceled: () -> Bool) throws -> String {
        let values = try freshValues(url, forKeys: [.fileSizeKey, .isRegularFileKey, .isSymbolicLinkKey])
        guard values.isRegularFile == true, values.isSymbolicLink != true,
              let size = values.fileSize, size >= 0, UInt64(size) == expectedSize else {
            throw failure("上传原图已变化或缺失")
        }
        let file = try FileHandle(forReadingFrom: url)
        defer { try? file.close() }
        var hash = SHA256()
        var count: UInt64 = 0
        while let bytes = try file.read(upToCount: 1_048_576), !bytes.isEmpty {
            try checkCancellation(isCanceled)
            count += UInt64(bytes.count)
            guard count <= expectedSize else { throw failure("上传原图已变化") }
            hash.update(data: bytes)
        }
        guard count == expectedSize else { throw failure("上传原图已变化") }
        return hex(hash.finalize())
    }

    private static func checkCancellation(_ isCanceled: () -> Bool) throws {
        if isCanceled() { throw CancellationError() }
    }
    private static func freshValues(_ url: URL, forKeys keys: Set<URLResourceKey>)
        throws -> URLResourceValues {
        // 文件提供者或同一路径可能已经变化；NSURL 会缓存 fileSize 等属性。
        var fresh = url
        fresh.removeAllCachedResourceValues()
        return try fresh.resourceValues(forKeys: keys)
    }
    private static func hex<D: Sequence>(_ digest: D) -> String where D.Element == UInt8 {
        digest.map { String(format: "%02x", $0) }.joined()
    }
    private static func failure(_ detail: String) -> Failure {
        Failure(detail: detail)
    }
}
