// 测试宿主与素材仅供 Debug 使用，正式页面不在本文件实现。
#if DEBUG
import MaiChatCore
import SwiftUI
import UserNotifications
import UIKit
import PDFKit

#if DEBUG && targetEnvironment(simulator)
/// 只持有请求 UUID 和宿主的安全状态；测试密码仍由 XCTest 输入正常 SecureField，
/// 探针不知道密码，也不向工具 JSON、诊断日志或模型请求写入密码。
private struct PasswordToolUITestProbe: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @State private var requestID: String?
    @State private var output = "idle"
    @State private var busy = false

    var body: some View {
        VStack(spacing: 6) {
            HStack {
                Button("打开密码工具") { Task { await run(action: "open") } }
                    .accessibilityIdentifier("password-tool-open")
                Button("查询状态") { Task { await run(action: "status") } }
                    .disabled(requestID == nil)
                    .accessibilityIdentifier("password-tool-status")
            }
            .disabled(busy || !appState.hasUnifiedAccount)
            Text("密码工具状态")
                .font(.caption2)
                .accessibilityIdentifier("password-tool-output")
                .accessibilityValue(output)
        }
        .padding(8)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 10))
        .padding(.trailing, 8)
    }

    private func run(action: String) async {
        guard !busy else { return }
        busy = true
        defer { busy = false }
        var arguments: [String: String] = ["action": action]
        if action == "status", let requestID { arguments["request_id"] = requestID }
        guard let data = try? JSONSerialization.data(withJSONObject: arguments),
              let encoded = String(data: data, encoding: .utf8) else { return }
        let result = await AIMobileHostToolProvider.shared.execute(
            name: "maichat_change_password", argumentsJSON: encoded)
        guard let raw = result.output, let bytes = raw.data(using: .utf8),
              let value = try? JSONSerialization.jsonObject(with: bytes) as? [String: Any],
              Set(value.keys).isSubset(of: ["request_id", "opened", "status", "changed", "error_code", "message"]),
              let id = value["request_id"] as? String, UUID(uuidString: id) != nil else {
            // 未知字段不能进入探针状态；固定失败标记也让 UI 契约测试直接失败而非吞掉泄漏。
            output = "invalid-password-tool-response"
            return
        }
        requestID = id
        output = raw
    }
}

/// 实际 UIKit 排版→同一个安全输入页→普通 C ABI 加密→PDFKit 独立打开验证。
/// 密码只在异步函数局部变量中使用；探针状态仅保存布尔结果和测试输出路径。
private struct PdfSecurityUITestProbe: View {
    @EnvironmentObject private var appState: RemoteIMAppState
    @ObservedObject private var model = AIAssistantModel.shared
    @State private var busy = false
    @State private var result = "idle"

