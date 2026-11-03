// 文件作用：在公开人脸素材合成的 24 MP 角落小脸上，验证普通平台服务与真实共用核心。
// 不访问用户照片、账号或网络；与 iOS 使用同一份 prepare/detect/publish，而非重写测试引擎。
import Foundation
import CoreImage
import ImageIO

@main
struct MaiPortraitRegionTests {
    static func check(_ value: @autoclosure () -> Bool, _ message: String) throws {
        if !value() { throw NSError(domain: "MaiPortraitRegionTests", code: 1, userInfo: [NSLocalizedDescriptionKey: message]) }
    }

    static func json(_ value: [String: Any]) throws -> String {
        String(data: try JSONSerialization.data(withJSONObject: value, options: .sortedKeys), encoding: .utf8)!
    }

    static func failure(_ code: String, _ operation: () throws -> Void) throws {
        do { try operation() }
        catch let failure as MaiPortraitHostFailure {
            let detail = try JSONSerialization.jsonObject(with: Data(failure.detail.utf8)) as! [String: Any]
            try check(detail["code"] as? String == code, "Unexpected structured error: \(failure.detail)")
            return
        }
        throw NSError(domain: "MaiPortraitRegionTests", code: 2, userInfo: [NSLocalizedDescriptionKey: "Expected \(code)"])
    }

    static func bytes(_ image: CIImage, _ bounds: CGRect, _ context: CIContext) -> Data {
        let width = Int(bounds.width), height = Int(bounds.height)
        var value = Data(count: width * height * 4)
        value.withUnsafeMutableBytes {
            context.render(image, toBitmap: $0.baseAddress!, rowBytes: width * 4, bounds: bounds,
                           format: .RGBA8, colorSpace: CGColorSpace(name: CGColorSpace.sRGB))
        }
        return value
    }

