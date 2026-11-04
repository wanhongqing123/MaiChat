// 相机请求代次和测试控制按钮，与正式 composer 分离。
#if DEBUG && targetEnvironment(simulator)
import SwiftUI
import UIKit

enum AICameraUITestSupport {
    static var isManualToolRequested: Bool {
        ProcessInfo.processInfo.arguments.contains("--ai-ui-test") &&
            ProcessInfo.processInfo.arguments.contains("--ai-manual-camera-tool-ui-test")
    }

    static var isManualRequested: Bool {
        ProcessInfo.processInfo.arguments.contains("--ai-ui-test") &&
            (ProcessInfo.processInfo.arguments.contains("--ai-manual-camera-ui-test") || isManualToolRequested)
    }

    @MainActor private(set) static var manualFixtureWorkspace: URL?

    static var isRequested: Bool {
        ProcessInfo.processInfo.arguments.contains("--agent-camera-ui-test")
    }

    @MainActor static func prepareManualFixture(_ model: AIAssistantModel) {
        guard isManualRequested else { return }
        let token = ProcessInfo.processInfo.environment["MAICHAT_AI_TEST_ID"].flatMap(UUID.init(uuidString:)) ?? UUID()
        let workspace = FileManager.default.temporaryDirectory
            .appendingPathComponent("manual-camera-ui-" + token.uuidString, isDirectory: true)
            .appendingPathComponent("Workspace", isDirectory: true)
        do {
            try FileManager.default.createDirectory(at: workspace, withIntermediateDirectories: true)
            manualFixtureWorkspace = workspace
            model.installSessionSwitchUITestFixture()
            model.ready = false
            model.installVideoToolTestWorkspace(workspace.path)
            model.uiTestSupport.screenFrameAccountID = "manual-camera-ui-fixture-owner"
            // 模式测试不启动 native Agent；异步准备真实文件导入后才开放普通拍摄入口。
            Task { @MainActor in
                do {
                    try await model.prepareManualCameraImportForUITesting(workspace)
                    guard model === AIAssistantModel.shared else { return }
                    model.ready = true
                } catch { model.showTransientError("合成相机测试导入目录不可用") }
            }
        } catch { model.showTransientError("合成相机测试目录不可用") }
    }

    @MainActor static func prepareMusicMini(_ music: MaiLocalMusicPlayer) {
        guard ProcessInfo.processInfo.arguments.contains("--ai-music-mini-ui-test"),
              let sample = Bundle.main.url(forResource: "ffplay-seek-sample", withExtension: "mp4")
        else { return }
        try? music.openLocalFile(sample, title: "测试音乐")
    }
}

/// 只显示合成色块和显式快门；没有 AVCaptureSession、定时器或真人画面。
struct AIManualCameraUITestPreview: View {
    let onCapture: (UIImage) -> Void
    let onCancel: () -> Void
    @ObservedObject private var capture = AIMobileCameraCapture.shared
    @ObservedObject private var requests = AIMobileLocalMediaRequests.shared

    var body: some View {
        ZStack {
            Color.blue.ignoresSafeArea()
            VStack(spacing: 24) {
                Text("手动快门合成画面").foregroundStyle(.white)
                Text("定时状态")
                    .accessibilityIdentifier("ai-manual-camera-timed-state")
                    .accessibilityValue("running:\(capture.isRunning);request:\(requests.cameraRequestID != nil)")
                Button("拍照") {
                    let image = UIGraphicsImageRenderer(size: CGSize(width: 320, height: 240)).image { context in
                        UIColor.blue.setFill(); context.fill(CGRect(x: 0, y: 0, width: 320, height: 240))
                    }
                    onCapture(image)
                }.accessibilityIdentifier("ai-manual-camera-shutter")
                Button("取消", action: onCancel).accessibilityIdentifier("ai-manual-camera-cancel")
                if AICameraUITestSupport.isManualToolRequested {
                    AICameraManualToolUITestPreviewControls()
                }
            }
            .font(.headline).foregroundStyle(.white)
        }
    }
}