    var body: some View {
        VStack {
            Button("测试 PDF 保护") { Task { await run() } }
                .disabled(busy || !appState.hasUnifiedAccount || model.workspacePath.isEmpty)
                .accessibilityIdentifier("pdf-security-run")
            Text("PDF 保护结果").font(.caption2)
                .accessibilityIdentifier("pdf-security-output")
                .accessibilityValue(result)
        }
        .padding(8)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 10))
        .padding(.trailing, 8)
    }

    private func run() async {
        guard !busy else { return }
        busy = true
        result = "running"
        defer { busy = false }
        let directory = URL(fileURLWithPath: model.workspacePath, isDirectory: true)
            .appendingPathComponent("pdf-protection-ui-\(UUID().uuidString)", isDirectory: true)
        let source = directory.appendingPathComponent("source.pdf")
        let output = directory.appendingPathComponent("protected.pdf")
        let alias = directory.deletingLastPathComponent()
            .appendingPathComponent("pdf-output-alias-\(UUID().uuidString)", isDirectory: true)
        defer {
            try? FileManager.default.removeItem(at: source)
            try? FileManager.default.removeItem(at: alias)
        }
        func publish(_ fields: [String: Any]) {
            guard let data = try? JSONSerialization.data(withJSONObject: fields),
                  let text = String(data: data, encoding: .utf8) else { return }
            result = text
        }
        do {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            // 模拟真机 /var 等父目录别名；核心回传规范路径后，普通宿主不能把成功误报成失败。
            try FileManager.default.createSymbolicLink(at: alias, withDestinationURL: directory)
            let metadata = try MaiPdfRenderer.render(html:
                "<html><body><h1>MaiChat PDF</h1><p>private fixture content</p></body></html>", output: source)
            guard let sourceDocument = PDFDocument(url: source), sourceDocument.pageCount > 0 else {
                publish(["status": "failed", "stage": "native-render"]); return
            }
            let sourcePages = sourceDocument.pageCount
            let password = try await AIPdfPasswordRequests.shared.requestPassword(appState: appState)
            let delivered = try await MaiPdfSecurity.encrypt(source: source,
                output: alias.appendingPathComponent("protected.pdf"), password: password)
            guard delivered.standardizedFileURL == output.resolvingSymlinksInPath().standardizedFileURL else {
                publish(["status": "failed", "stage": "canonical-output"]); return
            }
            guard let document = PDFDocument(url: output), document.isLocked else {
                publish(["status": "failed", "stage": "not-locked"]); return
            }
            let wrongPassword = password == "deliberately-wrong" ? "another-wrong" : "deliberately-wrong"
            guard !document.unlock(withPassword: wrongPassword), document.isLocked,
                  document.unlock(withPassword: password),
                  document.string?.contains("private fixture content") == true else {
                publish(["status": "failed", "stage": "independent-reader"]); return
            }
            publish(["status": "passed", "locked_without_password": true,
                     "wrong_password_rejected": true, "content_readable": true,
                     "path": output.path, "source_pages": sourcePages,
                     "encrypted_pages": document.pageCount, "renderer_pages": metadata["pages"] ?? 0])
        } catch is CancellationError {
            publish(["status": "canceled", "output_exists": FileManager.default.fileExists(atPath: output.path)])
        } catch {
            // 不保存底层异常或密码；任何阶段失败都只输出固定无凭据分类。
            publish(["status": "failed", "stage": "pdf-operation"])
        }
    }
}
#endif

#if targetEnvironment(simulator)
/// Exercise the production RootView and its real tab bar with content that
/// reaches both system edges, rather than a short list or a placeholder bar.
private struct AppBarsLayoutUITestRoot: View {
    @StateObject private var appState = RemoteIMAppState(loadedSettings: .empty)
    @State private var prepared = false
    @State private var toolOrderStage = 0

