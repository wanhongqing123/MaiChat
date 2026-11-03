// 相机请求代次和测试控制按钮，与正式 composer 分离。
#if DEBUG && targetEnvironment(simulator)
import SwiftUI
import UIKit

enum AICameraUITestSupport {
    static var isRequested: Bool {
        ProcessInfo.processInfo.arguments.contains("--agent-camera-ui-test")
    }

    @MainActor static func prepareMusicMini(_ music: MaiLocalMusicPlayer) {
        guard ProcessInfo.processInfo.arguments.contains("--ai-music-mini-ui-test"),
              let sample = Bundle.main.url(forResource: "ffplay-seek-sample", withExtension: "mp4")
        else { return }
        try? music.openLocalFile(sample, title: "测试音乐")
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
