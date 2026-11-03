// 测试宿主与素材仅供 Debug 使用，正式页面不在本文件实现。
#if DEBUG
import CoreVideo
import MaiChatCore
import SwiftUI

#if DEBUG && targetEnvironment(simulator)
// 使用正式 sampler、档案 API 和原会话附件入口；仅素材与账号身份是隔离测试值。
struct AIScreenFramesUITestRoot: View {
    @ObservedObject private var model = AIAssistantModel.shared
    @State private var binding: AIScreenFrameImportBinding?
    @State private var presentation: AIScreenFrameImportBinding?
    @State private var result = "未运行"
    @State private var busy = false
    @State private var hostReference: AIMobileScreenArchiveReference?
    @State private var originalFrames: [AIMobileScreenArchiveFrame] = []

    var body: some View {
        VStack(spacing: 18) {
            Text("屏幕截图选择").font(.title2.bold())
            Button("准备真实采样") { Task { await prepare() } }
                .disabled(busy || binding != nil)
                .accessibilityIdentifier("screen-frames-fixture-prepare")
            Button("打开截图") { presentation = binding }
                .disabled(binding == nil || busy)
                .accessibilityIdentifier("screen-frames-fixture-open")
            Button("切到 B 后打开 A 截图") {
                model.selected = "switch-B"
                presentation = binding
            }
            .disabled(binding == nil || busy)
            .accessibilityIdentifier("screen-frames-fixture-open-from-b")
            Text("A=\(model.attachments(for: "switch-A").count) B=\(model.attachments(for: "switch-B").count)")
                .accessibilityIdentifier("screen-frames-fixture-drafts")
            Button("验证账号隔离") { Task { await verifyAccountIsolation() } }
                .disabled(binding == nil || busy)
                .accessibilityIdentifier("screen-frames-fixture-account")
            Button("验证原图引用与回收") { Task { await verifyReferences() } }
                .disabled(binding == nil || busy)
                .accessibilityIdentifier("screen-frames-fixture-verify")
            Text(result).font(.caption).accessibilityIdentifier("screen-frames-fixture-result")
        }
        .padding(24)
        .sheet(item: $presentation) { original in
            AIMobileScreenFramesView(model: model, binding: original)
        }
    }

    @MainActor private func prepare() async {
        busy = true
        result = "真实采样中"
        defer { busy = false }
        do {
            let workspace = FileManager.default.temporaryDirectory
                .appendingPathComponent("screen-frame-ui-" + UUID().uuidString, isDirectory: true)
            model.installSessionSwitchUITestFixture()
            model.installScreenFrameUITestWorkspace(workspace.path, accountID: "screen-frame-test-a")
            let original = try model.makeScreenFrameImportSnapshot()
            let context = try await AIMobileScreenFrameArchive.shared.prepareScreenArchiveUITest(
                workspace: workspace, sessionID: original.ownerSessionID,
                pixelSize: CGSize(width: UIScreen.main.bounds.width * UIScreen.main.scale,
                                  height: UIScreen.main.bounds.height * UIScreen.main.scale))
            let listing = try await AIMobileScreenFrameArchive.shared.list(
                workspace: workspace, sessionID: original.ownerSessionID, batchID: context.batchID)
            guard let first = listing.frames.first else { throw AIBackendError(message: "缺少采样图片") }
            let payload = ["workspace_path": workspace.path, "session_id": original.ownerSessionID,
                           "batch_id": context.batchID, "frame_id": first.id]
            let encoded = String(data: try JSONSerialization.data(withJSONObject: payload), encoding: .utf8)!
            let retained = await AIMobileHostToolProvider.shared.execute(
                name: "mobile_retain_screen_frame", argumentsJSON: encoded)
            guard retained.errorCode == nil, let data = retained.output?.data(using: .utf8),
                  let receipt = try JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let file = receipt["file_path"] as? String,
                  let referenceID = receipt["reference_id"] as? String,
                  receipt["path"] as? String == first.path,
                  receipt["batch_id"] as? String == first.batchID,
                  receipt["frame_id"] as? String == first.id else {
                throw AIBackendError(message: retained.errorMessage ?? "宿主未保留原图引用")
            }
            let source = context.workspace.appendingPathComponent(first.path).standardizedFileURL
            guard URL(fileURLWithPath: file).standardizedFileURL == source,
                  first.path.hasPrefix(".maichat/screen-frame-archive/"),
                  !first.path.hasPrefix("screenshots/") else {
                throw AIBackendError(message: "宿主没有使用采样原图路径")
            }
            hostReference = AIMobileScreenArchiveReference(context: context, frame: first,
                referenceID: referenceID, fileURL: source)
            originalFrames = listing.frames.sorted { $0.elapsedMS < $1.elapsedMS }
            var foreign = payload
            foreign["session_id"] = original.ownerSessionID + ":another-owner"
            let rejected = await AIMobileHostToolProvider.shared.execute(
                name: "mobile_retain_screen_frame", argumentsJSON: String(data:
                    try JSONSerialization.data(withJSONObject: foreign), encoding: .utf8)!)
            guard rejected.errorCode == "not_found" else {
                throw AIBackendError(message: "宿主接受了其他会话的采样图片")
            }
            binding = try model.registerScreenFrameArchive(context: context, snapshot: original)
            result = "采样已准备"
        } catch { result = "FAIL: " + error.localizedDescription }
    }

