// 测试宿主：使用公开样例走正常 Root、宿主工具、C++ 处理及现有媒体预览，不请求付费模型。
#if DEBUG && targetEnvironment(simulator)
import SwiftUI
import Foundation
import CoreImage
import ImageIO
import CryptoKit
import MaiChatCore

struct AIPortraitUITestRoot: View {
    @StateObject private var state = RemoteIMAppState(loadedSettings: .empty)
    @State private var prepared = false
    @State private var status = "准备照片"
    @State private var resultPath = ""
    // 直接暴露真实处理结果的交付指引，不在 fixture 中替工具填充预期值。
    @State private var resultNextTool = ""
    @State private var busy = true
    @State private var qaJSON = ""
    private var isLargeTest: Bool { ProcessInfo.processInfo.environment["MAICHAT_PORTRAIT_LARGE_TEST"] == "1" }

    var body: some View {
        RootView()
            .environmentObject(state)
            .onAppear {
                guard !prepared else { return }
                prepared = true
                state.masterUserID = "portrait-ui-test"
                state.chatState = MasterChatState(ownerUserID: "portrait-ui-test")
                state.hasCompletedInitialLogin = true
                AIAssistantModel.shared.installSessionSwitchUITestFixture()
                Task { await process() }
            }
            .overlay(alignment: .bottomTrailing) {
                VStack(alignment: .trailing) {
                    Text(status).accessibilityIdentifier("portrait-fixture-status")
                    if !resultNextTool.isEmpty {
                        Text(resultNextTool).accessibilityIdentifier("portrait-fixture-next-tool")
                    }
                    Button("预览修图结果") {
                        Task { _ = await call(
                            "mobile_preview_image", arguments: ["path": resultPath]) }
                    }.disabled(resultPath.isEmpty || busy)
                    if !isLargeTest {
                        Button("测试本地训练") { Task { await train() } }.disabled(busy)
                    }
                    if !qaJSON.isEmpty {
                        Text("24MP 检验数据").accessibilityIdentifier("portrait-fixture-qa")
                            .accessibilityLabel(qaJSON)
                    }
                }
                .font(.caption).padding(8).background(.regularMaterial)
                .padding(.bottom, 110).padding(.trailing, 8)
            }
    }
    @MainActor private func call(_ name: String, arguments: [String: Any]) async -> AIMaiChatHostToolExecution {
        let data = try! JSONSerialization.data(withJSONObject: arguments)
        return await AIMobileHostToolProvider.shared.execute(name: name, argumentsJSON: String(decoding: data, as: UTF8.self))
    }
    @MainActor private func decode(_ result: AIMaiChatHostToolExecution) throws -> [String: Any] {
        if let error = result.errorMessage { throw AIBackendError(message: error) }
        guard let text = result.output, let object = try JSONSerialization.jsonObject(with: Data(text.utf8)) as? [String: Any]
        else { throw AIBackendError(message: "测试结果无法解析") }
        return object
    }
    private nonisolated static func checkLarge(_ value: Bool, _ message: String) throws {
        if !value { throw AIBackendError(message: message) }
    }

    private nonisolated static func generateLarge(_ fixture: URL, directory: URL) throws -> String {
        try autoreleasepool {
            guard let face = CIImage(contentsOf: fixture) else { throw AIBackendError(message: "Public face fixture cannot be decoded") }
            let source = directory.appendingPathComponent("original-large.png")
            let canvas = CIImage(color: CIColor(red: 0.4, green: 0.4, blue: 0.4))
                .cropped(to: CGRect(x: 0, y: 0, width: 5712, height: 4284))
            let image = face.transformed(by: CGAffineTransform(translationX: 104, y: 88)).composited(over: canvas)
            try CIContext(options: [.cacheIntermediates: false]).writePNGRepresentation(of: image,
                to: source, format: .RGBA8, colorSpace: CGColorSpace(name: CGColorSpace.sRGB)!)
            return SHA256.hash(data: try Data(contentsOf: source)).map { String(format: "%02x", $0) }.joined()
        }
    }