/// 只在双重 opt-in 的独立工作区内调用正式宿主；不生成模型回复，也不修改生产草稿。
@MainActor
private final class AICameraManualToolUITestState: ObservableObject {
    static let shared = AICameraManualToolUITestState()
    @Published private(set) var result = "idle"
    @Published private(set) var path = ""
    @Published private(set) var mimeType = ""
    @Published private(set) var fileCount = 0
    @Published private(set) var verified = false
    private var task: Task<Void, Never>?

    func start() {
        let model = AIAssistantModel.shared
        guard AICameraUITestSupport.isManualToolRequested, task == nil, model.ready,
              let workspace = AICameraUITestSupport.manualFixtureWorkspace,
              model.workspacePath == workspace.path,
              model.uiTestSupport.screenFrameAccountID == "manual-camera-ui-fixture-owner",
              model.selected.hasPrefix("switch-") else { return }
        let context = ["workspace_path": workspace.path, "session_id": model.selected]
        guard let data = try? JSONSerialization.data(withJSONObject: context) else { return }
        result = "pending"
        path = ""
        mimeType = ""
        verified = false
        refreshFiles()
        task = Task { @MainActor in
            let receipt = await AIMobileHostToolProvider.shared.execute(name: "mobile_take_photo",
                argumentsJSON: String(decoding: data, as: UTF8.self))
            defer { task = nil; refreshFiles() }
            if let code = receipt.errorCode { result = code; return }
            guard let output = receipt.output?.data(using: .utf8),
                  let object = try? JSONSerialization.jsonObject(with: output) as? [String: Any],
                  let returnedPath = object["path"] as? String,
                  let returnedMime = object["mime_type"] as? String else {
                result = "invalid_receipt"; return
            }
            path = returnedPath
            mimeType = returnedMime
            let image = workspace.appendingPathComponent(returnedPath).standardizedFileURL
                .resolvingSymlinksInPath()
            let root = workspace.standardizedFileURL.resolvingSymlinksInPath().path + "/"
            verified = !returnedPath.hasPrefix("/") && image.path.hasPrefix(root) &&
                returnedMime == "image/jpeg" && UIImage(contentsOfFile: image.path) != nil
            result = "succeeded"
        }
    }

    func cancelTask() {
        guard AICameraUITestSupport.isManualToolRequested else { return }
        task?.cancel()
    }

    func switchSession() {
        guard AICameraUITestSupport.isManualToolRequested, task != nil else { return }
        let model = AIAssistantModel.shared
        _ = model.selectUITestConversationIfNeeded(model.selected == "switch-A" ? "switch-B" : "switch-A")
    }

    /// 显式刷新而非定时轮询，保证越过两秒后的文件断言来自当前磁盘。
    func refreshFiles() {
        guard AICameraUITestSupport.isManualToolRequested,
              let workspace = AICameraUITestSupport.manualFixtureWorkspace,
              let files = FileManager.default.enumerator(at: workspace,
                includingPropertiesForKeys: [.isRegularFileKey]) else { fileCount = 0; return }
        fileCount = files.compactMap { $0 as? URL }.filter {
            ["jpg", "jpeg", "png", "heic"].contains($0.pathExtension.lowercased()) &&
                (try? $0.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true
        }.count
    }
}

private struct AICameraManualToolUITestStatus: View {
    let prefix: String
    @ObservedObject private var state = AICameraManualToolUITestState.shared
    @ObservedObject private var model = AIAssistantModel.shared
    @ObservedObject private var requests = AIMobileLocalMediaRequests.shared

    var body: some View {
        HStack {
            Text("工具状态").accessibilityIdentifier(prefix + "-state")
                .accessibilityValue("result:\(state.result);files:\(state.fileCount);attachments:\(model.pendingAttachments.values.flatMap { $0 }.count);request:\(requests.manualPhotoRequestID != nil);verified:\(state.verified)")
            Text("返回路径").accessibilityIdentifier(prefix + "-path").accessibilityValue(state.path)
            Text("返回类型").accessibilityIdentifier(prefix + "-mime").accessibilityValue(state.mimeType)
            Text("会话").accessibilityIdentifier(prefix + "-session").accessibilityValue(model.selected)
        }.font(.caption)
    }
}

struct AICameraManualToolUITestControls: View {
    @ObservedObject private var state = AICameraManualToolUITestState.shared
    @ObservedObject private var model = AIAssistantModel.shared