    @MainActor private func verifyAccountIsolation() async {
        guard let binding else { return }
        busy = true
        defer {
            model.installScreenFrameUITestWorkspace(binding.context.workspace.path,
                                                     accountID: "screen-frame-test-a")
            busy = false
        }
        do {
            let listing = try await AIMobileScreenFrameArchive.shared.list(
                workspace: binding.context.workspace, sessionID: binding.context.ownerSessionID,
                batchID: binding.id)
            guard let first = listing.frames.first else { throw AIBackendError(message: "缺少真实截图") }
            model.installScreenFrameUITestWorkspace(binding.context.workspace.path,
                                                     accountID: "screen-frame-test-b")
            var rejected = false
            do { _ = try await model.addScreenFrames([first.id], binding: binding) }
            catch { rejected = true }
            guard rejected, model.attachments(for: "switch-A").isEmpty,
                  model.attachments(for: "switch-B").isEmpty else {
                throw AIBackendError(message: "另一个账号接受了截图")
            }
            result = "账号隔离通过"
        } catch { result = "FAIL: " + error.localizedDescription }
    }

    @MainActor private func verifyReferences() async {
        guard let binding else { return }
        busy = true
        defer { busy = false }
        do {
            let archive = AIMobileScreenFrameArchive.shared
            let originals = try await archive.list(workspace: binding.context.workspace,
                                                   sessionID: binding.context.ownerSessionID)
            let attachments = model.attachments(for: "switch-A")
            guard !originals.batches.contains(where: { $0.id == binding.id }),
                  attachments.count == 2, model.attachments(for: "switch-B").isEmpty,
                  originalFrames.count >= 4, let hostReference else {
                throw AIBackendError(message: "清理状态或原会话附件数量不正确")
            }
            let references = attachments.compactMap(\.archiveReference)
            guard references.count == 2,
                  Set(references.map { $0.frame.id }) == Set(originalFrames.prefix(2).map(\.id)),
                  references.allSatisfy({ $0.referenceID != hostReference.referenceID }),
                  zip(attachments, references).allSatisfy({ file, reference in
                      file.relativePath == reference.frame.path &&
                          file.relativePath == reference.relativePath &&
                          reference.fileURL == binding.context.workspace
                              .appendingPathComponent(reference.frame.path).standardizedFileURL
                  }) else { throw AIBackendError(message: "选图没有保留独立的原路径引用") }
            let expectedFiles = Set(originalFrames.prefix(2).map(\.path))
            let context = binding.context
            let frames = originalFrames
            try await Task.detached(priority: .utility) {
                let directory = context.workspace.appendingPathComponent("screenshots", isDirectory: true)
                let names = (try? FileManager.default.contentsOfDirectory(atPath: directory.path)) ?? []
                guard !names.contains(where: { $0.lowercased().hasSuffix(".png") }) else {
                    throw AIBackendError(message: "选图创建了重复PNG")
                }
                for frame in frames {
                    let file = context.workspace.appendingPathComponent(frame.path)
                    if expectedFiles.contains(frame.path) {
                        let data = try Data(contentsOf: file)
                        guard data.starts(with: [137, 80, 78, 71, 13, 10, 26, 10]) else {
                            throw AIBackendError(message: "被引用原图已损坏或被清理")
                        }
                    } else if FileManager.default.fileExists(atPath: file.path) {
                        throw AIBackendError(message: "未选原图没有回收")
                    }
                }
            }.value

            // Removing the draft token must not invalidate the independent
            // host/history token for the very same original PNG.
            guard let first = attachments.first(where: { $0.archiveReference?.frame.id == hostReference.frame.id }),
                  let second = attachments.first(where: { $0.archiveReference?.frame.id != hostReference.frame.id }),
                  let secondReference = second.archiveReference else {
                throw AIBackendError(message: "缺少多引用测试图片")
            }
            model.removeAttachment(first, from: "switch-A")
            _ = try await archive.get(context: hostReference.context, frameID: hostReference.frame.id)
            model.removeAttachment(second, from: "switch-A")
            try await waitForRemoval(secondReference.fileURL)
            _ = try await archive.get(context: hostReference.context, frameID: hostReference.frame.id)
            try await archive.releaseSession(workspace: binding.context.workspace, sessionID: "switch-A")
            try await waitForRemoval(hostReference.fileURL)
            result = "PASS: 原图无重复、独立引用安全、未选清理及会话回收通过"
        } catch { result = "FAIL: " + error.localizedDescription }
    }

