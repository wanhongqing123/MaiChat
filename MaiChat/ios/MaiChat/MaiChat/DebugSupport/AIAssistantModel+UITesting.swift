// 测试宿主与素材仅供 Debug 使用，正式页面不在本文件实现。
#if DEBUG
import Foundation
import UIKit

struct AIQuestionAnswerUITestRequest {
    let sessionID: String
    let questionID: String
    let requestID: String
    let text: String
}

@MainActor
final class AIAssistantModelUITestSupport {
    var sessionSwitchEnabled = false
    var screenFrameAccountID: String?
    // Isolated composer fixtures confirm routing without contacting a model.
    var steeringHandler: ((AISteeringRequest) async throws -> String)?
    var questionHandler: ((AIQuestionAnswerUITestRequest) async throws -> String)?
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
    /// Load the unmodified C ABI snapshot exported after a real native Agent
    /// turn. Only the isolated workspace changes; message IDs, text, and tool
    /// receipts come from the snapshot and survive the normal Codable round-trip.
    @discardableResult
    func installNativePhotoReplySnapshotForUITesting(_ data: Data, sessionID: String,
                                                     workspacePath: String) -> Bool {
        guard AINativePhotoReplyUITestSupport.isRequested,
              let response = try? JSONDecoder().decode(AIResponse.self, from: data), response.ok,
              let sourceSessions = response.sessions, sourceSessions.contains(where: { $0.id == sessionID }),
              let sourceMessages = response.messages, !sourceMessages.isEmpty,
              let stored = try? JSONEncoder().encode(sourceMessages),
              let restored = try? JSONDecoder().decode([AIMessage].self, from: stored),
              restored == sourceMessages else { return false }
        applyUITestMutation(.workspace(workspacePath))
        applyUITestMutation(.clearRemoteSelection)
        sessions = sourceSessions
        applyUITestMutation(.selectedHistory(sessionID, restored.sorted { $0.id < $1.id }))
        configured = true
        ready = true
        return true
    }

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

    /// Exercise the real message projection, composer and durable answer queue.
    /// Only the final native receipt is replaced; no model or user history is used.
    func installQuestionConversationUITestFixture(
        handler: @escaping (AIQuestionAnswerUITestRequest) async throws -> String
    ) {
        installSteeringUITestFixture { _ in
            throw AIBackendError(message: "Question answers must not become steering messages")
        }
        uiTestSupport.questionHandler = handler
        let question = AIQuestion(id: "question-fixture", question: "复现测试问题：选择哪份素材？",
            options: ["第一份本地素材", "第二份本地素材"], sessionId: "switch-A",
            messageId: "question-owner", partId: "question-part", asked: 1000)
        let input = try! JSONSerialization.data(withJSONObject: [
            "question": question.question, "options": question.options])
        questions = [question]
        messages = [AIMessage(id: "question-owner", role: "assistant", created: 900,
            completed: 0, active: true, parts: [
                AIPart(id: "question-intro", kind: "text", text: "请在下方聊天输入框直接回答。"),
                AIPart(id: "question-part", kind: "tool", tool: "question",
                    input: String(decoding: input, as: UTF8.self), state: "running")])]
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

    // 固定片段身份逐步追加正文和工具，走正常 Root/消息行而不执行模型或工具。
    func installToolOrderUITestFixture(stage: Int) {
        let session = "tool-order-ui-test"
        func text(_ id: String, _ value: String) -> AIPart {
            AIPart(id: id, kind: "text", text: value)
        }
        func tool(_ id: String, _ name: String, _ result: String, running: Bool = false) -> AIPart {
            AIPart(id: id, kind: "tool", tool: name, input: "{}", output: result,
                   state: running ? "running" : "completed")
        }
        var parts = [text("intro", "先检查文件，再执行处理。"),
                     tool("tool-a1", "file_read", "A1 执行结果"),
                     tool("tool-a2", "file_stat", "A2 执行结果"),
                     text("answer-a", "A 段说明：文件检查已经完成。")]
        if stage > 0 {
            parts.append(tool("tool-b1", "shell", stage == 1 ? "" : "B1 执行结果", running: stage == 1))
        }
        if stage > 1 {
            parts += [text("answer-b", "B 段说明：处理结果已经核对。"),
                      tool("tool-c1", "file_write", "C1 执行结果"),
                      tool("tool-c2", "shell", "C2 执行结果"),
                      text("answer-c", "C 段说明：全部完成。")]
        }
        sessions = [AISession(id: session, title: "工具与正文顺序", busy: stage < 2)]
        selected = session
        messages = [AIMessage(id: "tool-order-message", role: "assistant", created: 1000,
                              completed: stage < 2 ? 0 : 5000, active: stage < 2, parts: parts)]
        configured = true
        ready = true
        historyLoaded = true
    }

    func installDrawerFontUITestFixture() {
        installHistoryUITestFixture(markdown: "偏好设置")
        sessions = [AISession(id: "history-ui-test", title: "偏好设置", busy: false)]
        applyUITestMutation(.remoteSessions([AIRemoteSession(
            sessionID: "drawer-remote", title: "偏好设置", updatedAt: 1793880000000, busy: false,
            deviceSlot: "mac", deviceName: "字体测试 Mac", deviceID: "drawer-font-mac",
            loginSessionID: "drawer-font-login", online: false)]))
    }

    func installHistoryUITestFixture(markdown: String? = nil) {
        let session = "history-ui-test"
        sessions = [AISession(id: session, title: "历史会话", busy: false)]
        selected = session
        messages = (0..<80).map { index in
            let part = AIPart(id: "history-part-\(index)", kind: "text",
                              text: index == 79 ? (markdown ?? "历史消息 \(index)\n第二行内容")
                                                : "历史消息 \(index)\n第二行内容", tool: nil,
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
