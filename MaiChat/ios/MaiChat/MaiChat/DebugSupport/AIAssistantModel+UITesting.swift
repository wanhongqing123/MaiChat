// 测试宿主与素材仅供 Debug 使用，正式页面不在本文件实现。
#if DEBUG
import Foundation
import UIKit

@MainActor
final class AIAssistantModelUITestSupport {
    var sessionSwitchEnabled = false
    var screenFrameAccountID: String?
    // Isolated composer fixtures confirm routing without contacting a model.
    var steeringHandler: ((AISteeringRequest) async throws -> String)?
}

enum AIAssistantUITestMutation {
    case workspace(String)
    case remoteSessions([AIRemoteSession])
    case clearRemoteSelection
    case attachments([String: [AIImportedFile]], firstMessageSessionID: String)
    case selectedHistory(String, [AIMessage])
}

extension AIAssistantModel {
    func installVideoToolTestWorkspace(_ path: String) {
        applyUITestMutation(.workspace(path))
    }
}

#if targetEnvironment(simulator)
extension AIAssistantModel {
    /// The host receipt comes from the real manual-photo execution. Round-trip
    /// the persisted part representation so a restored result uses the same row.
    @discardableResult
    func installManualPhotoToolReceiptForUITesting(_ output: String, in sessionID: String) -> Bool {
        guard AICameraUITestSupport.isManualToolRequested, selected == sessionID else { return false }
        let part = AIPart(id: "manual-photo-result-part", kind: "tool", tool: "mobile_take_photo",
                          input: "{}", output: output, state: "completed")
        let completed = AIMessage(id: "manual-photo-result-message", role: "assistant", created: 1,
                                  completed: 2, active: false, parts: [part])
        guard let data = try? JSONEncoder().encode(completed),
              let restored = try? JSONDecoder().decode(AIMessage.self, from: data) else { return false }
        applyUITestMutation(.selectedHistory(sessionID, [restored]))
        return restored == completed
    }

    func installSteeringUITestFixture(
        handler: @escaping (AISteeringRequest) async throws -> String
    ) {
        let workspace = FileManager.default.temporaryDirectory
            .appendingPathComponent("steering-outbox-" + ProcessInfo.processInfo.environment[
                "MAICHAT_AI_TEST_ID", default: UUID().uuidString], isDirectory: true)
            .appendingPathComponent("Workspace", isDirectory: true)
        try? FileManager.default.createDirectory(at: workspace, withIntermediateDirectories: true)
        applyUITestMutation(.workspace(workspace.path))
        uiTestSupport.sessionSwitchEnabled = true
        uiTestSupport.steeringHandler = handler
        uiTestSupport.screenFrameAccountID = "steering-fixture-owner"
        sessions = [AISession(id: "switch-A", title: "正在整理素材", busy: true,
                              turnId: "msg_fixture_turn_A"),
                    AISession(id: "switch-B", title: "另一个对话", busy: false)]
        selected = "switch-A"
        messages = sessionSwitchUITestMessages("A")
        configured = true
        ready = true
        historyLoaded = true
    }

    func installScreenFrameUITestWorkspace(_ path: String, accountID: String) {
        guard ProcessInfo.processInfo.arguments.contains("--ai-screen-frames-ui-test") else { return }
        applyUITestMutation(.workspace(path))
        uiTestSupport.screenFrameAccountID = accountID
    }

    func installImageZoomUITestFixture(_ path: String) {
        let session = "image-zoom-ui-test"
        sessions = [AISession(id: session, title: "图片预览测试", busy: false)]
        selected = session
        let object = ["delivery": "current_ai_session", "path": (path as NSString).lastPathComponent,
                      "type": "image", "mime_type": "image/png", "caption": "图片预览"]
        guard let data = try? JSONSerialization.data(withJSONObject: object),
              let output = String(data: data, encoding: .utf8) else { return }
        let part = AIPart(id: "image-zoom-part", kind: "tool", text: nil,
                          tool: "agent_send_media", input: "{}", output: output,
                          error: nil, state: "completed",
                          path: nil, mimeType: nil)
        messages = [AIMessage(id: "image-zoom-message", role: "assistant", created: 0,
                              completed: 1, active: false, parts: [part])]
        applyUITestMutation(.workspace((path as NSString).deletingLastPathComponent))
        ready = true
    }

    func installHistoryUITestFixture() {
        let session = "history-ui-test"
        sessions = [AISession(id: session, title: "历史会话", busy: false)]
        selected = session
        messages = (0..<80).map { index in
            let part = AIPart(id: "history-part-\(index)", kind: "text",
                              text: "历史消息 \(index)\n第二行内容", tool: nil,
                              input: nil, output: nil, error: nil, state: nil,
                              path: nil, mimeType: nil)
            return AIMessage(id: "history-message-\(index)", role: "assistant",
                             created: Int64(index), completed: Int64(index + 1),
                             active: false, parts: [part])
        }
        ready = true
    }