    private func waitForRemoval(_ file: URL) async throws {
        let clock = ContinuousClock()
        let deadline = clock.now.advanced(by: .seconds(3))
        while clock.now < deadline {
            let exists = await Task.detached(priority: .utility) {
                FileManager.default.fileExists(atPath: file.path)
            }.value
            if !exists { return }
            try await Task.sleep(for: .milliseconds(50))
        }
        throw AIBackendError(message: "最后一个引用释放后原图未在3秒内回收")
    }

}
#endif

#if DEBUG && targetEnvironment(simulator)
// The generated buffer is immutable after preparation and is read only on the
// sampler's serial queue.
private struct AIMobileScreenSamplerFixturePixels: @unchecked Sendable {
    let buffer: CVPixelBuffer
}

private final class AIMobileScreenSamplerFixtureProbe: @unchecked Sendable {
    private let lock = NSLock()
    private var savedCount = 0
    private var failure: String?
    var count: Int { lock.lock(); defer { lock.unlock() }; return savedCount }
    var error: String? { lock.lock(); defer { lock.unlock() }; return failure }
    func changed(_ batch: AIMobileScreenArchiveBatch?, _ error: String?) {
        lock.lock()
        if let batch { savedCount = batch.frameCount }
        if let error { failure = error }
        lock.unlock()
    }
}