    private nonisolated static func inspectLarge(resultJSON: String, detectionJSON: String,
                                                 directory: URL, sourceSHA: String) throws -> String {
        try autoreleasepool {
            let result = try JSONSerialization.jsonObject(with: Data(resultJSON.utf8)) as! [String: Any]
            let detection = try JSONSerialization.jsonObject(with: Data(detectionJSON.utf8)) as! [String: Any]
            let faces = detection["faces"] as? [[String: Any]] ?? []
            try checkLarge(faces.count == 1, "24MP tiled detection did not locate exactly one face")
            let box = faces[0]["box"] as! [String: NSNumber]
            try checkLarge(box["x"]!.doubleValue < 600 && box["y"]!.doubleValue > 3600,
                           "24MP face coordinates do not map back to the corner")
            let source = directory.appendingPathComponent("original-large.png")
            let output = directory.appendingPathComponent(result["path"] as! String)
            let mask = directory.appendingPathComponent(result["mask_path"] as! String)
            for url in [output, mask] {
                guard let decoder = CGImageSourceCreateWithURL(url as CFURL, nil),
                      let properties = CGImageSourceCopyPropertiesAtIndex(decoder, 0, nil) as? [String: Any] else {
                    throw AIBackendError(message: "24MP output is not a complete image")
                }
                try checkLarge(CGImageSourceGetStatus(decoder) == .statusComplete &&
                    properties[kCGImagePropertyPixelWidth as String] as? Int == 5712 &&
                    properties[kCGImagePropertyPixelHeight as String] as? Int == 4284,
                    "24MP output or mask lost the original dimensions")
                if url == mask {
                    guard let pixels = CGImageSourceCreateImageAtIndex(decoder, 0, nil) else {
                        throw AIBackendError(message: "Skin mask cannot be decoded")
                    }
                    try checkLarge(pixels.bitsPerComponent == 8 && pixels.bitsPerPixel == 8, "Skin mask is not grayscale L8")
                }
            }
            try checkLarge(result["face_count"] as? Int == 1 && result["person_count"] is NSNull &&
                result["face_analysis_performed"] as? Bool == true && result["person_analysis_performed"] as? Bool == false,
                "Face/person counts do not describe the performed analysis")
            let beforeL = (result["skin_L_before"] as! NSNumber).doubleValue
            let afterL = (result["skin_L_after"] as! NSNumber).doubleValue
            let beforeC = (result["skin_C_before"] as! NSNumber).doubleValue
            let afterC = (result["skin_C_after"] as! NSNumber).doubleValue
            try checkLarge(afterL > beforeL && afterC < beforeC, "24MP whitening did not improve mask-weighted L*/C*")
            let afterSHA = SHA256.hash(data: try Data(contentsOf: source)).map { String(format: "%02x", $0) }.joined()
            try checkLarge(afterSHA == sourceSHA, "24MP source file was modified")
            let originalImage = CIImage(contentsOf: source)!, editedImage = CIImage(contentsOf: output)!
            let roi = result["roi"] as! [String: Int]
            let x = roi["x"]!, y = 4284 - roi["y"]! - roi["height"]!
            let width = roi["width"]!, height = roi["height"]!
            let blocks = [CGRect(x: x, y: y, width: width, height: 1),
                CGRect(x: x, y: y + height - 1, width: width, height: 1),
                CGRect(x: x, y: y, width: 1, height: height),
                CGRect(x: x + width - 1, y: y, width: 1, height: height),
                CGRect(x: 5000, y: 3500, width: 64, height: 64)]
            let context = CIContext(options: [.cacheIntermediates: false])
            func bytes(_ image: CIImage, bounds: CGRect) -> Data {
                var data = Data(count: Int(bounds.width * bounds.height) * 4)
                data.withUnsafeMutableBytes {
                    context.render(image, toBitmap: $0.baseAddress!, rowBytes: Int(bounds.width) * 4,
                        bounds: bounds, format: .RGBA8, colorSpace: CGColorSpace(name: CGColorSpace.sRGB))
                }
                return data
            }
            for block in blocks {
                try checkLarge(bytes(originalImage, bounds: block) == bytes(editedImage, bounds: block),
                               "24MP ROI edge or background block changed")
            }
            var qa: [String: Any] = ["status": "PASS", "width": 5712, "height": 4284,
                "mask_width": 5712, "mask_height": 4284, "mask_format": "L8", "face_count": 1,
                "person_count": NSNull(), "face_analysis_performed": true, "person_analysis_performed": false,
                "source_sha_before": sourceSHA, "source_sha_after": afterSHA,
                "source_unchanged": true, "background_blocks_equal": true, "face_box": box]
            for key in ["skin_L_before", "skin_L_after", "skin_C_before", "skin_C_after", "roi", "processing_bbox",
                        "mask_semantics", "parameters", "processing_version", "analysis_version", "changed_pixel_count"] {
                qa[key] = result[key]
            }
            return String(data: try JSONSerialization.data(withJSONObject: qa, options: .sortedKeys), encoding: .utf8)!
        }
    }

