// 仅验证 Apple 宿主的小图、快照和文件生命周期，不重复共享上传协议用例。
import XCTest
import UIKit
import Foundation
import CoreGraphics
import ImageIO
import UniformTypeIdentifiers
@testable import MaiChatCore

final class CloudDriveImageThumbnailTests: XCTestCase {
    private func directory() throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(at: url) }
        return url
    }

    private func image(_ url: URL, color: CGColor = CGColor(red: 1, green: 0, blue: 0, alpha: 1))
        throws -> UInt64 {
        let context = try XCTUnwrap(CGContext(data: nil, width: 1024, height: 512,
            bitsPerComponent: 8, bytesPerRow: 4096, space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue))
        context.setFillColor(color)
        context.fill(CGRect(x: 0, y: 0, width: 1024, height: 512))
        let destination = try XCTUnwrap(CGImageDestinationCreateWithURL(
            url as CFURL, UTType.png.identifier as CFString, 1, nil))
        CGImageDestinationAddImage(destination, try XCTUnwrap(context.makeImage()), nil)
        XCTAssertTrue(CGImageDestinationFinalize(destination))
        return UInt64(try url.resourceValues(forKeys: [.fileSizeKey]).fileSize!)
    }

    private func prepare(_ original: URL, checkpoint: URL, size: UInt64) throws
        -> MaiCloudDriveImageThumbnail.Prepared {
        guard let result = try MaiCloudDriveImageThumbnail.prepare(
            checkpoint: checkpoint, sourceID: "owned-image", size: size,
            sourceURL: { original }, isCanceled: { false }) else {
            throw NSError(domain: "owned-thumbnail-fixture", code: 1)
        }
        return result
    }

    func testCreatesSmallJPEGAndRetainsOriginalBytes() throws {
        let root = try directory(), original = root.appendingPathComponent("owned.png")
        let count = try image(original), before = try Data(contentsOf: original)
        let checkpoint = root.appendingPathComponent("upload.maicloud.json")
        let result = try prepare(original, checkpoint: checkpoint, size: count)
        XCTAssertEqual(try Data(contentsOf: original), before)
        XCTAssertEqual(try Data(contentsOf: result.source), before)
        let data = try Data(contentsOf: result.jpeg)
        XCTAssertLessThanOrEqual(data.count, 2_000_000)
        let decoder = try XCTUnwrap(CGImageSourceCreateWithData(data as CFData, nil))
        XCTAssertEqual(CGImageSourceGetType(decoder) as String?, UTType.jpeg.identifier)
        let bitmap = try XCTUnwrap(CGImageSourceCreateImageAtIndex(decoder, 0, nil))
        XCTAssertEqual(bitmap.width, 320)
        XCTAssertEqual(bitmap.height, 160)
        let picture = XCTAttachment(image: UIImage(cgImage: bitmap))
        picture.name = "owned-cloud-small-jpeg"
        picture.lifetime = .keepAlways
        add(picture)
    }

    func testLargeEncodedOriginalUsesTheSameBoundedJPEGPath() throws {
        let root = try directory(), original = root.appendingPathComponent("large.png")
        _ = try image(original)
        let file = try FileHandle(forWritingTo: original)
        try file.truncate(atOffset: 21_000_001)
        try file.close()
        let result = try prepare(original, checkpoint: root.appendingPathComponent("large.maicloud.json"),
                                 size: 21_000_001)
        XCTAssertEqual(try result.source.resourceValues(forKeys: [.fileSizeKey]).fileSize, 21_000_001)
        XCTAssertLessThan(try Data(contentsOf: result.jpeg).count, 100_000)
    }

    func testResumeKeepsTheFirstSnapshotAndExactJPEG() throws {
        let root = try directory(), original = root.appendingPathComponent("owned.png")
        let count = try image(original), checkpoint = root.appendingPathComponent("upload.maicloud.json")
        let first = try prepare(original, checkpoint: checkpoint, size: count)
        let jpeg = try Data(contentsOf: first.jpeg), raw = try Data(contentsOf: first.source)
        _ = try image(original, color: CGColor(red: 0, green: 0, blue: 1, alpha: 1))
        let resumed = try XCTUnwrap(MaiCloudDriveImageThumbnail.prepare(
            checkpoint: checkpoint, sourceID: "owned-image", size: count,
            sourceURL: { XCTFail("Resume must use the retained snapshot"); return original },
            isCanceled: { false }))
        XCTAssertEqual(first.source, resumed.source)
        XCTAssertEqual(first.jpeg, resumed.jpeg)
        XCTAssertEqual(try Data(contentsOf: resumed.source), raw)
        XCTAssertEqual(try Data(contentsOf: resumed.jpeg), jpeg)
    }

    func testCorruptJPEGAndWrongIdentityFailWithoutReplacingRetainedData() throws {
        let root = try directory(), original = root.appendingPathComponent("owned.png")
        let count = try image(original), checkpoint = root.appendingPathComponent("upload.maicloud.json")
        let result = try prepare(original, checkpoint: checkpoint, size: count)
        XCTAssertThrowsError(try MaiCloudDriveImageThumbnail.prepare(
            checkpoint: checkpoint, sourceID: "other-owner", size: count,
            sourceURL: { original }, isCanceled: { false }))
        let corrupt = Data("owned corrupted JPEG".utf8)
        try corrupt.write(to: result.jpeg)
        XCTAssertThrowsError(try prepare(original, checkpoint: checkpoint, size: count))
        XCTAssertEqual(try Data(contentsOf: result.jpeg), corrupt)
        XCTAssertTrue(FileManager.default.fileExists(atPath: result.source.path))
    }

    func testPauseKeepsFilesAndConfirmedCleanupPreservesUserOriginal() throws {
        let root = try directory(), original = root.appendingPathComponent("owned.png")
        let count = try image(original), checkpoint = root.appendingPathComponent("upload.maicloud.json")
        let result = try prepare(original, checkpoint: checkpoint, size: count)
        XCTAssertThrowsError(try MaiCloudDriveImageThumbnail.prepare(
            checkpoint: checkpoint, sourceID: "owned-image", size: count,
            sourceURL: { original }, isCanceled: { true })) {
            XCTAssertTrue($0 is CancellationError)
        }
        XCTAssertTrue(FileManager.default.fileExists(atPath: result.jpeg.path))
        XCTAssertTrue(FileManager.default.fileExists(atPath: result.source.path))
        try MaiCloudDriveImageThumbnail.removeAfterCompletionOrAbort(checkpoint: checkpoint)
        XCTAssertFalse(FileManager.default.fileExists(atPath: result.jpeg.path))
        XCTAssertFalse(FileManager.default.fileExists(atPath: result.source.path))
        XCTAssertTrue(FileManager.default.fileExists(atPath: original.path))
    }

    func testLegacySmallCheckpointKeepsItsOriginalUploadMode() throws {
        let root = try directory(), checkpoint = root.appendingPathComponent("legacy.maicloud.json")
        try Data("{}".utf8).write(to: checkpoint)
        XCTAssertNil(try MaiCloudDriveImageThumbnail.prepare(
            checkpoint: checkpoint, sourceID: "legacy", size: 19_000_000,
            sourceURL: { XCTFail("Legacy checkpoint does not prepare another source"); return root },
            isCanceled: { false }))
        XCTAssertThrowsError(try MaiCloudDriveImageThumbnail.prepare(
            checkpoint: checkpoint, sourceID: "missing-thumb", size: 21_000_000,
            sourceURL: { root }, isCanceled: { false }))
    }

    func testOversizeAndCanceledRequestsDoNotOpenTheOriginal() throws {
        let root = try directory(), checkpoint = root.appendingPathComponent("upload.maicloud.json")
        XCTAssertThrowsError(try MaiCloudDriveImageThumbnail.prepare(
            checkpoint: checkpoint, sourceID: "oversize", size: 100_000_001,
            sourceURL: { XCTFail("Oversize is rejected before opening"); return root },
            isCanceled: { false }))
        XCTAssertThrowsError(try MaiCloudDriveImageThumbnail.prepare(
            checkpoint: checkpoint, sourceID: "canceled", size: 1,
            sourceURL: { XCTFail("Canceled is rejected before opening"); return root },
            isCanceled: { true }))
    }

    func testInvalidImageDoesNotPublishAPreparedReceipt() throws {
        let root = try directory(), original = root.appendingPathComponent("invalid.png")
        try Data("not an image".utf8).write(to: original)
        let checkpoint = root.appendingPathComponent("upload.maicloud.json")
        XCTAssertThrowsError(try prepare(original, checkpoint: checkpoint, size: 12))
        XCTAssertFalse(FileManager.default.fileExists(atPath: checkpoint.path + ".cloud-thumb.json"))
        XCTAssertFalse(FileManager.default.fileExists(atPath: checkpoint.path + ".cloud-thumb.jpg"))
    }

    func testEXIFOrientationIsAppliedOnceToTheSmallJPEG() throws {
        let root = try directory(), original = root.appendingPathComponent("rotated.jpg")
        let bitmap = try XCTUnwrap(CGContext(data: nil, width: 1024, height: 512,
            bitsPerComponent: 8, bytesPerRow: 4096, space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue))
        bitmap.setFillColor(CGColor(red: 0, green: 1, blue: 0, alpha: 1))
        bitmap.fill(CGRect(x: 0, y: 0, width: 1024, height: 512))
        let destination = try XCTUnwrap(CGImageDestinationCreateWithURL(
            original as CFURL, UTType.jpeg.identifier as CFString, 1, nil))
        CGImageDestinationAddImage(destination, try XCTUnwrap(bitmap.makeImage()), [
            kCGImagePropertyOrientation: 6,
        ] as CFDictionary)
        XCTAssertTrue(CGImageDestinationFinalize(destination))
        let size = UInt64(try original.resourceValues(forKeys: [.fileSizeKey]).fileSize!)
        let result = try prepare(original, checkpoint: root.appendingPathComponent("rotate.maicloud.json"),
                                 size: size)
        let decoder = try XCTUnwrap(CGImageSourceCreateWithURL(result.jpeg as CFURL, nil))
        let picture = try XCTUnwrap(CGImageSourceCreateImageAtIndex(decoder, 0, nil))
        XCTAssertEqual(picture.width, 160)
        XCTAssertEqual(picture.height, 320)
        let metadata = try XCTUnwrap(CGImageSourceCopyPropertiesAtIndex(
            decoder, 0, nil) as? [String: Any])
        XCTAssertEqual(metadata[kCGImagePropertyOrientation as String] as? Int ?? 1, 1)
    }

    func testTransparentSourceUsesWhiteOnlyInDerivedJPEG() throws {
        let root = try directory(), original = root.appendingPathComponent("alpha.png")
        let bitmap = try XCTUnwrap(CGContext(data: nil, width: 64, height: 64,
            bitsPerComponent: 8, bytesPerRow: 256, space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue))
        bitmap.setFillColor(CGColor(red: 1, green: 0, blue: 0, alpha: 1))
        bitmap.fill(CGRect(x: 16, y: 16, width: 32, height: 32))
        let destination = try XCTUnwrap(CGImageDestinationCreateWithURL(
            original as CFURL, UTType.png.identifier as CFString, 1, nil))
        CGImageDestinationAddImage(destination, try XCTUnwrap(bitmap.makeImage()), nil)
        XCTAssertTrue(CGImageDestinationFinalize(destination))
        let before = try Data(contentsOf: original)
        let result = try prepare(original, checkpoint: root.appendingPathComponent("alpha.maicloud.json"),
                                 size: UInt64(before.count))
        let decoder = try XCTUnwrap(CGImageSourceCreateWithURL(result.jpeg as CFURL, nil))
        let picture = try XCTUnwrap(CGImageSourceCreateImageAtIndex(decoder, 0, nil))
        let rendered = try XCTUnwrap(CGContext(data: nil, width: picture.width, height: picture.height,
            bitsPerComponent: 8, bytesPerRow: picture.width * 4, space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue))
        rendered.draw(picture, in: CGRect(x: 0, y: 0, width: picture.width, height: picture.height))
        let bytes = try XCTUnwrap(rendered.data).assumingMemoryBound(to: UInt8.self)
        XCTAssertGreaterThan(bytes[0], 240)
        XCTAssertGreaterThan(bytes[1], 240)
        XCTAssertGreaterThan(bytes[2], 240)
        XCTAssertEqual(try Data(contentsOf: original), before)
    }

    func testExistingCoreCheckpointDelegatesThumbnailRecovery() throws {
        let root = try directory(), original = root.appendingPathComponent("owned.png")
        let count = try image(original), checkpoint = root.appendingPathComponent("upload.maicloud.json")
        let first = try prepare(original, checkpoint: checkpoint, size: count)
        try Data("{}".utf8).write(to: checkpoint)
        try FileManager.default.removeItem(at: first.jpeg)
        // 核心负责区分远端缩图已 ready / 尚在 pending。宿主不解析私有checkpoint字段，
        // 不在前置检查中挡住ready恢复，也不能偷偷重新编码替换原指纹。
        let recovered = try prepare(original, checkpoint: checkpoint, size: count)
        XCTAssertEqual(recovered.jpeg, first.jpeg)
        XCTAssertEqual(recovered.source, first.source)
        XCTAssertFalse(FileManager.default.fileExists(atPath: recovered.jpeg.path))
        XCTAssertTrue(FileManager.default.fileExists(atPath: recovered.source.path))
    }

    func testFinishedUploadCleanupDoesNotWaitForAnotherSourceProvider() throws {
        let root = try directory(), original = root.appendingPathComponent("owned.png")
        let size = try image(original), finished = root.appendingPathComponent("finished.maicloud.json")
        _ = try prepare(original, checkpoint: finished, size: size)
        let pending = root.appendingPathComponent("pending.maicloud.json")
        let entered = DispatchSemaphore(value: 0), release = DispatchSemaphore(value: 0)
        let preparationDone = DispatchSemaphore(value: 0), cleanupDone = DispatchSemaphore(value: 0)
        DispatchQueue.global(qos: .utility).async {
            defer { preparationDone.signal() }
            _ = try? MaiCloudDriveImageThumbnail.prepare(
                checkpoint: pending, sourceID: "slow-file-provider", size: size,
                sourceURL: {
                    entered.signal()
                    _ = release.wait(timeout: .now() + 3)
                    return original
                }, isCanceled: { false })
        }
        XCTAssertEqual(entered.wait(timeout: .now() + 2), .success)
        DispatchQueue.global(qos: .utility).async {
            try? MaiCloudDriveImageThumbnail.removeAfterCompletionOrAbort(checkpoint: finished)
            cleanupDone.signal()
        }
        // 实际清理会由 MainActor 的完成路径调用，不能等待不相关来源的长时间 I/O。
        let cleanupResult = cleanupDone.wait(timeout: .now() + 0.3)
        XCTAssertEqual(cleanupResult, .success)
        release.signal()
        _ = preparationDone.wait(timeout: .now() + 5)
        if cleanupResult != .success {
            _ = cleanupDone.wait(timeout: .now() + 5)
        }
    }
}
