// 文件作用：为普通照片服务和 Agent 共用有界人脸检测、原尺寸修图与原子输出。
// 平台只做 ImageIO/CoreImage/Vision 适配；ROI 规划、皮肤处理、参数和指标由 C++ 决定。
import Foundation
import CoreImage
import ImageIO
import Vision
import CryptoKit

/// 后台任务和账号切换共用取消标记；正在执行的同步 C++ 调用结束后再次检查。
/// 不声称可中断 ORT 内核，也不把未接入本标记的 Agent 停止事件当成已取消。
final class MaiPortraitCancellation: @unchecked Sendable {
    private let lock = NSLock()
    private var canceled = false
    private var vision: VNRequest?

    func cancel() {
        lock.lock()
        canceled = true
        let request = vision
        lock.unlock()
        request?.cancel()
    }

    func check() throws {
        lock.lock()
        let value = canceled
        lock.unlock()
        if value || Task<Never, Never>.isCancelled { throw CancellationError() }
    }

    func track(_ request: VNRequest?) throws {
        lock.lock()
        vision = request
        let value = canceled
        lock.unlock()
        if value { request?.cancel(); throw CancellationError() }
    }
}

/// 不透传带私人路径的 ImageIO/Vision/NSError；JSON 可直接进入现有错误消息字段。
struct MaiPortraitHostFailure: LocalizedError, Sendable {
    let detail: String
    var errorDescription: String? { detail }

    init(_ code: String, stage: String, message: String, hint: String,
         width: Int = 0, height: Int = 0) {
        let fields: [String: Any] = ["code": code, "stage": stage, "message": message,
            "hint": hint, "width": width, "height": height,
            "full_raster_max_pixels": 12_000_000, "roi_max_side": 2048]
        detail = String(data: try! JSONSerialization.data(withJSONObject: fields, options: .sortedKeys), encoding: .utf8)!
    }

    init(detail: String) { self.detail = detail }
}

/// 准备阶段没有发布最终文件或登记训练记录；丢弃对象会清理自己的临时产物。
final class MaiPreparedPortrait: @unchecked Sendable {
    let imageTemporary: URL
    let maskTemporary: URL
    let imageOutput: URL
    let maskOutput: URL
    let metadata: String
    let profile: String
    let width: Int
    let height: Int

    init(imageTemporary: URL, maskTemporary: URL, imageOutput: URL, maskOutput: URL,
         metadata: String, profile: String, width: Int, height: Int) {
        self.imageTemporary = imageTemporary; self.maskTemporary = maskTemporary
        self.imageOutput = imageOutput; self.maskOutput = maskOutput
        self.metadata = metadata; self.profile = profile; self.width = width; self.height = height
    }

    deinit {
        try? FileManager.default.removeItem(at: imageTemporary)
        try? FileManager.default.removeItem(at: maskTemporary)
    }
}

enum MaiPortraitPhotoProcessor {
    // ImageIO/CoreImage 编码会有完整光栅的内部工作内存；同进程只允许一项照片任务。
    private static let processingLock = NSLock()

    private static func acquire() throws {
        guard processingLock.try() else {
            throw failed("busy", "budget", "Another portrait image operation is in progress", "Wait for it to finish before retrying this request")
        }
    }
    private struct Source {
        let url: URL
        let image: CIImage
        let width: Int
        let height: Int
        let size: Int
        let modified: Date?
    }

    private static func failed(_ code: String, _ stage: String, _ message: String,
                               _ hint: String, _ width: Int = 0, _ height: Int = 0) -> MaiPortraitHostFailure {
        MaiPortraitHostFailure(code, stage: stage, message: message, hint: hint, width: width, height: height)
    }

    private static func encoded(_ fields: [String: Any]) throws -> String {
        String(data: try JSONSerialization.data(withJSONObject: fields, options: .sortedKeys), encoding: .utf8)!
    }

    private static func take(_ pointer: UnsafeMutablePointer<CChar>?) throws -> String {
        guard let pointer else {
            throw failed("portrait_service_failed", "portrait", "Portrait service returned no result", "Retry after checking bundled models")
        }
        defer { maiPortraitFree(pointer) }
        return String(cString: pointer)
    }