    @MainActor private func expectLargeError(_ expected: String, result: AIMaiChatHostToolExecution) throws {
        guard let error = result.errorMessage,
              let detail = try JSONSerialization.jsonObject(with: Data(error.utf8)) as? [String: Any],
              detail["code"] as? String == expected else {
            throw AIBackendError(message: "Expected structured error " + expected)
        }
    }

    @MainActor private func processLarge() async {
        do {
            let directory = FileManager.default.temporaryDirectory.appendingPathComponent("portrait-ui-" + UUID().uuidString)
                .appendingPathComponent("Workspace")
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            AIAssistantModel.shared.installVideoToolTestWorkspace(directory.path)
            guard let fixture = Bundle.main.url(forResource: "portrait-test-face", withExtension: "jpg") else {
                throw AIBackendError(message: "Public face fixture is missing")
            }
            status = "24MP：生成公开角落小脸"
            let sourceSHA = try await Task.detached(priority: .userInitiated) {
                try Self.generateLarge(fixture, directory: directory)
            }.value
            status = "24MP：检测原图小脸"
            let detection = try decode(await call("mobile_detect_faces", arguments: ["path": "original-large.png"]))
            let detectionJSON = String(data: try JSONSerialization.data(withJSONObject: detection), encoding: .utf8)!
            status = "24MP：原尺寸局部美白"
            let parameters: [String: Any] = ["path": "original-large.png", "smoothing": 0, "brightening": 0,
                "warmth": 0, "whitening": 0.5, "use_personal_model": false]
            let result = try decode(await call("mobile_beautify_image", arguments: parameters))
            let resultJSON = String(data: try JSONSerialization.data(withJSONObject: result), encoding: .utf8)!
            qaJSON = try await Task.detached(priority: .utility) {
                try Self.inspectLarge(resultJSON: resultJSON, detectionJSON: detectionJSON, directory: directory, sourceSHA: sourceSHA)
            }.value
            var unknown = parameters; unknown["action"] = "delegate"
            try expectLargeError("unknown_parameter", result: await call("mobile_beautify_image", arguments: unknown))
            try expectLargeError("file_not_found", result: await call("mobile_beautify_image", arguments: ["path": "not-produced.png"]))
            try Data([137, 80, 78, 71, 13, 10, 26, 10, 0, 0]).write(to: directory.appendingPathComponent("partial.png"))
            try expectLargeError("input_not_ready", result: await call("mobile_beautify_image", arguments: ["path": "partial.png"]))
            var qa = try JSONSerialization.jsonObject(with: Data(qaJSON.utf8)) as! [String: Any]
            qa["unknown_parameter"] = true; qa["file_not_found"] = true; qa["input_not_ready"] = true
            qaJSON = String(data: try JSONSerialization.data(withJSONObject: qa, options: [.prettyPrinted, .sortedKeys]), encoding: .utf8)!
            let qaDirectory = FileManager.default.temporaryDirectory.appendingPathComponent("portrait-large-photo-qa", isDirectory: true)
            try FileManager.default.createDirectory(at: qaDirectory, withIntermediateDirectories: true)
            try Data(qaJSON.utf8).write(to: qaDirectory.appendingPathComponent("qa.json"), options: .atomic)
            resultPath = result["path"] as! String
            resultNextTool = result["next_tool"] as? String ?? ""
            AIAssistantModel.shared.installImageZoomUITestFixture(directory.appendingPathComponent(resultPath).path)
            let beforeL = (result["skin_L_before"] as! NSNumber).doubleValue, afterL = (result["skin_L_after"] as! NSNumber).doubleValue
            let beforeC = (result["skin_C_before"] as! NSNumber).doubleValue, afterC = (result["skin_C_after"] as! NSNumber).doubleValue
            status = String(format: "PASS：24MP 5712×4284 image+L8 mask\nL* %.2f→%.2f C* %.2f→%.2f", beforeL, afterL, beforeC, afterC)
        } catch { status = "FAIL：" + error.localizedDescription }
        busy = false
    }

