// 测试宿主：使用公开样例走正常 Root、宿主工具、C++ 处理及现有媒体预览，不请求付费模型。
#if DEBUG && targetEnvironment(simulator)
import SwiftUI
import Foundation
import MaiChatCore

struct AIPortraitUITestRoot: View {
    @StateObject private var state = RemoteIMAppState(loadedSettings: .empty)
    @State private var prepared = false
    @State private var status = "准备照片"
    @State private var resultPath = ""
    @State private var busy = true

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
                    Button("预览修图结果") {
                        Task { _ = await call(
                            "mobile_preview_image", arguments: ["path": resultPath]) }
                    }.disabled(resultPath.isEmpty || busy)
                    Button("测试本地训练") { Task { await train() } }.disabled(busy)
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
    @MainActor private func process() async {
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
            // 助手图片使用真实 agent_send_media 交付片段，复用既有消息缩略图和全屏预览。
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