    private static func output(_ value: String) throws -> [String: Any] {
        guard let fields = try JSONSerialization.jsonObject(with: Data(value.utf8)) as? [String: Any] else {
            throw failed("portrait_service_failed", "portrait", "Portrait service returned invalid data", "Do not use the incomplete output")
        }
        guard fields["ok"] as? Bool == true, let result = fields["output"] as? [String: Any] else {
            if let detail = fields["error"] as? [String: Any] { throw MaiPortraitHostFailure(detail: try encoded(detail)) }
            throw failed("portrait_service_failed", "portrait", "Portrait service could not complete the operation", "No output was committed")
        }
        return result
    }

    /// stat、文件尾和 ImageIO 完整状态一起检查，未完成的生产文件不能被误报成没有人脸。
    private static func source(_ url: URL) throws -> Source {
        // ImageIO 的有界缩略图也可能借用整图解码缓存；及时释放该阶段的临时对象。
        try autoreleasepool { try decodeSource(url) }
    }

    private static func decodeSource(_ url: URL) throws -> Source {
        guard FileManager.default.fileExists(atPath: url.path) else {
            throw failed("file_not_found", "input", "Source image does not exist", "Wait for the producing tool to finish before reading its output")
        }
        let values: URLResourceValues
        do { values = try url.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey, .contentModificationDateKey]) }
        catch { throw failed("input_io_failed", "input", "Cannot inspect the source image", "Check that this account can read the file") }
        guard values.isRegularFile == true else {
            throw failed("invalid_file_type", "input", "Source must be a regular image file", "Use a JPEG, PNG or HEIF image")
        }
        guard let size = values.fileSize, size > 0 else {
            throw failed("input_not_ready", "input", "Source image is empty", "Wait for the producing tool to finish")
        }
        guard size <= 50 * 1024 * 1024 else {
            throw failed("encoded_budget_exceeded", "budget", "Encoded image exceeds 50 MB", "Use a smaller encoded source without discarding the desired region")
        }
        // 常见渐进写入格式的尾标记，避免只凭可读元数据接受未写完的 JPEG/PNG。
        do {
            let handle = try FileHandle(forReadingFrom: url)
            defer { try? handle.close() }
            let header = try handle.read(upToCount: 8) ?? Data()
            try handle.seek(toOffset: UInt64(max(0, size - 12)))
            let tail = try handle.readToEnd() ?? Data()
            if header.starts(with: [0xff, 0xd8]) && !tail.suffix(2).elementsEqual([0xff, 0xd9]) {
                throw failed("input_not_ready", "decode", "JPEG input is incomplete", "Wait for the producing tool to finish and retry")
            }
            if header.starts(with: [137, 80, 78, 71, 13, 10, 26, 10]) &&
                !tail.elementsEqual([0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130]) {
                throw failed("input_not_ready", "decode", "PNG input is incomplete", "Wait for the producing tool to finish and retry")
            }
        } catch let error as MaiPortraitHostFailure { throw error }
        catch { throw failed("input_io_failed", "input", "Cannot read the source image", "Check file access and producer completion") }
        guard let decoder = CGImageSourceCreateWithURL(url as CFURL, [kCGImageSourceShouldCache: false] as CFDictionary),
              CGImageSourceGetCount(decoder) > 0, CGImageSourceGetStatus(decoder) == .statusComplete,
              CGImageSourceGetStatusAtIndex(decoder, 0) == .statusComplete,
              let properties = CGImageSourceCopyPropertiesAtIndex(decoder, 0, nil) as? [String: Any],
              let width = properties[kCGImagePropertyPixelWidth as String] as? Int,
              let height = properties[kCGImagePropertyPixelHeight as String] as? Int,
              width > 0, height > 0, width <= 20_000, height <= 20_000,
              Int64(width) * Int64(height) <= 100_000_000 else {
            throw failed("decode_failed", "decode", "Source image metadata is invalid or incomplete", "Use a complete supported image within 100 MP and 20000 pixels per side")
        }
        // 这是平台原尺寸解码／编码预算，不提高 C++ 旧全图 12 MP 门禁。
        // 尚未验证更大画面的峰值内存；不允许 ImageIO 隐式分配数 GB 或悄悄缩小输出。
        guard Int64(width) * Int64(height) <= 32_000_000 else {
            throw failed("output_budget_exceeded", "budget", "The iOS original-size image writer currently supports at most 32 MP",
                         "Use an explicitly selected smaller source; this operation never silently downsizes the original", width, height)
        }
        // 实际执行一次有界解码，而非把 CGImageSource 的存在等同于成功解码。
        guard CGImageSourceCreateThumbnailAtIndex(decoder, 0, [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceThumbnailMaxPixelSize: 256,
            kCGImageSourceShouldCacheImmediately: true,
        ] as CFDictionary) != nil,
              let decoded = CIImage(contentsOf: url, options: [.applyOrientationProperty: true]) else {
            throw failed("decode_failed", "decode", "Source pixels could not be decoded", "Verify that the image producer completed successfully", width, height)
        }
        let image = decoded.transformed(by: CGAffineTransform(translationX: -decoded.extent.minX, y: -decoded.extent.minY))
        return Source(url: url, image: image, width: Int(image.extent.width), height: Int(image.extent.height),
                      size: size, modified: values.contentModificationDate)
    }

    private static func unchanged(_ source: Source) throws {
        let values = try? source.url.resourceValues(forKeys: [.fileSizeKey, .contentModificationDateKey])
        guard values?.fileSize == source.size, values?.contentModificationDate == source.modified else {
            throw failed("input_not_ready", "input", "Source changed during processing", "Wait for the producing tool to finish, then retry", source.width, source.height)
        }
    }

    private static func positions(_ length: Int, side: Int) -> [Int] {
        guard length > side else { return [0] }
        var result = Array(stride(from: 0, through: length - side, by: side * 3 / 4))
        if result.last != length - side { result.append(length - side) }
        return result
    }

    private static func faces(_ source: Source, cancellation: MaiPortraitCancellation) throws -> [[String: Any]] {
        let width = source.width, height = source.height
        var windows = [CGRect(x: 0, y: 0, width: width, height: height)]
        if max(width, height) > 2048 {
            let side = 1400
            let xs = positions(width, side: side), ys = positions(height, side: side)
            guard xs.count * ys.count <= 64 else {
                throw failed("detection_budget_exceeded", "budget", "Native face windows exceed the bounded detection budget", "Use an explicitly selected region for this exceptionally large image", width, height)
            }
            windows += ys.flatMap { y in xs.map { x in CGRect(x: x, y: y, width: min(side, width), height: min(side, height)) } }
        }
        var candidates: [[String: Any]] = []
        for window in windows {
            let found: [[String: Any]] = try autoreleasepool {
                try cancellation.check()
                let bounds = CGRect(x: window.minX, y: CGFloat(height) - window.maxY, width: window.width, height: window.height)
                let crop = source.image.cropped(to: bounds).transformed(by: CGAffineTransform(translationX: -bounds.minX, y: -bounds.minY))
                let scale = min(1, 2048 / max(window.width, window.height))
                let analysis = crop.transformed(by: CGAffineTransform(scaleX: scale, y: scale))
                let request = VNDetectFaceLandmarksRequest()
                #if targetEnvironment(simulator)
                request.usesCPUOnly = true
                request.revision = VNDetectFaceLandmarksRequestRevision2
                #endif
                try cancellation.track(request)
                do { try VNImageRequestHandler(ciImage: analysis, options: [:]).perform([request]) }
                catch {
                    try cancellation.check()
                    throw failed("vision_failed", "face_detection", "Face detection could not decode or analyze a source window", "Check input readiness; this is not a successful no-face result", width, height)
                }
                try cancellation.track(nil)
                var local: [[String: Any]] = []
                for face in request.results ?? [] {
                    let box = face.boundingBox
                    func point(_ region: VNFaceLandmarkRegion2D?) -> [String: Double]? {
                        guard let points = region?.normalizedPoints, !points.isEmpty else { return nil }
                        let x = points.reduce(CGFloat.zero) { $0 + $1.x } / CGFloat(points.count)
                        let y = points.reduce(CGFloat.zero) { $0 + $1.y } / CGFloat(points.count)
                        return ["x": Double(window.minX + (box.minX + x * box.width) * window.width),
                                "y": Double(window.minY + (1 - box.minY - y * box.height) * window.height)]
                    }
                    var landmarks: [String: Any] = [:]
                    landmarks["left_eye"] = point(face.landmarks?.leftEye)
                    landmarks["right_eye"] = point(face.landmarks?.rightEye)
                    landmarks["mouth"] = point(face.landmarks?.outerLips)
                    local.append(["box": ["x": Double(window.minX + box.minX * window.width),
                        "y": Double(window.minY + (1 - box.maxY) * window.height),
                        "width": Double(box.width * window.width), "height": Double(box.height * window.height)],
                        "confidence": Double(face.confidence), "landmarks": landmarks])
                    guard candidates.count + local.count <= 256 else {
                        throw failed("too_many_face_candidates", "face_detection", "Face candidates exceed the detection budget", "Select a smaller region explicitly", width, height)
                    }
                }
                return local
            }
            candidates += found
        }
        let merged = try encoded(["_face_hints": candidates]).withCString { try output(take(maiPortraitMergeFaceHints($0))) }
        return merged["faces"] as? [[String: Any]] ?? []
    }

    static func detectFaces(sourceURL: URL, cancellation: MaiPortraitCancellation) throws -> String {
        try acquire()
        defer { processingLock.unlock() }
        return try autoreleasepool {
            let input = try source(sourceURL)
            let found = try faces(input, cancellation: cancellation)
            try unchanged(input)
            return try encoded(["source_width": input.width, "source_height": input.height,
                "coordinate_origin": "top_left", "faces": found, "analysis_performed": true,
                "detection_policy": "oriented_global_2048_plus_native_overlapping_1400_windows"])
        }
    }

    private static func rgba(_ image: CIImage, bounds: CGRect, context: CIContext) -> Data {
        let width = Int(bounds.width), height = Int(bounds.height)
        var pixels = Data(count: width * height * 4)
        pixels.withUnsafeMutableBytes { bytes in
            context.render(image, toBitmap: bytes.baseAddress!, rowBytes: width * 4, bounds: bounds,
                           format: .RGBA8, colorSpace: CGColorSpace(name: CGColorSpace.sRGB))
        }
        return pixels
    }

    static func prepare(arguments: String, sourceURL: URL, workspace: URL, allowedRoot: URL,
                        profile: String, cancellation: MaiPortraitCancellation) throws -> MaiPreparedPortrait {
        try acquire()
        defer { processingLock.unlock() }
        return try autoreleasepool {
            try prepareImage(arguments: arguments, sourceURL: sourceURL, workspace: workspace,
                allowedRoot: allowedRoot, profile: profile, cancellation: cancellation)
        }
    }

    private static func prepareImage(arguments: String, sourceURL: URL, workspace: URL, allowedRoot: URL,
                                     profile: String, cancellation: MaiPortraitCancellation) throws -> MaiPreparedPortrait {
        try cancellation.check()
        if let error = arguments.withCString({ maiPortraitValidateArguments("beautify", $0, 1) }) {
            throw MaiPortraitHostFailure(detail: try take(error))
        }
        var originalURL = sourceURL
        // 只有已有的结果账本才需要追溯；普通原图不伪造恢复缓存，也不提前创建个人记录。
        if FileManager.default.fileExists(atPath: URL(fileURLWithPath: profile).appendingPathComponent("portrait.sqlite3").path) {
            let restored = try profile.withCString { directory in
                try sourceURL.path.withCString { try take(maiPortraitOriginalSource(directory, $0)) }
            }
            originalURL = URL(fileURLWithPath: restored).resolvingSymlinksInPath().standardizedFileURL
        }
        originalURL = originalURL.resolvingSymlinksInPath().standardizedFileURL
        let root = allowedRoot.resolvingSymlinksInPath().standardizedFileURL.path
        guard originalURL.path.hasPrefix(root + "/") else {
            throw failed("source_outside_account", "input", "Original image is outside this account's allowed file domain", "Import the original into the current workspace")
        }
        let input = try source(originalURL)
        let context = CIContext(options: [.cacheIntermediates: false, .workingFormat: CIFormat.RGBA8.rawValue])
        let found = try faces(input, cancellation: cancellation)
        let hints = try encoded(["_face_hints": found])
        let specification = try JSONSerialization.jsonObject(with: Data(arguments.utf8)) as! [String: Any]
        let subject = (specification["subject_index"] as? NSNumber)?.intValue ?? -1
        let proxy = rgba(input.image.transformed(by: CGAffineTransform(scaleX: 64 / CGFloat(input.width), y: 64 / CGFloat(input.height))),
                         bounds: CGRect(x: 0, y: 0, width: 64, height: 64), context: context)
        var plan = try proxy.withUnsafeBytes { bytes in
            try hints.withCString { hints in
                try output(take(maiPortraitPlanFaceRegion(Int32(input.width), Int32(input.height), hints,
                    Int32(subject), bytes.bindMemory(to: UInt8.self).baseAddress, 64, 64, 256)))
            }
        }
        // 与旧全图路径保持相同的尺寸前缀和逐行 RGBA 指纹；只持有 128 行像素。
        var hash = SHA256()
        hash.update(data: Data("\(input.width)x\(input.height):".utf8))
        for top in stride(from: 0, to: input.height, by: 128) {
            try cancellation.check()
            let rows = min(128, input.height - top)
            autoreleasepool {
                hash.update(data: rgba(input.image, bounds: CGRect(x: 0, y: input.height - top - rows,
                    width: input.width, height: rows), context: context))
            }
        }
        plan["source_fingerprint"] = hash.finalize().map { String(format: "%02x", $0) }.joined()
        guard let region = plan["roi"] as? [String: Int], let x = region["x"], let y = region["y"],
              let width = region["width"], let height = region["height"] else {
            throw failed("invalid_region_context", "portrait", "Portrait service returned no native region", "No output was committed")
        }
        let bounds = CGRect(x: x, y: input.height - y - height, width: width, height: height)
        let pixels = rgba(input.image, bounds: bounds, context: context)
        let regionJSON = try encoded(plan)
        try cancellation.check()
        let result = pixels.withUnsafeBytes { bytes in
            arguments.withCString { arguments in profile.withCString { profile in
                originalURL.path.withCString { source in regionJSON.withCString { plan in
                    maiPortraitProcessRegionRgba(bytes.bindMemory(to: UInt8.self).baseAddress,
                        Int32(width), Int32(height), Int32(width * 4), arguments, profile, source, plan)
                } }
            } }
        }
        defer { maiPortraitFree(result.rgba); maiPortraitFree(result.mask); maiPortraitFree(result.metadata); maiPortraitFree(result.error) }
        if let error = result.error { throw MaiPortraitHostFailure(detail: String(cString: error)) }
        guard let edited = result.rgba, let alpha = result.mask, let metadata = result.metadata,
              Int(result.width) == width, Int(result.height) == height else {
            throw failed("portrait_service_failed", "portrait", "Portrait service returned an incomplete region", "No output was committed")
        }
        try cancellation.check()
        let colorSpace = CGColorSpace(name: CGColorSpace.sRGB)!
        let patch = CIImage(bitmapData: Data(bytes: edited, count: width * height * 4), bytesPerRow: width * 4,
            size: CGSize(width: width, height: height), format: .RGBA8, colorSpace: colorSpace)
            .transformed(by: CGAffineTransform(translationX: bounds.minX, y: bounds.minY))
        let mask = CIImage(bitmapData: Data(bytes: alpha, count: width * height), bytesPerRow: width,
            size: CGSize(width: width, height: height), format: .L8, colorSpace: CGColorSpaceCreateDeviceGray())
            .transformed(by: CGAffineTransform(translationX: bounds.minX, y: bounds.minY))
        let id = UUID().uuidString
        let prepared = MaiPreparedPortrait(imageTemporary: workspace.appendingPathComponent(".portrait-\(id).tmp.png"),
            maskTemporary: workspace.appendingPathComponent(".portrait-mask-\(id).tmp.png"),
            imageOutput: workspace.appendingPathComponent("edited-\(id).png"),
            maskOutput: workspace.appendingPathComponent("skin-mask-\(id).png"),
            metadata: String(cString: metadata), profile: profile, width: input.width, height: input.height)
        do {
            // patch 已在核心内软边合成；直接覆盖 ROI，不用导出的诊断 alpha 重复混合。
            // 编码器可能暂存完整光栅；每个文件各自释放临时对象，不能把两份缓冲叠加。
            try autoreleasepool {
                try context.writePNGRepresentation(of: patch.composited(over: input.image).cropped(to: input.image.extent),
                    to: prepared.imageTemporary, format: .RGBA8, colorSpace: colorSpace)
            }
            context.clearCaches()
            try cancellation.check()
            try autoreleasepool {
                try context.writePNGRepresentation(of: mask.composited(over: CIImage(color: .black).cropped(to: input.image.extent)),
                    to: prepared.maskTemporary, format: .L8, colorSpace: CGColorSpaceCreateDeviceGray())
            }
            context.clearCaches()
        } catch let error as CancellationError { throw error }
        catch { throw failed("encode_failed", "output", "Could not encode the full-size image and mask", "Check available storage; no output was committed", input.width, input.height) }
        for url in [prepared.imageTemporary, prepared.maskTemporary] {
            guard let decoder = CGImageSourceCreateWithURL(url as CFURL, nil), CGImageSourceGetStatus(decoder) == .statusComplete,
                  let properties = CGImageSourceCopyPropertiesAtIndex(decoder, 0, nil) as? [String: Any],
                  properties[kCGImagePropertyPixelWidth as String] as? Int == input.width,
                  properties[kCGImagePropertyPixelHeight as String] as? Int == input.height else {
                throw failed("encode_failed", "output", "Encoded output dimensions or completeness are invalid", "No output was committed", input.width, input.height)
            }
        }
        try unchanged(input)
        try cancellation.check()
        return prepared
    }

    /// 调用方必须在当前账号和工作区再次核对后、不跨 await 调用本方法。
    /// 先发布完整 image/mask，再登记结果；中途失败清理本次文件，不留下成功记录。
    static func publish(_ prepared: MaiPreparedPortrait, cancellation: MaiPortraitCancellation) throws -> [String: Any] {
        try cancellation.check()
        var published: [URL] = []
        do {
            for (temporary, final) in [(prepared.maskTemporary, prepared.maskOutput), (prepared.imageTemporary, prepared.imageOutput)] {
                _ = try temporary.path.withCString { temporary in
                    try final.path.withCString { try output(take(maiPortraitPublishOutput(temporary, $0))) }
                }
                published.append(final)
            }
            try cancellation.check()
            var response = try prepared.profile.withCString { profile in
                try prepared.metadata.withCString { metadata in
                    try prepared.imageOutput.path.withCString { try output(take(maiPortraitCommitResult(profile, metadata, $0))) }
                }
            }
            response.merge(["path": prepared.imageOutput.lastPathComponent, "mime_type": "image/png",
                "width": prepared.width, "height": prepared.height,
                "mask_path": prepared.maskOutput.lastPathComponent, "mask_width": prepared.width,
                "mask_height": prepared.height, "mask_coordinate_origin": "top_left",
                "next_tool": "mobile_preview_image"]) { _, new in new }
            return response
        } catch {
            for url in published { try? FileManager.default.removeItem(at: url) }
            throw error
        }
    }
}