    var body: some View {
        RootView()
            .environmentObject(appState)
            .safeAreaInset(edge: .top, spacing: 0) {
                if ProcessInfo.processInfo.arguments.contains("--tool-order-ui-test") {
                    Button("追加下一段工具") {
                        toolOrderStage += 1
                        AIAssistantModel.shared.installToolOrderUITestFixture(stage: toolOrderStage)
                    }
                    .accessibilityIdentifier("tool-order-advance")
                    .disabled(toolOrderStage >= 2)
                }
            }
            .onAppear {
                guard !prepared else { return }
                prepared = true
                let owner = "app-bars-ui-test"
                var contacts = (0..<35).map { index in
                    RemoteIMContact(userID: "bars-peer-\(index)",
                                    displayName: "边缘测试联系人 \(index)")
                }
                if ProcessInfo.processInfo.arguments.contains("--contacts-profile-ui-test") {
                    contacts[1] = RemoteIMContact(userID: "bars-peer-1",
                        displayName: "测试-Nickname-中英混合的很长昵称-LongContactName-完整资料")
                }
                // 同文样本走真实 IM 列表，便于跨页面核对字体而不读取线上会话。
                if ProcessInfo.processInfo.arguments.contains("--im-list-font-ui-test") {
                    contacts = [RemoteIMContact(userID: "bars-peer-34", displayName: "偏好设置"),
                        RemoteIMContact(userID: "bars-peer-33",
                            displayName: "测试-Nickname-中英混合的很长昵称-LongContactName-完整资料")]
                }
                var messages = contacts.enumerated().map { index, contact in
                    RemoteIMMessage(fromUserID: contact.userID, toUserID: owner,
                        text: "列表滚动样本 \(index)", direction: .incoming, status: .sent,
                        createdAt: Date(timeIntervalSince1970: Double(index)))
                }
                messages += (0..<80).map { index in
                    RemoteIMMessage(fromUserID: "bars-peer-34", toUserID: owner,
                        text: "边缘聊天消息 \(index)\n第二行内容", direction: .incoming,
                        status: .sent, createdAt: Date(timeIntervalSince1970: Double(index + 100)))
                }
                // 同一条表格走正常 IM 与创作中心，避免只验孤立的文本控件。
                var markdown: String? = ProcessInfo.processInfo.arguments.contains("--markdown-breaks-ui-test")
                    ? "| Case | Result |\n| --- | --- |\n| **Expected**:<br>320x180<br><br>details | Actual:<br/>320x240<br />next<BR>last |\n\n`literal <br>`"
                    : nil
                if ProcessInfo.processInfo.arguments.contains("--typography-ui-test"), markdown != nil {
                    markdown! += "\n\n# 标题 Heading\n\n你好 Hello，正文与 **加粗 Bold**、*斜体 Italic*。\n\n- 列表正文 List body\n- 保留 `inline code`\n\n> [!NOTE]\n> 引用 Quote\n\n```swift\nlet greeting = \"Hello 你好\"\n```"
                }
                if ProcessInfo.processInfo.arguments.contains("--menu-font-ui-test") ||
                   ProcessInfo.processInfo.arguments.contains("--im-list-font-ui-test") {
                    markdown = "偏好设置"
                }
                if let markdown {
                    messages.append(RemoteIMMessage(fromUserID: "bars-peer-34", toUserID: owner,
                        text: markdown, direction: .incoming, status: .sent,
                        createdAt: Date(timeIntervalSince1970: 1000)))
                }
                appState.masterUserID = owner
                appState.chatState = MasterChatState(ownerUserID: owner,
                                                     contacts: contacts, messages: messages)
                appState.hasCompletedInitialLogin = true
                appState.configureTypographyUITestFriends()
                AIAssistantModel.shared.installHistoryUITestFixture(markdown: markdown)
                if ProcessInfo.processInfo.arguments.contains("--tool-order-ui-test") {
                    AIAssistantModel.shared.installToolOrderUITestFixture(stage: 0)
                }
                if ProcessInfo.processInfo.arguments.contains("--drawer-font-ui-test") {
                    AIAssistantModel.shared.installDrawerFontUITestFixture()
                }
                AIAssistantModel.shared.historyLoaded = true
            }
    }
}

private struct AIAssistantUITestRoot: View {
    @StateObject private var appState = RemoteIMAppState(loadedSettings: .empty)

    var body: some View {
        AIAssistantView().environmentObject(appState)
    }
}

/// Local TLS integration route for the production iOS downloader and C bridge.
/// It is reachable only from a simulator launch carrying an explicit test flag.
private struct IMCurlDownloadUITestRoot: View {
    @State private var result = "running"

    var body: some View {
        Text(result)
            .accessibilityIdentifier(result == "passed" ? "im-media-all-ok" : "im-media-result")
            .task { await run() }
    }