    func installStreamingTypingUITestFixture() {
        let session = "stream-typing-ui-test"
        sessions = [AISession(id: session, title: "流式输入测试", busy: true)]
        selected = session
        messages = (0..<30).map { index in
            let part = AIPart(id: "stream-old-part-\(index)", kind: "text",
                              text: "先前的消息 \(index)", tool: nil,
                              input: nil, output: nil, error: nil, state: nil,
                              path: nil, mimeType: nil)
            return AIMessage(id: String(format: "stream-message-%02d", index),
                             role: "assistant", created: Int64(index),
                             completed: Int64(index + 1), active: false, parts: [part])
        }
        ready = true
        historyLoaded = true
        Task { @MainActor [weak self] in
            for index in 0..<60 {
                try? await Task.sleep(for: .milliseconds(100))
                guard let self, self.selected == session else { return }
                let part = AIPart(id: "stream-active-part", kind: "text",
                                  text: String(repeating: "正在输出内容。", count: index + 1),
                                  tool: nil, input: nil, output: nil, error: nil, state: nil,
                                  path: nil, mimeType: nil)
                let active = AIMessage(id: "stream-message-30", role: "assistant",
                                       created: 30, completed: 0, active: true, parts: [part])
                if self.messages.count == 30 { self.messages.append(active) }
                else { self.messages[30] = active }
            }
        }
    }

    func installSessionSwitchUITestFixture() {
        uiTestSupport.sessionSwitchEnabled = true
        sessions = ["A", "B", "C"].map {
            AISession(id: "switch-\($0)", title: "对话 \($0)", busy: false)
        }
        applyUITestMutation(.remoteSessions([AIRemoteSession(
            sessionID: "remote-demo", title: "Mac 上的编译任务", updatedAt: nil, busy: false,
            deviceSlot: "mac", deviceName: "工作 Mac", deviceID: "fixturemac",
            loginSessionID: "fixturelogin", online: false)]))
        selected = "switch-A"
        messages = sessionSwitchUITestMessages("A")
        ready = true
        historyLoaded = true
    }

    func installWelcomeUITestFixture() {
        sessions = []
        selected = ""
        applyUITestMutation(.clearRemoteSelection)
        messages = []
        configured = true
        ready = true
        historyLoaded = true
    }

    func installMediaDraftUITestFixture() {
        installSessionSwitchUITestFixture()
        let workspace = FileManager.default.temporaryDirectory
            .appendingPathComponent("ai-media-draft-ui-test", isDirectory: true)
        guard let video = Bundle.main.url(forResource: "ffplay-sample", withExtension: "mp4"),
              (try? FileManager.default.createDirectory(
                at: workspace, withIntermediateDirectories: true
              )) != nil else { return }
        let image = UIGraphicsImageRenderer(size: CGSize(width: 220, height: 160))
            .image { context in
                UIColor.systemBlue.setFill()
                context.fill(CGRect(x: 0, y: 0, width: 220, height: 160))
                UIColor.white.setFill()
                context.fill(CGRect(x: 45, y: 45, width: 130, height: 70))
            }
        let imageFile = workspace.appendingPathComponent("draft-image.png")
        let emptyDraftImageFile = workspace.appendingPathComponent("empty-draft-image.png")
        let videoFile = workspace.appendingPathComponent("draft-video.mp4")
        guard let imageData = image.pngData(),
              (try? imageData.write(to: imageFile, options: .atomic)) != nil,
              (try? imageData.write(to: emptyDraftImageFile, options: .atomic)) != nil else {
            return
        }
        if !FileManager.default.fileExists(atPath: videoFile.path) {
            try? FileManager.default.copyItem(at: video, to: videoFile)
        }
        applyUITestMutation(.workspace(workspace.path))
        var attachments: [String: [AIImportedFile]] = [:]
        attachments[selected] = [
            AIImportedFile(relativePath: imageFile.lastPathComponent,
                           mimeType: "image/png", isImage: true),
            AIImportedFile(relativePath: videoFile.lastPathComponent,
                           mimeType: "video/mp4", isImage: false)
        ]
        attachments[""] = [
            AIImportedFile(relativePath: emptyDraftImageFile.lastPathComponent,
                           mimeType: "image/png", isImage: true)
        ]
        // 模拟过去的首发目标仍存在，检验手动回到空草稿再选 A 不会误迁移文字。
        applyUITestMutation(.attachments(attachments, firstMessageSessionID: "switch-A"))
    }

    private func sessionSwitchUITestMessages(_ title: String) -> [AIMessage] {
        (0..<80).map { index in
            let part = AIPart(id: "switch-part-\(title)-\(index)", kind: "text",
                              text: "\(title) 历史消息 \(index)\n第二行内容", tool: nil,
                              input: nil, output: nil, error: nil, state: nil,
                              path: nil, mimeType: nil)
            return AIMessage(id: "switch-\(title)-message-\(index)", role: "assistant",
                             created: Int64(index), completed: Int64(index + 1),
                             active: false, parts: [part])
        }
    }

    func selectUITestConversationIfNeeded(_ id: String) -> Bool {
        guard uiTestSupport.sessionSwitchEnabled else { return false }
        applyUITestMutation(.selectedHistory(id, sessionSwitchUITestMessages(String(id.suffix(1)))))
        return true
    }
}
#endif

#endif