    static func main() throws {
        let arguments = CommandLine.arguments
        guard arguments.count == 5 else {
            print("Usage: MaiPortraitRegionTests <models> <runtime> <public-face.jpg> <new-output-directory>")
            return
        }
        let root = URL(fileURLWithPath: arguments[4], isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        arguments[1].withCString { model in arguments[2].withCString { maiPortraitConfigure(model, $0, nil) } }
        defer { maiPortraitConfigure("", "", nil) }
        let workspace = root.appendingPathComponent("Workspace", isDirectory: true)
        try FileManager.default.createDirectory(at: workspace, withIntermediateDirectories: true)
        let profile = root.appendingPathComponent("PortraitPreferences", isDirectory: true).path
        let source = workspace.appendingPathComponent("public-corner-24mp.png")
        let context = CIContext(options: [.cacheIntermediates: false])
        let colorSpace = CGColorSpace(name: CGColorSpace.sRGB)!
        let face = CIImage(contentsOf: URL(fileURLWithPath: arguments[3]), options: [.applyOrientationProperty: true])!
        let image = face.transformed(by: CGAffineTransform(translationX: 104, y: 88))
            .composited(over: CIImage(color: CIColor(red: 0.4, green: 0.4, blue: 0.4)).cropped(to: CGRect(x: 0, y: 0, width: 5712, height: 4284)))
        try context.writePNGRepresentation(of: image, to: source, format: .RGBA8, colorSpace: colorSpace)
        let cancellation = MaiPortraitCancellation()
        let detected = try MaiPortraitPhotoProcessor.detectFaces(sourceURL: source, cancellation: cancellation)
        let detection = try JSONSerialization.jsonObject(with: Data(detected.utf8)) as! [String: Any]
        let faces = detection["faces"] as! [[String: Any]]
        try check(faces.count == 1, "Tiled detection did not recover exactly one corner face")
        let box = faces[0]["box"] as! [String: NSNumber]
        try check(box["x"]!.doubleValue < 600 && box["y"]!.doubleValue > 3600, "Face was mapped to the wrong source coordinates")
        let missing = workspace.appendingPathComponent("not-produced.png")
        try failure("file_not_found") { _ = try MaiPortraitPhotoProcessor.detectFaces(sourceURL: missing, cancellation: cancellation) }
        let partial = workspace.appendingPathComponent("partial.png")
        try Data([137, 80, 78, 71, 13, 10, 26, 10, 0, 0]).write(to: partial)
        try failure("input_not_ready") { _ = try MaiPortraitPhotoProcessor.detectFaces(sourceURL: partial, cancellation: cancellation) }
        var invalid: [String: Any] = ["operation": "beautify", "path": source.path, "action": "delegate"]
        try failure("unknown_parameter") {
            _ = try MaiPortraitPhotoProcessor.prepare(arguments: json(invalid), sourceURL: source,
                workspace: workspace, allowedRoot: root, profile: profile, cancellation: cancellation)
        }
        invalid.removeValue(forKey: "action")
        invalid["whitening"] = "0.5"
        try failure("invalid_parameters") {
            _ = try MaiPortraitPhotoProcessor.prepare(arguments: json(invalid), sourceURL: source,
                workspace: workspace, allowedRoot: root, profile: profile, cancellation: cancellation)
        }
        let canceled = MaiPortraitCancellation(); canceled.cancel()
        do {
            _ = try MaiPortraitPhotoProcessor.prepare(arguments: "{}", sourceURL: source, workspace: workspace,
                allowedRoot: root, profile: profile, cancellation: canceled)
            try check(false, "Canceled processing produced a result")
        } catch is CancellationError { }
        var results: [[String: Any]] = []
        for strength in [0.5, 1.0] {
            let parameters: [String: Any] = ["operation": "beautify", "path": source.path,
                "smoothing": 0, "brightening": 0, "warmth": 0, "whitening": strength,
                "slim_face": 0, "enlarge_eyes": 0, "use_personal_model": false]
            let prepared = try MaiPortraitPhotoProcessor.prepare(arguments: json(parameters), sourceURL: source,
                workspace: workspace, allowedRoot: root, profile: profile, cancellation: cancellation)
            let response = try MaiPortraitPhotoProcessor.publish(prepared, cancellation: cancellation)
            try check(response["width"] as? Int == 5712 && response["height"] as? Int == 4284, "Original dimensions were lost")
            try check(response["face_count"] as? Int == 1 && response["person_count"] is NSNull,
                      "Face/person analysis flags do not describe the performed analysis")
            try check(response["face_analysis_performed"] as? Bool == true && response["person_analysis_performed"] as? Bool == false,
                      "Analysis was reported without performing it")
            let beforeL = (response["skin_L_before"] as! NSNumber).doubleValue
            let afterL = (response["skin_L_after"] as! NSNumber).doubleValue
            let beforeC = (response["skin_C_before"] as! NSNumber).doubleValue
            let afterC = (response["skin_C_after"] as! NSNumber).doubleValue
            try check(afterL > beforeL && afterC < beforeC, "Whitening did not increase skin L* and decrease skin C*")
            let edited = CIImage(contentsOf: prepared.imageOutput)!
            let mask = CIImage(contentsOf: prepared.maskOutput)!
            let original = CIImage(contentsOf: source)!
            var unchanged = 0, changed = 0
            // 逐带比较真实编码后产物：mask=0 的所有 RGB 必须保持原样，不能凭脸框代替皮肤指标。
            for top in stride(from: 0, to: 4284, by: 64) {
                let rows = min(64, 4284 - top)
                let bounds = CGRect(x: 0, y: 4284 - top - rows, width: 5712, height: rows)
                let a = bytes(original, bounds, context), b = bytes(edited, bounds, context), m = bytes(mask, bounds, context)
                a.withUnsafeBytes { a in b.withUnsafeBytes { b in m.withUnsafeBytes { m in
                    let a = a.bindMemory(to: UInt8.self), b = b.bindMemory(to: UInt8.self), m = m.bindMemory(to: UInt8.self)
                    for index in stride(from: 0, to: a.count, by: 4) {
                        let different = a[index] != b[index] || a[index + 1] != b[index + 1] || a[index + 2] != b[index + 2]
                        if different { changed += 1; if m[index] == 0 { unchanged += 1 } }
                    }
                } } }
            }
            try check(unchanged == 0, "Pixels outside the skin mask changed during full-size composition")
            try check(changed > 0, "Whitening encoded an unchanged image")
            var summary = response
            summary["path"] = prepared.imageOutput.lastPathComponent
            summary["changed_pixels_after_encoding"] = changed
            summary["unexpected_background_changes"] = unchanged
            results.append(summary)
        }
        let firstL = (results[0]["skin_L_after"] as! NSNumber).doubleValue - (results[0]["skin_L_before"] as! NSNumber).doubleValue
        let secondL = (results[1]["skin_L_after"] as! NSNumber).doubleValue - (results[1]["skin_L_before"] as! NSNumber).doubleValue
        try check(secondL > firstL, "Whitening strength did not increase the measured skin effect")
        // 发布前取消必须保留原图、不交付最终文件；不能只测开始前已取消的快捷返回。
        let publishingCancellation = MaiPortraitCancellation()
        let prepared = try MaiPortraitPhotoProcessor.prepare(arguments: json([
            "operation": "beautify", "path": source.path, "smoothing": 0, "brightening": 0,
            "warmth": 0, "whitening": 0.5, "use_personal_model": false]), sourceURL: source,
            workspace: workspace, allowedRoot: root, profile: profile, cancellation: publishingCancellation)
        publishingCancellation.cancel()
        do {
            _ = try MaiPortraitPhotoProcessor.publish(prepared, cancellation: publishingCancellation)
            try check(false, "Canceled publish produced a result")
        } catch is CancellationError { }
        try check(!FileManager.default.fileExists(atPath: prepared.imageOutput.path) &&
            !FileManager.default.fileExists(atPath: prepared.maskOutput.path), "Cancel before publish exposed an output")
        let temporary = workspace.appendingPathComponent("publish-owned.tmp")
        let occupied = workspace.appendingPathComponent("publish-existing.png")
        try Data("new-owned".utf8).write(to: temporary)
        try Data("existing".utf8).write(to: occupied)
        let published = temporary.path.withCString { temporary in occupied.path.withCString { maiPortraitPublishOutput(temporary, $0) } }
        try check(published != nil, "Atomic publisher returned no error envelope")
        let publishJSON = String(cString: published!)
        maiPortraitFree(published)
        let publishError = try JSONSerialization.jsonObject(with: Data(publishJSON.utf8)) as! [String: Any]
        try check(publishError["ok"] as? Bool == false, "Atomic publisher replaced an existing target")
        let existingBytes = try Data(contentsOf: occupied), ownedBytes = try Data(contentsOf: temporary)
        try check(existingBytes == Data("existing".utf8) && ownedBytes == Data("new-owned".utf8),
                  "Failed publish changed caller-owned files")
        let summary: [String: Any] = ["status": "PASS", "source_width": 5712, "source_height": 4284,
            "face_box": box, "results": results, "file_not_found": true, "partial_input": true,
            "unknown_parameter": true, "invalid_parameter_type": true, "canceled_before_prepare": true,
            "canceled_before_publish": true, "existing_target_not_replaced": true]
        print(try json(summary))
    }
}