    private func run() async {
        guard let encoded = ProcessInfo.processInfo.environment["MAICHAT_MEDIA_TEST_CA_BASE64"],
              let certificate = Data(base64Encoded: encoded) else {
            result = "missing-local-certificate"
            return
        }
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("im-media-\(UUID().uuidString)", isDirectory: true)
        do { try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true) }
        catch { result = "create-directory-failed"; return }
        defer { try? FileManager.default.removeItem(at: directory) }
        let downloader = MaiChatAttachmentDownloader()
        let image = directory.appendingPathComponent("image.png")
        let imageResult = await transfer(
            downloader, "https://127.0.0.1:18442/image", image, 1024 * 1024, certificate
        )
        guard imageResult.succeeded,
              let imageData = try? Data(contentsOf: image),
              imageData.starts(with: [137, 80, 78, 71]) else {
            result = "image-failed-\(imageResult.status)-\(imageResult.curlCode)"
            return
        }
        let file = directory.appendingPathComponent("file.bin")
        let fileResult = await transfer(
            downloader, "https://127.0.0.1:18442/file", file, 1024 * 1024, certificate
        )
        guard fileResult.succeeded,
              let fileData = try? Data(contentsOf: file),
              String(data: fileData, encoding: .utf8) == "MaiChat local TLS attachment fixture\n" else {
            result = "file-failed-\(fileResult.status)-\(fileResult.curlCode)"
            return
        }
        let held = directory.appendingPathComponent("held.bin")
        let oldAccountGeneration = downloader.generation
        let canceled = await withCheckedContinuation {
            (continuation: CheckedContinuation<MaiChatAttachmentDownloadResult, Never>) in
            downloader.download(
                signedURL: "https://127.0.0.1:18442/held", targetURL: held,
                maximumBytes: 1024 * 1024, certificatePEM: certificate
            ) { response in continuation.resume(returning: response) }
            Task {
                try? await Task.sleep(for: .milliseconds(200))
                downloader.cancelAll()
            }
        }
        guard canceled.status == Int32(MAI_DOWNLOAD_CANCELED.rawValue),
              !FileManager.default.fileExists(atPath: held.path) else {
            result = "cancel-failed-\(canceled.status)-\(canceled.curlCode)"
            return
        }
        let staleTarget = directory.appendingPathComponent("stale.bin")
        let staleResult = await withCheckedContinuation {
            (continuation: CheckedContinuation<MaiChatAttachmentDownloadResult, Never>) in
            downloader.download(
                signedURL: "https://127.0.0.1:18442/file", targetURL: staleTarget,
                maximumBytes: 1024 * 1024, generation: oldAccountGeneration,
                certificatePEM: certificate
            ) { response in continuation.resume(returning: response) }
        }
        guard staleResult.status == Int32(MAI_DOWNLOAD_CANCELED.rawValue),
              !FileManager.default.fileExists(atPath: staleTarget.path) else {
            result = "stale-account-failed-\(staleResult.status)"
            return
        }
        result = "passed"
    }

    private func transfer(
        _ downloader: MaiChatAttachmentDownloader,
        _ signedURL: String, _ target: URL, _ maximumBytes: UInt64, _ certificate: Data
    ) async -> MaiChatAttachmentDownloadResult {
        await withCheckedContinuation { continuation in
            downloader.download(
                signedURL: signedURL, targetURL: target,
                maximumBytes: maximumBytes, certificatePEM: certificate
            ) { response in continuation.resume(returning: response) }
        }
    }
}
#endif

#if DEBUG && targetEnvironment(simulator)
private struct AICalendarUITestRoot: View {
    @State private var result = "尚未创建日程"

    var body: some View {
        VStack(spacing: 20) {
            Button("创建系统日历测试事件") {
                Task {
                    let start = Int64(Date().timeIntervalSince1970 * 1_000) + 3_600_000
                    let response = await AIMobileHostToolProvider.shared.createCalendarEvent([
                        "title": "MaiChat 日历隔离测试",
                        "notes": "仅在隔离模拟器中创建",
                        "session_id": "ses_calendar_fixture",
                        "starts_at_utc_ms": start,
                        "ends_at_utc_ms": start + 1_800_000
                    ])
                    result = response.errorMessage ?? response.output ?? "日历没有返回结果"
                }
            }
            .accessibilityIdentifier("ai-calendar-create-sample")
            Text(result).accessibilityIdentifier("ai-calendar-result")
        }
    }
}
#endif