enum AIMobileScreenArchiveSimulatorFixture {
    // Uses the production sampler and ordinary C-backed archive APIs. Only the
    // source pixel buffer is safe generated content; timing and files are real.
    static func prepare(workspace root: URL, sessionID: String,
                        pixelSize: CGSize = CGSize(width: 320, height: 180)) async throws
        -> AIMobileScreenArchiveContext {
        let archive = AIMobileScreenFrameArchive.shared
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let (context, _) = try await archive.begin(workspace: root, sessionID: sessionID)
        let queue = DispatchQueue(label: "maichat.screen-archive-fixture")
        let probe = AIMobileScreenSamplerFixtureProbe()
        let sampler = AIMobileScreenFrameSampler(queue: queue,
            changed: { probe.changed($0, $1) }, terminal: { probe.changed(nil, $0) })
        func require(_ condition: Bool, _ message: String) throws {
            if !condition { throw AIMobileScreenArchiveError(code: "fixture_failed", message: message,
                                                             terminal: false, limitReason: nil) }
        }
        func waitForFrames(_ count: Int) async throws {
            for _ in 0..<65 {
                try Task.checkCancellation()
                if probe.count >= count { return }
                if let error = probe.error { try require(false, error) }
                try await Task.sleep(for: .milliseconds(100))
            }
            try require(false, "two-second sampler did not save expected stationary frames")
        }
        func finish(_ reason: String) async {
            await withCheckedContinuation { continuation in
                sampler.finish(reason: reason) { continuation.resume() }
            }
        }
        do {
            func makePixels(width: Int, height: Int) throws -> AIMobileScreenSamplerFixturePixels {
                try require(width > 0 && height > 0 && width <= 4096 && height <= 4096,
                            "fixture pixel size is out of bounds")
                var allocated: CVPixelBuffer?
                let status = CVPixelBufferCreate(kCFAllocatorDefault, width, height,
                    kCVPixelFormatType_32BGRA, [kCVPixelBufferCGImageCompatibilityKey: true,
                        kCVPixelBufferCGBitmapContextCompatibilityKey: true] as CFDictionary, &allocated)
                try require(status == kCVReturnSuccess && allocated != nil, "pixel buffer allocation failed")
                let pixels = allocated!
                CVPixelBufferLockBaseAddress(pixels, [])
                let memory = CVPixelBufferGetBaseAddress(pixels)!.assumingMemoryBound(to: UInt8.self)
                let stride = CVPixelBufferGetBytesPerRow(pixels)
                for y in 0..<height {
                    for x in 0..<width {
                        let offset = y * stride + x * 4
                        memory[offset] = x < width / 2 ? 35 : 240
                        memory[offset + 1] = 80
                        memory[offset + 2] = x < width / 2 ? 240 : 35
                        memory[offset + 3] = 255
                    }
                }
                CVPixelBufferUnlockBaseAddress(pixels, [])
                return AIMobileScreenSamplerFixturePixels(buffer: pixels)
            }
            let immutablePixels = try makePixels(width: Int(pixelSize.width), height: Int(pixelSize.height))
            sampler.bind(context: context)
            sampler.setPhase(.active)
            sampler.start(context: context)
            queue.async { sampler.accept(immutablePixels.buffer) }
            try await waitForFrames(2)
            let activeCount = probe.count
            sampler.setPhase(.inactive)
            try await Task.sleep(for: .milliseconds(2_200))
            try require(probe.count == activeCount, "inactive system/transition frame was archived")
            sampler.setPhase(.active)
            try await Task.sleep(for: .milliseconds(2_200))
            try require(probe.count == activeCount, "old phase buffer was reused")
            sampler.setPhase(.background)
            // Keep a landscape source as well: full-viewport fitting must not
            // stretch or crop media whose aspect ratio differs from the device.
            let landscapePixels = try makePixels(width: 320, height: 180)
            queue.async { sampler.accept(landscapePixels.buffer) }
            try await waitForFrames(activeCount + 2)
            let external = try sampler.latestExternal()
            try require(!external.0.isEmpty, "independent background frame cache was not updated")
            await finish("fixture_stop")
            let listing = try await archive.list(workspace: root, sessionID: context.ownerSessionID,
                                                 batchID: context.batchID)
            try require(listing.frames.count >= 4 && listing.batches.first?.status == "stopped",
                        "stopping did not retain the archived frames")
            for index in 1..<listing.frames.count {
                try require(listing.frames[index].elapsedMS - listing.frames[index - 1].elapsedMS >= 2_000,
                            "archive minimum interval was violated")
            }
            let first = listing.frames[0]
            let references = try await archive.retainSelected(frameIDs: [first.id], context: context)
            do {
                let expected = root.standardizedFileURL.resolvingSymlinksInPath()
                    .appendingPathComponent(first.path)
                try require(references.count == 1 && references[0].relativePath == first.path &&
                            references[0].fileURL == expected &&
                            FileManager.default.fileExists(atPath: expected.path),
                            "selected frame did not retain its original path")
                try await archive.release(references: references)
            } catch {
                try? await archive.release(references: references)
                throw error
            }
            let foreign = AIMobileScreenArchiveContext(workspace: root,
                ownerSessionID: sessionID + ":another-owner", batchID: context.batchID)
            do {
                _ = try await archive.get(context: foreign, frameID: first.id)
                try require(false, "another owner accessed the archived frame")
            } catch let error as AIMobileScreenArchiveError {
                try require(error.code != "fixture_failed", error.message)
            }
            return context
        } catch {
            await finish("fixture_failed")
            throw error
        }
    }
}

extension AIMobileScreenFrameArchive {
    func prepareScreenArchiveUITest(workspace: URL, sessionID: String,
                                   pixelSize: CGSize = CGSize(width: 320, height: 180)) async throws
        -> AIMobileScreenArchiveContext {
        try await AIMobileScreenArchiveSimulatorFixture.prepare(workspace: workspace,
            sessionID: sessionID, pixelSize: pixelSize)
    }
}
#endif
#endif