    var body: some View {
        VStack(spacing: 4) {
            HStack {
                Button("执行手动拍照工具") { state.start() }
                    .disabled(!model.ready || state.result == "pending")
                    .accessibilityIdentifier("ai-manual-camera-tool-open")
                Button("刷新工具状态") { state.refreshFiles() }
                    .accessibilityIdentifier("ai-manual-camera-tool-refresh")
            }
            AICameraManualToolUITestStatus(prefix: "ai-manual-camera-tool")
        }
    }
}

/// 覆盖层中的取消按钮操作持有正式工具执行的 Task，切会话走现有隔离会话接口。
private struct AICameraManualToolUITestPreviewControls: View {
    @ObservedObject private var state = AICameraManualToolUITestState.shared

    var body: some View {
        VStack(spacing: 12) {
            AICameraManualToolUITestStatus(prefix: "ai-manual-camera-tool-preview")
            Button("刷新工具状态") { state.refreshFiles() }
                .accessibilityIdentifier("ai-manual-camera-tool-preview-refresh")
            Button("取消工具 Task") { state.cancelTask() }
                .accessibilityIdentifier("ai-manual-camera-tool-cancel-task")
            Button("切换测试会话") { state.switchSession() }
                .accessibilityIdentifier("ai-manual-camera-tool-switch-session")
        }
    }
}

struct AICameraUITestControls: View {
    let open: () -> Void
    @State private var ownershipResult = ""

    var body: some View {
        Group {
            Button("模拟 Agent 打开相机", action: open)
                .accessibilityIdentifier("ai-camera-fixture-open")
            Button("验证相机请求代次") {
                ownershipResult = AIMobileLocalMediaRequests.verifyCameraRequestOwnership()
                    ? "相机请求代次通过" : "相机请求代次失败"
            }.accessibilityIdentifier("ai-camera-fixture-race")
            if !ownershipResult.isEmpty {
                Text(ownershipResult).accessibilityIdentifier("ai-camera-fixture-race-result")
            }
        }
    }
}

extension AIMobileLocalMediaRequests {
    /// 使用独立对象模拟旧授权与旧封存结果迟到，绝不接触当前相机或生产账号。
    static func verifyCameraRequestOwnership() -> Bool {
        let state = AIMobileLocalMediaRequests()
        state.presenterActive = true
        do {
            _ = try state.openCamera(in: "fixture-a", workspacePath: "/tmp")
            guard let first = state.cameraRequestID, state.beginCameraOpening(requestID: first),
                  state.closeCamera(in: "fixture-a"), state.cameraState == "stopping" else { return false }
            // 旧授权尚未返回前，不能开启另一轮。
            do { _ = try state.openCamera(in: "fixture-b", workspacePath: "/tmp"); return false }
            catch { }
            state.finishCameraOpening(requestID: first)
            _ = try state.openCamera(in: "fixture-b", workspacePath: "/tmp")
            guard let second = state.cameraRequestID, state.beginCameraOpening(requestID: second) else { return false }
            state.failCamera(.permissionDenied, requestID: first)
            state.finishCameraOpening(requestID: first)
            state.recordCameraBatch("stale-batch", requestID: first)
            guard state.cameraRequestID == second, state.cameraState == "opening" else { return false }
            state.recordCameraBatch("fixture-batch", requestID: second)
            state.markCameraVisible(second, batchID: "fixture-batch")
            state.finishCameraOpening(requestID: second)
            guard state.closeCamera(in: "fixture-b"), state.cameraState == "stopping" else { return false }
            state.finishCameraArchive(batchID: "stale-batch", count: 9)
            guard state.cameraState == "stopping" else { return false }
            state.finishCameraArchive(batchID: "fixture-batch", count: 2)
            return state.cameraState == "captured" && state.cameraStatus(in: "fixture-b")["frame_count"] == "2"
        } catch { return false }
    }
}
#endif
