// 测试宿主与素材仅供 Debug 使用，正式页面不在本文件实现。
#if DEBUG
import CoreImage
import CoreMedia
import Foundation
import ImageIO
import SwiftUI
import UIKit
import UniformTypeIdentifiers

#if targetEnvironment(simulator)
struct AICurrentAppScreenshotUITestRoot: View {
    @State private var result = "未运行"

    var body: some View {
        VStack(spacing: 24) {
            Text("MaiChat 当前页面截图").font(.title2.bold())
            RoundedRectangle(cornerRadius: 24).fill(Color.cyan)
                .overlay(Text("CURRENT APP ONLY").font(.title.bold()))
                .frame(height: 220)
            Button("验证截图") { Task { await verify() } }
                .accessibilityIdentifier("current-app-capture-run")
            Text(result).accessibilityIdentifier("current-app-capture-result")
                .font(.caption).textSelection(.enabled)
        }.padding(28)
    }

    @MainActor private func verify() async {
        let capture = AIMobileScreenCapture.shared
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("current-app-capture-" + UUID().uuidString, isDirectory: true)
        func output(_ suffix: String) -> URL {
            root.appendingPathComponent("screenshots/mobile_screen_" + suffix + ".png")
        }
        func require(_ condition: Bool, _ message: String) throws {
            if !condition { throw NSError(domain: "CurrentAppCaptureTest", code: 1,
                                          userInfo: [NSLocalizedDescriptionKey: message]) }
        }
        result = "运行中"
        do {
            try require(!capture.isSharing, "unexpected sharing before capture")
            let file = output("ordinary")
            let receipt = try await capture.capture(to: file, workspace: root,
                target: .currentApp, delayMilliseconds: 150)
            guard let bitmap = UIImage(contentsOfFile: file.path)?.cgImage else {
                throw AIMobileScreenCaptureError.captureFailed
            }
            try require(receipt.source == .currentApp && bitmap.width > 300 && bitmap.height > 500,
                        "ordinary capture did not produce current window pixels")
#if DEBUG
            AIAssistantModel.shared.installVideoToolTestWorkspace(root.path)
#endif
            let hostFile = output("host")
            let args: [String: Any] = ["output_path": hostFile.path,
                                      "target": "current_app", "delay_ms": 100]
            let encoded = String(data: try JSONSerialization.data(withJSONObject: args), encoding: .utf8)!
            let host = await AIMobileHostToolProvider.shared.execute(
                name: "mobile_capture_screen", argumentsJSON: encoded)
            guard let text = host.output, let data = text.data(using: .utf8),
                  let reply = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
                throw NSError(domain: "CurrentAppCaptureTest", code: 2,
                              userInfo: [NSLocalizedDescriptionKey: host.errorMessage ?? "missing host output"])
            }
            try require(reply["source"] as? String == "current_app" &&
                        reply["whole_device"] as? Bool == false &&
                        UIImage(contentsOfFile: hostFile.path) != nil && !capture.isSharing,
                        "host capture source or sharing contract failed")
            for invalid in ["\"target\":\"device\"", "\"delay_ms\":true", "\"delay_ms\":1.5",
                            "\"delay_ms\":30001", "\"extra\":1"] {
                let rejected = await AIMobileHostToolProvider.shared.execute(
                    name: "mobile_capture_screen",
                    argumentsJSON: "{\"output_path\":\"" + output("invalid").path + "\"," + invalid + "}")
                try require(rejected.errorCode == "invalid_input", "invalid host arguments accepted")
            }
            do {
                _ = try await capture.capture(to: file, workspace: root, target: .currentApp)
                throw NSError(domain: "CurrentAppCaptureTest", code: 3)
            } catch AIMobileScreenCaptureError.invalidOutput {}
            do {
                _ = try await capture.capture(to: root.appendingPathComponent("mobile_screen_escape.png"),
                                              workspace: root, target: .currentApp)
                throw NSError(domain: "CurrentAppCaptureTest", code: 4)
            } catch AIMobileScreenCaptureError.invalidOutput {}
            let canceledFile = output("canceled")
            let delayed = Task { @MainActor in
                try await capture.capture(to: canceledFile, workspace: root,
                    target: .currentApp, delayMilliseconds: 30_000)
            }
            delayed.cancel()
            do { _ = try await delayed.value; throw NSError(domain: "CurrentAppCaptureTest", code: 5) }
            catch is CancellationError {}
            try require(!FileManager.default.fileExists(atPath: canceledFile.path), "canceled PNG leaked")
            do {
                _ = try await capture.capture(to: output("external"), workspace: root)
                throw NSError(domain: "CurrentAppCaptureTest", code: 6)
            } catch AIMobileScreenCaptureError.unsupported {}
            try require(!capture.isSharing, "current app capture started sharing")
#if DEBUG
            capture.captureCloudPageSmokeIfRequested()
            try await Task.sleep(for: .milliseconds(1_000))
            let smokeBase = FileManager.default.urls(for: .applicationSupportDirectory,
                in: .userDomainMask)[0].appendingPathComponent("MaiChat/ScreenCaptureSmoke")
            let smokeFolders = try FileManager.default.contentsOfDirectory(
                at: smokeBase, includingPropertiesForKeys: [.creationDateKey])
                .sorted { $0.lastPathComponent < $1.lastPathComponent }
            var verifiedSmoke = false
            for folder in smokeFolders {
                let bytes = try Data(contentsOf: folder.appendingPathComponent("result.json"))
                let report = try JSONSerialization.jsonObject(with: bytes) as? [String: Any]
                if report?["ok"] as? Bool == true &&
                    report?["source"] as? String == "current_app" &&
                    report?["sharing_before"] as? Bool == false &&
                    report?["sharing_after"] as? Bool == false,
                   let imagePath = report?["path"] as? String,
                   UIImage(contentsOfFile: imagePath) != nil { verifiedSmoke = true }
            }
            try require(verifiedSmoke, "opt-in ordinary capture hook did not save a PNG/report")
#endif
            result = "PASS source=current_app sharing=false \(bitmap.width)x\(bitmap.height)\n" + file.path
        } catch { result = "FAIL " + error.localizedDescription }
    }
}
#endif
#endif