/// 仅 Debug 宿主识别测试旗标；Release Scene 始终进入普通 App。
@MainActor
enum MaiChatUITestSupport {
    static var bypassesBootstrap: Bool {
        #if targetEnvironment(simulator)
        if ProcessInfo.processInfo.arguments.contains("--ai-ui-test") { return true }
        #endif
        return ProcessInfo.processInfo.arguments.contains("--ffplay-agent-dock-ui-test") ||
            ProcessInfo.processInfo.arguments.contains("--ffplay-agent-real-video-test")
    }

    static func entryView() -> AnyView? {
        #if targetEnvironment(simulator)
        guard ProcessInfo.processInfo.arguments.contains("--ai-ui-test") else { return nil }
        return AnyView(MaiChatSimulatorUITestEntry())
        #else
        if bypassesBootstrap { return AnyView(MaiFfplayAgentDockUITestRoot()) }
        return nil
        #endif
    }

    #if targetEnvironment(simulator)
    static func configureAfterBootstrap(_ appState: RemoteIMAppState?) async {
        if ProcessInfo.processInfo.arguments.contains(
            "--cloud-drive-production-canary-normal"),
           let appState = appState {
            do {
                let account = try MaiCloudDriveProductionCanaryCredential.consume()
                try await appState.loginWithAccountPassword(
                    account: account.account,
                    password: account.password,
                    ageConfirmed: true)
            } catch {
                AppDiagnosticLog.shared.record(
                    level: .error, category: "cloud-drive-canary",
                    event: "account-login-failed")
            }
            return
        }
        // 仅本机正常 App 的交互验收使用；测试账号由本地模拟短信服务创建，
        // 不进入真机和发布包，也不触发任何付费模型请求。
        let environment = ProcessInfo.processInfo.environment
        if let phone = environment["MAICHAT_LOCAL_UI_PHONE"],
           let password = environment["MAICHAT_LOCAL_UI_PASSWORD"],
           !password.isEmpty,
           let appState = appState {
            do {
                try await appState.loginWithAccountPassword(
                    account: phone, password: password, ageConfirmed: true)
            } catch {
                AppDiagnosticLog.shared.record(
                    level: .error, category: "local-ui",
                    event: "local-account-login-failed",
                    fields: ["error": error.localizedDescription])
            }
        }
    }
    #endif

    @ViewBuilder static func overlayProbes(appState: RemoteIMAppState) -> some View {
        #if targetEnvironment(simulator)
        // 仅显式模拟器验收旗标添加控制探针；仍走普通 bootstrap、账号登录和真实 Root，
        // 不创建替代页面、不注入密码，也不进入真机或 Release 路径。
        if ProcessInfo.processInfo.arguments.contains("--password-tool-ui-test") {
            PasswordToolUITestProbe().environmentObject(appState)
        }
        if ProcessInfo.processInfo.arguments.contains("--pdf-security-ui-test") {
            PdfSecurityUITestProbe().environmentObject(appState)
        }
        #endif
    }
}