    @MainActor private func process() async {
        if isLargeTest { await processLarge(); return }
        do {
            let directory = FileManager.default.temporaryDirectory.appendingPathComponent("portrait-ui-" + UUID().uuidString).appendingPathComponent("Workspace")
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            AIAssistantModel.shared.installVideoToolTestWorkspace(directory.path)
            guard let fixture = Bundle.main.url(forResource: "portrait-test-face", withExtension: "jpg") else {
                throw AIBackendError(message: "缺少测试照片")
            }
            try FileManager.default.copyItem(at: fixture, to: directory.appendingPathComponent("original.jpg"))
            let result = try decode(await call("mobile_beautify_image", arguments: [
                "path": "original.jpg", "smoothing": 0, "brightening": 0, "warmth": 0, "whitening": 0.6,
                "use_personal_model": false,
            ]))
            guard let path = result["path"] as? String, result["result_id"] is String else {
                throw AIBackendError(message: "修图缺少图片或结果编号")
            }
            resultPath = path
            resultNextTool = result["next_tool"] as? String ?? ""
            // 旧 fixture 手拼媒体消息，仅复用生产缩略图与点按预览；不是实际 Agent 发送或持久化验收。
            AIAssistantModel.shared.installImageZoomUITestFixture(directory.appendingPathComponent(path).path)
            status = "PASS：本地美白已生成新图"
        } catch { status = "FAIL：" + error.localizedDescription }
        busy = false
    }
    @MainActor private func train() async {
        busy = true
        do {
            let first = try decode(await call("mobile_beautify_image", arguments: [
                "path": "original.jpg", "smoothing": 0, "brightening": 0, "warmth": 0,
                "whitening": 0.2, "use_personal_model": false]))
            let second = try decode(await call("mobile_beautify_image", arguments: [
                "path": "original.jpg", "smoothing": 0, "brightening": 0, "warmth": 0,
                "whitening": 0.7, "use_personal_model": false]))
            _ = try decode(await call("mobile_portrait_feedback", arguments: [
                "preferred_result_id": first["result_id"]!, "rejected_result_id": second["result_id"]!, "attributes": ["whitening"]]))
            let result = try decode(await call("model_continue_training", arguments: ["action": "train"]))
            guard result["status"] as? String == "candidate_only", result["personal_weights_updated"] as? Bool == true,
                  result["active_version"] as? Int == 0 else { throw AIBackendError(message: "单张照片训练状态不正确") }
            status = "PASS：权重已训练，单图候选未启用"
        } catch { status = "FAIL：" + error.localizedDescription }
        busy = false
    }
}
#endif