#if targetEnvironment(simulator)
private struct MaiChatSimulatorUITestEntry: View {
    var body: some View {
        if ProcessInfo.processInfo.arguments.contains("--ai-ui-test") {
            if ProcessInfo.processInfo.arguments.contains("--ai-account-tools-ui-test") {
                AccountToolsUITestRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--ai-local-inference-ui-test") {
                AILocalInferenceUITestRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--app-bars-layout-ui-test") ||
                        ProcessInfo.processInfo.arguments.contains("--my-settings-ui-test") ||
                        ProcessInfo.processInfo.arguments.contains("--markdown-breaks-ui-test") {
                AppBarsLayoutUITestRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--ai-screen-frames-ui-test") {
                #if DEBUG
                AIScreenFramesUITestRoot()
                #else
                EmptyView()
                #endif
            } else if ProcessInfo.processInfo.arguments.contains("--current-app-screen-capture-test") {
                AICurrentAppScreenshotUITestRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--im-curl-download-ui-test") {
                IMCurlDownloadUITestRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--im-group-ui-test") ||
                        ProcessInfo.processInfo.arguments.contains(
                            "--im-group-activation-fail-ui-test") {
                GroupChatUITestRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--video-circle-ui-test") {
                AIVideoCircleUITestRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--cloud-drive-mixed-media-test") {
                MaiCloudMixedMediaUITestRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--cloud-drive-image-pager-test") {
                MaiCloudImagePagerUITestRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--cloud-drive-ui-test") {
                MaiCloudDriveUITestRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--ai-calendar-ui-test") {
                AICalendarUITestRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--im-add-friend-menu-ui-test") {
                ChatAddFriendMenuUITestRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--permission-card-ui-test") {
                AIPermissionUITestRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--ai-image-fullscreen-ui-test") {
                AIImageFullscreenUITestRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--ai-image-mixed-ui-test") {
                AIImageMixedUITestRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--im-image-fullscreen-ui-test") {
                RemoteIMImageFullscreenUITestRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--text-media-ui-test") {
                AITextMediaUITestRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--video-bubble-ui-test") {
                AIVideoBubbleUITestRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--transcript-ui-test") {
                AITranscriptUITestRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--ai-history-ui-test") {
                AIHistoryUITestRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--ai-welcome-ui-test") {
                AIWelcomeUITestRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--ai-steering-ui-test") {
                AISteeringUITestRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--ai-session-switch-ui-test") {
                AISessionSwitchUITestRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--ai-session-reveal-ui-test") {
                AISessionRevealUITestRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--ai-portrait-ui-test") {
                AIPortraitUITestRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--ai-browser-ui-test") {
                #if DEBUG
                AIBrowserUITestRoot()
                #else
                AIAssistantView()
                #endif
            } else if ProcessInfo.processInfo.arguments.contains("--ai-media-draft-ui-test") {
                AIMediaDraftUITestRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--ai-local-music-ui-test") {
                AILocalMusicUITestRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--ai-stream-typing-ui-test") {
                AIStreamTypingUITestRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--voice-conversation-preview") {
                VoiceTranscriptionConversationPreviewRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--ffplay-ui-test") {
                MaiFfplayUITestRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--ffplay-swipe-ui-test") {
                MaiFfplaySwipeUITestRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--mixed-gallery-ui-test") {
                MaiMixedGalleryUITestRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--ffplay-seek-ui-test") {
                MaiFfplaySeekUITestRoot(startSeconds: 0)
            } else if ProcessInfo.processInfo.arguments.contains("--ffplay-start-ui-test") {
                MaiFfplaySeekUITestRoot(startSeconds: 6)
            } else if ProcessInfo.processInfo.arguments.contains("--ffplay-agent-dock-ui-test") {
                #if DEBUG
                MaiFfplayAgentDockUITestRoot()
                #else
                AIAssistantView()
                #endif
            } else if ProcessInfo.processInfo.arguments.contains("--voice-docked-ui-test") {
                VoiceTranscriptionDockUITestRoot()
            } else if ProcessInfo.processInfo.arguments.contains("--im-landscape-ui-test") ||
                        ProcessInfo.processInfo.arguments.contains("--im-large-history-ui-test") ||
                        ProcessInfo.processInfo.arguments.contains("--im-search-focus-ui-test") {
                ChatLandscapeUITestRoot()
            } else {
                AIAssistantUITestRoot()
            }
        } else {
            EmptyView()
        }

    }
}
#endif
#endif
